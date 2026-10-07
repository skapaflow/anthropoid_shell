#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "cd.h"
#include "link.h"
#include "../console/console.h"
#include "../config/config.h"
#include "../shell/execute.h"
#include "../shell/shell.h"

/*************************************************************
 * Shortcuts in the [link] section of data\config.ant:       *
 *   name = target                                           *
 *                                                           *
 * Port of link.c from the old project:                      *
 *   link        lists the shortcuts                         *
 *   link -f     opens config.ant                            *
 *   name        opens the target (a program runs in the     *
 *               console; folders and files open in the      *
 *               default program)                            *
 *   name\       enters the folder (cd; name/ works too)     *
 *   name.       opens the target and closes the terminal    *
 *                                                           *
 * The whole line must be the name (no spaces); names        *
 * ignore case; the first of a repeated name wins.           *
 *************************************************************/

#define LINK_TARGET 32768

enum { MODE_OPEN, MODE_CD, MODE_EXIT };

static void to_utf8 (const wchar_t *w, char *out, int max) {

	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, max, NULL, NULL))
		out[0] = 0;
}

static void link_list (void) {

	static wchar_t target[LINK_TARGET];
	static char name[1024], text[LINK_TARGET * 3];

	for (int i = 0; ant_config_value(CONFIG_LINK, i, target, LINK_TARGET); i++) {
		to_utf8(ant_config_name(CONFIG_LINK, i), name, sizeof name);
		to_utf8(target, text, sizeof text);
		print(CLEAR|CYAN, "[%s]", name);
		printf(" %s\n", text);
	}

	printf("\n link -f   opens data\\config.ant ([link] section)"
		"\n name      opens the shortcut"
		"\n name.     opens the shortcut and closes the terminal"
		"\n name\\     enters the shortcut folder\n");
}

/* program: runs in the console, like a command; anything else opens in the default program */
static bool is_program (const wchar_t *target) {

	int n = wcslen(target);
	return n > 4 && (!_wcsicmp(target + n - 4, L".exe") || !_wcsicmp(target + n - 4, L".com"));
}

static bool open_target (const wchar_t *target) {

	return (INT_PTR) ShellExecuteW(NULL, L"open", target, NULL, NULL, SW_SHOWMAXIMIZED) > 32;
}

static void missing (const wchar_t *name, const wchar_t *target) {

	ant_error(L" [!] shortcut [%ls]: \"%ls\" does not exist", name, target);
	ant_status = 1;
}

/* is the line the link command or a config.ant shortcut? (only checks, does not run) */
bool ant_link_is (const char *text) {

	static wchar_t target[LINK_TARGET];
	wchar_t line[ANTMAX];
	int n;

	if (!_stricmp(text, "link") || !_stricmp(text, "link -f"))
		return true;
	if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, line, ANTMAX))
		return false;
	n = wcslen(line);
	if (n > 1 && wcschr(L"\\/.", line[n - 1]))
		line[--n] = 0;
	if (n == 0 || wcspbrk(line, L" \t\\/:\""))
		return false;
	return ant_config_link(line, target, LINK_TARGET);
}

/*
 * Handles the line if it is a shortcut (or the link command). Returns
 * ANT_LINK_NONE when it is not, so the line goes on to be executed.
 */
int ant_cmd_link (const char *text) {

	static wchar_t target[LINK_TARGET];
	wchar_t line[ANTMAX];
	int n, mode = MODE_OPEN;

	if (!_stricmp(text, "link")) {
		link_list();
		return ANT_LINK_DONE;
	}
	if (!_stricmp(text, "link -f")) {
		if (!ant_config_open()) {
			ant_error(L" [!] could not open data\\config.ant");
			ant_status = 1;
		}
		return ANT_LINK_DONE;
	}

	if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, line, ANTMAX))
		return ANT_LINK_NONE;
	n = wcslen(line);
	if (n > 1 && (line[n - 1] == L'\\' || line[n - 1] == L'/'))
		mode = MODE_CD;
	else if (n > 1 && line[n - 1] == L'.')
		mode = MODE_EXIT;
	if (mode != MODE_OPEN)
		line[--n] = 0;

	/* shortcut name: a single word, no path */
	if (n == 0 || wcspbrk(line, L" \t\\/:\""))
		return ANT_LINK_NONE;
	if (!ant_config_link(line, target, LINK_TARGET))
		return ANT_LINK_NONE;

	if (mode == MODE_CD) {
		ant_cd_to(target);
		return ANT_LINK_DONE;
	}

	if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) {
		missing(line, target);
		return ANT_LINK_DONE;
	}

	if (mode == MODE_EXIT) {
		if (!open_target(target)) {
			missing(line, target);
			return ANT_LINK_DONE;
		}
		return ANT_LINK_EXIT;
	}

	if (is_program(target)) {
		wchar_t *argv[1] = { target };
		ant_exec_program(1, argv);
	} else if (!open_target(target))
		missing(line, target);

	return ANT_LINK_DONE;
}
