#ifndef ANT_EDITOR_HISTORY_H
#define ANT_EDITOR_HISTORY_H

#include <time.h>
#include <wchar.h>
#include <stdbool.h>

#define ANT_HISTORY_MAX 50

extern void ant_history_add (const wchar_t *);
extern int ant_history_count (void);
extern const wchar_t *ant_history_get (int);
extern time_t ant_history_time (int);
extern const wchar_t *ant_history_last (void);
extern bool ant_history_at_first (void);
extern bool ant_history_at_last (void);
extern const wchar_t *ant_history_prev (void);
extern const wchar_t *ant_history_next (void);
extern const wchar_t *ant_history_nth (int);
extern int ant_history_displayed (void);
extern bool ant_history_find (const wchar_t *, int, int *);
extern void ant_history_swap (int, int);
extern void ant_history_remove (int);
extern void ant_history_clear (void);
extern void ant_history_load (void);
extern void ant_history_print (int);
extern void ant_history_clear_log (void);

#endif
