#ifndef ANT_CONSOLE_CONSOLE_H
#define ANT_CONSOLE_CONSOLE_H

#include <wchar.h>
#include <stdbool.h>

#define CLEAR 0x8
#define BLUE 1
#define GREEN 2
#define CYAN 3
#define RED 4
#define PURPLE 5
#define YELLOW 6
#define WHITE 7
#define GRAY 8

#define _CLEAR 0x80
#define _BLUE 17
#define _GREEN 34
#define _CYAN 51
#define _RED 68
#define _PURPLE 85
#define _YELLOW 102
#define _WHITE 119
#define _GRAY 136

extern int get_console_width (void);
extern void set_cursor_type (bool, bool);
extern int print (int, const char *, ...);
extern void ant_error (const wchar_t *, ...);
extern void ant_write (const wchar_t *);
extern void print_big_text (const char *, const char *, int);
extern void ant_logo (int);

#endif