#include <stdlib.h>
#include <windows.h>

#include "string_util.h"

/* UTF-16 to UTF-8 in des, of max bytes; an empty string if it does not fit */
char *to_narrowchar (char *des, int max, const wchar_t *wstr) {

	if (!WideCharToMultiByte(CP_UTF8, 0, wstr, -1, des, max, NULL, NULL) && max > 0)
		des[0] = 0;
	return des;
}

/* the whole UTF-8 file in UTF-16, to be freed (NULL: no file or empty) */
wchar_t *ant_file_text (const wchar_t *path) {

	HANDLE f;
	DWORD size, got = 0;
	char *bytes;
	wchar_t *text;
	int n;

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return NULL;
	size = GetFileSize(f, NULL);
	bytes = (size && size != INVALID_FILE_SIZE) ? malloc(size) : NULL;
	if (!bytes || !ReadFile(f, bytes, size, &got, NULL) || !got) {
		free(bytes);
		CloseHandle(f);
		return NULL;
	}
	CloseHandle(f);
	n = MultiByteToWideChar(CP_UTF8, 0, bytes, got, NULL, 0);
	text = malloc(sizeof(wchar_t) * (n + 1));
	if (text) {
		MultiByteToWideChar(CP_UTF8, 0, bytes, got, text, n);
		text[n] = 0;
	}
	free(bytes);
	return text;
}
