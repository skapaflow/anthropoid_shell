#ifndef NOTE_EDITOR_H
#define NOTE_EDITOR_H

#include <wchar.h>
#include <stdbool.h>

#include "buffer.h"
#include "history.h"

/* commands (keys and menu items) */
enum {
	CMD_NONE,
	CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_EXIT,
	CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_DELETE, CMD_SELECT_ALL,
	CMD_FIND, CMD_FIND_NEXT, CMD_FIND_PREVIOUS, CMD_REPLACE, CMD_GOTO_LINE,
	CMD_WORD_WRAP, CMD_LINE_NUMBERS, CMD_SYNTAX, CMD_HIGHLIGHT_LINE,
	CMD_HELP, CMD_ABOUT
};

#define FIELD_MAX 1024

typedef struct {
	BUFFER   buf;
	HISTORY  history;
	wchar_t *path;              /* NULL: untitled */
	int      lang;              /* LANG_* for syntax highlighting */
	int      cy, cx;            /* cursor: line and UTF-16 index in it, always at a grapheme start */
	int      ay, ax;            /* selection anchor (the selection runs from it to the cursor) */
	bool     selecting;
	int      want_col;          /* column kept by vertical moves (-1: the cursor's) */
	int      top, top_row;      /* first visible line, and its first visible row (word wrap) */
	int      left;              /* first visible column (no word wrap) */
	bool     follow;            /* keep the cursor in view (the mouse wheel turns it off) */
	wchar_t  message[512];      /* status bar notice until the next key */
	int      message_style;
	wchar_t  find[FIELD_MAX];
	wchar_t  replace[FIELD_MAX];
	bool     quit;
} EDITOR;

extern EDITOR ed;

extern int  editor_run (const wchar_t *, int, int);
extern void run_command (int);
extern int  command_checked (int);
extern bool selection_range (int *, int *, int *, int *);
extern bool is_modified (void);
extern const wchar_t *file_name (void);
extern int  col_of (const LINE *, int);
extern int  pos_of (const LINE *, int);

#endif
