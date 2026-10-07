#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "shell.h"
#include "args.h"
#include "execute.h"
#include "parser.h"
#include "../builtins/builtins.h"
#include "../builtins/cd.h"
#include "../builtins/path.h"
#include "../config/config.h"
#include "expand.h"
#include "../console/console.h"
#include "../editor/complete.h"
#include "../editor/history.h"
#include "../editor/line_editor.h"

int ant_saved_codepage = 0;
int ant_status = 0; /* exit code of the last command (OSC 133;D) */
int ant_prev_status = 0;
volatile int ant_interrupted = 0;

char char_line_buf[ANTMAX] = {0};

/* built-ins and aliases; [path] folders from config.ant and PATH, in the order of the search */
static void commands_init (void) {

	static wchar_t dirs[32767 + ANTMAX * 4];
	static const wchar_t **names;
	int n = ant_path_dirs(dirs, ANTMAX * 4), nb = 0, na = ant_config_count(CONFIG_ALIAS);
	const wchar_t **grown;
	DWORD m;

	/* dirs holds the [path] folders, a NUL and then the PATH */
	dirs[n] = 0;
	m = GetEnvironmentVariableW(L"PATH", dirs + n + 1, 32766);
	if (m == 0 || m >= 32766)
		dirs[n + 1] = 0;

	while (ant_builtin_names[nb])
		nb++;
	if ((grown = realloc(names, sizeof(wchar_t *) * (nb + na + 1)))) {
		names = grown;
		memcpy(names, ant_builtin_names, sizeof(wchar_t *) * nb);
		for (int i = 0; i < na; i++)
			names[nb + i] = ant_config_name(CONFIG_ALIAS, i);
		names[nb + na] = NULL;
		ant_complete_set_commands(names, dirs, dirs + n + 1);
	} else
		ant_complete_set_commands(ant_builtin_names, dirs, dirs + n + 1);
}

/* reads data\config.ant again if it changed; reports what is wrong in it */
static void config_reload (void) {

	const wchar_t *w;

	if (!ant_config_load())
		return;
	if (ant_config_created())
		print(CLEAR|BLUE, " [!] data\\config.ant created (link -f, path -f and alias -f open it)\n");
	for (w = ant_config_warnings(); *w; ) {
		const wchar_t *end = wcschr(w, L'\n');
		int n = end ? (int) (end - w) : (int) wcslen(w);
		ant_error(L" [!] %.*ls", n, w);
		w += n + (end ? 1 : 0);
	}
	commands_init();
}

/* Ctrl+C and Ctrl+Break go to the foreground program; the shell keeps running */
static BOOL WINAPI ctrl_handler (DWORD event) {

	if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
		ant_interrupted = 1;
		return TRUE;
	}
	return FALSE;
}

/* has_args: antshell.exe got arguments, which are the first command line */
void ant_shell_run (bool has_args) {

	/* the cd history starts at the current folder */
	ant_cd_init();

	/* data\history.ant feeds the up arrow and the suggestion */
	ant_history_load();

	/* HOME (from USERPROFILE) for $HOME and ~ */
	ant_expand_init();

	/* data\config.ant (created from a template the first time); aliases go to
	   the parser; TAB in the first word: built-ins, aliases and .exe files from [path] and PATH */
	ant_parse_set_alias(ant_config_alias);
	config_reload();
	commands_init();

	/* "ignore Ctrl+C" is inherited: turn it off, or the programs run from here could not be stopped either */
	SetConsoleCtrlHandler(NULL, FALSE);
	SetConsoleCtrlHandler(ctrl_handler, TRUE);

	while (1) {

		bool keep;

		/* background jobs that finished */
		ant_jobs_report();

		/* config.ant edited while the shell is open */
		config_reload();

		/* antshell.exe arguments are the first command line; after that, the line editor */
		if (!has_args)
			ant_editor_read_line(NULL);
		else {
			ant_args_to_line();
			has_args = false;
		}

		keep = ant_run(char_line_buf);
		memset(char_line_buf, 0, sizeof(char)*ANTMAX);
		if (!keep)
			break;
	}

	/* closes the mark of the last command (exit) */
	ant_editor_end();
	ant_cd_free();
	ant_complete_set_commands(NULL, NULL, NULL);
}
