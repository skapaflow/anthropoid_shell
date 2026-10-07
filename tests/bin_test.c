/*************************************************************
 * Test of the programs in bin\ (ls, cat, mkdir, rm, cp, mv,  *
 * touch, head, tail, wc, sort, grep).                       *
 *                                                           *
 * usage: bin_test.exe <bin folder>                             *
 *                                                           *
 * Each case runs the program in a temporary folder, with the    *
 * given standard input, and compares the output (output and error  *
 * together, in UTF-8) and the exit code.                    *
 *************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

static int failures = 0, total = 0;
static wchar_t bin_dir[MAX_PATH];

/* runs "<bin>\<prog>.exe <args>" with 'input' on stdin; returns the exit code; out = output + error */
static int run (const wchar_t *prog, const wchar_t *args, const char *input, char *out, int max) {

	SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
	HANDLE out_r, out_w, in_r, in_w;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	wchar_t cmd[4096];
	DWORD code = 99, got, wrote;
	int n = 0;

	out[0] = 0;
	CreatePipe(&out_r, &out_w, &sa, 0);
	CreatePipe(&in_r, &in_w, &sa, 0);
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = in_r;
	si.hStdOutput = out_w;
	si.hStdError = out_w;
	swprintf(cmd, 4096, L"\"%ls\\%ls.exe\" %ls", bin_dir, prog, args);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		printf("FAIL  could not open %ls\n", cmd);
		failures++;
		total++;
		return -1;
	}
	CloseHandle(out_w);
	CloseHandle(in_r);
	if (input && *input)
		WriteFile(in_w, input, (DWORD) strlen(input), &wrote, NULL);
	CloseHandle(in_w);
	while (ReadFile(out_r, out + n, max - 1 - n, &got, NULL) && got > 0)
		n += got;
	out[n] = 0;
	CloseHandle(out_r);
	WaitForSingleObject(pi.hProcess, 10000);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return (int) code;
}

/* one case: program, arguments (UTF-8), input, expected output, expected code */
static void t (const char *name, const wchar_t *prog, const wchar_t *args, const char *input, const char *want, int want_code) {

	static char got[1 << 20];
	int code = run(prog, args, input, got, sizeof got);

	total++;
	if (code == want_code && !strcmp(got, want))
		return;
	failures++;
	printf("FAIL  %s\n      expected (%d): [%s]\n      got      (%d): [%s]\n", name, want_code, want, code, got);
}

/* opens "<exe> 30" hidden; false if it did not open */
static bool spawn_sleeper (const wchar_t *exe, PROCESS_INFORMATION *pi) {

	STARTUPINFOW si;
	wchar_t cmd[MAX_PATH * 2];

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	swprintf(cmd, MAX_PATH * 2, L"\"%ls\" 30", exe);
	return CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, pi) != 0;
}

/* did the process end with that code? (closes the handles; if it is still alive, kills it) */
static bool ended_with (PROCESS_INFORMATION *pi, DWORD want) {

	DWORD code = 0;
	bool ok = WaitForSingleObject(pi->hProcess, 5000) == WAIT_OBJECT_0;

	if (ok)
		GetExitCodeProcess(pi->hProcess, &code);
	else
		TerminateProcess(pi->hProcess, 1);
	CloseHandle(pi->hProcess);
	CloseHandle(pi->hThread);
	return ok && code == want;
}

static void put (const wchar_t *path, const char *text) {

	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD wrote;

	if (h != INVALID_HANDLE_VALUE) {
		WriteFile(h, text, (DWORD) strlen(text), &wrote, NULL);
		CloseHandle(h);
	}
}

static bool exists (const wchar_t *path) {

	return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void check_true (const char *name, bool ok) {

	total++;
	if (!ok) {
		failures++;
		printf("FAIL  %s\n", name);
	}
}

int wmain (int argc, wchar_t **argv) {

	wchar_t base[MAX_PATH], old[MAX_PATH];

	if (argc != 2) {
		fprintf(stderr, "usage: bin_test <bin folder>\n");
		return 2;
	}
	GetFullPathNameW(argv[1], MAX_PATH, bin_dir, NULL);
	GetCurrentDirectoryW(MAX_PATH, old);
	GetTempPathW(MAX_PATH, base);
	wcscat(base, L"ant_bin_test");
	CreateDirectoryW(base, NULL);
	SetCurrentDirectoryW(base);

	/* setup: a clean tree */
	{
		wchar_t cmd[MAX_PATH + 16];
		char junk[4096];
		swprintf(cmd, MAX_PATH + 16, L"-rf \"%ls\"", base);
		/* rm refuses '.', so the contents are deleted from outside */
		SetCurrentDirectoryW(old);
		run(L"rm", cmd, NULL, junk, sizeof junk);
		CreateDirectoryW(base, NULL);
		SetCurrentDirectoryW(base);
	}

	/* ---- mkdir, touch ---- */
	t("mkdir creates a folder", L"mkdir", L"d1", NULL, "", 0);
	check_true("mkdir created d1", exists(L"d1"));
	t("mkdir of an existing folder fails", L"mkdir", L"d1", NULL, "mkdir: cannot create directory 'd1': File exists\n", 1);
	t("mkdir -p creates the parents", L"mkdir", L"-p a\\b\\c \"x y\"", NULL, "", 0);
	check_true("mkdir -p created a\\b\\c", exists(L"a\\b\\c"));
	check_true("mkdir created 'x y'", exists(L"x y"));
	t("mkdir -p accepts what already exists", L"mkdir", L"-p a/b", NULL, "", 0);
	t("mkdir without an argument", L"mkdir", L"", NULL, "mkdir: missing operand\nmkdir: Try 'mkdir --help' for more information.\n", 2);
	t("mkdir invalid option", L"mkdir", L"-z", NULL, "mkdir: invalid option -- 'z'\nmkdir: Try 'mkdir --help' for more information.\n", 2);

	t("touch creates the file", L"touch", L"new.txt \"ação 中.txt\"", NULL, "", 0);
	check_true("touch created new.txt", exists(L"new.txt"));
	check_true("touch created the file with an accent and CJK", exists(L"ação 中.txt"));
	t("touch -c does not create", L"touch", L"-c not_created.txt", NULL, "", 0);
	check_true("touch -c did not create", !exists(L"not_created.txt"));

	/* ---- cat ---- */
	put(L"f1.txt", "one\ntwo\n");
	put(L"f2.txt", "three no newline");
	t("cat of one file", L"cat", L"f1.txt", NULL, "one\ntwo\n", 0);
	t("cat of two files", L"cat", L"f1.txt f2.txt", NULL, "one\ntwo\nthree no newline", 0);
	t("cat -n numbers", L"cat", L"-n f1.txt", NULL, "     1\tone\n     2\ttwo\n", 0);
	t("cat reads standard input", L"cat", L"", "da entrada\n", "da entrada\n", 0);
	t("cat - reads standard input", L"cat", L"f1.txt - f1.txt", "middle\n", "one\ntwo\nmiddle\none\ntwo\n", 0);
	t("cat of a missing file", L"cat", L"nope.txt", NULL, "cat: nope.txt: No such file or directory\n", 1);
	t("cat goes on after the error", L"cat", L"nope.txt f1.txt", NULL, "cat: nope.txt: No such file or directory\none\ntwo\n", 1);
	t("cat of a folder", L"cat", L"d1", NULL, "cat: d1: Is a directory\n", 1);
	t("cat passes UTF-8 through unchanged", L"cat", L"", "olá 中 😊\n", "olá 中 😊\n", 0);

	/* ---- ls ---- */
	t("mkdir lsdir", L"mkdir", L"lsdir", NULL, "", 0);
	put(L"lsdir\\b.txt", "bb");
	put(L"lsdir\\A.txt", "a");
	put(L"lsdir\\.hidden", "h");
	put(L"lsdir\\ação.txt", "x");
	t("mkdir lsdir\\sub", L"mkdir", L"lsdir\\sub", NULL, "", 0);
	t("ls in a pipe: one per line, no hidden ones, case-insensitive", L"ls", L"lsdir", NULL, "A.txt\nação.txt\nb.txt\nsub\n", 0);
	t("ls -a shows the hidden ones", L"ls", L"-a lsdir", NULL, ".\n..\n.hidden\nA.txt\nação.txt\nb.txt\nsub\n", 0);
	t("ls -1", L"ls", L"-1 lsdir", NULL, "A.txt\nação.txt\nb.txt\nsub\n", 0);
	t("ls of a file", L"ls", L"lsdir\\b.txt", NULL, "lsdir\\b.txt\n", 0);
	t("ls of a file and a folder", L"ls", L"lsdir\\b.txt lsdir\\sub", NULL, "lsdir\\b.txt\n\nlsdir\\sub:\n", 0);
	t("ls of two folders", L"ls", L"lsdir\\sub d1", NULL, "lsdir\\sub:\n\nd1:\n", 0);
	t("ls of a missing file", L"ls", L"nope", NULL, "ls: cannot access 'nope': No such file or directory\n", 2);
	{
		static char got[8192];
		run(L"ls", L"-l lsdir", NULL, got, sizeof got);
		total++;
		if (!strstr(got, "d---") || !strstr(got, " sub\n") || !strstr(got, "           2 ")) {
			failures++;
			printf("FAIL  ls -l shows attributes, size and name\n      got: [%s]\n", got);
		}
		run(L"ls", L"-lh lsdir", NULL, got, sizeof got);
		total++;
		if (!strstr(got, "     2 ")) {
			failures++;
			printf("FAIL  ls -lh shows the size\n      got: [%s]\n", got);
		}
	}

	/* ---- cp ---- */
	t("cp copies a file", L"cp", L"f1.txt copy.txt", NULL, "", 0);
	t("the copy is identical", L"cat", L"copy.txt", NULL, "one\ntwo\n", 0);
	t("cp into a folder", L"cp", L"f1.txt d1", NULL, "", 0);
	t("the file is in d1", L"cat", L"d1\\f1.txt", NULL, "one\ntwo\n", 0);
	t("cp of several into a folder", L"cp", L"f1.txt f2.txt d1", NULL, "", 0);
	t("cp of several into a file fails", L"cp", L"f1.txt f2.txt copy.txt", NULL, "cp: target 'copy.txt': Not a directory\n", 1);
	t("cp replaces the destination", L"cp", L"f2.txt copy.txt", NULL, "", 0);
	t("the destination was replaced", L"cat", L"copy.txt", NULL, "three no newline", 0);
	t("cp of a folder without -r", L"cp", L"lsdir lsdir2", NULL, "cp: -r not specified; omitting directory 'lsdir'\n", 1);
	t("cp -r copies the whole folder", L"cp", L"-r lsdir lsdir2", NULL, "", 0);
	t("the copied folder has everything", L"ls", L"-a lsdir2", NULL, ".\n..\n.hidden\nA.txt\nação.txt\nb.txt\nsub\n", 0);
	t("cp -r into an existing folder goes inside it", L"cp", L"-r lsdir d1", NULL, "", 0);
	check_true("d1\\lsdir\\b.txt exists", exists(L"d1\\lsdir\\b.txt"));
	t("cp of a folder into itself", L"cp", L"-r lsdir lsdir\\sub\\inside", NULL,
		"cp: cannot copy a directory, 'lsdir', into itself, 'lsdir\\sub\\inside'\n", 1);
	t("cp of the same file", L"cp", L"f1.txt f1.txt", NULL, "cp: 'f1.txt' and 'f1.txt' are the same file\n", 1);
	t("cp of a missing file", L"cp", L"nope.txt x.txt", NULL, "cp: nope.txt: No such file or directory\n", 1);
	t("cp without a destination", L"cp", L"f1.txt", NULL, "cp: missing destination file operand after 'f1.txt'\ncp: Try 'cp --help' for more information.\n", 2);
	t("cp a file with an accent and CJK", L"cp", L"\"ação 中.txt\" \"cópia 中.txt\"", NULL, "", 0);
	check_true("copy with an accent exists", exists(L"cópia 中.txt"));

	/* ---- mv ---- */
	t("mv renames", L"mv", L"copy.txt renamed.txt", NULL, "", 0);
	check_true("mv: the source is gone", !exists(L"copy.txt"));
	check_true("mv: the destination exists", exists(L"renamed.txt"));
	t("mv into a folder", L"mv", L"renamed.txt d1", NULL, "", 0);
	check_true("mv: it is in d1", exists(L"d1\\renamed.txt"));
	t("mv of several into a folder", L"mv", L"new.txt \"cópia 中.txt\" d1", NULL, "", 0);
	check_true("mv: several in d1", exists(L"d1\\new.txt") && exists(L"d1\\cópia 中.txt"));
	put(L"old.txt", "old");
	put(L"new2.txt", "new");
	t("mv replaces the destination", L"mv", L"new2.txt old.txt", NULL, "", 0);
	t("mv: the destination has the new content", L"cat", L"old.txt", NULL, "new", 0);
	t("mv renames a folder", L"mv", L"lsdir2 lsdir3", NULL, "", 0);
	check_true("mv of a folder", exists(L"lsdir3\\b.txt") && !exists(L"lsdir2"));
	t("mv of a missing file", L"mv", L"nope.txt x.txt", NULL, "mv: nope.txt: No such file or directory\n", 1);
	t("mv of a folder into itself", L"mv", L"lsdir3 lsdir3\\sub\\x", NULL,
		"mv: cannot move 'lsdir3' to a subdirectory of itself, 'lsdir3\\sub\\x'\n", 1);

	/* ---- rm ---- */
	t("rm deletes a file", L"rm", L"old.txt", NULL, "", 0);
	check_true("rm: old.txt is gone", !exists(L"old.txt"));
	t("rm of a missing file", L"rm", L"nope.txt", NULL, "rm: cannot remove 'nope.txt': No such file or directory\n", 1);
	t("rm -f of a missing file", L"rm", L"-f nope.txt", NULL, "", 0);
	t("rm of a folder without -r", L"rm", L"lsdir3", NULL, "rm: cannot remove 'lsdir3': Is a directory\n", 1);
	t("rm -r deletes the whole folder", L"rm", L"-r lsdir3", NULL, "", 0);
	check_true("rm -r: lsdir3 is gone", !exists(L"lsdir3"));
	t("rm -rf", L"rm", L"-rf lsdir d1 a \"x y\"", NULL, "", 0);
	check_true("rm -rf: the folders are gone", !exists(L"lsdir") && !exists(L"d1") && !exists(L"a") && !exists(L"x y"));
	t("rm refuses .", L"rm", L"-r .", NULL, "rm: refusing to remove '.' or '..': .\n", 1);
	t("rm refuses ..", L"rm", L"-r ..", NULL, "rm: refusing to remove '.' or '..': ..\n", 1);
	t("rm refuses the root", L"rm", L"-rf C:\\", NULL, "rm: refusing to remove the root: C:\\\n", 1);
	{
		/* read-only file: only -f deletes it */
		put(L"ro.txt", "x");
		SetFileAttributesW(L"ro.txt", FILE_ATTRIBUTE_READONLY);
		t("rm does not delete a read-only file without -f", L"rm", L"ro.txt", NULL, "rm: cannot remove 'ro.txt': Permission denied\n", 1);
		t("rm -f deletes a read-only file", L"rm", L"-f ro.txt", NULL, "", 0);
		check_true("rm -f: ro.txt is gone", !exists(L"ro.txt"));
	}

	/* ---- head, tail ---- */
	{
		char many[512] = "";
		for (int i = 1; i <= 15; i++)
			sprintf(many + strlen(many), "l%d\n", i);
		put(L"many.txt", many);
		t("head: 10 lines by default", L"head", L"many.txt", NULL, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n", 0);
		t("head -n 2", L"head", L"-n 2 many.txt", NULL, "l1\nl2\n", 0);
		t("head -n2", L"head", L"-n2 many.txt", NULL, "l1\nl2\n", 0);
		t("head -3", L"head", L"-3 many.txt", NULL, "l1\nl2\nl3\n", 0);
		t("head -c 5", L"head", L"-c 5 many.txt", NULL, "l1\nl2", 0);
		t("head -n 0", L"head", L"-n 0 many.txt", NULL, "", 0);
		t("head of standard input", L"head", L"-n 1", "a\nb\nc\n", "a\n", 0);
		t("head of several files", L"head", L"-n 1 f1.txt f2.txt", NULL, "==> f1.txt <==\none\n\n==> f2.txt <==\nthree no newline", 0);
		t("head of a missing file", L"head", L"nope", NULL, "head: nope: No such file or directory\n", 1);
		t("tail: 10 lines by default", L"tail", L"many.txt", NULL, "l6\nl7\nl8\nl9\nl10\nl11\nl12\nl13\nl14\nl15\n", 0);
		t("tail -n 2", L"tail", L"-n 2 many.txt", NULL, "l14\nl15\n", 0);
		t("tail -2", L"tail", L"-2 many.txt", NULL, "l14\nl15\n", 0);
		t("tail -n +14", L"tail", L"-n +14 many.txt", NULL, "l14\nl15\n", 0);
		t("tail -n +1 is everything", L"tail", L"-n +15 many.txt", NULL, "l15\n", 0);
		t("tail -n 0", L"tail", L"-n 0 many.txt", NULL, "", 0);
		t("tail without a final newline", L"tail", L"-n 1 f2.txt", NULL, "three no newline", 0);
		t("tail of standard input", L"tail", L"-n 2", "a\nb\nc\n", "b\nc\n", 0);
		t("tail of more lines than the file has", L"tail", L"-n 50 f1.txt", NULL, "one\ntwo\n", 0);
		t("tail of several files", L"tail", L"-n 1 f1.txt f2.txt", NULL, "==> f1.txt <==\ntwo\n\n==> f2.txt <==\nthree no newline", 0);
		t("head | tail: -f of several is an error", L"tail", L"-f f1.txt f2.txt", NULL, "tail: -f needs exactly one file\n", 2);
	}

	/* ---- wc ---- */
	put(L"wc.txt", "one two\nthree four five\nação 中\n");
	t("wc default", L"wc", L"wc.txt", NULL, "       3       7      35 wc.txt\n", 0);
	t("wc -l", L"wc", L"-l wc.txt", NULL, "3 wc.txt\n", 0);
	t("wc -w", L"wc", L"-w wc.txt", NULL, "7 wc.txt\n", 0);
	t("wc -c", L"wc", L"-c wc.txt", NULL, "35 wc.txt\n", 0);
	t("wc -m counts characters", L"wc", L"-m wc.txt", NULL, "31 wc.txt\n", 0);
	t("wc -lw", L"wc", L"-lw wc.txt", NULL, "       3       7 wc.txt\n", 0);
	t("wc of standard input", L"wc", L"-l", "a\nb\n", "2\n", 0);
	t("wc of two files has a total", L"wc", L"-l f1.txt wc.txt", NULL, "       2 f1.txt\n       3 wc.txt\n       5 total\n", 0);
	t("wc of a missing file", L"wc", L"nope", NULL, "wc: nope: No such file or directory\n", 1);

	/* ---- sort ---- */
	t("sort by bytes", L"sort", L"", "banana\napricot\nMango\nguava\n", "Mango\napricot\nbanana\nguava\n", 0);
	t("sort -f ignores case", L"sort", L"-f", "banana\napricot\nMango\nguava\n", "apricot\nbanana\nguava\nMango\n", 0);
	t("sort -r", L"sort", L"-r", "b\na\nc\n", "c\nb\na\n", 0);
	t("sort -n", L"sort", L"-n", "10\n9\n100\n-5\n2.5\n", "-5\n2.5\n9\n10\n100\n", 0);
	t("sort without -n sorts as text", L"sort", L"", "10\n9\n100\n", "10\n100\n9\n", 0);
	t("sort -u", L"sort", L"-u", "b\na\nb\na\nc\n", "a\nb\nc\n", 0);
	t("sort -nu", L"sort", L"-nu", "1\n01\n2\n", "1\n2\n", 0);
	t("sort -nr", L"sort", L"-nr", "1\n3\n2\n", "3\n2\n1\n", 0);
	t("sort without a final newline", L"sort", L"", "b\na", "a\nb\n", 0);
	t("sort of files", L"sort", L"f1.txt wc.txt", NULL, "ação 中\none\none two\nthree four five\ntwo\n", 0);
	t("sort is stable with -n", L"sort", L"-n", "1 b\n1 a\n", "1 a\n1 b\n", 0);
	t("sort of a missing file", L"sort", L"nope", NULL, "sort: nope: No such file or directory\n", 1);

	/* ---- grep ---- */
	{
		const char *fruits = "banana\napricot\nMango\nção 中 x\nguava\n10\n9\n100\n";
		put(L"fruits.txt", fruits);
		t("grep: plain text", L"grep", L"guava fruits.txt", NULL, "guava\n", 0);
		t("grep with no match exits with 1", L"grep", L"kiwi fruits.txt", NULL, "", 1);
		t("grep -n", L"grep", L"-n apr fruits.txt", NULL, "2:apricot\n", 0);
		t("grep -i", L"grep", L"-i mango fruits.txt", NULL, "Mango\n", 0);
		t("grep -v", L"grep", L"-v a fruits.txt", NULL, "ção 中 x\n10\n9\n100\n", 0);
		t("grep -c", L"grep", L"-c a fruits.txt", NULL, "4\n", 0);
		t("grep ^", L"grep", L"^ap fruits.txt", NULL, "apricot\n", 0);
		t("grep $", L"grep", L"a$ fruits.txt", NULL, "banana\nguava\n", 0);
		t("grep .", L"grep", L"^ap.icot$ fruits.txt", NULL, "apricot\n", 0);
		t("grep .* ", L"grep", L"\"^b.*a$\" fruits.txt", NULL, "banana\n", 0);
		t("grep +", L"grep", L"\"^[0-9]+$\" fruits.txt", NULL, "10\n9\n100\n", 0);
		t("grep ?", L"grep", L"\"^100?$\" fruits.txt", NULL, "10\n100\n", 0);
		t("grep [a-c]", L"grep", L"\"^[a-c]\" fruits.txt", NULL, "banana\napricot\n", 0);
		t("grep [^a-z]", L"grep", L"\"^[^a-z]\" fruits.txt", NULL, "Mango\nção 中 x\n10\n9\n100\n", 0);
		t("grep [[:digit:]]", L"grep", L"\"^[[:digit:]]$\" fruits.txt", NULL, "9\n", 0);
		t("grep \\d", L"grep", L"\"^\\d\\d$\" fruits.txt", NULL, "10\n", 0);
		t("grep a|b", L"grep", L"\"^guava$|^9$\" fruits.txt", NULL, "guava\n9\n", 0);
		t("grep UTF-8: . matches a whole character", L"grep", L"\"^ç.o 中 x$\" fruits.txt", NULL, "ção 中 x\n", 0);
		t("grep UTF-8 literal", L"grep", L"中 fruits.txt", NULL, "ção 中 x\n", 0);
		t("grep -F does not interpret", L"grep", L"-F \"a.a\" fruits.txt", NULL, "", 1);
		t("grep without -F interprets", L"grep", L"\"a.a\" fruits.txt", NULL, "banana\nguava\n", 0);
		t("grep \\. is the dot", L"grep", L"\"^a\\.a\" fruits.txt", NULL, "", 1);
		t("grep -e several patterns", L"grep", L"-e guava -e 9 fruits.txt", NULL, "guava\n9\n", 0);
		t("grep -l", L"grep", L"-l guava fruits.txt f1.txt", NULL, "fruits.txt\n", 0);
		t("grep -q", L"grep", L"-q guava fruits.txt", NULL, "", 0);
		t("grep -q with no match", L"grep", L"-q kiwi fruits.txt", NULL, "", 1);
		t("grep of standard input", L"grep", L"b", "a\nb\nc\n", "b\n", 0);
		t("grep of several files shows the names", L"grep", L"one f1.txt fruits.txt", NULL, "f1.txt:one\n", 0);
		t("grep -h without names", L"grep", L"-h one f1.txt fruits.txt", NULL, "one\n", 0);
		t("grep -H with name", L"grep", L"-H guava fruits.txt", NULL, "fruits.txt:guava\n", 0);
		t("grep -c of several", L"grep", L"-c guava f1.txt fruits.txt", NULL, "f1.txt:0\nfruits.txt:1\n", 0);
		t("grep of a missing file exits with 2", L"grep", L"x nope.txt", NULL, "grep: nope.txt: No such file or directory\n", 2);
		t("grep without a pattern", L"grep", L"", NULL, "grep: missing pattern\ngrep: Try 'grep --help' for more information.\n", 2);
		t("grep: CRLF e $", L"grep", L"\"b$\"", "a\r\nb\r\nc\r\n", "b\r\n", 0);
		{
			HANDLE h = CreateFileW(L"bin.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			DWORD w;
			WriteFile(h, "abc\0xyz\n", 8, &w, NULL);
			CloseHandle(h);
		}
		t("grep on a binary file warns", L"grep", L"xyz bin.dat", NULL, "Binary file bin.dat matches\n", 0);
		CreateDirectoryW(L"g", NULL);
		t("grep of a folder without -r", L"grep", L"x g", NULL, "grep: g: Is a directory\n", 2);
		CreateDirectoryW(L"g\\sub", NULL);
		put(L"g\\a.txt", "guava\napple\n");
		put(L"g\\sub\\b.txt", "guava green\n");
		t("grep -r in the folders", L"grep", L"-r guava g", NULL, "g\\a.txt:guava\ng\\sub\\b.txt:guava green\n", 0);
	}


	/* ---- pwd, sleep, tee, uniq ---- */
	{
		wchar_t cwd[MAX_PATH];
		char cwd8[MAX_PATH * 3], want[MAX_PATH * 3 + 2];

		GetCurrentDirectoryW(MAX_PATH, cwd);
		WideCharToMultiByte(CP_UTF8, 0, cwd, -1, cwd8, sizeof cwd8, NULL, NULL);
		sprintf(want, "%s\n", cwd8);
		t("pwd shows the current folder", L"pwd", L"", NULL, want, 0);
		t("pwd -P", L"pwd", L"-P", NULL, want, 0);
		t("pwd with an extra argument", L"pwd", L"x", NULL, "pwd: too many arguments\n", 1);
	}
	t("sleep with a fraction", L"sleep", L"0.05", NULL, "", 0);
	t("sleep with a suffix and a sum", L"sleep", L"0.02s 0.01", NULL, "", 0);
	t("sleep without an argument", L"sleep", L"", NULL, "sleep: missing operand\nsleep: Try 'sleep --help' for more information.\n", 1);
	t("sleep invalid", L"sleep", L"x", NULL, "sleep: invalid time interval 'x'\n", 1);
	t("sleep with an invalid suffix", L"sleep", L"1x", NULL, "sleep: invalid time interval '1x'\n", 1);
	t("sleep negative", L"sleep", L"-1", NULL, "sleep: invalid time interval '-1'\n", 1);

	t("show without an argument", L"show", L"", NULL, "show: missing operand\nshow: Try 'show --help' for more information.\n", 1);
	t("show invalid pid", L"show", L"abc", NULL, "show: invalid pid 'abc'\n", 1);
	t("show pid that does not exist", L"show", L"999999", NULL, "show: 999999: no such process\n", 1);

	t("tee copies to the output and files", L"tee", L"tee1.txt tee2.txt", "abc\ndef\n", "abc\ndef\n", 0);
	t("tee: file 1", L"cat", L"tee1.txt", NULL, "abc\ndef\n", 0);
	t("tee: file 2", L"cat", L"tee2.txt", NULL, "abc\ndef\n", 0);
	t("tee -a appends", L"tee", L"-a tee1.txt", "ghi\n", "ghi\n", 0);
	t("tee -a: file", L"cat", L"tee1.txt", NULL, "abc\ndef\nghi\n", 0);
	t("tee without -a replaces", L"tee", L"tee1.txt", "new\n", "new\n", 0);
	t("tee: replaced", L"cat", L"tee1.txt", NULL, "new\n", 0);
	t("tee only to the output", L"tee", L"", "só\n", "só\n", 0);
	t("tee to an impossible file", L"tee", L"nope_dir\\x.txt", "a\n", "tee: nope_dir\\x.txt: No such file or directory\na\n", 1);

	t("uniq merges consecutive repeats", L"uniq", L"", "a\na\nb\na\n", "a\nb\na\n", 0);
	t("uniq -c", L"uniq", L"-c", "a\na\nb\n", "      2 a\n      1 b\n", 0);
	t("uniq -d", L"uniq", L"-d", "a\na\nb\nc\nc\n", "a\nc\n", 0);
	t("uniq -u", L"uniq", L"-u", "a\na\nb\nc\nc\n", "b\n", 0);
	t("uniq -i", L"uniq", L"-i", "a\nA\nb\n", "a\nb\n", 0);
	t("uniq without -i tells them apart", L"uniq", L"", "a\nA\n", "a\nA\n", 0);
	t("uniq of a file", L"uniq", L"f1.txt", NULL, "one\ntwo\n", 0);
	t("uniq: sort | uniq -c", L"uniq", L"-c f1.txt", NULL, "      1 one\n      1 two\n", 0);
	t("uniq of a missing file", L"uniq", L"nope.txt", NULL, "uniq: nope.txt: No such file or directory\n", 1);

	/* ---- du, find ---- */
	CreateDirectoryW(L"ft", NULL);
	CreateDirectoryW(L"ft\\a", NULL);
	CreateDirectoryW(L"ft\\a\\b", NULL);
	CreateDirectoryW(L"ft\\c", NULL);
	put(L"ft\\a\\f.c", "x");
	put(L"ft\\e.txt", "");
	{
		HANDLE h = CreateFileW(L"ft\\a\\b\\big.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		char zeros[3000] = { 0 };
		DWORD w;
		WriteFile(h, zeros, sizeof zeros, &w, NULL);
		CloseHandle(h);
	}
	t("du per folder", L"du", L"ft", NULL, "3\tft\\a\\b\n3\tft\\a\n0\tft\\c\n3\tft\n", 0);
	t("du -s", L"du", L"-s ft", NULL, "3\tft\n", 0);
	t("du -d 1", L"du", L"-d 1 ft", NULL, "3\tft\\a\n0\tft\\c\n3\tft\n", 0);
	t("du -a", L"du", L"-a ft\\a", NULL, "3\tft\\a\\b\\big.bin\n3\tft\\a\\b\n1\tft\\a\\f.c\n3\tft\\a\n", 0);
	t("du -h", L"du", L"-h ft\\a\\b\\big.bin", NULL, "3K\tft\\a\\b\\big.bin\n", 0);
	t("du -c with two paths", L"du", L"-sc ft\\a ft\\c", NULL, "3\tft\\a\n0\tft\\c\n3\ttotal\n", 0);
	t("du of a file", L"du", L"ft\\a\\f.c", NULL, "1\tft\\a\\f.c\n", 0);
	t("du of a missing file", L"du", L"nope", NULL, "du: cannot access 'nope': No such file or directory\n", 1);
	t("du -d invalid", L"du", L"-d x ft", NULL, "du: invalid maximum depth 'x'\n", 1);

	t("find lists everything", L"find", L"ft", NULL, "ft\nft\\a\nft\\a\\b\nft\\a\\b\\big.bin\nft\\a\\f.c\nft\\c\nft\\e.txt\n", 0);
	t("find -type d", L"find", L"ft -type d", NULL, "ft\nft\\a\nft\\a\\b\nft\\c\n", 0);
	t("find -type f", L"find", L"ft -type f", NULL, "ft\\a\\b\\big.bin\nft\\a\\f.c\nft\\e.txt\n", 0);
	t("find -name", L"find", L"ft -name \"*.c\"", NULL, "ft\\a\\f.c\n", 0);
	t("find -name with ?", L"find", L"ft -name \"?.c\"", NULL, "ft\\a\\f.c\n", 0);
	t("find -iname", L"find", L"ft -iname \"F.C\"", NULL, "ft\\a\\f.c\n", 0);
	t("find -name with [ ]", L"find", L"ft -name \"[ab]\"", NULL, "ft\\a\nft\\a\\b\n", 0);
	t("find -name with [! ]", L"find", L"ft -type d -name \"[!ab]*\"", NULL, "ft\nft\\c\n", 0);
	t("find -empty", L"find", L"ft -empty", NULL, "ft\\c\nft\\e.txt\n", 0);
	t("find -size +2k", L"find", L"ft -size +2k", NULL, "ft\\a\\b\\big.bin\n", 0);
	t("find -size -1c", L"find", L"ft -type f -size -1c", NULL, "ft\\e.txt\n", 0);
	t("find -size 1c", L"find", L"ft -size 1c", NULL, "ft\\a\\f.c\n", 0);
	t("find -maxdepth", L"find", L"ft -maxdepth 1", NULL, "ft\nft\\a\nft\\c\nft\\e.txt\n", 0);
	t("find -mindepth", L"find", L"ft -mindepth 2", NULL, "ft\\a\\b\nft\\a\\b\\big.bin\nft\\a\\f.c\n", 0);
	t("find ! negates", L"find", L"ft ! -type d", NULL, "ft\\a\\b\\big.bin\nft\\a\\f.c\nft\\e.txt\n", 0);
	t("find -not -name", L"find", L"ft -type f -not -name \"*.c\"", NULL, "ft\\a\\b\\big.bin\nft\\e.txt\n", 0);
	t("find with several paths", L"find", L"ft\\c ft\\e.txt", NULL, "ft\\c\nft\\e.txt\n", 0);
	t("find of a missing file", L"find", L"nope", NULL, "find: 'nope': No such file or directory\n", 1);
	t("find with an unknown test", L"find", L"ft -bogus", NULL, "find: unknown predicate '-bogus'\n", 1);
	t("find -type invalid", L"find", L"ft -type z", NULL, "find: unknown argument to -type: z\n", 1);
	t("find without the test's argument", L"find", L"ft -name", NULL, "find: missing argument to '-name'\n", 1);
	CreateDirectoryW(L"ft2", NULL);
	CreateDirectoryW(L"ft2\\d", NULL);
	put(L"ft2\\d\\f.txt", "x");
	put(L"ft2\\g.txt", "y");
	t("find -delete deletes what matches", L"find", L"ft2 -name \"*.txt\" -delete", NULL, "", 0);
	check_true("find -delete: the files are gone, the folder stayed", !exists(L"ft2\\g.txt") && !exists(L"ft2\\d\\f.txt") && exists(L"ft2\\d"));
	t("find -delete of everything (the inner ones first)", L"find", L"ft2 -delete", NULL, "", 0);
	check_true("find -delete: everything is gone", !exists(L"ft2"));

	/* ---- kill ---- */
	t("kill -l", L"kill", L"-l", NULL, "HUP INT QUIT ABRT KILL USR1 USR2 PIPE ALRM TERM\n", 0);
	t("kill -l 9", L"kill", L"-l 9", NULL, "KILL\n", 0);
	t("kill without a pid", L"kill", L"", NULL, "kill: usage: kill [-s SIG | -SIG | -0] pid ...\n", 2);
	t("kill of text", L"kill", L"abc", NULL, "kill: abc: arguments must be process IDs\n", 1);
	t("kill of a pid that does not exist", L"kill", L"99999999", NULL, "kill: (99999999) - No such process\n", 1);
	t("kill invalid signal", L"kill", L"-FOO 1", NULL, "kill: FOO: invalid signal specification\n", 2);
	{
		static const struct { const wchar_t *args; DWORD code; } kills[] = {
			{ L"", 143 }, { L"-9", 137 }, { L"-s KILL", 137 }, { L"-TERM", 143 }, { L"-INT", 130 }, { L"-SIGHUP", 129 }
		};
		for (int k = 0; k < (int) (sizeof kills / sizeof *kills); k++) {
			STARTUPINFOW si;
			PROCESS_INFORMATION pi;
			wchar_t cmd[MAX_PATH * 2], args[128];
			char label[96];
			DWORD code = 0;

			memset(&si, 0, sizeof si);
			si.cb = sizeof si;
			swprintf(cmd, MAX_PATH * 2, L"\"%ls\\sleep.exe\" 30", bin_dir);
			if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				failures++;
				total++;
				printf("FAIL  kill: could not open sleep\n");
				continue;
			}
			swprintf(args, 128, L"-0 %lu", pi.dwProcessId);
			sprintf(label, "kill -0 of a live process (%d)", k);
			t(label, L"kill", args, NULL, "", 0);
			swprintf(args, 128, L"%ls %lu", kills[k].args, pi.dwProcessId);
			sprintf(label, "kill %ls kills the process", kills[k].args);
			t(label, L"kill", args, NULL, "", 0);
			total++;
			if (WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
				failures++;
				printf("FAIL  %s: the process is still alive\n", label);
				TerminateProcess(pi.hProcess, 1);
			} else {
				GetExitCodeProcess(pi.hProcess, &code);
				if (code != kills[k].code) {
					failures++;
					printf("FAIL  %s: code %lu, expected %lu\n", label, code, kills[k].code);
				}
			}
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			swprintf(args, 128, L"-0 %lu", pi.dwProcessId);
			{
				char want[64];
				sprintf(want, "kill: (%lu) - No such process\n", pi.dwProcessId);
				t("kill -0 of a process that ended", L"kill", args, NULL, want, 1);
			}
		}
	}


	/* ---- ps ---- */
	{
		static char got[1 << 20];
		DWORD me = GetCurrentProcessId();
		wchar_t args[128];
		char pid[32];
		const char *header = "   PID   PPID  THR  MEM(KiB)  NAME\n";
		int code;

		sprintf(pid, "%6lu ", me);
		swprintf(args, 128, L"-p %lu", me);
		code = run(L"ps", args, NULL, got, sizeof got);
		check_true("ps -p shows the header and the process", code == 0 && !strncmp(got, header, strlen(header)) &&
			strstr(got, pid) && strstr(got, "bin_test.exe"));
		swprintf(args, 128, L"-h -p %lu", me);
		code = run(L"ps", args, NULL, got, sizeof got);
		check_true("ps -h has no header", code == 0 && strncmp(got, "   PID", 6) && strstr(got, "bin_test.exe"));
		swprintf(args, 128, L"-f -p %lu", me);
		code = run(L"ps", args, NULL, got, sizeof got);
		check_true("ps -f shows the program path", code == 0 && strstr(got, "\\bin_test.exe") && strstr(got, ":\\"));
		code = run(L"ps", L"-C bin_test", NULL, got, sizeof got);
		check_true("ps -C finds by name without .exe", code == 0 && strstr(got, pid));
		code = run(L"ps", L"-C BIN_TEST.EXE", NULL, got, sizeof got);
		check_true("ps -C ignores case and accepts .exe", code == 0 && strstr(got, pid));
		code = run(L"ps", L"-p 99999999", NULL, got, sizeof got);
		check_true("ps of a pid that does not exist: only the header and code 1", code == 1 && !strcmp(got, header));
		code = run(L"ps", L"-C nome_que_nao_existe_zzz", NULL, got, sizeof got);
		check_true("ps -C of a program that does not exist exits with 1", code == 1);
		code = run(L"ps", L"-e", NULL, got, sizeof got);
		check_true("ps -e lists everything and includes this process", code == 0 && strstr(got, pid));
		code = run(L"ps", L"-m", NULL, got, sizeof got);
		check_true("ps -m runs", code == 0 && !strncmp(got, header, strlen(header)));
		code = run(L"ps", L"-p abc", NULL, got, sizeof got);
		check_true("ps -p invalid", code == 1 && strstr(got, "invalid process id: 'abc'"));
		code = run(L"ps", L"extra", NULL, got, sizeof got);
		check_true("ps with an extra argument", code == 2 && strstr(got, "unexpected argument 'extra'"));
	}


	/* ---- masterkill ---- */
	{
		static char got[1 << 16];
		wchar_t src[MAX_PATH], copy[MAX_PATH], name[64], args[256];
		char name8[64], pid_a[32], pid_b[32];
		PROCESS_INFORMATION a, b, c;
		DWORD me = GetCurrentProcessId();
		int code;

		swprintf(name, 64, L"ant_sleeper_%lu", me);
		sprintf(name8, "ant_sleeper_%lu", me);
		swprintf(src, MAX_PATH, L"%ls\\sleep.exe", bin_dir);
		swprintf(copy, MAX_PATH, L"%ls\\%ls.exe", base, name);
		CopyFileW(src, copy, FALSE);

		if (spawn_sleeper(copy, &a) && spawn_sleeper(copy, &b)) {
			sprintf(pid_a, "(%lu)", a.dwProcessId);
			sprintf(pid_b, "(%lu)", b.dwProcessId);
			code = run(L"masterkill", name, NULL, got, sizeof got);
			check_true("masterkill declares each kill with name and pid", code == 0 &&
				strstr(got, "KILL") && strstr(got, name8) && strstr(got, pid_a) && strstr(got, pid_b));
			check_true("masterkill ended the 1st (143)", ended_with(&a, 143));
			check_true("masterkill ended the 2nd (143)", ended_with(&b, 143));
		} else
			check_true("masterkill: could not open the test processes", false);

		if (spawn_sleeper(copy, &c)) {
			code = run(L"masterkill", L"ant_sleep", NULL, got, sizeof got);
			check_true("masterkill matches by prefix (runtime -> RuntimeBroker)", code == 0 && strstr(got, "KILL") && strstr(got, name8));
			check_true("masterkill prefix ends with 143", ended_with(&c, 143));
		}
		if (spawn_sleeper(copy, &c)) {
			swprintf(args, 256, L"\"%ls.EXE\"", name);
			code = run(L"masterkill", args, NULL, got, sizeof got);
			check_true("masterkill ignores case and accepts .exe", code == 0 && strstr(got, name8));
			check_true("masterkill .EXE ends with 143", ended_with(&c, 143));
		}
		code = run(L"masterkill", L"nome_que_nao_existe_zzz", NULL, got, sizeof got);
		check_true("masterkill declares UNKNOWN and exits 1", code == 1 && strstr(got, "UNKNOWN"));
		code = run(L"masterkill", L"bin_test", NULL, got, sizeof got);
		check_true("masterkill spares whoever started it (UNKNOWN, code 1)", code == 1 && strstr(got, "UNKNOWN"));
		code = run(L"masterkill", L"", NULL, got, sizeof got);
		check_true("masterkill with no name lists the processes", code == 0 && strstr(got, "bin_test"));
		code = run(L"masterkill", L"--help", NULL, got, sizeof got);
		check_true("masterkill --help shows the usage", code == 0 && strstr(got, "usage: masterkill"));
		DeleteFileW(copy);
	}

	/* ---- xargs ---- */
	{
		char got[8192];
		int code;

		put(L"xa.txt", "A\n");
		put(L"x b.txt", "B\n");
		t("xargs without a command writes the words (echo)", L"xargs", L"", "a b  c\n", "a b c\n", 0);
		t("xargs with empty input writes an empty line", L"xargs", L"", "", "\n", 0);
		t("xargs -r with empty input does not run", L"xargs", L"-r cat", "", "", 0);
		t("xargs -n1 one word at a time, with quotes", L"xargs", L"-n1", "a \"b c\" 'd e'\n", "a\nb c\nd e\n", 0);
		t("xargs -n2 groups two by two", L"xargs", L"-n2", "1 2 3 4 5", "1 2\n3 4\n5\n", 0);
		t("xargs runs the command with the words (quotes, space)", L"xargs", L"cat", "xa.txt \"x b.txt\"\n", "A\nB\n", 0);
		t("xargs: the command arguments come before the words", L"xargs", L"cat -n", "xa.txt\n", "     1\tA\n", 0);
		t("xargs -I replaces the string on each line", L"xargs", L"-I{} cat {}", "xa.txt\nx b.txt\n", "A\nB\n", 0);
		t("xargs -I skips blank lines", L"xargs", L"-I{} cat {}", "\n  xa.txt  \n\n", "A\n", 0);
		t("xargs -t shows the command line", L"xargs", L"-t cat", "xa.txt\n", "cat xa.txt\nA\n", 0);
		t("xargs -t quotes whatever has a space", L"xargs", L"-t -n1 cat", "\"x b.txt\"\n", "cat \"x b.txt\"\nB\n", 0);
		t("xargs: a failing command gives 123", L"xargs", L"cat nao_existe_zzz.txt", "xa.txt\n", "cat: nao_existe_zzz.txt: No such file or directory\nA\n", 123);
		t("xargs: a missing command gives 127", L"xargs", L"programa_que_nao_existe_zzz", "a\n", "xargs: programa_que_nao_existe_zzz: No such file or directory\n", 127);
		t("xargs: unclosed quote", L"xargs", L"", "it's\n", "xargs: unmatched single quote; by default quotes are special to xargs unless you use the -0 option\n", 1);
		t("xargs -n 0 is invalid", L"xargs", L"-n 0", "a\n", "xargs: invalid number for -n: '0'\n", 2);
		code = run(L"xargs", L"-P2 -n1 cat", "xa.txt \"x b.txt\"\n", got, sizeof got);
		check_true("xargs -P2 runs both commands", code == 0 && strstr(got, "A\n") && strstr(got, "B\n"));
		code = run(L"xargs", L"--help", NULL, got, sizeof got);
		check_true("xargs --help", code == 0 && strstr(got, "usage: xargs"));
		/* the input goes to the command as NUL: it does not steal the rest of the pipe */
		t("xargs: the command does not read xargs's input", L"xargs", L"-n1 cat", "xa.txt\n", "A\n", 0);
	}

	/* limpeza */
	{
		wchar_t cmd[MAX_PATH + 16];
		char junk[4096];
		SetCurrentDirectoryW(old);
		swprintf(cmd, MAX_PATH + 16, L"-rf \"%ls\"", base);
		run(L"rm", cmd, NULL, junk, sizeof junk);
	}

	printf("bin: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
