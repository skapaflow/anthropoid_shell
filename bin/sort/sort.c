#include "../common.h"

/*
 * sort [-r] [-n] [-u] [-f] [file ...]
 * The lines of all the files (or of standard input) in order: bytes of the UTF-8 text by default,
 * -n by the number at the start of the line, -f ignoring upper/lower case (ASCII), -r reversed,
 * -u without repeated lines. Lines that compare equal keep the order they came in.
 */

typedef struct {
	const char *text;
	size_t len;   /* with the \r of a CRLF, which is not compared */
	size_t key;   /* len without that \r */
	size_t index;
} LINE;

static bool reverse, numeric, unique, fold;

static unsigned char fold_byte (unsigned char c) {

	return (fold && c >= 'A' && c <= 'Z') ? (unsigned char) (c + 32) : c;
}

static double leading_number (const LINE *l) {

	char tmp[64];
	size_t n = l->key < 63 ? l->key : 63;

	memcpy(tmp, l->text, n);
	tmp[n] = 0;
	return strtod(tmp, NULL);
}

static int key_compare (const LINE *x, const LINE *y) {

	if (numeric) {
		double a = leading_number(x), b = leading_number(y);
		return a < b ? -1 : a > b ? 1 : 0;
	}
	{
		size_t n = x->key < y->key ? x->key : y->key;
		for (size_t i = 0; i < n; i++) {
			unsigned char a = fold_byte((unsigned char) x->text[i]), b = fold_byte((unsigned char) y->text[i]);
			if (a != b)
				return a < b ? -1 : 1;
		}
		return x->key < y->key ? -1 : x->key > y->key ? 1 : 0;
	}
}

static int compare (const void *p, const void *q) {

	const LINE *x = p, *y = q;
	int r = key_compare(x, y);

	/* last resort: the whole line, unless -u (then the first one stays) */
	if (r == 0 && !unique) {
		size_t n = x->key < y->key ? x->key : y->key;
		r = memcmp(x->text, y->text, n);
		if (r == 0)
			r = x->key < y->key ? -1 : x->key > y->key ? 1 : 0;
	}
	if (reverse)
		r = -r;
	if (r == 0)
		r = x->index < y->index ? -1 : x->index > y->index ? 1 : 0;
	return r;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	LINE *lines = NULL;
	size_t n = 0, cap = 0;

	bin_name = L"sort";
	bin_usage = "usage: sort [-r] [-n] [-u] [-f] [file ...]\n"
		"  The lines of all the files (or of standard input), in order.\n"
		"  -n  by the number at the start of the line   -f  ignore upper/lower case\n"
		"  -r  reversed                                 -u  no repeated lines\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'r': reverse = true; break;
			case 'n': numeric = true; break;
			case 'u': unique = true; break;
			case 'f': fold = true; break;
			default: opt_bad(ch);
		}
	}

	for (int i = o.i; i < argc || (i == o.i && o.i >= argc); i++) {
		const wchar_t *name = i < argc ? argv[i] : L"-";
		HANDLE h = in_open(name);
		char *data;
		size_t len, at = 0;

		if (h == INVALID_HANDLE_VALUE)
			continue;
		if (!in_slurp(h, &data, &len)) {
			err_path(name, GetLastError());
			in_close(h);
			continue;
		}
		in_close(h);
		/* the buffers stay allocated: the lines point into them */
		while (at < len) {
			const char *nl = memchr(data + at, '\n', len - at);
			size_t l = nl ? (size_t) (nl - (data + at)) : len - at;

			if (n == cap) {
				LINE *bigger = realloc(lines, sizeof(LINE) * (cap = cap ? cap * 2 : 1024));
				if (!bigger)
					return 1;
				lines = bigger;
			}
			lines[n].text = data + at;
			lines[n].len = l;
			lines[n].key = (l > 0 && data[at + l - 1] == '\r') ? l - 1 : l;
			lines[n].index = n;
			n++;
			at += l + 1;
		}
		if (o.i >= argc)
			break;
	}

	if (n > 1)
		qsort(lines, n, sizeof(LINE), compare);
	for (size_t i = 0; i < n; i++) {
		if (unique && i > 0 && key_compare(&lines[i - 1], &lines[i]) == 0)
			continue;
		out_bytes(lines[i].text, (int) lines[i].len);
		out_char('\n');
	}
	out_flush();
	return bin_status;
}
