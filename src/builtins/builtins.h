#ifndef ANT_BUILTINS_BUILTINS_H
#define ANT_BUILTINS_BUILTINS_H

#include <wchar.h>
#include <stdbool.h>

#include "../shell/parser.h"

/* result of ant_builtin */
enum {
	ANT_BUILTIN_NONE, /* not a built-in: run it as a program */
	ANT_BUILTIN_DONE,
	ANT_BUILTIN_EXIT  /* the shell must close (exit, "link.") */
};

extern const wchar_t *const ant_builtin_names[];

extern bool ant_is_builtin (const ANT_CMD *);
extern bool ant_builtin_named (const wchar_t *);
extern bool ant_builtin_is_calc (const ANT_CMD *);
extern int ant_builtin (const ANT_CMD *);

#endif
