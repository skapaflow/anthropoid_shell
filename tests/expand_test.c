/*************************************************************
 * Test of the expand module (src/shell/expand.c).              *
 *                                                           *
 * Each line goes through the parser and expansion; the      *
 * first command becomes {arg,arg >file}. Glob cases run     *
 * in a temporary folder with known files.                   *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#include "../src/shell/parser.h"
#include "../src/shell/expand.h"

int ant_status = 0;

static int failures = 0, total = 0;

static void check (const wchar_t *line, const wchar_t *want) {

	ANT_LIST list;
	ANT_CMD out;
	wchar_t got[4096] = L"";
	char a[4096], b[4096], l[1024];

	if (!ant_parse(line, &list))
		wcscpy(got, L"ERROR");
	else {
		if (!ant_expand_cmd(&list.p[0].cmd[0], &out, true))
			wcscpy(got, L"MEMORY");
		else {
			static const wchar_t *redir[] = { L"<", L">", L">>", L"2>", L"2>>", L"2>&1" };
			wcscat(got, L"{");
			for (int k = 0; k < out.argc; k++) {
				if (k)
					wcscat(got, L",");
				wcscat(got, out.argv[k]);
			}
			for (int k = 0; k < out.nredir; k++) {
				wcscat(got, L" ");
				wcscat(got, redir[out.redir[k].kind]);
				if (out.redir[k].path)
					wcscat(got, out.redir[k].path);
			}
			wcscat(got, L"}");
			ant_expand_free(&out);
		}
		ant_parse_free(&list);
	}

	total++;
	if (!wcscmp(got, want))
		return;
	failures++;
	WideCharToMultiByte(CP_UTF8, 0, line, -1, l, sizeof l, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, want, -1, a, sizeof a, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, got, -1, b, sizeof b, NULL, NULL);
	printf("FAIL  %s\n      expected: %s\n      got:      %s\n", l, a, b);
}

static void touch (const wchar_t *path) {

	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

/* deletes everything inside dir (files and subfolders), keeping dir itself */
static void wipe (const wchar_t *dir) {

	wchar_t pattern[MAX_PATH], path[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE h;

	swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
	if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
			continue;
		swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			wipe(path);
			RemoveDirectoryW(path);
		} else {
			SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(path);
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

int main (void) {

	wchar_t dir[MAX_PATH], old[MAX_PATH];

	/* variables */
	SetEnvironmentVariableW(L"ANT_T", L"value");
	SetEnvironmentVariableW(L"ANT_SP", L"C:\\with space\\x");
	SetEnvironmentVariableW(L"ANT_EMPTY", L"");
	SetEnvironmentVariableW(L"ANT_NONE", NULL);
	SetEnvironmentVariableW(L"HOME", L"C:\\home");
	SetEnvironmentVariableW(L"ANT_STAR", L"*");

	check(L"echo $ANT_T", L"{echo,value}");
	check(L"echo ${ANT_T}x", L"{echo,valuex}");
	check(L"echo $ANT_T$ANT_T", L"{echo,valuevalue}");
	check(L"echo \"$ANT_T and $ANT_T\"", L"{echo,value and value}");
	check(L"echo '$ANT_T'", L"{echo,$ANT_T}");
	check(L"echo a'$ANT_T'b\"$ANT_T\"", L"{echo,a$ANT_Tbvalue}");
	check(L"echo $ANT_SP", L"{echo,C:\\with space\\x}");
	check(L"echo $ANT_NONE", L"{echo}");
	check(L"echo $ANT_EMPTY x", L"{echo,x}");
	check(L"echo \"$ANT_NONE\"", L"{echo,}");
	check(L"echo x$ANT_NONE", L"{echo,x}");
	check(L"echo $", L"{echo,$}");
	check(L"echo $ x", L"{echo,$,x}");
	check(L"echo ${}", L"{echo,${}}");
	check(L"echo ${ANT_T", L"{echo,${ANT_T}");
	check(L"echo $1a", L"{echo,$1a}");
	check(L"echo $ant_t", L"{echo,value}");

	/* $? */
	ant_status = 0;
	check(L"echo $?", L"{echo,0}");
	ant_status = 9009;
	check(L"echo $?", L"{echo,9009}");
	check(L"echo \"status $?\"", L"{echo,status 9009}");
	check(L"echo '$?'", L"{echo,$?}");
	ant_status = 0;

	/* ~ */
	check(L"cd ~", L"{cd,C:\\home}");
	check(L"cp x ~\\Desktop", L"{cp,x,C:\\home\\Desktop}");
	check(L"cp x ~/Desktop", L"{cp,x,C:\\home/Desktop}");
	check(L"echo a~", L"{echo,a~}");
	check(L"echo ~x", L"{echo,~x}");
	check(L"echo '~'", L"{echo,~}");
	check(L"echo \"~\"", L"{echo,~}");
	check(L"echo ~ ~", L"{echo,C:\\home,C:\\home}");
	SetEnvironmentVariableW(L"HOME", NULL);
	SetEnvironmentVariableW(L"USERPROFILE", L"C:\\profile");
	check(L"echo ~", L"{echo,C:\\profile}");
	SetEnvironmentVariableW(L"HOME", L"C:\\home");

	/* redirections: $ and ~, no glob */
	check(L"ls > $ANT_T.txt", L"{ls >value.txt}");
	check(L"ls 2>> ~\\log", L"{ls 2>>C:\\home\\log}");
	check(L"ls > *.txt", L"{ls >*.txt}");

	/* glob in a known folder */
	GetCurrentDirectoryW(MAX_PATH, old);
	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"ant_expand_test");
	CreateDirectoryW(dir, NULL);
	wipe(dir);                                  /* nothing left over from an earlier run */
	SetCurrentDirectoryW(dir);
	touch(L"a.c");
	touch(L"B.c");
	touch(L"c.h");
	touch(L".hidden.c");
	touch(L"ação.c");
	CreateDirectoryW(L"sub", NULL);
	CreateDirectoryW(L"sub2", NULL);
	touch(L"sub\\x.c");
	touch(L"sub\\y.h");
	touch(L"sub2\\z.c");

	check(L"ls *.c", L"{ls,a.c,ação.c,B.c}");
	check(L"ls *.h", L"{ls,c.h}");
	check(L"ls ?.c", L"{ls,a.c,B.c}");
	check(L"ls *.C", L"{ls,a.c,ação.c,B.c}");
	check(L"ls *.zzz", L"{ls,*.zzz}");
	check(L"ls '*.c'", L"{ls,*.c}");
	check(L"ls \"*.c\"", L"{ls,*.c}");
	check(L"ls .*.c", L"{ls,.hidden.c}");
	check(L"ls sub\\*.c", L"{ls,sub\\x.c}");
	check(L"ls sub/*.c", L"{ls,sub/x.c}");
	check(L"ls sub*\\*.c", L"{ls,sub\\x.c,sub2\\z.c}");
	check(L"ls *\\*.h", L"{ls,sub\\y.h}");
	check(L"ls s*", L"{ls,sub,sub2}");
	check(L"ls *\\", L"{ls,sub\\,sub2\\}");
	check(L"ls .\\*.h", L"{ls,.\\c.h}");
	check(L"ls *.c *.h", L"{ls,a.c,ação.c,B.c,c.h}");
	check(L"ls $ANT_STAR", L"{ls,*}");
	check(L"ls a*c", L"{ls,a.c,ação.c}");
	check(L"2*3", L"{2*3}");
	check(L"ls ..\\ant_expand_test\\*.h", L"{ls,..\\ant_expand_test\\c.h}");

	SetCurrentDirectoryW(old);

	printf("expand: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
