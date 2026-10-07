#ifndef ANT_BUILTINS_ENV_H
#define ANT_BUILTINS_ENV_H

#include <wchar.h>
#include <stdbool.h>

#include "../shell/parser.h"

extern bool ant_env_is_assignment (const ANT_CMD *);
extern void ant_cmd_export (const ANT_CMD *);
extern void ant_cmd_unset (const ANT_CMD *);

#endif
