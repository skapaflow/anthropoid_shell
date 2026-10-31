#ifndef ANT_TYPES_RANK_H
#define ANT_TYPES_RANK_H

#include <time.h>
#include <wchar.h>
#include <stdbool.h>

typedef struct {
	wchar_t *text;
	double points;
	time_t last;
} ANT_RANK_ITEM;

/* a frecency table kept in a file: one instance per file (commands, folders) */
typedef struct {
	ANT_RANK_ITEM *item;
	int count;
	int max;                         /* entries kept */
	double maxage;                   /* aging threshold of the total points */
	bool fold;                       /* text compared ignoring case */
	bool (*keep) (const wchar_t *);  /* NULL, or false drops the entry when the file is read for a change */
} ANT_RANK;

extern bool ant_rank_init (ANT_RANK *, int, double, bool);
extern void ant_rank_clear (ANT_RANK *);
extern void ant_rank_load (ANT_RANK *, const wchar_t *);
extern void ant_rank_save (const ANT_RANK *, const wchar_t *);
extern double ant_rank_score (const ANT_RANK *, int, time_t);
extern int ant_rank_find (const ANT_RANK *, const wchar_t *);
extern void ant_rank_remove (ANT_RANK *, int);
extern void ant_rank_add (ANT_RANK *, const wchar_t *, const wchar_t *, double);

#endif
