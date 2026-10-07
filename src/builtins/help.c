#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <windows.h>

#include "help.h"
#include "../console/console.h"
#include "../shell/shell.h"

#define KEY_WIDTH 16 /* the names of an entry fill the first 16 columns of its line */

/* is 'name' one of the names in the key field of the line? (exact, or only the start when prefix) */
static bool line_has_name (const char *line, const char *name, bool prefix) {

	char key[KEY_WIDTH + 1];
	size_t n = strlen(name);

	strncpy(key, line, KEY_WIDTH);
	key[KEY_WIDTH] = 0;
	/* the names are separated by spaces, '/', ',' and parentheses: "HEAD / TAIL", "EXIT/NOP", "(SHIFT + TAB)" */
	for (char *tok = strtok(key, " /,()"); tok; tok = strtok(NULL, " /,()")) {
		if (strlen(tok) >= n && !_strnicmp(tok, name, n) && (prefix || strlen(tok) == n))
			return true;
	}
	return false;
}

static void print_line (const char *line) {

	print(CLEAR|CYAN, " %s", line);
	putchar(10);
}

/*
 * 'help': shows data\help.ant (port of help.c from the old project).
 * 'help name ...': only the entries that have that name (HISTORY, HEAD, TAIL, EXIT, ...);
 * without an exact name, the ones that start with it, as in bash's "help his*".
 */
void ant_cmd_help (const ANT_CMD *c) {

	static char lines[512][ANTMAX];
	wchar_t path[MAX_PATH];
	char buf[ANTMAX];
	FILE *file;
	int n = GetModuleFileNameW(NULL, path, MAX_PATH), count = 0;

	while (n > 0 && path[n - 1] != L'\\')
		n--;
	if (n == 0 || n + 24 >= MAX_PATH)
		return;
	wcscpy(path + n, L"data\\help.ant");

	if (!(file = _wfopen(path, L"rb"))) {
		ant_error(L" [!] data\\help.ant not found");
		ant_status = 1;
		return;
	}
	while (count < 512 && fgets(buf, ANTMAX, file)) {
		int len = strlen(buf);
		while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = 0;
		strcpy(lines[count++], buf);
	}
	fclose(file);

	if (c->argc < 2) {
		for (int i = 0; i < count; i++)
			print_line(lines[i]);
		return;
	}

	for (int a = 1; a < c->argc; a++) {
		char name[ANTMAX];
		int found = 0;

		if (!WideCharToMultiByte(CP_UTF8, 0, c->argv[a], -1, name, sizeof name, NULL, NULL))
			continue;
		for (int pass = 0; pass < 2 && !found; pass++)
			for (int i = 0; i < count; i++)
				if (line_has_name(lines[i], name, pass == 1)) {
					print_line(lines[i]);
					found++;
				}
		if (!found) {
			ant_error(L"ant: help: no help topics match '%ls'", c->argv[a]);
			ant_status = 1;
		}
	}
}
