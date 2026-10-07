#ifndef NOTE_UCD_H
#define NOTE_UCD_H

#include <wchar.h>
#include <stdint.h>

extern uint32_t ucd_code_point (const wchar_t *, int, int, int *);
extern int ucd_width (uint32_t);
extern int ucd_grapheme_next (const wchar_t *, int, int, int *);
extern int ucd_grapheme_prev (const wchar_t *, int, int);

#endif
