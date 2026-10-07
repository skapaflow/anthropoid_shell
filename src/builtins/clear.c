#include <windows.h>

#include "clear.h"
#include "../console/console.h"
#include "../shell/shell.h"

/* '.': clears the screen (the console's buffer, not only the window) */
void ant_cmd_clear (void) {

	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	COORD home = { 0, 0 };
	DWORD cells, count;

	if (!GetConsoleScreenBufferInfo(out, &csbi)) {
		ant_error(L"ant: .: the output is not a console");
		ant_status = 1;
		return;
	}

	cells = csbi.dwSize.X * csbi.dwSize.Y;
	if (!FillConsoleOutputCharacterW(out, L' ', cells, home, &count) ||
		!FillConsoleOutputAttribute(out, csbi.wAttributes, cells, home, &count)) {
		ant_error(L"ant: .: cannot clear the screen (error %lu)", GetLastError());
		ant_status = 1;
		return;
	}
	SetConsoleCursorPosition(out, home);
}
