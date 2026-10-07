#ifndef NOTE_SYNTAX_H
#define NOTE_SYNTAX_H

#include <wchar.h>

/* a language is 1 + its index in the table of syntax.c; 0 is plain text */
#define LANG_NONE 0

extern int syntax_detect (const wchar_t *);
extern const wchar_t *syntax_name (int);
extern int syntax_line (int, const wchar_t *, int, int, unsigned char *);

#endif
