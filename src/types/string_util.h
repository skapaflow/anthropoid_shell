#ifndef ANT_TYPES_STRING_UTIL_H
#define ANT_TYPES_STRING_UTIL_H

#include <wchar.h>

extern char *to_narrowchar (char *, int, const wchar_t *);
extern wchar_t *ant_file_text (const wchar_t *);

#endif
