#ifndef ANT_SHELL_EXECUTE_H
#define ANT_SHELL_EXECUTE_H

#include <wchar.h>
#include <stdbool.h>

#include "parser.h"

extern bool ant_run (const char *);
extern bool ant_exec_program (int, wchar_t **);
extern void ant_launch (const ANT_CMD *);
extern void ant_jobs_report (void);
extern void ant_jobs_list (void);
extern int ant_job_id (int);
extern int ant_job_pids (int, unsigned long *, int);
extern int ant_job_of_pid (unsigned long);
extern int ant_job_wait (int);
extern bool ant_job_kill (int, unsigned);
extern bool ant_job_background (int, wchar_t **, const wchar_t *, bool, bool);

#endif
