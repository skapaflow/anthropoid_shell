#include "../common.h"
#include <tlhelp32.h>
#include <psapi.h>

/*************************************************************
 * ps [-f] [-h] [-m] [-p pid,pid] [-C name]                  *
 *                                                           *
 * The processes that are running, one per line:             *
 *   PID PPID THR MEM(KiB) NAME                              *
 * (the working set of the process; "-" when Windows does   *
 * not let it be read, as with some system processes).       *
 *   -e -A -a -x  accepted (every process is listed anyway)  *
 *   -f           the full path of the program, when known   *
 *   -h           no header line                             *
 *   -m           the ones that use more memory first        *
 *   -p list      only these pids (commas)                   *
 *   -C name      only this program (name or name.exe)       *
 * Sorted by pid. Exit code 1 when nothing matched.          *
 *************************************************************/

typedef struct {
	DWORD pid, ppid, threads;
	unsigned long long mem; /* KiB; ~0 = unknown */
	wchar_t name[MAX_PATH];
	wchar_t *path;
} PROCINFO;

static bool by_memory;

static int compare (const void *a, const void *b) {

	const PROCINFO *x = a, *y = b;

	if (by_memory) {
		unsigned long long mx = x->mem == ~0ULL ? 0 : x->mem, my = y->mem == ~0ULL ? 0 : y->mem;
		if (mx != my)
			return mx > my ? -1 : 1;
	}
	return x->pid < y->pid ? -1 : x->pid > y->pid ? 1 : 0;
}

static bool pid_selected (const DWORD *pids, int n, DWORD pid) {

	for (int i = 0; i < n; i++)
		if (pids[i] == pid)
			return true;
	return false;
}

/* name or name.exe, ignoring case */
static bool name_is (const wchar_t *exe, const wchar_t *want) {

	size_t n = wcslen(want);

	if (!_wcsicmp(exe, want))
		return true;
	return n + 4 == wcslen(exe) && !_wcsnicmp(exe, want, n) && !_wcsicmp(exe + n, L".exe");
}

int wmain (int argc, wchar_t **argv) {

	OPTS o;
	int ch, count = 0, cap = 0, npids = 0;
	bool full = false, header = true;
	DWORD pids[256];
	const wchar_t *only_name = NULL;
	HANDLE snap;
	PROCESSENTRY32W e;
	PROCINFO *list = NULL;

	bin_name = L"ps";
	bin_usage = "usage: ps [-f] [-h] [-m] [-p pid,pid] [-C name]\n"
		"  The running processes: PID, parent PID, threads, memory (KiB) and name.\n"
		"  -f  the full path of the program      -h  no header line\n"
		"  -m  the ones that use more memory first\n"
		"  -p  only these pids (separated by commas)   -C  only this program (name or name.exe)\n"
		"  -e -A -a -x are accepted (every process is listed). Sorted by pid.\n"
		"  To end one: kill pid. To find one: ps | grep name.\n";

	opt_init(&o, argc, argv);
	while ((ch = opt_next(&o)) != -1) {
		switch (ch) {
			case 'e': case 'A': case 'a': case 'x': break;
			case 'f': full = true; break;
			case 'h': header = false; break;
			case 'm': by_memory = true; break;
			case 'C': only_name = opt_arg(&o, ch); break;
			case 'p': {
				wchar_t *copy = _wcsdup(opt_arg(&o, ch)), *ctx = NULL, *tok;

				if (!copy)
					return 1;
				for (tok = wcstok(copy, L",", &ctx); tok; tok = wcstok(NULL, L",", &ctx)) {
					wchar_t *end;
					long v = wcstol(tok, &end, 10);
					if (*end || v <= 0 || npids == 256) {
						err_print(L"invalid process id: '%ls'", tok);
						return 1;
					}
					pids[npids++] = (DWORD) v;
				}
				free(copy);
				break;
			}
			default: opt_bad(ch);
		}
	}
	if (o.i < argc) {
		err_print(L"unexpected argument '%ls'", argv[o.i]);
		err_print(L"Try '%ls --help' for more information.", bin_name);
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
			PROCINFO *p;
			HANDLE h;

			if (npids > 0 && !pid_selected(pids, npids, e.th32ProcessID))
				continue;
			if (only_name && !name_is(e.szExeFile, only_name))
				continue;
			if (count == cap) {
				PROCINFO *bigger = realloc(list, sizeof(PROCINFO) * (cap = cap ? cap * 2 : 256));
				if (!bigger)
					return 1;
				list = bigger;
			}
			p = &list[count++];
			memset(p, 0, sizeof *p);
			p->pid = e.th32ProcessID;
			p->ppid = e.th32ParentProcessID;
			p->threads = e.cntThreads;
			p->mem = ~0ULL;
			wcsncpy(p->name, e.szExeFile, MAX_PATH - 1);

			h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
			if (h) {
				PROCESS_MEMORY_COUNTERS pmc;
				if (K32GetProcessMemoryInfo(h, &pmc, sizeof pmc))
					p->mem = pmc.WorkingSetSize / 1024;
				if (full) {
					wchar_t path[32768];
					DWORD n = 32768;
					if (QueryFullProcessImageNameW(h, 0, path, &n))
						p->path = _wcsdup(path);
				}
				CloseHandle(h);
			}
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);

	if (count > 1)
		qsort(list, count, sizeof(PROCINFO), compare);
	if (header)
		out_str("   PID   PPID  THR  MEM(KiB)  NAME\n");
	for (int i = 0; i < count; i++) {
		PROCINFO *p = &list[i];

		if (p->mem == ~0ULL)
			out_fmt("%6lu %6lu %4lu %9s  ", p->pid, p->ppid, p->threads, "-");
		else
			out_fmt("%6lu %6lu %4lu %9llu  ", p->pid, p->ppid, p->threads, p->mem);
		out_w(p->path ? p->path : p->name);
		out_char('\n');
		free(p->path);
	}
	free(list);
	out_flush();
	return count > 0 ? 0 : 1;
}
