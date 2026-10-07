#ifndef ANT_SHELL_ARGS_H
#define ANT_SHELL_ARGS_H

#include <stdbool.h>

typedef struct ant_arg_line {
	char *string;
	struct ant_arg_line *next;
	struct ant_arg_line *prev;
} ANT_ARG;

extern ANT_ARG *ant_arg;

extern bool ant_args_to_line (void);
extern ANT_ARG *ant_arg_insert (const char *);
extern void ant_arg_free (void);

#endif