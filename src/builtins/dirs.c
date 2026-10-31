#include <time.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#include "dirs.h"
#include "../types/rank.h"

/*************************************************************
 * Frecency of folders, the zoxide that replaced [link]:     *
 *                                                           *
 *   - every folder the shell enters gets a point in         *
 *     data\dir_rank.ant (types/rank.c; ANT_DIR_RANK_FILE    *
 *     replaces the path, for the tests);                    *
 *   - a path whose first component is not in the current    *
 *     folder (game\, game\assets) means the folders of the  *
 *     table that have that name in their path, so game\     *
 *     is enough after visiting proj\game\assets: the        *
 *     parents are not stored, they are found when asked,    *
 *     and each one is worth the sum of everything below it; *
 *   - the rest of the typed path has to exist under it;     *
 *   - when several folders fit, the best score wins and     *
 *     TAB shows all of them (complete.c); choosing one     *
 *     there makes it win from then on (ant_dirs_choose).    *
 *************************************************************/

#define DIRS_MAX 200
#define DIRS_MAXAGE 2000.0

static ANT_RANK table;
static bool table_loaded = false;
static FILETIME table_time;
static DWORD table_size;

static bool is_sep (wchar_t c) {

	return c == L'\\' || c == L'/';
}

static bool is_dir (const wchar_t *path) {

	DWORD attr = GetFileAttributesW(path);

	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* data\dir_rank.ant next to antshell.exe, or ANT_DIR_RANK_FILE */
static bool table_path (wchar_t *out, int max) {

	int n = GetEnvironmentVariableW(L"ANT_DIR_RANK_FILE", out, max);

	if (n > 0 && n < max)
		return true;
	n = GetModuleFileNameW(NULL, out, max);
	if (n <= 0 || n >= max)
		return false;
	while (n > 0 && out[n - 1] != L'\\')
		n--;
	if (n == 0 || n + 20 >= max)
		return false;
	wcscpy(out + n, L"data\\dir_rank.ant");
	return true;
}

static bool table_ready (void) {

	if (!table.item) {
		if (!ant_rank_init(&table, DIRS_MAX, DIRS_MAXAGE, true))
			return false;
		table.keep = is_dir;
	}
	return true;
}

/* reads the file again if it changed since the last time (the other shells write to it too) */
static void table_sync (void) {

	wchar_t path[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA info;

	if (!table_ready() || !table_path(path, MAX_PATH))
		return;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
		ant_rank_clear(&table);
		table_loaded = false;
		return;
	}
	if (table_loaded && info.nFileSizeLow == table_size && !CompareFileTime(&info.ftLastWriteTime, &table_time))
		return;
	ant_rank_load(&table, path);
	table_time = info.ftLastWriteTime;
	table_size = info.nFileSizeLow;
	table_loaded = true;
}

/* one point for a folder the shell entered */
void ant_dirs_add (const wchar_t *dir) {

	wchar_t path[MAX_PATH], copy[ANT_DIRS_PATH];
	int n = wcslen(dir);

	if (n == 0 || n >= ANT_DIRS_PATH || !table_ready() || !table_path(path, MAX_PATH))
		return;
	wcscpy(copy, dir);
	/* no trailing separator, except for the root (C:\) */
	while (n > 3 && is_sep(copy[n - 1]))
		copy[--n] = 0;
	ant_rank_add(&table, path, copy, 1);
	table_loaded = false;
}

typedef struct {
	int item;     /* an entry of the table whose path has the folder as a prefix */
	int len;      /* units of that prefix */
	double score; /* the sum of the scores of everything under it */
} GROUP;

static void group_add (GROUP **groups, int *count, int *capacity, int item, int len, double score) {

	const wchar_t *text = table.item[item].text;

	for (int g = 0; g < *count; g++)
		if ((*groups)[g].len == len && !_wcsnicmp(table.item[(*groups)[g].item].text, text, len)) {
			(*groups)[g].score += score;
			return;
		}
	if (*count == *capacity) {
		int grow = *capacity ? *capacity * 2 : 32;
		GROUP *bigger = realloc(*groups, sizeof(GROUP) * grow);
		if (!bigger)
			return;
		*groups = bigger;
		*capacity = grow;
	}
	(*groups)[*count].item = item;
	(*groups)[*count].len = len;
	(*groups)[*count].score = score;
	(*count)++;
}

static int by_score (const void *x, const void *y) {

	double d = ((const ANT_DIR_CAND *) y)->score - ((const ANT_DIR_CAND *) x)->score;

	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/*
 * The folders that 'spec' (game, game\, game/assets\tree) can mean: the folders of the
 * table with a component named like the first one, plus the rest of the path, as long
 * as the whole thing exists. Sorted by score, best first; the array is for free().
 * 0 when the first component is not a plain name (., .., ~, a drive, a wildcard, a root \).
 */
int ant_dirs_resolve (const wchar_t *spec, ANT_DIR_CAND **out) {

	wchar_t rest[ANT_DIRS_PATH];
	const wchar_t *name = spec;
	GROUP *groups = NULL;
	ANT_DIR_CAND *cand;
	time_t now = time(NULL);
	int namelen = 0, restlen = 0, ngroups = 0, capacity = 0, n = 0;

	*out = NULL;
	if (is_sep(*name))
		return 0;
	while (name[namelen] && !is_sep(name[namelen]))
		namelen++;
	if (namelen == 0 || (namelen <= 2 && name[0] == L'.' && (namelen == 1 || name[1] == L'.')) ||
			(namelen == 1 && name[0] == L'~') || wmemchr(name, L':', namelen) ||
			wmemchr(name, L'*', namelen) || wmemchr(name, L'?', namelen))
		return 0;

	/* the other components, joined with \ and without empty ones (game//assets\) */
	for (const wchar_t *p = name + namelen; *p; ) {
		int len = 0;
		while (is_sep(*p))
			p++;
		while (p[len] && !is_sep(p[len]))
			len++;
		if (len == 0)
			break;
		if (restlen + len + 2 >= ANT_DIRS_PATH)
			return 0;
		rest[restlen++] = L'\\';
		wmemcpy(rest + restlen, p, len);
		restlen += len;
		p += len;
	}
	rest[restlen] = 0;

	table_sync();
	for (int i = 0; i < table.count; i++) {
		const wchar_t *text = table.item[i].text;
		double score = ant_rank_score(&table, i, now);

		for (int k = 0; text[k]; ) {
			int e = k;
			while (text[e] && !is_sep(text[e]))
				e++;
			if (e - k == namelen && !_wcsnicmp(text + k, name, namelen))
				group_add(&groups, &ngroups, &capacity, i, e, score);
			if (!text[e])
				break;
			k = e + 1;
		}
	}

	cand = malloc(sizeof(ANT_DIR_CAND) * (ngroups ? ngroups : 1));
	if (!cand) {
		free(groups);
		return 0;
	}
	for (int g = 0; g < ngroups; g++) {
		ANT_DIR_CAND *c = &cand[n];

		if (groups[g].len + restlen >= ANT_DIRS_PATH)
			continue;
		wmemcpy(c->path, table.item[groups[g].item].text, groups[g].len);
		wcscpy(c->path + groups[g].len, rest);
		if (!is_dir(c->path))
			continue;
		c->prefix = groups[g].len;
		c->score = groups[g].score;
		n++;
	}
	free(groups);
	if (n == 0) {
		free(cand);
		return 0;
	}
	qsort(cand, n, sizeof *cand, by_score);
	*out = cand;
	return n;
}

/* the best folder for 'spec' in out, and how many there are (count may be NULL) */
bool ant_dirs_best (const wchar_t *spec, wchar_t *out, int max, int *count) {

	ANT_DIR_CAND *cand;
	int n = ant_dirs_resolve(spec, &cand);
	bool ok = n > 0 && (int) wcslen(cand[0].path) < max;

	if (ok)
		wcscpy(out, cand[0].path);
	if (count)
		*count = n;
	free(cand);
	return ok;
}

/*
 * The user chose 'path' among the folders 'spec' can mean: the folder named by its first
 * component gets enough points to win from now on (an explicit choice is worth more than
 * the point the cd will add).
 */
void ant_dirs_choose (const wchar_t *spec, const wchar_t *path) {

	wchar_t file[MAX_PATH], prefix[ANT_DIRS_PATH];
	ANT_DIR_CAND *cand;
	int n = ant_dirs_resolve(spec, &cand), chosen = -1, len = wcslen(path);
	double top = 0;

	while (len > 3 && is_sep(path[len - 1]))
		len--;
	for (int i = 0; i < n; i++)
		if ((int) wcslen(cand[i].path) == len && !_wcsnicmp(cand[i].path, path, len))
			chosen = i;
		else if (cand[i].score > top)
			top = cand[i].score;
	if (chosen >= 0 && cand[chosen].score <= top && table_path(file, MAX_PATH) && table_ready()) {
		/* the folder is stamped now, which weighs its points by 4 */
		double points = (top - cand[chosen].score) / 4 + 1;

		wmemcpy(prefix, cand[chosen].path, cand[chosen].prefix);
		prefix[cand[chosen].prefix] = 0;
		ant_rank_add(&table, file, prefix, points);
		table_loaded = false;
	}
	free(cand);
}

int ant_dirs_count (void) {

	table_sync();
	return table.count;
}

/* the folders in the order of the file; the score is for now */
const wchar_t *ant_dirs_get (int i, double *score) {

	if (i < 0 || i >= table.count)
		return NULL;
	if (score)
		*score = ant_rank_score(&table, i, time(NULL));
	return table.item[i].text;
}

/* takes a folder out of the table; false if it was not there */
bool ant_dirs_remove (const wchar_t *dir) {

	wchar_t file[MAX_PATH], copy[ANT_DIRS_PATH];
	int i, n = wcslen(dir);

	if (n >= ANT_DIRS_PATH)
		return false;
	wcscpy(copy, dir);
	while (n > 3 && is_sep(copy[n - 1]))
		copy[--n] = 0;
	table_sync();
	if (!table_path(file, MAX_PATH) || (i = ant_rank_find(&table, copy)) < 0)
		return false;
	ant_rank_remove(&table, i);
	ant_rank_save(&table, file);
	table_loaded = false;
	return true;
}

void ant_dirs_clear (void) {

	wchar_t file[MAX_PATH];

	if (table_path(file, MAX_PATH))
		DeleteFileW(file);
	if (table.item)
		ant_rank_clear(&table);
	table_loaded = false;
}
