#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <windows.h>

#include "mk.h"
#include "../console/console.h"
#include "../shell/shell.h"

/*************************************************************
 * 'mk [-f] file...': creates files, like touch, but filled  *
 * from a template of data\mk.ant. The template is chosen by *
 * the name: an exact file name first (Makefile), then the   *
 * extension (main.c). Without a template the file is empty. *
 * 'mk -l' lists the templates.                              *
 *                                                           *
 * data\mk.ant: a header line "[.c .h]" (extensions) or      *
 * "[=Makefile]" (exact names) starts a template that goes   *
 * on until the next header. Lines before the first header   *
 * are comments. In the text, {name} is the file name without *
 * the extension, {NAME} the same as a C identifier in upper *
 * case (header guards) and {year} the current year.         *
 *************************************************************/

/* the whole data\mk.ant, NUL-terminated; NULL if it cannot be read */
static char *load_templates (void) {

	wchar_t path[MAX_PATH];
	int n = GetModuleFileNameW(NULL, path, MAX_PATH);
	FILE *file;
	char *text;
	long size;

	while (n > 0 && path[n - 1] != L'\\')
		n--;
	if (n == 0 || n + 16 >= MAX_PATH)
		return NULL;
	wcscpy(path + n, L"data\\mk.ant");
	if (!(file = _wfopen(path, L"rb")))
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	if (size < 0 || !(text = malloc(size + 1))) {
		fclose(file);
		return NULL;
	}
	size = (long) fread(text, 1, size, file);
	text[size] = 0;
	fclose(file);
	return text;
}

/* "[.c .h]" or "[=Makefile]": the names between the brackets in 'names'; false if the line is not a header */
static bool header_names (const char *line, int len, char *names, int max) {

	if (len < 4 || line[0] != '[' || line[len - 1] != ']' || (line[1] != '.' && line[1] != '='))
		return false;
	len -= 2;
	if (len >= max)
		len = max - 1;
	memcpy(names, line + 1, len);
	names[len] = 0;
	return true;
}

/* does the header list 'key' (".c" or "=Makefile"), ignoring case? */
static bool names_have (const char *names, const char *key) {

	char copy[256], *tok;

	strncpy(copy, names, sizeof copy - 1);
	copy[sizeof copy - 1] = 0;
	for (tok = strtok(copy, " \t"); tok; tok = strtok(NULL, " \t"))
		if (!_stricmp(tok, key))
			return true;
	return false;
}

/* the length of the line at 'p' without its line break; *next is where the following line starts */
static int next_line (const char *p, const char **next) {

	int len = 0;

	while (p[len] && p[len] != '\n')
		len++;
	*next = p[len] ? p + len + 1 : p + len;
	if (len > 0 && p[len - 1] == '\r')
		len--;
	return len;
}

/* the body of the template for the file name (UTF-8, no folder) in [*start, *end); false if none matches */
static bool find_template (const char *text, const char *file, const char **start, const char **end) {

	char exact[ANTMAX * 3], ext[ANTMAX * 3], names[256];
	const char *dot = strrchr(file, '.');
	const char *ext_start = NULL, *ext_end = NULL, *cur_start = NULL;
	bool in_exact = false, in_ext = false;
	const char *p = text;

	snprintf(exact, sizeof exact, "=%s", file);
	snprintf(ext, sizeof ext, "%s", dot ? dot : "");
	for (;;) {
		const char *line = p, *next;
		int len = next_line(p, &next);
		bool header = header_names(line, len, names, sizeof names), last = !*p;

		if (header || last) {
			/* the template that was open ends here: an exact name wins at once, an extension only if it is the first */
			if (cur_start && in_exact) {
				*start = cur_start;
				*end = line;
				return true;
			}
			if (cur_start && in_ext && !ext_start) {
				ext_start = cur_start;
				ext_end = line;
			}
			cur_start = NULL;
			in_exact = in_ext = false;
		}
		if (last)
			break;
		if (header) {
			in_exact = names_have(names, exact);
			in_ext = dot && names_have(names, ext);
			cur_start = next;
		}
		p = next;
	}
	if (ext_start) {
		*start = ext_start;
		*end = ext_end;
		return true;
	}
	return false;
}

/* writes [start, end) with {name}, {NAME} and {year} replaced; blank lines at the end are dropped; crlf: Windows line breaks */
static void write_body (FILE *out, const char *start, const char *end, const char *base, bool crlf) {

	SYSTEMTIME now;
	char upper[ANTMAX * 3];
	const char *p = start;
	int i;

	GetLocalTime(&now);
	for (i = 0; base[i] && i < (int) sizeof upper - 1; i++) {
		unsigned char ch = (unsigned char) base[i];

		upper[i] = (isalnum(ch) || ch >= 0x80) ? toupper(ch) : '_';
	}
	upper[i] = 0;

	/* drop the blank lines at the end, then end with one line break */
	while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (end == start)
		return;
	while (p < end) {
		if (!strncmp(p, "{name}", 6)) {
			fputs(base, out);
			p += 6;
		} else if (!strncmp(p, "{NAME}", 6)) {
			fputs(upper, out);
			p += 6;
		} else if (!strncmp(p, "{year}", 6)) {
			fprintf(out, "%d", now.wYear);
			p += 6;
		} else if (*p == '\r') {
			p++;
		} else if (*p == '\n') {
			fputs(crlf ? "\r\n" : "\n", out);
			p++;
		} else
			fputc(*p++, out);
	}
	fputs(crlf ? "\r\n" : "\n", out);
}

/* creates the folders of the path of the file (mk src\sub\main.c) */
static void make_parents (const wchar_t *path) {

	wchar_t dir[MAX_PATH];

	wcsncpy(dir, path, MAX_PATH - 1);
	dir[MAX_PATH - 1] = 0;
	for (wchar_t *p = dir + 1; *p; p++) {
		if ((*p == L'\\' || *p == L'/') && p[-1] != L':') {
			wchar_t save = *p;

			*p = 0;
			CreateDirectoryW(dir, NULL);
			*p = save;
		}
	}
}

/* the headers of data\mk.ant, one per line, without the brackets */
static void list_templates (const char *text) {

	const char *p = text;

	while (*p) {
		const char *next;
		char names[256];
		int len = next_line(p, &next);

		if (header_names(p, len, names, sizeof names)) {
			wchar_t wide[256];

			MultiByteToWideChar(CP_UTF8, 0, names, -1, wide, 256);
			ant_write(wide);
			putchar('\n');
		}
		p = next;
	}
}

static bool ends_with (const wchar_t *s, const wchar_t *suffix) {

	size_t n = wcslen(s), m = wcslen(suffix);

	return n >= m && !_wcsicmp(s + n - m, suffix);
}

void ant_cmd_mk (const ANT_CMD *c) {

	bool force = false, list = false;
	int first = 1;
	char *text;

	for (; first < c->argc && c->argv[first][0] == L'-' && c->argv[first][1]; first++) {
		if (!wcscmp(c->argv[first], L"--")) {
			first++;
			break;
		}
		for (const wchar_t *o = c->argv[first] + 1; *o; o++) {
			if (*o == L'f')
				force = true;
			else if (*o == L'l')
				list = true;
			else {
				ant_error(L"ant: mk: invalid option -- '%lc'", *o);
				ant_status = 2;
				return;
			}
		}
	}
	if (!list && first >= c->argc) {
		ant_error(L"usage: mk [-f] file...   (mk -l lists the templates)");
		ant_status = 2;
		return;
	}

	ant_status = 0;
	text = load_templates();
	if (list) {
		if (!text) {
			ant_error(L"ant: mk: data\\mk.ant not found");
			ant_status = 1;
			return;
		}
		list_templates(text);
		free(text);
		return;
	}
	if (!text) {
		ant_error(L"ant: mk: data\\mk.ant not found (the files will be empty)");
		text = _strdup("");
	}

	for (int i = first; i < c->argc; i++) {
		const wchar_t *path = c->argv[i], *slash = wcsrchr(path, L'\\'), *slash2 = wcsrchr(path, L'/');
		const wchar_t *file, *dot;
		char name[ANTMAX * 3], base[ANTMAX * 3];
		const char *start, *end;
		DWORD attr;
		FILE *out;
		bool have;

		if (slash2 > slash)
			slash = slash2;
		file = slash ? slash + 1 : path;
		if (!*file) {
			ant_error(L"ant: mk: '%ls': not a file name", path);
			ant_status = 1;
			continue;
		}
		attr = GetFileAttributesW(path);
		if (attr != INVALID_FILE_ATTRIBUTES && ((attr & FILE_ATTRIBUTE_DIRECTORY) || !force)) {
			ant_error(L"ant: mk: cannot create '%ls': %ls", path,
				(attr & FILE_ATTRIBUTE_DIRECTORY) ? L"is a directory" : L"file exists (mk -f overwrites)");
			ant_status = 1;
			continue;
		}

		WideCharToMultiByte(CP_UTF8, 0, file, -1, name, sizeof name, NULL, NULL);
		strcpy(base, name);
		dot = wcsrchr(file, L'.');
		if (dot && dot > file)
			*strrchr(base, '.') = 0;
		have = find_template(text, name, &start, &end);

		make_parents(path);
		if (!(out = _wfopen(path, L"wb"))) {
			ant_error(L"ant: mk: cannot create '%ls'", path);
			ant_status = 1;
			continue;
		}
		if (have)
			write_body(out, start, end, base, ends_with(file, L".bat") || ends_with(file, L".cmd"));
		fclose(out);
	}
	free(text);
}
