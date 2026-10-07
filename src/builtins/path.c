#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <windows.h>

#include "path.h"
#include "../config/config.h"
#include "../console/console.h"
#include "../shell/shell.h"

/* 'path' command: lists the [path] folders from config.ant; 'path -f' opens the file */
void ant_cmd_path (const char *opt) {

	static wchar_t dir[32768];
	static char buf[32768 * 3];

	if (opt && !strcmp(opt, "-f")) {
		if (!ant_config_open()) {
			ant_error(L" [!] could not open data\\config.ant");
			ant_status = 1;
		}
		return;
	}

	for (int i = 0; ant_config_value(CONFIG_PATH, i, dir, 32768); i++)
		if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, buf, sizeof buf, NULL, NULL))
			print(CLEAR|CYAN, "%s\n", buf);
	printf("\n path -f   opens data\\config.ant ([path] section)\n");
}

/*
 * Looks for 'name' in the [path] folders from config.ant, in file order;
 * without an extension, looks for name.exe. Returns the full path in out.
 */
bool ant_path_find (const wchar_t *name, wchar_t *out, int max) {

	static wchar_t dirs[ANTMAX * 4];
	const wchar_t *dot = wcsrchr(name, L'.');
	bool add_exe = !dot;

	if (ant_path_dirs(dirs, ANTMAX * 4) == 0)
		return false;

	for (wchar_t *p = dirs; *p; ) {
		wchar_t *end = wcschr(p, L';');
		int n = end ? (int) (end - p) : (int) wcslen(p);

		if (n > 0 && n + (int) wcslen(name) + 6 < max) {
			DWORD attr;
			wmemcpy(out, p, n);
			if (out[n - 1] != L'\\' && out[n - 1] != L'/')
				out[n++] = L'\\';
			wcscpy(out + n, name);
			if (add_exe)
				wcscat(out, L".exe");
			attr = GetFileAttributesW(out);
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
				return true;
		}
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

/*
 * [path] folders from config.ant separated by ';' like PATH, in out[0..max).
 * Returns the length. Used by TAB to complete commands in the first word.
 */
int ant_path_dirs (wchar_t *out, int max) {

	return ant_config_path(out, max);
}
