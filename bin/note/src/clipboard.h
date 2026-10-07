#ifndef NOTE_CLIPBOARD_H
#define NOTE_CLIPBOARD_H

#include <wchar.h>
#include <stdbool.h>

extern bool clipboard_set (const wchar_t *, int);
extern wchar_t *clipboard_get (int *);

#endif
