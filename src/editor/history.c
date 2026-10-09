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
 * command: "2026-09-30 14:05:33 C:\dir> git status";       *
 * 'history' numbers the lines when it shows them.           *
 * Every accepted line is appended; at startup the last      *
 * ANT_HISTORY_MAX go back into memory (up arrow, TAB        *
 * suggestion). ANT_HISTORY_FILE replaces the path (tests).  *
 * data\history_rank.ant keeps the most used commands, with  *
 * zoxide's frecency, which the autosuggestion tries first.  *
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
static wchar_t *log_read_path (const wchar_t *path) {

	HANDLE f;
	DWORD size, got = 0;
	char *bytes;
	wchar_t *text;
	int n;

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

static wchar_t *log_read (void) {

	wchar_t path[MAX_PATH];

	return log_path(path, MAX_PATH) ? log_read_path(path) : NULL;
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

/* "2026-09-30 14:05:33 " at s: its length (0 if it is not there) */
static int log_stamp (const wchar_t *s, struct tm *t) {

	int n = 0;

	memset(t, 0, sizeof *t);
	if (swscanf(s, L"%d-%d-%d %d:%d:%d %n", &t->tm_year, &t->tm_mon, &t->tm_mday,
			&t->tm_hour, &t->tm_min, &t->tm_sec, &n) < 6)
		return 0;
	return n;
}

/*
 * parses "2026-09-30 14:05:33 dir> text": *stamp points at the date, *dir at the
 * folder and *text at the command. The lines of the first format start with
 * their number ("42 2026-09-30 ..."), which is skipped.
 */
static bool log_parse (wchar_t *line, time_t *when, wchar_t **stamp, wchar_t **dir, wchar_t **text) {

	struct tm t;
	wchar_t *sep;
	int n = log_stamp(line, &t);

	if (n == 0 && iswdigit(*line) && (sep = wcschr(line, L' '))) {
		line = sep + 1;
		n = log_stamp(line, &t);
	}
	if (n == 0 || !(sep = wcsstr(line + n, L"> ")))
		return false;
	t.tm_year -= 1900;
	t.tm_mon--;
	t.tm_isdst = -1;
	*when = mktime(&t);
	*stamp = line;
	*dir = line + n;
	*text = sep + 2;
	return true;
}

/* appends "date time dir> cmd" in a single write, so two shells do not mix lines */
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
	head = swprintf(line, sizeof line / sizeof *line, L"%04d-%02d-%02d %02d:%02d:%02d %ls> ",
		t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, cwd);
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
	if (b > 0)
		WriteFile(f, bytes, b, &written, NULL);
	CloseHandle(f);
}

/* ---------- rank file ---------- */

/*
 * data\history_rank.ant: the ANT_HISTORY_MAX most used commands, one per line,
 * "points last-use command" (12.50 1760000000 git status), most points first;
 * the frecency of zoxide (src/db): running a command adds a point and stamps
 * the time. The autosuggestion weighs the points by how long ago the command
 * was used (rank_score), so what was run in the last hour comes before what
 * piled up points last month. Aging: when the points add up to more than
 * RANK_MAXAGE, all of them shrink to 90% of it and whatever falls below 1
 * goes away; it runs before the new point, so a new command always stays. A
 * new command with the list full replaces the one with the lowest score. The
 * file is read again before every change, so two shells add up their points.
 */
#define RANK_MAXAGE 1000.0

typedef struct {
	wchar_t *text;
	double points;
	time_t last;
} ANT_RANK;

static ANT_RANK rank[ANT_HISTORY_MAX];
static int rank_count = 0;

/* history.ant -> history_rank.ant (the same for the ANT_HISTORY_FILE of the tests) */
static bool rank_path (wchar_t *out, int max) {

	int n;

	if (!log_path(out, max))
		return false;
	n = wcslen(out);
	if (n + 6 >= max)
		return false;
	if (n >= 4 && !_wcsicmp(out + n - 4, L".ant"))
		n -= 4;
	wcscpy(out + n, L"_rank.ant");
	return true;
}

/* the points weighed by the time since the last use (zoxide's Dir::score) */
static double rank_score (int i, time_t now) {

	time_t age = now - rank[i].last;

	if (age < 60 * 60)
		return rank[i].points * 4;
	if (age < 24 * 60 * 60)
		return rank[i].points * 2;
	if (age < 7 * 24 * 60 * 60)
		return rank[i].points / 2;
	return rank[i].points / 4;
}

static void rank_remove (int i) {

	free(rank[i].text);
	memmove(rank + i, rank + i + 1, sizeof(ANT_RANK) * (rank_count - i - 1));
	rank_count--;
}

static void rank_clear (void) {

	while (rank_count)
		free(rank[--rank_count].text);
}

static void rank_load (void) {

	wchar_t path[MAX_PATH], *text, *line, *next, *end;

	rank_clear();
	if (!rank_path(path, MAX_PATH) || !(text = log_read_path(path)))
		return;
	for (line = text; *line && rank_count < ANT_HISTORY_MAX; line = next) {
		double points;
		long long last;

		next = log_next_line(line);
		points = wcstod(line, &end);
		if (end == line || *end != L' ' || points < 1)
			continue;
		line = end + 1;
		/* without the time (the first format, "points command"): used now */
		last = wcstoll(line, &end, 10);
		if (end != line && *end == L' ' && last >= 1000000000)
			line = end + 1;
		else
			last = time(NULL);
		if (*line && (rank[rank_count].text = _wcsdup(line))) {
			rank[rank_count].points = points;
			rank[rank_count].last = (time_t) last;
			rank_count++;
		}
	}
	free(text);
}

static void rank_save (void) {

	wchar_t path[MAX_PATH];
	size_t size = 1;
	wchar_t *text;
	char *bytes;
	HANDLE f;
	DWORD written;
	int n = 0, b;

	if (!rank_path(path, MAX_PATH))
		return;
	for (int i = 0; i < rank_count; i++)
		size += wcslen(rank[i].text) + 64;
	if (!(text = malloc(sizeof(wchar_t) * size)))
		return;
	for (int i = 0; i < rank_count; i++)
		n += swprintf(text + n, size - n, L"%.2f %lld %ls\n", rank[i].points, (long long) rank[i].last, rank[i].text);
	b = WideCharToMultiByte(CP_UTF8, 0, text, n, NULL, 0, NULL, NULL);
	bytes = malloc(b + 1);
	if (bytes) {
		b = WideCharToMultiByte(CP_UTF8, 0, text, n, bytes, b, NULL, NULL);
		f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
		if (f != INVALID_HANDLE_VALUE) {
			if (b > 0)
				WriteFile(f, bytes, b, &written, NULL);
			CloseHandle(f);
		}
	}
	free(bytes);
	free(text);
}

/* zoxide's Database::age: the total goes back to 90% of RANK_MAXAGE */
static void rank_age (void) {

	double total = 0, factor;

	for (int i = 0; i < rank_count; i++)
		total += rank[i].points;
	if (total <= RANK_MAXAGE)
		return;
	factor = 0.9 * RANK_MAXAGE / total;
	for (int i = rank_count - 1; i >= 0; i--) {
		rank[i].points *= factor;
		if (rank[i].points < 1)
			rank_remove(i);
	}
}

/* one more point for cmd, used now; it goes first among the commands with as many points */
static void rank_add (const wchar_t *cmd) {

	ANT_RANK r = { NULL, 0, 0 };
	time_t now = time(NULL);
	int i, at;

	rank_load();
	rank_age();
	for (i = 0; i < rank_count; i++)
		if (!wcscmp(rank[i].text, cmd))
			break;
	if (i < rank_count) {
		r = rank[i];
		memmove(rank + i, rank + i + 1, sizeof(ANT_RANK) * (rank_count - i - 1));
		rank_count--;
	} else {
		if (!(r.text = _wcsdup(cmd)))
			return;
		/* one command per line */
		for (wchar_t *p = r.text; *p; p++)
			if (*p == L'\r' || *p == L'\n')
				*p = L' ';
		/* full: the lowest score goes away (a tie: the one used longest ago, then the lower line) */
		if (rank_count == ANT_HISTORY_MAX) {
			int out = rank_count - 1;
			for (int k = rank_count - 2; k >= 0; k--)
				if (rank_score(k, now) < rank_score(out, now)
						|| (rank_score(k, now) == rank_score(out, now) && rank[k].last < rank[out].last))
					out = k;
			rank_remove(out);
		}
	}
	r.points++;
	r.last = now;
	for (at = 0; at < rank_count && rank[at].points > r.points; at++)
		;
	memmove(rank + at + 1, rank + at, sizeof(ANT_RANK) * (rank_count - at));
	rank[at] = r;
	rank_count++;
	rank_save();
}

/* autosuggestion: the command with the best score that 'accept' takes (a tie: the newest, then the upper line) */
const wchar_t *ant_history_rank_find (bool (*accept) (const wchar_t *)) {

	time_t now = time(NULL);
	int best = -1;

	for (int i = 0; i < rank_count; i++)
		if (accept(rank[i].text) && (best < 0 || rank_score(i, now) > rank_score(best, now)
				|| (rank_score(i, now) == rank_score(best, now) && rank[i].last > rank[best].last)))
			best = i;
	return best < 0 ? NULL : rank[best].text;
}

/* puts the last ANT_HISTORY_MAX commands of the file in memory */
void ant_history_load (void) {

	wchar_t *text = log_read(), *line, *next;

	rank_load();
	if (!text)
		return;
	for (line = text; *line; line = next) {
		time_t when;
		wchar_t *stamp, *dir, *cmd;

		next = log_next_line(line);
		if (!log_parse(line, &when, &stamp, &dir, &cmd))
			continue;
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

/*
 * 'history': prints the last 'max' lines of the log (all of them if max < 0); the
 * number of the line (not stored in the file, as in bash), date and time in light
 * green, the folder in dark gray, the command in the default color
 */
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
		time_t when;
		wchar_t *stamp, *dir, *cmd;

		next = log_next_line(line);
		if (log_parse(line, &when, &stamp, &dir, &cmd))
			lines[total++] = line;
	}
	for (int i = (max >= 0 && total > max) ? total - max : 0; i < total; i++) {
		time_t when;
		wchar_t *stamp, *dir, *cmd, keep;

		log_parse(lines[i], &when, &stamp, &dir, &cmd);
		dir[-1] = 0;
		print(CLEAR|GREEN, "%d %ls", i + 1, stamp);
		dir[-1] = L' ';
		keep = *cmd;
		*cmd = 0;
		to_narrowchar(narrow, sizeof narrow, dir - 1);
		print(GRAY, "%s", narrow);
		*cmd = keep;
		to_narrowchar(narrow, sizeof narrow, cmd);
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
	if (rank_path(path, MAX_PATH))
		DeleteFileW(path);
	rank_clear();
}

void ant_history_add (const wchar_t *cmd) {

	if (!cmd || !*cmd)
		return;

	log_append(cmd);
	rank_add(cmd);
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
