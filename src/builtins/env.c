#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "env.h"
#include "../console/console.h"
#include "../shell/shell.h"

/*************************************************************
 * Environment: export, unset and NAME=value. The shell      *
 * process owns the environment the programs inherit, so     *
 * this cannot be an external program.                       *
 *                                                           *
 *   export NAME=value ...   sets variables                  *
 *   export                  lists them (NAME=value)         *
 *   unset NAME ...          removes variables               *
 *   NAME=value ...          same as export (there are no    *
 *                           variables that stay in the      *
 *                           shell only)                     *
 *                                                           *
 * They last until the shell closes; to keep one, put it in  *
 * the [export] section of data\config.ant.                  *
 *************************************************************/

static bool name_start (wchar_t c) {

	return c == L'_' || (c < 128 && iswalpha(c));
}

/* NAME (letters, digits and _, not starting with a digit) in s[0..n) */
static bool valid_name (const wchar_t *s, int n) {

	if (n <= 0 || !name_start(s[0]))
		return false;
	for (int i = 1; i < n; i++)
		if (!name_start(s[i]) && !(s[i] >= L'0' && s[i] <= L'9'))
			return false;
	return true;
}

static bool is_assignment (const wchar_t *w) {

	const wchar_t *eq = wcschr(w, L'=');

	return eq && valid_name(w, (int) (eq - w));
}

/* every word is NAME=value (so "A=1 B=2" is, and "A=1 cmd" is not) */
bool ant_env_is_assignment (const ANT_CMD *c) {

	if (c->argc == 0)
		return false;
	for (int i = 0; i < c->argc; i++)
		if (!is_assignment(c->argv[i]))
			return false;
	return true;
}

static void list_environment (void) {

	wchar_t *block = GetEnvironmentStringsW();

	if (!block)
		return;
	for (const wchar_t *v = block; *v; v += wcslen(v) + 1) {
		/* "=C:=C:\dir" and the like are not variables */
		if (*v == L'=')
			continue;
		ant_write(v);
		putchar('\n');
	}
	FreeEnvironmentStringsW(block);
}

/* 'export ...' or the assignment words themselves */
void ant_cmd_export (const ANT_CMD *c) {

	bool word = !wcscmp(c->argv[0], L"export");

	if (word && c->argc == 1) {
		list_environment();
		return;
	}
	for (int i = word ? 1 : 0; i < c->argc; i++) {
		const wchar_t *w = c->argv[i], *eq = wcschr(w, L'=');
		wchar_t name[256];
		int n = eq ? (int) (eq - w) : (int) wcslen(w);

		if (!valid_name(w, n) || n >= 256) {
			ant_error(L"ant: export: '%ls': not a valid identifier", w);
			ant_status = 1;
			continue;
		}
		/* "export NAME" without a value: nothing to do, the variable stays as it is */
		if (!eq)
			continue;
		wmemcpy(name, w, n);
		name[n] = 0;
		if (!SetEnvironmentVariableW(name, eq + 1)) {
			ant_error(L"ant: export: '%ls': cannot set (error %lu)", name, GetLastError());
			ant_status = 1;
		}
	}
}

void ant_cmd_unset (const ANT_CMD *c) {

	for (int i = 1; i < c->argc; i++) {
		if (!valid_name(c->argv[i], (int) wcslen(c->argv[i]))) {
			ant_error(L"ant: unset: '%ls': not a valid identifier", c->argv[i]);
			ant_status = 1;
			continue;
		}
		SetEnvironmentVariableW(c->argv[i], NULL);
	}
}
