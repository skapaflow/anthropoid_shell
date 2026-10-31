#include <time.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "rank.h"
#include "string_util.h"

/*************************************************************
 * Frecency table of zoxide (src/db), kept in a text file,   *
 * one entry per line, most points first:                    *
 *   12.50 1760000000 git status                             *
 *   points, time of the last use, text.                     *
 * Using an entry adds points and stamps the time; the       *
 * score (ant_rank_score) weighs the points by how long ago  *
 * it was used. Aging: when the points add up to more than   *
 * maxage, all of them shrink to 90% of it and whatever      *
 * falls below 1 goes away; it runs before the new point, so *
 * a new entry always stays. A new entry with the table full *
 * replaces the one with the lowest score. The file is read  *
 * again before every change, so two shells add up their     *
 * points. Used by the commands (history.c) and by the       *
 * folders (dirs.c).                                         *
 *************************************************************/

bool ant_rank_init (ANT_RANK *r, int max, double maxage, bool fold) {

	memset(r, 0, sizeof *r);
	r->item = calloc(max, sizeof(ANT_RANK_ITEM));
	r->max = max;
	r->maxage = maxage;
	r->fold = fold;
	return r->item != NULL;
}

void ant_rank_clear (ANT_RANK *r) {

	while (r->count)
		free(r->item[--r->count].text);
}

/* the points weighed by the time since the last use (zoxide's Dir::score) */
double ant_rank_score (const ANT_RANK *r, int i, time_t now) {

	time_t age = now - r->item[i].last;

	if (age < 60 * 60)
		return r->item[i].points * 4;
	if (age < 24 * 60 * 60)
		return r->item[i].points * 2;
	if (age < 7 * 24 * 60 * 60)
		return r->item[i].points / 2;
	return r->item[i].points / 4;
}

void ant_rank_remove (ANT_RANK *r, int i) {

	free(r->item[i].text);
	memmove(r->item + i, r->item + i + 1, sizeof(ANT_RANK_ITEM) * (r->count - i - 1));
	r->count--;
}

/* index of the entry with this text, or -1 */
int ant_rank_find (const ANT_RANK *r, const wchar_t *text) {

	for (int i = 0; i < r->count; i++)
		if (r->fold ? !_wcsicmp(r->item[i].text, text) : !wcscmp(r->item[i].text, text))
			return i;
	return -1;
}

/* replaces what is in memory with the file (nothing if it cannot be read) */
void ant_rank_load (ANT_RANK *r, const wchar_t *path) {

	wchar_t *text, *line, *next, *end;

	ant_rank_clear(r);
	if (!(text = ant_file_text(path)))
		return;
	for (line = text; *line && r->count < r->max; line = next) {
		double points;
		long long last;

		/* cuts the line off the text (handles \r\n) */
		next = wcschr(line, L'\n');
		if (next) {
			if (next > line && next[-1] == L'\r')
				next[-1] = 0;
			*next++ = 0;
		} else
			next = line + wcslen(line);
		points = wcstod(line, &end);
		if (end == line || *end != L' ' || points < 1)
			continue;
		line = end + 1;
		/* without the time (the first format, "points text"): used now */
		last = wcstoll(line, &end, 10);
		if (end != line && *end == L' ' && last >= 1000000000)
			line = end + 1;
		else
			last = time(NULL);
		if (*line && (r->item[r->count].text = _wcsdup(line))) {
			r->item[r->count].points = points;
			r->item[r->count].last = (time_t) last;
			r->count++;
		}
	}
	free(text);
}

void ant_rank_save (const ANT_RANK *r, const wchar_t *path) {

	size_t size = 1;
	wchar_t *text;
	char *bytes;
	HANDLE f;
	DWORD written;
	int n = 0, b;

	for (int i = 0; i < r->count; i++)
		size += wcslen(r->item[i].text) + 64;
	if (!(text = malloc(sizeof(wchar_t) * size)))
		return;
	for (int i = 0; i < r->count; i++)
		n += swprintf(text + n, size - n, L"%.2f %lld %ls\n", r->item[i].points, (long long) r->item[i].last, r->item[i].text);
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

/* zoxide's Database::age: the total goes back to 90% of maxage */
static void age (ANT_RANK *r) {

	double total = 0, factor;

	for (int i = 0; i < r->count; i++)
		total += r->item[i].points;
	if (total <= r->maxage)
		return;
	factor = 0.9 * r->maxage / total;
	for (int i = r->count - 1; i >= 0; i--) {
		r->item[i].points *= factor;
		if (r->item[i].points < 1)
			ant_rank_remove(r, i);
	}
}

/*
 * 'points' more for the text, used now; it goes first among the entries with as many points.
 * The file is read again first and written back at the end.
 */
void ant_rank_add (ANT_RANK *r, const wchar_t *path, const wchar_t *text, double points) {

	ANT_RANK_ITEM item = { NULL, 0, 0 };
	time_t now = time(NULL);
	int i, at;

	ant_rank_load(r, path);
	if (r->keep)
		for (i = r->count - 1; i >= 0; i--)
			if (!r->keep(r->item[i].text))
				ant_rank_remove(r, i);
	age(r);
	i = ant_rank_find(r, text);
	if (i >= 0) {
		item = r->item[i];
		memmove(r->item + i, r->item + i + 1, sizeof(ANT_RANK_ITEM) * (r->count - i - 1));
		r->count--;
	} else {
		if (!(item.text = _wcsdup(text)))
			return;
		/* one entry per line */
		for (wchar_t *p = item.text; *p; p++)
			if (*p == L'\r' || *p == L'\n')
				*p = L' ';
		/* full: the lowest score goes away (a tie: the one used longest ago, then the lower line) */
		if (r->count == r->max) {
			int out = r->count - 1;
			for (int k = r->count - 2; k >= 0; k--)
				if (ant_rank_score(r, k, now) < ant_rank_score(r, out, now)
						|| (ant_rank_score(r, k, now) == ant_rank_score(r, out, now) && r->item[k].last < r->item[out].last))
					out = k;
			ant_rank_remove(r, out);
		}
	}
	item.points += points;
	item.last = now;
	for (at = 0; at < r->count && r->item[at].points > item.points; at++)
		;
	memmove(r->item + at + 1, r->item + at, sizeof(ANT_RANK_ITEM) * (r->count - at));
	r->item[at] = item;
	r->count++;
	ant_rank_save(r, path);
}
