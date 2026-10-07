#include "../common.h"

/* head [-n N | -N] [-c N] [file ...]: the first 10 lines (or N lines, or N bytes with -c) of each file or of standard input */

static long count = 10;
static bool by_bytes;

static void head_handle (HANDLE h, const wchar_t *name) {

	static char buf[65536];
	long left = count;
	int got;

	while (left > 0 && (got = in_read(h, buf, sizeof buf)) > 0) {
		int use = 0;

		if (by_bytes) {
			use = got < left ? got : (int) left;
			left -= use;
		} else {
			while (use < got && left > 0) {
				if (buf[use++] == '\n')
					left--;
			}
		}
		out_bytes(buf, use);
		if (use < got)
			break;
	}
	if (left > 0 && got < 0)
		err_path(name, GetLastError());
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles;
	bool digits = false;
	long number = 0;

	bin_name = L"head";
	bin_usage = "usage: head [-n N | -N] [-c N] [file ...]\n"
		"  The first 10 lines of each file (or of standard input).\n"
		"  -n N, -N  the first N lines\n"
		"  -c N      the first N bytes\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch >= '0' && ch <= '9') {
			number = number * 10 + (ch - '0');
			digits = true;
		} else if (ch == 'n' || ch == 'c') {
			const wchar_t *v = opt_arg(&o, ch);
			wchar_t *end;
			count = wcstol(v, &end, 10);
			if (*end || count < 0) {
				err_print(L"invalid number: '%ls'", v);
				return 2;
			}
			by_bytes = ch == 'c';
			digits = false;
		} else
			opt_bad(ch);
	}
	if (digits)
		count = number;

	nfiles = argc - o.i;
	if (nfiles == 0)
		head_handle(GetStdHandle(STD_INPUT_HANDLE), L"-");
	for (int i = o.i; i < argc; i++) {
		HANDLE h = in_open(argv[i]);

		if (h == INVALID_HANDLE_VALUE)
			continue;
		if (nfiles > 1) {
			if (i > o.i)
				out_char('\n');
			out_str("==> ");
			out_w(argv[i]);
			out_str(" <==\n");
		}
		head_handle(h, argv[i]);
		in_close(h);
	}
	out_flush();
	return bin_status;
}
