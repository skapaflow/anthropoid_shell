#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "cd.h"
#include "../console/console.h"
#include "../shell/shell.h"

/*************************************************************
 * cd (port of cd.c from the old project, now in UTF-16 so   *
 * accented names work):                                     *
 *                                                           *
 *   cd            shows the current folder                  *
 *   cd <folder>   changes folder (quotes optional)          *
 *   cd -          back to the previous folder ($OLDPWD)     *
 *   cd.. / cd\    goes up one level / to the root           *
 *                                                           *
 * Implicit cd (fish): a folder path alone on the line       *
 * enters it: .., src\, .\build, ~, D:                       *
 *                                                           *
 * Only "cd <folder>", "cd..", "cd\" and the implicit cd     *
 * enter the list of folders (each once, the current last),  *
 * which left-cd shows (line_editor.c).                      *
 *************************************************************/

#define CD_LOG_MAX  100
#define CD_PATH_MAX (MAX_PATH * 4)

static wchar_t *cd_log[CD_LOG_MAX];
static int cd_count = 0;

static void to_utf8 (const wchar_t *w, char *out, int max) {

	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, max, NULL, NULL))
		out[0] = 0;
}

/* each folder is stored once: going back to one moves it to the end, so the last is the current one */
static void log_insert (const wchar_t *path) {

	for (int i = 0; i < cd_count; i++)
		if (!_wcsicmp(cd_log[i], path)) {
			wchar_t *same = cd_log[i];
			memmove(cd_log + i, cd_log + i + 1, sizeof(wchar_t *) * (cd_count - i - 1));
			cd_log[cd_count - 1] = same;
			return;
		}
	if (cd_count == CD_LOG_MAX) {
		free(cd_log[0]);
		memmove(cd_log, cd_log + 1, sizeof(wchar_t *) * (CD_LOG_MAX - 1));
		cd_count--;
	}
	cd_log[cd_count] = _wcsdup(path);
	if (cd_log[cd_count])
		cd_count++;
}

void ant_cd_init (void) {

	wchar_t cwd[CD_PATH_MAX];

	if (GetCurrentDirectoryW(CD_PATH_MAX, cwd)) {
		log_insert(cwd);
		SetEnvironmentVariableW(L"PWD", cwd);
	}
}

void ant_cd_free (void) {

	for (int i = 0; i < cd_count; i++)
		free(cd_log[i]);
	cd_count = 0;
}

/* the folders of the session, for left-cd (the last is the current one) */
int ant_cd_count (void) {

	return cd_count;
}

/* NULL outside the list */
const wchar_t *ant_cd_get (int index) {

	return (index >= 0 && index < cd_count) ? cd_log[index] : NULL;
}

/* shows the current folder */
void ant_cd_print (void) {

	wchar_t cwd[CD_PATH_MAX];
	char buf[CD_PATH_MAX * 3];

	if (!GetCurrentDirectoryW(CD_PATH_MAX, cwd))
		return;
	to_utf8(cwd, buf, sizeof buf);
	print(CLEAR|GREEN, "\"%s\"\n", buf);
}

static void not_found (const wchar_t *path) {

	ant_error(L" [!] \"%ls\" not found", path);
	ant_status = 1;
}

/*
 * changes folder and, if that worked, stores it in the history and sets PWD
 * (the new folder) and OLDPWD (the one it came from) in the environment, as POSIX asks
 */
static void change_dir (const wchar_t *path) {

	wchar_t cwd[CD_PATH_MAX], before[CD_PATH_MAX];
	bool had_before = GetCurrentDirectoryW(CD_PATH_MAX, before) > 0;

	if (!SetCurrentDirectoryW(path)) {
		not_found(path);
		return;
	}
	if (GetCurrentDirectoryW(CD_PATH_MAX, cwd)) {
		log_insert(cwd);
		SetEnvironmentVariableW(L"PWD", cwd);
	}
	if (had_before)
		SetEnvironmentVariableW(L"OLDPWD", before);
	ant_cd_print();
}

/* cd -: goes back to $OLDPWD */
static void change_dir_back (void) {

	wchar_t old[CD_PATH_MAX];
	DWORD n = GetEnvironmentVariableW(L"OLDPWD", old, CD_PATH_MAX);

	if (n == 0 || n >= CD_PATH_MAX) {
		ant_error(L"ant: cd: OLDPWD not set");
		ant_status = 1;
		return;
	}
	change_dir(old);
}

/* used by link: enters the shortcut's folder like a "cd <folder>" */
void ant_cd_to (const wchar_t *path) {

	change_dir(path);
}

/*
 * Implicit cd, as in fish: a command that is a path to an existing folder
 * enters it. Only path-shaped words count, so a bare word is still a command:
 * "..", anything ending in \ or /, starting with .\ ..\ ./ ../ \ / or ~, or a
 * drive (D: or D:\...). "~" is %USERPROFILE%. Puts the folder in out.
 */
static bool implicit_target (const wchar_t *word, wchar_t *out, int max) {

	int n = wcslen(word);
	bool shaped = !wcscmp(word, L"..") ||
		(n > 0 && (word[n - 1] == L'\\' || word[n - 1] == L'/')) ||
		!wcsncmp(word, L".\\", 2) || !wcsncmp(word, L"./", 2) ||
		!wcsncmp(word, L"..\\", 3) || !wcsncmp(word, L"../", 3) ||
		word[0] == L'\\' || word[0] == L'/' ||
		(word[0] == L'~' && (n == 1 || word[1] == L'\\' || word[1] == L'/')) ||
		(iswalpha(word[0]) && word[1] == L':' && (n == 2 || word[2] == L'\\' || word[2] == L'/'));
	DWORD attr;

	if (!shaped)
		return false;
	if (word[0] == L'~') {
		DWORD m = GetEnvironmentVariableW(L"USERPROFILE", out, max);
		if (m == 0 || m + wcslen(word) >= (DWORD) max)
			return false;
		wcscat(out, word + 1);
	} else if (n < max)
		wcscpy(out, word);
	else
		return false;

	attr = GetFileAttributesW(out);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* is 'word' alone on the line an implicit cd? (only checks) */
bool ant_cd_is_implicit (const wchar_t *word) {

	wchar_t path[CD_PATH_MAX];
	return implicit_target(word, path, CD_PATH_MAX);
}

/* runs the implicit cd if 'word' is one; returns false if it is not */
bool ant_cd_implicit (const wchar_t *word) {

	wchar_t path[CD_PATH_MAX];

	if (!implicit_target(word, path, CD_PATH_MAX))
		return false;
	change_dir(path);
	return true;
}

/* is 'word' one of the words ant_cmd_cd runs? cd, cd.., cd\ and cd/ (cd\Windows is not) */
bool ant_cd_is_command (const wchar_t *word) {

	return !_wcsicmp(word, L"cd") || !_wcsicmp(word, L"cd..") || !_wcsicmp(word, L"cd\\") || !_wcsicmp(word, L"cd/");
}

/*
 * Runs the command if 'arg' is a cd; returns false if it is not.
 * The arguments come without quotes (parser.c); several arguments
 * after cd become one path with spaces, as in cmd.
 */
bool ant_cmd_cd (const ANT_ARG *arg) {

	const char *s = arg->string;

	if (_strnicmp(s, "cd", 2))
		return false;
	s += 2;

	if (*s == 0) {
		char path[ANTMAX] = {0};
		wchar_t wide[CD_PATH_MAX];
		const ANT_ARG *a = arg->next;

		if (!a) {
			/* POSIX: cd alone goes home ($HOME; USERPROFILE is the Windows default) */
			wchar_t home[CD_PATH_MAX];

			if (GetEnvironmentVariableW(L"HOME", home, CD_PATH_MAX) == 0 &&
				GetEnvironmentVariableW(L"USERPROFILE", home, CD_PATH_MAX) == 0) {
				ant_error(L"ant: cd: HOME not set");
				ant_status = 1;
			} else
				change_dir(home);
			return true;
		}
		for (; a; a = a->next) {
			if (strlen(path) + strlen(a->string) + 2 >= sizeof path)
				break;
			if (*path)
				strcat(path, " ");
			strcat(path, a->string);
		}
		if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, CD_PATH_MAX)) {
			ant_status = 1;
			return true;
		}
		if (!wcscmp(wide, L"-"))
			change_dir_back();
		else
			change_dir(wide);
		return true;
	}

	if (!strcmp(s, "..")) {
		change_dir(L"..");
		return true;
	}
	if (!strcmp(s, "\\") || !strcmp(s, "/")) {
		change_dir(L"\\");
		return true;
	}
	return false;
}
