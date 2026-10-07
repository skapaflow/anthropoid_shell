#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <limits.h>
#include <windows.h>

#include "editor.h"
#include "render.h"
#include "dialog.h"
#include "menu.h"
#include "screen.h"
#include "config.h"
#include "syntax.h"
#include "clipboard.h"
#include "ucd.h"

/*************************************************************
 * Editor of note: no modes, like MS-DOS EDIT.               *
 *                                                           *
 * The text only changes through edit_insert and             *
 * edit_delete, which record every change for undo. A user   *
 * command opens an undo step first (history_begin).         *
 *                                                           *
 * The selection runs from the anchor (ay, ax) to the        *
 * cursor (cy, cx) while 'selecting' is set.                 *
 *************************************************************/

EDITOR ed;

static bool     mouse_selecting;  /* left button held after a click in the text */
static bool     pasting;          /* the key is part of a paste coming in as keys */
static wchar_t *line_clip;        /* last whole line copied (pasting it goes above the line) */

#define LN (&ed.buf.lines[ed.cy])

bool is_modified (void) {

	return history_modified(&ed.history);
}

const wchar_t *file_name (void) {

	return ed.path ? ed.path : L"Untitled";
}

static void set_message (int style, const wchar_t *fmt, ...) {

	va_list ap;

	va_start(ap, fmt);
	vswprintf(ed.message, 512, fmt, ap);
	va_end(ap);
	ed.message_style = style;
}

/* screen column of position 'pos' of the line */
int col_of (const LINE *l, int pos) {

	int i = 0, col = 0, w, kind;

	while (i < pos && i < l->len) {
		i = cell_next(l->s, l->len, i, col, &w, &kind);
		col += w;
	}
	return col;
}

/* position of the line at column 'want' (or the last one before it) */
int pos_of (const LINE *l, int want) {

	int i = 0, col = 0, w, kind, next;

	while (i < l->len) {
		next = cell_next(l->s, l->len, i, col, &w, &kind);
		if (col + w > want)
			break;
		col += w;
		i = next;
	}
	return i;
}

/* selection in text order; false if there is none */
bool selection_range (int *y1, int *x1, int *y2, int *x2) {

	if (!ed.selecting || (ed.ay == ed.cy && ed.ax == ed.cx))
		return false;
	if (ed.ay < ed.cy || (ed.ay == ed.cy && ed.ax < ed.cx)) {
		*y1 = ed.ay; *x1 = ed.ax;
		*y2 = ed.cy; *x2 = ed.cx;
	} else {
		*y1 = ed.cy; *x1 = ed.cx;
		*y2 = ed.ay; *x2 = ed.ax;
	}
	return true;
}

static bool has_selection (void) {

	int y1, x1, y2, x2;
	return selection_range(&y1, &x1, &y2, &x2);
}

/* puts the cursor in the middle of the screen if it is out of view */
static void center_cursor (void) {

	int rows = view_rows();

	if (ed.cy < ed.top || ed.cy >= ed.top + rows) {
		ed.top = ed.cy - rows / 2;
		ed.top_row = 0;
		if (ed.top < 0)
			ed.top = 0;
	}
	ed.follow = true;
}

/*---------------------------------------------------------- changes */

/* inserts s[0..n) at (y, x); the cursor goes to its end */
static void edit_insert (int y, int x, const wchar_t *s, int n) {

	int ey, ex;

	if (n <= 0)
		return;
	history_add(&ed.history, true, y, x, s, n);
	buffer_insert(&ed.buf, y, x, s, n, &ey, &ex);
	ed.cy = ey;
	ed.cx = ex;
}

/* removes the text from (y1, x1) to (y2, x2); the cursor goes to its start */
static void edit_delete (int y1, int x1, int y2, int x2) {

	wchar_t *t;
	int n;

	if (y1 == y2 && x1 == x2)
		return;
	t = buffer_text(&ed.buf, y1, x1, y2, x2, &n);
	history_add(&ed.history, false, y1, x1, t, n);
	free(t);
	buffer_delete(&ed.buf, y1, x1, y2, x2);
	ed.cy = y1;
	ed.cx = x1;
}

static bool delete_selection (void) {

	int y1, x1, y2, x2;
	bool had = selection_range(&y1, &x1, &y2, &x2);

	if (had)
		edit_delete(y1, x1, y2, x2);
	ed.selecting = false;
	return had;
}

/* typed text replaces the selection; 'merge' keeps the rest of a paste in the same undo step */
static void type_text (const wchar_t *s, int n, bool merge) {

	history_begin(&ed.history, has_selection() ? EDIT_OTHER : EDIT_TYPE, ed.cy, ed.cx, merge);
	delete_selection();
	edit_insert(ed.cy, ed.cx, s, n);
}

/* Enter: breaks the line and repeats the indentation (not when pasting: the text brings its own) */
static void new_line (bool merge) {

	wchar_t *t;
	int indent = 0;

	history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, merge);
	delete_selection();
	if (settings.auto_indent && !pasting)
		while (indent < ed.cx && (LN->s[indent] == L' ' || LN->s[indent] == L'\t'))
			indent++;

	t = xrealloc(NULL, (indent + 1) * sizeof(wchar_t));
	t[0] = L'\n';
	memcpy(t + 1, LN->s, indent * sizeof(wchar_t));
	edit_insert(ed.cy, ed.cx, t, indent + 1);
	free(t);
}

/*---------------------------------------------------------- words */

/* 0: blank, 1: word, 2: punctuation */
static int char_class (wchar_t c) {

	if (c == L' ' || c == L'\t')
		return 0;
	if (c == L'_' || iswalnum(c))
		return 1;
	return 2;
}

/* start of the previous word (crosses to the end of the line above at the start of a line) */
static void word_left (int *y, int *x) {

	LINE *l = &ed.buf.lines[*y];
	int i = *x;

	if (i == 0) {
		if (*y > 0) {
			(*y)--;
			*x = ed.buf.lines[*y].len;
		}
		return;
	}
	while (i > 0 && char_class(l->s[i-1]) == 0)
		i--;
	if (i > 0) {
		int c = char_class(l->s[i-1]);
		while (i > 0 && char_class(l->s[i-1]) == c)
			i--;
	}
	*x = i;
}

/* start of the next word */
static void word_right (int *y, int *x) {

	LINE *l = &ed.buf.lines[*y];
	int i = *x;

	if (i >= l->len) {
		if (*y < ed.buf.count - 1) {
			(*y)++;
			*x = 0;
		}
		return;
	}
	if (char_class(l->s[i]) != 0) {
		int c = char_class(l->s[i]);
		while (i < l->len && char_class(l->s[i]) == c)
			i++;
	}
	while (i < l->len && char_class(l->s[i]) == 0)
		i++;
	*x = i;
}

/* selects the word (or run of blanks or punctuation) at (y, x) */
static void select_word (int y, int x) {

	LINE *l = &ed.buf.lines[y];
	int a = x, b = x, c;

	if (x < l->len)
		c = char_class(l->s[x]);
	else if (x > 0)
		c = char_class(l->s[--a]);
	else
		return;
	while (a > 0 && char_class(l->s[a-1]) == c)
		a--;
	while (b < l->len && char_class(l->s[b]) == c)
		b++;
	ed.ay = y;
	ed.ax = a;
	ed.cy = y;
	ed.cx = b;
	ed.selecting = true;
}

/*---------------------------------------------------------- deleting */

static void backspace (bool word, bool merge) {

	int y = ed.cy, x = ed.cx;

	if (has_selection()) {
		history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, merge);
		delete_selection();
		return;
	}
	ed.selecting = false;
	history_begin(&ed.history, word ? EDIT_OTHER : EDIT_BACKSPACE, ed.cy, ed.cx, merge);
	if (word)
		word_left(&y, &x);
	else if (x > 0)
		x = ucd_grapheme_prev(LN->s, LN->len, x);
	else if (y > 0) {
		y--;
		x = ed.buf.lines[y].len;
	}
	edit_delete(y, x, ed.cy, ed.cx);
}

static void delete_forward (bool word, bool merge) {

	int y = ed.cy, x = ed.cx;

	if (has_selection()) {
		history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, merge);
		delete_selection();
		return;
	}
	ed.selecting = false;
	history_begin(&ed.history, word ? EDIT_OTHER : EDIT_DELETE, ed.cy, ed.cx, merge);
	if (word)
		word_right(&y, &x);
	else if (x < LN->len)
		x = ucd_grapheme_next(LN->s, LN->len, x, NULL);
	else if (y < ed.buf.count - 1) {
		y++;
		x = 0;
	}
	edit_delete(ed.cy, ed.cx, y, x);
}

/*---------------------------------------------------------- indentation */

/* lines touched by the selection (a selection ending at column 0 leaves that line out) */
static void selected_lines (int *first, int *last) {

	int y1, x1, y2, x2;

	if (!selection_range(&y1, &x1, &y2, &x2)) {
		*first = *last = ed.cy;
		return;
	}
	*first = y1;
	*last = (x2 == 0 && y2 > y1) ? y2 - 1 : y2;
}

/* one level of indentation typed at column 'col': a tab, or spaces up to the next tab stop (expand_tabs) */
static int indent_text (wchar_t *out, int col) {

	int n;

	if (!settings.expand_tabs) {
		out[0] = L'\t';
		return 1;
	}
	n = settings.tab_size - col % settings.tab_size;
	wmemset(out, L' ', n);
	return n;
}

/* Tab / Shift+Tab over several lines: one level more or less on each, then selects them whole */
static void indent_lines (bool out) {

	int first, last, y, cy = ed.cy, cx = ed.cx, removed = 0;
	bool had = has_selection();
	wchar_t unit[16];

	selected_lines(&first, &last);
	history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, false);
	for (y = first; y <= last; y++) {
		LINE *l = &ed.buf.lines[y];
		int n = 0;
		if (!out) {
			if (l->len > 0)
				edit_insert(y, 0, unit, indent_text(unit, 0));
			continue;
		}
		if (l->len > 0 && l->s[0] == L'\t')
			n = 1;
		else
			while (n < l->len && n < settings.tab_size && l->s[n] == L' ')
				n++;
		edit_delete(y, 0, y, n);
		removed = n;
	}

	/* Shift+Tab on a line without selection: the cursor stays on its text */
	if (!had) {
		ed.cy = cy;
		ed.cx = cx > removed ? cx - removed : 0;
		ed.selecting = false;
		return;
	}
	ed.ay = first;
	ed.ax = 0;
	ed.cy = last;
	ed.cx = ed.buf.lines[last].len;
	ed.selecting = first != last || ed.cx > 0;
}

/* Tab over several selected lines (or Shift+Tab) indents; otherwise it types a tab */
static void tab_key (bool shift, bool merge) {

	int first, last, y1, x1, y2, x2;
	wchar_t unit[16];

	selected_lines(&first, &last);
	if (!pasting && (shift || (has_selection() && first != last)))
		indent_lines(shift);
	else if (!shift) {
		/* the tab replaces the selection: its stop counts from where the selection starts */
		if (!selection_range(&y1, &x1, &y2, &x2)) {
			y1 = ed.cy;
			x1 = ed.cx;
		}
		/* a pasted tab stays a tab */
		if (pasting)
			type_text(L"\t", 1, merge);
		else
			type_text(unit, indent_text(unit, col_of(&ed.buf.lines[y1], x1)), merge);
	}
}

/*---------------------------------------------------------- moving */

static void move_left (void) {

	if (ed.cx > 0)
		ed.cx = ucd_grapheme_prev(LN->s, LN->len, ed.cx);
	else if (ed.cy > 0) {
		ed.cy--;
		ed.cx = LN->len;
	}
}

static void move_right (void) {

	if (ed.cx < LN->len)
		ed.cx = ucd_grapheme_next(LN->s, LN->len, ed.cx, NULL);
	else if (ed.cy < ed.buf.count - 1) {
		ed.cy++;
		ed.cx = 0;
	}
}

/* 'dy' rows up or down keeping the column (with word wrap, rows on the screen, not lines) */
static void move_vertical (int dy) {

	int row = wrap_row_of(LN, ed.cx), start, end;

	if (ed.want_col < 0) {
		start = wrap_bounds(LN, row, &end);
		ed.want_col = wrap_col(LN, start, ed.cx);
	}
	for (; dy < 0 && wrap_step(&ed.cy, &row, -1); dy++)
		;
	for (; dy > 0 && wrap_step(&ed.cy, &row, 1); dy--)
		;
	start = wrap_bounds(LN, row, &end);
	ed.cx = wrap_pos(LN, start, end, ed.want_col);
}

/* scrolls the view by 'n' rows (negative: up) */
static void scroll_rows (int n) {

	for (; n < 0 && wrap_step(&ed.top, &ed.top_row, -1); n++)
		;
	for (; n > 0 && wrap_step(&ed.top, &ed.top_row, 1); n--)
		;
}

/* Home: switches between the first non-blank character and the start of the line */
static void move_home (void) {

	int first = 0;

	while (first < LN->len && (LN->s[first] == L' ' || LN->s[first] == L'\t'))
		first++;
	ed.cx = ed.cx == first ? 0 : first;
}

/* cursor keys; Shift extends the selection. Returns true for vertical moves (they keep want_col) */
static bool move_key (WORD vk, bool ctrl, bool shift) {

	int y1, x1, y2, x2, rows = view_rows();
	bool sel = selection_range(&y1, &x1, &y2, &x2);

	/* ← / → without Shift collapse the selection to its edge */
	if (!shift && !ctrl && sel && (vk == VK_LEFT || vk == VK_RIGHT)) {
		ed.cy = vk == VK_LEFT ? y1 : y2;
		ed.cx = vk == VK_LEFT ? x1 : x2;
		ed.selecting = false;
		return false;
	}
	if (shift && !ed.selecting) {
		ed.ay = ed.cy;
		ed.ax = ed.cx;
		ed.selecting = true;
	} else if (!shift)
		ed.selecting = false;

	switch (vk) {
	case VK_LEFT:
		if (ctrl)
			word_left(&ed.cy, &ed.cx);
		else
			move_left();
		return false;
	case VK_RIGHT:
		if (ctrl)
			word_right(&ed.cy, &ed.cx);
		else
			move_right();
		return false;
	case VK_UP:
		move_vertical(-1);
		return true;
	case VK_DOWN:
		move_vertical(1);
		return true;
	case VK_PRIOR:
		scroll_rows(-rows);
		move_vertical(-rows);
		return true;
	case VK_NEXT:
		scroll_rows(rows);
		move_vertical(rows);
		return true;
	case VK_HOME:
		if (ctrl)
			ed.cy = ed.cx = 0;
		else
			move_home();
		return false;
	case VK_END:
		if (ctrl)
			ed.cy = ed.buf.count - 1;
		ed.cx = LN->len;
		return false;
	}
	return false;
}

/*---------------------------------------------------------- clipboard */

static void copy (bool cut) {

	int y1, x1, y2, x2, n;
	wchar_t *t;

	if (selection_range(&y1, &x1, &y2, &x2)) {
		t = buffer_text(&ed.buf, y1, x1, y2, x2, &n);
		clipboard_set(t, n);
		free(t);
		free(line_clip);
		line_clip = NULL;
		if (cut) {
			history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, false);
			delete_selection();
		}
		return;
	}

	/* nothing selected: the whole line */
	n = LN->len + 1;
	t = xrealloc(NULL, (n + 1) * sizeof(wchar_t));
	memcpy(t, LN->s, LN->len * sizeof(wchar_t));
	t[n - 1] = L'\n';
	t[n] = 0;
	clipboard_set(t, n);
	free(line_clip);
	line_clip = t;

	if (cut) {
		history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, false);
		if (ed.cy < ed.buf.count - 1)
			edit_delete(ed.cy, 0, ed.cy + 1, 0);
		else if (ed.cy > 0)
			edit_delete(ed.cy - 1, ed.buf.lines[ed.cy - 1].len, ed.cy, LN->len);
		else
			edit_delete(0, 0, 0, LN->len);
		ed.cx = 0;
	}
}

static void paste_clipboard (void) {

	int n, y = ed.cy, x = ed.cx;
	wchar_t *t = clipboard_get(&n);

	if (!t || n == 0) {
		free(t);
		return;
	}
	history_begin(&ed.history, EDIT_OTHER, ed.cy, ed.cx, false);
	if (line_clip && !has_selection() && !wcscmp(line_clip, t)) {
		/* a whole line copied goes above the current one; the cursor stays on its line */
		edit_insert(y, 0, t, n);
		ed.cy = y + 1;
		ed.cx = x;
	} else {
		delete_selection();
		edit_insert(ed.cy, ed.cx, t, n);
	}
	free(t);
}

/*---------------------------------------------------------- undo */

static void clamp_cursor (void) {

	if (ed.cy > ed.buf.count - 1)
		ed.cy = ed.buf.count - 1;
	if (ed.cy < 0)
		ed.cy = 0;
	if (ed.cx > LN->len)
		ed.cx = LN->len;
}

static void undo (bool redo) {

	int y = ed.cy, x = ed.cx;
	bool done = redo ? history_redo(&ed.history, &ed.buf, &y, &x) : history_undo(&ed.history, &ed.buf, &y, &x);

	if (!done) {
		set_message(S_STATUS_MESSAGE, redo ? L" Nothing to redo" : L" Nothing to undo");
		return;
	}
	ed.cy = y;
	ed.cx = x;
	ed.selecting = false;
	clamp_cursor();
	center_cursor();
}

/*---------------------------------------------------------- search */

/* upper case in the query makes the search case sensitive */
static bool exact_case (const wchar_t *q) {

	for (; *q; q++)
		if (iswupper(*q))
			return true;
	return false;
}

static bool match_at (const LINE *l, int pos, const wchar_t *q, int n, bool exact) {

	int i;

	for (i = 0; i < n; i++) {
		wchar_t a = l->s[pos + i], b = q[i];
		if (a != b && (exact || towlower(a) != towlower(b)))
			return false;
	}
	return true;
}

/* first match starting at or after (y1, x1) and before (y2, x2) */
static bool search_forward (int y1, int x1, int y2, int x2, int *fy, int *fx) {

	int n = (int) wcslen(ed.find), y, pos;
	bool exact = exact_case(ed.find);

	for (y = y1; y <= y2 && y < ed.buf.count; y++) {
		LINE *l = &ed.buf.lines[y];
		int last = l->len - n;
		if (y == y2 && x2 - 1 < last)
			last = x2 - 1;
		for (pos = y == y1 ? x1 : 0; pos <= last; pos++)
			if (match_at(l, pos, ed.find, n, exact)) {
				*fy = y;
				*fx = pos;
				return true;
			}
	}
	return false;
}

/* last match starting at or after (y1, x1) and before (y2, x2) */
static bool search_backward (int y1, int x1, int y2, int x2, int *fy, int *fx) {

	int n = (int) wcslen(ed.find), y, pos;
	bool exact = exact_case(ed.find);

	for (y = y2; y >= y1 && y >= 0; y--) {
		LINE *l = &ed.buf.lines[y];
		int first = y == y1 ? x1 : 0, hi = l->len - n;
		if (y == y2 && x2 - 1 < hi)
			hi = x2 - 1;
		for (pos = hi; pos >= first; pos--)
			if (match_at(l, pos, ed.find, n, exact)) {
				*fy = y;
				*fx = pos;
				return true;
			}
	}
	return false;
}

static void select_match (int y, int x) {

	ed.ay = y;
	ed.ax = x;
	ed.cy = y;
	ed.cx = x + (int) wcslen(ed.find);
	ed.selecting = true;
	center_cursor();
}

/* next (or previous) match from the selection, wrapping around the file */
static bool find_next (bool backward) {

	int y1, x1, y2, x2, fy, fx, end = ed.buf.count - 1;
	bool found, wrapped = false;

	if (!ed.find[0])
		return false;
	if (!selection_range(&y1, &x1, &y2, &x2)) {
		y1 = y2 = ed.cy;
		x1 = x2 = ed.cx;
	}

	if (!backward) {
		found = search_forward(y2, x2, end, INT_MAX, &fy, &fx);
		if (!found && (found = search_forward(0, 0, y2, x2, &fy, &fx)))
			wrapped = true;
	} else {
		found = search_backward(0, 0, y1, x1, &fy, &fx);
		if (!found && (found = search_backward(y1, x1, end, INT_MAX, &fy, &fx)))
			wrapped = true;
	}

	if (!found) {
		set_message(S_STATUS_ERROR, L" Not found: %ls", ed.find);
		return false;
	}
	select_match(fy, fx);
	if (wrapped)
		set_message(S_STATUS_MESSAGE, backward ? L" Search wrapped to the end" : L" Search wrapped to the beginning");
	return true;
}

/* a one-line selection is the default text to search for */
static void selection_to_find (void) {

	int y1, x1, y2, x2, n;

	if (selection_range(&y1, &x1, &y2, &x2) && y1 == y2 && x2 - x1 < FIELD_MAX) {
		n = x2 - x1;
		memcpy(ed.find, ed.buf.lines[y1].s + x1, n * sizeof(wchar_t));
		ed.find[n] = 0;
	}
}

static void find (void) {

	static const wchar_t *labels[] = { L"Find what:" };
	wchar_t *values[] = { ed.find };

	selection_to_find();
	if (dialog_form(L"Find", labels, values, 1, false))
		find_next(false);
}

/* asks at each match: Replace, Skip, All or Cancel. All the replacements are one undo step */
static void replace (void) {

	static const wchar_t *labels[] = { L"Find what:", L"Replace with:" };
	static const wchar_t *buttons[] = { L"&Replace", L"&Skip", L"&All", L"&Cancel" };
	wchar_t *values[] = { ed.find, ed.replace };
	int n, rn, sy, sx, y, x, fy, fx, y2, x2, phase, count = 0;
	bool all = false, first = true;

	selection_to_find();
	if (!dialog_form(L"Replace", labels, values, 2, false) || !ed.find[0])
		return;
	n = (int) wcslen(ed.find);
	rn = (int) wcslen(ed.replace);
	if (!selection_range(&sy, &sx, &y2, &x2)) {
		sy = ed.cy;
		sx = ed.cx;
	}

	/* from the cursor to the end, then from the top back to the cursor */
	for (phase = 0; phase < 2; phase++) {
		y = phase == 0 ? sy : 0;
		x = phase == 0 ? sx : 0;
		for (;;) {
			bool found = phase == 0 ? search_forward(y, x, ed.buf.count - 1, INT_MAX, &fy, &fx)
				: search_forward(y, x, sy, sx, &fy, &fx);
			int answer = 2;
			if (!found)
				break;

			select_match(fy, fx);
			if (!all) {
				render_scroll();
				answer = dialog_ask(L"Replace", L"Replace this occurrence?", buttons, 4,
					fy - ed.top < view_rows() / 2 ? PLACE_BOTTOM : PLACE_TOP);
			}
			if (answer == 3)
				goto done;
			if (answer == 1) {
				y = fy;
				x = fx + n;
				continue;
			}
			if (answer == 2)
				all = true;

			history_begin(&ed.history, EDIT_OTHER, fy, fx, !first);
			first = false;
			edit_delete(fy, fx, fy, fx + n);
			edit_insert(fy, fx, ed.replace, rn);
			ed.selecting = false;
			count++;
			if (phase == 1 && fy == sy && fx < sx)
				sx += rn - n;
			y = fy;
			x = fx + rn;
		}
	}
done:
	ed.selecting = false;
	set_message(S_STATUS_MESSAGE, L" %d occurrence%ls replaced", count, count == 1 ? L"" : L"s");
}

static void goto_line (void) {

	static wchar_t number[FIELD_MAX];
	wchar_t label[64];
	const wchar_t *labels[] = { label };
	wchar_t *values[] = { number };
	int n;

	swprintf(label, 64, L"Line number (1-%d):", ed.buf.count);
	number[0] = 0;
	if (!dialog_form(L"Go to Line", labels, values, 1, false))
		return;
	n = _wtoi(number);
	if (n < 1)
		n = 1;
	if (n > ed.buf.count)
		n = ed.buf.count;
	ed.cy = n - 1;
	ed.cx = 0;
	ed.selecting = false;
	center_cursor();
}

/*---------------------------------------------------------- files */

static void reset_view (void) {

	ed.cy = ed.cx = ed.top = ed.top_row = ed.left = 0;
	ed.selecting = false;
	ed.want_col = -1;
	ed.follow = true;
}

/*
 * Recovery copy: when the window closes with unsaved changes, the text goes to
 * "<file>.note-recovery" ("untitled.note-recovery" in the current folder). Opening
 * the file again offers it back; saving the file removes it.
 */
static wchar_t *recovery_path (const wchar_t *path) {

	static wchar_t buf[FIELD_MAX + 32];

	swprintf(buf, FIELD_MAX + 32, L"%ls.note-recovery", path ? path : L"untitled");
	return buf;
}

/* called by the console when the window closes (on another thread, with the editor locked) */
static void write_recovery (void) {

	if (is_modified())
		buffer_save(&ed.buf, recovery_path(ed.path));
}

/* a recovery copy of the file just opened: Open it, Delete it or Ignore it (asked again next time) */
static void check_recovery (void) {

	static const wchar_t *buttons[] = { L"&Open", L"&Delete", L"&Ignore" };
	static wchar_t text[FIELD_MAX + 64];
	const wchar_t *copy;
	BUFFER b;

	if (!ed.path)
		return;
	copy = recovery_path(ed.path);
	if (GetFileAttributesW(copy) == INVALID_FILE_ATTRIBUTES)
		return;

	swprintf(text, FIELD_MAX + 64, L"Unsaved changes to %ls were kept when note closed.", ed.path);
	switch (dialog_ask(L"Recovery", text, buttons, 3, PLACE_CENTER)) {
	case 0:
		buffer_init(&b);
		if (buffer_load(&b, copy) != 0) {
			set_message(S_STATUS_ERROR, L" Could not open %ls: %ls", copy, system_message(GetLastError()));
			buffer_free(&b);
			return;
		}
		buffer_free(&ed.buf);
		ed.buf = b;
		history_clear(&ed.history);
		history_unsaved(&ed.history);
		set_message(S_STATUS_MESSAGE, L" Recovered copy opened: save to keep it");
		break;
	case 1:
		DeleteFileW(copy);
		break;
	}
}

/* 'as' (or untitled) asks for a name first */
static bool save (bool as) {

	static const wchar_t *labels[] = { L"File name:" };
	static const wchar_t *buttons[] = { L"&UTF-8", L"&ANSI", L"&Cancel" };
	static wchar_t name[FIELD_MAX];
	wchar_t *values[] = { name };
	const wchar_t *path = ed.path;

	if (as || !ed.path) {
		wcsncpy(name, ed.path ? ed.path : L"", FIELD_MAX - 1);
		name[FIELD_MAX - 1] = 0;
		if (!dialog_form(L"Save As", labels, values, 1, true))
			return false;
		path = name;
	}

	/* an ANSI file got characters its code page does not have: they would turn into '?' */
	if (!buffer_fits(&ed.buf)) {
		switch (dialog_ask(L"Save", L"Some characters do not exist in ANSI. Save as:", buttons, 3, PLACE_CENTER)) {
		case 0:
			ed.buf.encoding = ENC_UTF8;
			ed.buf.bom = false;
			break;
		case 2:
			return false;
		}
	}

	if (!buffer_save(&ed.buf, path)) {
		set_message(S_STATUS_ERROR, L" Could not save %ls: %ls", path, system_message(GetLastError()));
		return false;
	}
	if (path != ed.path) {
		free(ed.path);
		ed.path = _wcsdup(path);
		ed.lang = syntax_detect(ed.path);
		ed.buf.valid = 0;
	}
	history_saved(&ed.history);
	DeleteFileW(recovery_path(ed.path));
	set_message(S_STATUS_MESSAGE, L" Saved %ls (%d line%ls)", ed.path, ed.buf.count, ed.buf.count == 1 ? L"" : L"s");
	return true;
}

/* unsaved changes: Save, Don't Save or Cancel. false if the user cancelled (or the save failed) */
static bool ask_save_changes (void) {

	static const wchar_t *buttons[] = { L"&Save", L"Do&n't Save", L"&Cancel" };
	static wchar_t text[FIELD_MAX + 64];

	if (!is_modified())
		return true;
	swprintf(text, FIELD_MAX + 64, L"Save changes to %ls?", file_name());
	switch (dialog_ask(L"note", text, buttons, 3, PLACE_CENTER)) {
	case 0:
		return save(false);
	case 1:
		return true;
	}
	return false;
}

static bool load (const wchar_t *path) {

	BUFFER b;
	int r;

	buffer_init(&b);
	r = buffer_load(&b, path);
	if (r < 0) {
		set_message(S_STATUS_ERROR, L" Could not open %ls: %ls", path, system_message(GetLastError()));
		buffer_free(&b);
		return false;
	}
	buffer_free(&ed.buf);
	ed.buf = b;
	history_clear(&ed.history);
	free(ed.path);
	ed.path = _wcsdup(path);
	ed.lang = syntax_detect(path);
	reset_view();
	if (r == 1)
		set_message(S_STATUS_MESSAGE, L" New file");
	check_recovery();
	return true;
}

static void open_file (void) {

	static const wchar_t *labels[] = { L"File name:" };
	static wchar_t name[FIELD_MAX];
	wchar_t *values[] = { name };

	if (!ask_save_changes())
		return;
	name[0] = 0;
	if (dialog_form(L"Open", labels, values, 1, true))
		load(name);
}

static void new_file (void) {

	if (!ask_save_changes())
		return;
	buffer_free(&ed.buf);
	buffer_init(&ed.buf);
	history_clear(&ed.history);
	free(ed.path);
	ed.path = NULL;
	ed.lang = LANG_NONE;
	reset_view();
}

static void exit_editor (void) {

	static const wchar_t *buttons[] = { L"&Exit", L"&Cancel" };

	if (is_modified()) {
		if (ask_save_changes())
			ed.quit = true;
	} else if (dialog_ask(L"note", L"Exit note?", buttons, 2, PLACE_CENTER) == 0)
		ed.quit = true;
}

/*---------------------------------------------------------- help */

static void show_help (void) {

	static const wchar_t *lines[] = {
		L"Ctrl+N  Ctrl+O  Ctrl+S   New, open, save",
		L"Esc                      Exit (asks first)",
		L"",
		L"Ctrl+Z  Ctrl+Y           Undo, redo",
		L"Ctrl+X  Ctrl+C  Ctrl+V   Cut, copy, paste (no selection: the line)",
		L"Ctrl+A                   Select all",
		L"Shift + arrows           Select",
		L"Ctrl + ← →               Move by word",
		L"Ctrl+Backspace/Delete    Delete a word",
		L"Tab  Shift+Tab           Indent, unindent the selected lines",
		L"",
		L"Ctrl+F  F3  Shift+F3     Find, next, previous",
		L"Ctrl+H                   Replace",
		L"Ctrl+G                   Go to line",
		L"",
		L"Alt+Z                    Word wrap",
		L"Alt  F10                 Menu bar (Alt+letter opens a menu)",
		L"Mouse                    Click, drag to select, double click",
		L"                         selects a word, the wheel scrolls",
	};

	dialog_info(L"Keyboard Shortcuts", lines, (int) (sizeof lines / sizeof lines[0]));
}

static void show_about (void) {

	static const wchar_t *lines[] = {
		L"note 1.0",
		L"A simple text editor for the console",
		L"",
		L"Settings and colors: note.cfg next to note.exe",
	};

	dialog_info(L"About", lines, (int) (sizeof lines / sizeof lines[0]));
}

/*---------------------------------------------------------- commands */

/* state of a toggle command for the menu check mark; -1: not a toggle */
int command_checked (int cmd) {

	switch (cmd) {
	case CMD_WORD_WRAP:
		return settings.word_wrap;
	case CMD_LINE_NUMBERS:
		return settings.line_numbers;
	case CMD_SYNTAX:
		return settings.syntax;
	case CMD_HIGHLIGHT_LINE:
		return settings.highlight_line;
	}
	return -1;
}

void run_command (int cmd) {

	ed.follow = true;
	switch (cmd) {
	case CMD_NEW:            new_file(); break;
	case CMD_OPEN:           open_file(); break;
	case CMD_SAVE:           save(false); break;
	case CMD_SAVE_AS:        save(true); break;
	case CMD_EXIT:           exit_editor(); break;
	case CMD_UNDO:           undo(false); break;
	case CMD_REDO:           undo(true); break;
	case CMD_CUT:            copy(true); break;
	case CMD_COPY:           copy(false); break;
	case CMD_PASTE:          paste_clipboard(); break;
	case CMD_DELETE:         delete_forward(false, false); break;
	case CMD_FIND:           find(); break;
	case CMD_FIND_NEXT:      if (ed.find[0]) find_next(false); else find(); break;
	case CMD_FIND_PREVIOUS:  if (ed.find[0]) find_next(true); else find(); break;
	case CMD_REPLACE:        replace(); break;
	case CMD_GOTO_LINE:      goto_line(); break;
	case CMD_LINE_NUMBERS:   settings.line_numbers = !settings.line_numbers; break;
	case CMD_WORD_WRAP:
		settings.word_wrap = !settings.word_wrap;
		ed.left = ed.top_row = 0;
		break;
	case CMD_SYNTAX:         settings.syntax = !settings.syntax; break;
	case CMD_HIGHLIGHT_LINE: settings.highlight_line = !settings.highlight_line; break;
	case CMD_HELP:           show_help(); break;
	case CMD_ABOUT:          show_about(); break;
	case CMD_SELECT_ALL:
		ed.ay = ed.ax = 0;
		ed.cy = ed.buf.count - 1;
		ed.cx = LN->len;
		ed.selecting = true;
		break;
	}
	ed.want_col = -1;
}

/*---------------------------------------------------------- input */

static void handle_key (const KEY_EVENT_RECORD *k) {

	DWORD st = k->dwControlKeyState;
	bool altgr = (st & RIGHT_ALT_PRESSED) && (st & LEFT_CTRL_PRESSED);
	bool ctrl  = (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) && !altgr;
	bool alt   = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) && !altgr;
	bool shift = (st & SHIFT_PRESSED) != 0;
	static bool burst;   /* the last key had more keys queued behind it */
	bool pending = screen_pending();
	bool merge = burst;  /* a paste goes on: same undo step */
	bool vertical = false;
	WORD vk = k->wVirtualKeyCode;
	wchar_t c = k->uChar.UnicodeChar;

	pasting = pending || burst;
	burst = pending;
	ed.message[0] = 0;
	ed.follow = true;

	if (alt && !ctrl) {
		int m = menu_hotkey(vk);
		if (vk == 'Z')
			run_command(CMD_WORD_WRAP);
		else if (m >= 0)
			run_command(menu_run(m, true));
		return;
	}

	if (ctrl && !shift) {
		static const struct { WORD vk; int cmd; } keys[] = {
			{ 'N', CMD_NEW }, { 'O', CMD_OPEN }, { 'S', CMD_SAVE }, { 'Z', CMD_UNDO },
			{ 'Y', CMD_REDO }, { 'X', CMD_CUT }, { 'C', CMD_COPY }, { 'V', CMD_PASTE },
			{ 'A', CMD_SELECT_ALL }, { 'F', CMD_FIND }, { 'H', CMD_REPLACE }, { 'G', CMD_GOTO_LINE },
		};
		int i;
		for (i = 0; i < (int) (sizeof keys / sizeof keys[0]); i++)
			if (keys[i].vk == vk) {
				run_command(keys[i].cmd);
				return;
			}
	}

	switch (vk) {
	case VK_ESCAPE:
		run_command(CMD_EXIT);
		break;
	case VK_F1:
		run_command(CMD_HELP);
		break;
	case VK_F3:
		run_command(shift ? CMD_FIND_PREVIOUS : CMD_FIND_NEXT);
		break;
	case VK_F10:
		run_command(menu_run(0, false));
		break;
	case VK_RETURN:
		new_line(merge);
		break;
	case VK_BACK:
		backspace(ctrl, merge);
		break;
	case VK_DELETE:
		if (shift)
			copy(true);
		else
			delete_forward(ctrl, merge);
		break;
	case VK_INSERT:
		if (shift)
			paste_clipboard();
		else if (ctrl)
			copy(false);
		break;
	case VK_TAB:
		tab_key(shift, merge);
		break;
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_HOME:
	case VK_END:
		vertical = move_key(vk, ctrl, shift);
		break;
	default:
		if (!ctrl && !alt && c >= 0x20 && c != 0x7f)
			type_text(&c, 1, merge);
	}

	if (!vertical)
		ed.want_col = -1;
}

static void handle_mouse (const EVENT *ev) {

	int y, x;

	switch (ev->mouse) {
	case MOUSE_WHEEL:
		scroll_rows(-3 * ev->wheel);
		ed.follow = false;
		return;

	case MOUSE_DOWN:
	case MOUSE_DOUBLE:
		ed.message[0] = 0;
		if (ev->y == 0) {
			int m = menu_at(ev->x);
			if (m >= 0)
				run_command(menu_run(m, true));
			return;
		}
		if (ev->y >= screen_h - 1)
			return;
		render_hit(ev->x, ev->y, &y, &x);
		ed.want_col = -1;
		ed.follow = true;
		if (ev->mouse == MOUSE_DOUBLE) {
			select_word(y, x);
			mouse_selecting = false;
			return;
		}
		if (ev->mods & SHIFT_PRESSED) {
			if (!ed.selecting) {
				ed.ay = ed.cy;
				ed.ax = ed.cx;
				ed.selecting = true;
			}
		} else {
			ed.ay = y;
			ed.ax = x;
			ed.selecting = true;
		}
		ed.cy = y;
		ed.cx = x;
		mouse_selecting = true;
		return;

	case MOUSE_DRAG:
		if (!mouse_selecting)
			return;
		/* above or below the text scrolls one line */
		render_hit(ev->x, ev->y, &y, &x);
		ed.cy = y;
		ed.cx = x;
		ed.follow = true;
		return;

	case MOUSE_UP:
		mouse_selecting = false;
		return;
	}
}

/* opens 'file' (NULL: untitled) with the cursor at 'line' and 'col' (from 1; 0: not given) */
int editor_run (const wchar_t *file, int line, int col) {

	wchar_t config_error[256] = L"";
	bool config_ok = config_load(config_error, 256);
	EVENT ev;

	buffer_init(&ed.buf);
	reset_view();
	if (file) {
		int r = buffer_load(&ed.buf, file);
		if (r < 0) {
			console_write(STD_ERROR_HANDLE, L"note: %ls: %ls\n", file, system_message(GetLastError()));
			buffer_free(&ed.buf);
			return 1;
		}
		ed.path = _wcsdup(file);
		ed.lang = syntax_detect(file);
		if (r == 1)
			set_message(S_STATUS_MESSAGE, L" New file");
	}
	if (!config_ok)
		set_message(S_STATUS_ERROR, L" %ls", config_error);

	if (!screen_open()) {
		console_write(STD_ERROR_HANDLE, L"note: needs a console with VT sequences (Windows 10 or newer)\n");
		buffer_free(&ed.buf);
		return 1;
	}
	screen_on_close(write_recovery);
	check_recovery();

	if (line > 0) {
		ed.cy = line > ed.buf.count ? ed.buf.count - 1 : line - 1;
		ed.cx = col > 0 ? col - 1 : 0;
		clamp_cursor();
		center_cursor();
	}

	while (!ed.quit) {
		if (!screen_pending()) {
			render();
			render_cursor();
			out_flush();
		}
		screen_read(&ev);
		switch (ev.type) {
		case EV_END:
			ed.quit = true;
			break;
		case EV_RESIZE:
			screen_update_size();
			break;
		case EV_KEY:
			handle_key(&ev.key);
			break;
		case EV_MOUSE:
			handle_mouse(&ev);
			break;
		case EV_MENU:
			run_command(menu_run(0, false));
			break;
		}
	}

	screen_close();
	history_clear(&ed.history);
	buffer_free(&ed.buf);
	free(ed.path);
	free(line_clip);
	return 0;
}
