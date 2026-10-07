#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "complete.h"

/*************************************************************
 * File name completion for TAB.                             *
 *                                                           *
 * Follows fish (src/reader/reader.rs, complete.rs):         *
 *   - completes the word before the cursor, relative to     *
 *     the typed path (src\mo -> looks for mo* in src\);     *
 *   - match categories like StringFuzzyMatch::rank:         *
 *     prefix < substring < subsequence, and within each,    *
 *     same case < different case; an all-lowercase word     *
 *     ignores case ("smart case");                          *
 *   - only the options of the best category are kept;       *
 *   - a directory ends with the separator, so the next      *
 *     TAB continues inside it.                              *
 *************************************************************/

/* rank = (kind << 2) + case, as in fish */
enum { MATCH_PREFIX = 1, MATCH_SUBSTR = 2, MATCH_SUBSEQ = 3 };
enum { CASE_SAME = 0, CASE_FOLD = 2 };

#define RANK(t, c) (((t) << 2) + (c))
#define NO_MATCH   0x7FFF

static bool is_sep (wchar_t c) {

	return c == L'\\' || c == L'/';
}

static bool same_char (wchar_t a, wchar_t b, bool fold) {

	return fold ? towlower(a) == towlower(b) : a == b;
}

static bool starts_with (const wchar_t *s, const wchar_t *p, bool fold) {

	for (; *p; s++, p++)
		if (!*s || !same_char(*s, *p, fold))
			return false;
	return true;
}

static bool contains (const wchar_t *s, const wchar_t *p, bool fold) {

	for (; *s; s++)
		if (starts_with(s, p, fold))
			return true;
	return false;
}

static bool subsequence (const wchar_t *s, const wchar_t *p, bool fold) {

	for (; *s && *p; s++)
		if (same_char(*s, *p, fold))
			p++;
	return *p == 0;
}

static bool has_upper (const wchar_t *s) {

	for (; *s; s++)
		if (iswupper(*s))
			return true;
	return false;
}

/* best category in which 'name' matches 'word' */
static int match_rank (const wchar_t *name, const wchar_t *word) {

	int fold = has_upper(word) ? CASE_FOLD : CASE_SAME;

	if (!*word || starts_with(name, word, false))
		return RANK(MATCH_PREFIX, CASE_SAME);
	if (starts_with(name, word, true))
		return RANK(MATCH_PREFIX, fold);
	if (contains(name, word, false))
		return RANK(MATCH_SUBSTR, CASE_SAME);
	if (contains(name, word, true))
		return RANK(MATCH_SUBSTR, fold);
	if (subsequence(name, word, false))
		return RANK(MATCH_SUBSEQ, CASE_SAME);
	if (subsequence(name, word, true))
		return RANK(MATCH_SUBSEQ, fold);
	return NO_MATCH;
}

/* natural order ignoring case: foo2 before foo10; a directory's trailing separator does not count */
static int natural_cmp (const ANT_COMPLETION *x, const ANT_COMPLETION *y) {

	const wchar_t *a = x->name, *b = y->name;
	const wchar_t *ea = a + wcslen(a) - (x->dir ? 1 : 0);
	const wchar_t *eb = b + wcslen(b) - (y->dir ? 1 : 0);

	while (a < ea && b < eb) {
		if (iswdigit(*a) && iswdigit(*b)) {
			int na = 0, nb = 0;
			while (a < ea && *a == L'0')
				a++;
			while (b < eb && *b == L'0')
				b++;
			while (a + na < ea && iswdigit(a[na]))
				na++;
			while (b + nb < eb && iswdigit(b[nb]))
				nb++;
			if (na != nb)
				return na - nb;
			for (int i = 0; i < na; i++)
				if (a[i] != b[i])
					return a[i] - b[i];
			a += na;
			b += nb;
			continue;
		}
		if (towlower(*a) != towlower(*b))
			return (int) towlower(*a) - (int) towlower(*b);
		a++;
		b++;
	}
	if ((ea - a) != (eb - b))
		return (int) (ea - a) - (int) (eb - b);
	return wcscmp(x->name, y->name);
}

/* by name; the same name in several groups: the better group first */
static int compare_names (const void *x, const void *y) {

	int d = natural_cmp((const ANT_COMPLETION *) x, (const ANT_COMPLETION *) y);

	return d ? d : ((const ANT_COMPLETION *) x)->group - ((const ANT_COMPLETION *) y)->group;
}

/* by group, then by name */
static int compare_items (const void *x, const void *y) {

	int d = ((const ANT_COMPLETION *) x)->group - ((const ANT_COMPLETION *) y)->group;

	return d ? d : natural_cmp((const ANT_COMPLETION *) x, (const ANT_COMPLETION *) y);
}

/* group given to the items added from now on */
static int add_group = GROUP_FILE;

static bool add_item (ANT_COMPLETE *c, int *capacity, const wchar_t *name, bool dir) {

	int n = wcslen(name);
	wchar_t *copy = malloc(sizeof(wchar_t) * (n + 2));

	if (!copy)
		return false;
	if (c->count == *capacity) {
		int grow = *capacity ? *capacity * 2 : 64;
		ANT_COMPLETION *items = realloc(c->items, sizeof(ANT_COMPLETION) * grow);
		if (!items) {
			free(copy);
			return false;
		}
		c->items = items;
		*capacity = grow;
	}
	wcscpy(copy, name);
	if (dir) {
		copy[n] = c->sep;
		copy[n+1] = 0;
	}
	c->items[c->count].name = copy;
	c->items[c->count].dir = dir;
	c->items[c->count].group = add_group;
	c->count++;
	return true;
}

static void drop_items (ANT_COMPLETE *c) {

	for (int i = 0; i < c->count; i++)
		free(c->items[i].name);
	c->count = 0;
}

/* first-word commands: built-ins and folders to search for .exe files (besides the current one) */
static const wchar_t *const *command_builtins = NULL;
static wchar_t *command_user = NULL; /* [path] folders from config.ant, separated by ';' */
static wchar_t *command_path = NULL; /* PATH folders, separated by ';' */

void ant_complete_set_commands (const wchar_t *const *builtins, const wchar_t *user, const wchar_t *path) {

	command_builtins = builtins;
	free(command_user);
	command_user = user ? _wcsdup(user) : NULL;
	free(command_path);
	command_path = path ? _wcsdup(path) : NULL;
}

/* goes into the list if it matches in the best category so far */
static void add_match (ANT_COMPLETE *c, int *capacity, const wchar_t *name, bool dir) {

	int rank = match_rank(name, c->word);

	if (rank == NO_MATCH || rank > c->rank)
		return;
	if (rank < c->rank) {
		drop_items(c);
		c->rank = rank;
	}
	add_item(c, capacity, name, dir);
}

/* files and folders in c->dir */
static void search_files (ANT_COMPLETE *c, int *capacity, bool dirs_only) {

	wchar_t pattern[ANT_COMPLETE_PATH + 2];
	WIN32_FIND_DATAW fd;
	HANDLE find;
	bool dot = (c->word[0] == L'.');

	swprintf(pattern, ANT_COMPLETE_PATH + 2, L"%ls*", c->dir);
	find = FindFirstFileW(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;

	do {
		const wchar_t *name = fd.cFileName;

		if (!wcscmp(name, L"."))
			continue;
		if (dirs_only && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		/* hidden names only show up if the word starts with '.' (the ones with the attribute, if there is a word) */
		if (name[0] == L'.' && !dot)
			continue;
		if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) && !c->word[0])
			continue;
		add_match(c, capacity, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}

/* .exe programs in a folder ("" = current), without the extension */
static void search_exes (ANT_COMPLETE *c, int *capacity, const wchar_t *dir, int dlen) {

	wchar_t pattern[ANT_COMPLETE_PATH + 8], name[MAX_PATH];
	WIN32_FIND_DATAW fd;
	HANDLE find;

	if (dlen + 8 > ANT_COMPLETE_PATH)
		return;
	wmemcpy(pattern, dir, dlen);
	if (dlen > 0 && !is_sep(pattern[dlen - 1]))
		pattern[dlen++] = L'\\';
	wcscpy(pattern + dlen, L"*.exe");

	find = FindFirstFileW(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;

	do {
		int n = wcslen(fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		/* the *.exe pattern also matches 8.3 short names; check the real extension */
		if (n <= 4 || _wcsicmp(fd.cFileName + n - 4, L".exe"))
			continue;
		wcsncpy(name, fd.cFileName, n - 4);
		name[n - 4] = 0;
		add_match(c, capacity, name, false);
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}

/* .exe files of the folders in a list separated by ';' */
static void search_list (ANT_COMPLETE *c, int *capacity, const wchar_t *list) {

	for (const wchar_t *p = list; p && *p; ) {
		const wchar_t *end = wcschr(p, L';');
		int n = end ? (int) (end - p) : (int) wcslen(p);
		/* ignores spaces and quotes at the ends, common in PATH */
		while (n > 0 && (*p == L' ' || *p == L'"')) {
			p++;
			n--;
		}
		while (n > 0 && (p[n - 1] == L' ' || p[n - 1] == L'"'))
			n--;
		if (n > 0)
			search_exes(c, capacity, p, n);
		if (!end)
			break;
		p = end + 1;
	}
}

static bool has_command (const ANT_COMPLETE *c) {

	for (int i = 0; i < c->count; i++)
		if (c->items[i].group != GROUP_FOLDER)
			return true;
	return false;
}

/*
 * first word: built-ins and aliases, then the folders of the current one (a
 * folder alone on the line is a cd), its .exe files, the [path] of config.ant
 * and, last, the PATH (System32 and the like)
 */
static void search_commands (ANT_COMPLETE *c, int *capacity) {

	add_group = GROUP_BUILTIN;
	if (command_builtins)
		for (int i = 0; command_builtins[i]; i++)
			add_match(c, capacity, command_builtins[i], false);

	add_group = GROUP_FOLDER;
	search_files(c, capacity, true);

	add_group = GROUP_CURRENT;
	search_exes(c, capacity, L"", 0);

	add_group = GROUP_USER;
	search_list(c, capacity, command_user);

	add_group = GROUP_SYSTEM;
	search_list(c, capacity, command_path);

	add_group = GROUP_FILE;
}

/* sorts, drops repeated names (the same command in several folders) and computes the common prefix */
static int finish (ANT_COMPLETE *c) {

	int k = 0;

	if (c->count == 0)
		return 0;

	qsort(c->items, c->count, sizeof(ANT_COMPLETION), compare_names);

	for (int i = 0; i < c->count; i++) {
		if (k > 0 && !_wcsicmp(c->items[k - 1].name, c->items[i].name)) {
			free(c->items[i].name);
			continue;
		}
		c->items[k++] = c->items[i];
	}
	c->count = k;
	qsort(c->items, c->count, sizeof(ANT_COMPLETION), compare_items);

	c->prefix = wcslen(c->items[0].name);
	for (int i = 1; i < c->count && c->prefix > 0; i++) {
		int j = 0;
		while (j < c->prefix && c->items[i].name[j] == c->items[0].name[j])
			j++;
		c->prefix = j;
	}

	return c->count;
}

/*
 * Completes the word that ends at line[cursor]: fills 'c' and returns the
 * number of options. The options are sorted and 'c->prefix' holds the
 * length of the prefix common to all of them.
 *
 * In command position (start of the line or after |, ;, &&, || and &), with
 * no path and not empty, completes commands (as fish does); if none matches,
 * falls back to files. An empty word still lists the folder. An '@' in front
 * (open without waiting) stays out of the search and comes back in the
 * inserted text.
 */
int ant_complete (const wchar_t *line, int cursor, ANT_COMPLETE *c) {

	wchar_t raw[ANT_COMPLETE_PATH], quote = 0;
	bool command = true;
	int beg = 0, n = 0, cut = 0, capacity = 0;

	memset(c, 0, sizeof *c);
	c->sep = L'\\';
	c->rank = NO_MATCH;

	/* start of the word: after the last space or operator outside quotes (like parser.c) */
	for (int i = 0; i < cursor; i++) {
		if (quote) {
			if (line[i] == quote)
				quote = 0;
		} else if (line[i] == L'"' || line[i] == L'\'')
			quote = line[i];
		else if (line[i] == L' ' || line[i] == L'\t' || wcschr(L"|&;<>", line[i]))
			beg = i + 1;
	}
	c->beg = beg;
	c->end = cursor;

	/* command position: nothing before, or the last character before is | & ; (after < and > it is a file) */
	for (int i = beg - 1; i >= 0; i--)
		if (line[i] != L' ' && line[i] != L'\t') {
			command = wcschr(L"|&;", line[i]) != NULL;
			break;
		}

	if (cursor - beg >= ANT_COMPLETE_PATH - 1)
		return 0;

	for (int i = beg; i < cursor; i++) {
		if (line[i] == L'"' || line[i] == L'\'')
			c->quoted = true;
		else
			raw[n++] = line[i];
	}
	raw[n] = 0;

	/* @prog: completes what comes after the @ */
	if (command && raw[0] == L'@') {
		c->lead = L'@';
		memmove(raw, raw + 1, sizeof(wchar_t) * n);
		n--;
	}

	/* a typed wildcard must not become a FindFirstFile pattern */
	if (wcspbrk(raw, L"*?"))
		return 0;

	/* path up to the last \, / or : (C:) and the partial name after it */
	for (int i = 0; i < n; i++)
		if (is_sep(raw[i]) || raw[i] == L':')
			cut = i + 1;
	wcsncpy(c->dir, raw, cut);
	c->dir[cut] = 0;
	wcscpy(c->word, raw + cut);
	for (int i = cut - 1; i >= 0; i--)
		if (is_sep(raw[i])) {
			c->sep = raw[i];
			break;
		}

	if (command && cut == 0 && c->word[0]) {
		search_commands(c, &capacity);
		/* folders alone do not count: with no command, the files come back */
		if (finish(c) > 0 && has_command(c))
			return c->count;
		drop_items(c);
		c->rank = NO_MATCH;
	}

	search_files(c, &capacity, false);
	return finish(c);
}

/* the options matched at the start of the name (the typed prefix can be highlighted) */
bool ant_complete_is_prefix (const ANT_COMPLETE *c) {

	return c->rank < RANK(MATCH_SUBSTR, CASE_SAME);
}

/* characters that need quotes (the shell's, and the ones cmd also quoted) */
static bool needs_quotes (const wchar_t *s, int n) {

	for (int i = 0; i < n; i++)
		if (wcschr(L" !%&(){}[]=';`,^~+$", s[i]))
			return true;
	return false;
}

/*
 * the quote for a completed path: $ expands even inside double quotes (expand.c), so a
 * name with $ goes inside single quotes, unless it also has a ' (there is no escape)
 */
static wchar_t quote_for (const ANT_COMPLETE *c, const wchar_t *name, int nlen) {

	bool dollar = wcschr(c->dir, L'$') || wmemchr(name, L'$', nlen);
	bool apostrophe = wcschr(c->dir, L'\'') || wmemchr(name, L'\'', nlen);

	return (dollar && !apostrophe) ? L'\'' : L'"';
}

/*
 * Text that replaces line[c->beg..c->end): path + name of item 'item'
 * (or the common prefix, if item < 0), with quotes when needed.
 * Returns the length in UTF-16 units.
 */
int ant_complete_text (const ANT_COMPLETE *c, int item, int mode, wchar_t *out, int max) {

	const wchar_t *name = c->items[item < 0 ? 0 : item].name;
	int nlen = (item < 0) ? c->prefix : (int) wcslen(name);
	int dlen = wcslen(c->dir);
	bool dir = (item >= 0 && c->items[item].dir);
	bool quote = c->quoted || needs_quotes(c->dir, dlen) || needs_quotes(name, nlen);
	wchar_t q = quote_for(c, name, nlen);
	int k = 0;

	if (dlen + nlen + 5 > max)
		return 0;

	if (c->lead)
		out[k++] = c->lead;
	if (quote)
		out[k++] = q;
	wmemcpy(out + k, c->dir, dlen);
	k += dlen;
	wmemcpy(out + k, name, nlen);
	k += nlen;
	if (quote && mode != ANT_APPLY_PREFIX)
		out[k++] = q;
	if (mode == ANT_APPLY_UNIQUE && !dir)
		out[k++] = L' ';
	out[k] = 0;
	return k;
}

void ant_complete_free (ANT_COMPLETE *c) {

	drop_items(c);
	free(c->items);
	c->items = NULL;
}
