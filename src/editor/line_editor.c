#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

#include "complete.h"
#include "line_editor.h"
#include "history.h"
#include "../builtins/cd.h"
#include "../console/console.h"
#include "../types/string_util.h"
#include "../types/unicode.h"
#include "../shell/shell.h"

/*************************************************************
 * AntShell line editor.                                     *
 *                                                           *
 * Modeled on conhost's COOKED_READ_DATA (Windows            *
 * Terminal, src/host/readDataCooked.cpp):                   *
 *   - the line is UTF-16 (line/len/cursor);                 *
 *   - every edit goes through ant_replace();                *
 *   - only ant_redisplay() draws, and only when 'dirty';    *
 *   - keys come from ReadConsoleInputW (VK + modifiers).    *
 * On Enter the line becomes UTF-8 in char_line_buf.         *
 *************************************************************/

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

#define IS_LOW(c)  ((c) >= 0xDC00 && (c) <= 0xDFFF)

#define LINE_UNITS  (ANTMAX - 1) /* limit in UTF-16 units */
#define POPUP_ROWS  20           /* maximum height of the F7 list */
#define POPUP_UNITS 480          /* UTF-16 units per popup line */
#define PAGER_ROWS  100          /* maximum pager rows drawn */
#define PAGER_UNITS 1000         /* UTF-16 units per pager row */
#define DRAW_MAX    (ANTMAX + 64 + (PAGER_ROWS + 2) * (PAGER_UNITS + 64))

/* TAB pager, like fish's (src/pager.rs) */
#define PAGER_MAX_COLS  6  /* maximum columns */
#define PAGER_MIN_ROWS  4  /* rows shown before the 2nd TAB, at least */
#define PAGER_MIN_WIDTH 16 /* a console narrower than this does not show the pager */

enum { INPUT_KEEP, INPUT_ACCEPT, INPUT_CANCEL };

/* history popups (as in conhost) and the TAB pager */
enum { POPUP_NONE, POPUP_COPY_TO, POPUP_COPY_FROM, POPUP_NUMBER, POPUP_LIST, POPUP_DIRS, POPUP_PAGER };

/* selection directions in the pager */
enum { SEL_NEXT, SEL_PREV, SEL_NORTH, SEL_SOUTH, SEL_EAST, SEL_WEST, SEL_PAGE_NORTH, SEL_PAGE_SOUTH };

static int     popup = POPUP_NONE;
static wchar_t number[6];     /* F9: up to 5 digits */
static int     number_len;
static int     list_selected; /* F7 and the folder menu (left-cd) */
static wchar_t leftcd[ANTMAX]; /* left-cd: the command that goes to the history and the shell, not to the line */
static bool    leftcd_run;
static int     list_top;
static int     list_height;

static ANT_COMPLETE pager;              /* TAB options */
static int    *pager_widths;            /* columns of each option */
static wchar_t pager_line[ANTMAX];      /* the line when the pager opened (Esc and selection changes go back to it) */
static int     pager_len, pager_cursor;
static int     pager_selected = -1;     /* -1: none */
static int     pager_rows = 1;          /* grid rows in the last drawing */
static int     pager_visible = 1;       /* rows shown in the last drawing */
static int     pager_top;               /* first row shown */
static int     pager_remaining;         /* rows hidden until the 2nd TAB */
static bool    pager_disclosed;         /* 2nd TAB: shows everything */

static int     flash_beg = -1, flash_end = -1; /* span that flashes (TAB with no options) */

static wchar_t line[ANTMAX];
static int     len         = 0;
static int     cursor      = 0;
static bool    dirty       = false;
static bool    insert_mode = true;
static COORD   origin;          /* position right after the prompt */
static int     cell_mode   = -1; /* 0: one cell per UTF-16 unit (legacy conhost), 1: per grapheme */
static HANDLE  hin, hout;

static void write_wide (const wchar_t *s, int n) {

	DWORD written;
	WriteConsoleW(hout, s, (DWORD) n, &written, NULL);
}

/* grapheme of s[0..n) at 'pos': returns the start of the next one and its columns in the current console */
int ant_grapheme (const wchar_t *s, int n, int pos, int *width) {

	int u, w, next = ucd_grapheme_next(s, n, pos, &w);

	/* legacy conhost: each UTF-16 unit is one cell, a wide BMP character is two */
	if (cell_mode == 0) {
		w = 0;
		for (int i = pos; i < next; i += u)
			w += (ucd_width(ucd_code_point(s, next, i, &u)) == 2 || u == 2) ? 2 : 1;
	}

	if (width)
		*width = w;
	return next;
}

static int grapheme_next (int pos) {

	return ucd_grapheme_next(line, len, pos, NULL);
}

static int grapheme_prev (int pos) {

	return ucd_grapheme_prev(line, len, pos);
}

/* 1: space, 2: path/punctuation delimiter, 0: word */
static int delimiter_class (wchar_t c) {

	if (c == L' ')
		return 1;
	if (wcschr(L"\\/.,;:|\"'", c))
		return 2;
	return 0;
}

/* conhost's _wordPrev/_wordNext: "skip 1, skip spaces, skip the same class" */
static int word_prev (int pos) {

	if (pos > 0) {
		pos--;
		while (pos > 0 && line[pos] == L' ')
			pos--;
		int dc = delimiter_class(line[pos]);
		while (pos > 0 && delimiter_class(line[pos-1]) == dc)
			pos--;
	}
	return pos;
}

static int word_next_in (const wchar_t *s, int n, int pos) {

	if (pos < n) {
		pos++;
		int dc = delimiter_class(s[pos-1]);
		while (pos < n && delimiter_class(s[pos]) == dc)
			pos++;
		while (pos < n && s[pos] == L' ')
			pos++;
	}
	return pos;
}

static int word_next (int pos) {

	return word_next_in(line, len, pos);
}

static int utf8_size (const wchar_t *s, int n) {

	return n > 0 ? WideCharToMultiByte(CP_UTF8, 0, s, n, NULL, 0, NULL, NULL) : 0;
}

/* the only function that changes the line: replaces line[off..off+remove) with ins[0..count) */
static bool ant_replace (int off, int remove, const wchar_t *ins, int count) {

	if (off > len)
		off = len;
	if (remove > len - off)
		remove = len - off;
	if (remove == 0 && count == 0)
		return true;

	/* the result must fit in char_line_buf as UTF-8 */
	if (len - remove + count > LINE_UNITS ||
		utf8_size(line, len) - utf8_size(line + off, remove) + utf8_size(ins, count) > ANTMAX - 1)
		return false;

	memmove(line + off + count, line + off + remove, sizeof(wchar_t) * (len - off - remove));
	if (count)
		memcpy(line + off, ins, sizeof(wchar_t) * count);
	len += count - remove;
	line[len] = 0;
	cursor = off + count;
	dirty = true;
	return true;
}

static void ant_set_cursor (int pos) {

	cursor = (pos < 0 ? 0 : (pos > len ? len : pos));
	dirty = true;
}

/* replaces the whole line (history) and puts the cursor at the end */
static void ant_replace_all (const wchar_t *s) {

	ant_replace(0, len, s, wcslen(s));
	ant_set_cursor(len);
}

/*
 * History autosuggestion, like fish's (reader.rs, autosuggest): a command
 * that starts with the line (ignoring case) shows up in gray after the
 * cursor; the best frecency of data\history_rank.ant (points weighed by the
 * time since the last use) first, then the newest in the history. → and End accept all of it, Ctrl+→ one word. Only with the
 * cursor at the end, no popup and some text in the line.
 */
static bool suggest_on = false; /* turned off for the final drawing of the line */

static bool suggests (const wchar_t *h) {

	int hn = wcslen(h), p = 0;

	if (hn <= len || _wcsnicmp(h, line, len))
		return false;
	/* the suggestion starts at a whole grapheme (never suggests just a combining accent) */
	while (p < len)
		p = ucd_grapheme_next(h, hn, p, NULL);
	return p == len;
}

static const wchar_t *suggestion (void) {

	if (!suggest_on || popup != POPUP_NONE || cursor < len || (int) wcsspn(line, L" ") == len)
		return NULL;

	const wchar_t *h = ant_history_rank_find(suggests);

	if (h)
		return h;
	for (int i = ant_history_count() - 1; i >= 0; i--)
		if (suggests(ant_history_get(i)))
			return ant_history_get(i);
	return NULL;
}

/* puts the suggestion (or only its next word) into the line; false if there was no suggestion */
static bool accept_suggestion (bool one_word) {

	const wchar_t *s = suggestion();
	int sn;

	if (!s)
		return false;
	sn = wcslen(s);
	ant_replace(len, 0, s + len, (one_word ? word_next_in(s, sn, len) : sn) - len);
	return true;
}

/* appends a popup line: "\r\n", text cut at width-1 columns; returns the columns */
static int popup_line (wchar_t *out, int *n, const wchar_t *text, bool reverse, int width) {

	int tn = wcslen(text), cols = 0, units = 0;

	wcscpy(out + *n, reverse ? L"\r\n\x1b[K\x1b[7m" : L"\r\n\x1b[K");
	*n += wcslen(out + *n);

	for (int i = 0; i < tn; ) {
		int w, next = ant_grapheme(text, tn, i, &w);
		if (cols + w > width - 1 || units + (next - i) > POPUP_UNITS)
			break;
		cols += w;
		units += next - i;
		while (i < next)
			out[(*n)++] = text[i++];
	}

	if (reverse) {
		/* selection bar up to the end of the line */
		while (cols < width - 1 && units < POPUP_UNITS) {
			out[(*n)++] = L' ';
			cols++;
			units++;
		}
		wcscpy(out + *n, L"\x1b[m");
		*n += 3;
	}
	return cols;
}

/* F7: history list (or, on an empty line, the folder menu) with a scroll bar; returns the columns of the last line */
static int popup_list (wchar_t *out, int *n, int *rows, int width, int height) {

	bool dirs = (popup == POPUP_DIRS);
	int total = dirs ? ant_cd_count() : ant_history_count();
	int digits = 1, cols = 0;
	wchar_t text[POPUP_UNITS + 32];

	for (int t = total; t >= 10; t /= 10)
		digits++;

	list_height = total;
	if (list_height > height / 2 - 1)
		list_height = height / 2 - 1;
	if (list_height > POPUP_ROWS)
		list_height = POPUP_ROWS;
	if (list_height < 1)
		list_height = 1;

	if (list_selected < 0)
		list_selected = 0;
	if (list_selected > total - 1)
		list_selected = total - 1;
	if (list_top < 0)
		list_top = list_selected - list_height / 2;
	if (list_top > list_selected)
		list_top = list_selected;
	if (list_top < list_selected - list_height + 1)
		list_top = list_selected - list_height + 1;
	if (list_top > total - list_height)
		list_top = total - list_height;
	if (list_top < 0)
		list_top = 0;

	int track = (total <= 1) ? 0 : 1 + ((list_height - 3) * list_selected + (total - 1) / 2) / (total - 1);

	for (int off = 0; off < list_height && list_top + off < total; off++) {
		int index = list_top + off;
		wchar_t bar = L' ';
		if (total > list_height)
			bar = (off == 0) ? 0x25B2 : (off == list_height - 1) ? 0x25BC : (off == track) ? 0x2588 : 0x2592;
		if (dirs)
			/* the last folder is the current one, marked with '*' */
			swprintf(text, POPUP_UNITS + 32, L"%lc%lc%lc %.*ls", bar, index == list_selected ? 0x25BA : L' ',
				index == total - 1 ? L'*' : L' ', POPUP_UNITS, ant_cd_get(index));
		else
			swprintf(text, POPUP_UNITS + 32, L"%lc%lc%*d: %.*ls", bar, index == list_selected ? 0x25BA : L' ',
				digits, index, POPUP_UNITS, ant_history_get(index));
		cols = popup_line(out, n, text, index == list_selected, width);
		(*rows)++;
	}
	return cols;
}

/* appends s[0..n) to out, within the limit of one pager row */
static void pager_put (wchar_t *out, int *n, int *units, const wchar_t *s, int count) {

	if (*units + count > PAGER_UNITS)
		count = PAGER_UNITS - *units;
	if (count <= 0)
		return;
	wmemcpy(out + *n, s, count);
	*n += count;
	*units += count;
}

static void pager_spaces (wchar_t *out, int *n, int *units, int count) {

	while (count-- > 0 && *units < PAGER_UNITS) {
		out[(*n)++] = L' ';
		(*units)++;
	}
}

/*
 * Pager grid, like fish's completion_try_print: up to 6 columns filled top to
 * bottom, each column as wide as its longest option, 1 space between columns;
 * before the 2nd TAB it shows at most half the screen (at least 4 rows).
 * Returns how many rows it added below the command line.
 */
static int pager_draw (wchar_t *out, int *n, int width, int height, int line_rows) {

	int count = pager.count, term_width = width - 1, cols = 1, rows = count;
	int col_width[PAGER_MAX_COLS];
	int drawn = 0;

	if (term_width < PAGER_MIN_WIDTH)
		return 0;

	/* more columns first, until it fits the width */
	for (int c = (count < PAGER_MAX_COLS ? count : PAGER_MAX_COLS); c >= 1; c--) {
		int r = (count + c - 1) / c, total = c - 1;
		for (int k = 0; k < c; k++) {
			col_width[k] = 0;
			for (int i = k * r; i < (k + 1) * r && i < count; i++)
				if (pager_widths[i] > col_width[k])
					col_width[k] = pager_widths[i];
			if (col_width[k] > term_width)
				col_width[k] = term_width;
			total += col_width[k];
		}
		cols = c;
		rows = r;
		if (total <= term_width)
			break;
	}
	/* recomputes the widths of the final choice (the loop may have ended at c = 1) */
	for (int k = 0; k < cols; k++) {
		col_width[k] = 0;
		for (int i = k * rows; i < (k + 1) * rows && i < count; i++)
			if (pager_widths[i] > col_width[k])
				col_width[k] = pager_widths[i];
		if (col_width[k] > term_width)
			col_width[k] = term_width;
	}

	/* height: the screen minus the command line and the progress line */
	int avail = height - line_rows - 1;
	if (avail > PAGER_ROWS)
		avail = PAGER_ROWS;
	if (!pager_disclosed) {
		int half = (avail / 2 > PAGER_MIN_ROWS) ? avail / 2 : PAGER_MIN_ROWS;
		if (half < avail)
			avail = half;
	}
	if (avail < 1)
		avail = 1;

	pager_remaining = (!pager_disclosed && rows > avail) ? rows - avail : 0;
	/* only one hidden row: show it instead of spending the notice line */
	if (pager_remaining == 1) {
		avail++;
		pager_remaining = 0;
	}

	pager_rows = rows;
	pager_visible = (rows < avail) ? rows : avail;

	/* keeps the selection visible */
	if (pager_selected >= 0) {
		int sel_row = pager_selected % rows;
		if (sel_row < pager_top)
			pager_top = sel_row;
		if (sel_row >= pager_top + pager_visible)
			pager_top = sel_row - pager_visible + 1;
	}
	if (pager_top > rows - pager_visible)
		pager_top = rows - pager_visible;
	if (pager_top < 0)
		pager_top = 0;

	int highlight = ant_complete_is_prefix(&pager) ? pager.prefix : 0;

	for (int r = pager_top; r < pager_top + pager_visible; r++) {
		int units = 0;
		pager_put(out, n, &units, L"\r\n\x1b[K", 5);
		for (int k = 0; k < cols; k++) {
			int i = k * rows + r;
			if (i >= count)
				break;
			const wchar_t *name = pager.items[i].name;
			int nlen = wcslen(name), used = 0;
			bool truncate = pager_widths[i] > col_width[k];

			if (k > 0)
				pager_spaces(out, n, &units, 1);
			{
				/* attributes 241 (blue on white) and 27 (bright cyan on blue) of the old print() */
				const wchar_t *color = (i == pager_selected) ? L"\x1b[34;107m" : L"\x1b[96;44m";
				pager_put(out, n, &units, color, wcslen(color));
			}

			/* common prefix in bold underline, like fish_pager_color_prefix */
			for (int j = 0; j < nlen; ) {
				int w, next = ant_grapheme(name, nlen, j, &w);
				if (used + w > col_width[k] - (truncate ? 1 : 0))
					break;
				if (j == 0 && highlight > 0)
					pager_put(out, n, &units, L"\x1b[1;4m", 6);
				pager_put(out, n, &units, name + j, next - j);
				if (next >= highlight && j < highlight)
					pager_put(out, n, &units, L"\x1b[22;24m", 8);
				used += w;
				j = next;
			}
			if (truncate) {
				pager_put(out, n, &units, L"\x2026", 1);
				used++;
			}
			pager_spaces(out, n, &units, col_width[k] - used);
			pager_put(out, n, &units, L"\x1b[m", 3);
		}
		drawn++;
	}

	/* progress line, like fish's */
	if (pager_remaining > 1 || pager_top > 0 || pager_top + pager_visible < rows) {
		wchar_t text[96];
		int units = 0;
		if (pager_remaining > 1)
			swprintf(text, 96, L"\x2026" L"and %d more rows", pager_remaining);
		else
			swprintf(text, 96, L"rows %d to %d of %d", pager_top + 1, pager_top + pager_visible, rows);
		pager_put(out, n, &units, L"\r\n\x1b[K\x1b[97;46m", 13);
		pager_put(out, n, &units, text, wcslen(text));
		pager_put(out, n, &units, L"\x1b[m", 3);
		drawn++;
	}

	return drawn;
}

/* draws the line from 'origin' and places the cursor */
static void ant_redisplay (void) {

	static wchar_t out[DRAW_MAX];
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	int n = 0, row = 0, col = origin.X, cur_row = 0, cur_col = origin.X;

	if (!dirty)
		return;
	dirty = false;

	GetConsoleScreenBufferInfo(hout, &csbi);
	int width = csbi.dwSize.X;
	int height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

	wcscpy(out, L"\x1b[?25l");
	n = wcslen(out);

	for (int i = 0; i < len; ) {
		int w, next = ant_grapheme(line, len, i, &w);
		/* does not fit: wrap (a wide character at the last column leaves an empty cell) */
		if (col + w > width) {
			row++;
			col = 0;
		}
		if (cursor >= i && cursor < next) {
			cur_row = row;
			cur_col = col;
		}
		col += w;
		/* TAB with no options: the span flashes in reverse video */
		if (i == flash_beg) {
			wcscpy(out + n, L"\x1b[7m");
			n += 4;
		}
		while (i < next)
			out[n++] = line[i++];
		if (i == flash_end) {
			wcscpy(out + n, L"\x1b[27m");
			n += 5;
		}
	}

	if (cursor >= len) {
		cur_row = row;
		cur_col = col;
	}

	/* autosuggestion in gray up to the second-to-last column (does not wrap the line, as in fish);
	   what does not fit becomes an ellipsis */
	const wchar_t *sug = suggestion();
	if (sug && col < width - 1) {
		int sn = wcslen(sug), room = width - 1 - col, total = 0, used = 0, fit = len, w;

		for (int i = len; i < sn; total += w)
			i = ant_grapheme(sug, sn, i, &w);
		if (total > room)
			room--; /* room for the ellipsis */
		for (int i = len, next; i < sn; i = next) {
			next = ant_grapheme(sug, sn, i, &w);
			if (used + w > room)
				break;
			used += w;
			fit = next;
		}
		wcscpy(out + n, L"\x1b[90m");
		n += 5;
		wmemcpy(out + n, sug + len, fit - len);
		n += fit - len;
		if (fit < sn)
			out[n++] = 0x2026;
		wcscpy(out + n, L"\x1b[39m");
		n += 5;
	}

	/* popup below: first erase the rest of the line (the line may have shrunk);
	   not at the edge, because the cursor is still over the last character */
	if (popup != POPUP_NONE && col < width) {
		wcscpy(out + n, L"\x1b[K");
		n += 3;
	}

	if (popup == POPUP_PAGER) {
		/* pager below the line; the cursor stays on the command line */
		if (col >= width) {
			out[n++] = L' ';
			out[n++] = L'\r';
			row++;
			col = 0;
		}
		row += pager_draw(out, &n, width, height, row + 1);
	} else if (popup != POPUP_NONE) {
		/* popups below the line ("\r" also undoes the deferred wrap); the cursor stays hidden at the end */
		wchar_t text[64];
		switch (popup) {
			case POPUP_COPY_TO:
				col = popup_line(out, &n, L"Enter char to copy up to: ", true, width);
				row++;
				break;
			case POPUP_COPY_FROM:
				col = popup_line(out, &n, L"Enter char to delete up to: ", true, width);
				row++;
				break;
			case POPUP_NUMBER:
				swprintf(text, 64, L"Enter command number: %ls", number);
				col = popup_line(out, &n, text, true, width);
				row++;
				break;
			case POPUP_LIST:
			case POPUP_DIRS:
				col = popup_list(out, &n, &row, width, height);
				break;
		}
		cur_row = row;
		cur_col = col;
	} else if (col >= width) {
		/* text ended exactly at the edge: forces the VT deferred wrap */
		out[n++] = L' ';
		out[n++] = L'\r';
		row++;
		col = 0;
	}

	/* erases what is left of the previous drawing */
	wcscpy(out + n, L"\x1b[J");
	n += 3;

	SetConsoleCursorPosition(hout, origin);
	write_wide(out, n);

	/* if the screen scrolled, the origin moved up with it */
	GetConsoleScreenBufferInfo(hout, &csbi);
	if (csbi.dwCursorPosition.Y != origin.Y + row)
		origin.Y = csbi.dwCursorPosition.Y - row;

	if (cur_col >= width) {
		cur_row++;
		cur_col = 0;
	}

	COORD pos = { (SHORT) cur_col, (SHORT) (origin.Y + cur_row) };
	SetConsoleCursorPosition(hout, pos);
	if (popup == POPUP_NONE || popup == POPUP_PAGER)
		write_wide(L"\x1b[?25h", 6);
}

/* measures whether the console joins graphemes: "e" + U+0301 takes 1 cell (modern) or 2 (legacy) */
static void probe_cell_mode (void) {

	static const wchar_t probe[] = { L'e', 0x0301 };
	CONSOLE_SCREEN_BUFFER_INFO a, b;

	if (cell_mode >= 0)
		return;

	GetConsoleScreenBufferInfo(hout, &a);
	write_wide(probe, 2);
	GetConsoleScreenBufferInfo(hout, &b);
	cell_mode = (b.dwCursorPosition.X - a.dwCursorPosition.X == 1);
	SetConsoleCursorPosition(hout, a.dwCursorPosition);
	write_wide(L"\x1b[K", 3);
}

/*
 * Terminal integration (FinalTerm / OSC 133 marks, Windows Terminal's
 * doc/specs/#11000; VS Code understands the same ones):
 *   133;A  prompt start           9;9;<folder>  current folder (new tab in the same place)
 *   133;B  command start          133;C         output start
 *   133;D;<code>  command end, with the exit code
 * conhost without Windows Terminal just swallows the sequences.
 */
static bool command_pending = false; /* 133;C was written and 133;D is missing */

static void mark_command_done (void) {

	wchar_t mark[32];

	if (!command_pending)
		return;
	swprintf(mark, 32, L"\x1b]133;D;%d\x1b\\", ant_status);
	write_wide(mark, wcslen(mark));
	command_pending = false;
}

/* called when the shell exits: closes the mark of the last command */
void ant_editor_end (void) {

	DWORD mode;

	if (!command_pending)
		return;
	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!GetConsoleMode(hout, &mode))
		return;
	SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	mark_command_done();
	SetConsoleMode(hout, mode);
}

/* the prompt: just '>' */
static void write_prompt (void) {

	fputs(">", stdout);
	fflush(stdout);
}

static void print_prompt (void) {

	static wchar_t cwd[MAX_PATH * 4], mark[MAX_PATH * 4 + 16];
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD n;

	mark_command_done();
	write_wide(L"\x1b]133;A\x1b\\", 9);
	n = GetCurrentDirectoryW(MAX_PATH * 4, cwd);
	if (n > 0 && n < MAX_PATH * 4) {
		swprintf(mark, MAX_PATH * 4 + 16, L"\x1b]9;9;%ls\x1b\\", cwd);
		write_wide(mark, wcslen(mark));
	}

	write_prompt();
	write_wide(L"\x1b]133;B\x1b\\", 9);

	GetConsoleScreenBufferInfo(hout, &csbi);
	origin = csbi.dwCursorPosition;
}

static void insert_char (wchar_t ch) {

	int remove = 0;

	/* overwrite replaces the whole grapheme; the 2nd half of a surrogate pair only completes the 1st */
	if (!insert_mode && !IS_LOW(ch))
		remove = grapheme_next(cursor) - cursor;

	ant_replace(cursor, remove, &ch, 1);
}

/* TAB with no options: flashes the word (or the whole line, if the word is empty) */
static void flash (int beg, int end) {

	if (beg >= end) {
		beg = 0;
		end = len;
	}
	if (beg >= end)
		return;

	flash_beg = beg;
	flash_end = end;
	dirty = true;
	ant_redisplay();
	Sleep(120);
	flash_beg = flash_end = -1;
	dirty = true;
	ant_redisplay();
}

static void pager_close (void) {

	if (popup != POPUP_PAGER)
		return;
	ant_complete_free(&pager);
	free(pager_widths);
	pager_widths = NULL;
	popup = POPUP_NONE;
	dirty = true;
}

/* goes back to the line from when the pager opened and puts in the selected option (a transient edit) */
static void pager_apply_selection (void) {

	wchar_t text[ANTMAX];

	wmemcpy(line, pager_line, pager_len + 1);
	len = pager_len;
	cursor = pager_cursor;
	dirty = true;

	if (pager_selected >= 0) {
		int n = ant_complete_text(&pager, pager_selected, ANT_APPLY_SELECT, text, ANTMAX);
		if (n)
			ant_replace(pager.beg, pager.end - pager.beg, text, n);
	}
}

static void pager_move (int dir) {

	int count = pager.count, rows = pager_rows, s = pager_selected;
	int row = (s >= 0) ? s % rows : 0, col = (s >= 0) ? s / rows : 0;

	/* navigating shows the whole list */
	pager_disclosed = true;

	if (s < 0)
		s = (dir == SEL_PREV || dir == SEL_NORTH || dir == SEL_WEST || dir == SEL_PAGE_NORTH) ? count - 1 : 0;
	else
		switch (dir) {
			case SEL_NEXT:
			case SEL_SOUTH:
				s = (s + 1) % count;
				break;
			case SEL_PREV:
				s = (s + count - 1) % count;
				break;
			case SEL_NORTH:
				/* ↑ on the first item ends the navigation */
				s = (s == 0) ? -1 : s - 1;
				break;
			case SEL_EAST:
				if (s + rows < count)
					s += rows;
				break;
			case SEL_WEST:
				if (s - rows >= 0)
					s -= rows;
				break;
			case SEL_PAGE_SOUTH:
				row = (row + pager_visible < rows) ? row + pager_visible : rows - 1;
				s = col * rows + row;
				if (s >= count)
					s = count - 1;
				break;
			case SEL_PAGE_NORTH:
				row = (row - pager_visible > 0) ? row - pager_visible : 0;
				s = col * rows + row;
				break;
		}

	pager_selected = s;
	pager_apply_selection();
}

/*
 * TAB, like fish (reader.rs, handle_completions):
 *   no options: flash; one: insert it (a directory with its separator, a file
 *   with a space); several: insert the common prefix, open the pager and,
 *   unlike fish, which only shows it, select the 1st option right away. With
 *   the pager open, TAB walks through the options (and shows the rest of the
 *   list, if it was partial).
 */
static void tab_key (bool back) {

	wchar_t text[ANTMAX];
	int count, n;

	if (popup == POPUP_PAGER) {
		if (!pager_disclosed && pager_remaining > 0 && pager_selected < 0 && !back) {
			pager_disclosed = true;
			dirty = true;
		} else
			pager_move(back ? SEL_PREV : SEL_NEXT);
		return;
	}

	count = ant_complete(line, cursor, &pager);

	if (count == 0) {
		ant_complete_free(&pager);
		flash(pager.beg, pager.end);
		return;
	}

	if (count == 1) {
		n = ant_complete_text(&pager, 0, ANT_APPLY_UNIQUE, text, ANTMAX);
		if (n)
			ant_replace(pager.beg, pager.end - pager.beg, text, n);
		ant_complete_free(&pager);
		return;
	}

	/* a common prefix longer than what was typed goes into the line */
	if (pager.prefix > (int) wcslen(pager.word)) {
		n = ant_complete_text(&pager, -1, ANT_APPLY_PREFIX, text, ANTMAX);
		if (n && ant_replace(pager.beg, pager.end - pager.beg, text, n))
			pager.end = pager.beg + n;
	}

	pager_widths = malloc(sizeof(int) * count);
	if (!pager_widths) {
		ant_complete_free(&pager);
		return;
	}
	for (int i = 0; i < count; i++) {
		const wchar_t *name = pager.items[i].name;
		int nlen = wcslen(name), w;
		pager_widths[i] = 0;
		for (int j = 0; j < nlen; pager_widths[i] += w)
			j = ant_grapheme(name, nlen, j, &w);
	}

	wmemcpy(pager_line, line, len + 1);
	pager_len = len;
	pager_cursor = cursor;
	pager_top = 0;
	pager_remaining = 0;
	pager_disclosed = false;
	popup = POPUP_PAGER;

	/* selects the 1st option right away: the whole name goes into the line and the menu stays open
	   (Esc goes back to the line with the common prefix) */
	pager_selected = 0;
	pager_apply_selection();
}

/*
 * A key with the pager open. Returns -1 when the key must take the normal
 * path (the pager is already closed, keeping the selection in the line).
 */
static int pager_key (const KEY_EVENT_RECORD *k) {

	bool shift = (k->dwControlKeyState & SHIFT_PRESSED) != 0;
	bool navigating = pager_selected >= 0;

	switch (k->wVirtualKeyCode) {
		case VK_TAB:
			tab_key(shift);
			return INPUT_KEEP;
		case VK_DOWN:
			pager_move(SEL_SOUTH);
			return INPUT_KEEP;
		case VK_UP:
			pager_move(SEL_NORTH);
			return INPUT_KEEP;
		case VK_LEFT:
		case VK_RIGHT:
			if (!navigating)
				break;
			pager_move(k->wVirtualKeyCode == VK_LEFT ? SEL_WEST : SEL_EAST);
			return INPUT_KEEP;
		case VK_PRIOR:
		case VK_NEXT:
			if (!navigating)
				break;
			pager_move(k->wVirtualKeyCode == VK_PRIOR ? SEL_PAGE_NORTH : SEL_PAGE_SOUTH);
			return INPUT_KEEP;
		case VK_RETURN:
			/* navigating: Enter only accepts the option; otherwise it closes and runs */
			pager_close();
			return navigating ? INPUT_KEEP : INPUT_ACCEPT;
		case VK_ESCAPE:
			/* undoes the selection and closes */
			if (navigating) {
				pager_selected = -1;
				pager_apply_selection();
			}
			pager_close();
			return INPUT_KEEP;
	}

	pager_close();
	return -1;
}

/* → or F1 at the end of the line: copies the next grapheme of the previous command */
static void copy_next_from_history (void) {

	const wchar_t *cmd = ant_history_last();
	int cn = wcslen(cmd), b = 0, c = 0;

	while (c < cn) {
		int ce = ucd_grapheme_next(cmd, cn, c, NULL);
		if (b >= len) {
			ant_replace(len, 0, cmd + c, ce - c);
			break;
		}
		b = grapheme_next(b);
		c = ce;
	}
}

/*
 * keeps the selection of F7 and left-cd inside the list after every key: the redraw
 * also does it, but it is skipped while keys are queued, and Enter uses the index
 */
static void list_clamp (void) {

	int total = (popup == POPUP_DIRS) ? ant_cd_count() : ant_history_count();

	if (list_selected > total - 1)
		list_selected = total - 1;
	if (list_selected < 0)
		list_selected = 0;
}

static void popup_open (int kind) {

	popup = kind;
	number_len = 0;
	number[0] = 0;
	list_selected = ant_history_displayed();
	list_clamp();
	list_top = -1;
	dirty = true;
}

/* Left on an empty line: menu with the folders of the session; the selection starts on the current one */
static void popup_open_dirs (void) {

	popup = POPUP_DIRS;
	list_selected = ant_cd_count() - 1;
	list_clamp();
	list_top = -1;
	dirty = true;
}

static int popup_key (const KEY_EVENT_RECORD *k) {

	WORD vk = k->wVirtualKeyCode;
	wchar_t ch = k->uChar.UnicodeChar;
	bool shift = (k->dwControlKeyState & SHIFT_PRESSED) != 0;
	const wchar_t *cmd, *hit;

	dirty = true;

	if (vk == VK_ESCAPE) {
		popup = POPUP_NONE;
		return INPUT_KEEP;
	}

	switch (popup) {
		case POPUP_COPY_TO: /* F2: copies from the previous command up to the character */
			if (ch == L'\r')
				popup = POPUP_NONE;
			else if (ch >= 32) {
				cmd = ant_history_last();
				if (cursor < (int) wcslen(cmd) && (hit = wcschr(cmd + cursor, ch)))
					ant_replace(cursor, hit - cmd - cursor, cmd + cursor, hit - cmd - cursor);
				popup = POPUP_NONE;
			}
			break;
		case POPUP_COPY_FROM: /* F4: deletes from the cursor up to the character */
			if (ch == L'\r')
				popup = POPUP_NONE;
			else if (ch >= 32) {
				hit = wcschr(line + cursor, ch);
				ant_replace(cursor, (hit ? hit - line : len) - cursor, NULL, 0);
				popup = POPUP_NONE;
			}
			break;
		case POPUP_NUMBER: /* F9: command number */
			if (ch == L'\r') {
				ant_replace_all(ant_history_nth(_wtoi(number)));
				popup = POPUP_NONE;
			} else if (ch >= L'0' && ch <= L'9' && number_len < 5) {
				number[number_len++] = ch;
				number[number_len] = 0;
			} else if (ch == 8 && number_len > 0)
				number[--number_len] = 0;
			break;
		case POPUP_DIRS: /* left-cd: Enter goes to the folder, Left closes */
			if (ch == L'\r' && ant_cd_get(list_selected)) {
				/* the line stays empty: "cd <folder>" goes straight to the history and to the shell */
				swprintf(leftcd, ANTMAX, L"cd \"%ls\"", ant_cd_get(list_selected));
				leftcd_run = true;
				popup = POPUP_NONE;
				return INPUT_ACCEPT;
			}
			switch (vk) {
				case VK_LEFT:
					popup = POPUP_NONE;
					break;
				case VK_UP:
					list_selected--;
					break;
				case VK_DOWN:
					list_selected++;
					break;
				case VK_HOME:
					list_selected = 0;
					break;
				case VK_END:
					list_selected = ant_cd_count() - 1;
					break;
				case VK_PRIOR:
					list_selected -= list_height;
					break;
				case VK_NEXT:
					list_selected += list_height;
					break;
			}
			list_clamp();
			break;
		case POPUP_LIST: /* F7 */
			if (ch == L'\r') {
				ant_replace_all(ant_history_nth(list_selected));
				popup = POPUP_NONE;
				return INPUT_ACCEPT;
			}
			switch (vk) {
				case VK_F9:
					popup_open(POPUP_NUMBER);
					break;
				case VK_DELETE:
					ant_history_remove(list_selected);
					if (ant_history_count() == 0)
						popup = POPUP_NONE;
					break;
				case VK_LEFT:
				case VK_RIGHT:
					ant_replace_all(ant_history_nth(list_selected));
					popup = POPUP_NONE;
					break;
				case VK_UP:
					if (shift)
						ant_history_swap(list_selected, list_selected - 1);
					list_selected--;
					break;
				case VK_DOWN:
					if (shift)
						ant_history_swap(list_selected, list_selected + 1);
					list_selected++;
					break;
				case VK_HOME:
					list_selected = 0;
					break;
				case VK_END:
					list_selected = ant_history_count() - 1;
					break;
				case VK_PRIOR:
					list_selected -= list_height;
					break;
				case VK_NEXT:
					list_selected += list_height;
					break;
			}
			list_clamp();
			break;
	}
	return INPUT_KEEP;
}

/* Ctrl+V / Shift+Insert: pastes the clipboard text at the cursor; only the 1st line (a newline would run commands) */
static void paste_clipboard (void) {

	static wchar_t text[ANTMAX];
	int n = 0;

	if (!OpenClipboard(NULL))
		return;
	HANDLE data = GetClipboardData(CF_UNICODETEXT);
	const wchar_t *src = data ? (const wchar_t *) GlobalLock(data) : NULL;
	if (src) {
		for (; *src && *src != L'\r' && *src != L'\n' && n < ANTMAX - 1; src++)
			text[n++] = (*src == L'\t') ? L' ' : *src;
		GlobalUnlock(data);
	}
	CloseClipboard();
	if (n)
		ant_replace(cursor, 0, text, n);
}

/* Ctrl+Insert: copies the whole line to the clipboard (Ctrl+C stays "cancel", as in any shell) */
static void copy_line (void) {

	HGLOBAL mem;

	if (len == 0 || !OpenClipboard(NULL))
		return;
	mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(wchar_t) * (len + 1));
	if (mem) {
		wchar_t *dst = (wchar_t *) GlobalLock(mem);
		memcpy(dst, line, sizeof(wchar_t) * len);
		dst[len] = 0;
		GlobalUnlock(mem);
		EmptyClipboard();
		if (!SetClipboardData(CF_UNICODETEXT, mem))
			GlobalFree(mem);
	}
	CloseClipboard();
}

static int handle_key (const KEY_EVENT_RECORD *k) {

	bool ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
	bool alt  = (k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
	wchar_t ch = k->uChar.UnicodeChar;
	int pos;

	if (ctrl && !alt && k->wVirtualKeyCode == 'C')
		return INPUT_CANCEL;

	if (popup == POPUP_NONE && ((ctrl && k->wVirtualKeyCode == 'V') ||
		(k->wVirtualKeyCode == VK_INSERT && (k->dwControlKeyState & SHIFT_PRESSED)))) {
		paste_clipboard();
		return INPUT_KEEP;
	}
	if (popup == POPUP_NONE && ctrl && k->wVirtualKeyCode == VK_INSERT) {
		copy_line();
		return INPUT_KEEP;
	}

	if (popup == POPUP_PAGER) {
		int result = pager_key(k);
		if (result >= 0)
			return result;
	}

	if (popup != POPUP_NONE)
		return popup_key(k);

	switch (k->wVirtualKeyCode) {
		case VK_UP:
		case VK_F5:
			if (!ant_history_at_first())
				ant_replace_all(ant_history_prev());
			return INPUT_KEEP;
		case VK_DOWN:
			if (!ant_history_at_last())
				ant_replace_all(ant_history_next());
			return INPUT_KEEP;
		case VK_PRIOR:
			if (!ant_history_at_first())
				ant_replace_all(ant_history_nth(0));
			return INPUT_KEEP;
		case VK_NEXT:
			if (!ant_history_at_last())
				ant_replace_all(ant_history_nth(ant_history_count() - 1));
			return INPUT_KEEP;
		case VK_F1:
			if (cursor < len)
				ant_set_cursor(grapheme_next(cursor));
			else
				copy_next_from_history();
			return INPUT_KEEP;
		case VK_F2:
			if (ant_history_count())
				popup_open(POPUP_COPY_TO);
			return INPUT_KEEP;
		case VK_F3: {
			/* copies the rest of the previous command from the cursor on */
			const wchar_t *last = ant_history_last();
			int ln = wcslen(last);
			if (ln > cursor)
				ant_replace(cursor, len - cursor, last + cursor, ln - cursor);
			return INPUT_KEEP;
		}
		case VK_F4:
			popup_open(POPUP_COPY_FROM);
			return INPUT_KEEP;
		case VK_F7:
			if (alt) {
				ant_history_clear();
				dirty = true; /* the autosuggestion goes away */
			}
			else if (!ctrl && ant_history_count())
				popup_open(POPUP_LIST);
			return INPUT_KEEP;
		case VK_F8: {
			/* commands that start with the text up to the cursor, newest to oldest */
			int index, keep = cursor;
			if (ant_history_find(line, cursor, &index)) {
				ant_replace_all(ant_history_nth(index));
				ant_set_cursor(keep);
			}
			return INPUT_KEEP;
		}
		case VK_F9:
			if (ant_history_count())
				popup_open(POPUP_NUMBER);
			return INPUT_KEEP;
	}

	switch (k->wVirtualKeyCode) {
		case VK_RETURN:
			return INPUT_ACCEPT;
		case VK_ESCAPE:
			ant_replace(0, len, NULL, 0);
			return INPUT_KEEP;
		case VK_BACK:
			pos = ctrl ? word_prev(cursor) : grapheme_prev(cursor);
			ant_replace(pos, cursor - pos, NULL, 0);
			return INPUT_KEEP;
		case VK_DELETE:
			ant_replace(cursor, grapheme_next(cursor) - cursor, NULL, 0);
			return INPUT_KEEP;
		case VK_HOME:
			if (ctrl)
				ant_replace(0, cursor, NULL, 0);
			ant_set_cursor(0);
			return INPUT_KEEP;
		case VK_END:
			if (ctrl)
				ant_replace(cursor, len - cursor, NULL, 0);
			else if (cursor == len)
				accept_suggestion(false);
			ant_set_cursor(len);
			return INPUT_KEEP;
		case VK_LEFT:
			if (len == 0 && !ctrl && !alt && ant_cd_count() > 1) {
				popup_open_dirs();
				return INPUT_KEEP;
			}
			ant_set_cursor(ctrl ? word_prev(cursor) : grapheme_prev(cursor));
			return INPUT_KEEP;
		case VK_RIGHT:
			/* at the end: accepts the autosuggestion (Ctrl: one word); without one, conhost's → */
			if (ctrl) {
				if (cursor < len || !accept_suggestion(true))
					ant_set_cursor(word_next(cursor));
			} else if (cursor < len)
				ant_set_cursor(grapheme_next(cursor));
			else if (!accept_suggestion(false))
				copy_next_from_history();
			return INPUT_KEEP;
		case VK_INSERT:
			insert_mode = !insert_mode;
			set_cursor_type(insert_mode, true);
			return INPUT_KEEP;
		case VK_TAB:
			tab_key((k->dwControlKeyState & SHIFT_PRESSED) != 0);
			return INPUT_KEEP;
	}

	if (ch == L'\r') /* Ctrl+M */
		return INPUT_ACCEPT;
	if (ch == 8) { /* Ctrl+H */
		pos = grapheme_prev(cursor);
		ant_replace(pos, cursor - pos, NULL, 0);
		return INPUT_KEEP;
	}

	/* F1..F12, arrows etc. have no text; Alt without Ctrl is a shortcut (AltGr = Ctrl+Alt is text) */
	if (ch < 32 || ch == 0x7F || (alt && !ctrl))
		return INPUT_KEEP;

	insert_char(ch);
	return INPUT_KEEP;
}

/* redirected stdin (file/pipe): reads one UTF-8 line; the end of the input closes the shell */
static void read_redirected (void) {

	if (!fgets(char_line_buf, ANTMAX, stdin)) {
		strcpy(char_line_buf, "nop");
		return;
	}
	char_line_buf[strcspn(char_line_buf, "\r\n")] = 0;
	printf("%s\n", char_line_buf);
}

void ant_editor_read_line (const char *str) {

	INPUT_RECORD ir;
	DWORD in_mode, out_mode, count, pending;
	CONSOLE_CURSOR_INFO cursor_info;
	int result = INPUT_KEEP;

	hin  = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);

	if (!GetConsoleMode(hin, &in_mode)) {
		write_prompt();
		read_redirected();
		return;
	}

	/* a child program may have changed the modes: set them again on every read */
	GetConsoleMode(hout, &out_mode);
	GetConsoleCursorInfo(hout, &cursor_info);
	/* ENABLE_EXTENDED_FLAGS keeps the mouse selection (QuickEdit) of the console; without it the mode call turns it off */
	SetConsoleMode(hin, (in_mode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
		~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
	SetConsoleMode(hout, out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

	print_prompt();
	probe_cell_mode();

	len = cursor = 0;
	line[0] = 0;
	insert_mode = true;
	popup = POPUP_NONE;
	leftcd_run = false;
	suggest_on = true;
	set_cursor_type(insert_mode, true);

	if (str && *str) {
		static wchar_t tmp[ANTMAX];
		MultiByteToWideChar(CP_UTF8, 0, str, -1, tmp, ANTMAX);
		tmp[LINE_UNITS] = 0;
		ant_replace(0, 0, tmp, wcslen(tmp));
	}
	dirty = true;
	ant_redisplay();

	/* one event at a time: whatever comes after Enter (a paste) is left for the next line */
	while (result == INPUT_KEEP && ReadConsoleInputW(hin, &ir, 1, &count)) {

		if (count == 0)
			continue;

		if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT)
			dirty = true;
		else if (ir.EventType == KEY_EVENT) {
			const KEY_EVENT_RECORD *k = &ir.Event.KeyEvent;
			if (k->bKeyDown) {
				for (int r = (k->wRepeatCount ? k->wRepeatCount : 1); r > 0 && result == INPUT_KEEP; r--)
					result = handle_key(k);
			/* Alt + numeric keypad: the character arrives when Alt is released */
			} else if (k->wVirtualKeyCode == VK_MENU && k->uChar.UnicodeChar)
				insert_char(k->uChar.UnicodeChar);
		}

		/* a paste makes many events: draw only when the queue is empty */
		if (result == INPUT_KEEP && GetNumberOfConsoleInputEvents(hin, &pending) && pending == 0)
			ant_redisplay();
	}

	/* closes the popup and leaves the final line on the screen, without the autosuggestion */
	pager_close();
	popup = POPUP_NONE;
	suggest_on = false;
	ant_set_cursor(len);
	ant_redisplay();

	if (result == INPUT_CANCEL) {
		write_wide(L"^C", 2);
		len = cursor = 0;
		line[0] = 0;
	} else if (result == INPUT_ACCEPT)
		ant_history_add(leftcd_run ? leftcd : line);
	write_wide(L"\r\n", 2);

	/* from here on it is the command's output */
	if (result == INPUT_ACCEPT) {
		write_wide(L"\x1b]133;C\x1b\\", 9);
		command_pending = true;
	}

	to_narrowchar(char_line_buf, ANTMAX, leftcd_run && result == INPUT_ACCEPT ? leftcd : line);

	SetConsoleCursorInfo(hout, &cursor_info);
	SetConsoleMode(hin, in_mode);
	SetConsoleMode(hout, out_mode);
}
