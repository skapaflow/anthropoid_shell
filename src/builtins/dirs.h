#ifndef ANT_BUILTINS_DIRS_H
#define ANT_BUILTINS_DIRS_H

#include <wchar.h>
#include <stdbool.h>

#define ANT_DIRS_PATH 1024

/* a folder a typed path can mean */
typedef struct {
	wchar_t path[ANT_DIRS_PATH]; /* the whole folder, without a trailing separator */
	int prefix;                  /* units of path that are the folder named by the first component */
	double score;
} ANT_DIR_CAND;

extern void ant_dirs_add (const wchar_t *);
extern int ant_dirs_resolve (const wchar_t *, ANT_DIR_CAND **);
extern bool ant_dirs_best (const wchar_t *, wchar_t *, int, int *);
extern void ant_dirs_choose (const wchar_t *, const wchar_t *);
extern int ant_dirs_count (void);
extern const wchar_t *ant_dirs_get (int, double *);
extern bool ant_dirs_remove (const wchar_t *);
extern void ant_dirs_clear (void);

#endif
