#include "../common.h"

/*************************************************************
 * core [--help]                                             *
 *                                                           *
 * Memory and disk usage: one bar per item.                  *
 *   RAM  bar 76%   12.1 GB / 16.0 GB                        *
 *   C:   bar 90%   62.8 GB / 69.9 GB                        *
 * Drives are the fixed, removable and network ones that are *
 * ready (CD drives and empty readers are left out).         *
 * The bar is green up to 70%, yellow up to 90%, red above.  *
 *************************************************************/

#define BAR_CELLS 30

static void put_size (double bytes) {

	static const char *unit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
	char text[32];
	int u = 0;

	while (bytes >= 1024.0 && u < 5) {
		bytes /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(text, sizeof text, "%.0f %s", bytes, unit[u]);
	else
		snprintf(text, sizeof text, "%.1f %s", bytes, unit[u]);
	out_fmt("%9s", text);
}

/* label, bar, percentage and "used / total" in one line */
static void put_line (const char *label, unsigned long long used, unsigned long long total) {

	int pct = total ? (int) ((used * 100 + total / 2) / total) : 0;
	int full = (pct * BAR_CELLS + 50) / 100;
	WORD color = pct < 70 ? FOREGROUND_GREEN : (pct < 90 ? FOREGROUND_GREEN | FOREGROUND_RED : FOREGROUND_RED);

	out_fmt(" %-4s ", label);
	out_color(color | FOREGROUND_INTENSITY);
	for (int i = 0; i < BAR_CELLS; i++)
		out_str(i < full ? "\xE2\x96\x88" : "\xE2\x96\x91");	/* █ ░ */
	out_color_off();
	out_fmt(" %3d%% ", pct);
	put_size((double) used);
	out_str(" / ");
	put_size((double) total);
	out_char('\n');
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	MEMORYSTATUSEX mem = {sizeof mem};
	DWORD drives;
	int count = 0, ch;

	bin_name = L"core";
	bin_usage =
		"Usage: core\n"
		"Show the memory in use and the space used on each drive.\n";
	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1)
		opt_bad(ch);
	if (o.i < argc) {
		err_print(L"extra operand '%ls'", argv[o.i]);
		out_flush();
		return 2;
	}

	/* no "insert a disk" dialog for an empty reader */
	SetErrorMode(SEM_FAILCRITICALERRORS);

	out_char('\n');
	if (GlobalMemoryStatusEx(&mem))
		put_line("RAM", mem.ullTotalPhys - mem.ullAvailPhys, mem.ullTotalPhys);

	drives = GetLogicalDrives();
	for (int i = 0; i < 26; i++) {
		wchar_t root[] = {L'A' + i, L':', L'\\', 0};
		char label[4] = {(char) ('A' + i), ':', 0};
		ULARGE_INTEGER avail, total, free_total;
		UINT type;

		if (!(drives & (1u << i)))
			continue;
		type = GetDriveTypeW(root);
		if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE)
			continue;
		if (!GetDiskFreeSpaceExW(root, &avail, &total, &free_total))
			continue;
		if (count == 0)
			out_char('\n');
		put_line(label, total.QuadPart - free_total.QuadPart, total.QuadPart);
		count++;
	}

	out_fmt("\n %d drive%s\n", count, count == 1 ? "" : "s");
	out_flush();
	return 0;
}
