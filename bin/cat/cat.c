#include "../common.h"

/* cat [-n] [file ...]: the files (or standard input, or "-") one after the other; -n numbers the lines */

static bool number;
static unsigned long line_no = 1;
static bool at_line_start = true;

static void put_chunk (const char *buf, int n) {

	if (!number) {
		out_bytes(buf, n);
		return;
	}
	for (int i = 0; i < n; i++) {
		if (at_line_start) {
			out_fmt("%6lu\t", line_no++);
			at_line_start = false;
		}
		out_char(buf[i]);
		if (buf[i] == '\n')
			at_line_start = true;
	}
}

static void cat_handle (HANDLE h, const wchar_t *name) {

	static char buf[65536];
	int got;

	while ((got = in_read(h, buf, sizeof buf)) > 0)
		put_chunk(buf, got);
	if (got < 0)
		err_path(name, GetLastError());
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;

	bin_name = L"cat";
	bin_usage = "usage: cat [-n] [file ...]\n"
		"  Writes the files one after the other (no file or \"-\": standard input).\n"
		"  -n  number the lines\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch == 'n')
			number = true;
		else
			opt_bad(ch);
	}

	if (o.i >= argc)
		cat_handle(GetStdHandle(STD_INPUT_HANDLE), L"-");
	for (int i = o.i; i < argc; i++) {
		HANDLE h = in_open(argv[i]);

		if (h == INVALID_HANDLE_VALUE)
			continue;
		cat_handle(h, argv[i]);
		in_close(h);
	}
	out_flush();
	return bin_status;
}
