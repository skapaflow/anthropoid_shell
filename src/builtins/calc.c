#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "calc.h"
#include "../shell/shell.h"
#include "../console/console.h"

/* only digits, '.' and '-' exist in the big font */
static bool big_text_fits (const char *s) {

	int w = get_console_width();

	if (!*s || (int) strlen(s) * 6 + 1 > w)
		return false;
	for (; *s; s++)
		if (!strchr("0123456789.-", *s))
			return false;
	return true;
}

/*
 * Calculator (the line is a calculator expression): infix or RPN, see calc_eval.c.
 * Returns 0 when the line was handled (the result, or an error, was shown) and 1
 * when it is not an expression. In the console the result comes in big letters;
 * redirected (2+3 | cat, 2+3 > f), as plain text. ant_status: 0, 1 arithmetic
 * error (division by zero), 2 syntax error.
 */
int ant_cmd_calc (char *str) {

	char text[512], err[256];
	double value;
	int code = 0;
	DWORD mode;

	if (!str || !*str || !ant_calc_is_expression(str))
		return 1;

	if (!ant_calc_eval(str, &value, err, sizeof err, &code)) {
		wchar_t w[256];

		MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 256);
		ant_error(L"ant: calc: %ls", w);
		ant_status = code;
		return 0;
	}
	ant_status = 0;
	ant_calc_format(value, text, sizeof text);

	if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
		printf("%s\n", text);
	} else if (big_text_fits(text)) {
		printf("\n");
		print_big_text(text, '\xdb', CLEAR|GREEN);
	} else {
		printf("\n");
		print(CLEAR|GREEN, " %s\n", text);
	}
	return 0;
}
