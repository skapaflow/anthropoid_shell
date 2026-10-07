#include "../common.h"

/*************************************************************
 * kill [-s SIG | -SIG | -0] pid ...                         *
 * kill -l [n]                                               *
 *                                                           *
 * Ends the processes. Windows has no signals, so every      *
 * signal does the same (TerminateProcess) and the process   *
 * ends with the code a shell reports for it: 128 + the      *
 * signal number. -0 only tests whether the process exists.  *
 * Jobs of the shell (kill %1) are handled by the shell.     *
 *************************************************************/

static const struct { const wchar_t *name; int number; } signals[] = {
	{ L"HUP", 1 }, { L"INT", 2 }, { L"QUIT", 3 }, { L"ABRT", 6 }, { L"KILL", 9 }, { L"USR1", 10 },
	{ L"USR2", 12 }, { L"PIPE", 13 }, { L"ALRM", 14 }, { L"TERM", 15 }
};
#define NSIGNALS ((int) (sizeof signals / sizeof *signals))

static bool all_digits (const wchar_t *w) {

	if (!*w)
		return false;
	for (; *w; w++)
		if (*w < L'0' || *w > L'9')
			return false;
	return true;
}

/* signal number from a name (HUP, SIGHUP, hup) or a number; -1 if unknown */
static int signal_number (const wchar_t *name) {

	if (all_digits(name))
		return _wtoi(name) <= 64 ? _wtoi(name) : -1;
	if (!_wcsnicmp(name, L"SIG", 3))
		name += 3;
	for (int i = 0; i < NSIGNALS; i++)
		if (!_wcsicmp(name, signals[i].name))
			return signals[i].number;
	return -1;
}

static void list_signals (const wchar_t *arg) {

	if (arg) {
		int n = signal_number(arg);

		for (int i = 0; i < NSIGNALS; i++)
			if (signals[i].number == n) {
				out_w(signals[i].name);
				out_char('\n');
				out_flush();
				return;
			}
		err_print(L"%ls: invalid signal specification", arg);
		out_flush();
		ExitProcess(1);
	}
	for (int i = 0; i < NSIGNALS; i++) {
		if (i)
			out_char(' ');
		out_w(signals[i].name);
	}
	out_char('\n');
}

int wmain (int argc, wchar_t **argv) {

	int i = 1, number = 15;

	bin_name = L"kill";
	bin_usage = "usage: kill [-s SIG | -SIG | -0] pid ...\n"
		"       kill -l [n]\n"
		"  Ends the processes. There are no signals on Windows: every one ends the process,\n"
		"  with the code 128 + the number of the signal (KILL 9, TERM 15, the default).\n"
		"  -0  only tests that the process exists   -l  lists the signal names\n"
		"  Jobs of the shell: kill %1 (the shell does it).\n";

	for (; i < argc && argv[i][0] == L'-' && argv[i][1]; i++) {
		const wchar_t *opt = argv[i] + 1;

		if (!wcscmp(argv[i], L"--")) {
			i++;
			break;
		}
		if (!wcscmp(argv[i], L"--help")) {
			out_str(bin_usage);
			out_flush();
			return 0;
		}
		if (!wcscmp(opt, L"l") || !wcscmp(opt, L"L")) {
			list_signals(i + 1 < argc ? argv[i + 1] : NULL);
			out_flush();
			return 0;
		}
		if (!wcscmp(opt, L"s") || !wcscmp(opt, L"n")) {
			if (++i >= argc) {
				err_print(L"option requires an argument -- '%ls'", opt);
				return 2;
			}
			opt = argv[i];
		}
		if ((number = signal_number(opt)) < 0) {
			err_print(L"%ls: invalid signal specification", opt);
			return 2;
		}
	}
	if (i >= argc) {
		err_print(L"usage: kill [-s SIG | -SIG | -0] pid ...");
		return 2;
	}

	for (; i < argc; i++) {
		DWORD pid, exit_code;
		HANDLE h;

		if (!all_digits(argv[i]) || (pid = (DWORD) _wtol(argv[i])) == 0) {
			err_print(L"%ls: arguments must be process IDs", argv[i]);
			bin_status = 1;
			continue;
		}
		h = OpenProcess(number == 0 ? PROCESS_QUERY_LIMITED_INFORMATION : PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!h) {
			err_print(L"(%lu) - %ls", pid, GetLastError() == ERROR_ACCESS_DENIED ? L"Operation not permitted" : L"No such process");
			bin_status = 1;
			continue;
		}
		if (number == 0) {
			/* a finished process whose handle is still around does not count */
			if (!GetExitCodeProcess(h, &exit_code) || exit_code != STILL_ACTIVE) {
				err_print(L"(%lu) - No such process", pid);
				bin_status = 1;
			}
		} else if (!TerminateProcess(h, (UINT) (128 + number))) {
			err_print(L"(%lu) - %ls", pid, GetLastError() == ERROR_ACCESS_DENIED ? L"Operation not permitted" : L"No such process");
			bin_status = 1;
		}
		CloseHandle(h);
	}
	return bin_status;
}
