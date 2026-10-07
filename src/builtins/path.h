#ifndef ANT_BUILTINS_PATH_H
#define ANT_BUILTINS_PATH_H

#include <wchar.h>
#include <stdbool.h>

extern void ant_cmd_path (const char *);
extern bool ant_path_find (const wchar_t *, wchar_t *, int);
extern int ant_path_dirs (wchar_t *, int);

#endif