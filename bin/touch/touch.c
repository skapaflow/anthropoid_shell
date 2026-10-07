#include "../common.h"

/* touch [-c] file ...: sets the modification time to now; creates the file if it does not exist (-c: does not) */

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	bool no_create = false;

	bin_name = L"touch";
	bin_usage = "usage: touch [-c] file ...\n"
		"  Updates the modification time to now, creating an empty file if it does not exist.\n"
		"  -c  do not create\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch == 'c')
			no_create = true;
		else
			opt_bad(ch);
	}
	if (o.i >= argc) {
		err_print(L"missing file operand");
		err_print(L"Try '%ls --help' for more information.", bin_name);
		return 2;
	}

	for (int i = o.i; i < argc; i++) {
		FILETIME now;
		HANDLE h;

		if (!path_exists(argv[i]) && no_create)
			continue;
		/* OPEN_ALWAYS creates it empty; BACKUP_SEMANTICS lets a folder be opened too */
		h = CreateFileW(argv[i], FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			no_create ? OPEN_EXISTING : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			err_print(L"cannot touch '%ls': %ls", argv[i], err_text(GetLastError()));
			bin_status = 1;
			continue;
		}
		GetSystemTimeAsFileTime(&now);
		if (!SetFileTime(h, NULL, &now, &now)) {
			err_print(L"cannot touch '%ls': %ls", argv[i], err_text(GetLastError()));
			bin_status = 1;
		}
		CloseHandle(h);
	}
	return bin_status;
}
