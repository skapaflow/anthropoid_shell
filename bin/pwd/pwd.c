#include "../common.h"

/*
 * pwd [-L | -P]: writes the current folder. It is here by convention: the shell has its own
 * "/" (and cd shows the folder too). -L and -P are accepted and do the same thing.
 */

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	wchar_t cwd[32768];
	DWORD n;

	bin_name = L"pwd";
	bin_usage = "usage: pwd [-L | -P]\n  Writes the current folder (the shell also does it with \"/\").\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch != 'L' && ch != 'P')
			opt_bad(ch);
	}
	if (o.i < argc) {
		err_print(L"too many arguments");
		return 1;
	}
	n = GetCurrentDirectoryW(32768, cwd);
	if (n == 0 || n >= 32768) {
		err_print(L"cannot get the current folder: %ls", err_text(GetLastError()));
		return 1;
	}
	out_w(cwd);
	out_char('\n');
	out_flush();
	return 0;
}
