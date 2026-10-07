#ifndef NOTE_RENDER_H
#define NOTE_RENDER_H

#include <stdbool.h>

#include "buffer.h"

extern int  gutter_width (void);
extern int  view_rows (void);
extern int  view_cols (void);
extern int  wrap_rows (const LINE *);
extern int  wrap_row_of (const LINE *, int);
extern int  wrap_bounds (const LINE *, int, int *);
extern bool wrap_step (int *, int *, int);
extern int  wrap_col (const LINE *, int, int);
extern int  wrap_pos (const LINE *, int, int, int);
extern void render_scroll (void);
extern void render (void);
extern void render_cursor (void);
extern void render_hit (int, int, int *, int *);

#endif
