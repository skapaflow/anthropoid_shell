#include <stdio.h>
#include <windows.h>

#include "clear.h"

void ant_cmd_clear (void) {

    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not get the console handle.\n");
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
        fprintf(stderr, "Could not get the console buffer information.\n");
        return;
    }

    DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD count;
    COORD homeCoords = {0, 0};

    if (!FillConsoleOutputCharacter(hStdOut, ' ', cellCount, homeCoords, &count) ||
        !FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count)) {
        fprintf(stderr, "Could not clear the console screen.\n");
        return;
    }

    SetConsoleCursorPosition(hStdOut, homeCoords);
}