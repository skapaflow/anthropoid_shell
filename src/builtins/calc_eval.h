#ifndef ANT_BUILTINS_CALC_EVAL_H
#define ANT_BUILTINS_CALC_EVAL_H

#include <stdbool.h>
#include <stddef.h>

extern bool ant_calc_is_expression (const char *);
extern bool ant_calc_eval (const char *, double *, char *, size_t, int *);
extern void ant_calc_format (double, char *, size_t);

#endif
