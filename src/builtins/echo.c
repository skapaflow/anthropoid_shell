#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>

#include "echo.h"
#include "../console/console.h"

/* 'echo [-n] words...': the words separated by a space, then a newline (-n: no newline) */
void ant_cmd_echo (const ANT_CMD *c) {

	int first = 1;
	bool newline = true;

	if (c->argc > 1 && !wcscmp(c->argv[1], L"-n")) {
		newline = false;
		first = 2;
	}
	for (int i = first; i < c->argc; i++) {
		if (i > first)
			putchar(' ');
		ant_write(c->argv[i]);
	}
	if (newline)
		putchar('\n');
}
