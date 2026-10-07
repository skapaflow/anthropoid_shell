#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "parser.h"

/*************************************************************
 * Command line in the sh/bash model (a subset):             *
 *                                                           *
 *   list     = pipeline { (; | && | || | &) pipeline }      *
 *   pipeline = command { | command }                        *
 *   command  = word { word | redirection }                  *
 *   redir.   = < p | > p | >> p | 2> p | 2>> p | 2>&1       *
 *                                                           *
 * Words: quotes "..." and '...' keep spaces and             *
 * operators; parts written together become one word         *
 * (a"b c"d). The backslash is a path separator, not an      *
 * escape.                                                   *
 *                                                           *
 * Aliases (ant_parse_set_alias): the first word of a        *
 * command, without quotes, is replaced by the alias text    *
 * and the line is read again from there, as in sh.          *
 *************************************************************/

#define ALIAS_NAME  64
#define ALIAS_DEPTH 16  /* alias inside alias in the same command */
#define ALIAS_TOTAL 100 /* expansions per line */

typedef struct {
	const wchar_t *s;
	int pos;
	ANT_LIST *list;
	ANT_PIPELINE pipe; /* pipeline being built */
	ANT_CMD cmd;       /* command being built */
	int pending;       /* redirection waiting for its file, or -1 */
	const wchar_t *pending_text;
	wchar_t *buffer;   /* line with the aliases already replaced (p->s points to it) */
	wchar_t used[ALIAS_DEPTH][ALIAS_NAME]; /* aliases already replaced in this command */
	int nused, nexpanded;
} PARSER;

static const wchar_t *(*alias_lookup) (const wchar_t *);

/* function that returns an alias text (or NULL); NULL turns aliases off */
void ant_parse_set_alias (const wchar_t *(*lookup) (const wchar_t *)) {

	alias_lookup = lookup;
}

static bool is_space (wchar_t c) {

	return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

static bool is_op (wchar_t c) {

	return c == L'|' || c == L'&' || c == L';' || c == L'<' || c == L'>';
}

static bool fail (PARSER *p, const wchar_t *fmt, const wchar_t *what) {

	swprintf(p->list->error, 128, fmt, what);
	return false;
}

static void *grow (void *array, int count, size_t size) {

	/* grows 4 at a time */
	if (count % 4 == 0)
		return realloc(array, size * (count + 4));
	return array;
}

static void cmd_free (ANT_CMD *c) {

	for (int i = 0; i < c->argc; i++) {
		free(c->argv[i]);
		free(c->lit[i]);
	}
	for (int i = 0; i < c->nredir; i++) {
		free(c->redir[i].path);
		free(c->redir[i].lit);
	}
	free(c->argv);
	free(c->lit);
	free(c->redir);
	memset(c, 0, sizeof *c);
}

static void pipe_free (ANT_PIPELINE *pl) {

	for (int i = 0; i < pl->ncmd; i++)
		cmd_free(&pl->cmd[i]);
	free(pl->cmd);
	memset(pl, 0, sizeof *pl);
}

void ant_parse_free (ANT_LIST *list) {

	for (int i = 0; i < list->n; i++)
		pipe_free(&list->p[i]);
	free(list->p);
	list->p = NULL;
	list->n = 0;
}

static bool add_word (ANT_CMD *c, wchar_t *word, char *lit) {

	c->argv = grow(c->argv, c->argc, sizeof(wchar_t *));
	if (!c->argv)
		return false;
	c->lit = grow(c->lit, c->argc, sizeof(char *));
	if (!c->lit)
		return false;
	c->lit[c->argc] = lit;
	c->argv[c->argc++] = word;
	return true;
}

static bool add_redir (ANT_CMD *c, int kind, wchar_t *path, char *lit) {

	c->redir = grow(c->redir, c->nredir, sizeof(ANT_REDIR));
	if (!c->redir)
		return false;
	c->redir[c->nredir].kind = kind;
	c->redir[c->nredir].path = path;
	c->redir[c->nredir].lit = lit;
	c->nredir++;
	return true;
}

/* closes the command being built and puts it in the pipeline */
static bool end_cmd (PARSER *p, const wchar_t *op) {

	static const wchar_t *redir_text[] = { L"<", L">", L">>", L"2>", L"2>>", L"2>&1" };

	if (p->pending >= 0)
		return fail(p, L"missing file after '%ls'", p->pending_text);
	if (p->cmd.argc == 0 && p->cmd.nredir > 0)
		return fail(p, L"missing command before '%ls'", redir_text[p->cmd.redir[0].kind]);
	if (p->cmd.argc == 0)
		return fail(p, L"syntax error near '%ls'", op);
	p->pipe.cmd = grow(p->pipe.cmd, p->pipe.ncmd, sizeof(ANT_CMD));
	if (!p->pipe.cmd)
		return fail(p, L"out of memory%ls", L"");
	p->pipe.cmd[p->pipe.ncmd++] = p->cmd;
	memset(&p->cmd, 0, sizeof p->cmd);
	p->nused = 0;
	return true;
}

/* closes the pipeline being built with operator 'op' and puts it in the list */
static bool end_pipe (PARSER *p, int op) {

	p->pipe.op = op;
	p->list->p = grow(p->list->p, p->list->n, sizeof(ANT_PIPELINE));
	if (!p->list->p)
		return fail(p, L"out of memory%ls", L"");
	p->list->p[p->list->n++] = p->pipe;
	memset(&p->pipe, 0, sizeof p->pipe);
	return true;
}

/* reads a word (with its quotes resolved) starting at p->pos; *lit gets how each character was quoted */
static wchar_t *read_word (PARSER *p, char **lit) {

	const wchar_t *s = p->s;
	int n = 0, cap = 16;
	wchar_t *w = malloc(sizeof(wchar_t) * cap);
	char *l = malloc(cap);

	*lit = NULL;
	if (!w || !l) {
		free(w);
		free(l);
		return NULL;
	}

	while (s[p->pos] && !is_space(s[p->pos])) {
		wchar_t c = s[p->pos];
		int mode = QUOTE_NONE, from = p->pos, to;

		if (is_op(c))
			break;

		if (c == L'"' || c == L'\'') {
			int close = p->pos + 1;
			while (s[close] && s[close] != c)
				close++;
			if (!s[close]) {
				free(w);
				free(l);
				fail(p, L"unclosed quote %ls", c == L'"' ? L"\"" : L"'");
				return NULL;
			}
			mode = (c == L'"') ? QUOTE_DOUBLE : QUOTE_SINGLE;
			from = p->pos + 1;
			to = close;
			p->pos = close + 1;
		} else {
			to = p->pos + 1;
			p->pos++;
		}

		for (int i = from; i < to; i++) {
			if (n + 2 >= cap) {
				wchar_t *nw = realloc(w, sizeof(wchar_t) * (cap * 2));
				char *nl;
				if (!nw) {
					free(w);
					free(l);
					return NULL;
				}
				w = nw;
				nl = realloc(l, cap * 2);
				if (!nl) {
					free(w);
					free(l);
					return NULL;
				}
				l = nl;
				cap *= 2;
			}
			w[n] = s[i];
			l[n++] = (char) mode;
		}
	}

	w[n] = 0;
	*lit = l;
	return w;
}

/* a word, or the file of a pending redirection */
static bool take_word (PARSER *p) {

	char *lit;
	wchar_t *w = read_word(p, &lit);

	if (!w)
		return p->list->error[0] ? false : fail(p, L"out of memory%ls", L"");
	if (p->pending >= 0) {
		int kind = p->pending;
		p->pending = -1;
		if (!add_redir(&p->cmd, kind, w, lit)) {
			free(w);
			free(lit);
			return fail(p, L"out of memory%ls", L"");
		}
		return true;
	}
	if (!add_word(&p->cmd, w, lit)) {
		free(w);
		free(lit);
		return fail(p, L"out of memory%ls", L"");
	}
	return true;
}

static bool set_pending (PARSER *p, int kind, const wchar_t *text, int len) {

	if (p->pending >= 0)
		return fail(p, L"syntax error near '%ls'", text);
	p->pending = kind;
	p->pending_text = text;
	p->pos += len;
	return true;
}

/*
 * Is the first word of the command an alias? Replaces the word with its text
 * and returns true (reading goes on from the same position, now in the alias
 * text). The same alias is not replaced again in the same command: "ls = ls -l"
 * does not loop.
 */
static bool expand_alias (PARSER *p) {

	const wchar_t *s = p->s, *value;
	wchar_t name[ALIAS_NAME], *line;
	int end = p->pos, n;

	if (!alias_lookup || p->nexpanded == ALIAS_TOTAL || p->nused == ALIAS_DEPTH)
		return false;
	while (s[end] && !is_space(s[end]) && !is_op(s[end]) && s[end] != L'"' && s[end] != L'\'')
		end++;
	n = end - p->pos;
	/* a word with quotes is not an alias */
	if (n == 0 || n >= ALIAS_NAME || s[end] == L'"' || s[end] == L'\'')
		return false;
	wmemcpy(name, s + p->pos, n);
	name[n] = 0;
	for (int i = 0; i < p->nused; i++)
		if (!_wcsicmp(p->used[i], name))
			return false;
	if (!(value = alias_lookup(name)))
		return false;

	line = malloc(sizeof(wchar_t) * (p->pos + wcslen(value) + wcslen(s + end) + 1));
	if (!line)
		return false;
	wmemcpy(line, s, p->pos);
	wcscpy(line + p->pos, value);
	wcscat(line, s + end);
	free(p->buffer);
	p->buffer = line;
	p->s = line;
	wcscpy(p->used[p->nused++], name);
	p->nexpanded++;
	return true;
}

/*
 * Splits the line into pipelines, commands, words and redirections.
 * Returns false on a syntax error (message in list->error).
 */
bool ant_parse (const wchar_t *line, ANT_LIST *list) {

	PARSER p;
	bool ok = true;

	memset(list, 0, sizeof *list);
	memset(&p, 0, sizeof p);
	p.s = line;
	p.list = list;
	p.pending = -1;

	while (ok) {
		const wchar_t *s = p.s;

		while (is_space(s[p.pos]))
			p.pos++;
		if (!s[p.pos])
			break;

		wchar_t c = s[p.pos], d = s[p.pos + 1];

		if (c == L'|' && d == L'|') {
			ok = end_cmd(&p, L"||") && end_pipe(&p, OP_OR);
			p.pos += 2;
		} else if (c == L'|') {
			ok = end_cmd(&p, L"|");
			p.pos++;
		} else if (c == L'&' && d == L'&') {
			ok = end_cmd(&p, L"&&") && end_pipe(&p, OP_AND);
			p.pos += 2;
		} else if (c == L'&') {
			ok = end_cmd(&p, L"&") && end_pipe(&p, OP_BG);
			p.pos++;
		} else if (c == L';') {
			ok = end_cmd(&p, L";") && end_pipe(&p, OP_SEQ);
			p.pos++;
		} else if (c == L'<')
			ok = set_pending(&p, REDIR_IN, L"<", 1);
		else if (c == L'>' && d == L'>')
			ok = set_pending(&p, REDIR_APPEND, L">>", 2);
		else if (c == L'>')
			ok = set_pending(&p, REDIR_OUT, L">", 1);
		else if (c == L'2' && d == L'>' && s[p.pos + 2] == L'&' && s[p.pos + 3] == L'1' &&
			(!s[p.pos + 4] || is_space(s[p.pos + 4]) || is_op(s[p.pos + 4]))) {
			if (p.pending >= 0)
				ok = fail(&p, L"syntax error near '%ls'", L"2>&1");
			else if (!add_redir(&p.cmd, REDIR_ERR_TO_OUT, NULL, NULL))
				ok = fail(&p, L"out of memory%ls", L"");
			p.pos += 4;
		} else if (c == L'2' && d == L'>' && s[p.pos + 2] == L'>')
			ok = set_pending(&p, REDIR_ERR_APPEND, L"2>>", 3);
		else if (c == L'2' && d == L'>')
			ok = set_pending(&p, REDIR_ERR, L"2>", 2);
		else if (p.cmd.argc == 0 && p.pending < 0 && expand_alias(&p))
			; /* read again from the alias text */
		else
			ok = take_word(&p);
	}

	/* end of the line */
	if (ok) {
		if (p.pending >= 0)
			ok = fail(&p, L"missing file after '%ls'", p.pending_text);
		else if (p.cmd.argc > 0 || p.cmd.nredir > 0 || p.pipe.ncmd > 0)
			ok = end_cmd(&p, L"|") && end_pipe(&p, OP_END);
		else if (list->n > 0) {
			/* "a ;" and "a &" may end the line; "a &&" and "a ||" may not */
			int last = list->p[list->n - 1].op;
			if (last == OP_AND || last == OP_OR)
				ok = fail(&p, L"incomplete command after '%ls'", last == OP_AND ? L"&&" : L"||");
			else if (last == OP_SEQ)
				list->p[list->n - 1].op = OP_END;
		}
	}

	if (!ok) {
		cmd_free(&p.cmd);
		pipe_free(&p.pipe);
		ant_parse_free(list);
	}
	free(p.buffer);
	return ok;
}
