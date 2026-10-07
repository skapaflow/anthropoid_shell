#include <wchar.h>
#include <windows.h>

#include "console/console.h"
#include "shell/shell.h"

int main (int argc, char const *argv[]) {

#ifdef __WIN32__
	ant_saved_codepage = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
#endif

	if (argc == 1)
		ant_logo(22);

	ant_shell_run(argc > 1);

#ifdef __WIN32__
	SetConsoleOutputCP(ant_saved_codepage);
#endif

	/* the code of "exit N" (0 when the shell closed any other way) */
	return ant_status;
}