#include "../common.h"

/* tee [-a] [file ...]: copies the standard input to the standard output and to each file (-a: appends instead of replacing) */

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nfiles = 0, got;
	bool append = false;
	HANDLE *files;
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	static char buf[65536];

	bin_name = L"tee";
	bin_usage = "usage: tee [-a] [file ...]\n"
		"  Copies the standard input to the standard output and to each file.\n"
		"  -a  append to the files instead of replacing them\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch == 'a')
			append = true;
		else if (ch != 'i')
			opt_bad(ch);
	}

	files = calloc(argc, sizeof(HANDLE));
	if (!files)
		return 1;
	for (int i = o.i; i < argc; i++) {
		HANDLE h;

		if (append)
			h = CreateFileW(argv[i], FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		else
			h = CreateFileW(argv[i], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			err_path(argv[i], GetLastError());
			continue;
		}
		files[nfiles++] = h;
	}

	while ((got = in_read(in, buf, sizeof buf)) > 0) {
		out_bytes(buf, got);
		/* flushed at each piece: tee is often at the middle of a pipe that someone is reading live */
		out_flush();
		for (int i = 0; i < nfiles; i++) {
			DWORD wrote;
			WriteFile(files[i], buf, got, &wrote, NULL);
		}
	}
	for (int i = 0; i < nfiles; i++)
		CloseHandle(files[i]);
	out_flush();
	return bin_status;
}
