#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "builtins.h"
#include "calc.h"
#include "cd.h"
#include "clear.h"
#include "echo.h"
#include "env.h"
#include "help.h"
#include "link.h"
#include "mk.h"
#include "bg.h"
#include "path.h"
#include "which.h"
#include "../config/config.h"
#include "../console/console.h"
#include "../editor/history.h"
#include "../shell/args.h"
#include "../shell/execute.h"
#include "../shell/shell.h"

/*************************************************************
 * Built-in commands: the dispatch table, plus the small     *
 * commands that do not need a module of their own (alias,   *
 * history). Commands with more logic live in cd.c, link.c,  *
 * path.c, help.c, calc.c and clear.c.                       *
 *************************************************************/

/* names TAB completes in the first word, besides programs and aliases */
const wchar_t *const ant_builtin_names[] = {
	L"alias", L"bg", L"cd", L"echo", L"exit", L"export", L"false", L"help", L"history", L"jobs", L"mk", L"nop", L"path",
	L"true", L"unset", L"ver", L"wait", L"which", NULL
};

/* commands recognized by name, in and out of ant_builtin (cd, @prog, links and the like are decided by ant_is_builtin) */
static const wchar_t *const builtin_words[] = {
	L".", L"nop", L"ver", L"log", L"history", L"alias", L"path", L"jobs", L"help", L"exit", L"/", L"\\",
	L"echo", L"true", L"false", L"export", L"unset", L"which", L"wait", L"mk", L"bg", NULL
};

/* is it the name of a built-in command? (for which) */
bool ant_builtin_named (const wchar_t *name) {

	for (int i = 0; builtin_words[i]; i++)
		if (!wcscmp(name, builtin_words[i]))
			return true;
	return !wcscmp(name, L"cd");
}

/* the help topic of "name --help": the name itself, or the command it is an alias of; NULL if it has no entry */
static const wchar_t *help_topic (const wchar_t *w) {

	if (!wcscmp(w, L"log"))
		return L"history";
	if (!wcscmp(w, L"/") || !wcscmp(w, L"\\"))
		return NULL;
	return ant_builtin_named(w) ? w : NULL;
}

/* the command's words in UTF-8 joined by spaces (the text calc and link receive) */
static void command_text (const ANT_CMD *c, char *out, int max) {

	int used = 0;

	out[0] = 0;
	for (int i = 0; i < c->argc; i++) {
		int n = WideCharToMultiByte(CP_UTF8, 0, c->argv[i], -1, out + used + (used ? 1 : 0),
			max - used - 1, NULL, NULL);
		if (n <= 0)
			break;
		if (used)
			out[used] = ' ';
		used += n - 1 + (used ? 1 : 0);
	}
}

/* 'alias': lists the aliases from config.ant like bash does (alias name='text'); 'alias name' shows one */
static void cmd_alias (const char *opt) {

	static char name[1024], text[ANTMAX * 3];
	int shown = 0;

	if (opt && !strcmp(opt, "-f")) {
		if (!ant_config_open()) {
			ant_error(L" [!] could not open data\\config.ant");
			ant_status = 1;
		}
		return;
	}
	for (int i = 0; ant_config_name(CONFIG_ALIAS, i); i++) {
		WideCharToMultiByte(CP_UTF8, 0, ant_config_name(CONFIG_ALIAS, i), -1, name, sizeof name, NULL, NULL);
		if (opt && _stricmp(opt, name))
			continue;
		WideCharToMultiByte(CP_UTF8, 0, ant_config_raw(CONFIG_ALIAS, i), -1, text, sizeof text, NULL, NULL);
		printf("alias %s='%s'\n", name, text);
		shown++;
	}
	if (opt && !shown) {
		ant_error(L"alias: %hs: not found", opt);
		ant_status = 1;
	} else if (!shown)
		printf(" no aliases; alias -f opens data\\config.ant ([alias] section)\n");
}

/* a plain non-negative integer (the N of 'history N') */
static bool only_digits (const char *s) {

	if (!*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

/* lines of the console window, or -1 when the output is not a console (pipe or file) */
static int window_rows (void) {

	CONSOLE_SCREEN_BUFFER_INFO info;

	if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
		return -1;
	return info.srWindow.Bottom - info.srWindow.Top + 1;
}

/*
 * 'history' (and the old 'log'): the last lines of data\history.ant, as many as fit in
 * the window (all of them when the output is redirected); 'history N' the last N,
 * 'history all' everything; 'clear' (or -d) erases the log
 */
static void cmd_history (const char *opt) {

	if (opt && (!strcmp(opt, "clear") || !strcmp(opt, "-d"))) {
		ant_history_clear();
		ant_history_clear_log();
		print(CLEAR|BLUE, " [!] history cleared\n");
	} else if (!opt || !strcmp(opt, "all") || only_digits(opt)) {
		int rows = window_rows(), max = -1;

		if (opt && only_digits(opt))
			max = atoi(opt);
		else if (!opt && rows > 0)
			max = rows > 3 ? rows - 2 : 1;
		ant_history_print(max);
	} else {
		printf("\n history          last lines of the log (fits the window)\n history N        last N lines\n"
			" history all      the whole log (history all | findstr text)\n history clear    clears the log\n\n"
			" keys: \xe2\x86\x91\xe2\x86\x93 F5 PgUp PgDn F1 F2 F3 F4 F7 F8 F9, Alt+F7 clears\n");
		if (strcmp(opt, "-h") && strcmp(opt, "--help"))
			ant_status = 2; /* unknown option, as in sh */
	}
}

/* ---------- exit, wait and kill %n ---------- */

/*
 * 'exit [n]' and 'nop [n]': closes the shell with code n (0..255; more is cut to the low byte).
 * Without n, exit uses the code of the last command ($?), as POSIX says; nop (the old
 * "no operation") uses 0. A word that is not a number is an error and exits with 2.
 */
static int exit_command (const ANT_CMD *c, bool nop) {

	int code;

	if (c->argc > 2) {
		ant_error(L"ant: %ls: too many arguments", c->argv[0]);
		ant_status = 1;
		return ANT_BUILTIN_DONE;
	}
	if (c->argc == 2) {
		wchar_t *end;
		long v = wcstol(c->argv[1], &end, 10);

		if (!*c->argv[1] || *end) {
			ant_error(L"ant: %ls: %ls: numeric argument required", c->argv[0], c->argv[1]);
			code = 2;
		} else
			code = (int) (v & 0xFF);
	} else
		code = nop ? 0 : (ant_prev_status & 0xFF);
	ant_status = code;
	return ANT_BUILTIN_EXIT;
}

/* %n, %% / %+ / % (the current job): the job number (-1: the current one); false if it is not a job word */
static bool job_word (const wchar_t *w, int *id) {

	if (w[0] != L'%')
		return false;
	if (!w[1] || !wcscmp(w, L"%%") || !wcscmp(w, L"%+")) {
		*id = -1;
		return true;
	}
	for (int i = 1; w[i]; i++)
		if (w[i] < L'0' || w[i] > L'9')
			return false;
	*id = _wtoi(w + 1);
	return *id > 0;
}

static bool all_digits (const wchar_t *w) {

	if (!*w)
		return false;
	for (; *w; w++)
		if (*w < L'0' || *w > L'9')
			return false;
	return true;
}

/*
 * 'wait [%n | pid ...]': waits for background jobs (all of them without arguments) and
 * returns the exit code of the last one waited for; 127 if there is no such job.
 * Ctrl+C stops the wait (code 130) and the job keeps running.
 */
static void cmd_wait (const ANT_CMD *c) {

	if (c->argc == 1) {
		ant_status = ant_job_wait(0) == 130 ? 130 : 0;
		return;
	}
	for (int i = 1; i < c->argc; i++) {
		int id = 0, code;

		if (!job_word(c->argv[i], &id)) {
			if (all_digits(c->argv[i]))
				id = ant_job_of_pid((unsigned long) _wtol(c->argv[i]));
			if (id == 0) {
				ant_error(L"ant: wait: %ls: no such job", c->argv[i]);
				ant_status = 127;
				continue;
			}
		}
		if (id < 0)
			id = ant_job_id(-1);
		code = id > 0 ? ant_job_wait(id) : -1;
		if (code < 0) {
			ant_error(L"ant: wait: %ls: no such job", c->argv[i]);
			ant_status = 127;
		} else
			ant_status = code;
		if (code == 130)
			return;
	}
}

/* the exit code a signal gives to the processes it ends (128 + number, as a shell reports it); -1 if unknown */
static int signal_code (const wchar_t *name) {

	static const struct { const wchar_t *name; int number; } signals[] = {
		{ L"HUP", 1 }, { L"INT", 2 }, { L"QUIT", 3 }, { L"KILL", 9 }, { L"TERM", 15 }
	};

	if (all_digits(name))
		return _wtoi(name) <= 64 ? 128 + _wtoi(name) : -1;
	if (!_wcsnicmp(name, L"SIG", 3))
		name += 3;
	for (int i = 0; i < (int) (sizeof signals / sizeof *signals); i++)
		if (!_wcsicmp(name, signals[i].name))
			return 128 + signals[i].number;
	return -1;
}

/* does it have the form "kill ... %n ..."? Then it is a job command and runs in the shell */
static bool kill_has_jobs (const ANT_CMD *c) {

	if (wcscmp(c->argv[0], L"kill"))
		return false;
	for (int i = 1; i < c->argc; i++)
		if (c->argv[i][0] == L'%')
			return true;
	return false;
}

/*
 * 'kill [-s SIG | -SIG] %n ... pid ...' when there is a job among the operands (otherwise kill
 * is the program of bin\ ). Every signal ends the process: Windows has no signals.
 */
static void cmd_kill_jobs (const ANT_CMD *c) {

	int i = 1, code = 128 + 15;

	for (; i < c->argc && c->argv[i][0] == L'-' && c->argv[i][1]; i++) {
		const wchar_t *sig = c->argv[i] + 1;

		if (!wcscmp(c->argv[i], L"--")) {
			i++;
			break;
		}
		if (!wcscmp(sig, L"s") && i + 1 < c->argc)
			sig = c->argv[++i];
		if ((code = signal_code(sig)) < 0) {
			ant_error(L"ant: kill: %ls: invalid signal specification", sig);
			ant_status = 1;
			return;
		}
	}
	for (; i < c->argc; i++) {
		int id;

		if (job_word(c->argv[i], &id)) {
			if (!ant_job_kill(id, (unsigned) code)) {
				ant_error(L"ant: kill: %ls: no such job", c->argv[i]);
				ant_status = 1;
			}
		} else if (all_digits(c->argv[i])) {
			HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD) _wtol(c->argv[i]));

			if (!h || !TerminateProcess(h, (UINT) code)) {
				ant_error(L"ant: kill: (%ls) - No such process", c->argv[i]);
				ant_status = 1;
			}
			if (h)
				CloseHandle(h);
		} else {
			ant_error(L"ant: kill: %ls: arguments must be process or job IDs", c->argv[i]);
			ant_status = 1;
		}
	}
}

/* does the command run inside the shell? (the same decision as ant_builtin, without running it) */
bool ant_is_builtin (const ANT_CMD *c) {

	static char text[ANTMAX];
	const wchar_t *w = c->argv[0];

	for (int i = 0; builtin_words[i]; i++)
		if (!wcscmp(w, builtin_words[i]))
			return true;
	if (w[0] == L'@' || ant_env_is_assignment(c) || kill_has_jobs(c))
		return true;
	if (ant_cd_is_command(w))
		return true;
	if (c->argc == 1 && ant_cd_is_implicit(w))
		return true;
	command_text(c, text, ANTMAX);
	return ant_calc_is_expression(text) || ant_link_is(text);
}

/* does the command read as a calculator expression (2 * 3, 2 3 +)? then the shell does not glob its * */
bool ant_builtin_is_calc (const ANT_CMD *c) {

	static char text[ANTMAX];

	command_text(c, text, ANTMAX);
	return ant_calc_is_expression(text);
}

/*
 * Runs a command already split by the parser if it is a built-in. The words
 * also become an ANT_ARG list and UTF-8 text in char_line_buf, which is what
 * the older built-ins read.
 */
int ant_builtin (const ANT_CMD *c) {

	static char word[ANTMAX * 3];
	int result = ANT_BUILTIN_DONE;

	ant_arg_free();
	for (int i = 0; i < c->argc; i++)
		if (WideCharToMultiByte(CP_UTF8, 0, c->argv[i], -1, word, sizeof word, NULL, NULL))
			ant_arg_insert(word);
	command_text(c, char_line_buf, ANTMAX);
	if (!ant_arg)
		return ANT_BUILTIN_NONE;

	const char *s = ant_arg->string;
	const char *opt = ant_arg->next ? ant_arg->next->string : NULL;

	/* "name --help" on a built-in: the same text as "help name" */
	if (c->argc == 2 && !wcscmp(c->argv[1], L"--help") && help_topic(c->argv[0])) {
		wchar_t *topic_argv[2] = { L"help", (wchar_t *) help_topic(c->argv[0]) };
		ANT_CMD topic = { 0 };

		topic.argc = 2;
		topic.argv = topic_argv;
		ant_cmd_help(&topic);
		ant_arg_free();
		return ANT_BUILTIN_DONE;
	}

	if (!strcmp(s, "."))
		ant_cmd_clear();
	else if (!strcmp(s, "nop") || !strcmp(s, "exit"))
		result = exit_command(c, !strcmp(s, "nop"));
	else if (!strcmp(s, "wait"))
		cmd_wait(c);
	else if (kill_has_jobs(c))
		cmd_kill_jobs(c);
	else if (!strcmp(s, "ver"))
		ant_logo(22);
	else if (!strcmp(s, "history") || !strcmp(s, "log"))
		cmd_history(opt);
	else if (!strcmp(s, "path"))
		ant_cmd_path(opt);
	else if (!strcmp(s, "alias"))
		cmd_alias(opt);
	else if (!strcmp(s, "help"))
		ant_cmd_help(c);
	else if (!strcmp(s, "jobs"))
		ant_jobs_list();
	else if (!strcmp(s, "echo"))
		ant_cmd_echo(c);
	else if (!strcmp(s, "true"))
		ant_status = 0;
	else if (!strcmp(s, "false"))
		ant_status = 1;
	else if (!strcmp(s, "export") || ant_env_is_assignment(c))
		ant_cmd_export(c); /* export, or plain NAME=value */
	else if (!strcmp(s, "unset"))
		ant_cmd_unset(c);
	else if (!strcmp(s, "which"))
		ant_cmd_which(c);
	else if (!strcmp(s, "mk"))
		ant_cmd_mk(c);
	else if (!strcmp(s, "bg"))
		ant_cmd_bg(c);
	else if (s[0] == '@')
		ant_launch(c); /* @prog: open without waiting, as in the old project */
	else if (ant_cmd_cd(ant_arg))
		; /* cd, cd.. and cd\ */
	else if (!strcmp(s, "/") || !strcmp(s, "\\"))
		ant_cd_print();
	else if (c->argc == 1 && !ant_link_is(char_line_buf) && ant_cd_implicit(c->argv[0]))
		; /* implicit cd, as in fish: "..", "src\", ".\build", "~"; a [link] "name\" comes first */
	else if (!ant_cmd_calc(char_line_buf))
		; /* calculator expression */
	else {
		/* [link] shortcuts from config.ant come before programs, as in the old project */
		int link = ant_cmd_link(char_line_buf);
		result = (link == ANT_LINK_EXIT) ? ANT_BUILTIN_EXIT :
			(link == ANT_LINK_DONE) ? ANT_BUILTIN_DONE : ANT_BUILTIN_NONE;
	}

	ant_arg_free();
	return result;
}
