#include "../common.h"

/*
 * mv source ... destination
 * The destination is a folder when there is more than one source (or when it already is one);
 * otherwise it is the new name. It replaces what is there. Across drives a file is copied and then
 * removed; a folder too.
 */

static bool move_one (const wchar_t *src, const wchar_t *dst) {

	DWORD a = path_attr(src);
	wchar_t *from, *to;

	if (a == INVALID_FILE_ATTRIBUTES) {
		err_path(src, GetLastError());
		return false;
	}
	if (path_exists(dst) && same_file(src, dst)) {
		err_print(L"'%ls' and '%ls' are the same file", src, dst);
		bin_status = 1;
		return false;
	}
	if ((a & FILE_ATTRIBUTE_DIRECTORY) && path_exists(dst) && !path_is_dir(dst)) {
		err_print(L"cannot overwrite non-directory '%ls' with directory '%ls'", dst, src);
		bin_status = 1;
		return false;
	}
	if (a & FILE_ATTRIBUTE_DIRECTORY) {
		from = path_full(src);
		to = path_full(dst);
		if (from && to && path_inside(from, to)) {
			err_print(L"cannot move '%ls' to a subdirectory of itself, '%ls'", src, dst);
			bin_status = 1;
			free(from);
			free(to);
			return false;
		}
		free(from);
		free(to);
	}

	/* files replace what is there; a folder goes where nothing exists (or into an empty one, by the rename) */
	if (MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		return true;

	if ((a & FILE_ATTRIBUTE_DIRECTORY) && GetLastError() == ERROR_NOT_SAME_DEVICE) {
		/* a folder to another drive: copy everything, then remove the original */
		if (tree_copy(src, dst, true)) {
			if (!tree_remove(src, true))
				bin_status = 1;
			return true;
		}
		return false;
	}
	err_print(L"cannot move '%ls' to '%ls': %ls", src, dst, err_text(GetLastError()));
	bin_status = 1;
	return false;
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, nsrc;
	bool dest_dir;
	const wchar_t *dest;

	bin_name = L"mv";
	bin_usage = "usage: mv source ... destination\n"
		"  Moves or renames. With several sources, or a folder as destination, they go inside it.\n"
		"  What already exists at the destination is replaced.\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		if (ch != 'f')
			opt_bad(ch);
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
		wchar_t *target;

		if (dest_dir) {
			int len;
			const wchar_t *base = path_base(argv[i], &len);
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
		move_one(argv[i], target);
		free(target);
	}
	return bin_status;
}
