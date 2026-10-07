#include "../common.h"

/* sleep time ...: waits; time is a number (it may have decimals) with an optional suffix s, m, h or d; several are added */

int wmain (int argc, wchar_t **argv) {

	double total = 0;

	bin_name = L"sleep";
	bin_usage = "usage: sleep time ...\n"
		"  Waits. time is a number (decimals allowed) with an optional suffix:\n"
		"  s seconds (default), m minutes, h hours, d days. Several times are added.\n";

	if (argc == 2 && !wcscmp(argv[1], L"--help")) {
		out_str(bin_usage);
		out_flush();
		return 0;
	}
	if (argc < 2) {
		err_print(L"missing operand");
		err_print(L"Try '%ls --help' for more information.", bin_name);
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		wchar_t *end;
		double v = wcstod(argv[i], &end), factor = 1;

		if (end == argv[i] || v < 0) {
			err_print(L"invalid time interval '%ls'", argv[i]);
			return 1;
		}
		if (*end) {
			switch (towlower(*end)) {
				case L's': factor = 1; break;
				case L'm': factor = 60; break;
				case L'h': factor = 3600; break;
				case L'd': factor = 86400; break;
				default: factor = -1;
			}
			if (factor < 0 || end[1]) {
				err_print(L"invalid time interval '%ls'", argv[i]);
				return 1;
			}
		}
		total += v * factor;
	}

	/* in pieces: Sleep takes 32 bits of milliseconds */
	for (double left = total * 1000.0; left > 0; ) {
		DWORD ms = left > 1e9 ? 1000000000UL : (DWORD) (left + 0.5);

		Sleep(ms);
		left -= ms;
	}
	return 0;
}
