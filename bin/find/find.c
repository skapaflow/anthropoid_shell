#include "../common.h"

/*************************************************************
 * find [path ...] [tests] [action]                          *
 *                                                           *
 * Walks the folders and writes the paths that pass every    *
 * test (they are all required; there is no -o).             *
 *   -name P  -iname P   the name matches P (* ? [a-z] [!a]; *
 *                       case does not matter on Windows)    *
 *   -type f|d|l         file, folder or link                *
 *   -size [+-]N[ckMGb]  size: more than, less than or N     *
 *                       (no suffix: blocks of 512 bytes)    *
 *   -empty              empty file or folder                *
 *   -mindepth N  -maxdepth N                                *
 *   ! or -not           negates the next test               *
 * Actions: -print (default), -print0, -delete (empty        *
 * folders and files, the inside first).                      *
 * Quote the patterns ("*.c"), or the shell expands them.    *
 *************************************************************/

enum { T_NAME, T_TYPE, T_SIZE, T_EMPTY };
enum { ACT_PRINT, ACT_PRINT0, ACT_DELETE };

typedef struct {
	int kind;
	bool negate;
	const wchar_t *pattern;
	wchar_t type;
	long long n;
	int cmp;       /* size: '+', '-' or '=' */
	long long unit;
} TEST;

static TEST tests[64];
static int ntests;
static int action = ACT_PRINT;
static int max_depth = 1 << 30, min_depth = 0;

static bool dir_is_empty (const wchar_t *path) {

	wchar_t *pattern = path_join(path, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h;
	bool empty = true;

	if (!pattern)
		return false;
	h = FindFirstFileW(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	do {
		if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
			empty = false;
			break;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return empty;
}

static bool passes (const TEST *t, const wchar_t *path, const wchar_t *name, DWORD attr, ULONGLONG size) {

	bool is_link = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) && !is_link;
	bool r = false;

	switch (t->kind) {
		case T_NAME:
			r = glob_match(t->pattern, name);
			break;
		case T_TYPE:
			r = t->type == L'l' ? is_link : t->type == L'd' ? is_dir : (!is_dir && !is_link);
			break;
		case T_SIZE: {
			long long units = (long long) ((size + t->unit - 1) / t->unit);
			r = t->cmp == '+' ? units > t->n : t->cmp == '-' ? units < t->n : units == t->n;
			break;
		}
		case T_EMPTY:
			r = is_dir ? dir_is_empty(path) : (!(attr & FILE_ATTRIBUTE_DIRECTORY) && size == 0);
			break;
	}
	return r != t->negate;
}

static void act (const wchar_t *path, const wchar_t *name, DWORD attr, ULONGLONG size, int depth) {

	if (depth < min_depth)
		return;
	for (int i = 0; i < ntests; i++)
		if (!passes(&tests[i], path, name, attr, size))
			return;

	if (action == ACT_DELETE) {
		bool dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (!(dir ? RemoveDirectoryW(path) : DeleteFileW(path))) {
			err_print(L"cannot delete '%ls': %ls", path, err_text(GetLastError()));
			bin_status = 1;
		}
		return;
	}
	out_w(path);
	out_char(action == ACT_PRINT0 ? '\0' : '\n');
}

static void visit (const wchar_t *path, const wchar_t *name, DWORD attr, ULONGLONG size, int depth) {

	bool descend = (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT) && depth < max_depth;

	/* -delete takes the inside first */
	if (action != ACT_DELETE)
		act(path, name, attr, size, depth);
	if (descend) {
		wchar_t *pattern = path_join(path, L"*");
		WIN32_FIND_DATAW fd;
		HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;

		free(pattern);
		if (h == INVALID_HANDLE_VALUE) {
			if (GetLastError() != ERROR_FILE_NOT_FOUND) {
				err_print(L"'%ls': %ls", path, err_text(GetLastError()));
				bin_status = 1;
			}
		} else {
			do {
				wchar_t *child;
				if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
					continue;
				if ((child = path_join(path, fd.cFileName))) {
					visit(child, fd.cFileName, fd.dwFileAttributes, ((ULONGLONG) fd.nFileSizeHigh << 32) | fd.nFileSizeLow, depth + 1);
					free(child);
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}
	if (action == ACT_DELETE)
		act(path, name, attr, size, depth);
}

static long long parse_number (const wchar_t *s, wchar_t **end) {

	return wcstoll(s, end, 10);
}

int wmain (int argc, wchar_t **argv) {

	int i = 1, npaths;
	const wchar_t **paths = calloc(argc, sizeof(wchar_t *));
	bool negate = false;

	bin_name = L"find";
	bin_usage = "usage: find [path ...] [tests] [action]\n"
		"  Writes the paths that pass every test.\n"
		"  -name P, -iname P    name matches P (* ? [a-z] [!a]); case does not matter\n"
		"  -type f|d|l          file, folder or link\n"
		"  -size [+-]N[ckMGb]   more than / less than / exactly N (no suffix: 512-byte blocks)\n"
		"  -empty               empty file or folder\n"
		"  -mindepth N  -maxdepth N\n"
		"  ! or -not            negates the next test\n"
		"  actions: -print (default), -print0, -delete\n"
		"  Quote the patterns (\"*.c\"): the shell expands the bare ones.\n";

	if (argc == 2 && !wcscmp(argv[1], L"--help")) {
		out_str(bin_usage);
		out_flush();
		return 0;
	}
	if (!paths)
		return 1;

	for (npaths = 0; i < argc && argv[i][0] != L'-' && wcscmp(argv[i], L"!") && wcscmp(argv[i], L"("); i++)
		paths[npaths++] = argv[i];

	for (; i < argc; i++) {
		const wchar_t *a = argv[i];
		TEST *t = ntests < 64 ? &tests[ntests] : NULL;

		if (!wcscmp(a, L"!") || !wcscmp(a, L"-not")) {
			negate = !negate;
			continue;
		}
		if (!wcscmp(a, L"-print"))
			action = ACT_PRINT;
		else if (!wcscmp(a, L"-print0"))
			action = ACT_PRINT0;
		else if (!wcscmp(a, L"-delete"))
			action = ACT_DELETE;
		else if (!wcscmp(a, L"-empty") && t) {
			memset(t, 0, sizeof *t);
			t->kind = T_EMPTY;
			t->negate = negate;
			ntests++;
		} else if ((!wcscmp(a, L"-name") || !wcscmp(a, L"-iname") || !wcscmp(a, L"-type") || !wcscmp(a, L"-size") ||
			!wcscmp(a, L"-maxdepth") || !wcscmp(a, L"-mindepth")) && t) {
			const wchar_t *v;
			wchar_t *end;

			if (++i >= argc) {
				err_print(L"missing argument to '%ls'", a);
				return 1;
			}
			v = argv[i];
			memset(t, 0, sizeof *t);
			t->negate = negate;
			if (!wcscmp(a, L"-name") || !wcscmp(a, L"-iname")) {
				t->kind = T_NAME;
				t->pattern = v;
				ntests++;
			} else if (!wcscmp(a, L"-type")) {
				if (wcslen(v) != 1 || !wcschr(L"fdl", v[0])) {
					err_print(L"unknown argument to -type: %ls", v);
					return 1;
				}
				t->kind = T_TYPE;
				t->type = v[0];
				ntests++;
			} else if (!wcscmp(a, L"-size")) {
				t->kind = T_SIZE;
				t->cmp = (v[0] == L'+' || v[0] == L'-') ? v[0] : '=';
				if (v[0] == L'+' || v[0] == L'-')
					v++;
				t->n = parse_number(v, &end);
				t->unit = 512;
				if (end == v || (*end && end[1])) {
					err_print(L"invalid argument '%ls' to -size", argv[i]);
					return 1;
				}
				switch (*end) {
					case 0: case L'b': t->unit = 512; break;
					case L'c': t->unit = 1; break;
					case L'k': t->unit = 1024; break;
					case L'M': t->unit = 1024LL * 1024; break;
					case L'G': t->unit = 1024LL * 1024 * 1024; break;
					default:
						err_print(L"invalid argument '%ls' to -size", argv[i]);
						return 1;
				}
				ntests++;
			} else {
				long long n = parse_number(v, &end);
				if (*end || n < 0) {
					err_print(L"invalid argument '%ls' to '%ls'", v, a);
					return 1;
				}
				if (!wcscmp(a, L"-maxdepth"))
					max_depth = (int) n;
				else
					min_depth = (int) n;
			}
		} else {
			err_print(L"unknown predicate '%ls'", a);
			return 1;
		}
		negate = false;
	}

	if (npaths == 0)
		paths[npaths++] = L".";
	for (int k = 0; k < npaths; k++) {
		WIN32_FILE_ATTRIBUTE_DATA d;
		int len;
		const wchar_t *base = path_base(paths[k], &len);
		wchar_t name[MAX_PATH];

		if (!GetFileAttributesExW(paths[k], GetFileExInfoStandard, &d)) {
			err_print(L"'%ls': %ls", paths[k], err_text(GetLastError()));
			bin_status = 1;
			continue;
		}
		if (len <= 0 || len >= MAX_PATH) {
			wcscpy(name, paths[k]);
		} else {
			wmemcpy(name, base, len);
			name[len] = 0;
		}
		visit(paths[k], name, d.dwFileAttributes, ((ULONGLONG) d.nFileSizeHigh << 32) | d.nFileSizeLow, 0);
	}
	out_flush();
	return bin_status;
}
