#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <windows.h>

#include "config.h"

/*************************************************************
 * Settings and colors.                                      *
 *                                                           *
 * The defaults below are the Monokai theme. note.cfg,       *
 * next to note.exe, overrides any of them:                  *
 *                                                           *
 *   [editor]                                                *
 *   tab_size = 4                                            *
 *                                                           *
 *   [colors]                                                *
 *   text    = #F8F8F2 on #272822                            *
 *   type    = #66D9EF italic                                *
 *************************************************************/

STYLE theme[S_COUNT] = {
	[S_TEXT]                = { 0xF8F8F2, 0x272822 },
	[S_COMMENT]             = { 0x75715E, -1 },
	[S_KEYWORD]             = { 0xF92672, -1 },
	[S_TYPE]                = { 0x66D9EF, -1, false, true },
	[S_FUNCTION]            = { 0xA6E22E, -1 },
	[S_STRING]              = { 0xE6DB74, -1 },
	[S_NUMBER]              = { 0xAE81FF, -1 },
	[S_CONSTANT]            = { 0xAE81FF, -1 },
	[S_PREPROCESSOR]        = { 0xF92672, -1 },
	[S_OPERATOR]            = { 0xF92672, -1 },
	[S_CONTROL]             = { 0xF8F8F0, 0xF92672 },
	[S_CURRENT_LINE]        = { -1, 0x3E3D32 },
	[S_SELECTION]           = { -1, 0x49483E },
	[S_BRACKET]             = { 0xF8F8F2, 0x75715E, true },
	[S_LINE_NUMBER]         = { 0x90908A, -1 },
	[S_LINE_NUMBER_CURRENT] = { 0xF8F8F2, -1 },
	[S_MENU_BAR]            = { 0xF8F8F2, 0x1E1F1C },
	[S_MENU_HOTKEY]         = { 0xFD971F, -1 },
	[S_MENU]                = { 0xF8F8F2, 0x3E3D32 },
	[S_MENU_SELECTED]       = { 0x272822, 0xA6E22E },
	[S_MENU_SHORTCUT]       = { 0x90908A, -1 },
	[S_STATUS_BAR]          = { 0xF8F8F2, 0x1E1F1C },
	[S_STATUS_MESSAGE]      = { 0xE6DB74, -1 },
	[S_STATUS_ERROR]        = { 0xF92672, -1, true },
	[S_DIALOG]              = { 0xF8F8F2, 0x3E3D32 },
	[S_DIALOG_TITLE]        = { 0xE6DB74, -1, true },
	[S_BUTTON]              = { 0xF8F8F2, 0x49483E },
	[S_BUTTON_SELECTED]     = { 0x272822, 0xA6E22E },
	[S_BUTTON_HOTKEY]       = { 0xFD971F, -1 },
	[S_FIELD]               = { 0xF8F8F2, 0x272822 },
	[S_SHADOW]              = { -1, 0x141411 },
};

SETTINGS settings = {
	.tab_size       = 4,
	.line_numbers   = true,
	.syntax         = true,
	.auto_indent    = true,
	.highlight_line = true,
	.mouse          = true,
	.expand_tabs    = false,
	.word_wrap      = false,
};

/* key names in [colors], in the order of the S_* enum */
static const wchar_t *style_names[S_COUNT] = {
	L"text", L"comment", L"keyword", L"type", L"function", L"string", L"number",
	L"constant", L"preprocessor", L"operator", L"control", L"current_line", L"selection",
	L"bracket", L"line_number", L"line_number_current", L"menu_bar", L"menu_hotkey", L"menu",
	L"menu_selected", L"menu_shortcut", L"status_bar", L"status_message", L"status_error",
	L"dialog", L"dialog_title", L"button", L"button_selected", L"button_hotkey", L"field",
	L"shadow",
};

/* "#RRGGBB" -> 0xRRGGBB; "none" -> -1; -2 if invalid */
static int parse_color (const wchar_t *s) {

	int value = 0, i;

	if (!_wcsicmp(s, L"none") || !_wcsicmp(s, L"default"))
		return -1;
	if (s[0] != L'#' || wcslen(s) != 7)
		return -2;
	for (i = 1; i < 7; i++) {
		wchar_t c = towlower(s[i]);
		if (c >= L'0' && c <= L'9')
			value = value * 16 + (c - L'0');
		else if (c >= L'a' && c <= L'f')
			value = value * 16 + (c - L'a' + 10);
		else
			return -2;
	}
	return value;
}

/* "#fg [on #bg] [bold] [italic] [underline]" */
static bool parse_style (wchar_t *value, STYLE *style) {

	STYLE st = { -1, -1, false, false, false };
	bool on = false, have_fg = false;
	wchar_t *context = NULL, *word;

	for (word = wcstok(value, L" \t", &context); word; word = wcstok(NULL, L" \t", &context)) {
		if (!_wcsicmp(word, L"on"))
			on = true;
		else if (!_wcsicmp(word, L"bold"))
			st.bold = true;
		else if (!_wcsicmp(word, L"italic"))
			st.italic = true;
		else if (!_wcsicmp(word, L"underline"))
			st.underline = true;
		else {
			int color = parse_color(word);
			if (color == -2)
				return false;
			if (on)
				st.bg = color;
			else if (!have_fg) {
				st.fg = color;
				have_fg = true;
			} else
				return false;
			on = false;
		}
	}
	if (on)
		return false;
	*style = st;
	return true;
}

static bool parse_bool (const wchar_t *s, bool *out) {

	if (!_wcsicmp(s, L"true") || !_wcsicmp(s, L"yes") || !_wcsicmp(s, L"on")) {
		*out = true;
		return true;
	}
	if (!_wcsicmp(s, L"false") || !_wcsicmp(s, L"no") || !_wcsicmp(s, L"off")) {
		*out = false;
		return true;
	}
	return false;
}

static bool set_editor (const wchar_t *key, const wchar_t *value) {

	if (!wcscmp(key, L"tab_size")) {
		int n = _wtoi(value);
		if (n < 1 || n > 16)
			return false;
		settings.tab_size = n;
		return true;
	}
	if (!wcscmp(key, L"line_numbers"))
		return parse_bool(value, &settings.line_numbers);
	if (!wcscmp(key, L"syntax"))
		return parse_bool(value, &settings.syntax);
	if (!wcscmp(key, L"auto_indent"))
		return parse_bool(value, &settings.auto_indent);
	if (!wcscmp(key, L"highlight_line"))
		return parse_bool(value, &settings.highlight_line);
	if (!wcscmp(key, L"mouse"))
		return parse_bool(value, &settings.mouse);
	if (!wcscmp(key, L"expand_tabs"))
		return parse_bool(value, &settings.expand_tabs);
	if (!wcscmp(key, L"word_wrap"))
		return parse_bool(value, &settings.word_wrap);
	return false;
}

static bool set_color (const wchar_t *key, wchar_t *value) {

	int i;

	for (i = 0; i < S_COUNT; i++)
		if (!wcscmp(key, style_names[i]))
			return parse_style(value, &theme[i]);
	return false;
}

static wchar_t *trim (wchar_t *s) {

	wchar_t *end;

	while (*s == L' ' || *s == L'\t')
		s++;
	end = s + wcslen(s);
	while (end > s && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\r'))
		*--end = 0;
	return s;
}

/*
 * Reads note.cfg from the folder of note.exe. A missing file keeps the defaults.
 * Returns false and a message in 'error' at the first bad line (the lines before it
 * are already applied).
 */
bool config_load (wchar_t *error, int size) {

	wchar_t path[MAX_PATH + 16], *text, *line, *next;
	char *data;
	HANDLE f;
	DWORD got;
	LARGE_INTEGER bytes;
	int n, wn, number = 0;
	enum { NONE, EDITOR, COLORS } section = NONE;
	bool ok = true;

	GetModuleFileNameW(NULL, path, MAX_PATH);
	if (wcsrchr(path, L'\\'))
		wcsrchr(path, L'\\')[1] = 0;
	wcscat(path, L"note.cfg");

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return true;
	if (!GetFileSizeEx(f, &bytes) || bytes.QuadPart > 1024 * 1024) {
		CloseHandle(f);
		swprintf(error, size, L"note.cfg: file too large");
		return false;
	}
	n = (int) bytes.QuadPart;
	data = malloc(n + 1);
	if (!data || !ReadFile(f, data, n, &got, NULL)) {
		free(data);
		CloseHandle(f);
		return true;
	}
	CloseHandle(f);
	n = (int) got;

	wn = n ? MultiByteToWideChar(CP_UTF8, 0, data, n, NULL, 0) : 0;
	text = malloc((wn + 1) * sizeof(wchar_t));
	if (wn)
		MultiByteToWideChar(CP_UTF8, 0, data, n, text, wn);
	text[wn] = 0;
	free(data);

	for (line = text; line && ok; line = next) {
		wchar_t *key, *value, *eq;

		next = wcschr(line, L'\n');
		if (next)
			*next++ = 0;
		number++;

		key = trim(line);
		if (key[0] == 0xFEFF)
			key = trim(key + 1);
		if (!key[0] || key[0] == L'#' || key[0] == L';')
			continue;

		if (key[0] == L'[') {
			if (!wcscmp(key, L"[editor]"))
				section = EDITOR;
			else if (!wcscmp(key, L"[colors]"))
				section = COLORS;
			else {
				swprintf(error, size, L"note.cfg:%d: unknown section %ls", number, key);
				ok = false;
			}
			continue;
		}

		eq = wcschr(key, L'=');
		if (!eq) {
			swprintf(error, size, L"note.cfg:%d: expected key = value", number);
			ok = false;
			continue;
		}
		*eq = 0;
		value = trim(eq + 1);
		key = trim(key);

		if (section == EDITOR ? !set_editor(key, value) : section == COLORS ? !set_color(key, value) : true) {
			swprintf(error, size, L"note.cfg:%d: bad setting '%ls'", number, key);
			ok = false;
		}
	}

	free(text);
	return ok;
}
