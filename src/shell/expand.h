#ifndef ANT_SHELL_EXPAND_H
#define ANT_SHELL_EXPAND_H

#include <wchar.h>
#include <stdbool.h>

#include "parser.h"

extern void ant_expand_init (void);
extern bool ant_expand_cmd (const ANT_CMD *, ANT_CMD *, bool);
extern bool ant_expand_has_wild (const ANT_CMD *);
extern void ant_expand_free (ANT_CMD *);

#endif
