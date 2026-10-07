#ifndef ANT_BUILTINS_CD_H
#define ANT_BUILTINS_CD_H

#include <wchar.h>
#include <stdbool.h>

#include "../shell/args.h"

extern void ant_cd_init (void);
extern void ant_cd_free (void);
extern void ant_cd_print (void);
extern int ant_cd_count (void);
extern const wchar_t *ant_cd_get (int);
extern void ant_cd_to (const wchar_t *);
extern bool ant_cd_is_implicit (const wchar_t *);
extern bool ant_cd_implicit (const wchar_t *);
extern bool ant_cd_is_command (const wchar_t *);
extern bool ant_cmd_cd (const ANT_ARG *);

#endif
