#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#include "execute.h"
#include "expand.h"
#include "parser.h"
#include "shell.h"
#include "../builtins/builtins.h"
#include "../builtins/path.h"
#include "../console/console.h"

/*************************************************************
 * Runs the line in the sh model:                            *
 *   a ; b     always runs b                                 *
 *   a && b    runs b if a ended with 0                      *
 *   a || b    runs b if a failed                            *
 * The exit code (ant_status) is the one of the last         *
 * command that ran; skipped commands do not change it.      *
 *                                                           *
 * Programs: CreateProcessW with the command line built      *
 * from the words, following the Windows quoting rules       *
 * (CommandLineToArgvW / CRT), so accents and spaces         *
 * reach the program intact.                                 *
 *************************************************************/

#define CMDLINE_MAX 32768

/* appends 'arg' to out, with quotes when needed (CRT rules: a \ before " is escaped) */
static bool append_arg (wchar_t *out, int *n, const wchar_t *arg) {

	bool quote = !*arg || wcspbrk(arg, L" \t\"") != NULL;
	int k = *n;

	if (k > 0) {
		if (k + 1 >= CMDLINE_MAX)
			return false;
		out[k++] = L' ';
	}

	if (!quote) {
		int len = wcslen(arg);
		if (k + len >= CMDLINE_MAX)
			return false;
		wmemcpy(out + k, arg, len);
		*n = k + len;
		out[*n] = 0;
		return true;
	}

	out[k++] = L'"';
	for (const wchar_t *p = arg; ; p++) {
		int slashes = 0;
		while (*p == L'\\') {
			slashes++;
			p++;
		}
		/* backslashes before a quote (or before the end) are doubled; the quote becomes \" */
		int copies = (*p == 0) ? slashes * 2 : (*p == L'"') ? slashes * 2 + 1 : slashes;
		if (k + copies + 3 >= CMDLINE_MAX)
			return false;
		while (copies--)
			out[k++] = L'\\';
		if (*p == 0)
			break;
		out[k++] = *p;
	}
	out[k++] = L'"';
	out[k] = 0;
	*n = k;
	return true;
}

/* program: with a path, as it is; otherwise [path] from config.ant; otherwise CreateProcessW searches (current folder, system, PATH) */
static bool command_line (int argc, wchar_t **argv, wchar_t *out) {

	static wchar_t program[MAX_PATH * 4];
	int n = 0;

	out[0] = 0;
	if (!wcspbrk(argv[0], L"\\/:") && ant_path_find(argv[0], program, MAX_PATH * 4)) {
		if (!append_arg(out, &n, program))
			return false;
	} else if (!append_arg(out, &n, argv[0]))
		return false;

	for (int i = 1; i < argc; i++)
		if (!append_arg(out, &n, argv[i]))
			return false;
	return true;
}

/* a launch error goes where the command's 2> goes (err NULL: the shell's own error output) */
static void spawn_error (HANDLE err, const wchar_t *fmt, const wchar_t *name, DWORD code) {

	HANDLE saved = GetStdHandle(STD_ERROR_HANDLE);

	if (err)
		SetStdHandle(STD_ERROR_HANDLE, err);
	ant_error(fmt, name, code);
	if (err)
		SetStdHandle(STD_ERROR_HANDLE, saved);
}

/*
 * Starts the program in argv. in/out/err NULL = inherit the console.
 * Returns false if it could not start (message already shown, ant_status set).
 * title != NULL: the program gets a console of its own, hidden, with that title.
 */
static bool spawn_to (HANDLE msg, int argc, wchar_t **argv, HANDLE in, HANDLE out, HANDLE err, DWORD flags, const wchar_t *title, PROCESS_INFORMATION *pi) {

	static wchar_t cmd[CMDLINE_MAX];
	STARTUPINFOW si;
	bool redirect = in || out || err;
	HANDLE std[3] = { NULL, NULL, NULL };
	bool ok;

	if (!command_line(argc, argv, cmd)) {
		spawn_error(msg, L"ant: %ls: command line too long", argv[0], 0);
		ant_status = 126;
		return false;
	}

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;

	/* only the three ends of this process are inheritable, and only during CreateProcess */
	if (redirect) {
		HANDLE want[3] = {
			in  ? in  : GetStdHandle(STD_INPUT_HANDLE),
			out ? out : GetStdHandle(STD_OUTPUT_HANDLE),
			err ? err : GetStdHandle(STD_ERROR_HANDLE)
		};
		HANDLE self = GetCurrentProcess();
		for (int i = 0; i < 3; i++)
			if (want[i] && want[i] != INVALID_HANDLE_VALUE)
				DuplicateHandle(self, want[i], self, &std[i], 0, TRUE, DUPLICATE_SAME_ACCESS);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = std[0];
		si.hStdOutput = std[1];
		si.hStdError = std[2];
	}
	if (title) {
		si.lpTitle = (wchar_t *) title;
		si.dwFlags |= STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
	}

	ok = CreateProcessW(NULL, cmd, NULL, NULL, redirect, flags, NULL, NULL, &si, pi);

	for (int i = 0; i < 3; i++)
		if (std[i])
			CloseHandle(std[i]);

	if (!ok) {
		DWORD e = GetLastError();
		if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME) {
			spawn_error(msg, L"ant: %ls: command not found", argv[0], e);
			ant_status = 9009; /* like cmd's errorlevel */
		} else {
			spawn_error(msg, L"ant: %ls: cannot execute (error %lu)", argv[0], e);
			ant_status = 126;
		}
		return false;
	}
	CloseHandle(pi->hThread);
	return true;
}

static bool spawn (int argc, wchar_t **argv, HANDLE in, HANDLE out, HANDLE err, DWORD flags, PROCESS_INFORMATION *pi) {

	return spawn_to(err, argc, argv, in, out, err, flags, NULL, pi);
}

static int wait_process (HANDLE process) {

	DWORD code = 0;

	WaitForSingleObject(process, INFINITE);
	GetExitCodeProcess(process, &code);
	CloseHandle(process);
	return (int) code;
}

/* runs a program in the console and waits; also used by the link shortcuts */
bool ant_exec_program (int argc, wchar_t **argv) {

	PROCESS_INFORMATION pi;

	/* the program sees the console's original output code page, not the shell's UTF-8 */
	fflush(stdout);
	SetConsoleOutputCP(ant_saved_codepage);
	bool ok = spawn(argc, argv, NULL, NULL, NULL, 0, &pi);
	if (ok) {
		putchar(10);
		fflush(stdout);
		ant_status = wait_process(pi.hProcess);
		putchar(10);
	}
	SetConsoleOutputCP(CP_UTF8);
	return ok;
}

/* built-in or program; returns false if the shell must close */
static bool run_command (const ANT_CMD *c) {

	int b;

	ant_prev_status = ant_status;
	ant_status = 0;
	b = ant_builtin(c);
	if (b == ANT_BUILTIN_EXIT)
		return false;
	if (b == ANT_BUILTIN_NONE)
		ant_exec_program(c->argc, c->argv);
	return true;
}

/* ---------- pipes and redirections ---------- */

#define PIPE_BUFFER 65536

typedef struct {
	HANDLE in, out, err; /* NULL = console */
	HANDLE *own;         /* handles opened for this command, closed at the end (room: STAGE_OWN) */
	int nown;
	bool builtin;
	bool failed;         /* a redirection did not open: do not run */
	HANDLE process;
	int status;
} STAGE;

/* what a stage can own: one handle per redirection, the two pipe ends and the NUL of & */
#define STAGE_OWN(c) ((c)->nredir + 3)

static void own (STAGE *s, HANDLE h) {

	if (h && h != INVALID_HANDLE_VALUE)
		s->own[s->nown++] = h;
}

static void close_owned (STAGE *s) {

	for (int i = 0; i < s->nown; i++)
		CloseHandle(s->own[i]);
	s->nown = 0;
}

/* opens the file of a redirection; /dev/null becomes NUL */
static HANDLE open_redirect (const ANT_REDIR *r) {

	const wchar_t *path = _wcsicmp(r->path, L"/dev/null") ? r->path : L"NUL";

	switch (r->kind) {
		case REDIR_IN:
			return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		case REDIR_OUT:
		case REDIR_ERR:
			return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		default: /* >> and 2>>: FILE_APPEND_DATA makes every write go to the end, including the programs' writes */
			return CreateFileW(path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	}
}

/* applies the redirections in order ("> f 2>&1" sends both to f) */
static bool apply_redirects (const ANT_CMD *c, STAGE *s) {

	for (int i = 0; i < c->nredir; i++) {
		const ANT_REDIR *r = &c->redir[i];
		HANDLE h;

		if (r->kind == REDIR_ERR_TO_OUT) {
			s->err = s->out ? s->out : GetStdHandle(STD_OUTPUT_HANDLE);
			continue;
		}
		h = open_redirect(r);
		if (h == INVALID_HANDLE_VALUE) {
			ant_error(L"ant: %ls: cannot open (error %lu)", r->path, GetLastError());
			return false;
		}
		own(s, h);
		if (r->kind == REDIR_IN)
			s->in = h;
		else if (r->kind == REDIR_OUT || r->kind == REDIR_APPEND)
			s->out = h;
		else
			s->err = h;
	}
	return true;
}

/* replaces the standard fd and handle (1 = output, 2 = error) with 'h'; returns the old fd to restore */
static int redirect_fd (int fd, DWORD which, HANDLE h, HANDLE *saved) {

	HANDLE copy;
	int old, nfd;

	if (!h)
		return -1;
	if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return -1;
	if ((nfd = _open_osfhandle((intptr_t) copy, _O_WRONLY | _O_TEXT)) < 0) {
		CloseHandle(copy);
		return -1;
	}
	old = _dup(fd);
	_dup2(nfd, fd);
	_close(nfd);
	*saved = GetStdHandle(which);
	SetStdHandle(which, h);
	return old;
}

static void restore_fd (int fd, DWORD which, int old, HANDLE saved) {

	if (old < 0)
		return;
	_dup2(old, fd);
	_close(old);
	SetStdHandle(which, saved);
}

/* built-in inside the shell, with output and error pointed at the pipe or file */
static int run_builtin (const ANT_CMD *c, HANDLE out, HANDLE err) {

	HANDLE saved_out = NULL, saved_err = NULL;
	int old_out, old_err, b;

	fflush(stdout);
	fflush(stderr);
	old_out = redirect_fd(1, STD_OUTPUT_HANDLE, out, &saved_out);
	old_err = redirect_fd(2, STD_ERROR_HANDLE, err, &saved_err);

	ant_prev_status = ant_status;
	ant_status = 0;
	b = ant_builtin(c);

	fflush(stdout);
	fflush(stderr);
	restore_fd(1, STD_OUTPUT_HANDLE, old_out, saved_out);
	restore_fd(2, STD_ERROR_HANDLE, old_err, saved_err);
	return b;
}

/* wide text on standard output in UTF-8 (the shell's code page); wprintf would turn accents into '?' */
static void wout (const wchar_t *fmt, ...) {

	static wchar_t buf[1024];
	static char u8[3072];
	va_list ap;

	va_start(ap, fmt);
	vswprintf(buf, 1024, fmt, ap);
	va_end(ap);
	if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof u8, NULL, NULL))
		fputs(u8, stdout);
	fflush(stdout);
}

/* ---------- background jobs (&) ---------- */

#define JOBS_MAX  32
#define JOB_TEXT  200

typedef struct {
	int id;
	int nproc;
	HANDLE *proc;
	DWORD pid;               /* of the last process, like sh's $! */
	unsigned seq;            /* order of creation: the highest is the current job (%%) */
	wchar_t text[JOB_TEXT];  /* the line, for the "done" notice */
} JOB;

static JOB jobs[JOBS_MAX];
static int njobs = 0;
static unsigned job_seq = 0;

/* pipeline text to show (words with spaces inside quotes) */
static void pipeline_text (const ANT_PIPELINE *p, wchar_t *out, int max) {

	int k = 0;

	out[0] = 0;
	for (int i = 0; i < p->ncmd; i++)
		for (int j = 0; j < p->cmd[i].argc; j++) {
			const wchar_t *w = p->cmd[i].argv[j];
			bool quote = wcschr(w, L' ') != NULL;
			int need = wcslen(w) + 6;
			if (k + need >= max)
				return;
			if (k > 0)
				k += swprintf(out + k, max - k, (j == 0) ? L" | " : L" ");
			k += swprintf(out + k, max - k, quote ? L"\"%ls\"" : L"%ls", w);
		}
}

static int job_new_id (void) {

	int id = 1;

	/* the lowest free number, as in bash */
	for (bool used = true; used; id++) {
		used = false;
		for (int i = 0; i < njobs; i++)
			if (jobs[i].id == id)
				used = true;
		if (!used)
			return id;
	}
	return id;
}

/* reports the jobs that finished; called before each prompt */
void ant_jobs_report (void) {

	for (int i = 0; i < njobs; ) {
		JOB *j = &jobs[i];
		bool done = true;
		DWORD code = 0;

		for (int k = 0; k < j->nproc; k++)
			if (WaitForSingleObject(j->proc[k], 0) == WAIT_TIMEOUT)
				done = false;
		if (!done) {
			i++;
			continue;
		}

		if (j->nproc > 0)
			GetExitCodeProcess(j->proc[j->nproc - 1], &code);
		fflush(stdout);
		if (code == 0)
			wout(L"[%d] done  %ls\n", j->id, j->text);
		else
			wout(L"[%d] exit %lu  %ls\n", j->id, code, j->text);
		fflush(stdout);

		for (int k = 0; k < j->nproc; k++)
			CloseHandle(j->proc[k]);
		free(j->proc);
		jobs[i] = jobs[--njobs];
	}
}

/* 'jobs' command: the ones still running */
void ant_jobs_list (void) {

	for (int i = 0; i < njobs; i++)
		wout(L"[%d] running  %ls  (pid %lu)\n", jobs[i].id, jobs[i].text, jobs[i].pid);
	fflush(stdout);
}

/* the job with that number (id < 0: the current one, the most recent); NULL if there is none */
static JOB *job_find (int id) {

	JOB *best = NULL;

	for (int i = 0; i < njobs; i++) {
		if (id >= 0 && jobs[i].id == id)
			return &jobs[i];
		if (id < 0 && (!best || jobs[i].seq > best->seq))
			best = &jobs[i];
	}
	return best;
}

/* the number of the job (id < 0: the current one), or 0 if there is none */
int ant_job_id (int id) {

	JOB *j = job_find(id);

	return j ? j->id : 0;
}

/* the pids of the processes of a job; returns how many (0: no such job) */
int ant_job_pids (int id, unsigned long *pids, int max) {

	JOB *j = job_find(id);
	int n = 0;

	for (int k = 0; j && k < j->nproc && n < max; k++)
		pids[n++] = GetProcessId(j->proc[k]);
	return n;
}

/* the job whose last process has that pid (0: none) */
int ant_job_of_pid (unsigned long pid) {

	for (int i = 0; i < njobs; i++)
		if (jobs[i].pid == pid)
			return jobs[i].id;
	return 0;
}

static void job_remove (JOB *j) {

	for (int k = 0; k < j->nproc; k++)
		CloseHandle(j->proc[k]);
	free(j->proc);
	*j = jobs[--njobs];
}

/*
 * Waits for a job (id > 0), or for all of them (id == 0), without the "done" notice.
 * Returns the exit code of the last process of the job, -1 if there is no such job and
 * 130 if Ctrl+C interrupted the wait (the job keeps running).
 */
int ant_job_wait (int id) {

	int code = 0;

	ant_interrupted = 0;
	if (id == 0 && njobs == 0)
		return 0;
	for (;;) {
		JOB *j = (id == 0) ? (njobs > 0 ? &jobs[0] : NULL) : job_find(id);
		DWORD exit_code = 0;

		if (!j)
			return id == 0 ? code : -1;
		for (int k = 0; k < j->nproc; ) {
			DWORD w = WaitForSingleObject(j->proc[k], 100);
			if (w == WAIT_TIMEOUT) {
				if (ant_interrupted)
					return 130;
				continue;
			}
			k++;
		}
		if (j->nproc > 0)
			GetExitCodeProcess(j->proc[j->nproc - 1], &exit_code);
		code = (int) exit_code;
		job_remove(j);
		if (id != 0)
			return code;
	}
}

/* ends the processes of a job with that exit code; false if there is no such job */
bool ant_job_kill (int id, unsigned code) {

	JOB *j = job_find(id);

	if (!j)
		return false;
	for (int k = 0; k < j->nproc; k++)
		if (WaitForSingleObject(j->proc[k], 0) == WAIT_TIMEOUT)
			TerminateProcess(j->proc[k], code);
	return true;
}

/* ---------- pipes, redirections and & ---------- */

/*
 * a | b | c with redirections. External programs start first; then the
 * built-ins run in the shell, writing into pipes the programs are already
 * reading. Exit code: the one of the last command.
 *
 * In the background (&) it does not wait: the processes go into their own
 * group (Ctrl+C at the prompt does not reach them), read from NUL (they do not
 * steal the keyboard) and become a job. Built-ins of a background pipeline run
 * right away.
 */
static bool run_piped (const ANT_PIPELINE *p, bool background) {

	int n = p->ncmd, slots = 0;
	STAGE *st = calloc(n, sizeof(STAGE));
	HANDLE *owned;
	bool keep = true, last_external, any_external = false;
	DWORD flags = background ? CREATE_NEW_PROCESS_GROUP : 0;

	for (int i = 0; i < n; i++)
		slots += STAGE_OWN(&p->cmd[i]);
	owned = malloc(sizeof(HANDLE) * slots);
	if (!st || !owned) {
		free(st);
		free(owned);
		ant_error(L"ant: out of memory");
		ant_status = 1;
		return true;
	}
	for (int i = 0, k = 0; i < n; k += STAGE_OWN(&p->cmd[i]), i++) {
		st[i].own = owned + k;
		st[i].builtin = ant_is_builtin(&p->cmd[i]);
	}

	/* pipes between the commands; a built-in does not read input, so whatever writes to it goes to NUL */
	for (int i = 0; i + 1 < n; i++) {
		HANDLE r, w;
		if (st[i + 1].builtin) {
			w = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
			r = NULL;
		} else if (!CreatePipe(&r, &w, NULL, PIPE_BUFFER))
			r = w = NULL;
		st[i].out = w;
		own(&st[i], w);
		st[i + 1].in = r;
		own(&st[i + 1], r);
	}

	/* in the background the first command reads from NUL (the redirections below may change that) */
	if (background) {
		st[0].in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		own(&st[0], st[0].in);
	}

	for (int i = 0; i < n; i++)
		if (!apply_redirects(&p->cmd[i], &st[i])) {
			st[i].failed = true;
			st[i].status = 1;
		}

	last_external = !background && !st[n - 1].builtin && !st[n - 1].failed && !st[n - 1].out;
	for (int i = 0; i < n; i++)
		if (!st[i].builtin && !st[i].failed)
			any_external = true;

	/* programs see the console's original output code page, redirected or not
	   (as in cmd: "dir > f.txt" writes in the console's code page) */
	fflush(stdout);
	if (any_external)
		SetConsoleOutputCP(ant_saved_codepage);
	if (last_external) {
		putchar(10);
		fflush(stdout);
	}

	/* step 1: start the programs and close in the shell the ends that went to them */
	for (int i = 0; i < n; i++) {
		PROCESS_INFORMATION pi;
		if (st[i].builtin)
			continue;
		if (!st[i].failed) {
			if (spawn(p->cmd[i].argc, p->cmd[i].argv, st[i].in, st[i].out, st[i].err, flags, &pi)) {
				st[i].process = pi.hProcess;
				st[i].status = (int) pi.dwProcessId;
			} else
				st[i].status = ant_status;
		}
		close_owned(&st[i]);
	}

	/* step 2: built-ins in the shell */
	for (int i = 0; i < n; i++) {
		if (st[i].builtin && !st[i].failed) {
			/* a built-in writing to the screen uses the shell's UTF-8 */
			if (any_external && !st[i].out)
				SetConsoleOutputCP(CP_UTF8);
			if (run_builtin(&p->cmd[i], st[i].out, st[i].err) == ANT_BUILTIN_EXIT)
				keep = false;
			if (any_external && !st[i].out)
				SetConsoleOutputCP(ant_saved_codepage);
			st[i].status = ant_status;
		}
		close_owned(&st[i]);
	}

	if (background) {
		/* becomes a job; the exit code of & is 0, as in sh */
		JOB *j = (njobs < JOBS_MAX) ? &jobs[njobs] : NULL;
		int count = 0;

		for (int i = 0; i < n; i++)
			if (st[i].process)
				count++;
		if (count > 0 && j && (j->proc = malloc(sizeof(HANDLE) * count))) {
			j->nproc = 0;
			for (int i = 0; i < n; i++)
				if (st[i].process) {
					j->proc[j->nproc++] = st[i].process;
					j->pid = (DWORD) st[i].status;
				}
			j->id = job_new_id();
			j->seq = ++job_seq;
			pipeline_text(p, j->text, JOB_TEXT);
			njobs++;
			if (any_external)
				SetConsoleOutputCP(CP_UTF8);
			wout(L"[%d] %lu\n", j->id, j->pid);
			fflush(stdout);
		} else {
			/* no room in the table: keeps running, it is just not tracked */
			for (int i = 0; i < n; i++)
				if (st[i].process)
					CloseHandle(st[i].process);
			if (any_external)
				SetConsoleOutputCP(CP_UTF8);
		}
		ant_status = 0;
		free(owned);
		free(st);
		return keep;
	}

	/* waits for the programs */
	for (int i = 0; i < n; i++)
		if (st[i].process)
			st[i].status = wait_process(st[i].process);

	if (last_external) {
		putchar(10);
		fflush(stdout);
	}
	if (any_external)
		SetConsoleOutputCP(CP_UTF8);

	ant_status = st[n - 1].status;
	free(owned);
	free(st);
	return keep;
}

/*
 * The console window of a 'bg -d' job (found by its title, which the program has not
 * changed yet): takes the close item out of the system menu so the window cannot be
 * closed by accident, then gives it the command line as its title.
 */
static void bg_protect_window (const wchar_t *title, const wchar_t *text) {

	HWND w = NULL;

	for (int i = 0; i < 150 && !w; i++) {
		w = FindWindowW(NULL, title);
		if (!w)
			Sleep(20);
	}
	if (!w) {
		ant_error(L"ant: bg: -d: could not find the console window");
		return;
	}
	DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);
	DrawMenuBar(w);
	SetWindowTextW(w, text);
}

/*
 * 'bg': starts a program in the background with a console of its own, hidden (the
 * terminal stays free and closing it does not end the program), and registers it as
 * a job (jobs, wait, kill). The output stays in that console, to be seen with 'show'.
 * to_file: stdout and stderr go to <base>_<pid>.txt instead (and it reads from NUL).
 * no_close: the close button of the hidden console is disabled.
 * Returns false if it could not start (message shown, ant_status set).
 */
bool ant_job_background (int argc, wchar_t **argv, const wchar_t *base, bool to_file, bool no_close) {

	PROCESS_INFORMATION pi;
	JOB *j = (njobs < JOBS_MAX) ? &jobs[njobs] : NULL;
	HANDLE in = NULL, out = NULL;
	wchar_t text[JOB_TEXT], title[JOB_TEXT], tmp[MAX_PATH], log[MAX_PATH] = L"";
	int k = 0;
	bool ok;

	text[0] = 0;
	for (int i = 0; i < argc; i++) {
		const wchar_t *w = argv[i];
		bool quote = wcschr(w, L' ') != NULL;
		if (k + (int) wcslen(w) + 4 >= JOB_TEXT)
			break;
		k += swprintf(text + k, JOB_TEXT - k, quote ? L"%ls\"%ls\"" : L"%ls%ls", i ? L" " : L"", w);
	}
	if (no_close)
		swprintf(title, JOB_TEXT, L"antbg:%lu:%u", GetCurrentProcessId(), job_seq + 1);
	else
		wcscpy(title, text);

	if (to_file) {
		/* the name only gets its pid after the program starts: opens under a temporary name and renames */
		swprintf(tmp, MAX_PATH, L"%ls_new.tmp", base);
		out = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (out == INVALID_HANDLE_VALUE) {
			ant_error(L"ant: bg: %ls: cannot create the output file (error %lu)", tmp, GetLastError());
			ant_status = 1;
			return false;
		}
		in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
		if (in == INVALID_HANDLE_VALUE)
			in = NULL;
	}

	/* the program sees the console's original output code page; errors of the launch go to the screen */
	fflush(stdout);
	SetConsoleOutputCP(ant_saved_codepage);
	ok = spawn_to(NULL, argc, argv, in, out, out, CREATE_NEW_CONSOLE, title, &pi);
	SetConsoleOutputCP(CP_UTF8);
	if (in)
		CloseHandle(in);
	if (out)
		CloseHandle(out);
	if (!ok) {
		if (to_file)
			DeleteFileW(tmp);
		return false;
	}

	if (to_file) {
		swprintf(log, MAX_PATH, L"%ls_%lu.txt", base, pi.dwProcessId);
		if (!MoveFileExW(tmp, log, MOVEFILE_REPLACE_EXISTING))
			wcscpy(log, tmp);
	}
	if (no_close)
		bg_protect_window(title, text);

	if (j && (j->proc = malloc(sizeof(HANDLE)))) {
		j->proc[0] = pi.hProcess;
		j->nproc = 1;
		j->pid = pi.dwProcessId;
		j->id = job_new_id();
		j->seq = ++job_seq;
		wcscpy(j->text, text);
		njobs++;
		if (log[0])
			wout(L"[%d] %lu  %ls\n", j->id, j->pid, log);
		else
			wout(L"[%d] %lu\n", j->id, j->pid);
	} else {
		/* no room in the table: keeps running, it is just not tracked */
		CloseHandle(pi.hProcess);
		wout(L"[?] %lu  %ls\n", pi.dwProcessId, log);
	}
	ant_status = 0;
	return true;
}

/*
 * $VAR, $?, ~ and * ? of every command of the pipeline, done right before it
 * runs (so "false; echo $?" and "export A=1 && echo $A" see the current state).
 * A calculator expression (2 * 3) is not globbed. false: nothing to run
 * (a command that expanded to nothing, like "$EMPTY"); ant_status is already set.
 */
static bool expand_pipeline (const ANT_PIPELINE *p, ANT_PIPELINE *x) {

	memset(x, 0, sizeof *x);
	x->op = p->op;
	x->cmd = calloc(p->ncmd, sizeof(ANT_CMD));
	if (!x->cmd) {
		ant_error(L"ant: out of memory");
		ant_status = 1;
		return false;
	}
	for (int i = 0; i < p->ncmd; i++) {
		ANT_CMD *c = &x->cmd[i];
		bool ok = ant_expand_cmd(&p->cmd[i], c, false);

		/* the glob pass only when there is a * or ? to expand */
		if (ok && ant_expand_has_wild(&p->cmd[i]) && !ant_builtin_is_calc(c)) {
			ant_expand_free(c);
			ok = ant_expand_cmd(&p->cmd[i], c, true);
		}
		if (!ok) {
			ant_error(L"ant: out of memory");
			ant_status = 1;
		} else if (c->argc == 0) {
			ant_status = 0;
			ok = false;
		}
		if (!ok) {
			for (int k = 0; k <= i; k++)
				ant_expand_free(&x->cmd[k]);
			free(x->cmd);
			x->cmd = NULL;
			return false;
		}
		x->ncmd++;
	}
	return true;
}

static bool run_pipeline (const ANT_PIPELINE *p) {

	ANT_PIPELINE x;
	bool keep;

	if (!expand_pipeline(p, &x))
		return true;
	if (x.op == OP_BG)
		keep = run_piped(&x, true);
	else if (x.ncmd == 1 && x.cmd[0].nredir == 0)
		keep = run_command(&x.cmd[0]);
	else
		keep = run_piped(&x, false);

	for (int i = 0; i < x.ncmd; i++)
		ant_expand_free(&x.cmd[i]);
	free(x.cmd);
	return keep;
}

/* @prog args: opens through ShellExecute (its own window, without waiting); '@' alone opens the current folder */
void ant_launch (const ANT_CMD *c) {

	static wchar_t params[CMDLINE_MAX], program[MAX_PATH * 4];
	const wchar_t *target = c->argv[0] + 1;
	SHELLEXECUTEINFOW sei;
	int n = 0;

	if (!*target)
		target = L".";
	else if (!wcspbrk(target, L"\\/:") && ant_path_find(target, program, MAX_PATH * 4))
		target = program;

	params[0] = 0;
	for (int i = 1; i < c->argc; i++)
		if (!append_arg(params, &n, c->argv[i]))
			break;

	memset(&sei, 0, sizeof sei);
	sei.cbSize = sizeof sei;
	/* no error dialog box: the error goes to the console */
	sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
	sei.lpVerb = L"open";
	sei.lpFile = target;
	sei.lpParameters = n ? params : NULL;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&sei)) {
		ant_error(L"ant: %ls: cannot open (error %lu)", target, GetLastError());
		ant_status = 1;
	}
}

/*
 * Runs a line (UTF-8). Returns false when the shell must close.
 * Syntax error: a message and ant_status = 2, as in sh.
 */
bool ant_run (const char *text) {

	static wchar_t line[ANTMAX];
	ANT_LIST list;
	bool keep = true, run = true;

	if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, line, ANTMAX))
		return true;
	if (!ant_parse(line, &list)) {
		ant_error(L"ant: %ls", list.error);
		ant_status = 2;
		return true;
	}

	for (int i = 0; i < list.n && keep; i++) {
		int op;
		if (run)
			keep = run_pipeline(&list.p[i]);
		op = list.p[i].op;
		run = (op == OP_AND) ? (ant_status == 0) : (op == OP_OR) ? (ant_status != 0) : true;
	}

	ant_parse_free(&list);
	return keep;
}
