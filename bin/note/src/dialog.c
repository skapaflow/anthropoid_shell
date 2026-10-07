#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <windows.h>

#include "dialog.h"
#include "editor.h"
#include "render.h"
#include "screen.h"
#include "config.h"
#include "clipboard.h"
#include "ucd.h"

/*************************************************************
 * Modal boxes drawn over the editor:                        *
 *                                                           *
 *   dialog_ask   a question with buttons                    *
 *   dialog_form  one or more text fields, OK / Cancel       *
 *   dialog_info  lines of text, any key closes              *
 *                                                           *
 * Labels mark their hotkey with '&' ("&Save").              *
 *************************************************************/

int label_width (const wchar_t *s) {

	int n = 0;

	for (; *s; s++)
		if (*s != L'&')
			n++;
	return n;
}

wchar_t label_hotkey (const wchar_t *s) {

	const wchar_t *p = wcschr(s, L'&');
	return p && p[1] ? towupper(p[1]) : 0;
}

/* label in style 'fg' with its hotkey letter in 'hot', over the background of 'bg' */
void out_label (const wchar_t *s, int fg, int hot, int bg) {

	out_style(fg, bg);
	for (; *s; s++) {
		if (*s == L'&' && s[1]) {
			s++;
			out_style(hot, bg);
			out_n(s, 1);
			out_style(fg, bg);
		} else
			out_n(s, 1);
	}
}

/* box with border, optional title and a shadow (when it fits) */
void draw_box (int x, int y, int w, int h, int style, const wchar_t *title) {

	bool shadow_r = x + w + 2 <= screen_w, shadow_b = y + h < screen_h;
	int r;

	for (r = 0; r < h; r++) {
		out_goto(y + r, x);
		out_style(style, style);
		out(r == 0 ? L"┌" : r == h - 1 ? L"└" : L"│");
		out_char(r == 0 || r == h - 1 ? L'─' : L' ', w - 2);
		out(r == 0 ? L"┐" : r == h - 1 ? L"┘" : L"│");
		if (r > 0 && shadow_r) {
			out_style(S_SHADOW, S_SHADOW);
			out(L"  ");
		}
	}
	if (shadow_b) {
		out_goto(y + h, x + 2);
		out_style(S_SHADOW, S_SHADOW);
		out_char(L' ', shadow_r ? w : w - 2);
	}
	if (title) {
		int tw = text_width(title, (int) wcslen(title));
		if (tw + 4 <= w) {
			out_goto(y, x + (w - tw - 2) / 2);
			out_style(S_DIALOG_TITLE, style);
			out(L" ");
			out(title);
			out(L" ");
		}
	}
}

static void draw_centered (int x, int y, int w, const wchar_t *s) {

	int n = (int) wcslen(s), tw = text_width(s, n), pad = tw < w ? (w - tw) / 2 : 0;

	out_goto(y, x + pad);
	out_text(s, n, w - pad, S_DIALOG, S_DIALOG);
}

/* a row of buttons centered in [x, x + w); returns their total width and each start in 'at' */
static void draw_buttons (int x, int y, int w, const wchar_t **labels, int n, int sel, int *at) {

	int total = 2 * (n - 1), i, bx;

	for (i = 0; i < n; i++)
		total += label_width(labels[i]) + 4;
	bx = x + (w - total) / 2;
	if (bx < x)
		bx = x;

	for (i = 0; i < n; i++) {
		int style = i == sel ? S_BUTTON_SELECTED : S_BUTTON;
		at[i] = bx;
		out_goto(y, bx);
		out_style(style, style);
		out(L"  ");
		out_label(labels[i], style, i == sel ? style : S_BUTTON_HOTKEY, style);
		out_style(style, style);
		out(L"  ");
		bx += label_width(labels[i]) + 6;
	}
}

/* button under (mx, my), or -1 */
static int button_at (int mx, int my, int y, const wchar_t **labels, int n, const int *at) {

	int i;

	if (my != y)
		return -1;
	for (i = 0; i < n; i++)
		if (mx >= at[i] && mx < at[i] + label_width(labels[i]) + 4)
			return i;
	return -1;
}

static int box_y (int h, int place) {

	int y = place == PLACE_TOP ? 2 : place == PLACE_BOTTOM ? screen_h - h - 3 : (screen_h - h) / 2;
	return y < 1 ? 1 : y;
}

/*
 * Question with buttons; returns the index chosen. Arrows/Tab move between
 * buttons, Enter picks, the hotkey letters pick, Esc picks the last one (Cancel).
 */
int dialog_ask (const wchar_t *title, const wchar_t *text, const wchar_t **buttons, int n, int place) {

	int sel = 0, at[8], i;
	EVENT ev;

	for (;;) {
		int tw = text_width(text, (int) wcslen(text)), bw = 2 * (n - 1);
		int w, h = 7, x, y;

		for (i = 0; i < n; i++)
			bw += label_width(buttons[i]) + 4;
		w = (tw > bw ? tw : bw) + 8;
		if (w > screen_w - 2)
			w = screen_w - 2;
		x = (screen_w - w) / 2;
		y = box_y(h, place);

		render();
		draw_box(x, y, w, h, S_DIALOG, title);
		draw_centered(x + 2, y + 2, w - 4, text);
		draw_buttons(x + 1, y + 4, w - 2, buttons, n, sel, at);
		out_flush();

		screen_read(&ev);
		switch (ev.type) {
		case EV_END:
			return n - 1;
		case EV_RESIZE:
			screen_update_size();
			continue;
		case EV_MOUSE:
			if (ev.mouse == MOUSE_DOWN && (i = button_at(ev.x, ev.y, y + 4, buttons, n, at)) >= 0)
				return i;
			continue;
		case EV_KEY:
			break;
		default:
			continue;
		}

		switch (ev.key.wVirtualKeyCode) {
		case VK_LEFT:
		case VK_UP:
			sel = (sel + n - 1) % n;
			break;
		case VK_RIGHT:
		case VK_DOWN:
			sel = (sel + 1) % n;
			break;
		case VK_TAB:
			sel = (ev.mods & SHIFT_PRESSED) ? (sel + n - 1) % n : (sel + 1) % n;
			break;
		case VK_RETURN:
			return sel;
		case VK_ESCAPE:
			return n - 1;
		default:
			for (i = 0; i < n; i++)
				if (ev.key.uChar.UnicodeChar && towupper(ev.key.uChar.UnicodeChar) == label_hotkey(buttons[i]))
					return i;
		}
	}
}

/*---------------------------------------------------------- form */

typedef struct {
	wchar_t *s;      /* the caller's buffer (FIELD_MAX units) */
	int      len, cur;
} FIELD;

static void field_insert (FIELD *f, const wchar_t *s, int n) {

	if (f->len + n > FIELD_MAX - 1)
		n = FIELD_MAX - 1 - f->len;
	if (n <= 0)
		return;
	memmove(f->s + f->cur + n, f->s + f->cur, (f->len - f->cur) * sizeof(wchar_t));
	memcpy(f->s + f->cur, s, n * sizeof(wchar_t));
	f->len += n;
	f->cur += n;
	f->s[f->len] = 0;
}

static void field_delete (FIELD *f, int from, int to) {

	memmove(f->s + from, f->s + to, (f->len - to) * sizeof(wchar_t));
	f->len -= to - from;
	f->cur = from;
	f->s[f->len] = 0;
}

/* first column shown so that the cursor stays inside 'width' */
static int field_skip (const FIELD *f, int width) {

	int col = text_width(f->s, f->cur);
	return col > width - 1 ? col - (width - 1) : 0;
}

/* edits the focused field; false if the key is not for the field */
static bool field_key (FIELD *f, const KEY_EVENT_RECORD *k) {

	bool ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
		!(k->dwControlKeyState & RIGHT_ALT_PRESSED);
	wchar_t c = k->uChar.UnicodeChar;

	switch (k->wVirtualKeyCode) {
	case VK_BACK:
		if (f->cur > 0)
			field_delete(f, ucd_grapheme_prev(f->s, f->len, f->cur), f->cur);
		return true;
	case VK_DELETE:
		if (f->cur < f->len)
			field_delete(f, f->cur, ucd_grapheme_next(f->s, f->len, f->cur, NULL));
		return true;
	case VK_LEFT:
		f->cur = ucd_grapheme_prev(f->s, f->len, f->cur);
		return true;
	case VK_RIGHT:
		if (f->cur < f->len)
			f->cur = ucd_grapheme_next(f->s, f->len, f->cur, NULL);
		return true;
	case VK_HOME:
		f->cur = 0;
		return true;
	case VK_END:
		f->cur = f->len;
		return true;
	}

	if (ctrl && k->wVirtualKeyCode == 'V') {
		int n;
		wchar_t *t = clipboard_get(&n);
		if (t) {
			/* first line only */
			int i = 0;
			while (i < n && t[i] != L'\n')
				i++;
			field_insert(f, t, i);
			free(t);
		}
		return true;
	}
	if (!ctrl && c >= 0x20 && c != 0x7f) {
		field_insert(f, &c, 1);
		return true;
	}
	return false;
}

/*---------------------------------------------------------- file name completion */

/* matches of the completion in progress; count 0: none (any key but Tab ends it) */
static struct {
	wchar_t **names;
	int       count, index;
	int       base;       /* length of the folder part kept in front of every match */
} completion;

static void completion_end (void) {

	int i;

	for (i = 0; i < completion.count; i++)
		free(completion.names[i]);
	free(completion.names);
	memset(&completion, 0, sizeof completion);
}

static int compare_names (const void *a, const void *b) {

	return _wcsicmp(*(wchar_t * const *) a, *(wchar_t * const *) b);
}

/* files and folders (these with a '\') of the field's folder whose names start with what was typed */
static void completion_list (const FIELD *f) {

	static wchar_t pattern[FIELD_MAX + 4];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	const wchar_t *prefix;
	int base = f->len, n, cap = 0;

	while (base > 0 && !wcschr(L"\\/:", f->s[base - 1]))
		base--;
	prefix = f->s + base;
	n = f->len - base;
	swprintf(pattern, FIELD_MAX + 4, L"%ls*", f->s);

	h = FindFirstFileW(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		bool folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		size_t len = wcslen(fd.cFileName);
		wchar_t *name;
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		/* the pattern also matches 8.3 short names: keep only the real prefix matches */
		if (_wcsnicmp(fd.cFileName, prefix, n))
			continue;
		if (completion.count == cap) {
			cap = cap ? cap * 2 : 32;
			completion.names = xrealloc(completion.names, cap * sizeof(wchar_t *));
		}
		name = xrealloc(NULL, (len + 2) * sizeof(wchar_t));
		wcscpy(name, fd.cFileName);
		if (folder)
			wcscat(name, L"\\");
		completion.names[completion.count++] = name;
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	qsort(completion.names, completion.count, sizeof(wchar_t *), compare_names);
	completion.base = base;
}

/* Tab (dir 1) or Shift+Tab (-1) in a file name field: the first press completes, the next ones cycle */
static void complete_file (FIELD *f, int dir) {

	const wchar_t *name;
	int n;

	if (completion.count == 0) {
		completion_list(f);
		if (completion.count == 0)
			return;
		completion.index = dir > 0 ? 0 : completion.count - 1;
	} else
		completion.index = (completion.index + dir + completion.count) % completion.count;

	name = completion.names[completion.index];
	n = (int) wcslen(name);
	if (completion.base + n > FIELD_MAX - 1)
		n = FIELD_MAX - 1 - completion.base;
	memcpy(f->s + completion.base, name, n * sizeof(wchar_t));
	f->len = f->cur = completion.base + n;
	f->s[f->len] = 0;
}

/*
 * Text fields with OK / Cancel. 'values' are FIELD_MAX buffers that come in with
 * the initial text. Tab and the arrows move between fields; Enter confirms (if the
 * first field is not empty), Esc cancels. With 'files', the first field is a file
 * name and Tab / Shift+Tab complete it.
 */
static bool form_loop (const wchar_t *title, const wchar_t **labels, wchar_t **values, int n, bool files) {

	static const wchar_t *buttons[] = { L"OK", L"Cancel" };
	FIELD fields[4];
	int focus = 0, at[2], i;
	EVENT ev;

	if (n > 4)
		n = 4;
	for (i = 0; i < n; i++) {
		fields[i].s = values[i];
		fields[i].len = fields[i].cur = (int) wcslen(values[i]);
	}

	for (;;) {
		int w = screen_w - 4 < 64 ? screen_w - 4 : 64, h = 3 * n + 4;
		int x = (screen_w - w) / 2, y = box_y(h, PLACE_CENTER), fw = w - 4;

		render();
		draw_box(x, y, w, h, S_DIALOG, title);
		for (i = 0; i < n; i++) {
			FIELD *f = &fields[i];
			out_goto(y + 2 + 3 * i, x + 2);
			out_text(labels[i], (int) wcslen(labels[i]), fw, S_DIALOG, S_DIALOG);
			out_goto(y + 3 + 3 * i, x + 2);
			out_cells(f->s, f->len, NULL, field_skip(f, fw), fw, S_FIELD, S_FIELD, false);
		}
		draw_buttons(x + 1, y + h - 2, w - 2, buttons, 2, -1, at);
		if (files) {
			wchar_t hint[48];
			if (completion.count > 0)
				swprintf(hint, 48, L"%d of %d", completion.index + 1, completion.count);
			else
				swprintf(hint, 48, L"Tab completes");
			out_goto(y + 4, x + w - 2 - (int) wcslen(hint));
			out_text(hint, (int) wcslen(hint), (int) wcslen(hint), S_MENU_SHORTCUT, S_DIALOG);
		}
		out_goto(y + 3 + 3 * focus, x + 2 + text_width(fields[focus].s, fields[focus].cur) - field_skip(&fields[focus], fw));
		out(L"\x1b[?25h");
		out_flush();

		screen_read(&ev);
		switch (ev.type) {
		case EV_END:
			return false;
		case EV_RESIZE:
			screen_update_size();
			continue;
		case EV_MOUSE:
			if (ev.mouse != MOUSE_DOWN)
				continue;
			completion_end();
			switch (button_at(ev.x, ev.y, y + h - 2, buttons, 2, at)) {
			case 0:
				if (fields[0].len > 0)
					return true;
				continue;
			case 1:
				return false;
			}
			for (i = 0; i < n; i++)
				if (ev.y == y + 3 + 3 * i && ev.x >= x + 2 && ev.x < x + 2 + fw) {
					FIELD *f = &fields[i];
					int col = ev.x - (x + 2) + field_skip(f, fw), p = 0, c = 0, cw, kind, next;
					while (p < f->len) {
						next = cell_next(f->s, f->len, p, c, &cw, &kind);
						if (c + cw > col)
							break;
						c += cw;
						p = next;
					}
					focus = i;
					f->cur = p;
				}
			continue;
		case EV_KEY:
			break;
		default:
			continue;
		}

		if (ev.key.wVirtualKeyCode == VK_TAB && files && focus == 0) {
			complete_file(&fields[0], (ev.mods & SHIFT_PRESSED) ? -1 : 1);
			continue;
		}
		completion_end();

		switch (ev.key.wVirtualKeyCode) {
		case VK_RETURN:
			if (fields[0].len > 0)
				return true;
			continue;
		case VK_ESCAPE:
			return false;
		case VK_TAB:
			focus = (ev.mods & SHIFT_PRESSED) ? (focus + n - 1) % n : (focus + 1) % n;
			continue;
		case VK_UP:
			focus = (focus + n - 1) % n;
			continue;
		case VK_DOWN:
			focus = (focus + 1) % n;
			continue;
		}
		field_key(&fields[focus], &ev.key);
	}
}

bool dialog_form (const wchar_t *title, const wchar_t **labels, wchar_t **values, int n, bool files) {

	bool ok = form_loop(title, labels, values, n, files);

	completion_end();
	return ok;
}

/* lines of text in a box; any key or click closes it */
void dialog_info (const wchar_t *title, const wchar_t **lines, int n) {

	static const wchar_t *buttons[] = { L"OK" };
	int at[1], i;
	EVENT ev;

	for (;;) {
		int w = 0, h, x, y, shown;

		for (i = 0; i < n; i++) {
			int lw = text_width(lines[i], (int) wcslen(lines[i]));
			if (lw > w)
				w = lw;
		}
		w += 6;
		if (w > screen_w - 2)
			w = screen_w - 2;
		shown = n;
		if (shown + 5 > screen_h - 1)
			shown = screen_h - 6;
		if (shown < 0)
			shown = 0;
		h = shown + 5;
		x = (screen_w - w) / 2;
		y = box_y(h, PLACE_CENTER);

		render();
		draw_box(x, y, w, h, S_DIALOG, title);
		for (i = 0; i < shown; i++) {
			out_goto(y + 2 + i, x + 3);
			out_text(lines[i], (int) wcslen(lines[i]), w - 5, S_DIALOG, S_DIALOG);
		}
		draw_buttons(x + 1, y + h - 2, w - 2, buttons, 1, 0, at);
		out_flush();

		screen_read(&ev);
		if (ev.type == EV_RESIZE) {
			screen_update_size();
			continue;
		}
		if (ev.type == EV_KEY || ev.type == EV_END || ev.type == EV_MENU ||
				(ev.type == EV_MOUSE && ev.mouse == MOUSE_DOWN))
			return;
	}
}
