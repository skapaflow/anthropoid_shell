#ifndef ANT_BUILTINS_LINK_H
#define ANT_BUILTINS_LINK_H

#include <stdbool.h>

/* result of ant_cmd_link */
enum {
	ANT_LINK_NONE, /* not a shortcut: goes on to execution */
	ANT_LINK_DONE, /* shortcut handled */
	ANT_LINK_EXIT  /* "name.": opened the shortcut and the shell must close */
};

extern bool ant_link_is (const char *);
extern int ant_cmd_link (const char *);

#endif
