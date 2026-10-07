#ifndef ANT_BIN_COMMON_H
#define ANT_BIN_COMMON_H

/*************************************************************
 * Shared code of the bin\ programs (ls, cat, cp, ...).      *
 * Header only: every program is one .c file that includes   *
 * this and is compiled on its own:                          *
 *   clang -O2 -Wall -municode bin\ls\ls.c -o bin\ls.exe     *
 *                                                           *
 *   - wmain: arguments arrive in UTF-16;                    *
 *   - output goes through a UTF-8 buffer: a console gets it *
 *     by WriteConsoleW (right whatever the code page), a    *
 *     pipe or file gets the UTF-8 bytes;                    *
 *   - errors in the unix style to standard error:           *
 *     "name: file: No such file or directory";              *
 *   - exit code: 0 ok, 1 error, 2 usage.                    *
 *************************************************************/

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <windows.h>

static const wchar_t *bin_name = L"";
static const char *bin_usage = "";
static int bin_status = 0;

/* ---------- output ---------- */

#define OUT_SIZE 65536

static char out_buf[OUT_SIZE];
static int out_len = 0;
static HANDLE out_handle = NULL;
static bool out_console = false;
static WORD out_default_attr = 7;

static inline void out_init (void) {

	DWORD mode;
	CONSOLE_SCREEN_BUFFER_INFO info;

	if (out_handle)
		return;
	out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	out_console = GetConsoleMode(out_handle, &mode) != 0;
	if (out_console && GetConsoleScreenBufferInfo(out_handle, &info))
		out_default_attr = info.wAttributes;
}

/* how many bytes of s[0..n) end on a complete UTF-8 character */
static inline int utf8_complete (const char *s, int n) {

	for (int back = 1; back <= 3 && back <= n; back++) {
		unsigned char c = (unsigned char) s[n - back];
		if ((c & 0xC0) == 0x80)
			continue;
		if (c >= 0xF0 && back < 4)
			return n - back;
		if (c >= 0xE0 && c < 0xF0 && back < 3)
			return n - back;
		if (c >= 0xC0 && c < 0xE0 && back < 2)
			return n - back;
		return n;
	}
	return n;
}

static inline void out_flush (void) {

	int done = 0;

	out_init();
	if (out_len == 0)
		return;

	if (out_console) {
		int n = utf8_complete(out_buf, out_len), w;
		wchar_t *wide = malloc(sizeof(wchar_t) * (n + 1));

		if (wide && (w = MultiByteToWideChar(CP_UTF8, 0, out_buf, n, wide, n)) > 0) {
			DWORD wrote;
			for (int at = 0; at < w; at += wrote)
				if (!WriteConsoleW(out_handle, wide + at, w - at, &wrote, NULL))
					break;
		}
		free(wide);
		memmove(out_buf, out_buf + n, out_len - n);
		out_len -= n;
		return;
	}

	while (done < out_len) {
		DWORD wrote;
		/* the reader went away (ls | head -1): stop quietly, as a SIGPIPE would */
		if (!WriteFile(out_handle, out_buf + done, out_len - done, &wrote, NULL))
			ExitProcess(1);
		done += wrote;
	}
	out_len = 0;
}

static inline void out_bytes (const char *s, int n) {

	while (n > 0) {
		int room = OUT_SIZE - out_len, k = n < room ? n : room;
		memcpy(out_buf + out_len, s, k);
		out_len += k;
		s += k;
		n -= k;
		if (out_len == OUT_SIZE)
			out_flush();
	}
}

static inline void out_str (const char *s) {

	out_bytes(s, (int) strlen(s));
}

static inline void out_char (char c) {

	out_bytes(&c, 1);
}

static inline void out_fmt (const char *fmt, ...) {

	char text[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (n > 0)
		out_bytes(text, n < (int) sizeof text ? n : (int) sizeof text - 1);
}

/* wide text as UTF-8 (a surrogate pair is never cut in two) */
static inline void out_w (const wchar_t *w) {

	char u8[256 * 3 + 8];

	while (*w) {
		int n = 0, k;

		while (w[n] && n < 255)
			n++;
		if (n > 0 && w[n] && w[n - 1] >= 0xD800 && w[n - 1] < 0xDC00)
			n--;
		k = WideCharToMultiByte(CP_UTF8, 0, w, n, u8, sizeof u8, NULL, NULL);
		if (k > 0)
			out_bytes(u8, k);
		w += n;
	}
}

/* color on a console (nothing when the output is a pipe or file) */
static inline void out_color (WORD attr) {

	out_init();
	if (!out_console)
		return;
	out_flush();
	SetConsoleTextAttribute(out_handle, attr);
}

static inline void out_color_off (void) {

	out_init();
	if (!out_console)
		return;
	out_flush();
	SetConsoleTextAttribute(out_handle, out_default_attr);
}

/* ---------- errors ---------- */

/* one line on standard error: "name: text" (text in UTF-16), red on a console */
static inline void err_line (const wchar_t *text) {

	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	DWORD mode, wrote;
	wchar_t line[4200];
	int n;

	out_flush();
	n = swprintf(line, 4200, L"%ls: %ls\n", bin_name, text);
	if (n <= 0)
		return;
	if (GetConsoleMode(h, &mode)) {
		CONSOLE_SCREEN_BUFFER_INFO info;
		bool have = GetConsoleScreenBufferInfo(h, &info);
		/* the console wants \r\n */
		line[n - 1] = L'\r';
		line[n++] = L'\n';
		SetConsoleTextAttribute(h, FOREGROUND_RED | FOREGROUND_INTENSITY);
		WriteConsoleW(h, line, n, &wrote, NULL);
		if (have)
			SetConsoleTextAttribute(h, info.wAttributes);
	} else {
		char u8[4200 * 3];
		int k = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof u8, NULL, NULL);
		if (k > 0)
			WriteFile(h, u8, k, &wrote, NULL);
	}
}

static inline void err_print (const wchar_t *fmt, ...) {

	wchar_t text[4096];
	va_list ap;

	va_start(ap, fmt);
	vswprintf(text, 4096, fmt, ap);
	va_end(ap);
	err_line(text);
}

/* Windows error code as the unix message */
static inline const wchar_t *err_text (DWORD code) {

	static wchar_t other[512];

	switch (code) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_INVALID_NAME:
		case ERROR_BAD_PATHNAME:
			return L"No such file or directory";
		case ERROR_ACCESS_DENIED:
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return L"Permission denied";
		case ERROR_ALREADY_EXISTS:
		case ERROR_FILE_EXISTS:
			return L"File exists";
		case ERROR_DIR_NOT_EMPTY:
			return L"Directory not empty";
		case ERROR_DIRECTORY:
			return L"Not a directory";
		case ERROR_NOT_SAME_DEVICE:
			return L"Invalid cross-device link";
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL:
			return L"No space left on device";
		default: {
			int n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, other, 512, NULL);
			while (n > 0 && (other[n - 1] == L'\r' || other[n - 1] == L'\n' || other[n - 1] == L' '))
				other[--n] = 0;
			if (n == 0)
				swprintf(other, 512, L"error %lu", code);
			return other;
		}
	}
}

/* "name: path: No such file or directory"; sets the exit code to 1 */
static inline void err_path (const wchar_t *path, DWORD code) {

	err_print(L"%ls: %ls", path, err_text(code));
	bin_status = 1;
}

static inline void err_path_text (const wchar_t *path, const wchar_t *text) {

	err_print(L"%ls: %ls", path, text);
	bin_status = 1;
}

/* ---------- options ---------- */

typedef struct {
	int argc;
	wchar_t **argv;
	int i;             /* next argument to look at; the operands start here when opt_next ends */
	const wchar_t *p;  /* next letter of a group like -rf */
} OPTS;

static inline void opt_init (OPTS *o, int argc, wchar_t **argv) {

	o->argc = argc;
	o->argv = argv;
	o->i = 1;
	o->p = NULL;
}

static inline void opt_bad (int ch) {

	err_print(L"invalid option -- '%lc'", ch);
	err_print(L"Try '%ls --help' for more information.", bin_name);
	out_flush();
	ExitProcess(2);
}

/* the next option letter; -1 when the operands begin. "--" ends the options, --help prints the usage */
static inline int opt_next (OPTS *o) {

	if (!o->p) {
		const wchar_t *a;

		if (o->i >= o->argc)
			return -1;
		a = o->argv[o->i];
		if (a[0] != L'-' || !a[1])
			return -1;
		if (!wcscmp(a, L"--")) {
			o->i++;
			return -1;
		}
		if (!wcscmp(a, L"--help")) {
			out_str(bin_usage);
			out_flush();
			ExitProcess(0);
		}
		if (a[1] == L'-') {
			err_print(L"unrecognized option '%ls'", a);
			err_print(L"Try '%ls --help' for more information.", bin_name);
			out_flush();
			ExitProcess(2);
		}
		o->p = a + 1;
	}
	{
		int ch = *o->p++;
		if (!*o->p) {
			o->p = NULL;
			o->i++;
		}
		return ch;
	}
}

/* the argument of an option that takes one (-n 5 or -n5) */
static inline const wchar_t *opt_arg (OPTS *o, int ch) {

	const wchar_t *v;

	if (o->p) {
		v = o->p;
		o->p = NULL;
		o->i++;
		return v;
	}
	if (o->i >= o->argc) {
		err_print(L"option requires an argument -- '%lc'", ch);
		out_flush();
		ExitProcess(2);
	}
	return o->argv[o->i++];
}

/* ---------- paths ---------- */

static inline DWORD path_attr (const wchar_t *p) {

	return GetFileAttributesW(p);
}

static inline bool path_exists (const wchar_t *p) {

	return path_attr(p) != INVALID_FILE_ATTRIBUTES;
}

static inline bool path_is_dir (const wchar_t *p) {

	DWORD a = path_attr(p);

	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static inline bool is_sep (wchar_t c) {

	return c == L'\\' || c == L'/';
}

/* pointer to the last component of p, ignoring separators at the end; len gets its length */
static inline const wchar_t *path_base (const wchar_t *p, int *len) {

	int n = (int) wcslen(p);
	const wchar_t *end, *start;

	while (n > 1 && is_sep(p[n - 1]) && p[n - 2] != L':')
		n--;
	end = p + n;
	start = end;
	while (start > p && !is_sep(start[-1]) && !(start - p == 2 && start[-1] == L':'))
		start--;
	*len = (int) (end - start);
	return start;
}

/* dir\name in a new string */
static inline wchar_t *path_join (const wchar_t *dir, const wchar_t *name) {

	int n = (int) wcslen(dir);
	wchar_t *r = malloc(sizeof(wchar_t) * (n + wcslen(name) + 2));

	if (!r)
		return NULL;
	wcscpy(r, dir);
	if (n > 0 && !is_sep(dir[n - 1]) && dir[n - 1] != L':')
		r[n++] = L'\\';
	wcscpy(r + n, name);
	return r;
}

/* the full path in a new string */
static inline wchar_t *path_full (const wchar_t *p) {

	DWORD n = GetFullPathNameW(p, 0, NULL, NULL);
	wchar_t *r;

	if (n == 0 || !(r = malloc(sizeof(wchar_t) * (n + 1))))
		return NULL;
	GetFullPathNameW(p, n + 1, r, NULL);
	return r;
}

/* is 'inner' the same as, or inside, 'outer'? (both full paths) */
static inline bool path_inside (const wchar_t *outer, const wchar_t *inner) {

	int n = (int) wcslen(outer);

	while (n > 0 && is_sep(outer[n - 1]))
		n--;
	if (_wcsnicmp(outer, inner, n) != 0)
		return false;
	return inner[n] == 0 || is_sep(inner[n]);
}

/* the same file? (same volume and index) */
static inline bool same_file (const wchar_t *a, const wchar_t *b) {

	HANDLE ha = CreateFileW(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	HANDLE hb = CreateFileW(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	BY_HANDLE_FILE_INFORMATION ia, ib;
	bool same = false;

	if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
		GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib))
		same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
			ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
	if (ha != INVALID_HANDLE_VALUE)
		CloseHandle(ha);
	if (hb != INVALID_HANDLE_VALUE)
		CloseHandle(hb);
	return same;
}

/* the root of a drive or share ("C:\", "\")? */
static inline bool path_is_root (const wchar_t *full) {

	int n = (int) wcslen(full);

	return (n == 3 && full[1] == L':' && is_sep(full[2])) || (n == 1 && is_sep(full[0]));
}

/* ---------- input ---------- */

/* reads up to 'size' bytes (UTF-8 when it comes from a console); 0 = end, -1 = error */
static inline int in_read (HANDLE h, char *buf, int size) {

	DWORD mode, got;

	if (GetConsoleMode(h, &mode)) {
		wchar_t wide[1024];
		DWORD want = (DWORD) (size / 3);
		int n;

		if (want > 1024)
			want = 1024;
		if (want == 0)
			return -1;
		if (!ReadConsoleW(h, wide, want, &got, NULL))
			return -1;
		/* Ctrl+Z is the end of the input on a console */
		if (got > 0 && wide[0] == 0x1A)
			return 0;
		n = WideCharToMultiByte(CP_UTF8, 0, wide, got, buf, size, NULL, NULL);
		return n;
	}
	if (!ReadFile(h, buf, size, &got, NULL))
		return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
	return (int) got;
}

/* opens a file to read; "-" is standard input. INVALID_HANDLE_VALUE: the error is already shown */
static inline HANDLE in_open (const wchar_t *path) {

	DWORD a;
	HANDLE h;

	if (!wcscmp(path, L"-"))
		return GetStdHandle(STD_INPUT_HANDLE);
	a = path_attr(path);
	if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
		err_path_text(path, L"Is a directory");
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		err_path(path, GetLastError());
	return h;
}

static inline void in_close (HANDLE h) {

	if (h != INVALID_HANDLE_VALUE && h != GetStdHandle(STD_INPUT_HANDLE))
		CloseHandle(h);
}

/* the rest of the input in memory (NUL-terminated, but len does not count it); false on a read error */
static inline bool in_slurp (HANDLE h, char **data, size_t *len) {

	size_t cap = 65536, n = 0;
	char *buf = malloc(cap + 1);

	if (!buf)
		return false;
	for (;;) {
		int got;
		if (n + 65536 > cap) {
			char *bigger;
			cap *= 2;
			if (!(bigger = realloc(buf, cap + 1))) {
				free(buf);
				return false;
			}
			buf = bigger;
		}
		got = in_read(h, buf + n, 65536);
		if (got < 0) {
			free(buf);
			return false;
		}
		if (got == 0)
			break;
		n += got;
	}
	buf[n] = 0;
	*data = buf;
	*len = n;
	return true;
}

/* ---------- widths (for columns) ---------- */

/* cells a code point takes on the screen (an approximation: wide East Asian ranges and emoji = 2) */
static inline int cell_width (unsigned long c) {

	if (c == 0)
		return 0;
	if (c >= 0x0300 && c <= 0x036F)
		return 0;
	if (c == 0x200B || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F))
		return 0;
	if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
		(c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
		(c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD))
		return 2;
	return 1;
}

static inline int text_width (const wchar_t *s) {

	int w = 0;

	for (; *s; s++) {
		unsigned long c = *s;
		if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) {
			c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
			s++;
		}
		w += cell_width(c);
	}
	return w;
}

/* ---------- name patterns (find, pkill) ---------- */

/* * ? [abc] [a-z] [!a] [^a]; case does not matter (Windows names) */
/* [abc] [a-z] [!a] [^a] starting at p (after the '['); returns where the set ends, or NULL if it has no ] */
static inline const wchar_t *set_match (const wchar_t *p, wchar_t c, bool *ok) {

	bool neg = false, found = false;

	if (*p == L'!' || *p == L'^') {
		neg = true;
		p++;
	}
	for (bool first = true; *p && (*p != L']' || first); first = false) {
		wchar_t lo = *p++, hi = lo;

		if (*p == L'-' && p[1] && p[1] != L']') {
			hi = p[1];
			p += 2;
		}
		if (towlower(c) >= towlower(lo) && towlower(c) <= towlower(hi))
			found = true;
	}
	if (*p != L']')
		return NULL;
	*ok = found != neg;
	return p + 1;
}

static inline bool glob_match (const wchar_t *p, const wchar_t *s) {

	for (; *p; p++) {
		if (*p == L'*') {
			while (p[1] == L'*')
				p++;
			for (const wchar_t *t = s; ; t++) {
				if (glob_match(p + 1, t))
					return true;
				if (!*t)
					return false;
			}
		}
		if (!*s)
			return false;
		if (*p == L'[') {
			bool ok = false;
			const wchar_t *end = set_match(p + 1, *s, &ok);
			if (end) {
				if (!ok)
					return false;
				p = end - 1;
				s++;
				continue;
			}
		}
		if (*p != L'?' && towlower(*p) != towlower(*s))
			return false;
		s++;
	}
	return !*s;
}

/* ---------- copy and remove (cp, mv, rm) ---------- */

static inline void clear_readonly (const wchar_t *path) {

	DWORD a = path_attr(path);

	if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
		SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
}

static inline bool file_copy (const wchar_t *src, const wchar_t *dst, bool force) {

	if (path_exists(dst) && same_file(src, dst)) {
		err_print(L"'%ls' and '%ls' are the same file", src, dst);
		bin_status = 1;
		return false;
	}
	if (CopyFileW(src, dst, FALSE))
		return true;
	if (force && GetLastError() == ERROR_ACCESS_DENIED && path_exists(dst)) {
		clear_readonly(dst);
		if (CopyFileW(src, dst, FALSE))
			return true;
	}
	err_path(dst, GetLastError());
	return false;
}

/* copies a file or a whole folder; false if something failed (already reported) */
static inline bool tree_copy (const wchar_t *src, const wchar_t *dst, bool force) {

	DWORD a = path_attr(src);
	wchar_t *pattern;
	WIN32_FIND_DATAW fd;
	HANDLE h;
	bool ok = true;

	if (a == INVALID_FILE_ATTRIBUTES) {
		err_path(src, GetLastError());
		return false;
	}
	if (!(a & FILE_ATTRIBUTE_DIRECTORY))
		return file_copy(src, dst, force);

	if (!CreateDirectoryW(dst, NULL) && !path_is_dir(dst)) {
		err_path(dst, GetLastError());
		return false;
	}
	if (!(pattern = path_join(src, L"*")))
		return false;
	h = FindFirstFileW(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE)
		return true;
	do {
		wchar_t *from, *to;
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		from = path_join(src, fd.cFileName);
		to = path_join(dst, fd.cFileName);
		if (from && to)
			ok &= tree_copy(from, to, force);
		free(from);
		free(to);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return ok;
}

static inline bool file_delete (const wchar_t *path, bool force) {

	if (DeleteFileW(path))
		return true;
	if (force && GetLastError() == ERROR_ACCESS_DENIED) {
		clear_readonly(path);
		if (DeleteFileW(path))
			return true;
	}
	err_print(L"cannot remove '%ls': %ls", path, err_text(GetLastError()));
	bin_status = 1;
	return false;
}

/* removes a file or a whole folder (a link to a folder is removed, not followed) */
static inline bool tree_remove (const wchar_t *path, bool force) {

	DWORD a = path_attr(path);
	wchar_t *pattern;
	WIN32_FIND_DATAW fd;
	HANDLE h;
	bool ok = true;

	if (a == INVALID_FILE_ATTRIBUTES) {
		err_print(L"cannot remove '%ls': %ls", path, err_text(GetLastError()));
		bin_status = 1;
		return false;
	}
	if (!(a & FILE_ATTRIBUTE_DIRECTORY))
		return file_delete(path, force);

	if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
		if (!(pattern = path_join(path, L"*")))
			return false;
		h = FindFirstFileW(pattern, &fd);
		free(pattern);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				wchar_t *child;
				if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
					continue;
				if ((child = path_join(path, fd.cFileName)))
					ok &= tree_remove(child, force);
				free(child);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}
	if (!RemoveDirectoryW(path)) {
		if (force && GetLastError() == ERROR_ACCESS_DENIED) {
			clear_readonly(path);
			if (RemoveDirectoryW(path))
				return ok;
		}
		if (ok) {
			err_print(L"cannot remove '%ls': %ls", path, err_text(GetLastError()));
			bin_status = 1;
		}
		return false;
	}
	return ok;
}

#endif
