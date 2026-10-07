#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

#include "args.h"
#include "../console/console.h"
#include "shell.h"

ANT_ARG *ant_arg = NULL;

/* appends n units of s to line, at *k; false if they do not fit */
static bool put (wchar_t *line, int *k, const wchar_t *s, int n) {

	if (*k + n >= ANTMAX)
		return false;
	wmemcpy(line + *k, s, n);
	*k += n;
	line[*k] = 0;
	return true;
}

/*
 * The arguments of antshell.exe become the first command line, in char_line_buf (UTF-8).
 * They are read from the UTF-16 command line: the argv of main comes in the ANSI code
 * page, which loses every character outside it. An argument with a space (or an empty
 * one) goes inside double quotes; a " inside it goes as '"', since the shell has no
 * escape character. false: they do not fit in a line (the error is shown).
 */
bool ant_args_to_line (void) {

	static wchar_t line[ANTMAX];
	int argc, k = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool ok = argv != NULL;

	line[0] = 0;
	char_line_buf[0] = 0;
	for (int i = 1; ok && i < argc; i++) {
		const wchar_t *a = argv[i];
		bool quote = !*a || wcspbrk(a, L" \t");

		if (i > 1)
			ok = put(line, &k, L" ", 1);
		if (ok && quote)
			ok = put(line, &k, L"\"", 1);
		for (; ok && *a; a++) {
			if (*a != L'"')
				ok = put(line, &k, a, 1);
			else if (quote)
				ok = put(line, &k, L"\"'\"'\"", 5); /* closes the quotes, '"', opens them again */
			else
				ok = put(line, &k, L"'\"'", 3);
		}
		if (ok && quote)
			ok = put(line, &k, L"\"", 1);
	}
	if (ok && !WideCharToMultiByte(CP_UTF8, 0, line, -1, char_line_buf, ANTMAX, NULL, NULL))
		ok = false;
	if (argv)
		LocalFree(argv);

	if (!ok) {
		char_line_buf[0] = 0;
		ant_error(L"ant: the arguments are too long for a command line (%d bytes at most)", ANTMAX - 1);
		ant_status = 1;
	}
	return ok;
}

ANT_ARG *ant_arg_insert (const char *str) {

	ANT_ARG *newnode = (ANT_ARG *) malloc(sizeof(ANT_ARG));

	if (!newnode) {
		printf("Memory allocation failed\n");
		exit(1);
	}

	newnode->string = strdup(str);
	newnode->next = NULL;
	newnode->prev = NULL;

	if (ant_arg == NULL) {
		ant_arg = newnode;
	} else {
		ANT_ARG *temp = ant_arg;
		while (temp->next) {
			temp = temp->next;
		}
		temp->next = newnode;
		newnode->prev = temp;
	}

	return newnode;
}

void ant_arg_free (void) {

	ANT_ARG *tmp;
	while (ant_arg) {
		if (ant_arg->string)
			free(ant_arg->string);
		tmp = ant_arg;
		ant_arg = ant_arg->next;
		free(tmp);
	}

	ant_arg = NULL;
}
