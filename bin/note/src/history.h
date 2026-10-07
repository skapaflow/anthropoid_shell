#ifndef NOTE_HISTORY_H
#define NOTE_HISTORY_H

#include <wchar.h>
#include <stdbool.h>

#include "buffer.h"

/* kinds of edit: consecutive edits of the same kind (typing, Backspace, Delete) share one undo step */
enum { EDIT_OTHER, EDIT_TYPE, EDIT_BACKSPACE, EDIT_DELETE };

/* one change to the text */
typedef struct {
	int      group;              /* undo step it belongs to */
	bool     insert;             /* true: 'text' went in at (y, x); false: it came out from there */
	int      y, x;
	wchar_t *text;
	int      len;
	int      before_y, before_x; /* cursor before the step */
} CHANGE;

typedef struct {
	CHANGE *items;
	int     count, pos, cap;     /* items[pos..count) can be redone */
	int     group, last_group;   /* current step (0: none open) and the last id used */
	int     kind;                /* kind of the current step */
	int     end_y, end_x;        /* where the last change ended (a step goes on from there) */
	int     before_y, before_x;
	int     saved;               /* step at the top when the file was saved */
} HISTORY;

extern void history_clear (HISTORY *);
extern void history_begin (HISTORY *, int, int, int, bool);
extern void history_add (HISTORY *, bool, int, int, const wchar_t *, int);
extern bool history_undo (HISTORY *, BUFFER *, int *, int *);
extern bool history_redo (HISTORY *, BUFFER *, int *, int *);
extern void history_saved (HISTORY *);
extern bool history_modified (const HISTORY *);
extern void history_unsaved (HISTORY *);

#endif
