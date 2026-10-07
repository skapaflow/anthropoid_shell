#include "../common.h"

/*************************************************************
 * grep [-i] [-n] [-v] [-c] [-l] [-q] [-r] [-F] [-h] [-H]   *
 *      [-e pattern ...] pattern [file ...]                  *
 *                                                           *
 * Lines that match a pattern. The patterns are a small      *
 * regular expression (the text is UTF-8):                   *
 *   c      a character          .     any character        *
 *   [abc] [a-z] [^0-9] [[:alpha:]]   a set                  *
 *   * + ?  zero or more / one or more / zero or one of the  *
 *          previous one                                     *
 *   ^ $    start / end of the line                          *
 *   a|b    either one                                       *
 *   \c     the character c as it is; \w \d \s (and \W \D \S) *
 * -F reads the patterns as plain text. -i ignores upper and *
 * lower case of ASCII letters.                              *
 *************************************************************/

enum { A_LIT, A_ANY, A_SET, A_BOL, A_EOL };

typedef struct {
	unsigned char kind;
	char q;               /* 0, '*', '+' or '?' */
	bool neg;             /* set: [^...] */
	unsigned char len;    /* literal: bytes of the character */
	unsigned char c[4];
	unsigned char set[32]; /* set: a bit per ASCII code */
} RE_ATOM;

#define MAXATOMS 512

typedef struct {
	RE_ATOM atoms[MAXATOMS];
	int n;
} REGEX;

static REGEX *alts;
static int nalts;
static bool icase;

static bool opt_number, opt_invert, opt_count, opt_list, opt_quiet, opt_recursive, opt_fixed;
static bool show_name, hide_name;

/* ---------- compiling ---------- */

static void set_add (RE_ATOM *a, int c) {

	a->set[c >> 3] |= (unsigned char) (1 << (c & 7));
	if (icase && c < 128 && isalpha(c)) {
		int o = c ^ 0x20;
		a->set[o >> 3] |= (unsigned char) (1 << (o & 7));
	}
}

static void set_class (RE_ATOM *a, int kind, bool negate) {

	for (int c = 0; c < 128; c++) {
		bool in = false;
		switch (kind) {
			case 'w': in = isalnum(c) || c == '_'; break;
			case 'd': in = isdigit(c); break;
			case 's': in = c == ' ' || (c >= 9 && c <= 13); break;
		}
		if (in != negate)
			set_add(a, c);
	}
}

static int utf8_len (unsigned char c) {

	return c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
}

/* [:name:] at p (p points after the '['): the predicate, or NULL */
static bool posix_class (RE_ATOM *a, const char **pp) {

	static const struct { const char *name; int (*test) (int); } classes[] = {
		{ "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "space", isspace },
		{ "upper", isupper }, { "lower", islower }, { "punct", ispunct }, { "xdigit", isxdigit }
	};
	const char *p = *pp;

	if (p[0] != ':')
		return false;
	for (int i = 0; i < (int) (sizeof classes / sizeof *classes); i++) {
		size_t n = strlen(classes[i].name);
		if (!strncmp(p + 1, classes[i].name, n) && p[1 + n] == ':' && p[2 + n] == ']') {
			for (int c = 0; c < 128; c++)
				if (classes[i].test(c))
					set_add(a, c);
			*pp = p + 3 + n;
			return true;
		}
	}
	return false;
}

/* one alternative of the pattern, pattern[0..len) */
static bool compile (REGEX *r, const char *p, int len, bool fixed) {

	const char *end = p + len;

	r->n = 0;
	while (p < end) {
		RE_ATOM *a;

		if (r->n >= MAXATOMS - 1)
			return false;
		a = &r->atoms[r->n];
		memset(a, 0, sizeof *a);

		if (fixed) {
			a->kind = A_LIT;
			a->len = (unsigned char) utf8_len((unsigned char) *p);
			if (p + a->len > end)
				a->len = (unsigned char) (end - p);
			memcpy(a->c, p, a->len);
			p += a->len;
			r->n++;
			continue;
		}

		if (*p == '^' && r->n == 0) {
			a->kind = A_BOL;
			p++;
		} else if (*p == '$' && p + 1 == end) {
			a->kind = A_EOL;
			p++;
		} else if (*p == '.') {
			a->kind = A_ANY;
			p++;
		} else if (*p == '[') {
			const char *q = p + 1;
			bool first = true;

			a->kind = A_SET;
			if (*q == '^') {
				a->neg = true;
				q++;
			}
			for (;; first = false) {
				int lo;
				if (q >= end)
					return false; /* no closing ] */
				if (*q == ']' && !first)
					break;
				if (*q == '[' && q + 1 < end && q[1] == ':') {
					const char *after = q + 1;
					if (posix_class(a, &after)) {
						q = after;
						continue;
					}
				}
				lo = (unsigned char) *q++;
				if (lo >= 128)
					continue; /* a non-ASCII character in a set: not supported, it is skipped */
				if (q + 1 < end && *q == '-' && q[1] != ']') {
					int hi = (unsigned char) q[1];
					if (hi < 128) {
						for (int c = lo; c <= hi; c++)
							set_add(a, c);
						q += 2;
						continue;
					}
				}
				set_add(a, lo);
			}
			p = q + 1;
		} else if (*p == '\\' && p + 1 < end) {
			char e = p[1];
			if (e == 'w' || e == 'd' || e == 's') {
				a->kind = A_SET;
				set_class(a, e, false);
				p += 2;
			} else if (e == 'W' || e == 'D' || e == 'S') {
				a->kind = A_SET;
				set_class(a, e + 32, true);
				a->neg = false;
				p += 2;
			} else {
				a->kind = A_LIT;
				a->len = (unsigned char) utf8_len((unsigned char) e);
				if (p + 1 + a->len > end)
					a->len = (unsigned char) (end - (p + 1));
				memcpy(a->c, p + 1, a->len);
				p += 1 + a->len;
			}
		} else if ((*p == '*' || *p == '+' || *p == '?') && r->n > 0 && r->atoms[r->n - 1].q == 0 &&
			r->atoms[r->n - 1].kind != A_BOL && r->atoms[r->n - 1].kind != A_EOL) {
			r->atoms[r->n - 1].q = *p++;
			continue;
		} else {
			a->kind = A_LIT;
			a->len = (unsigned char) utf8_len((unsigned char) *p);
			if (p + a->len > end)
				a->len = (unsigned char) (end - p);
			memcpy(a->c, p, a->len);
			p += a->len;
		}
		r->n++;
	}
	return true;
}

/* the pattern split at '|' (outside [ ]) in alternatives */
static bool add_pattern (const char *pat, int len) {

	int start = 0;
	bool in_set = false;

	for (int i = 0; i <= len; i++) {
		if (i < len && pat[i] == '\\' && !opt_fixed) {
			i++;
			continue;
		}
		if (i < len && !opt_fixed) {
			if (pat[i] == '[' && !in_set)
				in_set = true;
			else if (pat[i] == ']' && in_set && i > 0 && pat[i - 1] != '[' && pat[i - 1] != '^')
				in_set = false;
		}
		if (i == len || (pat[i] == '|' && !in_set && !opt_fixed)) {
			REGEX *bigger = realloc(alts, sizeof(REGEX) * (nalts + 1));
			if (!bigger)
				return false;
			alts = bigger;
			if (!compile(&alts[nalts], pat + start, i - start, opt_fixed))
				return false;
			nalts++;
			start = i + 1;
		}
	}
	return true;
}

/* ---------- matching ---------- */

/* one occurrence of the atom at t: where it ends, or NULL */
static const char *single (const RE_ATOM *x, const char *t, const char *end) {

	if (t >= end)
		return NULL;
	switch (x->kind) {
		case A_LIT:
			if (t + x->len > end)
				return NULL;
			if (x->len == 1 && icase) {
				if (tolower((unsigned char) *t) != tolower(x->c[0]))
					return NULL;
			} else if (memcmp(t, x->c, x->len))
				return NULL;
			return t + x->len;
		case A_ANY: {
			int k = utf8_len((unsigned char) *t);
			return t + k <= end ? t + k : end;
		}
		case A_SET: {
			unsigned char c = (unsigned char) *t;
			int k = utf8_len(c);
			bool in;
			if (c >= 128)
				in = false;
			else
				in = (x->set[c >> 3] >> (c & 7)) & 1;
			if (in == x->neg)
				return NULL;
			return t + k <= end ? t + k : end;
		}
	}
	return NULL;
}

static const char *match_here (const RE_ATOM *a, int i, int n, const char *t, const char *end, const char *start) {

	const RE_ATOM *x;

	if (i == n)
		return t;
	x = &a[i];
	if (x->kind == A_BOL)
		return t == start ? match_here(a, i + 1, n, t, end, start) : NULL;
	if (x->kind == A_EOL)
		return t == end ? match_here(a, i + 1, n, t, end, start) : NULL;

	if (x->q == 0) {
		const char *r = single(x, t, end);
		return r ? match_here(a, i + 1, n, r, end, start) : NULL;
	}
	if (x->q == '?') {
		const char *r = single(x, t, end), *res;
		if (r && (res = match_here(a, i + 1, n, r, end, start)))
			return res;
		return match_here(a, i + 1, n, t, end, start);
	}
	{
		/* * and +: takes as many as it can and gives back one at a time */
		size_t cap = (size_t) (end - t) + 1;
		const char **stack = malloc(sizeof(char *) * cap), *p = t, *res = NULL;
		size_t k = 0;
		int min = x->q == '+' ? 1 : 0;

		if (!stack)
			return NULL;
		stack[0] = t;
		while (k + 1 < cap) {
			const char *r = single(x, p, end);
			if (!r || r == p)
				break;
			stack[++k] = r;
			p = r;
		}
		for (long j = (long) k; j >= min && !res; j--)
			res = match_here(a, i + 1, n, stack[j], end, start);
		free(stack);
		return res;
	}
}

/* does the line [line, end) have a match? */
static bool line_matches (const char *line, const char *end) {

	for (int k = 0; k < nalts; k++) {
		const REGEX *r = &alts[k];
		bool anchored = r->n > 0 && r->atoms[0].kind == A_BOL;

		for (const char *t = line; ; ) {
			if (match_here(r->atoms, 0, r->n, t, end, line))
				return true;
			if (anchored || t >= end)
				break;
			t += utf8_len((unsigned char) *t);
			if (t > end)
				t = end;
		}
	}
	return false;
}

/* ---------- files ---------- */

static bool matched_any = false;

/* greps a buffer; name is for the output */
static void grep_data (const char *data, size_t len, const wchar_t *name) {

	unsigned long long nmatch = 0, line_no = 0;
	size_t at = 0;
	bool binary = memchr(data, 0, len < 8192 ? len : 8192) != NULL;

	while (at < len) {
		const char *nl = memchr(data + at, '\n', len - at);
		size_t raw = nl ? (size_t) (nl - (data + at)) : len - at, n = raw;
		bool hit;

		if (n > 0 && data[at + n - 1] == '\r')
			n--;
		line_no++;
		hit = line_matches(data + at, data + at + n);
		if (opt_invert)
			hit = !hit;
		if (hit) {
			nmatch++;
			matched_any = true;
			if (opt_quiet) {
				out_flush();
				ExitProcess(0);
			}
			if (opt_list)
				break;
			if (binary && !opt_count) {
				out_str("Binary file ");
				out_w(name);
				out_str(" matches\n");
				return;
			}
			if (!opt_count) {
				if (show_name) {
					out_w(name);
					out_char(':');
				}
				if (opt_number)
					out_fmt("%llu:", line_no);
				out_bytes(data + at, (int) raw);
				out_char('\n');
			}
		}
		at += raw + 1;
	}
	if (opt_list) {
		if (nmatch > 0) {
			out_w(name);
			out_char('\n');
		}
	} else if (opt_count) {
		if (show_name) {
			out_w(name);
			out_char(':');
		}
		out_fmt("%llu\n", nmatch);
	}
}

static void grep_path (const wchar_t *path, bool from_walk);

static void grep_dir (const wchar_t *path) {

	wchar_t *pattern = path_join(path, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h;

	if (!pattern)
		return;
	h = FindFirstFileW(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		wchar_t *child;
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		/* links to folders are not followed (no loops) */
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			continue;
		if ((child = path_join(path, fd.cFileName)))
			grep_path(child, true);
		free(child);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static void grep_path (const wchar_t *path, bool from_walk) {

	HANDLE h;
	char *data;
	size_t len;
	const wchar_t *shown = path;

	/* "grep -r x" with no folder searches "." and shows the names without the ".\" */
	if (from_walk && path[0] == L'.' && is_sep(path[1]))
		shown = path + 2;

	if (wcscmp(path, L"-") && path_is_dir(path)) {
		if (opt_recursive)
			grep_dir(path);
		else
			err_path_text(path, L"Is a directory");
		return;
	}
	h = in_open(path);
	if (h == INVALID_HANDLE_VALUE)
		return;
	if (!in_slurp(h, &data, &len))
		err_path(path, GetLastError());
	else {
		grep_data(data, len, wcscmp(path, L"-") ? shown : L"(standard input)");
		free(data);
	}
	in_close(h);
}

static char *to_utf8 (const wchar_t *w, int *len) {

	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = malloc(n > 0 ? n : 1);

	if (!s)
		return NULL;
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	*len = n > 0 ? n - 1 : 0;
	return s;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles;
	wchar_t **patterns = calloc(argc, sizeof(wchar_t *));
	int npat = 0;

	bin_name = L"grep";
	bin_usage = "usage: grep [-i] [-n] [-v] [-c] [-l] [-q] [-r] [-F] [-h] [-H] [-e pattern] pattern [file ...]\n"
		"  Writes the lines that match. No file or \"-\": standard input.\n"
		"  -i ignore upper/lower case   -n line numbers     -v the lines that do not match\n"
		"  -c count the lines           -l only the names    -q write nothing, exit 0 if it matched\n"
		"  -r search folders            -F plain text        -h no names   -H always names\n"
		"  -e pattern (repeat it for several)\n"
		"  Pattern: c  .  [abc] [a-z] [^0-9] [[:alpha:]]  * + ?  ^ $  a|b  \\c  \\w \\d \\s (and \\W \\D \\S)\n"
		"  Exit code: 0 matched, 1 did not, 2 error.\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'i': icase = true; break;
			case 'n': opt_number = true; break;
			case 'v': opt_invert = true; break;
			case 'c': opt_count = true; break;
			case 'l': opt_list = true; break;
			case 'q': opt_quiet = true; break;
			case 'r': case 'R': opt_recursive = true; break;
			case 'F': opt_fixed = true; break;
			case 'E': case 'G': break; /* the patterns are already extended */
			case 'h': hide_name = true; break;
			case 'H': show_name = true; break;
			case 'e': patterns[npat++] = (wchar_t *) opt_arg(&o, ch); break;
			default: opt_bad(ch);
		}
	}
	if (npat == 0) {
		if (o.i >= argc) {
			err_print(L"missing pattern");
			err_print(L"Try '%ls --help' for more information.", bin_name);
			return 2;
		}
		patterns[npat++] = argv[o.i++];
	}
	for (int i = 0; i < npat; i++) {
		int len;
		char *u8 = to_utf8(patterns[i], &len);

		if (!u8 || !add_pattern(u8, len)) {
			err_print(L"invalid pattern: %ls", patterns[i]);
			return 2;
		}
		free(u8);
	}

	nfiles = argc - o.i;
	if (!hide_name && (nfiles > 1 || opt_recursive))
		show_name = true;
	if (hide_name)
		show_name = false;

	if (nfiles == 0) {
		if (opt_recursive)
			grep_path(L".", true);
		else
			grep_path(L"-", false);
	}
	for (int i = o.i; i < argc; i++)
		grep_path(argv[i], false);
	out_flush();
	return bin_status ? 2 : (matched_any ? 0 : 1);
}
