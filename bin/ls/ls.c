#include "../common.h"

/*************************************************************
 * ls [-a] [-l] [-h] [-1] [file or folder ...]              *
 *                                                           *
 *   -a  also hidden names (.name and the hidden attribute)  *
 *   -l  attributes, size, date and name, one per line       *
 *   -h  sizes like 1.5K (with -l)                           *
 *   -1  one name per line                                   *
 *                                                           *
 * On a console the names come in columns, folders in blue,  *
 * programs in green and links in cyan; in a pipe or a file, *
 * one name per line, without colors.                        *
 *************************************************************/

typedef struct {
	wchar_t *name;
	DWORD attr;
	ULONGLONG size;
	FILETIME time;
} ENTRY;

static bool opt_all, opt_long, opt_human, opt_one;

static int compare (const void *a, const void *b) {

	const ENTRY *x = a, *y = b;
	int r = _wcsicmp(x->name, y->name);

	return r ? r : wcscmp(x->name, y->name);
}

static bool hidden (const ENTRY *e) {

	return e->name[0] == L'.' || (e->attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

static bool is_program (const wchar_t *name) {

	const wchar_t *dot = wcsrchr(name, L'.');

	return dot && (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".bat") || !_wcsicmp(dot, L".cmd") ||
		!_wcsicmp(dot, L".com") || !_wcsicmp(dot, L".ps1"));
}

static WORD entry_color (const ENTRY *e) {

	if (e->attr & FILE_ATTRIBUTE_REPARSE_POINT)
		return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	if (e->attr & FILE_ATTRIBUTE_DIRECTORY)
		return FOREGROUND_BLUE | FOREGROUND_INTENSITY;
	if (is_program(e->name))
		return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
	if (hidden(e))
		return FOREGROUND_INTENSITY;
	return 0;
}

static void put_name (const ENTRY *e) {

	WORD color = entry_color(e);

	if (color)
		out_color(color);
	out_w(e->name);
	if (color)
		out_color_off();
}

static void put_size (ULONGLONG size) {

	if (opt_human) {
		static const char unit[] = "BKMGTP";
		double v = (double) size;
		int u = 0;

		while (v >= 1024.0 && u < 5) {
			v /= 1024.0;
			u++;
		}
		if (u == 0)
			out_fmt("%6llu", size);
		else if (v < 10.0)
			out_fmt("%5.1f%c", v, unit[u]);
		else
			out_fmt("%5.0f%c", v, unit[u]);
	} else
		out_fmt("%12llu", size);
}

static void put_long (const ENTRY *e) {

	FILETIME local;
	SYSTEMTIME t;
	char mode[5];

	mode[0] = (e->attr & FILE_ATTRIBUTE_REPARSE_POINT) ? 'l' : (e->attr & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : '-';
	mode[1] = (e->attr & FILE_ATTRIBUTE_READONLY) ? 'r' : '-';
	mode[2] = (e->attr & FILE_ATTRIBUTE_HIDDEN) ? 'h' : '-';
	mode[3] = (e->attr & FILE_ATTRIBUTE_SYSTEM) ? 's' : '-';
	mode[4] = 0;
	FileTimeToLocalFileTime(&e->time, &local);
	FileTimeToSystemTime(&local, &t);
	out_str(mode);
	out_char(' ');
	put_size((e->attr & FILE_ATTRIBUTE_DIRECTORY) ? 0 : e->size);
	out_fmt(" %04d-%02d-%02d %02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
	put_name(e);
	out_char('\n');
}

/* names in columns filled top to bottom, like ls; one per line when it is not a console */
static void put_columns (ENTRY *e, int n) {

	CONSOLE_SCREEN_BUFFER_INFO info;
	int width = 80, cols = 1, rows = n, *w, *colw;

	out_init();
	if (!out_console || opt_one || opt_long) {
		for (int i = 0; i < n; i++) {
			if (opt_long)
				put_long(&e[i]);
			else {
				put_name(&e[i]);
				out_char('\n');
			}
		}
		return;
	}
	if (GetConsoleScreenBufferInfo(out_handle, &info))
		width = info.srWindow.Right - info.srWindow.Left + 1;

	w = malloc(sizeof(int) * n);
	colw = malloc(sizeof(int) * (n + 1));
	if (!w || !colw) {
		free(w);
		free(colw);
		return;
	}
	for (int i = 0; i < n; i++)
		w[i] = text_width(e[i].name);

	for (int c = n; c >= 1; c--) {
		int r = (n + c - 1) / c, total = 2 * (c - 1);
		for (int k = 0; k < c; k++) {
			colw[k] = 0;
			for (int i = k * r; i < (k + 1) * r && i < n; i++)
				if (w[i] > colw[k])
					colw[k] = w[i];
			total += colw[k];
		}
		cols = c;
		rows = r;
		if (total < width)
			break;
	}
	/* a column with nothing left (n = 5 in 4 columns of 2 rows leaves the 4th empty) still counts as a column */
	for (int r = 0; r < rows; r++) {
		for (int k = 0; k < cols; k++) {
			int i = k * rows + r;
			if (i >= n)
				break;
			put_name(&e[i]);
			/* pad unless it is the last one of the row */
			if ((k + 1) * rows + r < n)
				for (int s = w[i]; s < colw[k] + 2; s++)
					out_char(' ');
		}
		out_char('\n');
	}
	free(w);
	free(colw);
}

static bool entry_from_find (ENTRY *e, const WIN32_FIND_DATAW *fd) {

	e->name = _wcsdup(fd->cFileName);
	e->attr = fd->dwFileAttributes;
	e->size = ((ULONGLONG) fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
	e->time = fd->ftLastWriteTime;
	return e->name != NULL;
}

/* lists one folder */
static void list_dir (const wchar_t *path) {

	wchar_t *pattern = path_join(path, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h;
	ENTRY *list = NULL;
	int n = 0, cap = 0;

	if (!pattern)
		return;
	h = FindFirstFileW(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE) {
		err_print(L"cannot access '%ls': %ls", path, err_text(GetLastError()));
		bin_status = 2;
		return;
	}
	do {
		ENTRY e;
		if (!entry_from_find(&e, &fd))
			break;
		if (!opt_all && hidden(&e)) {
			free(e.name);
			continue;
		}
		if (n == cap) {
			ENTRY *bigger = realloc(list, sizeof(ENTRY) * (cap = cap ? cap * 2 : 64));
			if (!bigger)
				break;
			list = bigger;
		}
		list[n++] = e;
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	if (n > 1)
		qsort(list, n, sizeof(ENTRY), compare);
	put_columns(list, n);
	for (int i = 0; i < n; i++)
		free(list[i].name);
	free(list);
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles = 0, nops;
	ENTRY *files;
	wchar_t **dirs;
	int ndirs = 0;

	bin_name = L"ls";
	bin_usage = "usage: ls [-a] [-l] [-h] [-1] [file or folder ...]\n"
		"  -a  also hidden names (.name, hidden and system files)\n"
		"  -l  attributes (d folder, l link, r read-only, h hidden, s system), size, date, name\n"
		"  -h  sizes like 1.5K (with -l)\n"
		"  -1  one name per line (always in a pipe or a file)\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'a': opt_all = true; break;
			case 'l': opt_long = true; break;
			case 'h': opt_human = true; break;
			case '1': opt_one = true; break;
			default: opt_bad(ch);
		}
	}

	nops = argc - o.i;
	if (nops == 0) {
		list_dir(L".");
		out_flush();
		return bin_status;
	}

	/* files first, then the folders, as ls does */
	files = calloc(nops, sizeof(ENTRY));
	dirs = calloc(nops, sizeof(wchar_t *));
	if (!files || !dirs)
		return 1;
	for (int i = o.i; i < argc; i++) {
		WIN32_FILE_ATTRIBUTE_DATA d;

		if (!GetFileAttributesExW(argv[i], GetFileExInfoStandard, &d)) {
			err_print(L"cannot access '%ls': %ls", argv[i], err_text(GetLastError()));
			bin_status = 2;
			continue;
		}
		if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			dirs[ndirs++] = argv[i];
		else {
			files[nfiles].name = argv[i];
			files[nfiles].attr = d.dwFileAttributes;
			files[nfiles].size = ((ULONGLONG) d.nFileSizeHigh << 32) | d.nFileSizeLow;
			files[nfiles].time = d.ftLastWriteTime;
			nfiles++;
		}
	}
	if (nfiles > 1)
		qsort(files, nfiles, sizeof(ENTRY), compare);
	if (nfiles > 0)
		put_columns(files, nfiles);
	for (int i = 0; i < ndirs; i++) {
		if (nfiles > 0 || i > 0 || nops > 1) {
			if (nfiles > 0 || i > 0)
				out_char('\n');
			out_w(dirs[i]);
			out_str(":\n");
		}
		list_dir(dirs[i]);
	}
	out_flush();
	return bin_status;
}
