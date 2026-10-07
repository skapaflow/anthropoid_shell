#include <time.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "history.h"
#include "../console/console.h"
#include "../shell/shell.h"
#include "../types/string_util.h"

/*************************************************************
 * Command history (UTF-16), with the same semantics as      *
 * conhost's CommandHistory (src/host/history.cpp):          *
 *   - index 0 is the oldest;                                *
 *   - 'displayed' is the last command shown (↑/↓/F8/F7);    *
 *   - 'reset': the next ↑ returns 'displayed' without       *
 *     moving back;                                          *
 *   - a repeat in a row is not stored again.                *
 *                                                           *
 * The full log lives in data\history.ant, one line per      *
 * command: "42 2026-09-30 14:05:33 C:\dir> git status".     *
 * Every accepted line is appended; at startup the last      *
 * ANT_HISTORY_MAX go back into memory (up arrow, TAB        *
 * suggestion). ANT_HISTORY_FILE replaces the path (tests).  *
 *************************************************************/

typedef struct {
	wchar_t *text;
	time_t time;
} ANT_HISTORY;

static ANT_HISTORY entries[ANT_HISTORY_MAX];
static int count = 0;
static int displayed = -1;
static bool reset = true;

static void remove_at (int i) {

	free(entries[i].text);
	memmove(entries + i, entries + i + 1, sizeof(ANT_HISTORY) * (count - i - 1));
	count--;
}

/* ---------- log file ---------- */

static int log_index = 0; /* index of the last line of the file */

/* data\history.ant next to antshell.exe, or ANT_HISTORY_FILE */
static bool log_path (wchar_t *out, int max) {

	int n = GetEnvironmentVariableW(L"ANT_HISTORY_FILE", out, max);

	if (n > 0 && n < max)
		return true;
	n = GetModuleFileNameW(NULL, out, max);
	if (n <= 0 || n >= max)
		return false;
	while (n > 0 && out[n - 1] != L'\\')
		n--;
	if (n == 0 || n + 20 >= max)
		return false;
	wcscpy(out + n, L"data\\history.ant");
	return true;
}

/* the whole file in UTF-16 (NULL: no file or empty) */
static wchar_t *log_read (void) {

	wchar_t path[MAX_PATH];
	HANDLE f;
	DWORD size, got = 0;
	char *bytes;
	wchar_t *text;
	int n;

	if (!log_path(path, MAX_PATH))
		return NULL;
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return NULL;
	size = GetFileSize(f, NULL);
	bytes = (size && size != INVALID_FILE_SIZE) ? malloc(size) : NULL;
	if (!bytes || !ReadFile(f, bytes, size, &got, NULL) || !got) {
		free(bytes);
		CloseHandle(f);
		return NULL;
	}
	CloseHandle(f);
	n = MultiByteToWideChar(CP_UTF8, 0, bytes, got, NULL, 0);
	text = malloc(sizeof(wchar_t) * (n + 1));
	if (text) {
		MultiByteToWideChar(CP_UTF8, 0, bytes, got, text, n);
		text[n] = 0;
	}
	free(bytes);
	return text;
}

/* cuts the next line off the text (handles \r\n) and returns where the following one starts */
static wchar_t *log_next_line (wchar_t *line) {

	wchar_t *end = wcschr(line, L'\n');

	if (!end)
		return line + wcslen(line);
	if (end > line && end[-1] == L'\r')
		end[-1] = 0;
	*end = 0;
	return end + 1;
}

/* parses "42 2026-09-30 14:05:33 dir> text"; *text points at the command */
static bool log_parse (wchar_t *line, int *index, time_t *when, wchar_t **text) {

	struct tm t;
	int i, n = 0;
	wchar_t *sep;

	memset(&t, 0, sizeof t);
	if (swscanf(line, L"%d %d-%d-%d %d:%d:%d %n", &i, &t.tm_year, &t.tm_mon, &t.tm_mday,
			&t.tm_hour, &t.tm_min, &t.tm_sec, &n) < 7 || n == 0)
		return false;
	sep = wcsstr(line + n, L"> ");
	if (!sep)
		return false;
	t.tm_year -= 1900;
	t.tm_mon--;
	t.tm_isdst = -1;
	*index = i;
	*when = mktime(&t);
	*text = sep + 2;
	return true;
}

/* appends "index date time dir> cmd" in a single write, so two shells do not mix lines */
static void log_append (const wchar_t *cmd) {

	static wchar_t line[ANTMAX + 2 * MAX_PATH];
	static char bytes[(ANTMAX + 2 * MAX_PATH) * 3];
	wchar_t path[MAX_PATH], cwd[MAX_PATH];
	struct tm *t;
	time_t now = time(NULL);
	HANDLE f;
	DWORD written;
	int n, head, b;

	if (!log_path(path, MAX_PATH))
		return;
	if (!GetCurrentDirectoryW(MAX_PATH, cwd))
		cwd[0] = 0;
	t = localtime(&now);
	head = swprintf(line, sizeof line / sizeof *line, L"%d %04d-%02d-%02d %02d:%02d:%02d %ls> ",
		log_index + 1, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, cwd);
	if (head <= 0)
		return;
	n = swprintf(line + head, sizeof line / sizeof *line - head - 1, L"%ls", cmd);
	if (n < 0)
		return;
	/* one command per line */
	for (int i = head; i < head + n; i++)
		if (line[i] == L'\r' || line[i] == L'\n')
			line[i] = L' ';
	line[head + n] = L'\n';
	b = WideCharToMultiByte(CP_UTF8, 0, line, head + n + 1, bytes, sizeof bytes, NULL, NULL);
	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	if (b > 0 && WriteFile(f, bytes, b, &written, NULL))
		log_index++;
	CloseHandle(f);
}

/* puts the last ANT_HISTORY_MAX commands of the file in memory; remembers the last index */
void ant_history_load (void) {

	wchar_t *text = log_read(), *line, *next;

	if (!text)
		return;
	for (line = text; *line; line = next) {
		int index;
		time_t when;
		wchar_t *cmd;

		next = log_next_line(line);
		if (!log_parse(line, &index, &when, &cmd))
			continue;
		log_index = index;
		if (*cmd && (count == 0 || wcscmp(entries[count - 1].text, cmd))) {
			if (count == ANT_HISTORY_MAX)
				remove_at(0);
			entries[count].text = _wcsdup(cmd);
			entries[count].time = when;
			if (entries[count].text)
				count++;
		}
	}
	free(text);
	displayed = count - 1;
	reset = true;
}

/* 'history': prints the last 'max' lines of the log (all of them if max < 0); index, date and time in light green */
void ant_history_print (int max) {

	static char narrow[ANTMAX * 3];
	wchar_t *text = log_read(), *line, *next, **lines;
	int total = 0;

	if (!text)
		return;
	lines = malloc(sizeof(wchar_t *) * (wcslen(text) / 2 + 1));
	if (!lines) {
		free(text);
		return;
	}
	for (line = text; *line; line = next) {
		wchar_t *sp = line;

		next = log_next_line(line);
		/* index, date and time are the first 3 fields */
		for (int field = 0; field < 3 && sp; field++)
			sp = wcschr(sp + (field ? 1 : 0), L' ');
		if (sp)
			lines[total++] = line;
	}
	for (int i = (max >= 0 && total > max) ? total - max : 0; i < total; i++) {
		wchar_t *sp = lines[i];

		for (int field = 0; field < 3; field++)
			sp = wcschr(sp + (field ? 1 : 0), L' ');
		*sp = 0;
		print(CLEAR|GREEN, "%ls", lines[i]);
		*sp = L' ';
		/* the folder ("folder>") in dark gray, the command in the default color */
		wchar_t *cmd = wcsstr(sp, L"> ");
		wchar_t keep = 0;

		if (cmd) {
			cmd += 2;
			keep = *cmd;
			*cmd = 0;
			to_narrowchar(narrow, sp);
			print(GRAY, "%s", narrow);
			*cmd = keep;
			sp = cmd;
		}
		to_narrowchar(narrow, sp);
		printf("%s\n", narrow);
	}
	free(lines);
	free(text);
}

/* 'history clear': empties the file too */
void ant_history_clear_log (void) {

	wchar_t path[MAX_PATH];

	if (log_path(path, MAX_PATH)) {
		HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, TRUNCATE_EXISTING, 0, NULL);
		if (f != INVALID_HANDLE_VALUE)
			CloseHandle(f);
	}
	log_index = 0;
}

void ant_history_add (const wchar_t *cmd) {

	if (!cmd || !*cmd)
		return;

	log_append(cmd);
	if (count == 0 || wcscmp(entries[count-1].text, cmd)) {
		wchar_t *copy = _wcsdup(cmd);
		if (!copy)
			return;
		/* full: drops the oldest */
		if (count == ANT_HISTORY_MAX) {
			remove_at(0);
			displayed--;
		}
		entries[count].text = copy;
		entries[count].time = time(NULL);
		count++;

		/* ran a history command unchanged: continue from where it was */
		if (displayed < 0 || displayed >= count || wcscmp(entries[displayed].text, cmd))
			displayed = count - 1;
	}
	reset = true;
}

int ant_history_count (void) {

	return count;
}

const wchar_t *ant_history_get (int i) {

	return (i >= 0 && i < count) ? entries[i].text : L"";
}

time_t ant_history_time (int i) {

	return (i >= 0 && i < count) ? entries[i].time : 0;
}

/* reference command for F1/F2/F3/→ */
const wchar_t *ant_history_last (void) {

	return ant_history_get(displayed);
}

bool ant_history_at_first (void) {

	return count == 0 || (!reset && displayed <= 0);
}

bool ant_history_at_last (void) {

	return count == 0 || displayed == count - 1;
}

const wchar_t *ant_history_nth (int i) {

	if (count == 0) {
		displayed = 0;
		return L"";
	}
	displayed = (i < 0 ? 0 : (i >= count ? count - 1 : i));
	return entries[displayed].text;
}

const wchar_t *ant_history_prev (void) {

	if (reset)
		reset = false;
	else
		displayed--;
	return ant_history_nth(displayed);
}

const wchar_t *ant_history_next (void) {

	return ant_history_nth(displayed + 1);
}

int ant_history_displayed (void) {

	return displayed;
}

/* F8: searches backwards (wrapping around) for a command that starts with prefix[0..n) */
bool ant_history_find (const wchar_t *prefix, int n, int *index) {

	int i = displayed;

	if (count == 0)
		return false;

	if (reset)
		reset = false;
	else
		i = (i <= 0 ? count - 1 : i - 1);

	if (i < 0 || i >= count)
		i = count - 1;

	for (int k = 0; k < count; k++) {
		if (!wcsncmp(entries[i].text, prefix, n) && (int) wcslen(entries[i].text) >= n) {
			*index = i;
			return true;
		}
		i = (i <= 0 ? count - 1 : i - 1);
	}
	return false;
}

/* Shift+↑/↓ in the F7 list: swaps two commands */
void ant_history_swap (int a, int b) {

	ANT_HISTORY t;

	if (a < 0 || b < 0 || a >= count || b >= count || a == b)
		return;
	t = entries[a];
	entries[a] = entries[b];
	entries[b] = t;
}

void ant_history_remove (int i) {

	if (i < 0 || i >= count)
		return;
	remove_at(i);
	if (displayed == i)
		displayed = -1;
	else if (displayed > i)
		displayed--;
}

void ant_history_clear (void) {

	while (count)
		remove_at(count - 1);
	displayed = -1;
	reset = true;
}
