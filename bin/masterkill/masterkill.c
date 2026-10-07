#include "../common.h"
#include <tlhelp32.h>

/*************************************************************
 * masterkill [name ...]                                     *
 *                                                           *
 * Ends processes by name and SAYS which ones. Where pkill   *
 * is a quiet unix program, masterkill is loud on purpose:   *
 * every service it ends (or fails to) is declared, one per  *
 * line, so you know the junk is gone. That is the whole     *
 * point -- here the silent-unix rule does not help.         *
 *                                                           *
 * With no name it lists the running processes in a grid     *
 * (the ones of session 0 -- services -- in one color, the   *
 * ones of your session in another).                         *
 *                                                           *
 * The term matches the START of the process name (a prefix, *
 * case does not matter): masterkill runtime ends every       *
 * RuntimeBroker. Several terms at once.                      *
 *                                                           *
 * It never ends itself nor whoever started it (the shell).  *
 * Exit code 0 when every name given ended at least one and   *
 * none was denied; 1 otherwise.                             *
 *************************************************************/

typedef struct {
	DWORD pid, ppid, session;
	wchar_t name[MAX_PATH];
} ENT;

static ENT *procs = NULL;
static int nprocs = 0;

/* lowercase a name with full Unicode case folding, independent of the
 * locale (so case is ignored for accented letters too, not only A-Z) */
static void fold (wchar_t *dst, const wchar_t *src) {

	if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, src, -1,
	                  dst, MAX_PATH, NULL, NULL, 0) <= 0) {
		wcsncpy(dst, src, MAX_PATH - 1);
		dst[MAX_PATH - 1] = 0;
	}
}

/* the term is a prefix of the process name (anchored at the start,
 * case never matters): "runtime" matches "RuntimeBroker.exe" */
static bool name_matches (const wchar_t *term, const wchar_t *exe) {

	wchar_t lt[MAX_PATH], le[MAX_PATH];
	size_t n;

	fold(lt, term);
	fold(le, exe);
	n = wcslen(lt);

	return n > 0 && wcsncmp(le, lt, n) == 0;
}

static bool in_list (const DWORD *list, int n, DWORD pid) {

	for (int i = 0; i < n; i++)
		if (list[i] == pid)
			return true;
	return false;
}

/* true only when we can prove 'parent' started AFTER 'child' -- then the
 * ppid Toolhelp reported is a reused, stale number, not our real parent */
static bool stale_parent (DWORD parent, DWORD child) {

	HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
	HANDLE hc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child);
	FILETIME pc, pe, pk, pu, cc, ce, ck, cu;
	bool stale = false;

	if (hp && hc && GetProcessTimes(hp, &pc, &pe, &pk, &pu) &&
	    GetProcessTimes(hc, &cc, &ce, &ck, &cu))
		stale = CompareFileTime(&pc, &cc) > 0;
	if (hp)
		CloseHandle(hp);
	if (hc)
		CloseHandle(hc);
	return stale;
}

/* loud badge: "  <mark> <label> name (pid)" with color on a console */
static void report (WORD mark_attr, const char *mark, WORD label_attr, const char *label,
                    const wchar_t *name, DWORD pid) {

	out_char(' ');
	out_color(mark_attr);
	out_str(mark);
	out_color_off();
	out_color(label_attr);
	out_str(label);
	out_color_off();
	out_char(' ');
	out_w(name);
	if (pid)
		out_fmt(" (%lu)", pid);
	out_char('\n');
}

/* no name: the running processes in a grid, six per line */
static void list_grid (void) {

	int col = 0;

	for (int k = 0; k < nprocs; k++) {
		ENT *p = &procs[k];
		wchar_t cell[16];

		if (!p->pid)
			continue; /* the System Idle Process (pid 0) */
		swprintf(cell, 16, L"%15.15ls", p->name);
		out_color(p->session ? 31 : 12);
		out_w(cell);
		out_color_off();
		out_color(11);
		out_str(col < 5 ? "|" : "\n");
		out_color_off();
		col = (col < 5) ? col + 1 : 0;
	}
	if (col != 0)
		out_char('\n');
}

int wmain (int argc, wchar_t **argv) {

	HANDLE snap;
	PROCESSENTRY32W e;
	DWORD skip[64];
	int i = 1, cap = 0, nskip = 0;
	int ended = 0, denied = 0, unknown = 0;

	bin_name = L"masterkill";
	bin_usage = "usage: masterkill [term ...]\n"
		"  Ends processes by name and says which ones: loud, not silent. Every\n"
		"  service it ends, denies or cannot find is declared, one per line.\n"
		"  Each term matches the START of the name (a prefix, case ignored):\n"
		"  masterkill runtime ends every RuntimeBroker. Several terms at once.\n"
		"  With no term it lists the running processes.\n"
		"  It never ends itself nor whoever started it (the shell).\n"
		"  Exit 0 if every term ended at least one and none was denied; else 1.\n";

	for (; i < argc && argv[i][0] == L'-' && argv[i][1]; i++) {
		if (!wcscmp(argv[i], L"--")) {
			i++;
			break;
		}
		if (!wcscmp(argv[i], L"--help")) {
			out_str(bin_usage);
			out_flush();
			return 0;
		}
		err_print(L"unrecognized option '%ls'", argv[i]);
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
			if (nprocs == cap) {
				ENT *bigger = realloc(procs, sizeof(ENT) * (cap = cap ? cap * 2 : 256));
				if (!bigger) {
					CloseHandle(snap);
					return 1;
				}
				procs = bigger;
			}
			procs[nprocs].pid = e.th32ProcessID;
			procs[nprocs].ppid = e.th32ParentProcessID;
			procs[nprocs].session = 0;
			ProcessIdToSessionId(e.th32ProcessID, &procs[nprocs].session);
			wcsncpy(procs[nprocs].name, e.szExeFile, MAX_PATH - 1);
			procs[nprocs].name[MAX_PATH - 1] = 0;
			nprocs++;
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);

	if (i >= argc) {
		list_grid();
		out_flush();
		free(procs);
		return 0;
	}

	/* Never touch myself nor the shell that launched me -- and only those
	 * two. We do NOT walk the whole parent chain: a ppid from Toolhelp can
	 * be a reused, stale number, and climbing it can wander onto an
	 * unrelated live process and wrongly spare it (that is why a running
	 * service could come back UNKNOWN). The one hop to the parent is
	 * trusted only when the parent really started before us. */
	{
		DWORD me = GetCurrentProcessId(), parent = 0;

		skip[nskip++] = me;
		for (int k = 0; k < nprocs; k++)
			if (procs[k].pid == me) {
				parent = procs[k].ppid;
				break;
			}
		if (parent && !stale_parent(parent, me))
			skip[nskip++] = parent;
	}

	for (; i < argc; i++) {
		bool matched = false;

		for (int k = 0; k < nprocs; k++) {
			ENT *p = &procs[k];
			HANDLE h;

			if (!p->pid || in_list(skip, nskip, p->pid))
				continue;
			if (!name_matches(argv[i], p->name))
				continue;

			matched = true;
			h = OpenProcess(PROCESS_TERMINATE, FALSE, p->pid);
			if (!h) {
				denied++;
				report(207, " ! ", 128, "FAIL:", p->name, p->pid);
			} else if (TerminateProcess(h, 143)) { /* 128 + TERM(15) */
				ended++;
				report(26, " > ", 31, "KILL:", p->name, p->pid);
				CloseHandle(h);
			} else {
				denied++;
				report(79, " ! ", 74, "DENIED:", p->name, p->pid);
				CloseHandle(h);
			}
		}
		if (!matched) {
			unknown++;
			report(176, " ! ", 48, "UNKNOWN:", argv[i], 0);
		}
	}

	out_fmt("\n %d ended, %d denied, %d not found\n", ended, denied, unknown);
	out_flush();
	free(procs);
	return (denied == 0 && unknown == 0) ? 0 : 1;
}
