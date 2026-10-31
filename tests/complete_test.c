/*************************************************************
 * Test of the complete module (src/editor/complete.c).         *
 *                                                           *
 * usage: complete_test.exe                                    *
 *                                                           *
 * Builds a tree in a temporary folder, enters it and        *
 * checks the options and the text TAB would put on the line.     *
 *************************************************************/

#include <time.h>
#include <stdio.h>
#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

#include "../src/editor/complete.h"
#include "../src/builtins/dirs.h"

static int failures = 0, total = 0;
static wchar_t root[MAX_PATH];

static const wchar_t *dirs[] = {
	L"src", L"src\\include", L"src\\modules", L"data", L"data2",
	L"Program Files", L"Program Files\\Common Files", L".git", NULL
};
static const wchar_t *files[] = {
	L"Makefile", L"docs.txt", L"foo2.txt", L"foo10.txt", L"src\\main.c",
	L".hidden", L"hid.txt", L"a\U0001F60Ab.txt", L"data2\\pay$day.txt", L"data2\\it's $5.txt", NULL
};

static void u8 (const wchar_t *w, char *out, int max) {

	WideCharToMultiByte(CP_UTF8, 0, w, -1, out, max, NULL, NULL);
}

static void report (bool ok, const char *name, const wchar_t *want, const wchar_t *got) {

	char a[2048], b[2048];

	total++;
	if (ok)
		return;
	failures++;
	u8(want, a, sizeof a);
	u8(got, b, sizeof b);
	printf("FAIL  %s\n      expected: [%s]\n      got:      [%s]\n", name, a, b);
}

/* options separated by '|' */
static void list_of (const ANT_COMPLETE *c, wchar_t *out) {

	out[0] = 0;
	for (int i = 0; i < c->count; i++) {
		if (i)
			wcscat(out, L"|");
		wcscat(out, c->items[i].name);
	}
}

static void check_list (const char *name, const wchar_t *line, const wchar_t *want) {

	ANT_COMPLETE c;
	wchar_t got[4096];

	ant_complete(line, wcslen(line), &c);
	list_of(&c, got);
	report(!wcscmp(got, want), name, want, got);
	ant_complete_free(&c);
}

/* line as it would end up: the single option applied, or the common prefix if there are several */
static void check_apply (const char *name, const wchar_t *line, const wchar_t *want) {

	ANT_COMPLETE c;
	wchar_t text[2048], got[4096];
	int n = ant_complete(line, wcslen(line), &c);

	wcsncpy(got, line, c.beg);
	got[c.beg] = 0;
	if (n == 1)
		ant_complete_text(&c, 0, ANT_APPLY_UNIQUE, text, 2048);
	else if (n > 1 && c.prefix > (int) wcslen(c.word))
		ant_complete_text(&c, -1, ANT_APPLY_PREFIX, text, 2048);
	else
		wcscpy(text, line + c.beg);
	wcscat(got, text);
	report(!wcscmp(got, want), name, want, got);
	ant_complete_free(&c);
}

static void build_tree (void) {

	wchar_t path[MAX_PATH];

	GetTempPathW(MAX_PATH, root);
	wcscat(root, L"ant_complete_test");
	CreateDirectoryW(root, NULL);
	for (int i = 0; dirs[i]; i++) {
		swprintf(path, MAX_PATH, L"%ls\\%ls", root, dirs[i]);
		CreateDirectoryW(path, NULL);
	}
	for (int i = 0; files[i]; i++) {
		swprintf(path, MAX_PATH, L"%ls\\%ls", root, files[i]);
		CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
	}
	swprintf(path, MAX_PATH, L"%ls\\hid.txt", root);
	SetFileAttributesW(path, FILE_ATTRIBUTE_HIDDEN);
	SetCurrentDirectoryW(root);
}

static void remove_tree (void) {

	wchar_t path[MAX_PATH];
	int n = 0;

	SetCurrentDirectoryW(L"..");
	for (int i = 0; files[i]; i++) {
		swprintf(path, MAX_PATH, L"%ls\\%ls", root, files[i]);
		SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(path);
	}
	while (dirs[n])
		n++;
	for (int i = n - 1; i >= 0; i--) {
		swprintf(path, MAX_PATH, L"%ls\\%ls", root, dirs[i]);
		RemoveDirectoryW(path);
	}
	RemoveDirectoryW(root);
}

int main (void) {

	build_tree();

	/* lists (natural order, case-insensitive; directories with \; hidden ones left out) */
	check_list("empty word lists the folder", L"",
		L"a\U0001F60Ab.txt|data\\|data2\\|docs.txt|foo2.txt|foo10.txt|Makefile|Program Files\\|src\\");
	check_list("after 'cd ' too", L"cd ",
		L"a\U0001F60Ab.txt|data\\|data2\\|docs.txt|foo2.txt|foo10.txt|Makefile|Program Files\\|src\\");
	check_list("src\\ lists inside src", L"cd src\\", L"include\\|main.c|modules\\");
	check_list("src/ keeps the / slash", L"cd src/", L"include/|main.c|modules/");
	check_list("src\\m", L"cd src\\m", L"main.c|modules\\");
	check_list("natural order: foo2 before foo10", L"foo", L"foo2.txt|foo10.txt");
	check_list("'.' shows hidden ones with a dot and ..", L".", L"..\\|.git\\|.hidden");
	check_list("hidden by attribute shows up with a word", L"hid", L"hid.txt");
	check_list("nothing matches", L"zzz", L"");
	check_list("a wildcard does not become a pattern", L"*.txt", L"");

	/* categories: only the best one stays */
	check_list("lower case ignores case (smart case)", L"mak", L"Makefile");
	check_list("different capitals: prefix ignoring case", L"MAK", L"Makefile");
	check_list("substring when there is no prefix (smart case: file matches Files)", L"file", L"Makefile|Program Files\\");
	check_list("substring with a capital: exact case wins", L"File", L"Program Files\\");
	check_list("subsequence when there is no substring", L"mkf", L"Makefile");
	check_list("prefix beats substring", L"d", L"data\\|data2\\|docs.txt");

	/* applied text */
	check_apply("single folder: separator, no space", L"cd s", L"cd src\\");
	check_apply("chaining: src\\mo -> src\\modules\\", L"cd src\\mo", L"cd src\\modules\\");
	check_apply("single file: space at the end", L"type Mak", L"type Makefile ");
	check_apply("different case replaces the word", L"type MAK", L"type Makefile ");
	check_apply("several: inserts the common prefix", L"cd da", L"cd data");
	check_apply("common prefix shorter than the word: nothing changes", L"type d", L"type d");
	check_apply("a name with a space gets quotes", L"cd Prog", L"cd \"Program Files\\\"");
	check_apply("inside quotes it continues", L"cd \"Program Files\\Com", L"cd \"Program Files\\Common Files\\\"");
	check_apply("slash / stays /", L"cd src/in", L"cd src/include/");
	check_apply("emoji in the name", L"type a", L"type a\U0001F60Ab.txt ");
	check_apply("a name with $ goes in single quotes ($ expands inside double ones)", L"type data2\\pay", L"type 'data2\\pay$day.txt' ");
	check_apply("typed in double quotes, a name with $ still gets single ones", L"type \"data2\\pay", L"type 'data2\\pay$day.txt' ");
	check_apply("a name with $ and ' keeps double quotes (no escape for ')", L"type data2\\it", L"type \"data2\\it's $5.txt\" ");

	/* word in the middle of the line: completes up to the cursor */
	{
		ANT_COMPLETE c;
		const wchar_t *line = L"type Mak rest";
		ant_complete(line, 8, &c);
		report(c.beg == 5 && c.end == 8 && c.count == 1, "word in the middle of the line (beg 5, end 8)",
			L"5 8 1", c.count == 1 ? L"ok" : L"wrong");
		ant_complete_free(&c);
	}

	/* commands in the 1st word: built-ins + .exe from the current folder and from two "PATH" */
	{
		static const wchar_t *builtins[] = { L"cd", L"history", L"nop", L"path", L"ver", NULL };
		static const wchar_t *bin1_files[] = { L"git.exe", L"gitk.exe", L"make.exe", L"notes.txt", NULL };
		static const wchar_t *bin2_files[] = { L"git.exe", L"gcc.exe", L"Vim.EXE", NULL };
		wchar_t tmp[MAX_PATH], bin1[MAX_PATH], bin2[MAX_PATH], cmdpath[MAX_PATH * 2 + 16], path[MAX_PATH];

		GetTempPathW(MAX_PATH, tmp);
		swprintf(bin1, MAX_PATH, L"%lsant_complete_bin1", tmp);
		swprintf(bin2, MAX_PATH, L"%lsant_complete_bin2", tmp);
		CreateDirectoryW(bin1, NULL);
		CreateDirectoryW(bin2, NULL);
		for (int i = 0; bin1_files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin1, bin1_files[i]);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}
		for (int i = 0; bin2_files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin2, bin2_files[i]);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}
		CloseHandle(CreateFileW(L"myapp.exe", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));

		/* extra spaces, quotes and ';', as in a real PATH */
		swprintf(cmdpath, MAX_PATH * 2 + 16, L"%ls;  \"%ls\" ;", bin1, bin2);
		ant_complete_set_commands(builtins, NULL, cmdpath);

		check_list("1st word: .exe without extension and without repeats (git in two folders)", L"gi", L"git|gitk");
		check_list("built-in", L"ve", L"ver");
		check_list(".EXE extension in capitals", L"vi", L"Vim");
		check_list(".exe from the current folder", L"my", L"myapp");
		check_list("spaces before are still the 1st word", L"  gc", L"gcc");
		check_list(".txt in PATH is not a command", L"notes", L"");
		check_list("2nd word is still a file", L"type gcc", L"");
		check_list("with a path it is still a file", L"src\\m", L"main.c|modules\\");
		check_list("no command matches: falls back to files", L"Makef", L"Makefile");
		check_list("empty word still lists the folder", L"",
			L"a\U0001F60Ab.txt|data\\|data2\\|docs.txt|foo2.txt|foo10.txt|Makefile|myapp.exe|Program Files\\|src\\");
		check_apply("single command gets a space", L"gc", L"gcc ");
		check_apply("several commands: inserts the common prefix", L"gi", L"git");

		/* command position after | ; && || & (and a file after < and >) */
		check_list("after |", L"ver | gi", L"git|gitk");
		check_list("after | with no space", L"ver|gi", L"git|gitk");
		check_list("after &&", L"cd && gc", L"gcc");
		check_list("after ||", L"cd || gc", L"gcc");
		check_list("after ;", L"cd; ve", L"ver");
		check_list("after &", L"x & my", L"myapp");
		check_list("after > it is a file, not a command", L"type x > Makef", L"Makefile");
		check_list("after < it is a file", L"sort < Makef", L"Makefile");
		check_list("| inside quotes is not an operator", L"echo \"a | gc", L"");
		check_apply("completes after | and keeps the rest", L"ver | gc", L"ver | gcc ");
		check_apply("after | with no space", L"ver|gc", L"ver|gcc ");

		/* @prog */
		check_list("@ in front: completes the command", L"@gi", L"git|gitk");
		check_apply("@ comes back in the inserted text", L"@gc", L"@gcc ");
		check_apply("@ with a path completes a file", L"@src\\ma", L"@src\\main.c ");
		check_apply("@ after &&", L"cd && @gc", L"cd && @gcc ");

		/* order: built-ins, folders, .exe from the current folder, [path] from config.ant and, last, PATH */
		ant_complete_set_commands(builtins, bin1, bin2);
		check_list("[path] before PATH (git in bin1 comes before gcc in bin2)", L"g", L"git|gitk|gcc");
		check_list("the same name in [path] and PATH shows up once", L"gi", L"git|gitk");
		check_list("built-in, folder and then the rest", L"p", L"path|Program Files\\");
		check_apply("single command gets a space (ma: only make)", L"ma", L"make ");
		check_list("only a folder matches: the files come back", L"d", L"data\\|data2\\|docs.txt");

		ant_complete_set_commands(NULL, NULL, NULL);
		DeleteFileW(L"myapp.exe");
		for (int i = 0; bin1_files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin1, bin1_files[i]);
			DeleteFileW(path);
		}
		for (int i = 0; bin2_files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin2, bin2_files[i]);
			DeleteFileW(path);
		}
		RemoveDirectoryW(bin1);
		RemoveDirectoryW(bin2);
	}

	/* ---------- folders already visited (builtins/dirs.c): game\ means proj\game ---------- */
	{
		static const wchar_t *tree[] = {
			L"proj", L"proj\\game", L"proj\\game\\assets", L"proj\\game\\assets\\tree", L"proj\\game\\maps",
			L"proj\\src", L"proj\\src\\other", L"old", L"old\\game", L"old\\game\\assets", L"lone", L"lone\\only",
			L"lone\\only\\sub", L"here", L"here\\src", L"here\\src\\inner", NULL
		};
		wchar_t base[MAX_PATH], path[MAX_PATH], rank_file[MAX_PATH], text[8192], want[4096], best[ANT_DIRS_PATH];
		char bytes[8192];
		DWORD written;
		HANDLE f;
		time_t now = time(NULL);
		int n;

		GetTempPathW(MAX_PATH, base);
		wcscat(base, L"ant_complete_dirs");
		CreateDirectoryW(base, NULL);
		for (int i = 0; tree[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", base, tree[i]);
			CreateDirectoryW(path, NULL);
		}
		/* the current folder has only src: proj, old and lone are far from here */
		swprintf(path, MAX_PATH, L"%ls\\here", base);
		SetCurrentDirectoryW(path);

		/* what the shell visited: proj\game\assets\tree a lot, old\game\assets less, lone\only\sub, proj\src\other */
		swprintf(rank_file, MAX_PATH, L"%ls\\dir_rank.ant", base);
		SetEnvironmentVariableW(L"ANT_DIR_RANK_FILE", rank_file);
		swprintf(text, 8192, L"5.00 %lld %ls\\proj\\game\\assets\\tree\n2.00 %lld %ls\\old\\game\\assets\n"
			L"3.00 %lld %ls\\lone\\only\\sub\n1.00 %lld %ls\\proj\\src\\other\n",
			(long long) now, base, (long long) now, base, (long long) now, base, (long long) now, base);
		n = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof bytes, NULL, NULL);
		f = CreateFileW(rank_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		WriteFile(f, bytes, n - 1, &written, NULL);
		CloseHandle(f);

		/* two game folders: TAB lists them, best score first (the parents are not stored, they are found) */
		swprintf(want, 4096, L"%ls\\proj\\game\\|%ls\\old\\game\\", base, base);
		check_list("game\\ with game in two places: the folders themselves, best score first", L"game\\", want);
		check_list("the same after cd", L"cd game\\", want);
		check_list("game\\a: both have assets", L"game\\a", want);
		{
			ANT_COMPLETE c;
			wchar_t got[2048];
			int count = ant_complete(L"cd game\\a", 9, &c);

			ant_complete_text(&c, 1, ANT_APPLY_SELECT, got, 2048);
			/* the temporary folder may need quotes (spaces, ~ of a short name) */
			int k = 0;

			for (int i = 0; got[i]; i++)
				if (got[i] != L'"')
					got[k++] = got[i];
			got[k] = 0;
			swprintf(want, 4096, L"%ls\\old\\game\\a", base);
			report(count == 2 && c.roots && !wcscmp(got, want), "the 2nd folder as text: absolute path + the name typed", want, got);
			ant_complete_free(&c);
		}

		/* only one of them has the name being typed: the options are inside it, written after game\ */
		check_list("game\\m: only proj\\game has maps", L"game\\m", L"maps\\");
		check_apply("game\\m -> game\\maps\\", L"cd game\\m", L"cd game\\maps\\");
		check_list("game\\assets\\t: only proj has tree", L"game\\assets\\t", L"tree\\");
		check_apply("game\\assets\\t -> game\\assets\\tree\\", L"game\\assets\\t", L"game\\assets\\tree\\");

		/* a parent that was never entered: lone\ -> lone, found through lone\only\sub */
		check_list("lone\\ (only the child was visited)", L"lone\\", L"only\\");
		check_apply("lone\\o", L"cd lone\\o", L"cd lone\\only\\");
		check_list("a subfolder that was never entered still completes", L"lone\\only\\", L"sub\\");

		/* what does not use the table */
		check_list("a folder that exists here wins", L"src\\", L"inner\\");
		check_list("not the first word or the argument of cd", L"ls game\\", L"");
		check_list("a name that is nowhere", L"nowhere\\", L"");
		check_list("a drive is not looked for", L"Z:\\game\\", L"");
		check_list("@prog does not either", L"@game\\", L"");

		/* the hint: where it goes and how many others */
		n = ant_complete_where(L"game\\", 5, best, ANT_DIRS_PATH);
		swprintf(want, 4096, L"%ls\\proj\\game", base);
		report(n == 2 && !wcscmp(best, want), "hint: game\\ goes to proj\\game, +1", want, best);
		n = ant_complete_where(L"lone\\", 5, best, ANT_DIRS_PATH);
		swprintf(want, 4096, L"%ls\\lone", base);
		report(n == 1 && !wcscmp(best, want), "hint: lone\\ goes to lone", want, best);
		n = ant_complete_where(L"src\\", 4, best, ANT_DIRS_PATH);
		report(n == 0, "hint: none when the folder exists here", L"0", n ? L"found" : L"0");
		n = ant_complete_where(L"game", 4, best, ANT_DIRS_PATH);
		report(n == 0, "hint: none without a separator", L"0", n ? L"found" : L"0");

		/* choosing in the menu makes it win from then on, even against more points */
		swprintf(want, 4096, L"%ls\\old\\game", base);
		ant_dirs_choose(L"game\\", want);
		swprintf(want, 4096, L"%ls\\old\\game\\|%ls\\proj\\game\\", base, base);
		check_list("after choosing old\\game it comes first", L"game\\", want);

		/* entering a folder gives it a point: the parents follow */
		swprintf(path, MAX_PATH, L"%ls\\proj\\game\\maps", base);
		for (int i = 0; i < 12; i++)
			ant_dirs_add(path);
		swprintf(want, 4096, L"%ls\\proj\\game\\|%ls\\old\\game\\", base, base);
		check_list("twelve cd into proj\\game\\maps: proj\\game wins again", L"game\\", want);

		/* forgetting */
		swprintf(path, MAX_PATH, L"%ls\\lone\\only\\sub", base);
		report(ant_dirs_remove(path), "remove: the entry was there", L"1", L"0");
		check_list("removed: lone\\ is not found anymore", L"lone\\", L"");
		report(!ant_dirs_remove(path), "remove: the entry is gone now", L"0", L"1");
		ant_dirs_clear();
		check_list("cleared: nothing", L"game\\", L"");

		SetCurrentDirectoryW(root);
		SetEnvironmentVariableW(L"ANT_DIR_RANK_FILE", NULL);
		DeleteFileW(rank_file);
		for (n = 0; tree[n]; n++)
			;
		for (int i = n - 1; i >= 0; i--) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", base, tree[i]);
			RemoveDirectoryW(path);
		}
		RemoveDirectoryW(base);
	}

	remove_tree();

	printf("complete: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
