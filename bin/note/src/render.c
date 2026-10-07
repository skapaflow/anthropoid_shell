#include <string.h>
#include <wchar.h>

#include "render.h"
#include "editor.h"
#include "screen.h"
#include "config.h"
#include "syntax.h"
#include "menu.h"
#include "ucd.h"

/*************************************************************
 * Drawing of the editor screen:                             *
 *                                                           *
 *   row 0          menu bar (and the file name)             *
 *   rows 1..h-2    line numbers + text                      *
 *   row h-1        status bar                               *
 *                                                           *
 * The text is laid out in visual rows. Without word wrap    *
 * each line is one row (scrolled sideways by ed.left);      *
 * with it a long line takes several rows, broken after a    *
 * blank when there is one. The view starts at row           *
 * ed.top_row of line ed.top.                                *
 *************************************************************/

#define TEXT_TOP 1

static unsigned char *attr;
static int attr_cap;
static int attr_line = -1;   /* line whose styles are in attr */

int gutter_width (void) {

	int digits = 1, n = ed.buf.count;

	if (!settings.line_numbers)
		return 0;
	while (n >= 10) {
		n /= 10;
		digits++;
	}
	return (digits < 3 ? 3 : digits) + 2;
}

int view_rows (void) {

	return screen_h > 2 ? screen_h - 2 : 1;
}

int view_cols (void) {

	int cols = screen_w - gutter_width();
	return cols > 1 ? cols : 1;
}

/*---------------------------------------------------------- visual rows */

/* end of the row of line l that starts at 'start': the widest part that fits, broken after a blank if possible */
static int wrap_end (const LINE *l, int start) {

	int i = start, col = 0, w, kind, next, blank = -1, width = view_cols();

	while (i < l->len) {
		next = cell_next(l->s, l->len, i, col, &w, &kind);
		if (col + w > width)
			return blank > start ? blank : i > start ? i : next;
		if (l->s[i] == L' ' || l->s[i] == L'\t')
			blank = next;
		col += w;
		i = next;
	}
	return l->len;
}

/* number of rows of a line */
int wrap_rows (const LINE *l) {

	int rows = 1, start = 0;

	if (!settings.word_wrap)
		return 1;
	while ((start = wrap_end(l, start)) < l->len)
		rows++;
	return rows;
}

/* row of the line that holds position x (a break point belongs to the row it starts) */
int wrap_row_of (const LINE *l, int x) {

	int row = 0, start = 0, end;

	if (!settings.word_wrap)
		return 0;
	for (;;) {
		end = wrap_end(l, start);
		if (end >= l->len || x < end)
			return row;
		start = end;
		row++;
	}
}

/* start and end of row 'row' of the line (the last row if there are fewer) */
int wrap_bounds (const LINE *l, int row, int *end) {

	int start = 0, e = l->len;

	if (settings.word_wrap)
		for (;;) {
			e = wrap_end(l, start);
			if (row-- == 0 || e >= l->len)
				break;
			start = e;
		}
	*end = e;
	return start;
}

/* one row down (dir 1) or up (-1) from row 'row' of line 'y'; false at the edge of the text */
bool wrap_step (int *y, int *row, int dir) {

	if (dir > 0) {
		if (*row + 1 < wrap_rows(&ed.buf.lines[*y]))
			(*row)++;
		else if (*y + 1 < ed.buf.count) {
			(*y)++;
			*row = 0;
		} else
			return false;
	} else {
		if (*row > 0)
			(*row)--;
		else if (*y > 0) {
			(*y)--;
			*row = wrap_rows(&ed.buf.lines[*y]) - 1;
		} else
			return false;
	}
	return true;
}

/* screen column of position x in the row that starts at 'start' */
int wrap_col (const LINE *l, int start, int x) {

	int i = start, col = 0, w, kind;

	while (i < x && i < l->len) {
		i = cell_next(l->s, l->len, i, col, &w, &kind);
		col += w;
	}
	return col;
}

/* position at column 'want' of the row [start, end); the break of a row that goes on is never picked */
int wrap_pos (const LINE *l, int start, int end, int want) {

	int i = start, col = 0, w, kind, next;

	while (i < end) {
		next = cell_next(l->s, l->len, i, col, &w, &kind);
		if (col + w > want)
			break;
		col += w;
		i = next;
	}
	if (i == end && end < l->len)
		i = ucd_grapheme_prev(l->s, l->len, end);
	return i;
}

/* the first visible row must exist (lines change under it) */
static void clamp_top (void) {

	int rows;

	if (ed.top > ed.buf.count - 1)
		ed.top = ed.buf.count - 1;
	if (ed.top < 0)
		ed.top = 0;
	rows = wrap_rows(&ed.buf.lines[ed.top]);
	if (ed.top_row > rows - 1)
		ed.top_row = rows - 1;
	if (ed.top_row < 0)
		ed.top_row = 0;
}

/* keeps the cursor inside the visible part */
void render_scroll (void) {

	LINE *l = &ed.buf.lines[ed.cy];
	int rows = view_rows(), crow = wrap_row_of(l, ed.cx), y, row, n;

	clamp_top();
	if (ed.cy < ed.top || (ed.cy == ed.top && crow < ed.top_row)) {
		ed.top = ed.cy;
		ed.top_row = crow;
	} else {
		/* rows from the top of the view down to the cursor */
		y = ed.top;
		row = ed.top_row;
		for (n = 0; n < rows && (y < ed.cy || row < crow); n++)
			wrap_step(&y, &row, 1);
		if (n >= rows) {
			/* the cursor goes on the last row */
			ed.top = ed.cy;
			ed.top_row = crow;
			for (n = 1; n < rows && wrap_step(&ed.top, &ed.top_row, -1); n++)
				;
		}
	}

	if (settings.word_wrap)
		ed.left = 0;
	else {
		int cols = view_cols(), col = col_of(l, ed.cx);
		if (col < ed.left)
			ed.left = col;
		if (col >= ed.left + cols)
			ed.left = col - cols + 1;
	}
}

/*---------------------------------------------------------- drawing */

/* syntax state at the start of line y; the lines above it are scanned once and cached */
static int line_state (int y) {

	BUFFER *b = &ed.buf;

	while (b->valid <= y) {
		int i = b->valid;
		b->lines[i].state = i == 0 ? 0 :
			syntax_line(ed.lang, b->lines[i-1].s, b->lines[i-1].len, b->lines[i-1].state, NULL);
		b->valid++;
	}
	return b->lines[y].state;
}

#define BRACKET_LINES 5000   /* how far the matching bracket is searched */

static int  bracket_y[2], bracket_x[2];
static bool bracket_on;

/* the bracket at the cursor (or just before it) and its partner, nesting counted */
static bool find_brackets (void) {

	static const wchar_t *open = L"([{", *close = L")]}";
	LINE *l = &ed.buf.lines[ed.cy];
	int x = ed.cx, y = ed.cy, depth = 0, lines = 0, dir;
	wchar_t c, partner;

	if (x < l->len && l->s[x] && wcschr(L"()[]{}", l->s[x]))
		;
	else if (x > 0 && l->s[x-1] && wcschr(L"()[]{}", l->s[x-1]))
		x--;
	else
		return false;

	c = l->s[x];
	dir = wcschr(open, c) ? 1 : -1;
	partner = dir > 0 ? close[wcschr(open, c) - open] : open[wcschr(close, c) - close];
	bracket_y[0] = y;
	bracket_x[0] = x;

	for (;;) {
		x += dir;
		while (x < 0 || x >= ed.buf.lines[y].len) {
			y += dir;
			if (y < 0 || y >= ed.buf.count || ++lines > BRACKET_LINES)
				return false;
			x = dir > 0 ? 0 : ed.buf.lines[y].len - 1;
		}
		if (ed.buf.lines[y].s[x] == c)
			depth++;
		else if (ed.buf.lines[y].s[x] == partner && depth-- == 0) {
			bracket_y[1] = y;
			bracket_x[1] = x;
			return true;
		}
	}
}

/* styles of line y into attr: syntax, selection and brackets */
static void line_styles (int y, bool sel, int y1, int x1, int y2, int x2) {

	LINE *l = &ed.buf.lines[y];
	int i;

	if (attr_line == y)
		return;
	attr_line = y;
	if (l->len + 1 > attr_cap) {
		attr_cap = l->len + 256;
		attr = xrealloc(attr, attr_cap);
	}
	if (settings.syntax && ed.lang != LANG_NONE)
		syntax_line(ed.lang, l->s, l->len, line_state(y), attr);
	else
		memset(attr, S_TEXT, l->len);

	if (sel && y >= y1 && y <= y2) {
		int from = y == y1 ? x1 : 0, to = y == y2 ? x2 : l->len;
		for (i = from; i < to; i++)
			attr[i] |= ATTR_SELECTED;
	}
	if (bracket_on)
		for (i = 0; i < 2; i++)
			if (bracket_y[i] == y)
				attr[bracket_x[i]] = S_BRACKET;
}

/* row 'row' of line y on screen row 'screen_row' (y < 0: past the end of the text) */
static void render_row (int y, int row, int screen_row, bool sel, int y1, int x1, int y2, int x2) {

	int gw = gutter_width(), cols = view_cols(), bg = S_TEXT, start, end;
	LINE *l;

	out_goto(screen_row, 0);
	if (y < 0) {
		out_text(NULL, 0, gw + cols, S_TEXT, S_TEXT);
		return;
	}
	l = &ed.buf.lines[y];
	if (settings.highlight_line && y == ed.cy && !sel)
		bg = S_CURRENT_LINE;

	/* the number goes on the first row of the line only */
	if (gw) {
		wchar_t num[24];
		if (row == 0)
			swprintf(num, 24, L" %*d ", gw - 2, y + 1);
		else
			num[0] = 0;
		out_text(num, (int) wcslen(num), gw, y == ed.cy ? S_LINE_NUMBER_CURRENT : S_LINE_NUMBER, bg);
	}

	line_styles(y, sel, y1, x1, y2, x2);
	start = wrap_bounds(l, row, &end);
	out_cells(l->s + start, end - start, attr + start, settings.word_wrap ? 0 : ed.left, cols,
		S_TEXT, bg, sel && y >= y1 && y < y2 && end == l->len);
}

static void render_status (void) {

	wchar_t right[192], selected[48] = L"";
	const wchar_t *left = ed.message[0] ? ed.message : L" F1 Help   Alt Menu   Esc Exit";
	int left_style = ed.message[0] ? ed.message_style : S_STATUS_BAR;
	int y1, x1, y2, x2, lw, rw;

	if (selection_range(&y1, &x1, &y2, &x2)) {
		if (y1 == y2)
			swprintf(selected, 48, L"%d selected   ", x2 - x1);
		else
			swprintf(selected, 48, L"%d lines selected   ", y2 - y1 + 1);
	}
	swprintf(right, 192, L"%lsLn %d/%d, Col %d   %ls   %ls   %ls ",
		selected, ed.cy + 1, ed.buf.count, col_of(&ed.buf.lines[ed.cy], ed.cx) + 1,
		syntax_name(ed.lang),
		buffer_encoding_name(&ed.buf),
		ed.buf.crlf ? L"CRLF" : L"LF");

	lw = text_width(left, (int) wcslen(left));
	rw = (int) wcslen(right);
	out_goto(screen_h - 1, 0);
	out_text(left, (int) wcslen(left), screen_w, left_style, S_STATUS_BAR);
	if (lw + rw + 2 <= screen_w) {
		out_goto(screen_h - 1, screen_w - rw);
		out_text(right, rw, rw, S_STATUS_BAR, S_STATUS_BAR);
	}
}

/* the whole editor screen, cursor hidden */
void render (void) {

	int r, rows = view_rows(), y1 = 0, x1 = 0, y2 = 0, x2 = 0, y, row;
	bool sel = selection_range(&y1, &x1, &y2, &x2), more = true;

	clamp_top();
	if (ed.follow)
		render_scroll();
	bracket_on = !sel && find_brackets();
	attr_line = -1;

	out(L"\x1b[?25l");
	menu_draw_bar(-1);
	y = ed.top;
	row = ed.top_row;
	for (r = 0; r < rows && TEXT_TOP + r < screen_h - 1; r++) {
		render_row(more ? y : -1, row, TEXT_TOP + r, sel, y1, x1, y2, x2);
		if (more)
			more = wrap_step(&y, &row, 1);
	}
	render_status();
}

/* shows the cursor if it is in view */
void render_cursor (void) {

	LINE *l = &ed.buf.lines[ed.cy];
	int y = ed.top, row = ed.top_row, crow = wrap_row_of(l, ed.cx), n = 0, start, end, col;

	while (y < ed.cy || (y == ed.cy && row < crow)) {
		if (++n >= view_rows() || !wrap_step(&y, &row, 1))
			return;
	}
	if (y != ed.cy || row != crow)
		return;

	start = wrap_bounds(l, crow, &end);
	col = wrap_col(l, start, ed.cx);
	if (settings.word_wrap) {
		/* the end of a row filled to the edge */
		if (col >= view_cols())
			col = view_cols() - 1;
	} else {
		col -= ed.left;
		if (col < 0 || col >= view_cols())
			return;
	}
	out_goto(TEXT_TOP + n, gutter_width() + col);
	out(L"\x1b[?25h");
}

/* text position under screen cell (sx, sy); above or below the text it goes one row further */
void render_hit (int sx, int sy, int *y, int *x) {

	int ty = ed.top, row = ed.top_row, n = sy - TEXT_TOP, start, end, col;
	LINE *l;

	clamp_top();
	if (n < 0)
		wrap_step(&ty, &row, -1);
	else
		while (n-- > 0 && wrap_step(&ty, &row, 1))
			;
	l = &ed.buf.lines[ty];
	start = wrap_bounds(l, row, &end);
	col = sx - gutter_width() + (settings.word_wrap ? 0 : ed.left);
	*y = ty;
	*x = wrap_pos(l, start, end, col < 0 ? 0 : col);
}
