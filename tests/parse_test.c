/*************************************************************
 * Test of the parse module (src/shell/parser.c).              *
 *                                                           *
 * Each line becomes a canonical text:                        *
 *   {arg,arg >arq 2>&1} | {arg} && {arg} ;                  *
 * and is compared with the expected one. Syntax errors      *
 * become "ERRORR: <message>".                                       *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

#include "../src/shell/parser.h"

static int failures = 0, total = 0;

static void dump (const ANT_LIST *list, wchar_t *out) {

	static const wchar_t *redir[] = { L"<", L">", L">>", L"2>", L"2>>", L"2>&1" };
	static const wchar_t *op[] = { L"", L" ;", L" &&", L" ||", L" &" };

	out[0] = 0;
	for (int i = 0; i < list->n; i++) {
		const ANT_PIPELINE *p = &list->p[i];
		if (i)
			wcscat(out, L" ");
		for (int j = 0; j < p->ncmd; j++) {
			const ANT_CMD *c = &p->cmd[j];
			if (j)
				wcscat(out, L" | ");
			wcscat(out, L"{");
			for (int k = 0; k < c->argc; k++) {
				if (k)
					wcscat(out, L",");
				wcscat(out, c->argv[k]);
			}
			for (int k = 0; k < c->nredir; k++) {
				wcscat(out, L" ");
				wcscat(out, redir[c->redir[k].kind]);
				if (c->redir[k].path)
					wcscat(out, c->redir[k].path);
			}
			wcscat(out, L"}");
		}
		wcscat(out, op[p->op]);
	}
}

static void check (const wchar_t *line, const wchar_t *want) {

	ANT_LIST list;
	wchar_t got[4096];
	char a[4096], b[4096], l[1024];

	if (ant_parse(line, &list)) {
		dump(&list, got);
		ant_parse_free(&list);
	} else
		swprintf(got, 4096, L"ERROR: %ls", list.error);

	total++;
	if (!wcscmp(got, want))
		return;
	failures++;
	WideCharToMultiByte(CP_UTF8, 0, line, -1, l, sizeof l, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, want, -1, a, sizeof a, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, got, -1, b, sizeof b, NULL, NULL);
	printf("FAIL  [%s]\n      expected: %s\n      got:      %s\n", l, a, b);
}

/* test aliases: ll and up use others; a1 and b1 point at each other */
static const wchar_t *test_alias (const wchar_t *name) {

	static const wchar_t *table[][2] = {
		{ L"ll", L"fs -l" }, { L"ls", L"ls -a" }, { L"up", L"cd.. && ll" },
		{ L"a1", L"b1" }, { L"b1", L"a1" }, { L"quit", L"|" }, { NULL, NULL }
	};

	for (int i = 0; table[i][0]; i++)
		if (!_wcsicmp(table[i][0], name))
			return table[i][1];
	return NULL;
}

int main (void) {

	/* words and quotes */
	check(L"", L"");
	check(L"   ", L"");
	check(L"a", L"{a}");
	check(L"a b  c", L"{a,b,c}");
	check(L"type \"my file.txt\"", L"{type,my file.txt}");
	check(L"echo 'k & l' \"x | y\"", L"{echo,k & l,x | y}");
	check(L"a\"b c\"d", L"{ab cd}");
	check(L"echo \"\"", L"{echo,}");
	check(L"echo '\"'", L"{echo,\"}");
	check(L"cd C:\\Users\\a\\", L"{cd,C:\\Users\\a\\}");
	check(L"echo a\u00e7\u00e3o \U0001F642", L"{echo,a\u00e7\u00e3o,\U0001F642}");

	/* pipes and lists */
	check(L"a | b | c", L"{a} | {b} | {c}");
	check(L"a|b", L"{a} | {b}");
	check(L"a && b || c ; d & e &", L"{a} && {b} || {c} ; {d} & {e} &");
	check(L"a;b", L"{a} ; {b}");
	check(L"a ;", L"{a}");
	check(L"a &", L"{a} &");
	check(L"make && term", L"{make} && {term}");

	/* redirections */
	check(L"dir > list.txt", L"{dir >list.txt}");
	check(L"dir>list.txt", L"{dir >list.txt}");
	check(L"a >> log.txt", L"{a >>log.txt}");
	check(L"sort < names.txt", L"{sort <names.txt}");
	check(L"a 2> err.txt", L"{a 2>err.txt}");
	check(L"a 2>> err.txt", L"{a 2>>err.txt}");
	check(L"make > out.txt 2>&1", L"{make >out.txt 2>&1}");
	check(L"make 2>&1 | findstr error", L"{make 2>&1} | {findstr,error}");
	check(L"a > \"with space \u00e7.txt\"", L"{a >with space \u00e7.txt}");
	check(L"a2>x", L"{a2 >x}");
	check(L"echo 2 > x", L"{echo,2 >x}");

	/* calculator and cd are still words */
	check(L"2+3", L"{2+3}");
	check(L"(1+2)*3", L"{(1+2)*3}");
	check(L"2 3 +", L"{2,3,+}");
	check(L"cd..", L"{cd..}");
	check(L"cd\\", L"{cd\\}");
	check(L"cd > x", L"{cd >x}");
	check(L"cd<x", L"{cd <x}");
	check(L"@notepad a.txt", L"{@notepad,a.txt}");

	/* erros */
	check(L"| a", L"ERROR: syntax error near '|'");
	check(L"a |", L"ERROR: syntax error near '|'");
	check(L"a | | b", L"ERROR: syntax error near '|'");
	check(L"&& a", L"ERROR: syntax error near '&&'");
	check(L"a &&", L"ERROR: incomplete command after '&&'");
	check(L"a ||", L"ERROR: incomplete command after '||'");
	check(L"; a", L"ERROR: syntax error near ';'");
	check(L"a >", L"ERROR: missing file after '>'");
	check(L"a > | b", L"ERROR: missing file after '>'");
	check(L"a > > b", L"ERROR: syntax error near '>'");
	check(L"echo \"abc", L"ERROR: unclosed quote \"");
	check(L"echo 'abc", L"ERROR: unclosed quote '");
	check(L"> x", L"ERROR: missing command before '>'");

	/* alias: 1st word of each command, unquoted, as in sh */
	ant_parse_set_alias(test_alias);
	check(L"ll", L"{fs,-l}");
	check(L"LL dir", L"{fs,-l,dir}");
	check(L"ll | more", L"{fs,-l} | {more}");
	check(L"echo ll", L"{echo,ll}");
	check(L"a && ll ; ll &", L"{a} && {fs,-l} ; {fs,-l} &");
	check(L"\"ll\"", L"{ll}");
	check(L"'ll' x", L"{ll,x}");
	check(L"ll\"x\"", L"{llx}");
	check(L"ls", L"{ls,-a}");
	check(L"up", L"{cd..} && {fs,-l}");
	check(L"a1", L"{a1}");
	check(L"b1 x", L"{b1,x}");
	check(L"> x ll", L"{fs,-l >x}");
	check(L"ll > x", L"{fs,-l >x}");
	check(L"quit", L"ERROR: syntax error near '|'");
	ant_parse_set_alias(NULL);
	check(L"ll", L"{ll}");

	printf("parse: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
