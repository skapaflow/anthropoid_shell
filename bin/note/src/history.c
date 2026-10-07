#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "history.h"

/*************************************************************
 * Undo and redo.                                            *
 *                                                           *
 * Every change to the text is an insertion or a deletion    *
 * (CHANGE). A user command opens a step (history_begin) and *
 * its changes share the step's group id; undo reverts the   *
 * whole group. Typing, Backspace and Delete keep adding to  *
 * the same step while the cursor stays where the last       *
 * change ended.                                             *
 *                                                           *
 * "Modified" means the step at the top is not the one that  *
 * was there when the file was saved.                        *
 *************************************************************/

static void free_from (HISTORY *h, int from) {

	int i;

	for (i = from; i < h->count; i++) {
		if (h->items[i].group == h->saved)
			h->saved = -1;   /* the saved state can no longer be reached */
		free(h->items[i].text);
	}
	h->count = from;
}

void history_clear (HISTORY *h) {

	free_from(h, 0);
	free(h->items);
	memset(h, 0, sizeof *h);
}

static int top_group (const HISTORY *h) {

	return h->pos > 0 ? h->items[h->pos - 1].group : 0;
}

/*
 * Opens a step for a command of kind 'kind' with the cursor at (y, x). It goes on
 * with the open step if the kind repeats at the point where the last change ended,
 * or always if 'merge' (a paste coming in as keys).
 */
void history_begin (HISTORY *h, int kind, int y, int x, bool merge) {

	bool same = h->group != 0 && h->pos == h->count && top_group(h) == h->group &&
		(merge || (kind != EDIT_OTHER && kind == h->kind && y == h->end_y && x == h->end_x));

	h->kind = kind;
	if (!same) {
		h->group = ++h->last_group;
		h->before_y = y;
		h->before_x = x;
	}
}

void history_add (HISTORY *h, bool insert, int y, int x, const wchar_t *s, int n) {

	CHANGE *c;

	if (h->group == 0)
		history_begin(h, EDIT_OTHER, y, x, false);
	free_from(h, h->pos);
	if (h->count == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 256;
		h->items = xrealloc(h->items, h->cap * sizeof(CHANGE));
	}

	c = &h->items[h->count++];
	c->group = h->group;
	c->insert = insert;
	c->y = y;
	c->x = x;
	c->len = n;
	c->text = xrealloc(NULL, (n + 1) * sizeof(wchar_t));
	memcpy(c->text, s, n * sizeof(wchar_t));
	c->before_y = h->before_y;
	c->before_x = h->before_x;
	h->pos = h->count;

	if (insert)
		text_end(y, x, s, n, &h->end_y, &h->end_x);
	else {
		h->end_y = y;
		h->end_x = x;
	}
}

/* reverts the last step; the cursor goes back to where it was before it */
bool history_undo (HISTORY *h, BUFFER *b, int *y, int *x) {

	int group;

	if (h->pos == 0)
		return false;
	group = h->items[h->pos - 1].group;
	while (h->pos > 0 && h->items[h->pos - 1].group == group) {
		CHANGE *c = &h->items[--h->pos];
		if (c->insert) {
			int ey, ex;
			text_end(c->y, c->x, c->text, c->len, &ey, &ex);
			buffer_delete(b, c->y, c->x, ey, ex);
		} else
			buffer_insert(b, c->y, c->x, c->text, c->len, NULL, NULL);
		*y = c->before_y;
		*x = c->before_x;
	}
	h->group = 0;
	return true;
}

/* applies the next step again; the cursor goes to the end of its last change */
bool history_redo (HISTORY *h, BUFFER *b, int *y, int *x) {

	int group;

	if (h->pos == h->count)
		return false;
	group = h->items[h->pos].group;
	while (h->pos < h->count && h->items[h->pos].group == group) {
		CHANGE *c = &h->items[h->pos++];
		if (c->insert)
			buffer_insert(b, c->y, c->x, c->text, c->len, y, x);
		else {
			int ey, ex;
			text_end(c->y, c->x, c->text, c->len, &ey, &ex);
			buffer_delete(b, c->y, c->x, ey, ex);
			*y = c->y;
			*x = c->x;
		}
	}
	h->group = 0;
	return true;
}

/* the file was saved: the current state is the clean one, and the next edit starts a new step */
void history_saved (HISTORY *h) {

	h->saved = top_group(h);
	h->group = 0;
}

bool history_modified (const HISTORY *h) {

	return top_group(h) != h->saved;
}

/* the text differs from the file (a recovered copy): no state counts as saved */
void history_unsaved (HISTORY *h) {

	h->saved = -1;
}
