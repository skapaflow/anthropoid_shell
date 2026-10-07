#ifndef NOTE_DIALOG_H
#define NOTE_DIALOG_H

#include <wchar.h>
#include <stdbool.h>

/* where a question box goes (so it does not hide the text being asked about) */
enum { PLACE_CENTER, PLACE_TOP, PLACE_BOTTOM };

extern int  dialog_ask (const wchar_t *, const wchar_t *, const wchar_t **, int, int);
extern bool dialog_form (const wchar_t *, const wchar_t **, wchar_t **, int, bool);
extern void dialog_info (const wchar_t *, const wchar_t **, int);
extern void draw_box (int, int, int, int, int, const wchar_t *);
extern void out_label (const wchar_t *, int, int, int);
extern int  label_width (const wchar_t *);
extern wchar_t label_hotkey (const wchar_t *);

#endif
