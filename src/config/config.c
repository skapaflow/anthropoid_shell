#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#include "config.h"

/*************************************************************
 * data\config.ant, next to antshell.exe, in sections:           *
 *                                                           *
 *   # comment (only at the start of a line)                 *
 *   [path]     one folder per line (searched before PATH)   *
 *   [export]   NAME = value                                 *
 *   [alias]    name = command                               *
 *   [link]     name = target                                *
 *                                                           *
 * Values: "quotes" keep spaces at the ends; $VAR and        *
 * ${VAR} become the variable's value (except in [alias]     *
 * and inside 'single quotes'). A repeated name: the first   *
 * one wins. The shell reads the file again when it          *
 * changes. The first time, without config.ant, it writes   *
 * one from a template (ant_config_template).                *
 *************************************************************/

#define VALUE_MAX 32768 /* largest value of an environment variable */
#define VAR_NAME_MAX  256

typedef struct {
	int section;
	wchar_t *name;  /* NULL in [path] */
	wchar_t *value;
	bool literal;   /* inside single quotes: no $VAR */
} ENTRY;

/* the value the variable had before [export] (NULL: it did not exist) */
typedef struct {
	wchar_t *name;
	wchar_t *old;
} SAVED;

static ENTRY  *entries;
static int     count;
static SAVED  *saved;
static int     nsaved;
static wchar_t warnings[4096];
static bool    created;

static const wchar_t *section_names[CONFIG_SECTIONS] = { L"path", L"export", L"alias", L"link" };

static bool blank (wchar_t c) {

	return c == L' ' || c == L'\t';
}

static wchar_t *dup_n (const wchar_t *s, int n) {

	wchar_t *d = malloc(sizeof(wchar_t) * (n + 1));

	if (d) {
		wmemcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

/* warning "config.ant, line N: ..." with 'what' (up to 200 characters) in place of the %ls */
static void warn (int line, const wchar_t *fmt, const wchar_t *what, int n) {

	wchar_t text[600], arg[201];
	int k;

	if (n > 200)
		n = 200;
	wmemcpy(arg, what, n);
	arg[n] = 0;
	k = swprintf(text, 600, L"config.ant, line %d: ", line);
	swprintf(text + k, 600 - k, fmt, arg);
	if (wcslen(warnings) + wcslen(text) + 2 < sizeof warnings / sizeof *warnings) {
		wcscat(warnings, text);
		wcscat(warnings, L"\n");
	}
}

static void entries_free (void) {

	for (int i = 0; i < count; i++) {
		free(entries[i].name);
		free(entries[i].value);
	}
	free(entries);
	entries = NULL;
	count = 0;
}

static bool add (int section, const wchar_t *name, int nlen, const wchar_t *value, int vlen, bool literal) {

	ENTRY *e;

	if (count % 16 == 0) {
		ENTRY *grown = realloc(entries, sizeof(ENTRY) * (count + 16));
		if (!grown)
			return false;
		entries = grown;
	}
	e = &entries[count];
	e->section = section;
	e->name = name ? dup_n(name, nlen) : NULL;
	e->value = dup_n(value, vlen);
	e->literal = literal;
	if (!e->value || (name && !e->name)) {
		free(e->name);
		free(e->value);
		return false;
	}
	count++;
	return true;
}

/* first entry of the section with this name (ignoring case), or -1 */
static int find (int section, const wchar_t *name, int n) {

	for (int i = 0; i < count; i++)
		if (entries[i].section == section && (int) wcslen(entries[i].name) == n && !_wcsnicmp(entries[i].name, name, n))
			return i;
	return -1;
}

/* position of the i-th entry of the section, or -1 */
static int nth (int section, int i) {

	for (int k = 0; k < count; k++)
		if (entries[k].section == section && i-- == 0)
			return k;
	return -1;
}

/* "value" and 'value' lose their quotes; single quotes turn $VAR off */
static void unquote (const wchar_t **s, int *n, bool *literal) {

	*literal = false;
	if (*n >= 2 && ((*s)[0] == L'\'' || (*s)[0] == L'"') && (*s)[*n - 1] == (*s)[0]) {
		*literal = ((*s)[0] == L'\'');
		(*s)++;
		*n -= 2;
	}
}

static bool valid_name (int section, const wchar_t *s, int n) {

	/* link: one word with no path (name\ and name. are modes); alias: no operators */
	const wchar_t *bad = (section == CONFIG_LINK) ? L" \t\\/:\"'" :
		(section == CONFIG_ALIAS) ? L" \t|&;<>\"'" : L" \t\"'";

	if (n == 0)
		return false;
	for (int i = 0; i < n; i++)
		if (wcschr(bad, s[i]))
			return false;
	return true;
}

/* one "name = value" line of the section */
static void parse_pair (int section, int line, const wchar_t *s, int n) {

	const wchar_t *eq = wmemchr(s, L'=', n), *name = s, *value;
	int nn, vn;
	bool literal;

	if (!eq) {
		warn(line, L"missing '=' in \"%ls\"", s, n);
		return;
	}
	nn = (int) (eq - s);
	while (nn > 0 && blank(name[nn - 1]))
		nn--;
	value = eq + 1;
	vn = n - (int) (value - s);
	while (vn > 0 && blank(*value)) {
		value++;
		vn--;
	}

	if (!valid_name(section, name, nn)) {
		warn(line, L"invalid name \"%ls\"", name, nn);
		return;
	}
	if (find(section, name, nn) >= 0) {
		warn(line, L"%ls repeated; the first one wins", name, nn);
		return;
	}
	unquote(&value, &vn, &literal);
	if (vn == 0 && section != CONFIG_EXPORT) {
		warn(line, L"%ls has no value", name, nn);
		return;
	}
	add(section, name, nn, value, vn, literal);
}

/* replaces the configuration in memory with the text (NULL: empty); warnings in ant_config_warnings */
void ant_config_parse (const wchar_t *text) {

	int section = -1, line = 0; /* -1: before the first section; -2: unknown section */

	entries_free();
	warnings[0] = 0;
	if (!text)
		return;
	if (*text == 0xFEFF)
		text++;

	while (*text) {
		const wchar_t *s = text, *end = wcschr(text, L'\n');
		int n = end ? (int) (end - text) : (int) wcslen(text);

		text += n + (end ? 1 : 0);
		line++;
		while (n > 0 && (blank(s[n - 1]) || s[n - 1] == L'\r'))
			n--;
		while (n > 0 && blank(*s)) {
			s++;
			n--;
		}
		if (n == 0 || *s == L'#')
			continue;

		if (*s == L'[' && s[n - 1] == L']') {
			section = -2;
			for (int k = 0; k < CONFIG_SECTIONS; k++)
				if ((int) wcslen(section_names[k]) == n - 2 && !_wcsnicmp(s + 1, section_names[k], n - 2))
					section = k;
			if (section == -2)
				warn(line, L"unknown section %ls (use [path], [export], [alias] or [link])", s, n);
			continue;
		}

		if (section == -1)
			warn(line, L"\"%ls\" outside a section", s, n);
		else if (section == CONFIG_PATH) {
			bool literal;
			unquote(&s, &n, &literal);
			if (n > 0)
				add(CONFIG_PATH, NULL, 0, s, n, literal);
		} else if (section >= 0)
			parse_pair(section, line, s, n);
	}
}

const wchar_t *ant_config_warnings (void) {

	return warnings;
}

static bool name_char (wchar_t c, bool first) {

	return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_' || (!first && c >= L'0' && c <= L'9');
}

/*
 * $NAME and ${NAME} become the value of the environment variable (empty if it
 * does not exist); a $ that does not start a name stays as it is. Returns the
 * length of out.
 */
int ant_config_expand (const wchar_t *in, wchar_t *out, int max) {

	static wchar_t val[VALUE_MAX];
	int n = 0;

	for (const wchar_t *s = in; *s; ) {
		const wchar_t *name = NULL;
		int nlen = 0, skip = 0;

		if (s[0] == L'$' && s[1] == L'{') {
			const wchar_t *close = wcschr(s + 2, L'}');
			if (close) {
				name = s + 2;
				nlen = (int) (close - name);
				skip = nlen + 3;
			}
		} else if (s[0] == L'$' && name_char(s[1], true)) {
			name = s + 1;
			while (name_char(name[nlen], false))
				nlen++;
			skip = nlen + 1;
		}

		if (name && nlen > 0 && nlen < VAR_NAME_MAX) {
			wchar_t var[VAR_NAME_MAX];
			DWORD m;
			wmemcpy(var, name, nlen);
			var[nlen] = 0;
			m = GetEnvironmentVariableW(var, val, VALUE_MAX);
			for (DWORD k = 0; k < m && m < VALUE_MAX && n < max - 1; k++)
				out[n++] = val[k];
			s += skip;
			continue;
		}
		if (n < max - 1)
			out[n++] = *s;
		s++;
	}
	out[n] = 0;
	return n;
}

static bool value_of (int k, wchar_t *out, int max) {

	if (k < 0)
		return false;
	if (entries[k].literal) {
		wcsncpy(out, entries[k].value, max - 1);
		out[max - 1] = 0;
	} else
		ant_config_expand(entries[k].value, out, max);
	return true;
}

int ant_config_count (int section) {

	int n = 0;

	for (int i = 0; i < count; i++)
		if (entries[i].section == section)
			n++;
	return n;
}

const wchar_t *ant_config_name (int section, int i) {

	int k = nth(section, i);
	return k < 0 ? NULL : entries[k].name;
}

/* value as it is in the file (without quotes, not expanded) */
const wchar_t *ant_config_raw (int section, int i) {

	int k = nth(section, i);
	return k < 0 ? NULL : entries[k].value;
}

/* value with $VAR expanded */
bool ant_config_value (int section, int i, wchar_t *out, int max) {

	return value_of(nth(section, i), out, max);
}

const wchar_t *ant_config_alias (const wchar_t *name) {

	int k = find(CONFIG_ALIAS, name, wcslen(name));
	return k < 0 ? NULL : entries[k].value;
}

bool ant_config_link (const wchar_t *name, wchar_t *out, int max) {

	return value_of(find(CONFIG_LINK, name, wcslen(name)), out, max);
}

/* [path] folders separated by ';', like PATH; returns the length */
int ant_config_path (wchar_t *out, int max) {

	static wchar_t dir[VALUE_MAX];
	int len = 0;

	out[0] = 0;
	for (int i = 0; ant_config_value(CONFIG_PATH, i, dir, VALUE_MAX); i++) {
		int n = wcslen(dir);
		if (n == 0 || len + n + 2 > max)
			continue;
		if (len > 0)
			out[len++] = L';';
		wmemcpy(out + len, dir, n);
		len += n;
		out[len] = 0;
	}
	return len;
}

/* ---------- [export] ---------- */

/* puts the variables back to what they were before the last [export] */
static void exports_restore (void) {

	for (int i = nsaved - 1; i >= 0; i--) {
		SetEnvironmentVariableW(saved[i].name, saved[i].old);
		free(saved[i].name);
		free(saved[i].old);
	}
	free(saved);
	saved = NULL;
	nsaved = 0;
}

/* applies [export] in file order (a value can use a variable defined above it) */
static void exports_apply (void) {

	static wchar_t value[VALUE_MAX];

	for (int k = 0; k < count; k++) {
		SAVED *grown;
		DWORD m;

		if (entries[k].section != CONFIG_EXPORT)
			continue;
		if (!(grown = realloc(saved, sizeof(SAVED) * (nsaved + 1))))
			return;
		saved = grown;
		saved[nsaved].name = _wcsdup(entries[k].name);
		saved[nsaved].old = NULL;
		m = GetEnvironmentVariableW(entries[k].name, NULL, 0);
		if (m && (saved[nsaved].old = malloc(sizeof(wchar_t) * m)))
			GetEnvironmentVariableW(entries[k].name, saved[nsaved].old, m);
		if (!saved[nsaved].name) {
			free(saved[nsaved].old);
			return;
		}
		nsaved++;
		value_of(k, value, VALUE_MAX);
		SetEnvironmentVariableW(entries[k].name, value);
	}
}

/* ---------- files ---------- */

/* data\<name> next to antshell.exe */
static bool data_file (const wchar_t *name, wchar_t *out, int max) {

	int n = GetModuleFileNameW(NULL, out, max);

	if (n <= 0 || n >= max)
		return false;
	while (n > 0 && out[n - 1] != L'\\')
		n--;
	if (n == 0 || n + 6 + (int) wcslen(name) >= max)
		return false;
	wcscpy(out + n, L"data\\");
	wcscat(out, name);
	return true;
}

bool ant_config_file (wchar_t *out, int max) {

	return data_file(L"config.ant", out, max);
}

/* the whole file in UTF-16: UTF-8, or ANSI if it is not valid UTF-8; NULL if it did not open */
static wchar_t *read_text (const wchar_t *path) {

	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD size, got = 0;
	UINT cp = CP_UTF8;
	DWORD flags = MB_ERR_INVALID_CHARS;
	char *bytes;
	wchar_t *text = NULL;
	int n = 0;

	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	size = GetFileSize(h, NULL);
	if (size == INVALID_FILE_SIZE || size > (1 << 24) || !(bytes = malloc(size + 1))) {
		CloseHandle(h);
		return NULL;
	}
	if (!ReadFile(h, bytes, size, &got, NULL))
		got = 0;
	CloseHandle(h);

	if (got) {
		n = MultiByteToWideChar(cp, flags, bytes, got, NULL, 0);
		if (n == 0) {
			cp = CP_ACP;
			flags = 0;
			n = MultiByteToWideChar(cp, flags, bytes, got, NULL, 0);
		}
	}
	if ((text = malloc(sizeof(wchar_t) * (n + 1)))) {
		if (n)
			MultiByteToWideChar(cp, flags, bytes, got, text, n);
		text[n] = 0;
	}
	free(bytes);
	return text;
}

/* writes in UTF-8; does not overwrite a file that already exists */
static bool write_new (const wchar_t *path, const wchar_t *text) {

	int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
	char *bytes = n > 0 ? malloc(n) : NULL;
	HANDLE h;
	DWORD written = 0;
	bool ok;

	if (!bytes)
		return false;
	WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, n, NULL, NULL);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	ok = (h != INVALID_HANDLE_VALUE);
	if (ok) {
		ok = WriteFile(h, bytes, n - 1, &written, NULL) && written == (DWORD) (n - 1);
		CloseHandle(h);
	}
	free(bytes);
	return ok;
}

/* ---------- the template of a new config.ant ---------- */

/*
 * Text of a new config.ant. bin_dir (may be NULL) goes in [path], so the
 * programs of bin\ are found from the start. NULL if there is no memory.
 */
wchar_t *ant_config_template (const wchar_t *bin_dir) {

	enum { SIZE = 4096 };
	wchar_t *text = malloc(sizeof(wchar_t) * SIZE);

	if (!text)
		return NULL;
	swprintf(text, SIZE,
		L"# AntShell configuration. antshell.exe reads this file again whenever it changes.\r\n"
		L"#\r\n"
		L"#   [path]    folders searched for programs (before PATH)\r\n"
		L"#   [export]  environment variables:  NAME = value\r\n"
		L"#   [alias]   command aliases:        name = command\r\n"
		L"#   [link]    folder/file shortcuts:  name = target\r\n"
		L"#             (name opens it, name\\ enters the folder, name. opens it and closes the shell)\r\n"
		L"#\r\n"
		L"# $VAR and ${VAR} become the variable's value (except in [alias] and inside 'single quotes');\r\n"
		L"# \"quotes\" keep spaces at the start and at the end. Comments only at the start of a line.\r\n"
		L"\r\n[path]\r\n%ls%ls"
		L"\r\n[export]\r\n# EDITOR = notepad\r\n"
		L"\r\n[alias]\r\n# ll = ls -l\r\n"
		L"\r\n[link]\r\n# docs = $USERPROFILE\\Documents\r\n",
		bin_dir ? bin_dir : L"", bin_dir ? L"\r\n" : L"");
	return text;
}

/* ---------- loading ---------- */

/* runs `command "file"` (the command may have arguments) and waits for it; false if it did not start */
static bool run_editor (const wchar_t *command, const wchar_t *file) {

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	wchar_t cmd[MAX_PATH * 2 + 64];

	if (swprintf(cmd, sizeof cmd / sizeof *cmd, L"%ls \"%ls\"", command, file) < 0)
		return false;
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		return false;
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

/* note.exe: bin\ next to antshell.exe, then PATH */
static bool find_note (wchar_t *out, int max) {

	int n = GetModuleFileNameW(NULL, out, max);

	if (n > 0 && n < max) {
		while (n > 0 && out[n - 1] != L'\\')
			n--;
	} else
		n = 0;
	if (n > 0 && n + 12 < max) {
		wcscpy(out + n, L"bin\\note.exe");
		if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
			return true;
	}
	return SearchPathW(NULL, L"note.exe", NULL, max, out, NULL) > 0;
}

/*
 * Opens config.ant: in %EDITOR% (if it is set and starts), then in note.exe
 * (runs in this console and the shell waits), then in the .txt editor (its own
 * window, without waiting: the .ant extension has no associated program).
 */
bool ant_config_open (void) {

	SHELLEXECUTEINFOW sei;
	wchar_t path[MAX_PATH], editor[MAX_PATH], note[MAX_PATH], quoted[MAX_PATH + 2];
	DWORD n;

	if (!ant_config_file(path, MAX_PATH))
		return false;

	n = GetEnvironmentVariableW(L"EDITOR", editor, MAX_PATH);
	if (n > 0 && n < MAX_PATH && run_editor(editor, path))
		return true;
	if (find_note(note, MAX_PATH)) {
		swprintf(quoted, sizeof quoted / sizeof *quoted, L"\"%ls\"", note);
		if (run_editor(quoted, path))
			return true;
	}

	memset(&sei, 0, sizeof sei);
	sei.cbSize = sizeof sei;
	sei.fMask = SEE_MASK_CLASSNAME;
	sei.lpClass = L".txt";
	sei.lpVerb = L"open";
	sei.lpFile = path;
	sei.nShow = SW_SHOW;
	return ShellExecuteExW(&sei);
}

/* did the last ant_config_load create config.ant? */
bool ant_config_created (void) {

	return created;
}

/* no config.ant: creates it from the template, with the bin\ folder next to antshell.exe in [path] */
static void create_file (const wchar_t *path) {

	wchar_t data[MAX_PATH], bin[MAX_PATH];
	wchar_t *slash, *text;
	bool have_bin = false;

	wcsncpy(data, path, MAX_PATH - 1);
	data[MAX_PATH - 1] = 0;
	if ((slash = wcsrchr(data, L'\\')))
		*slash = 0;                             /* the data\ folder */
	CreateDirectoryW(data, NULL);

	wcscpy(bin, data);
	if ((slash = wcsrchr(bin, L'\\')) && (slash - bin) + 4 < MAX_PATH) {
		wcscpy(slash + 1, L"bin");              /* bin\ is next to data\ */
		have_bin = GetFileAttributesW(bin) != INVALID_FILE_ATTRIBUTES;
	}

	text = ant_config_template(have_bin ? bin : NULL);
	created = text && write_new(path, text);
	free(text);
}

/*
 * Reads data\config.ant if it changed since the last time (or if this is the
 * first time). Returns true when the configuration changed (warnings in
 * ant_config_warnings).
 */
bool ant_config_load (void) {

	static bool loaded = false;
	static FILETIME stamp;
	static DWORD size;
	WIN32_FILE_ATTRIBUTE_DATA fa;
	wchar_t path[MAX_PATH], *text;

	created = false;
	if (!ant_config_file(path, MAX_PATH))
		return false;

	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
		if (!loaded)
			create_file(path);
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
			/* no file: empty configuration */
			bool changed = !loaded || count > 0;
			ant_config_parse(NULL);
			exports_restore();
			memset(&stamp, 0, sizeof stamp);
			size = 0;
			loaded = true;
			return changed;
		}
	}

	if (loaded && !CompareFileTime(&fa.ftLastWriteTime, &stamp) && fa.nFileSizeLow == size)
		return false;
	stamp = fa.ftLastWriteTime;
	size = fa.nFileSizeLow;

	text = read_text(path);
	ant_config_parse(text);
	free(text);
	exports_restore();
	exports_apply();
	loaded = true;
	return true;
}
