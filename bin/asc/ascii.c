#include <stdio.h>
#include <stdarg.h>
#include <windows.h>
#define ASCII 127
#define ASCII_EXTENDED 255
#define BOX_H 196	/* horizontal line in code pages 437/850 */

#define GREEN 10
#define CYAN 11
#define RED 12
#define WHITE 15

static HANDLE out;

void print (int color, const char *fmt, ...) {

	CONSOLE_SCREEN_BUFFER_INFO info;
	BOOL console = GetConsoleScreenBufferInfo(out, &info);
	va_list ap;

	fflush(stdout);
	if (console) SetConsoleTextAttribute(out, color);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (console) SetConsoleTextAttribute(out, info.wAttributes);
}

void print_asc (const char *s, int i) {

	printf("[");
	if (s == NULL)
		print(CYAN, "%c ", i);
	else
		print(CYAN, "%s", s);
	print(WHITE, "%3d", i);
	print(RED, "x%02x", i);
	print(GREEN, "%03o", i);
	printf("] ");
}

int main (void) {

	static const char *esc[14] = {
		[0]  = "\\0", [7]  = "\\a", [8]  = "\\b", [9]  = "\\t",
		[10] = "\\n", [11] = "\\v", [12] = "\\f", [13] = "\\r"
	};

	out = GetStdHandle(STD_OUTPUT_HANDLE);
	print(CYAN, " American Standard Code for Information Interchange\n ");
	for (int i = 0; i < 50; i++)
		putchar(BOX_H);
	putchar('\n');
	for (int i = 0, j = 0; i <= ASCII_EXTENDED; i++, j++) {
		if (j == 7) {
			putchar('\n');
			j = 0;
		}
		print_asc(i < 14 ? esc[i] : NULL, i);
		if (i == ASCII) {
			print(CYAN, "\n\n Extended (code page %u)\n", GetConsoleOutputCP());
			j = -1;
		}
	}
	putchar('\n');
	return 0;
}
