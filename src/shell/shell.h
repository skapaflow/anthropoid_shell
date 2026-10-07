#ifndef ANT_SHELL_SHELL_H
#define ANT_SHELL_SHELL_H

#include <wchar.h>

#define ANTMIN 128
#define ANTMID 512
#define ANTMAX 1024

extern int ant_saved_codepage;
extern int ant_status;
extern volatile int ant_interrupted; /* Ctrl+C or Ctrl+Break arrived (the shell itself ignores it) */
extern int ant_prev_status; /* ant_status before the command that is running (what a bare "exit" returns) */
extern char    char_line_buf[ANTMAX];

extern void ant_shell_run (int , char const **);

#endif