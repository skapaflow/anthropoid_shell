#include "../common.h"

/*
 * cp [-r] [-f] source ... destination
 * The destination is a folder when there is more than one source (or when it already is one);
 * otherwise it is the new name. -r copies folders; -f replaces a read-only destination.
 */

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nsrc;
	bool recursive = false, force = false, dest_dir;
	const wchar_t *dest;

	bin_name = L"cp";
	bin_usage = "usage: cp [-r] [-f] source ... destination\n"
		"  -r  copy folders and their content\n"
		"  -f  replace a read-only destination\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'r': case 'R': recursive = true; break;
			case 'f': force = true; break;
			default: opt_bad(ch);
		}
	}
	nsrc = argc - o.i - 1;
	if (nsrc < 1) {
		err_print(nsrc < 0 ? L"missing file operand" : L"missing destination file operand after '%ls'", nsrc < 0 ? L"" : argv[o.i]);
		err_print(L"Try '%ls --help' for more information.", bin_name);
		return 2;
	}
	dest = argv[argc - 1];
	dest_dir = path_is_dir(dest);
	if (nsrc > 1 && !dest_dir) {
		err_print(L"target '%ls': Not a directory", dest);
		return 1;
	}

	for (int i = o.i; i < argc - 1; i++) {
		const wchar_t *src = argv[i];
		wchar_t *target = NULL, *from, *to;
		DWORD a = path_attr(src);

		if (a == INVALID_FILE_ATTRIBUTES) {
			err_path(src, GetLastError());
			continue;
		}
		if (dest_dir) {
			int len;
			const wchar_t *base = path_base(src, &len);
			wchar_t name[MAX_PATH];
			if (len <= 0 || len >= MAX_PATH)
				continue;
			wmemcpy(name, base, len);
			name[len] = 0;
			target = path_join(dest, name);
		} else
			target = _wcsdup(dest);
		if (!target)
			continue;

		if ((a & FILE_ATTRIBUTE_DIRECTORY) && !recursive) {
			err_print(L"-r not specified; omitting directory '%ls'", src);
			bin_status = 1;
			free(target);
			continue;
		}
		if (a & FILE_ATTRIBUTE_DIRECTORY) {
			from = path_full(src);
			to = path_full(target);
			if (from && to && path_inside(from, to)) {
				err_print(L"cannot copy a directory, '%ls', into itself, '%ls'", src, target);
				bin_status = 1;
				free(from);
				free(to);
				free(target);
				continue;
			}
			free(from);
			free(to);
		}
		if (!tree_copy(src, target, force))
			bin_status = 1;
		free(target);
	}
	return bin_status;
}
