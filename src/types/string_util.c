#include <windows.h>

#include "string_util.h"

/* UTF-16 to UTF-8 in des, of max bytes; an empty string if it does not fit */
char *to_narrowchar (char *des, int max, const wchar_t *wstr) {

	if (!WideCharToMultiByte(CP_UTF8, 0, wstr, -1, des, max, NULL, NULL) && max > 0)
		des[0] = 0;
	return des;
}
