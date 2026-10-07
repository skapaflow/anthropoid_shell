#include "../common.h"

/*
 * uniq [-c] [-d] [-u] [-i] [file]: collapses the equal lines that follow each other.
 *   -c  puts the number of times before each line   -d  only the repeated ones (one copy each)
 *   -u  only the ones that are not repeated          -i  ignores upper/lower case (ASCII)
 * (sort | uniq is how to find the repeats in a whole file)
 */

static bool opt_count, opt_dup, opt_uniq, opt_icase;

static bool same_line (const char *a, size_t na, const char *b, size_t nb) {

	if (na != nb)
		return false;
	if (!opt_icase)
		return memcmp(a, b, na) == 0;
	for (size_t i = 0; i < na; i++) {
		unsigned char x = (unsigned char) a[i], y = (unsigned char) b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return false;
	}
	return true;
}

static void put_group (const char *line, size_t len, unsigned long count) {

	if (opt_dup && count < 2)
		return;
	if (opt_uniq && count > 1)
		return;
	if (opt_count)
		out_fmt("%7lu ", count);
	out_bytes(line, (int) len);
	out_char('\n');
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	HANDLE h;
	char *data;
	size_t len, at = 0;
	const char *cur = NULL;
	size_t cur_len = 0;
	unsigned long count = 0;

	bin_name = L"uniq";
	bin_usage = "usage: uniq [-c] [-d] [-u] [-i] [file]\n"
		"  Collapses equal lines that follow each other.\n"
		"  -c  count before each line   -d  only repeated lines   -u  only lines not repeated\n"
		"  -i  ignore upper/lower case\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'c': opt_count = true; break;
			case 'd': opt_dup = true; break;
			case 'u': opt_uniq = true; break;
			case 'i': opt_icase = true; break;
			default: opt_bad(ch);
		}
	}
	if (argc - o.i > 1) {
		err_print(L"extra operand '%ls'", argv[o.i + 1]);
		return 1;
	}

	h = in_open(o.i < argc ? argv[o.i] : L"-");
	if (h == INVALID_HANDLE_VALUE)
		return 1;
	if (!in_slurp(h, &data, &len)) {
		err_path(o.i < argc ? argv[o.i] : L"-", GetLastError());
		return 1;
	}
	in_close(h);

	while (at < len) {
		const char *nl = memchr(data + at, '\n', len - at);
		size_t l = nl ? (size_t) (nl - (data + at)) : len - at, key = l;

		if (key > 0 && data[at + key - 1] == '\r')
			key--;
		if (cur && same_line(cur, cur_len, data + at, key))
			count++;
		else {
			if (cur)
				put_group(cur, cur_len, count);
			cur = data + at;
			cur_len = key;
			count = 1;
		}
		at += l + 1;
	}
	if (cur)
		put_group(cur, cur_len, count);
	out_flush();
	return bin_status;
}
