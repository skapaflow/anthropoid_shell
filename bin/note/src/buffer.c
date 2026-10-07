#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "buffer.h"
#include "screen.h"

/*************************************************************
 * Text of note: an array of UTF-16 lines.                   *
 *                                                           *
 * Reading: UTF-16 LE/BE (by the BOM), UTF-8 (with or        *
 * without BOM); if it is not valid UTF-8, the Windows ANSI  *
 * code page (old files). Saving uses the same encoding and  *
 * the line break of the first line of the file.             *
 *                                                           *
 * Text crossing lines uses '\n' between them                *
 * (buffer_insert, buffer_delete, buffer_text).              *
 *************************************************************/

#define MAX_FILE_SIZE (256 * 1024 * 1024)

void *xrealloc (void *p, size_t n) {

	p = realloc(p, n);
	if (!p) {
		screen_close();
		console_write(STD_ERROR_HANDLE, L"note: out of memory\n");
		exit(1);
	}
	return p;
}

static void line_reserve (LINE *l, int need) {

	int cap = l->cap ? l->cap : 16;

	if (need <= l->cap)
		return;
	while (cap < need)
		cap *= 2;
	l->s = xrealloc(l->s, cap * sizeof(wchar_t));
	l->cap = cap;
}

static void line_insert (LINE *l, int at, const wchar_t *s, int n) {

	if (n <= 0)
		return;
	line_reserve(l, l->len + n);
	memmove(l->s + at + n, l->s + at, (l->len - at) * sizeof(wchar_t));
	memcpy(l->s + at, s, n * sizeof(wchar_t));
	l->len += n;
}

/* 'count' empty lines at 'at' */
static void lines_insert (BUFFER *b, int at, int count) {

	if (b->count + count > b->cap) {
		while (b->count + count > b->cap)
			b->cap = b->cap ? b->cap * 2 : 64;
		b->lines = xrealloc(b->lines, b->cap * sizeof(LINE));
	}
	memmove(b->lines + at + count, b->lines + at, (b->count - at) * sizeof(LINE));
	memset(b->lines + at, 0, count * sizeof(LINE));
	b->count += count;
}

static void lines_delete (BUFFER *b, int at, int count) {

	int i;

	for (i = at; i < at + count; i++)
		free(b->lines[i].s);
	memmove(b->lines + at, b->lines + at + count, (b->count - at - count) * sizeof(LINE));
	b->count -= count;
}

/* a change in line 'y' makes the syntax state of the lines after it unknown */
static void invalidate (BUFFER *b, int y) {

	if (b->valid > y + 1)
		b->valid = y + 1;
}

/* where text 's' inserted at (y, x) ends */
void text_end (int y, int x, const wchar_t *s, int n, int *ey, int *ex) {

	int i, last = -1;

	for (i = 0; i < n; i++)
		if (s[i] == L'\n') {
			y++;
			last = i;
		}
	*ey = y;
	*ex = last < 0 ? x + n : n - last - 1;
}

/* inserts s[0..n) ('\n' breaks lines) at (y, x); the end goes to (ey, ex) */
void buffer_insert (BUFFER *b, int y, int x, const wchar_t *s, int n, int *ey, int *ex) {

	int breaks = 0, i, start, row;
	LINE *l;

	invalidate(b, y);
	for (i = 0; i < n; i++)
		if (s[i] == L'\n')
			breaks++;

	if (breaks == 0) {
		line_insert(&b->lines[y], x, s, n);
		if (ey) {
			*ey = y;
			*ex = x + n;
		}
		return;
	}

	/* the tail of line y goes to the end of the last new line */
	lines_insert(b, y + 1, breaks);
	l = &b->lines[y];
	line_insert(&b->lines[y + breaks], 0, l->s + x, l->len - x);
	l->len = x;

	row = y;
	start = 0;
	for (i = 0; i <= n; i++)
		if (i == n || s[i] == L'\n') {
			line_insert(&b->lines[row], row == y ? x : 0, s + start, i - start);
			if (i < n)
				row++;
			start = i + 1;
		}

	if (ey)
		text_end(y, x, s, n, ey, ex);
}

/* removes the text from (y1, x1) to (y2, x2) */
void buffer_delete (BUFFER *b, int y1, int x1, int y2, int x2) {

	LINE *first = &b->lines[y1], *last = &b->lines[y2];

	invalidate(b, y1);
	if (y1 == y2) {
		memmove(first->s + x1, first->s + x2, (first->len - x2) * sizeof(wchar_t));
		first->len -= x2 - x1;
		return;
	}
	first->len = x1;
	line_insert(first, x1, last->s + x2, last->len - x2);
	lines_delete(b, y1 + 1, y2 - y1);
}

/* copy of the text from (y1, x1) to (y2, x2), '\n' between lines; free() it */
wchar_t *buffer_text (BUFFER *b, int y1, int x1, int y2, int x2, int *n) {

	int size = 1, y, len = 0;
	wchar_t *t;

	for (y = y1; y <= y2; y++)
		size += b->lines[y].len + 1;
	t = xrealloc(NULL, size * sizeof(wchar_t));

	for (y = y1; y <= y2; y++) {
		LINE *l = &b->lines[y];
		int from = y == y1 ? x1 : 0, to = y == y2 ? x2 : l->len;
		memcpy(t + len, l->s + from, (to - from) * sizeof(wchar_t));
		len += to - from;
		if (y < y2)
			t[len++] = L'\n';
	}
	t[len] = 0;
	*n = len;
	return t;
}

static void buffer_clear (BUFFER *b) {

	lines_delete(b, 0, b->count);
	b->valid = 0;
}

void buffer_init (BUFFER *b) {

	memset(b, 0, sizeof *b);
	b->crlf = true;
	lines_insert(b, 0, 1);
}

void buffer_free (BUFFER *b) {

	buffer_clear(b);
	free(b->lines);
	memset(b, 0, sizeof *b);
}

/* 0: read; 1: does not exist (new file); -1: error (GetLastError) */
int buffer_load (BUFFER *b, const wchar_t *path) {

	HANDLE f;
	LARGE_INTEGER size;
	DWORD got;
	char *data, *p;
	wchar_t *w;
	int n, wn, i, start, row;
	UINT cp = CP_UTF8;
	bool first_eol = true;

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? 1 : -1;

	if (!GetFileSizeEx(f, &size) || size.QuadPart > MAX_FILE_SIZE) {
		CloseHandle(f);
		SetLastError(ERROR_FILE_TOO_LARGE);
		return -1;
	}

	n = (int) size.QuadPart;
	data = xrealloc(NULL, n + 1);
	if (!ReadFile(f, data, (DWORD) n, &got, NULL)) {
		DWORD e = GetLastError();
		free(data);
		CloseHandle(f);
		SetLastError(e);
		return -1;
	}
	CloseHandle(f);
	n = (int) got;

	b->bom = false;
	b->encoding = ENC_UTF8;
	p = data;
	if (n >= 2 && (!memcmp(p, "\xFF\xFE", 2) || !memcmp(p, "\xFE\xFF", 2))) {
		unsigned char *u = (unsigned char *) p + 2;
		b->encoding = p[0] == '\xFF' ? ENC_UTF16LE : ENC_UTF16BE;
		wn = (n - 2) / 2;
		w = xrealloc(NULL, (wn + 1) * sizeof(wchar_t));
		for (i = 0; i < wn; i++)
			w[i] = b->encoding == ENC_UTF16LE ? u[2*i] | (u[2*i+1] << 8) : (u[2*i] << 8) | u[2*i+1];
		free(data);
		goto split;
	}
	if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
		b->bom = true;
		p += 3;
		n -= 3;
	}

	wn = 0;
	if (n > 0) {
		wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, NULL, 0);
		if (wn == 0 && !b->bom) {
			wn = MultiByteToWideChar(CP_ACP, 0, p, n, NULL, 0);
			if (wn > 0) {
				cp = CP_ACP;
				b->encoding = ENC_ANSI;
			}
		}
		if (wn == 0)
			wn = MultiByteToWideChar(CP_UTF8, 0, p, n, NULL, 0);
	}
	w = xrealloc(NULL, (wn + 1) * sizeof(wchar_t));
	if (wn > 0)
		MultiByteToWideChar(cp, 0, p, n, w, wn);
	free(data);

split:
	/* split into lines; an empty last line stands for the '\n' at the end of the file */
	buffer_clear(b);
	b->crlf = true;
	row = 1;
	for (i = 0; i < wn; i++)
		if (w[i] == L'\n')
			row++;
	lines_insert(b, 0, row);

	row = 0;
	start = 0;
	for (i = 0; i <= wn; i++) {
		if (i == wn || w[i] == L'\n') {
			int end = i;
			if (i < wn) {
				bool cr = end > start && w[end-1] == L'\r';
				if (first_eol) {
					b->crlf = cr;
					first_eol = false;
				}
				if (cr)
					end--;
			}
			line_insert(&b->lines[row++], 0, w + start, end - start);
			start = i + 1;
		}
	}
	free(w);
	return 0;
}

const wchar_t *buffer_encoding_name (const BUFFER *b) {

	switch (b->encoding) {
	case ENC_ANSI:    return L"ANSI";
	case ENC_UTF16LE: return L"UTF-16 LE";
	case ENC_UTF16BE: return L"UTF-16 BE";
	}
	return b->bom ? L"UTF-8 BOM" : L"UTF-8";
}

/* does every character fit the encoding? (only the ANSI code page can miss some) */
bool buffer_fits (BUFFER *b) {

	BOOL lost = FALSE;
	int i;

	if (b->encoding != ENC_ANSI)
		return true;
	for (i = 0; i < b->count && !lost; i++)
		if (b->lines[i].len)
			WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, b->lines[i].s, b->lines[i].len, NULL, 0, NULL, &lost);
	return !lost;
}

/* s[0..n) in the encoding of the file, appended to data */
static void encode (BUFFER *b, const wchar_t *s, int n, char **data, size_t *len, size_t *cap) {

	int need, i;

	if (b->encoding == ENC_UTF16LE || b->encoding == ENC_UTF16BE)
		need = 2 * n;
	else
		need = n ? WideCharToMultiByte(b->encoding == ENC_ANSI ? CP_ACP : CP_UTF8, 0, s, n, NULL, 0, NULL, NULL) : 0;

	if (*len + need > *cap) {
		while (*len + need > *cap)
			*cap *= 2;
		*data = xrealloc(*data, *cap);
	}

	if (b->encoding == ENC_UTF16LE || b->encoding == ENC_UTF16BE) {
		unsigned char *u = (unsigned char *) *data + *len;
		bool le = b->encoding == ENC_UTF16LE;
		for (i = 0; i < n; i++) {
			u[2*i]     = le ? s[i] & 255 : s[i] >> 8;
			u[2*i + 1] = le ? s[i] >> 8 : s[i] & 255;
		}
	} else if (need)
		WideCharToMultiByte(b->encoding == ENC_ANSI ? CP_ACP : CP_UTF8, 0, s, n, *data + *len, need, NULL, NULL);
	*len += need;
}

/* writes a temporary file next to it and swaps it in: a failure does not destroy the file */
bool buffer_save (BUFFER *b, const wchar_t *path) {

	size_t cap = 4096, n = 0;
	char *data = xrealloc(NULL, cap);
	wchar_t *tmp;
	HANDLE f;
	DWORD written, err = 0;
	BOOL ok;
	int i;

	if (b->encoding == ENC_UTF16LE || b->encoding == ENC_UTF16BE || (b->bom && b->encoding == ENC_UTF8))
		encode(b, L"\xFEFF", 1, &data, &n, &cap);

	for (i = 0; i < b->count; i++) {
		encode(b, b->lines[i].s, b->lines[i].len, &data, &n, &cap);
		if (i < b->count - 1)
			encode(b, b->crlf ? L"\r\n" : L"\n", b->crlf ? 2 : 1, &data, &n, &cap);
	}

	tmp = xrealloc(NULL, (wcslen(path) + 16) * sizeof(wchar_t));
	swprintf(tmp, wcslen(path) + 16, L"%ls.note~", path);

	f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		err = GetLastError();
		free(data);
		free(tmp);
		SetLastError(err);
		return false;
	}
	ok = WriteFile(f, data, (DWORD) n, &written, NULL) && written == n;
	if (!ok)
		err = GetLastError();
	if (!CloseHandle(f) && ok) {
		ok = FALSE;
		err = GetLastError();
	}
	free(data);

	if (ok) {
		/* ReplaceFileW keeps the attributes and permissions of the original */
		if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
			ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
		else
			ok = ReplaceFileW(path, tmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
		if (!ok)
			err = GetLastError();
	}
	if (!ok)
		DeleteFileW(tmp);
	free(tmp);

	if (!ok) {
		SetLastError(err);
		return false;
	}
	return true;
}
