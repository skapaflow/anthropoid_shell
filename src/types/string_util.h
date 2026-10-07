#ifndef ANT_TYPES_STRING_UTIL_H
#define ANT_TYPES_STRING_UTIL_H

#include <wchar.h>
#include <stdbool.h>

extern bool isnumbers (char *);
extern char *strsep (char **, const char *);
extern char *strrmc (char *, const char *);
extern void trim (char *);
extern char *strtrim (char *, const char *);
extern char *float_trim (char *);
extern char *to_narrowchar (char *, const wchar_t *);
extern wchar_t *to_widechar (wchar_t *, const char *);

#endif