#include <wchar.h>
#include <windows.h>

#include "clipboard.h"
#include "buffer.h"

/*************************************************************
 * Windows clipboard (CF_UNICODETEXT). Inside note lines are *
 * joined by '\n'; on the clipboard by "\r\n".               *
 *************************************************************/

bool clipboard_set (const wchar_t *s, int n) {

	HGLOBAL mem;
	wchar_t *p;
	int i, breaks = 0, k = 0;

	for (i = 0; i < n; i++)
		if (s[i] == L'\n')
			breaks++;

	mem = GlobalAlloc(GMEM_MOVEABLE, (n + breaks + 1) * sizeof(wchar_t));
	if (!mem)
		return false;
	p = GlobalLock(mem);
	for (i = 0; i < n; i++) {
		if (s[i] == L'\n')
			p[k++] = L'\r';
		p[k++] = s[i];
	}
	p[k] = 0;
	GlobalUnlock(mem);

	if (!OpenClipboard(NULL)) {
		GlobalFree(mem);
		return false;
	}
	EmptyClipboard();
	if (!SetClipboardData(CF_UNICODETEXT, mem)) {
		CloseClipboard();
		GlobalFree(mem);
		return false;
	}
	CloseClipboard();
	return true;
}

/* clipboard text with '\n' line breaks (free() it), or NULL */
wchar_t *clipboard_get (int *n) {

	HANDLE data;
	const wchar_t *p;
	wchar_t *t = NULL;
	int i, k = 0, len;

	*n = 0;
	if (!OpenClipboard(NULL))
		return NULL;
	data = GetClipboardData(CF_UNICODETEXT);
	if (data && (p = GlobalLock(data)) != NULL) {
		len = (int) wcslen(p);
		t = xrealloc(NULL, (len + 1) * sizeof(wchar_t));
		for (i = 0; i < len; i++) {
			if (p[i] == L'\r') {
				t[k++] = L'\n';
				if (p[i+1] == L'\n')
					i++;
			} else
				t[k++] = p[i];
		}
		t[k] = 0;
		*n = k;
		GlobalUnlock(data);
	}
	CloseClipboard();
	return t;
}
