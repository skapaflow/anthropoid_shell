#include "../common.h"

/*
 * tail [-n N | -N | -n +N] [-f] [file ...]
 * The last 10 lines (or N lines; +N: from line N on) of each file or of standard input.
 * -f keeps the (single) file open and writes what is added to it, until Ctrl+C.
 */

static long count = 10;
static bool from_start; /* -n +N */

/* big inputs are written in pieces (out_bytes takes an int) */
static void put_all (const char *data, size_t len) {

	while (len > 0) {
		int k = len > (1u << 30) ? (1 << 30) : (int) len;
		out_bytes(data, k);
		data += k;
		len -= k;
	}
}

/* prints the tail of data[0..len) */
static void tail_data (const char *data, size_t len) {

	size_t i;

	if (from_start) {
		long skip = count > 0 ? count - 1 : 0;

		for (i = 0; i < len && skip > 0; i++)
			if (data[i] == '\n')
				skip--;
		put_all(data + i, len - i);
		return;
	}
	if (count == 0)
		return;
	{
		size_t end = len;
		long seen = 0;

		/* a newline that ends the text does not start a line */
		if (end > 0 && data[end - 1] == '\n')
			end--;
		for (i = end; i > 0; i--)
			if (data[i - 1] == '\n' && ++seen == count)
				break;
		put_all(data + i, len - i);
	}
}

static void tail_handle (HANDLE h, const wchar_t *name, bool follow) {

	char *data;
	size_t len;

	if (!in_slurp(h, &data, &len)) {
		err_path(name, GetLastError());
		return;
	}
	tail_data(data, len);
	free(data);

	if (follow) {
		static char buf[65536];
		LARGE_INTEGER size;
		LONGLONG pos = (LONGLONG) len;

		for (;;) {
			int got;
			out_flush();
			Sleep(250);
			if (!GetFileSizeEx(h, &size))
				return;
			if (size.QuadPart < pos) {
				/* truncated: starts again */
				SetFilePointer(h, 0, NULL, FILE_BEGIN);
				pos = 0;
			}
			while (pos < size.QuadPart && (got = in_read(h, buf, sizeof buf)) > 0) {
				out_bytes(buf, got);
				pos += got;
			}
		}
	}
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles;
	bool digits = false, follow = false;
	long number = 0;

	bin_name = L"tail";
	bin_usage = "usage: tail [-n N | -N | -n +N] [-f] [file ...]\n"
		"  The last 10 lines of each file (or of standard input).\n"
		"  -n N, -N  the last N lines; -n +N: from line N on\n"
		"  -f        keep the file open and write what is added to it (one file)\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch >= '0' && ch <= '9') {
			number = number * 10 + (ch - '0');
			digits = true;
		} else if (ch == 'n') {
			const wchar_t *v = opt_arg(&o, ch);
			wchar_t *end;
			from_start = v[0] == L'+';
			count = wcstol(v, &end, 10);
			if (*end || count < 0) {
				err_print(L"invalid number: '%ls'", v);
				return 2;
			}
			digits = false;
		} else if (ch == 'f')
			follow = true;
		else
			opt_bad(ch);
	}
	if (digits) {
		count = number;
		from_start = false;
	}

	nfiles = argc - o.i;
	if (follow && nfiles != 1) {
		err_print(L"-f needs exactly one file");
		return 2;
	}
	if (nfiles == 0)
		tail_handle(GetStdHandle(STD_INPUT_HANDLE), L"-", false);
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
		tail_handle(h, argv[i], follow);
		in_close(h);
	}
	out_flush();
	return bin_status;
}
