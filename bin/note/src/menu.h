#ifndef NOTE_MENU_H
#define NOTE_MENU_H

#include <wchar.h>
#include <stdbool.h>

extern void menu_draw_bar (int);
extern int  menu_run (int, bool);
extern int  menu_hotkey (wchar_t);
extern int  menu_at (int);

#endif
