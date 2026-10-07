#include "../common.h"

/* mkdir [-p] folder ...: creates folders; -p creates the missing parents and accepts one that already exists */

static bool parents;

/* every folder of the path, from the first one */
static bool make_parents (const wchar_t *path) {

	wchar_t *copy = _wcsdup(path);
	bool ok = true;
	int n;

	if (!copy)
		return false;
	n = (int) wcslen(copy);
	while (n > 1 && is_sep(copy[n - 1]) && copy[n - 2] != L':')
		copy[--n] = 0;

	for (int i = 1; copy[i] && ok; i++) {
		if (!is_sep(copy[i]) || copy[i - 1] == L':' || is_sep(copy[i - 1]))
			continue;
		copy[i] = 0;
		if (!CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS && !path_is_dir(copy)) {
			err_print(L"cannot create directory '%ls': %ls", copy, err_text(GetLastError()));
			bin_status = 1;
			ok = false;
		}
		copy[i] = path[i];
	}
	if (ok && !CreateDirectoryW(copy, NULL) && !path_is_dir(copy)) {
		err_print(L"cannot create directory '%ls': %ls", path, err_text(GetLastError()));
		bin_status = 1;
		ok = false;
	}
	free(copy);
	return ok;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;

	bin_name = L"mkdir";
	bin_usage = "usage: mkdir [-p] folder ...\n"
		"  -p  create the missing parent folders; no error if it already exists\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch == 'p')
			parents = true;
		else
			opt_bad(ch);
	}
	if (o.i >= argc) {
		err_print(L"missing operand");
		err_print(L"Try '%ls --help' for more information.", bin_name);
		return 2;
	}

	for (int i = o.i; i < argc; i++) {
		if (parents)
			make_parents(argv[i]);
		else if (!CreateDirectoryW(argv[i], NULL)) {
			err_print(L"cannot create directory '%ls': %ls", argv[i], err_text(GetLastError()));
			bin_status = 1;
		}
	}
	return bin_status;
}
