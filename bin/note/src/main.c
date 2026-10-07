#include <wchar.h>
#include <wctype.h>
#include <windows.h>
#include <shellapi.h>

#include "editor.h"
#include "screen.h"

/*
 * "file.c:120" or "file.c:120:5", as compilers print them: the numbers are cut
 * off the name. A ':' is never part of a file name on Windows, except after a
 * drive letter ("C:5" is left alone).
 */
static void split_position (wchar_t *arg, int *line, int *col) {

	int numbers[2], count = 0;
	wchar_t *colon, *p;

	while (count < 2 && (colon = wcsrchr(arg, L':')) != NULL && colon - arg > 1 && colon[1]) {
		for (p = colon + 1; *p && iswdigit(*p); p++)
			;
		if (*p)
			break;
		numbers[count++] = _wtoi(colon + 1);
		*colon = 0;
	}
	if (count == 1)
		*line = numbers[0];
	else if (count == 2) {
		*line = numbers[1];
		*col = numbers[0];
	}
}

int main (void) {

	int argc, line = 0, col = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (!argv)
		return 1;

	if (argc > 2 || (argc == 2 && (!wcscmp(argv[1], L"/?") || !wcscmp(argv[1], L"-h") || !wcscmp(argv[1], L"--help")))) {
		console_write(STD_OUTPUT_HANDLE,
			L"note - a simple text editor for the console\n"
			L"\n"
			L"usage: note [file[:line[:column]]]\n"
			L"\n"
			L"  F1      keyboard shortcuts\n"
			L"  Alt     menu bar\n"
			L"  Ctrl+S  save\n"
			L"  Esc     exit (asks first)\n"
			L"\n"
			L"Settings and colors: note.cfg next to note.exe\n");
		return argc > 2;
	}

	if (argc == 2)
		split_position(argv[1], &line, &col);
	return editor_run(argc == 2 ? argv[1] : NULL, line, col);
}
