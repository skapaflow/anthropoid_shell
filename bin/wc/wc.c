#include "../common.h"

/*
 * wc [-l] [-w] [-m] [-c] [file ...]: lines, words, characters (UTF-8) and bytes of each file
 * or of standard input; with several files, also the total. Without options: lines, words, bytes.
 */

typedef struct {
	unsigned long long lines, words, chars, bytes;
} COUNTS;

static bool show_lines, show_words, show_chars, show_bytes;
static int width;

static void count_handle (HANDLE h, const wchar_t *name, COUNTS *c) {

	static unsigned char buf[65536];
	bool in_word = false;
	int got;

	memset(c, 0, sizeof *c);
	while ((got = in_read(h, (char *) buf, sizeof buf)) > 0) {
		for (int i = 0; i < got; i++) {
			unsigned char b = buf[i];
			bool space = b == ' ' || (b >= 9 && b <= 13);

			if (b == '\n')
				c->lines++;
			if ((b & 0xC0) != 0x80)
				c->chars++;
			if (space)
				in_word = false;
			else if (!in_word) {
				in_word = true;
				c->words++;
			}
		}
		c->bytes += got;
	}
	if (got < 0)
		err_path(name, GetLastError());
}

static void put_number (unsigned long long v) {

	out_fmt(" %*llu", width, v);
}

static void put_counts (const COUNTS *c, const wchar_t *name) {

	bool first = true;
	char text[32];

	/* no padding for a single number alone on its line */
	if (width == 0) {
		snprintf(text, sizeof text, "%llu", show_lines ? c->lines : show_words ? c->words : show_chars ? c->chars : c->bytes);
		out_str(text);
		if (name) {
			out_char(' ');
			out_w(name);
		}
		out_char('\n');
		return;
	}
	if (show_lines) { put_number(c->lines); first = false; }
	if (show_words) { put_number(c->words); first = false; }
	if (show_chars) { put_number(c->chars); first = false; }
	if (show_bytes) { put_number(c->bytes); first = false; }
	(void) first;
	if (name) {
		out_char(' ');
		out_w(name);
	}
	out_char('\n');
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles, nshown;
	COUNTS total, c;

	bin_name = L"wc";
	bin_usage = "usage: wc [-l] [-w] [-m] [-c] [file ...]\n"
		"  Lines, words and bytes of each file (or of standard input).\n"
		"  -l lines  -w words  -m characters (UTF-8)  -c bytes\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'l': show_lines = true; break;
			case 'w': show_words = true; break;
			case 'm': show_chars = true; break;
			case 'c': show_bytes = true; break;
			default: opt_bad(ch);
		}
	}
	if (!show_lines && !show_words && !show_chars && !show_bytes)
		show_lines = show_words = show_bytes = true;
	nshown = show_lines + show_words + show_chars + show_bytes;
	nfiles = argc - o.i;
	width = (nshown == 1 && nfiles <= 1) ? 0 : 7;

	memset(&total, 0, sizeof total);
	if (nfiles == 0) {
		count_handle(GetStdHandle(STD_INPUT_HANDLE), L"-", &c);
		put_counts(&c, NULL);
	}
	for (int i = o.i; i < argc; i++) {
		HANDLE h = in_open(argv[i]);

		if (h == INVALID_HANDLE_VALUE)
			continue;
		count_handle(h, argv[i], &c);
		in_close(h);
		put_counts(&c, argv[i]);
		total.lines += c.lines;
		total.words += c.words;
		total.chars += c.chars;
		total.bytes += c.bytes;
	}
	if (nfiles > 1)
		put_counts(&total, L"total");
	out_flush();
	return bin_status;
}
