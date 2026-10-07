#include "../common.h"
#include <tlhelp32.h>

/*************************************************************
 * pkill [-SIG | -s SIG] [-f] [-l] name ...                  *
 *                                                           *
 * Ends the processes by name. The name is exact (name or    *
 * name.exe, case does not matter) and may have * ? and      *
 * [a-z]: pkill "chrome*" (quote it or the shell expands).   *
 *   -SIG -s SIG  the signal, as in kill: there are no       *
 *                signals on Windows, every one ends the      *
 *                process (128 + number); -0 only counts      *
 *   -f           the name is part of the full path of the   *
 *                program (pkill -f runtime)                 *
 *   -l           only lists "pid name", kills nothing       *
 *                (what pgrep -l does)                       *
 * It never ends itself or the processes that started it     *
 * (the shell). Silent; exit code 0 if it matched, 1 if not.  *
 *************************************************************/

typedef struct {
	DWORD pid, ppid;
	wchar_t name[MAX_PATH];
} ENT;

static const struct { const wchar_t *name; int number; } signals[] = {
	{ L"HUP", 1 }, { L"INT", 2 }, { L"QUIT", 3 }, { L"ABRT", 6 }, { L"KILL", 9 }, { L"USR1", 10 },
	{ L"USR2", 12 }, { L"PIPE", 13 }, { L"ALRM", 14 }, { L"TERM", 15 }
};

static bool all_digits (const wchar_t *w) {

	if (!*w)
		return false;
	for (; *w; w++)
		if (*w < L'0' || *w > L'9')
			return false;
	return true;
}

static int signal_number (const wchar_t *name) {

	if (all_digits(name))
		return _wtoi(name) <= 64 ? _wtoi(name) : -1;
	if (!_wcsnicmp(name, L"SIG", 3))
		name += 3;
	for (int i = 0; i < (int) (sizeof signals / sizeof *signals); i++)
		if (!_wcsicmp(name, signals[i].name))
			return signals[i].number;
	return -1;
}

/* the name matches the pattern: as it is, or without the .exe */
static bool name_matches (const wchar_t *pattern, const wchar_t *exe) {

	size_t n = wcslen(exe);

	if (glob_match(pattern, exe))
		return true;
	if (n > 4 && !_wcsicmp(exe + n - 4, L".exe")) {
		wchar_t bare[MAX_PATH];
		wcsncpy(bare, exe, n - 4);
		bare[n - 4] = 0;
		return glob_match(pattern, bare);
	}
	return false;
}

static bool in_list (const DWORD *list, int n, DWORD pid) {

	for (int i = 0; i < n; i++)
		if (list[i] == pid)
			return true;
	return false;
}

int wmain (int argc, wchar_t **argv) {

	int i = 1, number = 15, count = 0, cap = 0, nskip = 0, matched = 0, done = 0;
	bool full = false, list_only = false;
	ENT *ents = NULL;
	DWORD skip[64];
	HANDLE snap;
	PROCESSENTRY32W e;

	bin_name = L"pkill";
	bin_usage = "usage: pkill [-SIG | -s SIG] [-f] [-l] name ...\n"
		"  Ends the processes with that name (exact, name or name.exe, case does not matter;\n"
		"  * ? and [a-z] allowed: pkill \"chrome*\"). Exit code 0 if one matched, 1 if none.\n"
		"  -SIG, -s SIG  the signal as in kill (every signal ends the process; -0 only counts)\n"
		"  -f            the name is part of the full path of the program\n"
		"  -l            only list \"pid name\", kill nothing\n"
		"  It never ends itself nor the processes that started it (the shell).\n";

	for (; i < argc && argv[i][0] == L'-' && argv[i][1]; i++) {
		const wchar_t *opt = argv[i] + 1;
		bool flags = true;

		if (!wcscmp(argv[i], L"--")) {
			i++;
			break;
		}
		if (!wcscmp(argv[i], L"--help")) {
			out_str(bin_usage);
			out_flush();
			return 0;
		}
		for (const wchar_t *c = opt; *c; c++)
			if (*c != L'l' && *c != L'f')
				flags = false;
		if (flags) {
			for (const wchar_t *c = opt; *c; c++) {
				if (*c == L'l')
					list_only = true;
				else
					full = true;
			}
			continue;
		}
		if (!wcscmp(opt, L"s") || !wcscmp(opt, L"n")) {
			if (++i >= argc) {
				err_print(L"option requires an argument -- '%ls'", opt);
				return 2;
			}
			opt = argv[i];
		}
		if ((number = signal_number(opt)) < 0) {
			err_print(L"%ls: invalid signal specification", opt);
			return 2;
		}
	}
	if (i >= argc) {
		err_print(L"usage: pkill [-SIG | -s SIG] [-f] [-l] name ...");
		return 2;
	}

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		err_print(L"cannot list the processes: %ls", err_text(GetLastError()));
		return 1;
	}
	memset(&e, 0, sizeof e);
	e.dwSize = sizeof e;
	if (Process32FirstW(snap, &e)) {
		do {
			if (count == cap) {
				ENT *bigger = realloc(ents, sizeof(ENT) * (cap = cap ? cap * 2 : 256));
				if (!bigger)
					return 1;
				ents = bigger;
			}
			ents[count].pid = e.th32ProcessID;
			ents[count].ppid = e.th32ParentProcessID;
			wcsncpy(ents[count].name, e.szExeFile, MAX_PATH - 1);
			ents[count].name[MAX_PATH - 1] = 0;
			count++;
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);

	/* myself and everyone who started me (the shell, the terminal...) are never touched */
	for (DWORD pid = GetCurrentProcessId(); pid && nskip < 64 && !in_list(skip, nskip, pid); ) {
		DWORD parent = 0;

		skip[nskip++] = pid;
		for (int k = 0; k < count; k++)
			if (ents[k].pid == pid)
				parent = ents[k].ppid;
		pid = parent;
	}

	for (int k = 0; k < count; k++) {
		const ENT *p = &ents[k];
		bool hit = false;
		wchar_t path[32768];

		if (in_list(skip, nskip, p->pid) || p->pid == 0)
			continue;
		if (full) {
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
			DWORD n = 32768;

			wcscpy(path, p->name);
			if (h) {
				if (!QueryFullProcessImageNameW(h, 0, path, &n))
					wcscpy(path, p->name);
				CloseHandle(h);
			}
		}
		for (int a = i; a < argc && !hit; a++) {
			if (full) {
				wchar_t wrapped[1024];
				if (wcslen(argv[a]) < 1020) {
					swprintf(wrapped, 1024, L"*%ls*", argv[a]);
					hit = glob_match(wrapped, path);
				}
			} else
				hit = name_matches(argv[a], p->name);
		}
		if (!hit)
			continue;

		matched++;
		if (list_only) {
			out_fmt("%lu ", p->pid);
			out_w(full ? path : p->name);
			out_char('\n');
			continue;
		}
		{
			HANDLE h = OpenProcess(number == 0 ? PROCESS_QUERY_LIMITED_INFORMATION : PROCESS_TERMINATE, FALSE, p->pid);
			bool ok = h != NULL;

			if (h && number != 0)
				ok = TerminateProcess(h, (UINT) (128 + number)) != 0;
			if (h)
				CloseHandle(h);
			if (ok)
				done++;
			else {
				err_print(L"(%lu) - %ls", p->pid, GetLastError() == ERROR_ACCESS_DENIED ? L"Operation not permitted" : L"No such process");
				bin_status = 1;
			}
		}
	}
	free(ents);
	out_flush();
	if (list_only)
		return matched > 0 ? 0 : 1;
	return done > 0 ? 0 : 1;
}
