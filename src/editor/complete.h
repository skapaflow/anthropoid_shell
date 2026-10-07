#ifndef ANT_EDITOR_COMPLETE_H
#define ANT_EDITOR_COMPLETE_H

#include <wchar.h>
#include <stdbool.h>

#define ANT_COMPLETE_PATH 1024

/* how the completion text goes into the line */
enum {
	ANT_APPLY_PREFIX, /* common prefix: leaves the quote open to keep typing */
	ANT_APPLY_SELECT, /* item chosen in the pager: closes the quote, no space */
	ANT_APPLY_UNIQUE  /* the only option: closes the quote and adds a space after a file */
};

/* where a command comes from; the pager lists the groups in this order */
enum {
	GROUP_FILE,    /* file completion (no groups) */
	GROUP_BUILTIN, /* built-ins and aliases */
	GROUP_FOLDER,  /* folders of the current one (implicit cd) */
	GROUP_CURRENT, /* .exe of the current folder */
	GROUP_USER,    /* [path] of config.ant */
	GROUP_SYSTEM   /* PATH */
};

typedef struct {
	wchar_t *name; /* name; a directory ends with the separator */
	bool dir;
	int group;     /* GROUP_* */
} ANT_COMPLETION;

typedef struct {
	int beg, end;                  /* part of the line the completion replaces */
	bool quoted;                   /* the word already had quotes */
	wchar_t dir[ANT_COMPLETE_PATH];  /* typed path, without quotes (e.g. src\) */
	wchar_t word[ANT_COMPLETE_PATH]; /* typed part of the name (e.g. mo) */
	wchar_t sep;                   /* separator in use: \ or / */
	wchar_t lead;                  /* '@' in front of the word (open without waiting), or 0 */
	int rank;                      /* match category (lower = better) */
	int prefix;                    /* units of the prefix common to all names */
	int count;
	ANT_COMPLETION *items;
} ANT_COMPLETE;

extern void ant_complete_set_commands (const wchar_t *const *, const wchar_t *, const wchar_t *);
extern int ant_complete (const wchar_t *, int, ANT_COMPLETE *);
extern bool ant_complete_is_prefix (const ANT_COMPLETE *);
extern int ant_complete_text (const ANT_COMPLETE *, int, int, wchar_t *, int);
extern void ant_complete_free (ANT_COMPLETE *);

#endif
