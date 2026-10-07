#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <windows.h>

#include "screen.h"
#include "config.h"
#include "buffer.h"
#include "ucd.h"

/*************************************************************
 * Console for note.                                         *
 *                                                           *
 *   - VT alternate screen (the shell screen comes back on   *
 *     exit), no auto wrap;                                  *
 *   - input from ReadConsoleInputW (VK + modifiers and      *
 *     mouse), without the system Ctrl+C and without the     *
 *     QuickEdit selection;                                  *
 *   - each frame is built in obuf and goes out in a single  *
 *     WriteConsoleW (no flicker);                           *
 *   - colors are 24-bit SGR taken from the theme.           *
 *************************************************************/

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef MOUSE_HWHEELED
#define MOUSE_HWHEELED 0x0008
#endif

int screen_w = 80, screen_h = 25;

static HANDLE   hin, hout;
static DWORD    in_mode, out_mode;
static bool     opened;
static int      window_x, window_y;   /* top left of the window in the buffer */
static wchar_t *obuf;
static int      olen, ocap;
static bool     legacy_cells;         /* the console gives each UTF-16 unit its own cell */

/* held by the main thread except while it waits for input, so on_close sees a whole edit */
static CRITICAL_SECTION lock;
static void (*on_close) (void);

/*
 * Ctrl+C and Ctrl+Break must not kill the editor while the alternate screen is
 * up. Closing the window (or the terminal tab) calls on_close first: the process
 * ends when this handler returns.
 */
static BOOL WINAPI ctrl_handler (DWORD type) {

	if (type == CTRL_CLOSE_EVENT) {
		if (on_close) {
			EnterCriticalSection(&lock);
			on_close();
		}
		return FALSE;
	}
	return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

void screen_on_close (void (*fn) (void)) {

	on_close = fn;
}

/* does the console join graphemes? "e" + U+0301 takes 1 cell (modern) or 2 (legacy conhost) */
static void probe_cells (void) {

	static const wchar_t probe[] = { L'e', 0x0301 };
	CONSOLE_SCREEN_BUFFER_INFO a, b;
	DWORD n;

	GetConsoleScreenBufferInfo(hout, &a);
	WriteConsoleW(hout, probe, 2, &n, NULL);
	GetConsoleScreenBufferInfo(hout, &b);
	legacy_cells = b.dwCursorPosition.X - a.dwCursorPosition.X != 1;
}

bool screen_open (void) {

	DWORD mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;

	hin  = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);

	if (!GetConsoleMode(hin, &in_mode) || !GetConsoleMode(hout, &out_mode))
		return false;

	/* without ENABLE_VIRTUAL_TERMINAL_PROCESSING the console is too old */
	if (!SetConsoleMode(hout, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
		return false;
	if (settings.mouse)
		mode |= ENABLE_MOUSE_INPUT;
	SetConsoleMode(hin, mode);
	InitializeCriticalSection(&lock);
	EnterCriticalSection(&lock);
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
	opened = true;

	out(L"\x1b[?1049h\x1b[?7l\x1b[H");
	out_flush();
	probe_cells();
	screen_update_size();
	return true;
}

void screen_close (void) {

	if (!opened)
		return;
	out(L"\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
	out_flush();
	FlushConsoleInputBuffer(hin);
	SetConsoleMode(hin, in_mode);
	SetConsoleMode(hout, out_mode);
	SetConsoleCtrlHandler(ctrl_handler, FALSE);
	LeaveCriticalSection(&lock);
	opened = false;
}

/* size of the visible window; true if it changed */
bool screen_update_size (void) {

	CONSOLE_SCREEN_BUFFER_INFO csbi;
	int w, h;

	if (!GetConsoleScreenBufferInfo(hout, &csbi))
		return false;
	window_x = csbi.srWindow.Left;
	window_y = csbi.srWindow.Top;
	w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	if (w == screen_w && h == screen_h)
		return false;
	screen_w = w;
	screen_h = h;
	return true;
}

static bool is_modifier (WORD vk) {

	return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL ||
		vk == VK_LWIN || vk == VK_RWIN || vk == VK_NUMLOCK || vk == VK_SCROLL;
}

/*
 * Next event. Key repeats come one by one; a left Alt pressed and released alone
 * becomes EV_MENU; mouse moves without a button are dropped.
 */
void screen_read (EVENT *ev) {

	static KEY_EVENT_RECORD last;
	static int repeat;
	static bool alt_alone;
	static DWORD buttons;
	INPUT_RECORD ir;
	DWORD n;

	memset(ev, 0, sizeof *ev);

	if (repeat > 0) {
		repeat--;
		ev->type = EV_KEY;
		ev->key = last;
		ev->mods = last.dwControlKeyState;
		return;
	}

	for (;;) {
		BOOL ok;

		if (opened)
			LeaveCriticalSection(&lock);
		ok = ReadConsoleInputW(hin, &ir, 1, &n);
		if (opened)
			EnterCriticalSection(&lock);
		if (!ok) {
			ev->type = EV_END;
			return;
		}
		if (n == 0)
			continue;

		switch (ir.EventType) {
		case WINDOW_BUFFER_SIZE_EVENT:
			ev->type = EV_RESIZE;
			return;

		case FOCUS_EVENT:
			alt_alone = false;
			continue;

		case KEY_EVENT: {
			KEY_EVENT_RECORD *k = &ir.Event.KeyEvent;

			if (k->wVirtualKeyCode == VK_MENU) {
				/* Alt + numeric keypad: the character arrives when Alt is released */
				if (!k->bKeyDown && k->uChar.UnicodeChar) {
					alt_alone = false;
					ev->type = EV_KEY;
					ev->key = *k;
					ev->key.bKeyDown = TRUE;
					ev->key.wVirtualKeyCode = 0;
					ev->key.dwControlKeyState = 0;
					return;
				}
				/* right Alt is AltGr: never the menu */
				if (k->bKeyDown)
					alt_alone = !(k->dwControlKeyState & (ENHANCED_KEY | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
				else if (alt_alone) {
					alt_alone = false;
					ev->type = EV_MENU;
					return;
				}
				continue;
			}
			if (!k->bKeyDown)
				continue;
			alt_alone = false;
			if (is_modifier(k->wVirtualKeyCode))
				continue;

			last = *k;
			repeat = (last.wRepeatCount ? last.wRepeatCount : 1) - 1;
			ev->type = EV_KEY;
			ev->key = last;
			ev->mods = last.dwControlKeyState;
			return;
		}

		case MOUSE_EVENT: {
			MOUSE_EVENT_RECORD *m = &ir.Event.MouseEvent;
			bool left = (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
			bool was  = (buttons & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

			alt_alone = false;
			ev->type = EV_MOUSE;
			ev->x = m->dwMousePosition.X - window_x;
			ev->y = m->dwMousePosition.Y - window_y;
			ev->mods = m->dwControlKeyState;

			if (m->dwEventFlags & MOUSE_HWHEELED)
				continue;
			if (m->dwEventFlags & MOUSE_WHEELED) {
				ev->mouse = MOUSE_WHEEL;
				ev->wheel = (short) HIWORD(m->dwButtonState) > 0 ? 1 : -1;
				return;
			}
			buttons = m->dwButtonState;
			if ((m->dwEventFlags & DOUBLE_CLICK) && left)
				ev->mouse = MOUSE_DOUBLE;
			else if (m->dwEventFlags & MOUSE_MOVED) {
				if (!left)
					continue;
				ev->mouse = MOUSE_DRAG;
			} else if (left && !was)
				ev->mouse = MOUSE_DOWN;
			else if (!left && was)
				ev->mouse = MOUSE_UP;
			else
				continue;
			return;
		}
		}
	}
}

/* is a key press or a mouse action waiting? (paste: do not redraw after every character) */
bool screen_pending (void) {

	INPUT_RECORD ir[64];
	DWORD n, i;

	if (!PeekConsoleInputW(hin, ir, 64, &n))
		return false;
	for (i = 0; i < n; i++) {
		if (ir[i].EventType == KEY_EVENT && ir[i].Event.KeyEvent.bKeyDown &&
				!is_modifier(ir[i].Event.KeyEvent.wVirtualKeyCode))
			return true;
		if (ir[i].EventType == MOUSE_EVENT && (ir[i].Event.MouseEvent.dwButtonState ||
				(ir[i].Event.MouseEvent.dwEventFlags & MOUSE_WHEELED)))
			return true;
	}
	return false;
}

void out_n (const wchar_t *s, int n) {

	if (n <= 0)
		return;
	if (olen + n > ocap) {
		while (olen + n > ocap)
			ocap = ocap ? ocap * 2 : 16384;
		obuf = xrealloc(obuf, ocap * sizeof(wchar_t));
	}
	memcpy(obuf + olen, s, n * sizeof(wchar_t));
	olen += n;
}

void out (const wchar_t *s) {

	out_n(s, (int) wcslen(s));
}

void out_char (wchar_t c, int count) {

	while (count-- > 0)
		out_n(&c, 1);
}

/* row and column from 0 */
void out_goto (int row, int col) {

	wchar_t seq[32];

	swprintf(seq, 32, L"\x1b[%d;%dH", row + 1, col + 1);
	out(seq);
}

/*
 * Foreground and attributes of style 'fg' over the background of style 'bg'.
 * An unset color falls back to the other style, then to the text style. The
 * selection may also force a foreground.
 */
void out_style (int fg, int bg) {

	const STYLE *f = &theme[fg], *b = &theme[bg];
	int fc = f->fg >= 0 ? f->fg : b->fg >= 0 ? b->fg : theme[S_TEXT].fg;
	int bc = b->bg >= 0 ? b->bg : f->bg >= 0 ? f->bg : theme[S_TEXT].bg;
	wchar_t seq[96];
	int n;

	if (bg == S_SELECTION && b->fg >= 0)
		fc = b->fg;

	n = swprintf(seq, 96, L"\x1b[0");
	if (fc >= 0)
		n += swprintf(seq + n, 96 - n, L";38;2;%d;%d;%d", fc >> 16, (fc >> 8) & 255, fc & 255);
	if (bc >= 0)
		n += swprintf(seq + n, 96 - n, L";48;2;%d;%d;%d", bc >> 16, (bc >> 8) & 255, bc & 255);
	if (f->bold)
		n += swprintf(seq + n, 96 - n, L";1");
	if (f->italic)
		n += swprintf(seq + n, 96 - n, L";3");
	if (f->underline)
		n += swprintf(seq + n, 96 - n, L";4");
	swprintf(seq + n, 96 - n, L"m");
	out(seq);
}

void out_flush (void) {

	DWORD written;

	if (olen > 0)
		WriteConsoleW(hout, obuf, (DWORD) olen, &written, NULL);
	olen = 0;
}

/*
 * Cell starting at s[i], at column 'col': returns the start of the next one, its
 * width in 'w' and its kind. A tab runs to the next tab stop; control characters
 * (C0, DEL, C1) and zero-width graphemes become one '?' cell (they never reach the
 * terminal raw, which would interpret them).
 */
int cell_next (const wchar_t *s, int n, int i, int col, int *w, int *kind) {

	wchar_t c = s[i];
	int next;

	if (c == L'\t') {
		*w = settings.tab_size - col % settings.tab_size;
		*kind = CELL_TAB;
		return i + 1;
	}
	if (c < 0x20 || (c >= 0x7f && c < 0xa0)) {
		*w = 1;
		*kind = CELL_CONTROL;
		return i + 1;
	}
	next = ucd_grapheme_next(s, n, i, w);
	*kind = CELL_TEXT;

	/* legacy conhost: each UTF-16 unit is one cell, a wide BMP character is two */
	if (legacy_cells) {
		int k, u;
		*w = 0;
		for (k = i; k < next; k += u)
			*w += (ucd_width(ucd_code_point(s, next, k, &u)) == 2 || u == 2) ? 2 : 1;
		return next;
	}
	if (*w == 0) {
		*w = 1;
		*kind = CELL_CONTROL;
	}
	return next;
}

/* columns taken by s[0..n) */
int text_width (const wchar_t *s, int n) {

	int i = 0, col = 0, w, kind;

	while (i < n) {
		i = cell_next(s, n, i, col, &w, &kind);
		col += w;
	}
	return col;
}

/*
 * Draws s[0..n) skipping its first 'skip' columns and filling exactly 'width'
 * columns (cut or padded with spaces). attr[i] (may be NULL) is the style of each
 * unit, with ATTR_SELECTED for the selection; 'fg' is the style without attr and
 * 'bg' the background. 'eol' paints one selected cell after the text (the line
 * break is selected). A wide cell cut at an edge becomes spaces.
 */
void out_cells (const wchar_t *s, int n, const unsigned char *attr, int skip, int width, int fg, int bg, bool eol) {

	int i = 0, col = 0, end = skip + width, w, kind, next;
	int cur_fg = -1, cur_bg = -1;

	while (i < n && col < end) {
		int f = attr ? attr[i] & ~ATTR_SELECTED : fg;
		int b = attr && (attr[i] & ATTR_SELECTED) ? S_SELECTION : bg;

		next = cell_next(s, n, i, col, &w, &kind);
		if (kind == CELL_CONTROL)
			f = S_CONTROL;
		/* a style with its own background (control, bracket) shows it, except in the selection */
		if (f != fg && b != S_SELECTION && theme[f].bg >= 0)
			b = f;
		if (col + w > skip && (f != cur_fg || b != cur_bg)) {
			out_style(f, b);
			cur_fg = f;
			cur_bg = b;
		}

		if (col + w <= skip)
			; /* before the visible part */
		else if (col < skip || col + w > end || kind == CELL_TAB) {
			int a = col < skip ? skip : col;
			int z = col + w > end ? end : col + w;
			out_char(L' ', z - a);
		} else if (kind == CELL_CONTROL)
			out_char(L'?', 1);
		else
			out_n(s + i, next - i);
		col += w;
		i = next;
	}
	if (col < skip)
		col = skip;
	if (eol && col < end && col >= skip) {
		out_style(fg, S_SELECTION);
		out_char(L' ', 1);
		col++;
	}
	out_style(fg, bg);
	out_char(L' ', end - col);
}

/* plain UI text in one style: 'width' columns, cut or padded */
void out_text (const wchar_t *s, int n, int width, int fg, int bg) {

	out_cells(s, n, NULL, 0, width, fg, bg, false);
}

/* text for the console (or UTF-8, when redirected) outside the editor screen */
void console_write (DWORD std, const wchar_t *fmt, ...) {

	static wchar_t buf[2048];
	static char u8[2048 * 3];
	HANDLE h = GetStdHandle(std);
	DWORD mode, n;
	va_list ap;

	va_start(ap, fmt);
	vswprintf(buf, 2048, fmt, ap);
	va_end(ap);

	if (GetConsoleMode(h, &mode))
		WriteConsoleW(h, buf, (DWORD) wcslen(buf), &n, NULL);
	else {
		int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof u8, NULL, NULL);
		if (len > 1)
			WriteFile(h, u8, len - 1, &n, NULL);
	}
}

/* Windows message for an error code (in the system language) */
const wchar_t *system_message (DWORD code) {

	static wchar_t msg[512];
	DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, code, 0, msg, 512, NULL);

	if (n == 0)
		swprintf(msg, 512, L"error %lu", code);
	else
		while (n > 0 && (msg[n-1] == L'\r' || msg[n-1] == L'\n' || msg[n-1] == L' ' || msg[n-1] == L'.'))
			msg[--n] = 0;
	return msg;
}
