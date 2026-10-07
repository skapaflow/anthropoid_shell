#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "which.h"
#include "builtins.h"
#include "path.h"
#include "../config/config.h"
#include "../console/console.h"
#include "../shell/shell.h"

/*************************************************************
 * 'which [-a] name...': what the name runs, in the order    *
 * the shell looks for it:                                   *
 *   1. alias (config.ant [alias])                           *
 *   2. built-in command                                     *
 *   3. shortcut (config.ant [link])                         *
 *   4. the [path] folders of config.ant                     *
 *   5. what CreateProcess finds: the folder of antshell.exe, the *
 *      current folder, the system folders and PATH          *
 * Without -a it stops at the first; -a lists all of them    *
 * (the same file is shown once). Exit code 1 if a name is   *
 * not found.                                                *
 *************************************************************/

#define SEEN_MAX 128

static wchar_t seen[SEEN_MAX][MAX_PATH];
static int nseen;

static bool is_file (const wchar_t *path) {

	DWORD attr = GetFileAttributesW(path);

	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* shows a program once; returns true if it was new */
static bool report (const wchar_t *path) {

	wchar_t full[MAX_PATH];

	if (!GetFullPathNameW(path, MAX_PATH, full, NULL))
		wcscpy(full, path);
	for (int i = 0; i < nseen; i++)
		if (!_wcsicmp(seen[i], full))
			return false;
	if (nseen < SEEN_MAX)
		wcscpy(seen[nseen++], full);
	ant_write(full);
	putchar('\n');
	return true;
}

/* dir\name (name.exe when it has no extension) exists as a file? result in out */
static bool in_dir (const wchar_t *dir, int n, const wchar_t *name, wchar_t *out) {

	if (n <= 0 || n + (int) wcslen(name) + 6 >= MAX_PATH)
		return false;
	wmemcpy(out, dir, n);
	if (out[n - 1] != L'\\' && out[n - 1] != L'/')
		out[n++] = L'\\';
	wcscpy(out + n, name);
	if (!wcsrchr(name, L'.'))
		wcscat(out, L".exe");
	return is_file(out);
}

/* every folder of a ';' list */
static bool search_list (const wchar_t *list, const wchar_t *name, bool all) {

	wchar_t out[MAX_PATH];
	bool found = false;

	for (const wchar_t *p = list; *p; ) {
		const wchar_t *end = wcschr(p, L';');
		int n = end ? (int) (end - p) : (int) wcslen(p);

		if (in_dir(p, n, name, out) && report(out)) {
			found = true;
			if (!all)
				return true;
		}
		if (!end)
			break;
		p = end + 1;
	}
	return found;
}

static bool find_program (const wchar_t *name, bool all) {

	static wchar_t dirs[ANTMAX * 4], path[32768], out[MAX_PATH];
	bool found = false;

	/* with a folder in the name there is no search */
	if (wcspbrk(name, L"\\/:")) {
		if (is_file(name))
			return report(name);
		if (!wcsrchr(name, L'.') && wcslen(name) + 5 < MAX_PATH) {
			swprintf(out, MAX_PATH, L"%ls.exe", name);
			if (is_file(out))
				return report(out);
		}
		return false;
	}

	if (ant_path_dirs(dirs, ANTMAX * 4) > 0 && search_list(dirs, name, all)) {
		found = true;
		if (!all)
			return true;
	}
	if (SearchPathW(NULL, name, wcsrchr(name, L'.') ? NULL : L".exe", MAX_PATH, out, NULL) && report(out)) {
		found = true;
		if (!all)
			return true;
	}
	if (all && GetEnvironmentVariableW(L"PATH", path, 32768) < 32768 && search_list(path, name, true))
		found = true;
	return found;
}

void ant_cmd_which (const ANT_CMD *c) {

	bool all = false;
	int first = 1;

	if (c->argc > 1 && !wcscmp(c->argv[1], L"-a")) {
		all = true;
		first = 2;
	}
	if (first >= c->argc) {
		ant_error(L"usage: which [-a] name...");
		ant_status = 2;
		return;
	}

	for (int i = first; i < c->argc; i++) {
		const wchar_t *name = c->argv[i];
		wchar_t dest[2048];
		const wchar_t *alias = ant_config_alias(name);
		bool found = false;

		nseen = 0;
		if (alias) {
			ant_write(name);
			ant_write(L": aliased to '");
			ant_write(alias);
			ant_write(L"'\n");
			found = true;
		}
		if ((!found || all) && ant_builtin_named(name)) {
			ant_write(name);
			ant_write(L": shell built-in\n");
			found = true;
		}
		if ((!found || all) && ant_config_link(name, dest, 2048)) {
			ant_write(name);
			ant_write(L": link to ");
			ant_write(dest);
			ant_write(L"\n");
			found = true;
		}
		if ((!found || all) && find_program(name, all))
			found = true;

		if (!found) {
			ant_error(L"ant: which: %ls: not found", name);
			ant_status = 1;
		}
	}
}
