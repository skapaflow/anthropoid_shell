#include "../common.h"

/* xargs [-0] [-n N] [-I str] [-P N] [-r] [-t] [command [arg ...]]: builds command lines from the standard input and runs them */

#define LINE_LIMIT 30000   /* the command line of CreateProcessW holds 32767 characters */

static wchar_t **items;
static int nitems = 0, items_cap = 0;
static wchar_t **base;     /* the command and its own arguments */
static int nbase = 0;
static bool trace = false;
static int max_procs = 1, nprocs = 0;
static HANDLE procs[MAXIMUM_WAIT_OBJECTS];
static HANDLE nul_in = INVALID_HANDLE_VALUE;
static int xstatus = 0;

static void add_item (const char *s, int n) {

	int w = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
	wchar_t *t;

	if (nitems == items_cap) {
		items_cap = items_cap ? items_cap * 2 : 256;
		items = realloc(items, items_cap * sizeof(wchar_t *));
		if (!items)
			ExitProcess(1);
	}
	t = malloc((w + 1) * sizeof(wchar_t));
	if (!t)
		ExitProcess(1);
	if (w > 0)
		MultiByteToWideChar(CP_UTF8, 0, s, n, t, w);
	t[w] = 0;
	items[nitems++] = t;
}

/* NUL-separated: no processing at all */
static void split_nul (const char *d, size_t len) {

	size_t i = 0, start = 0;

	for (; i < len; i++)
		if (!d[i]) {
			add_item(d + start, (int) (i - start));
			start = i + 1;
		}
	if (start < len)
		add_item(d + start, (int) (len - start));
}

/* one item per line, blanks around it trimmed, empty lines skipped (the -I mode) */
static void split_lines (const char *d, size_t len) {

	size_t i = 0;

	while (i < len) {
		size_t s = i, e;

		while (i < len && d[i] != '\n')
			i++;
		e = i;
		if (i < len)
			i++;
		while (s < e && (d[s] == ' ' || d[s] == '\t' || d[s] == '\r'))
			s++;
		while (e > s && (d[e - 1] == ' ' || d[e - 1] == '\t' || d[e - 1] == '\r'))
			e--;
		if (e > s)
			add_item(d + s, (int) (e - s));
	}
}

/* blanks and newlines separate; '...' and "..." group (the backslash is a path separator here, not an escape) */
static void split_words (const char *d, size_t len) {

	size_t i = 0;
	char *w = malloc(len + 1);

	if (!w)
		ExitProcess(1);
	while (i < len) {
		int n = 0;
		bool any = false;

		while (i < len && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n' || d[i] == '\r'))
			i++;
		if (i >= len)
			break;
		while (i < len && d[i] != ' ' && d[i] != '\t' && d[i] != '\n' && d[i] != '\r') {
			if (d[i] == '\'' || d[i] == '"') {
				char q = d[i++];

				any = true;
				while (i < len && d[i] != q && d[i] != '\n')
					w[n++] = d[i++];
				if (i >= len || d[i] != q) {
					err_print(L"unmatched %hs quote; by default quotes are special to xargs unless you use the -0 option", q == '"' ? "double" : "single");
					out_flush();
					ExitProcess(1);
				}
				i++;
			} else
				w[n++] = d[i++];
		}
		if (n > 0 || any)
			add_item(w, n);
	}
	free(w);
}

/* the argument as the Windows command line wants it (quoted when needed); out = NULL only measures */
static int quote_into (wchar_t *out, const wchar_t *a) {

	int n = 0;
	bool needs = !*a || wcspbrk(a, L" \t\n\v\"") != NULL;

#define PUT(c) do { if (out) out[n] = (c); n++; } while (0)
	if (!needs) {
		for (; *a; a++)
			PUT(*a);
		return n;
	}
	PUT(L'"');
	for (;;) {
		int bs = 0;

		while (*a == L'\\') {
			a++;
			bs++;
		}
		if (!*a) {
			for (int k = 0; k < bs * 2; k++)
				PUT(L'\\');
			break;
		}
		if (*a == L'"') {
			for (int k = 0; k < bs * 2 + 1; k++)
				PUT(L'\\');
		} else {
			for (int k = 0; k < bs; k++)
				PUT(L'\\');
		}
		PUT(*a);
		a++;
	}
	PUT(L'"');
#undef PUT
	return n;
}

static wchar_t *replace_all (const wchar_t *s, const wchar_t *what, const wchar_t *with) {

	size_t wl = wcslen(what), rl = wcslen(with), n = 0;
	wchar_t *out, *o;

	for (const wchar_t *p = s; (p = wcsstr(p, what)); p += wl)
		n++;
	out = malloc((wcslen(s) + n * (rl > wl ? rl - wl : 0) + 1) * sizeof(wchar_t));
	if (!out)
		ExitProcess(1);
	o = out;
	while (*s) {
		if (!wcsncmp(s, what, wl)) {
			wcscpy(o, with);
			o += rl;
			s += wl;
		} else
			*o++ = *s++;
	}
	*o = 0;
	return out;
}

/* a child ended: 0 is fine, 255 stops everything, anything else is remembered (123) */
static void note_exit (DWORD code) {

	if (code == 255) {
		err_print(L"%ls: exited with status 255; aborting", base[0]);
		out_flush();
		ExitProcess(124);
	}
	if (code != 0 && !xstatus)
		xstatus = 123;
}

static void reap_one (void) {

	DWORD r, code = 0;

	if (nprocs == 0)
		return;
	r = WaitForMultipleObjects(nprocs, procs, FALSE, INFINITE);
	if (r >= WAIT_OBJECT_0 + nprocs)
		return;
	r -= WAIT_OBJECT_0;
	GetExitCodeProcess(procs[r], &code);
	CloseHandle(procs[r]);
	procs[r] = procs[--nprocs];
	note_exit(code);
}

static void trace_line (const wchar_t *line) {

	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	DWORD mode, wrote;
	size_t n = wcslen(line);

	out_flush();
	if (GetConsoleMode(h, &mode)) {
		WriteConsoleW(h, line, (DWORD) n, &wrote, NULL);
		WriteConsoleW(h, L"\r\n", 2, &wrote, NULL);
	} else {
		char *u8 = malloc(n * 3 + 3);
		int k;

		if (!u8)
			return;
		k = WideCharToMultiByte(CP_UTF8, 0, line, (int) n, u8, (int) n * 3, NULL, NULL);
		u8[k++] = '\n';
		WriteFile(h, u8, k, &wrote, NULL);
		free(u8);
	}
}

/* runs one command line: args[0] is the program (or, without a command, the words that the built-in echo prints) */
static void run (wchar_t **args, int nargs) {

	static wchar_t *line;
	static size_t line_cap;
	size_t need = 1, len = 0;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE), he = GetStdHandle(STD_ERROR_HANDLE);

	for (int i = 0; i < nargs; i++)
		need += quote_into(NULL, args[i]) + 1;
	if (need > line_cap) {
		line_cap = need;
		line = realloc(line, line_cap * sizeof(wchar_t));
		if (!line)
			ExitProcess(1);
	}
	for (int i = 0; i < nargs; i++) {
		if (i)
			line[len++] = L' ';
		len += quote_into(line + len, args[i]);
	}
	line[len] = 0;
	if (trace)
		trace_line(line);

	if (nbase == 0) {
		/* no command: echo */
		for (int i = 0; i < nargs; i++) {
			if (i)
				out_char(' ');
			out_w(args[i]);
		}
		out_char('\n');
		return;
	}

	out_flush();
	while (nprocs >= max_procs)
		reap_one();
	if (nul_in == INVALID_HANDLE_VALUE) {
		SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

		nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	}
	if (ho && ho != INVALID_HANDLE_VALUE)
		SetHandleInformation(ho, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	if (he && he != INVALID_HANDLE_VALUE)
		SetHandleInformation(he, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = nul_in;
	si.hStdOutput = ho;
	si.hStdError = he;
	if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		DWORD e = GetLastError();

		err_print(L"%ls: %ls", args[0], err_text(e));
		out_flush();
		ExitProcess(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 127 : 126);
	}
	CloseHandle(pi.hThread);
	procs[nprocs++] = pi.hProcess;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, maxn = 0;
	bool nul = false, no_empty = false;
	const wchar_t *repl = NULL;
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	char *data;
	size_t len;

	bin_name = L"xargs";
	bin_usage = "usage: xargs [-0] [-n N] [-I str] [-P N] [-r] [-t] [command [arg ...]]\n"
		"  Reads words from the standard input and runs the command with them as\n"
		"  arguments. Without a command it prints the words, as echo does.\n"
		"  Blanks and newlines separate the words; '...' and \"...\" group them.\n"
		"  -0      the words are separated by NUL (use with find -print0)\n"
		"  -n N    at most N words per command line\n"
		"  -I str  one command per input line, with str replaced by the line\n"
		"  -P N    run up to N commands at the same time\n"
		"  -r      do not run the command when the input is empty\n"
		"  -t      show each command line on the standard error before running it\n"
		"  The command is a program (cmd.exe built-ins and shell aliases do not run).\n"
		"  Exit: 0 ok; 123 a command failed; 124 a command exited with 255;\n"
		"  126 could not run it; 127 not found.\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case '0': nul = true; break;
			case 'r': no_empty = true; break;
			case 't': trace = true; break;
			case 'n':
			case 'P': {
				const wchar_t *v = opt_arg(&o, ch), *e;
				long n = wcstol(v, (wchar_t **) &e, 10);

				if (!*v || *e || n < 1 || n > 1000000) {
					err_print(L"invalid number for -%lc: '%ls'", ch, v);
					return 2;
				}
				if (ch == 'n')
					maxn = (int) n;
				else
					max_procs = n > MAXIMUM_WAIT_OBJECTS ? MAXIMUM_WAIT_OBJECTS : (int) n;
				break;
			}
			case 'I':
				repl = opt_arg(&o, ch);
				if (!*repl) {
					err_print(L"the -I string cannot be empty");
					return 2;
				}
				break;
			default:
				opt_bad(ch);
		}
	}

	nbase = argc - o.i;
	base = argv + o.i;
	if (nbase == 0 && repl) {
		static wchar_t *only[1];

		only[0] = (wchar_t *) repl;
		base = only;
	}

	if (!in_slurp(in, &data, &len)) {
		err_print(L"cannot read the standard input");
		return 1;
	}
	if (nul)
		split_nul(data, len);
	else if (repl)
		split_lines(data, len);
	else
		split_words(data, len);
	free(data);

	if (repl) {
		int count = nbase ? nbase : 1;
		wchar_t **args = malloc(count * sizeof(wchar_t *));

		if (!args)
			return 1;
		for (int i = 0; i < nitems; i++) {
			for (int j = 0; j < count; j++)
				args[j] = replace_all(base[j], repl, items[i]);
			run(args, count);
			for (int j = 0; j < count; j++)
				free(args[j]);
		}
	} else if (nitems > 0 || !no_empty) {
		int i = 0;
		wchar_t **args = malloc((nbase + nitems + 1) * sizeof(wchar_t *));

		if (!args)
			return 1;
		do {
			size_t cur = 0;
			int count = nbase;

			for (int j = 0; j < nbase; j++) {
				args[j] = base[j];
				cur += quote_into(NULL, base[j]) + 1;
			}
			while (i < nitems && (maxn == 0 || count - nbase < maxn)) {
				size_t l = quote_into(NULL, items[i]) + 1;

				if (count > nbase && cur + l > LINE_LIMIT)
					break;
				cur += l;
				args[count++] = items[i++];
			}
			run(args, count);
		} while (i < nitems);
	}

	while (nprocs > 0)
		reap_one();
	out_flush();
	return xstatus ? xstatus : bin_status;
}
