#include "../common.h"

/*************************************************************
 * du [-a] [-s] [-h] [-c] [-d N] [path ...]                  *
 *                                                           *
 * How much space the files take: the sum of the sizes of    *
 * the files, in KiB (rounded up). For each folder, the      *
 * total of what is inside it.                               *
 *   -a  also one line per file       -s  only the total of   *
 *   -h  sizes like 1.5M                  each path           *
 *   -c  a grand total at the end     -d N  folders down to   *
 *                                          N levels (-s: 0)  *
 * Links to folders are not followed.                         *
 *************************************************************/

static bool opt_all, opt_human, opt_total;
static int max_depth = 1 << 30;

static void put_size (unsigned long long bytes) {

	if (opt_human) {
		static const char unit[] = "KMGTP";
		double v = (double) ((bytes + 1023) / 1024);
		int u = 0;

		if (bytes == 0) {
			out_str("0");
			return;
		}
		while (v > 1023.0 && u < 4) {
			v /= 1024.0;
			u++;
		}
		/* rounded up, as du does */
		if (v < 10.0 && u > 0)
			out_fmt("%.1f%c", (double) ((long long) (v * 10.0 + 0.999)) / 10.0, unit[u]);
		else
			out_fmt("%llu%c", (unsigned long long) (v + 0.999), unit[u]);
	} else
		out_fmt("%llu", (bytes + 1023) / 1024);
}

static void put_line (unsigned long long bytes, const wchar_t *path) {

	put_size(bytes);
	out_char('\t');
	out_w(path);
	out_char('\n');
}

/* size of a folder: what is inside it; prints a line for it (and for the files with -a) up to max_depth */
static unsigned long long walk_dir (const wchar_t *path, int depth) {

	wchar_t *pattern = path_join(path, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h;
	unsigned long long total = 0;

	if (!pattern)
		return 0;
	h = FindFirstFileW(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE) {
		err_print(L"cannot read directory '%ls': %ls", path, err_text(GetLastError()));
		bin_status = 1;
		return 0;
	}
	do {
		wchar_t *child;
		unsigned long long size;

		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		if (!(child = path_join(path, fd.cFileName)))
			continue;
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			size = walk_dir(child, depth + 1);
		else {
			size = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (((unsigned long long) fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
			if (opt_all && depth + 1 <= max_depth)
				put_line(size, child);
		}
		total += size;
		free(child);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (depth <= max_depth)
		put_line(total, path);
	return total;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	unsigned long long grand = 0;

	bin_name = L"du";
	bin_usage = "usage: du [-a] [-s] [-h] [-c] [-d N] [path ...]\n"
		"  The space the files take (sum of their sizes, in KiB), one line per folder.\n"
		"  -a  also one line per file     -s  only the total of each path\n"
		"  -h  sizes like 1.5M            -c  a grand total at the end\n"
		"  -d N  only folders down to N levels\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'a': opt_all = true; break;
			case 's': max_depth = 0; break;
			case 'h': opt_human = true; break;
			case 'c': opt_total = true; break;
			case 'k': break;
			case 'd': {
				const wchar_t *v = opt_arg(&o, ch);
				wchar_t *end;
				long n = wcstol(v, &end, 10);
				if (*end || n < 0) {
					err_print(L"invalid maximum depth '%ls'", v);
					return 1;
				}
				max_depth = (int) n;
				break;
			}
			default: opt_bad(ch);
		}
	}

	for (int i = o.i; i < argc || (i == o.i && o.i >= argc); i++) {
		const wchar_t *path = i < argc ? argv[i] : L".";
		WIN32_FILE_ATTRIBUTE_DATA d;
		unsigned long long size;

		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) {
			err_print(L"cannot access '%ls': %ls", path, err_text(GetLastError()));
			bin_status = 1;
			if (o.i >= argc)
				break;
			continue;
		}
		if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			size = walk_dir(path, 0);
		else {
			size = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (((unsigned long long) d.nFileSizeHigh << 32) | d.nFileSizeLow);
			put_line(size, path);
		}
		grand += size;
		if (o.i >= argc)
			break;
	}
	if (opt_total)
		put_line(grand, L"total");
	out_flush();
	return bin_status;
}
