#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

#include "bg.h"
#include "builtins.h"
#include "../console/console.h"
#include "../shell/execute.h"
#include "../shell/shell.h"

/*************************************************************
 * 'bg [-o] [-d] program [args...]': runs the program in the *
 * background, in a console of its own that stays hidden: the *
 * terminal is free and closing it does not end the program.  *
 * What it writes stays in that console (progress bars and    *
 * the like), to be seen later with 'show'. It becomes a job: *
 * jobs, wait %n and kill %n work on it.                      *
 *   -o  stdout and stderr go to <program>_<pid>.txt in the   *
 *       current folder (one file per run: youtube_11804.txt) *
 *   -d  disables the close button of the hidden console, so  *
 *       a shown window cannot end the program by accident    *
 * It runs programs only: a built-in, an alias or a [link]    *
 * runs in the shell itself and cannot go to the background   *
 * this way.                                                  *
 *************************************************************/

/* output file base: the program name without folder and extension */
static bool base_name (const wchar_t *program, wchar_t *out, int max) {

	const wchar_t *base = program, *dot;
	int n;

	for (const wchar_t *p = program; *p; p++)
		if (*p == L'\\' || *p == L'/' || *p == L':')
			base = p + 1;
	dot = wcsrchr(base, L'.');
	n = dot && dot > base ? (int) (dot - base) : (int) wcslen(base);
	if (n == 0 || n + 16 > max)
		return false;
	wmemcpy(out, base, n);
	out[n] = 0;
	return true;
}

void ant_cmd_bg (const ANT_CMD *c) {

	wchar_t base[MAX_PATH];
	ANT_CMD inner = { 0 };
	bool to_file = false, no_close = false;
	int first = 1;

	/* options come before the program: later words belong to it */
	for (; first < c->argc && c->argv[first][0] == L'-' && c->argv[first][1]; first++) {
		const wchar_t *o = c->argv[first] + 1;
		for (; *o; o++) {
			if (*o == L'o')
				to_file = true;
			else if (*o == L'd')
				no_close = true;
			else {
				ant_error(L"ant: bg: -%lc: invalid option", *o);
				ant_status = 2;
				return;
			}
		}
	}
	if (first >= c->argc) {
		ant_error(L"usage: bg [-o] [-d] program [args...]   (-o: output to <program>_<pid>.txt, -d: no close button)");
		ant_status = 2;
		return;
	}
	inner.argc = c->argc - first;
	inner.argv = c->argv + first;
	if (ant_is_builtin(&inner)) {
		ant_error(L"ant: bg: %ls: a built-in runs in the shell, only programs go to the background", c->argv[first]);
		ant_status = 1;
		return;
	}
	if (!base_name(c->argv[first], base, MAX_PATH)) {
		ant_error(L"ant: bg: %ls: invalid program name", c->argv[first]);
		ant_status = 1;
		return;
	}
	ant_job_background(inner.argc, inner.argv, base, to_file, no_close);
}
