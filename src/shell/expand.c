#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "expand.h"
#include "shell.h"

/*************************************************************
 * Expansions done by the shell, in the sh model, on each    *
 * command right before it runs (so "false; echo $?" and     *
 * "export A=1 && echo $A" see the current state):           *
 *                                                           *
 *   $NAME ${NAME}  environment variable (empty if unset)    *
 *   $?             exit code of the last command            *
 *   ~ ~\dir ~/dir  home folder, only at the start of a word  *
 *   * ?            file names (a pattern that matches       *
 *                  nothing stays as typed; names starting   *
 *                  with . need a . in the pattern)          *
 *                                                           *
 * Quotes (see ANT_CMD.lit): '...' expands nothing, "..."     *
 * only $. The value of a variable is one word (it is not    *
 * split at spaces, folders have them) and is not globbed.   *
 * A bare $NAME that expands to nothing drops the word.      *
 *************************************************************/

#define GLOB_MAX      10000
#define GLOB_SEGMENTS 64

typedef struct {
	wchar_t *s;
	char *l; /* quoting of each character */
	int n, cap;
} BUF;

static bool buf_put (BUF *b, wchar_t c, char mode) {

	if (b->n + 2 > b->cap) {
		int cap = b->cap ? b->cap * 2 : 32;
		wchar_t *s = realloc(b->s, sizeof(wchar_t) * cap);
		char *l;
		if (!s)
			return false;
		b->s = s;
		l = realloc(b->l, cap);
		if (!l)
			return false;
		b->l = l;
		b->cap = cap;
	}
	b->s[b->n] = c;
	b->l[b->n++] = mode;
	b->s[b->n] = 0;
	return true;
}

static bool buf_put_text (BUF *b, const wchar_t *text) {

	/* what a variable or ~ expands to is literal: no more expansion, no glob */
	for (; *text; text++)
		if (!buf_put(b, *text, QUOTE_SINGLE))
			return false;
	return true;
}

static void buf_free (BUF *b) {

	free(b->s);
	free(b->l);
	memset(b, 0, sizeof *b);
}

/* ---------- home and variables ---------- */

static bool is_name_start (wchar_t c) {

	return c == L'_' || (c < 128 && iswalpha(c));
}

static bool is_name_char (wchar_t c) {

	return is_name_start(c) || (c >= L'0' && c <= L'9');
}

/* the value of a variable in a static buffer ("" if unset) */
static const wchar_t *variable (const wchar_t *name) {

	static wchar_t value[32768];
	DWORD n = GetEnvironmentVariableW(name, value, 32768);

	if (n == 0 || n >= 32768)
		value[0] = 0;
	return value;
}

/* HOME, or USERPROFILE when HOME is not defined (the Windows default) */
static const wchar_t *home (void) {

	static wchar_t h[MAX_PATH * 2];
	const wchar_t *v = variable(L"HOME");

	if (!*v)
		v = variable(L"USERPROFILE");
	wcsncpy(h, v, MAX_PATH * 2 - 1);
	h[MAX_PATH * 2 - 1] = 0;
	return h;
}

/* defines HOME from USERPROFILE when it does not exist, so $HOME and ~ agree */
void ant_expand_init (void) {

	wchar_t profile[MAX_PATH * 2];

	if (GetEnvironmentVariableW(L"HOME", NULL, 0) == 0 && GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH * 2))
		SetEnvironmentVariableW(L"HOME", profile);
}

/* $... starting at w[i] (w[i] == '$'): appends the value to b and returns where the word goes on */
static int expand_dollar (BUF *b, const wchar_t *w, int i, bool *failed) {

	wchar_t name[256];
	int j = i + 1, n = 0;

	if (w[j] == L'?') {
		wchar_t number[16];
		swprintf(number, 16, L"%d", ant_status);
		*failed |= !buf_put_text(b, number);
		return j + 1;
	}
	if (w[j] == L'{') {
		int close = j + 1;
		while (w[close] && w[close] != L'}' && n < 255)
			name[n++] = w[close++];
		name[n] = 0;
		if (w[close] == L'}' && n > 0 && is_name_start(name[0])) {
			*failed |= !buf_put_text(b, variable(name));
			return close + 1;
		}
		*failed |= !buf_put(b, L'$', QUOTE_SINGLE);
		return i + 1;
	}
	if (is_name_start(w[j])) {
		while (is_name_char(w[j]) && n < 255)
			name[n++] = w[j++];
		name[n] = 0;
		*failed |= !buf_put_text(b, variable(name));
		return j;
	}
	*failed |= !buf_put(b, L'$', QUOTE_SINGLE);
	return i + 1;
}

/* ~ and $ in a word; *dropped: a bare $NAME that expanded to nothing */
static bool expand_word (const wchar_t *w, const char *lit, BUF *b, bool *dropped) {

	int i = 0;
	bool failed = false, quoted = false, dollar = false;

	*dropped = false;
	for (int k = 0; w[k]; k++)
		if (lit && lit[k] != QUOTE_NONE)
			quoted = true;

	if (w[0] == L'~' && (!lit || lit[0] == QUOTE_NONE) && (!w[1] || w[1] == L'\\' || w[1] == L'/')) {
		const wchar_t *h = home();
		if (*h) {
			failed |= !buf_put_text(b, h);
			i = 1;
		}
	}

	while (w[i] && !failed) {
		char mode = lit ? lit[i] : QUOTE_NONE;
		if (w[i] == L'$' && mode != QUOTE_SINGLE) {
			dollar = true;
			i = expand_dollar(b, w, i, &failed);
		} else {
			failed |= !buf_put(b, w[i], mode);
			i++;
		}
	}
	/* an empty result still needs a string */
	if (!failed && !b->s) {
		b->s = calloc(1, sizeof(wchar_t));
		b->l = calloc(1, 1);
		failed = !b->s || !b->l;
	}
	*dropped = !failed && b->n == 0 && dollar && !quoted;
	return !failed;
}

/* ---------- globbing ---------- */

/* pattern p (quoting l) against a name; only * and ? without quotes are wild; case does not matter */
static bool match (const wchar_t *p, const char *l, const wchar_t *name) {

	for (; *p; p++, l++) {
		if (*p == L'*' && *l == QUOTE_NONE) {
			while (p[1] == L'*' && l[1] == QUOTE_NONE) {
				p++;
				l++;
			}
			for (const wchar_t *t = name; ; t++) {
				if (match(p + 1, l + 1, t))
					return true;
				if (!*t)
					return false;
			}
		}
		if (!*name)
			return false;
		if (!(*p == L'?' && *l == QUOTE_NONE) && towlower(*p) != towlower(*name))
			return false;
		name++;
	}
	return !*name;
}

static bool is_wild (const wchar_t *s, const char *l) {

	for (int i = 0; s[i]; i++)
		if ((s[i] == L'*' || s[i] == L'?') && l[i] == QUOTE_NONE)
			return true;
	return false;
}

typedef struct {
	wchar_t *seg[GLOB_SEGMENTS]; /* pieces of the pattern between separators */
	char *lit[GLOB_SEGMENTS];
	wchar_t sep[GLOB_SEGMENTS];  /* separator that follows each piece (0 for the last) */
	int nseg;
	wchar_t **found;
	int nfound;
	bool full;
} GLOB;

static void glob_add (GLOB *g, const wchar_t *path) {

	wchar_t **f;

	if (g->nfound >= GLOB_MAX) {
		g->full = true;
		return;
	}
	f = realloc(g->found, sizeof(wchar_t *) * (g->nfound + 1));
	if (!f)
		return;
	g->found = f;
	if ((f[g->nfound] = _wcsdup(path)))
		g->nfound++;
}

/* name order, case-insensitive, with the separators first so that sub\x sorts before sub2\x */
static int compare_names (const void *a, const void *b) {

	const wchar_t *x = *(wchar_t *const *) a, *y = *(wchar_t *const *) b;

	for (; *x && *y; x++, y++) {
		wchar_t cx = (*x == L'/') ? L'\\' : *x, cy = (*y == L'/') ? L'\\' : *y;
		if (towlower(cx) != towlower(cy)) {
			if (cx == L'\\')
				return -1;
			if (cy == L'\\')
				return 1;
			return towlower(cx) < towlower(cy) ? -1 : 1;
		}
	}
	return (*x ? 1 : 0) - (*y ? 1 : 0);
}

/* prefix: the path built so far (ends with a separator, or is empty) */
static void glob_walk (GLOB *g, wchar_t *prefix, int idx) {

	int len = wcslen(prefix);
	bool last = idx == g->nseg - 1;

	if (g->full)
		return;

	if (!is_wild(g->seg[idx], g->lit[idx])) {
		/* plain piece: goes into the path as it is */
		int n = wcslen(g->seg[idx]);
		if (len + n + 2 >= MAX_PATH * 2)
			return;
		wcscpy(prefix + len, g->seg[idx]);
		if (last) {
			if (GetFileAttributesW(prefix) != INVALID_FILE_ATTRIBUTES)
				glob_add(g, prefix);
		} else {
			prefix[len + n] = g->sep[idx];
			prefix[len + n + 1] = 0;
			glob_walk(g, prefix, idx + 1);
		}
		prefix[len] = 0;
		return;
	}

	{
		wchar_t pattern[MAX_PATH * 2];
		WIN32_FIND_DATAW fd;
		HANDLE h;
		bool dot = g->seg[idx][0] == L'.' && g->lit[idx][0] == QUOTE_NONE;

		if (len + 2 >= MAX_PATH * 2)
			return;
		swprintf(pattern, MAX_PATH * 2, L"%ls*", prefix);
		h = FindFirstFileW(pattern, &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do {
			int n = wcslen(fd.cFileName);
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
				continue;
			if (fd.cFileName[0] == L'.' && !dot)
				continue;
			if (!match(g->seg[idx], g->lit[idx], fd.cFileName))
				continue;
			if (len + n + 2 >= MAX_PATH * 2)
				continue;
			wcscpy(prefix + len, fd.cFileName);
			if (last)
				glob_add(g, prefix);
			else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				prefix[len + n] = g->sep[idx];
				prefix[len + n + 1] = 0;
				glob_walk(g, prefix, idx + 1);
			}
			prefix[len] = 0;
		} while (!g->full && FindNextFileW(h, &fd));
		FindClose(h);
	}
}

/* the matches of the pattern in b, sorted; g->nfound == 0 if none */
static void glob_expand (const BUF *b, GLOB *g) {

	wchar_t prefix[MAX_PATH * 2];
	int start = 0;

	memset(g, 0, sizeof *g);
	for (int i = 0; ; i++) {
		if (b->s[i] == L'\\' || b->s[i] == L'/' || !b->s[i]) {
			if (g->nseg == GLOB_SEGMENTS)
				return;
			g->seg[g->nseg] = _wcsdup(b->s + start);
			g->lit[g->nseg] = malloc(i - start + 1);
			if (!g->seg[g->nseg] || !g->lit[g->nseg])
				return;
			g->seg[g->nseg][i - start] = 0;
			memcpy(g->lit[g->nseg], b->l + start, i - start);
			g->lit[g->nseg][i - start] = 0;
			g->sep[g->nseg] = b->s[i];
			g->nseg++;
			start = i + 1;
			if (!b->s[i])
				break;
		}
	}
	prefix[0] = 0;
	glob_walk(g, prefix, 0);
	if (g->nfound > 1)
		qsort(g->found, g->nfound, sizeof(wchar_t *), compare_names);
}

static void glob_free (GLOB *g) {

	for (int i = 0; i < g->nseg; i++) {
		free(g->seg[i]);
		free(g->lit[i]);
	}
	/* 'found' belongs to the caller */
}

/* ---------- commands ---------- */

static bool add_arg (ANT_CMD *out, wchar_t *word) {

	wchar_t **argv = realloc(out->argv, sizeof(wchar_t *) * (out->argc + 2));

	if (!argv) {
		free(word);
		return false;
	}
	out->argv = argv;
	out->argv[out->argc++] = word;
	out->argv[out->argc] = NULL;
	return true;
}

void ant_expand_free (ANT_CMD *c) {

	for (int i = 0; i < c->argc; i++)
		free(c->argv[i]);
	for (int i = 0; i < c->nredir; i++)
		free(c->redir[i].path);
	free(c->argv);
	free(c->redir);
	memset(c, 0, sizeof *c);
}

/* the file of a redirection: $ and ~ only, no glob */
static wchar_t *expand_path (const wchar_t *path, const char *lit) {

	BUF b = { 0 };
	bool dropped;
	wchar_t *result;

	if (!expand_word(path, lit, &b, &dropped)) {
		buf_free(&b);
		return NULL;
	}
	result = b.s;
	free(b.l);
	return result;
}

/*
 * does a word of 'in' have a * or ? outside quotes? Only those can glob: the value of a
 * variable never does. ($? also says yes; it only costs a glob pass that finds nothing.)
 */
bool ant_expand_has_wild (const ANT_CMD *in) {

	for (int i = 0; i < in->argc; i++) {
		const wchar_t *w = in->argv[i];
		const char *l = in->lit ? in->lit[i] : NULL;

		for (int k = 0; w[k]; k++)
			if ((w[k] == L'*' || w[k] == L'?') && (!l || l[k] == QUOTE_NONE))
				return true;
	}
	return false;
}

/*
 * 'out' gets the words and redirection files of 'in' with the expansions done
 * (out has no quoting information, it is ready to run); glob: also expands * and ?.
 * false: out of memory.
 */
bool ant_expand_cmd (const ANT_CMD *in, ANT_CMD *out, bool glob) {

	memset(out, 0, sizeof *out);

	for (int i = 0; i < in->argc; i++) {
		BUF b = { 0 };
		bool dropped, added = false;
		GLOB g = { 0 };

		if (!expand_word(in->argv[i], in->lit ? in->lit[i] : NULL, &b, &dropped)) {
			buf_free(&b);
			ant_expand_free(out);
			return false;
		}
		if (dropped) {
			buf_free(&b);
			continue;
		}

		if (glob && is_wild(b.s, b.l)) {
			glob_expand(&b, &g);
			for (int k = 0; k < g.nfound; k++) {
				if (!add_arg(out, g.found[k])) {
					for (int r = k + 1; r < g.nfound; r++)
						free(g.found[r]);
					free(g.found);
					glob_free(&g);
					buf_free(&b);
					ant_expand_free(out);
					return false;
				}
				added = true;
			}
			free(g.found);
			glob_free(&g);
		}
		/* no match (or no pattern): the word stays as typed */
		if (added)
			free(b.s);
		else if (!add_arg(out, b.s)) {
			free(b.l);
			ant_expand_free(out);
			return false;
		}
		free(b.l);
	}

	if (in->nredir > 0) {
		out->redir = calloc(in->nredir, sizeof(ANT_REDIR));
		if (!out->redir) {
			ant_expand_free(out);
			return false;
		}
		for (int i = 0; i < in->nredir; i++) {
			out->redir[i].kind = in->redir[i].kind;
			if (in->redir[i].path) {
				out->redir[i].path = expand_path(in->redir[i].path, in->redir[i].lit);
				if (!out->redir[i].path) {
					out->nredir = i;
					ant_expand_free(out);
					return false;
				}
			}
			out->nredir = i + 1;
		}
	}
	return true;
}
