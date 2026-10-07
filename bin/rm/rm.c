#include "../common.h"

/*
 * rm [-r] [-f] file ...: removes files; -r also folders and what is inside them;
 * -f does not complain about what does not exist and also removes read-only files.
 * It does not go to the recycle bin. "." , ".." and the root of a drive are refused.
 */

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch;
	bool recursive = false, force = false;

	bin_name = L"rm";
	bin_usage = "usage: rm [-r] [-f] file ...\n"
		"  -r  remove folders and their content\n"
		"  -f  ignore what does not exist; remove read-only files\n"
		"  Nothing goes to the recycle bin.\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'r': case 'R': recursive = true; break;
			case 'f': force = true; break;
			default: opt_bad(ch);
		}
	}
	if (o.i >= argc) {
		if (!force) {
			err_print(L"missing operand");
			err_print(L"Try '%ls --help' for more information.", bin_name);
			return 2;
		}
		return 0;
	}

	for (int i = o.i; i < argc; i++) {
		const wchar_t *p = argv[i];
		int len;
		const wchar_t *base = path_base(p, &len);
		wchar_t *full;
		DWORD a;

		if ((len == 1 && base[0] == L'.') || (len == 2 && base[0] == L'.' && base[1] == L'.')) {
			err_print(L"refusing to remove '.' or '..': %ls", p);
			bin_status = 1;
			continue;
		}
		full = path_full(p);
		if (full && path_is_root(full)) {
			err_print(L"refusing to remove the root: %ls", p);
			bin_status = 1;
			free(full);
			continue;
		}
		free(full);

		a = path_attr(p);
		if (a == INVALID_FILE_ATTRIBUTES) {
			if (!force) {
				err_print(L"cannot remove '%ls': %ls", p, err_text(GetLastError()));
				bin_status = 1;
			}
			continue;
		}
		if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT) && !recursive) {
			err_print(L"cannot remove '%ls': Is a directory", p);
			bin_status = 1;
			continue;
		}
		tree_remove(p, force);
	}
	return bin_status;
}
