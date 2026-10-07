#include <wchar.h>
#include <wctype.h>
#include <windows.h>

#include "menu.h"
#include "editor.h"
#include "dialog.h"
#include "render.h"
#include "screen.h"
#include "config.h"

/*************************************************************
 * Menu bar, like the one of MS-DOS EDIT.                    *
 *                                                           *
 * Alt (alone) or F10 highlights the bar; Alt+letter opens a *
 * menu. Arrows move, Enter or the item letter runs it, Esc  *
 * closes. The mouse opens menus on the bar and runs an item *
 * when released on it.                                      *
 *************************************************************/

typedef struct {
	const wchar_t *label;     /* NULL: separator */
	const wchar_t *shortcut;
	int            command;
} MENU_ITEM;

typedef struct {
	const wchar_t   *title;
	const MENU_ITEM *items;
	int              count;
} MENU;

static const MENU_ITEM file_items[] = {
	{ L"&New",          L"Ctrl+N", CMD_NEW },
	{ L"&Open...",      L"Ctrl+O", CMD_OPEN },
	{ L"&Save",         L"Ctrl+S", CMD_SAVE },
	{ L"Save &As...",   L"",       CMD_SAVE_AS },
	{ NULL },
	{ L"E&xit",         L"Esc",    CMD_EXIT },
};

static const MENU_ITEM edit_items[] = {
	{ L"&Undo",         L"Ctrl+Z", CMD_UNDO },
	{ L"&Redo",         L"Ctrl+Y", CMD_REDO },
	{ NULL },
	{ L"Cu&t",          L"Ctrl+X", CMD_CUT },
	{ L"&Copy",         L"Ctrl+C", CMD_COPY },
	{ L"&Paste",        L"Ctrl+V", CMD_PASTE },
	{ L"&Delete",       L"Del",    CMD_DELETE },
	{ NULL },
	{ L"Select &All",   L"Ctrl+A", CMD_SELECT_ALL },
};

static const MENU_ITEM search_items[] = {
	{ L"&Find...",       L"Ctrl+F",   CMD_FIND },
	{ L"Find &Next",     L"F3",       CMD_FIND_NEXT },
	{ L"Find &Previous", L"Shift+F3", CMD_FIND_PREVIOUS },
	{ L"&Replace...",    L"Ctrl+H",   CMD_REPLACE },
	{ NULL },
	{ L"&Go to Line...", L"Ctrl+G",   CMD_GOTO_LINE },
};

static const MENU_ITEM view_items[] = {
	{ L"&Word Wrap",              L"Alt+Z", CMD_WORD_WRAP },
	{ NULL },
	{ L"&Line Numbers",           L"",      CMD_LINE_NUMBERS },
	{ L"&Syntax Highlighting",    L"",      CMD_SYNTAX },
	{ L"&Highlight Current Line", L"",      CMD_HIGHLIGHT_LINE },
};

static const MENU_ITEM help_items[] = {
	{ L"&Keyboard Shortcuts", L"F1", CMD_HELP },
	{ L"&About",              L"",   CMD_ABOUT },
};

#define ITEMS(a) a, (int) (sizeof a / sizeof a[0])

static const MENU menus[] = {
	{ L"&File",   ITEMS(file_items) },
	{ L"&Edit",   ITEMS(edit_items) },
	{ L"&Search", ITEMS(search_items) },
	{ L"&View",   ITEMS(view_items) },
	{ L"&Help",   ITEMS(help_items) },
};

#define MENU_COUNT ((int) (sizeof menus / sizeof menus[0]))

/* column where menu m starts on the bar (" File " takes the title plus a space on each side) */
static int title_x (int m) {

	int x = 0, i;

	for (i = 0; i < m; i++)
		x += label_width(menus[i].title) + 2;
	return x;
}

int menu_at (int x) {

	int i;

	for (i = 0; i < MENU_COUNT; i++)
		if (x >= title_x(i) && x < title_x(i) + label_width(menus[i].title) + 2)
			return i;
	return -1;
}

int menu_hotkey (wchar_t c) {

	int i;

	for (i = 0; i < MENU_COUNT; i++)
		if (towupper(c) == label_hotkey(menus[i].title))
			return i;
	return -1;
}

/* the bar on row 0, with menu 'active' highlighted (-1: none) and the file name on the right */
void menu_draw_bar (int active) {

	static wchar_t name[FIELD_MAX + 8];
	int i, end = title_x(MENU_COUNT), n, w;

	out_goto(0, 0);
	for (i = 0; i < MENU_COUNT; i++) {
		int style = i == active ? S_MENU_SELECTED : S_MENU_BAR;
		out_style(style, style);
		out(L" ");
		out_label(menus[i].title, style, i == active ? style : S_MENU_HOTKEY, style);
		out_style(style, style);
		out(L" ");
	}

	swprintf(name, FIELD_MAX + 8, L"%ls%ls ", file_name(), is_modified() ? L" *" : L"");
	n = (int) wcslen(name);
	w = text_width(name, n);
	if (end + w + 2 > screen_w) {
		out_text(NULL, 0, screen_w - end, S_MENU_BAR, S_MENU_BAR);
		return;
	}
	out_text(NULL, 0, screen_w - end - w, S_MENU_BAR, S_MENU_BAR);
	out_text(name, n, w, S_MENU_BAR, S_MENU_BAR);
}

/* position and width of the dropdown of menu m */
static void dropdown_rect (int m, int *x, int *w) {

	const MENU *menu = &menus[m];
	int label = 0, shortcut = 0, i;

	for (i = 0; i < menu->count; i++) {
		const MENU_ITEM *it = &menu->items[i];
		if (!it->label)
			continue;
		if (label_width(it->label) > label)
			label = label_width(it->label);
		if ((int) wcslen(it->shortcut) > shortcut)
			shortcut = (int) wcslen(it->shortcut);
	}
	/* "│ √ Label   Shortcut │" */
	*w = 2 + 3 + label + (shortcut ? 3 + shortcut : 0) + 1;
	*x = title_x(m);
	if (*x + *w + 2 > screen_w)
		*x = screen_w - *w - 2;
	if (*x < 0)
		*x = 0;
}

static void draw_dropdown (int m, int sel) {

	const MENU *menu = &menus[m];
	int x, w, i;

	dropdown_rect(m, &x, &w);
	draw_box(x, 1, w, menu->count + 2, S_MENU, NULL);

	for (i = 0; i < menu->count; i++) {
		const MENU_ITEM *it = &menu->items[i];
		int style = i == sel ? S_MENU_SELECTED : S_MENU, check, pad;

		out_goto(2 + i, x);
		if (!it->label) {
			out_style(S_MENU, S_MENU);
			out(L"├");
			out_char(L'─', w - 2);
			out(L"┤");
			continue;
		}
		check = command_checked(it->command);
		out_style(S_MENU, S_MENU);
		out(L"│");
		out_style(style, style);
		out(check > 0 ? L" √ " : L"   ");
		out_label(it->label, style, i == sel ? style : S_MENU_HOTKEY, style);
		pad = w - 2 - 3 - label_width(it->label) - (int) wcslen(it->shortcut) - 1;
		out_style(style, style);
		out_char(L' ', pad);
		out_style(i == sel ? style : S_MENU_SHORTCUT, style);
		out(it->shortcut);
		out_style(style, style);
		out(L" ");
		out_style(S_MENU, S_MENU);
		out(L"│");
	}
}

/* item of menu m under (mx, my): index, -1 outside the dropdown, -2 on a border or separator */
static int dropdown_hit (int m, int mx, int my) {

	int x, w, i;

	dropdown_rect(m, &x, &w);
	if (mx < x || mx >= x + w || my < 1 || my >= menus[m].count + 3)
		return -1;
	i = my - 2;
	if (i < 0 || i >= menus[m].count || mx == x || mx == x + w - 1 || !menus[m].items[i].label)
		return -2;
	return i;
}

/* next item from 'sel' in direction 'dir', skipping separators */
static int step_item (int m, int sel, int dir) {

	int n = menus[m].count, i;

	for (i = 0; i < n; i++) {
		sel = (sel + dir + n) % n;
		if (menus[m].items[sel].label)
			return sel;
	}
	return 0;
}

/*
 * Menu loop, starting at menu m (open: with its dropdown, otherwise only the bar
 * highlighted). Returns the command picked, or CMD_NONE.
 */
int menu_run (int m, bool open) {

	int sel = open ? step_item(m, -1, 1) : -1, i;
	EVENT ev;

	for (;;) {
		render();
		menu_draw_bar(m);
		if (open)
			draw_dropdown(m, sel);
		out_flush();

		screen_read(&ev);
		switch (ev.type) {
		case EV_END:
		case EV_MENU:
			return CMD_NONE;
		case EV_RESIZE:
			screen_update_size();
			continue;

		case EV_MOUSE:
			if (ev.mouse == MOUSE_WHEEL)
				continue;
			if (ev.y == 0) {
				int k = menu_at(ev.x);
				if (ev.mouse == MOUSE_DOWN || ev.mouse == MOUSE_DOUBLE) {
					if (k < 0 || (k == m && open))
						return CMD_NONE;
				} else if (ev.mouse != MOUSE_DRAG || k < 0)
					continue;
				if (k != m || !open) {
					m = k;
					open = true;
					sel = -1;
				}
				continue;
			}
			i = open ? dropdown_hit(m, ev.x, ev.y) : -1;
			if (i >= 0) {
				sel = i;
				if (ev.mouse == MOUSE_UP)
					return menus[m].items[i].command;
			} else if (i == -1 && (ev.mouse == MOUSE_DOWN || ev.mouse == MOUSE_DOUBLE))
				return CMD_NONE;
			continue;

		case EV_KEY:
			break;
		default:
			continue;
		}

		switch (ev.key.wVirtualKeyCode) {
		case VK_ESCAPE:
		case VK_F10:
			return CMD_NONE;
		case VK_LEFT:
			m = (m + MENU_COUNT - 1) % MENU_COUNT;
			sel = open ? step_item(m, -1, 1) : -1;
			break;
		case VK_RIGHT:
			m = (m + 1) % MENU_COUNT;
			sel = open ? step_item(m, -1, 1) : -1;
			break;
		case VK_DOWN:
			sel = open ? step_item(m, sel < 0 ? -1 : sel, 1) : step_item(m, -1, 1);
			open = true;
			break;
		case VK_UP:
			sel = step_item(m, sel < 0 ? 0 : sel, -1);
			open = true;
			break;
		case VK_RETURN:
			if (open && sel >= 0)
				return menus[m].items[sel].command;
			open = true;
			sel = step_item(m, -1, 1);
			break;
		default: {
			wchar_t c = towupper(ev.key.uChar.UnicodeChar);
			if (!c)
				break;
			if (!open) {
				int k = menu_hotkey(c);
				if (k >= 0) {
					m = k;
					open = true;
					sel = step_item(m, -1, 1);
				}
				break;
			}
			for (i = 0; i < menus[m].count; i++)
				if (menus[m].items[i].label && label_hotkey(menus[m].items[i].label) == c)
					return menus[m].items[i].command;
			/* Alt+letter of another menu */
			if (ev.mods & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
				int k = menu_hotkey(c);
				if (k >= 0) {
					m = k;
					sel = step_item(m, -1, 1);
				}
			}
		}
		}
	}
}
