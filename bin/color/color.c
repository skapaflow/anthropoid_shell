#include "../common.h"

/*************************************************************
 * color [--help]                                            *
 *                                                           *
 * The 256 text attributes of the Windows console, 16 by 16: *
 * each number written in its own color. The row is the      *
 * background (0-15), the column the text color (0-15), so   *
 * the number is background * 16 + text, as in SetConsole-   *
 * TextAttribute. On a pipe or file only the numbers come.   *
 *************************************************************/

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;

	bin_name = L"color";
	bin_usage =
		"Usage: color\n"
		"Show the 256 console color attributes (row: background, column: text).\n";
	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1)
		opt_bad(ch);
	if (o.i < argc) {
		err_print(L"extra operand '%ls'", argv[o.i]);
		out_flush();
		return 2;
	}

	for (int bg = 0; bg < 16; bg++) {
		for (int fg = 0; fg < 16; fg++) {
			out_color((WORD) (bg * 16 + fg));
			out_fmt(" %3d", bg * 16 + fg);
		}
		out_color_off();
		out_char('\n');
	}
	out_flush();
	return 0;
}
