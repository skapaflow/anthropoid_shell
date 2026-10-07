/*************************************************************
 * Test of the terminal integration (OSC 133/9;9 marks).     *
 *                                                           *
 * usage: osc_test.exe antshell.exe [OpenConsole.exe]              *
 *                                                           *
 * Runs antshell.exe inside a ConPTY, types commands through the *
 * input pipe and checks the marks in the output VT stream. *
 * Two sessions:                                             *
 *   - Windows ConPTY (CreatePseudoConsole);              *
 *   - OpenConsole.exe from Windows Terminal, opened as    *
 *     src/winconpty/winconpty.cpp does (if it exists). Only this *
 *     one keeps the order between marks and text, so only in it  *
 *     does the test check that the prompt (">") sits between      *
 *     133;A e 133;B.                                        *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>
#include <winternl.h>

#ifndef OBJ_INHERIT
#define OBJ_INHERIT 0x00000002
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif

/* same layout as the PseudoConsole of src/winconpty/winconpty.h (kernelbase reads hPtyReference from it) */
typedef struct {
	HANDLE hSignal;
	HANDLE hPtyReference;
	HANDLE hConPtyProcess;
} PSEUDO_CONSOLE;

typedef struct {
	bool modern;
	HPCON inbox;
	PSEUDO_CONSOLE own;
	HANDLE in_w, out_r;
	HANDLE child, reader;
	CRITICAL_SECTION lock;
	char *buf;
	size_t len, cap;
} PTY;

static int failures = 0, total = 0;

static void report (bool ok, const char *session, const char *name, const char *detail) {

	total++;
	if (ok)
		return;
	failures++;
	printf("FAIL  [%s] %s\n", session, name);
	if (detail)
		printf("      %s\n", detail);
}

/* ---------- ConPTY ---------- */

static DWORD WINAPI reader_thread (void *arg) {

	PTY *p = arg;
	char chunk[4096];
	DWORD n;

	while (ReadFile(p->out_r, chunk, sizeof chunk, &n, NULL) && n > 0) {
		EnterCriticalSection(&p->lock);
		if (p->len + n + 1 > p->cap) {
			p->cap = (p->len + n + 1) * 2;
			p->buf = realloc(p->buf, p->cap);
		}
		memcpy(p->buf + p->len, chunk, n);
		p->len += n;
		p->buf[p->len] = 0;
		LeaveCriticalSection(&p->lock);
	}
	return 0;
}

static NTSTATUS open_condrv (HANDLE *h, const wchar_t *name, ACCESS_MASK access, HANDLE parent, bool inherit, ULONG options) {

	UNICODE_STRING s;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK io;

	s.Buffer = (PWSTR) name;
	s.Length = (USHORT) (wcslen(name) * sizeof(wchar_t));
	s.MaximumLength = s.Length + sizeof(wchar_t);
	InitializeObjectAttributes(&oa, &s, OBJ_CASE_INSENSITIVE | (inherit ? OBJ_INHERIT : 0), parent, NULL);
	return NtOpenFile(h, access, &oa, &io, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, options);
}

/* _CreatePseudoConsole of winconpty.cpp, with OpenConsole.exe in place of conhost */
static bool create_modern (PTY *p, const wchar_t *openconsole, COORD size, HANDLE in_r, HANDLE out_w) {

	HANDLE server = NULL, reference = NULL, sig_conhost, sig_ours;
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T list_size = 0;
	wchar_t cmd[MAX_PATH * 2];
	HANDLE inherited[4];

	if (open_condrv(&server, L"\\Device\\ConDrv\\Server", GENERIC_ALL, NULL, true, 0) < 0)
		return false;
	if (open_condrv(&reference, L"\\Reference", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, server, false,
		FILE_SYNCHRONOUS_IO_NONALERT) < 0)
		return false;
	if (!CreatePipe(&sig_conhost, &sig_ours, NULL, 0))
		return false;
	SetHandleInformation(sig_conhost, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(in_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(out_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

	swprintf(cmd, MAX_PATH * 2, L"\"%ls\" --headless --width %d --height %d --signal 0x%llx --server 0x%llx",
		openconsole, size.X, size.Y, (unsigned long long) (ULONG_PTR) sig_conhost, (unsigned long long) (ULONG_PTR) server);

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = out_w;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	inherited[0] = server;
	inherited[1] = in_r;
	inherited[2] = out_w;
	inherited[3] = sig_conhost;
	InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list_size);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &list_size);
	UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, NULL, NULL);

	bool ok = CreateProcessW(openconsole, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
		&si.StartupInfo, &pi);
	DeleteProcThreadAttributeList(si.lpAttributeList);
	HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
	CloseHandle(server);
	CloseHandle(sig_conhost);
	if (!ok)
		return false;
	CloseHandle(pi.hThread);

	p->own.hSignal = sig_ours;
	p->own.hPtyReference = reference;
	p->own.hConPtyProcess = pi.hProcess;
	return true;
}

static bool pty_start (PTY *p, bool modern, const wchar_t *openconsole, const wchar_t *term) {

	HANDLE in_r, out_w;
	COORD size = { 120, 30 };
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T list_size = 0;
	wchar_t cmd[MAX_PATH + 4];
	void *pc;

	memset(p, 0, sizeof *p);
	p->modern = modern;
	InitializeCriticalSection(&p->lock);
	if (!CreatePipe(&in_r, &p->in_w, NULL, 0) || !CreatePipe(&p->out_r, &out_w, NULL, 0))
		return false;

	if (modern) {
		if (!create_modern(p, openconsole, size, in_r, out_w))
			return false;
		pc = &p->own;
	} else {
		if (FAILED(CreatePseudoConsole(size, in_r, out_w, 0, &p->inbox)))
			return false;
		pc = p->inbox;
	}
	/* ConPTY kept its ends */
	CloseHandle(in_r);
	CloseHandle(out_w);

	p->reader = CreateThread(NULL, 0, reader_thread, p, 0, NULL);

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	/* null standard handles: without this the child inherits the test's stdout (when it is a pipe)
	   instead of using the ConPTY console */
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list_size);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &list_size);
	UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof(HPCON), NULL, NULL);
	swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", term);
	bool ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
	DeleteProcThreadAttributeList(si.lpAttributeList);
	HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
	if (!ok)
		return false;
	CloseHandle(pi.hThread);
	p->child = pi.hProcess;
	return true;
}

static void pty_stop (PTY *p) {

	if (p->modern) {
		/* like ClosePseudoConsole: closes the signal and the reference, OpenConsole exits */
		CloseHandle(p->own.hSignal);
		CloseHandle(p->own.hPtyReference);
		if (WaitForSingleObject(p->own.hConPtyProcess, 3000) != WAIT_OBJECT_0)
			TerminateProcess(p->own.hConPtyProcess, 1);
		CloseHandle(p->own.hConPtyProcess);
	} else
		ClosePseudoConsole(p->inbox);
	CloseHandle(p->in_w);
	if (WaitForSingleObject(p->reader, 3000) != WAIT_OBJECT_0)
		TerminateThread(p->reader, 1);
	CloseHandle(p->reader);
	CloseHandle(p->out_r);
	if (p->child)
		CloseHandle(p->child);
	DeleteCriticalSection(&p->lock);
}

static void pty_send (PTY *p, const char *text) {

	DWORD n;
	WriteFile(p->in_w, text, (DWORD) strlen(text), &n, NULL);
}

static int count_of (PTY *p, const char *needle) {

	int c = 0;

	EnterCriticalSection(&p->lock);
	for (const char *s = p->buf; s && (s = strstr(s, needle)); s++)
		c++;
	LeaveCriticalSection(&p->lock);
	return c;
}

static bool wait_count (PTY *p, const char *needle, int want, int ms) {

	for (int t = 0; t < ms; t += 20) {
		if (count_of(p, needle) >= want)
			return true;
		Sleep(20);
	}
	return false;
}

/* ---------- stream analysis ---------- */

/* prompt text expected in the session: just ">" */
static char prompt_texts[2][MAX_PATH * 3];

/* events in order: OSC 133 and 9;9 marks ("A", "B", "C", "D;n", "9;9;folder") and "PROMPT" for the prompt text */
static int events_of (const char *buf, char events[][MAX_PATH * 3], int max) {

	int n = 0;

	for (const char *s = buf; *s && n < max; ) {
		if (s[0] == 0x1b && s[1] == ']') {
			const char *b = s + 2, *e = b;
			while (*e && *e != 0x07 && !(e[0] == 0x1b && e[1] == '\\'))
				e++;
			int len = (int) (e - b);
			if (!strncmp(b, "133;", 4) && len - 4 < MAX_PATH * 3) {
				memcpy(events[n], b + 4, len - 4);
				events[n++][len - 4] = 0;
			} else if (!strncmp(b, "9;9;", 4) && len < MAX_PATH * 3) {
				memcpy(events[n], b, len);
				events[n++][len] = 0;
			}
			s = *e ? e + (*e == 0x07 ? 1 : 2) : e;
		} else if (!strncmp(s, prompt_texts[0], strlen(prompt_texts[0]))) {
			strcpy(events[n++], "PROMPT");
			s += strlen(prompt_texts[0]);
		} else if (!strncmp(s, prompt_texts[1], strlen(prompt_texts[1]))) {
			strcpy(events[n++], "PROMPT");
			s += strlen(prompt_texts[1]);
		} else
			s++;
	}
	return n;
}

static void run_session (bool modern, const wchar_t *openconsole, const wchar_t *term) {

	const char *name = modern ? "OpenConsole do Windows Terminal" : "ConPTY do Windows";
	static char events[256][MAX_PATH * 3];
	char expected[4096], got[4096], cwd9[MAX_PATH * 3 + 8], src9[MAX_PATH * 3 + 16], detail[6200];
	wchar_t cwd[MAX_PATH];
	PTY p;
	int n;

	if (!pty_start(&p, modern, openconsole, term)) {
		report(false, name, "open antshell.exe in the ConPTY", NULL);
		return;
	}

	bool ready = wait_count(&p, "\x1b]133;B", 1, 15000);
	report(ready, name, "first prompt (133;B)", "the prompt did not show up");
	if (ready) {
		pty_send(&p, "zz_nao_existe\r");
		wait_count(&p, "\x1b]133;B", 2, 10000);
		pty_send(&p, "cmd /c exit 7\r");
		wait_count(&p, "\x1b]133;B", 3, 10000);
		pty_send(&p, "cd src\r");
		wait_count(&p, "\x1b]133;B", 4, 10000);
		pty_send(&p, "cd zz_nao_existe\r");
		wait_count(&p, "\x1b]133;B", 5, 10000);
		pty_send(&p, "ver && cmd /c exit 5\r");
		wait_count(&p, "\x1b]133;B", 6, 10000);
		pty_send(&p, "cmd /c exit 3 || cmd /c exit 4\r");
		wait_count(&p, "\x1b]133;B", 7, 10000);
		pty_send(&p, "cmd /c exit 3 && ver\r");
		wait_count(&p, "\x1b]133;B", 8, 10000);
		pty_send(&p, "| ver\r");
		wait_count(&p, "\x1b]133;B", 9, 10000);
		/* exit without an argument closes with the code of the last command (here the syntax error, 2), as POSIX says */
		pty_send(&p, "exit\r");
		report(WaitForSingleObject(p.child, 10000) == WAIT_OBJECT_0, name, "exit ends antshell.exe", NULL);
		Sleep(300);
	}
	pty_stop(&p);

	/* expected sequence of the marks (the prompt text only takes part in the order check):
	   9;9 follows the cd; a good cd gives D;0 and a missing folder D;1; lists give the
	   code of the last command that ran (ver && exit 5 = 5, exit 3 || exit 4 = 4,
	   exit 3 && ver = 3); a syntax error gives 2 */
	GetCurrentDirectoryW(MAX_PATH, cwd);
	strcpy(cwd9, "9;9;");
	WideCharToMultiByte(CP_UTF8, 0, cwd, -1, cwd9 + 4, MAX_PATH * 3, NULL, NULL);
	snprintf(src9, sizeof src9, "%s\\src", cwd9);
	strcpy(prompt_texts[0], ">");
	strcpy(prompt_texts[1], ">");
	snprintf(expected, sizeof expected,
		"A|%s|B|C|D;9009|A|%s|B|C|D;7|A|%s|B|C|D;0|A|%s|B|C|D;1|A|%s|B|C|D;5|"
		"A|%s|B|C|D;4|A|%s|B|C|D;3|A|%s|B|C|D;2|A|%s|B|C|D;2|",
		cwd9, cwd9, cwd9, src9, src9, src9, src9, src9, src9);

	n = events_of(p.buf ? p.buf : "", events, 256);
	got[0] = 0;
	for (int i = 0; i < n; i++)
		if (strcmp(events[i], "PROMPT")) {
			strncat(got, events[i], sizeof got - strlen(got) - 2);
			strcat(got, "|");
		}
	snprintf(detail, sizeof detail, "expected: %s\n      got:      %s", expected, got);
	report(!strcmp(got, expected), name,
		"marks: A 9;9 B, C, D;9009, D;7, 9;9 follows the cd, D;1, lists (D;5 D;4 D;3), syntax D;2, exit without an argument D;2", detail);
	report(n > 0 && !strcmp(events[0], "A"), name, "first mark is 133;A (no D before the 1st command)", NULL);

	/* only OpenConsole keeps the order between marks and text */
	if (modern) {
		bool order = true;
		int prompts = 0;
		for (int i = 0; i < n; i++)
			if (!strcmp(events[i], "PROMPT")) {
				prompts++;
				/* before: A and 9;9; after: B */
				if (i < 2 || strcmp(events[i-2], "A") || strncmp(events[i-1], "9;9;", 4) ||
					i + 1 >= n || strcmp(events[i+1], "B"))
					order = false;
			}
		report(order && prompts == 9, name, "the prompt (>) sits between 133;A/9;9 and 133;B in the 9 prompts", NULL);
	}

	free(p.buf);
}

/* Ctrl+C in a long program: the program stops and the shell goes on (SetConsoleCtrlHandler) */
static void run_ctrl_c (bool modern, const wchar_t *openconsole, const wchar_t *term) {

	const char *name = modern ? "OpenConsole do Windows Terminal" : "ConPTY do Windows";
	PTY p;

	if (!pty_start(&p, modern, openconsole, term)) {
		report(false, name, "open antshell.exe in the ConPTY (Ctrl+C)", NULL);
		return;
	}
	if (wait_count(&p, "\x1b]133;B", 1, 15000)) {
		/* a background job (its own group) must not die with Ctrl+C */
		pty_send(&p, "ping -n 8 127.0.0.2 > NUL &\r");
		wait_count(&p, "\x1b]133;B", 2, 10000);
		/* the line echo, the ping header and the 1st reply show the IP */
		pty_send(&p, "ping -n 30 127.0.0.1\r");
		wait_count(&p, "127.0.0.1", 3, 10000);
		Sleep(300);
		pty_send(&p, "\x03");
		bool back = wait_count(&p, "\x1b]133;B", 3, 10000);
		report(back, name, "Ctrl+C interrupts the program and the shell shows the next prompt",
			WaitForSingleObject(p.child, 0) == WAIT_TIMEOUT ? "antshell.exe is alive, but the prompt did not come back" : "antshell.exe exited");
		if (!back) {
			/* end of the output, for diagnostics */
			EnterCriticalSection(&p.lock);
			printf("      --- end of the output:\n      ");
			for (size_t i = p.len > 600 ? p.len - 600 : 0; i < p.len; i++) {
				char ch = p.buf[i];
				if (ch == 0x1b)
					printf("<ESC>");
				else if (ch == '\n')
					printf("\n      ");
				else if (ch != '\r')
					putchar(ch);
			}
			putchar('\n');
			LeaveCriticalSection(&p.lock);
		}
		pty_send(&p, "jobs\r");
		wait_count(&p, "\x1b]133;B", 4, 10000);
		report(count_of(&p, "running") > 0 && count_of(&p, "127.0.0.2") > 1, name,
			"background job survives the foreground Ctrl+C", "the job is gone");
		pty_send(&p, "nop\r");
		report(WaitForSingleObject(p.child, 10000) == WAIT_OBJECT_0, name, "the shell still responds after Ctrl+C", NULL);
	} else
		report(false, name, "first prompt (Ctrl+C)", NULL);
	pty_stop(&p);
	free(p.buf);
}

int main (int argc, char **argv) {

	wchar_t term[MAX_PATH], openconsole[MAX_PATH];
	int wargc;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

	(void) argc;
	(void) argv;
	if (wargc < 2) {
		fprintf(stderr, "usage: osc_test antshell.exe [OpenConsole.exe]\n");
		return 2;
	}
	GetFullPathNameW(wargv[1], MAX_PATH, term, NULL);

	/* "ignore Ctrl+C" is inherited by the children; if whoever started the test turned it on, turn it off */
	SetConsoleCtrlHandler(NULL, FALSE);

	run_session(false, NULL, term);
	run_ctrl_c(false, NULL, term);

	if (wargc >= 3 && GetFileAttributesW(wargv[2]) != INVALID_FILE_ATTRIBUTES) {
		GetFullPathNameW(wargv[2], MAX_PATH, openconsole, NULL);
		run_session(true, openconsole, term);
		run_ctrl_c(true, openconsole, term);
	} else
		printf("SKIP  OpenConsole.exe from Windows Terminal not found; only the Windows ConPTY was tested\n");

	printf("osc: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
