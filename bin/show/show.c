#include "../common.h"

/* show [-h] pid ...: shows the console window of a program started with 'bg' (it runs hidden); -h hides it again */

#define MAX_PIDS 64

/* the console window of the process with that pid (NULL if it has none); attaches to its console for a moment */
static HWND console_window_of (DWORD pid, DWORD *error) {

	HWND w;

	FreeConsole();
	if (!AttachConsole(pid)) {
		*error = GetLastError();
		return NULL;
	}
	w = GetConsoleWindow();
	FreeConsole();
	*error = w ? 0 : ERROR_NOT_FOUND;
	return w;
}

/* gets standard output and error back after FreeConsole: a console is reopened, a file or pipe was never lost */
static void restore_streams (const DWORD *ids, const bool *was_console) {

	for (int i = 0; i < 2; i++) {
		HANDLE h;

		if (!was_console[i])
			continue;
		h = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE)
			SetStdHandle(ids[i], h);
	}
}

int wmain (int argc, wchar_t **argv) {

	static const DWORD ids[2] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
	DWORD pid[MAX_PIDS], err[MAX_PIDS], mode;
	HWND win[MAX_PIDS];
	bool was_console[2], hide = false;
	int first = 1, n = 0;

	bin_name = L"show";
	bin_usage = "usage: show [-h] pid ...\n"
		"  Shows the console window of a program started with 'bg' (it runs hidden;\n"
		"  jobs lists the pids). -h hides it again.\n";

	if (argc == 2 && !wcscmp(argv[1], L"--help")) {
		out_str(bin_usage);
		out_flush();
		return 0;
	}
	if (argc > 1 && !wcscmp(argv[1], L"-h")) {
		hide = true;
		first = 2;
	}
	if (first >= argc) {
		err_print(L"missing operand");
		err_print(L"Try '%ls --help' for more information.", bin_name);
		return 1;
	}
	for (int i = first; i < argc && n < MAX_PIDS; i++) {
		wchar_t *end;
		unsigned long v = wcstoul(argv[i], &end, 10);

		if (end == argv[i] || *end || v == 0) {
			err_print(L"invalid pid '%ls'", argv[i]);
			return 1;
		}
		pid[n++] = (DWORD) v;
	}

	/* attaching to another console loses ours: finds every window first, then reports */
	for (int i = 0; i < 2; i++)
		was_console[i] = GetConsoleMode(GetStdHandle(ids[i]), &mode) != 0;
	for (int i = 0; i < n; i++)
		win[i] = console_window_of(pid[i], &err[i]);
	restore_streams(ids, was_console);
	out_handle = NULL;

	for (int i = 0; i < n; i++) {
		if (!win[i]) {
			if (err[i] == ERROR_NOT_FOUND || err[i] == ERROR_INVALID_HANDLE)
				err_print(L"%lu: no console window", pid[i]);
			else if (err[i] == ERROR_INVALID_PARAMETER)
				err_print(L"%lu: no such process", pid[i]);
			else
				err_print(L"%lu: cannot reach the process (error %lu)", pid[i], err[i]);
			bin_status = 1;
			continue;
		}
		ShowWindow(win[i], hide ? SW_HIDE : SW_SHOW);
		if (!hide) {
			ShowWindow(win[i], SW_RESTORE);
			SetForegroundWindow(win[i]);
		}
	}
	out_flush();
	return bin_status;
}
