#ifndef NOTE_SCREEN_H
#define NOTE_SCREEN_H

#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

/* event types */
enum { EV_KEY, EV_MOUSE, EV_MENU, EV_RESIZE, EV_END };

/* mouse actions */
enum { MOUSE_DOWN, MOUSE_UP, MOUSE_DRAG, MOUSE_DOUBLE, MOUSE_WHEEL };

/* cell kinds for cell_next */
enum { CELL_TEXT, CELL_TAB, CELL_CONTROL };

/* attribute bit: the unit is selected */
#define ATTR_SELECTED 0x80

typedef struct {
	int              type;
	KEY_EVENT_RECORD key;       /* EV_KEY */
	int              mouse;     /* EV_MOUSE: MOUSE_* */
	int              x, y;      /* EV_MOUSE: screen cell */
	int              wheel;     /* MOUSE_WHEEL: +1 up, -1 down */
	DWORD            mods;      /* control key state */
} EVENT;

extern int screen_w, screen_h;

extern bool screen_open (void);
extern void screen_close (void);
extern bool screen_update_size (void);
extern void screen_read (EVENT *);
extern bool screen_pending (void);
extern void screen_on_close (void (*) (void));

extern void out (const wchar_t *);
extern void out_n (const wchar_t *, int);
extern void out_char (wchar_t, int);
extern void out_goto (int, int);
extern void out_style (int, int);
extern void out_text (const wchar_t *, int, int, int, int);
extern void out_cells (const wchar_t *, int, const unsigned char *, int, int, int, int, bool);
extern void out_flush (void);

extern int  cell_next (const wchar_t *, int, int, int, int *, int *);
extern int  text_width (const wchar_t *, int);

extern void console_write (DWORD, const wchar_t *, ...);
extern const wchar_t *system_message (DWORD);

#endif
