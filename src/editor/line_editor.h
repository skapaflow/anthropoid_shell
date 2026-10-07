#ifndef ANT_EDITOR_LINE_EDITOR_H
#define ANT_EDITOR_LINE_EDITOR_H

#include <wchar.h>

extern void ant_editor_read_line (const char *);
extern void ant_editor_end (void);
extern int ant_grapheme (const wchar_t *, int, int, int *);

#endif