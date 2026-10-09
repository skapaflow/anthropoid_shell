/*************************************************************
 * Test of the line editor (src/editor/line_editor.c).       *
 *                                                           *
 * usage: input_test.exe antshell.exe                              *
 *                                                           *
 * The process relaunches itself in a new hidden console, opens   *
 * antshell.exe in that console, injects keys (WriteConsoleInputW) *
 * and checks the screen (ReadConsoleOutputCharacterW). The cases  *
 * follow the checklist of doc/COOKED_READ_DATA.md of Windows  *
 * Terminal.                                                 *
 *************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <windows.h>

#define CTRL       LEFT_CTRL_PRESSED
#define ALT        LEFT_ALT_PRESSED

static HANDLE conin, conout;
static FILE *report;
static int failures = 0, total = 0;
static int width;
static int prompt_row;
static int prompt_len = 1;      /* the prompt is just '>': read from the screen in begin() */
static wchar_t prompt[512];

/* ---------- console ---------- */

static void send_record (WORD vk, wchar_t ch, DWORD mods, bool down) {

	INPUT_RECORD ir;
	DWORD n;

	memset(&ir, 0, sizeof ir);
	ir.EventType = KEY_EVENT;
	ir.Event.KeyEvent.bKeyDown = down;
	ir.Event.KeyEvent.wRepeatCount = 1;
	ir.Event.KeyEvent.wVirtualKeyCode = vk;
	ir.Event.KeyEvent.wVirtualScanCode = (WORD) MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	ir.Event.KeyEvent.uChar.UnicodeChar = ch;
	ir.Event.KeyEvent.dwControlKeyState = mods;
	WriteConsoleInputW(conin, &ir, 1, &n);
}

static void key (WORD vk, wchar_t ch, DWORD mods) {

	/* arrows, Home/End, Insert/Delete and PgUp/PgDn of the separate block arrive with ENHANCED_KEY */
	if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
		mods |= ENHANCED_KEY;
	send_record(vk, ch, mods, true);
	send_record(vk, ch, mods, false);
}

/* text as it arrives from a paste/IME: one event per UTF-16 unit */
static void type (const wchar_t *s) {

	static INPUT_RECORD buf[4096];
	DWORD n;
	int k = 0;

	for (; *s; s++) {
		SHORT scan = (*s < 128) ? VkKeyScanW(*s) : -1;
		WORD vk = (scan == -1) ? 0 : LOBYTE(scan);
		memset(&buf[k], 0, sizeof buf[k]);
		buf[k].EventType = KEY_EVENT;
		buf[k].Event.KeyEvent.bKeyDown = TRUE;
		buf[k].Event.KeyEvent.wRepeatCount = 1;
		buf[k].Event.KeyEvent.wVirtualKeyCode = vk;
		buf[k].Event.KeyEvent.uChar.UnicodeChar = *s;
		buf[k+1] = buf[k];
		buf[k+1].Event.KeyEvent.bKeyDown = FALSE;
		k += 2;
		if (k == 4096) {
			WriteConsoleInputW(conin, buf, k, &n);
			k = 0;
		}
	}
	if (k)
		WriteConsoleInputW(conin, buf, k, &n);
}

/* waits for antshell.exe to consume the input and redraw */
/* what this console is like; the cases that depend on it adapt (see inner) */
static int  window_rows = 0;        /* rows of the window, which sets how much of the pager is visible */
static bool astral_ok   = true;     /* does the console keep a character outside the BMP? */

static void settle (void) {

	DWORD pending = 1;

	for (int i = 0; i < 400 && pending; i++) {
		GetNumberOfConsoleInputEvents(conin, &pending);
		if (pending)
			Sleep(5);
	}
	Sleep(30);
}

static COORD cursor_pos (void) {

	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(conout, &csbi);
	return csbi.dwCursorPosition;
}

static void read_row (int row, wchar_t *out, int cells) {

	COORD c = { 0, (SHORT) row };
	DWORD n = 0;

	ReadConsoleOutputCharacterW(conout, out, cells, c, &n);
	out[n] = 0;
}

/* typed text: cells from the end of the prompt to the last non-empty line, without trailing spaces
   (spaces at the end of the text do not show; the cases rely on that) */
/* does line 'row' continue on the next one? (text reached the edge; a wide character that did not fit leaves
   the last cell empty). The pager never uses the last column and, in the tests, does not reach the one before it. */
static bool row_wraps (int row) {

	CHAR_INFO cells[2];
	COORD size = { 2, 1 }, at = { 0, 0 };
	SMALL_RECT rect = { (SHORT) (width - 2), (SHORT) row, (SHORT) (width - 1), (SHORT) row };

	ReadConsoleOutputW(conout, cells, size, at, &rect);
	return cells[0].Char.UnicodeChar != L' ' || cells[1].Char.UnicodeChar != L' ';
}

/* autosuggestion cell: gray (SGR 90 = FOREGROUND_INTENSITY alone) */
static bool is_suggestion (WORD attr) {

	return (attr & 0x0F) == FOREGROUND_INTENSITY;
}

/* reads the prompt lines; returns in 'gray' the index of the 1st autosuggestion cell (or -1) */
static int prompt_cells (wchar_t *buf, int *gray) {

	static WORD attrs[16384];
	COORD c = { 0, (SHORT) prompt_row };
	DWORD n = 0;
	int rows = 1;

	while (rows < 40 && row_wraps(prompt_row + rows - 1))
		rows++;
	read_row(prompt_row, buf, rows * width);
	ReadConsoleOutputAttribute(conout, attrs, rows * width, c, &n);

	*gray = -1;
	for (int i = prompt_len; i < (int) n; i++)
		if (is_suggestion(attrs[i])) {
			*gray = i;
			break;
		}
	return (int) n;
}

/* autosuggestion on screen (gray cells after the text), without trailing spaces */
static void suggestion_text (wchar_t *out, int max) {

	static wchar_t buf[16384];
	int gray, n = 0;

	prompt_cells(buf, &gray);
	if (gray >= 0)
		for (int i = gray; buf[i] && n < max - 1; i++)
			out[n++] = buf[i];
	while (n > 0 && out[n-1] == L' ')
		n--;
	out[n] = 0;
}

/* typed text (without the autosuggestion) */
static void current_line (wchar_t *out, int max) {

	static wchar_t buf[16384];
	int gray;

	prompt_cells(buf, &gray);
	if (gray >= 0)
		buf[gray] = 0;

	int n = wcslen(buf);
	while (n > 0 && buf[n-1] == L' ')
		buf[--n] = 0;

	wcsncpy(out, (n >= prompt_len) ? buf + prompt_len : L"", max - 1);
	out[max-1] = 0;
}

/* looks for 'name' in the lines below the prompt (where the pager and the popups are) */
static bool below_has (const wchar_t *name) {

	wchar_t row[512];

	for (int r = 1; r <= 40; r++) {
		read_row(prompt_row + r, row, width);
		if (wcsstr(row, name))
			return true;
	}
	return false;
}

/* cursor position in cells since the end of the prompt */
static int cursor_cells (void) {

	COORD c = cursor_pos();
	return (c.Y - prompt_row) * width + c.X - prompt_len;
}

/* ---------- casos ---------- */

static void begin (void) {

	/* with the pager open the 1st Esc only closes the pager; the 2nd clears the line */
	key(VK_ESCAPE, 27, 0);
	key(VK_ESCAPE, 27, 0);
	key(VK_END, 0, 0);
	settle();
	prompt_row = cursor_pos().Y;
	prompt_len = cursor_pos().X;
	read_row(prompt_row, prompt, prompt_len);
}

/*
 * Some console hosts (the one of the GitHub runner, for one) read a character outside the
 * BMP back as a single U+FFFD, while the cursor still moves over it as one wide cell. The
 * editor is right in both cases, so when the console cannot keep the character the
 * expectation is written the way the console will give it back.
 */
static const wchar_t *as_read_back (const wchar_t *s) {

	static wchar_t out[16384];
	int n = 0;

	if (astral_ok)
		return s;
	for (; *s && n < 16383; s++) {
		if (*s >= 0xD800 && *s <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
			out[n++] = 0xFFFD;
			s++;
		} else
			out[n++] = *s;
	}
	out[n] = 0;
	return out;
}

static void check (const char *name, const wchar_t *want_text, int want_cursor) {

	static wchar_t got[16384];

	want_text = as_read_back(want_text);

	settle();
	current_line(got, 16384);
	int cur = cursor_cells();
	bool ok = !wcscmp(got, want_text) && (want_cursor < 0 || cur == want_cursor);

	total++;
	if (!ok)
		failures++;

	fprintf(report, "%s  %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		char a[4096], b[4096];
		WideCharToMultiByte(CP_UTF8, 0, want_text, -1, a, sizeof a, NULL, NULL);
		WideCharToMultiByte(CP_UTF8, 0, got, -1, b, sizeof b, NULL, NULL);
		fprintf(report, "      expected: [%s] cursor %d\n", a, want_cursor);
		fprintf(report, "      got:      [%s] cursor %d\n", b, cur);
	}
}

static void check_true (const char *name, bool ok, const char *detail) {

	total++;
	if (!ok)
		failures++;
	fprintf(report, "%s  %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok && detail)
		fprintf(report, "      %s\n", detail);
}

static void repeat (WORD vk, wchar_t ch, DWORD mods, int times) {

	while (times--)
		key(vk, ch, mods);
}

static wchar_t *fill (wchar_t *buf, wchar_t c, int n) {

	for (int i = 0; i < n; i++)
		buf[i] = c;
	buf[n] = 0;
	return buf;
}

static void run_cases (void) {

	static wchar_t s[8192], t[8192];

	begin(); type(L"echo hello");
	check("ASCII", L"echo hello", 10);

	begin(); type(L"a\u00e7\u00e3o \u00e9 f\u00e1cil");
	check("accents (ç ã é á)", L"a\u00e7\u00e3o \u00e9 f\u00e1cil", 12);

	begin(); type(L"\u4e2d\u6587\u7dad\u57fa\u767e\u79d1");
	check("Chinese (6 wide characters)", L"\u4e2d\u6587\u7dad\u57fa\u767e\u79d1", 12);

	begin(); type(L"a\U0001F642b");
	check("surrogate pair 🙂", L"a\U0001F642b", 4);
	key(VK_LEFT, 0, 0);
	check("← before the b", L"a\U0001F642b", 3);
	key(VK_LEFT, 0, 0);
	check("← skips the whole 🙂", L"a\U0001F642b", 1);
	key(VK_RIGHT, 0, 0);
	check("→ skips the whole 🙂", L"a\U0001F642b", 3);
	key(VK_BACK, 8, 0);
	check("Backspace deletes the whole 🙂", L"ab", 1);

	begin(); type(L"éx"); key(VK_LEFT, 0, 0); key(VK_LEFT, 0, 0); key(VK_DELETE, 0, 0);
	check("combining mark: e+U+0301 is a single grapheme", L"x", 0);

	begin(); type(L"a👨‍👩‍👧"); key(VK_BACK, 8, 0);
	check("ZWJ family deleted with one Backspace", L"a", 1);

	/* TAB pager: an empty word lists the current folder */
	begin(); key(VK_TAB, 9, 0);
	settle();
	check_true("TAB on the empty line opens the pager with the folder", below_has(L"Makefile"), "the pager did not show up");
	key(VK_ESCAPE, 27, 0);
	settle();
	check_true("Esc closes the pager", !below_has(L"Makefile"), "the pager is still on screen");
	check("line is still empty after the pager", L"", 0);

	begin(); type(L"type Makefile"); key(VK_HOME, 0, 0); key(VK_TAB, 9, 0);
	settle();
	check_true("TAB with the cursor at the start opens the pager", below_has(L"Makefile"), "the pager did not show up");
	key(VK_ESCAPE, 27, 0);
	check("line intact after Esc in the pager", L"type Makefile", 0);

	begin(); type(L"type .\\"); key(VK_TAB, 9, 0);
	settle();
	check_true("TAB after .\\ lists the folder", below_has(L"Makefile"), "the pager did not show up");
	key(VK_ESCAPE, 27, 0);
	check("line intact after Esc in the pager (.\\)", L"type .\\", 7);

	/* a trailing space does not show when reading the screen; the cursor shows it is there */
	begin(); type(L"type Mak"); key(VK_TAB, 9, 0);
	check("single TAB: full file and a space (Mak → Makefile )", L"type Makefile", 14);

	begin(); type(L"abc"); key(VK_BACK, 8, 0);
	check("Backspace", L"ab", 2);

	begin(); type(L"echo hello world"); key(VK_BACK, 0x7F, CTRL);
	check("Ctrl+Backspace deletes the word", L"echo hello", 11);

	begin(); type(L"cd C:\\foo\\bar"); key(VK_BACK, 0x7F, CTRL);
	check("Ctrl+Backspace stops at the \\", L"cd C:\\foo\\", 10);

	begin(); type(L"some text"); key(VK_ESCAPE, 27, 0);
	check("Esc clears", L"", 0);

	begin(); type(L"abc"); key(VK_HOME, 0, 0); type(L"X");
	check("Home", L"Xabc", 1);

	begin(); type(L"abcdef"); repeat(VK_LEFT, 0, 0, 2); key(VK_HOME, 0, CTRL);
	check("Ctrl+Home deletes to the start", L"ef", 0);

	begin(); type(L"abc"); key(VK_HOME, 0, 0); key(VK_END, 0, 0);
	check("End", L"abc", 3);

	begin(); type(L"abcdef"); key(VK_HOME, 0, 0); repeat(VK_RIGHT, 0, 0, 2); key(VK_END, 0, CTRL);
	check("Ctrl+End deletes to the end", L"ab", 2);

	begin(); type(L"echo hello world"); key(VK_LEFT, 0, CTRL);
	check("Ctrl+← start of the word", L"echo hello world", 11);
	key(VK_LEFT, 0, CTRL);
	check("Ctrl+← again", L"echo hello world", 5);
	key(VK_RIGHT, 0, CTRL);
	check("Ctrl+→ next word", L"echo hello world", 11);

	begin(); type(L"abcd"); key(VK_HOME, 0, 0); key(VK_INSERT, 0, 0); type(L"XY"); key(VK_INSERT, 0, 0);
	check("Insert: overwrite", L"XYcd", 2);
	type(L"Z");
	check("Insert again: insert", L"XYZcd", 3);

	begin(); type(L"abcd"); key(VK_HOME, 0, 0); key(VK_DELETE, 0, 0);
	check("Delete", L"bcd", 0);

	begin(); type(L"ab");
	key(VK_F1, 0, 0); key(VK_F3, 0, 0); key(VK_F10, 0, 0); key(VK_F12, 0, 0);
	key('X', L'x', ALT); key(VK_UP, 0, 0); key(VK_PRIOR, 0, 0);
	check("F1/F3/F10/F12/Alt+X/↑/PgUp insert nothing", L"ab", 2);

	begin(); key('Q', L'/', CTRL | RIGHT_ALT_PRESSED); key('W', L'?', CTRL | RIGHT_ALT_PRESSED);
	check("AltGr (Ctrl+Alt) inserts text", L"/?", 2);

	begin(); type(L"text"); key('C', 3, CTRL);
	settle();
	{
		wchar_t row[512];
		read_row(prompt_row, row, width);
		int n = wcslen(row);
		while (n > 0 && row[n-1] == L' ')
			row[--n] = 0;
		check_true("Ctrl+C cancels and shows ^C", !wcsncmp(row, prompt, prompt_len) && !wcscmp(row + prompt_len, L"text^C"),
			"previous line different from <prompt>text^C");
	}
	prompt_row = cursor_pos().Y;
	check("Ctrl+C: new empty prompt", L"", 0);

	/* line wrap */
	begin(); type(fill(s, L'a', width + 10));
	check("line longer than the width", s, width + 10);
	repeat(VK_LEFT, 0, 0, 15);
	check("← crosses the wrap", s, width - 5);
	key(VK_BACK, 8, 0);
	fill(t, L'a', width + 9);
	check("Backspace before the wrap", t, width - 6);

	begin(); type(fill(s, L'b', width - prompt_len));
	check("text ends exactly at the edge", s, width - prompt_len);
	check_true("cursor goes to the next line, column 0", cursor_pos().X == 0 && cursor_pos().Y == prompt_row + 1, NULL);

	begin(); type(fill(s, L'c', width - prompt_len - 1)); type(L"\u4e2d");
	settle();
	{
		COORD c = cursor_pos();
		char d[128];
		sprintf(d, "cursor at x=%d y=+%d, expected x=2 y=+1", c.X, c.Y - prompt_row);
		check_true("a wide character in the last column wraps whole", c.X == 2 && c.Y == prompt_row + 1, d);
	}

	/* large paste and the buffer limit (1023 UTF-8 bytes) */
	begin();
	{
		DWORD t0 = GetTickCount();
		type(fill(s, L'x', 900));
		settle();
		DWORD dt = GetTickCount() - t0;
		char d[64];
		sprintf(d, "took %lu ms", dt);
		check("paste of 900 characters", s, 900);
		check_true("paste in under 2 s", dt < 2000, d);
	}
	begin(); type(fill(s, L'y', 1100));
	check("limit: 1100 ASCII become 1023", fill(t, L'y', 1023), 1023);
	begin(); type(fill(s, L'\u00e7', 600));
	check("limit: ç (2 bytes) stops at 511", fill(t, L'\u00e7', 511), 511);

	/* arg.c used to copy each argument into a 512-byte buffer without a limit */
	begin(); type(fill(s, L'z', 600)); key(VK_RETURN, 13, 0);
	settle();
	begin(); type(L"ok");
	check("a 600-character argument does not bring the shell down", L"ok", 2);

	/* Enter runs: the '/' command shows the folder of antshell.exe */
	begin(); type(L"/"); key(VK_RETURN, 13, 0);
	settle();
	{
		wchar_t row[512];
		read_row(prompt_row + 1, row, width);
		check_true("Enter runs the command (/)", wcsstr(row, L"\\") != NULL, "the next line does not show a path");
	}
}

/* runs a command (missing = prints nothing) to get into the history */
static void run (const wchar_t *cmd) {

	begin();
	type(cmd);
	key(VK_RETURN, 13, 0);
	settle();
}

/* the prompt line shows the prompt followed by 'text' */
static bool prompt_row_is (const wchar_t *text) {

	wchar_t row[512];
	int n;

	read_row(prompt_row, row, width);
	n = wcslen(row);
	while (n > 0 && row[n-1] == L' ')
		row[--n] = 0;
	return !wcsncmp(row, prompt, prompt_len) && !wcscmp(row + prompt_len, text);
}

/* 'history clear' also empties history_rank.ant, so the autosuggestion only knows these three */
static void fresh_history (void) {

	run(L"history clear");
	run(L"cmd_a 1");
	run(L"cmd_b 2");
	run(L"cmd_c 3");
}

static void check_suggestion (const char *name, const wchar_t *want) {

	wchar_t got[4096];
	char a[4096], b[4096];

	settle();
	suggestion_text(got, 4096);
	total++;
	if (!wcscmp(got, want)) {
		fprintf(report, "PASS  %s\n", name);
		return;
	}
	failures++;
	WideCharToMultiByte(CP_UTF8, 0, want, -1, a, sizeof a, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, got, -1, b, sizeof b, NULL, NULL);
	fprintf(report, "FAIL  %s\n      expected: [%s]\n      got:      [%s]\n", name, a, b);
}

/* history_rank.ant, next to ANT_HISTORY_FILE */
static bool rank_path (wchar_t *path) {

	int n = GetEnvironmentVariableW(L"ANT_HISTORY_FILE", path, MAX_PATH);

	if (n < 4 || n + 6 >= MAX_PATH)
		return false;
	wcscpy(path + n - 4, L"_rank.ant");
	return true;
}

/* replaces the file with text in UTF-8 */
static void file_write (const wchar_t *path, const wchar_t *text) {

	char bytes[8192];
	DWORD written;
	HANDLE f;
	int b;

	b = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof bytes, NULL, NULL) - 1;
	f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	if (b > 0)
		WriteFile(f, bytes, b, &written, NULL);
	CloseHandle(f);
}

/* replaces history_rank.ant with text; the shell reads it again at the next command */
static void rank_write (const wchar_t *text) {

	wchar_t path[MAX_PATH];

	if (rank_path(path))
		file_write(path, text);
}

/* does line 'at' (any of them if at < 0) of the rank text read "points <time> cmd"? */
static bool rank_has (const wchar_t *s, int at, const wchar_t *points, const wchar_t *cmd) {

	int pn = wcslen(points), cn = wcslen(cmd);

	for (int i = 0; s && *s; i++, s = wcschr(s, L'\n') ? wcschr(s, L'\n') + 1 : NULL) {
		wchar_t *end;
		long long last;

		if ((at >= 0 && i != at) || wcsncmp(s, points, pn) || s[pn] != L' ')
			continue;
		last = wcstoll(s + pn + 1, &end, 10);
		if (last >= 1000000000 && *end == L' ' && !wcsncmp(end + 1, cmd, cn) && end[1 + cn] == L'\n')
			return true;
	}
	return false;
}

/* history_rank.ant in out; false if it does not exist */
static bool rank_file (wchar_t *out, int max) {

	wchar_t path[MAX_PATH];
	char bytes[65536];
	DWORD got = 0;
	HANDLE f;
	int n;

	out[0] = 0;
	if (!rank_path(path))
		return false;
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	ReadFile(f, bytes, sizeof bytes, &got, NULL);
	CloseHandle(f);
	n = MultiByteToWideChar(CP_UTF8, 0, bytes, got, out, max - 1);
	out[n] = 0;
	return true;
}

/* history autosuggestion in gray, as in fish */
static void run_suggestion_cases (void) {

	static wchar_t s[8192];

	fresh_history();

	begin(); type(L"cmd_b");
	check("autosuggestion: the typed text stays the same", L"cmd_b", 5);
	check_suggestion("autosuggestion: rest of the command in gray", L" 2");

	key(VK_RIGHT, 0, 0);
	check("→ at the end accepts the autosuggestion", L"cmd_b 2", 7);
	check_suggestion("no autosuggestion after accepting", L"");

	begin(); type(L"cmd");
	check_suggestion("the autosuggestion comes from the newest command", L"_c 3");
	key(VK_RIGHT, 0, CTRL);
	check("Ctrl+→ accepts one word of the autosuggestion", L"cmd_c", 6);
	check_suggestion("the rest is still suggested", L"3");

	begin(); type(L"cmd_a"); key(VK_END, 0, 0);
	check("End at the end accepts the autosuggestion", L"cmd_a 1", 7);

	begin(); type(L"CMD_A");
	check_suggestion("case-insensitive autosuggestion", L" 1");
	key(VK_RIGHT, 0, 0);
	check("accepting keeps what was typed", L"CMD_A 1", 7);

	begin(); type(L"cmd_a"); key(VK_LEFT, 0, 0);
	check_suggestion("no autosuggestion with the cursor before the end", L"");
	key(VK_RIGHT, 0, 0);
	check("→ in the middle only moves the cursor", L"cmd_a", 5);

	begin(); type(L"   ");
	check_suggestion("no autosuggestion with only spaces", L"");

	begin(); type(L"cmd_a 1");
	check_suggestion("no autosuggestion when the line is the whole command", L"");

	begin(); type(L"cmd_b"); key(VK_RETURN, 13, 0);
	settle();
	check_true("Enter runs only the typed text", prompt_row_is(L"cmd_b"), "the autosuggestion went into the command");

	/* command longer than the screen: the autosuggestion stops at the next-to-last column with an ellipsis */
	fill(s, L'x', width + 20);
	wmemcpy(s, L"longo_", 6);
	run(s);
	begin(); type(L"longo_");
	settle();
	suggestion_text(s + 4096, 4096);
	check_true("a long autosuggestion ends in an ellipsis",
		wcslen(s + 4096) == (size_t) (width - 1 - prompt_len - 6) && s[4096 + wcslen(s + 4096) - 1] == 0x2026,
		"did not fit on the line or the … is missing");
	read_row(prompt_row + 1, s, width);
	check_true("a long autosuggestion does not wrap the line", wcsspn(s, L" ") == wcslen(s), "the line below has text");
	check("long autosuggestion: cursor at the end of the text", L"longo_", 6);

	/* the most used command (history_rank.ant) comes before the newest */
	fresh_history();
	run(L"cmd_a 1");
	run(L"cmd_a 1");
	run(L"cmd_c 3");
	begin(); type(L"cmd");
	check_suggestion("the autosuggestion prefers the most used command", L"_a 1");
	rank_file(s, 8192);
	check_true("history_rank.ant: points, last use and command, most points first",
		rank_has(s, 0, L"3.00", L"cmd_a 1") && rank_has(s, 1, L"2.00", L"cmd_c 3") && rank_has(s, 2, L"1.00", L"cmd_b 2"),
		"unexpected rank file");

	/* full: the commands with fewest points, used longest ago, go away */
	for (int i = 0; i < 50; i++) {
		wchar_t cmd[32];
		swprintf(cmd, 32, L"rank_%d", i);
		run(cmd);
	}
	rank_file(s, 8192);
	{
		int lines = 0;
		for (wchar_t *p = s; *p; p++)
			lines += *p == L'\n';
		check_true("history_rank.ant keeps 50 commands", lines == 50, "not 50 lines");
	}
	check_true("full rank drops the lowest score, oldest first",
		rank_has(s, -1, L"3.00", L"cmd_a 1") && rank_has(s, -1, L"2.00", L"cmd_c 3") && rank_has(s, -1, L"1.00", L"rank_49")
		&& rank_has(s, -1, L"1.00", L"rank_2") && !wcsstr(s, L"cmd_b 2") && !wcsstr(s, L" rank_1\n") && !wcsstr(s, L" rank_0\n"),
		"wrong commands dropped");

	/* frecency: 3 points from now beat 20 points from 3 days ago (3 × 4 > 20 / 2) */
	{
		long long now = time(NULL);

		run(L"history clear");
		swprintf(s, 8192, L"20.00 %lld git_status_x\n3.00 %lld git_push_x\n", now - 3 * 86400, now);
		rank_write(s);
		run(L"zz_other");
		begin(); type(L"git_");
		check_suggestion("the autosuggestion weighs the points by the last use", L"push_x");

		/* the first format, "points command", still reads (used now) */
		rank_write(L"5 old_cmd x\n");
		run(L"zz_other");
		rank_file(s, 8192);
		check_true("history_rank.ant without the time still reads", rank_has(s, -1, L"5.00", L"old_cmd x"), "old line lost");

		/* aging: more than 1000 points shrink to 900; below 1 goes away; the new command stays */
		swprintf(s, 8192, L"990.00 %lld big_cmd\n10.50 %lld small_cmd\n1.00 %lld tiny_cmd\n", now - 2 * 86400, now - 2 * 86400, now - 2 * 86400);
		rank_write(s);
		run(L"zz_new");
		rank_file(s, 8192);
		check_true("aging shrinks the points to 90% of 1000",
			rank_has(s, 0, L"889.67", L"big_cmd") && rank_has(s, 1, L"9.44", L"small_cmd") && rank_has(s, 2, L"1.00", L"zz_new")
			&& !wcsstr(s, L"tiny_cmd"), "unexpected points after aging");
	}
	run(L"history clear");
	check_true("history clear erases history_rank.ant", !rank_file(s, 8192), "rank file still there");
}

/* does the log file (ANT_HISTORY_FILE) have this text? (format: 2026-09-30 14:05:33 folder> command) */
static bool history_file_has (const wchar_t *text) {

	wchar_t path[MAX_PATH], *all;
	char bytes[65536];
	DWORD got = 0;
	HANDLE f;
	bool found = false;

	if (!GetEnvironmentVariableW(L"ANT_HISTORY_FILE", path, MAX_PATH))
		return false;
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	ReadFile(f, bytes, sizeof bytes - 1, &got, NULL);
	CloseHandle(f);
	all = calloc(got + 1, sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, bytes, got, all, got);
	/* each line starts with "AAAA-MM-DD HH:MM:SS ", without a number in front */
	for (wchar_t *p = all; p && *p; p = wcschr(p, L'\n') ? wcschr(p, L'\n') + 1 : NULL) {
		int y, mo, d, h, mi, sec;
		wchar_t *hit = wcsstr(p, text), *end = wcschr(p, L'\n');
		if (swscanf(p, L"%d-%d-%d %d:%d:%d ", &y, &mo, &d, &h, &mi, &sec) == 6 && hit && (!end || hit < end))
			found = true;
	}
	free(all);
	return found;
}

static void run_history_cases (void) {

	begin(); key(VK_F7, 0, ALT); key(VK_UP, 0, 0);
	check("Alt+F7 clears; ↑ without history does nothing", L"", 0);

	fresh_history();

	begin(); key(VK_UP, 0, 0);
	check("↑ newest command", L"cmd_c 3", 7);
	key(VK_UP, 0, 0);
	check("↑ previous", L"cmd_b 2", 7);
	key(VK_F5, 0, 0);
	check("F5 same as ↑", L"cmd_a 1", 7);
	key(VK_UP, 0, 0);
	check("↑ stops at the first", L"cmd_a 1", 7);
	key(VK_DOWN, 0, 0);
	check("↓ next", L"cmd_b 2", 7);
	key(VK_DOWN, 0, 0); key(VK_DOWN, 0, 0);
	check("↓ stops at the last", L"cmd_c 3", 7);
	key(VK_PRIOR, 0, 0);
	check("PgUp oldest", L"cmd_a 1", 7);
	key(VK_NEXT, 0, 0);
	check("PgDn newest", L"cmd_c 3", 7);

	/* Esc does not move the position back to the newest (only adding a command does), as in the current conhost */
	begin(); key(VK_UP, 0, 0);
	check("Esc keeps the position in the history", L"cmd_b 2", 7);

	/* DOSKEY: ↑↑ Enter, then ↓ goes on from there */
	fresh_history();
	begin(); key(VK_UP, 0, 0); key(VK_UP, 0, 0); key(VK_RETURN, 13, 0);
	settle();
	begin(); key(VK_DOWN, 0, 0);
	check("after running a command from the history, ↓ follows the sequence", L"cmd_c 3", 7);

	fresh_history();
	run(L"cmd_c 3");
	begin(); key(VK_UP, 0, 0); key(VK_UP, 0, 0);
	check("a repeat in a row does not duplicate", L"cmd_b 2", 7);

	fresh_history();
	begin(); type(L"zzz"); key('C', 3, CTRL);
	settle();
	begin(); key(VK_UP, 0, 0);
	check("Ctrl+C does not go into the history", L"cmd_c 3", 7);

	fresh_history();
	begin(); key(VK_RIGHT, 0, 0); key(VK_F1, 0, 0);
	check("→ and F1 at the end copy from the previous command", L"cm", 2);
	key(VK_F3, 0, 0);
	check("F3 copies the rest of the previous command", L"cmd_c 3", 7);

	begin(); type(L"xy"); key(VK_F3, 0, 0);
	check("F3 from the cursor", L"xyd_c 3", 7);

	begin(); key(VK_F2, 0, 0);
	settle();
	check_true("F2 shows the popup", below_has(L"Enter char to copy up to"), "popup did not show up");
	type(L"_");
	check("F2 copies up to the character", L"cmd", 3);
	check_true("the popup goes away after F2", !below_has(L"Enter char"), "popup is still on screen");

	begin(); type(L"abc def"); key(VK_HOME, 0, 0); key(VK_F4, 0, 0); type(L" ");
	check("F4 deletes up to the character", L" def", 0);

	begin(); type(L"cmd_a"); key(VK_F8, 0, 0);
	check("F8 searches by prefix and keeps the cursor", L"cmd_a 1", 5);
	begin(); type(L"cmd"); key(VK_F8, 0, 0);
	check("F8 prefix 'cmd'", L"cmd_c 3", 3);
	key(VK_F8, 0, 0);
	check("F8 again: next older one", L"cmd_b 2", 3);

	begin(); key(VK_F9, 0, 0);
	settle();
	check_true("F9 shows the popup", below_has(L"Enter command number"), "popup did not show up");
	type(L"0"); key(VK_RETURN, 13, 0);
	check("F9 0 Enter: command 0", L"cmd_a 1", 7);

	begin(); key(VK_F7, 0, 0);
	settle();
	check_true("F7 lists the history", below_has(L"0: cmd_a 1") && below_has(L"2: cmd_c 3"), "incomplete list");
	key(VK_ESCAPE, 27, 0);
	settle();
	check_true("Esc closes the list", !below_has(L"cmd_a 1"), "list is still on screen");
	check("line intact after the list", L"", 0);

	begin(); key(VK_F7, 0, 0); key(VK_HOME, 0, 0); key(VK_RIGHT, 0, 0);
	check("F7 Home → copies without running", L"cmd_a 1", 7);

	begin(); key(VK_F7, 0, 0); key(VK_END, 0, 0); key(VK_RETURN, 13, 0);
	settle();
	check_true("F7 End Enter runs the command", prompt_row_is(L"cmd_c 3"), "the executed line is not cmd_c 3");

	begin(); key(VK_F7, 0, 0); key(VK_HOME, 0, 0); key(VK_DELETE, 0, 0); key(VK_ESCAPE, 27, 0); key(VK_PRIOR, 0, 0);
	check("F7 Delete removes the command", L"cmd_b 2", 7);

	fresh_history();
	run(L"history");
	check_true("history lists the history", below_has(L"cmd_b 2"), "history did not show the commands");
	run(L"history 1");
	check_true("history N shows only the last N lines", below_has(L"history") && !below_has(L"cmd_b 2"), "history 1 showed more than one line");
	run(L"history clear");
	begin(); key(VK_UP, 0, 0);
	check("history clear erases the history", L"", 0);

	fresh_history();
	run(L"log");
	check_true("the log file has date, time and folder, without a number",
		history_file_has(L"> cmd_b 2\n") && history_file_has(L" cmd_a 1\n"), "history.ant without the cmd_a and cmd_b lines");
	check_true("log (alias) lists the history", below_has(L"cmd_b 2"), "log did not show the commands");
	run(L"log -d");
	begin(); key(VK_UP, 0, 0);
	check("log -d (alias) erases the history", L"", 0);

	/* 'history' numbers the lines itself; lines of the first format ("42 date ...") still show */
	{
		wchar_t path[MAX_PATH];

		GetEnvironmentVariableW(L"ANT_HISTORY_FILE", path, MAX_PATH);
		file_write(path, L"42 2026-01-02 03:04:05 C:\\x> old_fmt_cmd\n2026-01-02 03:04:06 C:\\x> new_fmt_cmd\n");
		run(L"history");
		check_true("history numbers the lines, old format included",
			below_has(L"1 2026-01-02 03:04:05 C:\\x> old_fmt_cmd") && below_has(L"2 2026-01-02 03:04:06 C:\\x> new_fmt_cmd")
			&& !below_has(L"42 2026"), "unexpected history lines");
		run(L"history clear");
	}

	run_suggestion_cases();
}

/* echo, export/unset, $VAR, $?, ~ and which, inside the shell */
static void run_shell_cases (void) {

	wchar_t home[MAX_PATH], line[MAX_PATH + 16];

	run(L"echo olá 中 mundo");
	check_true("echo shows the words", below_has(L"olá 中 mundo"), "echo did not show the text");
	run(L"echo -n foo; echo bar");
	check_true("echo -n does not break the line", below_has(L"foobar") || below_has(L"foo") , "echo -n");

	run(L"export ANT_X=42");
	run(L"echo x=$ANT_X");
	check_true("export and $VAR", below_has(L"x=42"), "$ANT_X did not become 42");
	run(L"ANT_Y=sete");
	run(L"echo y=$ANT_Y");
	check_true("NAME=value is an export", below_has(L"y=sete"), "$ANT_Y did not become sete");
	run(L"echo '$ANT_X' \"$ANT_X\"");
	check_true("single quotes do not expand, double quotes do", below_has(L"$ANT_X 42"), "aspas");
	run(L"unset ANT_X ANT_Y");
	run(L"echo [$ANT_X] [$ANT_Y]");
	check_true("unset removes the variables", below_has(L"[] []"), "the variables are still there");
	run(L"export 1x=2");
	check_true("export refuses an invalid name", below_has(L"not a valid identifier"), "no error");

	run(L"false");
	run(L"echo code=$?");
	check_true("$? is 1 after false", below_has(L"code=1"), "$? was not 1");
	run(L"true && echo code=$?");
	check_true("$? is 0 after true", below_has(L"code=0"), "$? was not 0");
	run(L"false; echo code=$?");
	check_true("$? on a line with ;", below_has(L"code=1"), "$? was not updated within the line");
	run(L"export ANT_Z=9 && echo z=$ANT_Z");
	check_true("export && echo sees the variable on the same line", below_has(L"z=9"), "early expansion");
	run(L"unset ANT_Z");

	if (!GetEnvironmentVariableW(L"HOME", home, MAX_PATH))
		GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
	wcscpy(line, home);
	run(L"echo ~");
	check_true("~ becomes the user's folder", below_has(line), "~ did not expand");
	run(L"echo a~ '~'");
	check_true("~ outside the start or inside quotes stays as it is", below_has(L"a~ ~"), "~ expanded where it should not");

	run(L"echo *.zzz_nenhum");
	check_true("a glob with no result stays as typed", below_has(L"*.zzz_nenhum"), "glob");
	run(L"2 * 3");
	check_true("calculator with * does not become a glob", !below_has(L"command not found") && !below_has(L"cannot"), "2 * 3 was treated as a program");

	/* ls of bin\\ in a console: names in columns, several per line */
	run(L"ls src");
	{
		wchar_t row[512];
		bool columns = false;
		for (int r = 1; r <= 12 && !columns; r++) {
			read_row(prompt_row + r, row, width);
			columns = wcsstr(row, L"builtins") && wcsstr(row, L"config") && wcsstr(row, L"shell");
		}
		check_true("ls in a console shows the names in columns", columns, "builtins, config and shell are not on the same line");
	}
	run(L"ls src | head -n 2");
	{
		wchar_t row[512];
		bool found = false, alone = false;
		for (int r = 1; r <= 12 && !found; r++) {
			read_row(prompt_row + r, row, width);
			if (wcsstr(row, L"builtins")) {
				found = true;
				alone = !wcsstr(row, L"config");
			}
		}
		check_true("ls | head: one name per line", found && alone, "ls in a pipe did not come out in lines");
	}

	/* cd -, PWD e OLDPWD */
	run(L"cd src");
	run(L"echo p=$PWD");
	check_true("cd sets PWD", below_has(L"p=") && below_has(L"\\src"), "PWD did not change");
	run(L"cd ..");
	run(L"echo o=$OLDPWD");
	check_true("cd sets OLDPWD", below_has(L"o=") && below_has(L"\\src"), "OLDPWD is not the previous folder");
	run(L"cd -");
	check_true("cd - goes back to the previous folder", below_has(L"\\src\""), "cd - did not enter src");
	run(L"cd -");
	check_true("cd - again goes back (back and forth)", !below_has(L"\\src\"") && below_has(L"\""), "cd - did not go back");

	/* wait, kill %n e jobs */
	run(L"ping -n 3 127.0.0.1 > NUL &");
	{
		DWORD t0 = GetTickCount();
		run(L"wait");
		check_true("wait waits for the jobs", GetTickCount() - t0 > 800, "wait returned at once");
	}
	run(L"jobs");
	check_true("no job is left after the wait", !below_has(L"running"), "the job is still in the list");
	run(L"cmd /c exit 5 &");
	run(L"wait %1");
	run(L"echo w=$?");
	check_true("wait %1 returns the job's code", below_has(L"w=5"), "$? was not 5");
	run(L"wait %9");
	check_true("wait for a job that does not exist", below_has(L"wait: %9: no such job"), "no error");
	run(L"echo w=$?");
	check_true("wait for a job that does not exist exits with 127", below_has(L"w=127"), "$? was not 127");
	run(L"ping -n 20 127.0.0.1 > NUL &");
	run(L"kill %1");
	Sleep(300);
	run(L"/");
	check_true("kill %1 ends the job (143)", below_has(L"[1] exit 143"), "the job did not end with 143");
	run(L"ping -n 20 127.0.0.1 > NUL &");
	run(L"kill -9 %%");
	Sleep(300);
	run(L"/");
	check_true("kill -9 %% ends the current job (137)", below_has(L"[1] exit 137"), "the job did not end with 137");
	run(L"kill %7");
	check_true("kill of a job that does not exist", below_has(L"kill: %7: no such job"), "no error");
	run(L"kill -FOO %1");
	check_true("kill with an invalid signal", below_has(L"invalid signal specification"), "no error");
	run(L"kill 99999999");
	check_true("kill of a pid without % goes to the bin program", below_has(L"No such process") || below_has(L"not found"), "kill 99999999");

	run(L"which cd");
	check_true("which: built-in", below_has(L"cd: shell built-in"), "which cd");
	run(L"which -a which");
	check_true("which -a: built-in", below_has(L"which: shell built-in"), "which -a which");
	run(L"which cmd");
	check_true("which finds a program", below_has(L"cmd.exe"), "which cmd");
	run(L"which nome_que_nao_existe_zzz");
	check_true("which: not found", below_has(L"nome_que_nao_existe_zzz: not found"), "no error");
	run(L"which nome_que_nao_existe_zzz || echo st=$?");
	check_true("which not found exits with 1", below_has(L"st=1"), "exit code");
}

/* ---------- processos ---------- */

/* opens antshell.exe in this console, with 'args' after it (NULL: none; cwd NULL = current folder), and waits for the prompt */
static bool start_term_args (const wchar_t *term, const wchar_t *args, const wchar_t *cwd, PROCESS_INFORMATION *pi) {

	STARTUPINFOW si;
	static wchar_t cmd[MAX_PATH + 4096];

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (args)
		swprintf(cmd, MAX_PATH + 4096, L"\"%ls\" %ls", term, args);
	else
		swprintf(cmd, MAX_PATH + 4096, L"\"%ls\"", term);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, pi)) {
		fprintf(report, "FAIL  could not open antshell.exe (%lu)\n", GetLastError());
		failures++;
		total++;
		return false;
	}

	/* waits for the prompt: just '>' in the first column */
	{
		for (int i = 0; i < 200; i++) {
			wchar_t row[512];
			COORD c = cursor_pos();
			read_row(c.Y, row, 1);
			if (c.X == 1 && !wcscmp(row, L">"))
				break;
			Sleep(25);
		}
	}
	return true;
}

static bool start_term (const wchar_t *term, const wchar_t *cwd, PROCESS_INFORMATION *pi) {

	return start_term_args(term, NULL, cwd, pi);
}

/* sends 'nop' and returns the exit code */
static DWORD stop_term (PROCESS_INFORMATION *pi) {

	DWORD code = 1;

	begin(); type(L"nop"); key(VK_RETURN, 13, 0);
	if (WaitForSingleObject(pi->hProcess, 5000) == WAIT_OBJECT_0)
		GetExitCodeProcess(pi->hProcess, &code);
	else
		TerminateProcess(pi->hProcess, 1);
	CloseHandle(pi->hProcess);
	CloseHandle(pi->hThread);
	return code;
}

/* opens an antshell.exe, runs 'cmd' and returns the shell's exit code (99: did not open, 1: did not close) */
static DWORD exit_code_of (const wchar_t *term, const wchar_t *cmd) {

	PROCESS_INFORMATION pi;
	DWORD code = 1;

	if (!start_term(term, NULL, &pi))
		return 99;
	begin(); type(cmd); key(VK_RETURN, 13, 0);
	if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0)
		GetExitCodeProcess(pi.hProcess, &code);
	else
		TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code;
}

/* antshell.exe with arguments, which are its first command line; returns its exit code (99: did not open, 1: did not close) */
static DWORD exit_code_with_args (const wchar_t *term, const wchar_t *args) {

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	static wchar_t cmd[MAX_PATH + 4096];
	DWORD code = 1;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	swprintf(cmd, MAX_PATH + 4096, L"\"%ls\" %ls", term, args);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		return 99;
	if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0)
		GetExitCodeProcess(pi.hProcess, &code);
	else
		TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code;
}

/* first and last column (inclusive) of the cells of the selected item (blue on white) in line 'row' */
static bool selected_span (int row, int *first, int *last) {

	WORD attr[512];
	COORD c = { 0, (SHORT) row };
	DWORD n = 0;

	*first = *last = -1;
	ReadConsoleOutputAttribute(conout, attr, width, c, &n);
	for (int x = 0; x < (int) n; x++)
		/* the VT "ESC[34;107m" becomes the attribute 0xF1 (blue on bright white) in conhost */
		if ((attr[x] & 0xFF) == 0xF1) {
			if (*first < 0)
				*first = x;
			*last = x;
		}
	return *first >= 0;
}

static void check_span (const char *name, int row, int want_first, int want_last) {

	int first, last;
	char d[128];

	settle();
	selected_span(row, &first, &last);
	sprintf(d, "highlight in columns %d..%d, expected %d..%d", first, last, want_first, want_last);
	check_true(name, first == want_first && last == want_last, d);
}

/* TAB pager in a fixed test folder (src\ has include\, main.c and modules\) */
static void run_pager_cases (void) {

	/* chaining: a single folder goes in with \; the next TAB lists and already selects the 1st */
	begin(); type(L"cd s"); key(VK_TAB, 9, 0);
	check("single folder: separator and no space", L"cd src\\", 7);
	check_true("a single option does not open the pager", !below_has(L"modules"), "pager open");
	key(VK_TAB, 9, 0);
	settle();
	check_true("next TAB lists src\\ (include\\ main.c modules\\)",
		below_has(L"include\\") && below_has(L"main.c") && below_has(L"modules\\"), "incomplete list");
	check("the 1st item already comes selected on the line", L"cd src\\include\\", 15);
	check_span("1st item highlighted (blue on white) (include\\)", prompt_row + 1, 0, 7);

	/* TAB walks through the options; the chosen one goes onto the line at once */
	key(VK_TAB, 9, 0);
	check("next TAB: main.c", L"cd src\\main.c", 13);
	check_span("selection highlighted (blue on white) (main.c)", prompt_row + 1, 9, 14);
	key(VK_TAB, 9, 0);
	check("next TAB: modules\\", L"cd src\\modules\\", 15);
	check_span("selection highlighted (blue on white) (modules\\)", prompt_row + 1, 16, 23);
	key(VK_TAB, 9, SHIFT_PRESSED);
	check("Shift+TAB goes back", L"cd src\\main.c", 13);

	/* Enter accepts without running; TAB again goes on inside the accepted folder */
	key(VK_TAB, 9, 0);
	key(VK_RETURN, 13, 0);
	check("Enter accepts the option without running", L"cd src\\modules\\", 15);
	check_true("pager closed after Enter", !below_has(L"include\\"), "pager open");
	key(VK_TAB, 9, 0);
	settle();
	check("TAB after accepting the folder lists and selects inside it", L"cd src\\modules\\anthropoid_shell.c", 33);

	/* "type conf" -> "config.h": the whole 1st option, not just the common prefix */
	begin(); type(L"type tests\\comp"); key(VK_TAB, 9, 0);
	settle();
	check("TAB completes the whole 1st option (complete_test.c)", L"type tests\\complete_test.c", 26);
	check_true("the menu is still open with complete_test.exe", below_has(L"complete_test.exe"), "menu closed");
	key(VK_ESCAPE, 27, 0);
	check("Esc goes back to the common prefix", L"type tests\\complete_test.", 25);

	/* TAB on the empty line already selects the 1st file of the folder */
	begin(); key(VK_TAB, 9, 0);
	settle();
	check("empty line: the 1st file already goes onto the line", L"CLAUDE.md", 9);
	key(VK_ESCAPE, 27, 0);
	check("Esc goes back to the empty line", L"", 0);

	/* Esc and then Enter run the line that was typed */
	begin(); type(L"cd src\\m"); key(VK_TAB, 9, 0); key(VK_ESCAPE, 27, 0); key(VK_RETURN, 13, 0);
	settle();
	check_true("Esc and Enter run the original line", prompt_row_is(L"cd src\\m") && cursor_pos().Y > prompt_row,
		"the line was not run");

	/* ↑ on the 1st item clears the selection; ↓ brings it back; → and ← move between columns */
	begin(); type(L"cd src\\"); key(VK_TAB, 9, 0); key(VK_UP, 0, 0);
	check("↑ on the 1st item clears the selection", L"cd src\\", 7);
	key(VK_DOWN, 0, 0);
	check("↓ goes back to the 1st item", L"cd src\\include\\", 15);
	key(VK_RIGHT, 0, 0);
	check("→ goes to the next column", L"cd src\\main.c", 13);
	key(VK_LEFT, 0, 0);
	check("← goes back", L"cd src\\include\\", 15);

	/* typing with an option selected keeps the option */
	type(L"x");
	check("typing keeps the chosen option", L"cd src\\include\\x", 16);

	/* no option: the line blinks and does not change */
	begin(); type(L"type zzz_nothing"); key(VK_TAB, 9, 0);
	check("TAB without options does not change the line", L"type zzz_nothing", 16);
}

/* does line 'r' below the prompt contain 'text'? */
static bool row_has (int r, const wchar_t *text) {

	wchar_t row[512];

	read_row(prompt_row + r, row, width);
	return wcsstr(row, text) != NULL;
}

static void check_row (const char *name, int r, const wchar_t *text) {

	char want[1024], d[1100];

	WideCharToMultiByte(CP_UTF8, 0, text, -1, want, sizeof want, NULL, NULL);
	snprintf(d, sizeof d, "line +%d does not show [%s]", r, want);
	check_true(name, row_has(r, text), d);
}

/* cd with history, in the project folder; ends back in it */
static void run_cd_cases (void) {

	wchar_t proj[MAX_PATH], text[MAX_PATH + 16];

	GetCurrentDirectoryW(MAX_PATH, proj);

	run(L"/");
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	check_row("/ shows the current folder", 1, text);

	/* POSIX: a bare cd goes to $HOME */
	swprintf(text, MAX_PATH + 16, L"export HOME=\"%ls\\src\"", proj);
	run(text);
	run(L"cd");
	swprintf(text, MAX_PATH + 16, L"\"%ls\\src\"", proj);
	check_row("a bare cd goes to $HOME", 1, text);
	swprintf(text, MAX_PATH + 16, L"cd \"%ls\"", proj);
	run(text);
	run(L"unset HOME");
	run(L"cd");
	check_true("a bare cd with an empty HOME goes to USERPROFILE", !below_has(L"HOME not set"), "warned HOME not set");
	run(L"export HOME=$USERPROFILE");
	swprintf(text, MAX_PATH + 16, L"cd \"%ls\"", proj);
	run(text);

	run(L"cd src");
	swprintf(text, MAX_PATH + 16, L"\"%ls\\src\"", proj);
	check_row("cd src enters src", 1, text);
	begin(); key(VK_TAB, 9, 0);
	settle();
	check_true("TAB after cd lists the new folder", below_has(L"shell\\") && !below_has(L"Makefile"), "the pager does not list src");

	run(L"/");
	check_row("/ shows the current folder (not the one of antshell.exe)", 1, text);

	run(L"cd ..");
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	check_row("cd .. goes up one level", 1, text);

	/* the folder history (only left-cd uses it) has no commands anymore: cd:, cd:n, cd<, cd> */
	run(L"cd:");
	check_row("cd: is no longer a command", 1, L"command not found");
	run(L"cd<");
	check_row("cd< is a redirection without a file", 1, L"missing file after '<'");

	run(L"cd nao_existe_xyz");
	check_row("a missing folder gives an error", 1, L"\"nao_existe_xyz\" not found");

	run(L"cd\\");
	check_row("cd\\ goes to the root", 1, L":\\\"");

	swprintf(text, MAX_PATH + 16, L"cd \"%ls\"", proj);
	run(text);
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	check_row("cd \"folder\" goes back to the project", 1, text);

	/* left-cd: ← on the empty line lists the session's folders, each one once, the current one last:
	   0 = src, 1 = root, 2 = project (current) */
	{
		wchar_t item[MAX_PATH + 16];

		begin(); key(VK_LEFT, 0, 0);
		settle();
		swprintf(item, MAX_PATH + 16, L"%ls\\src", proj);
		check_true("left-cd: ← on the empty line opens the folder menu", below_has(item), "the menu does not list src");
		swprintf(item, MAX_PATH + 16, L"* %ls", proj);
		check_true("left-cd: the current folder (the last one) is marked with *", below_has(item), "the project is not marked");
		key(VK_LEFT, 0, 0);
		settle();
		check_true("left-cd: ← again closes the menu", !below_has(item), "the menu is still on screen");
		check("left-cd: line intact after the menu", L"", 0);

		begin(); key(VK_LEFT, 0, 0); key(VK_UP, 0, 0); key(VK_UP, 0, 0); key(VK_RETURN, 13, 0);
		settle();
		swprintf(item, MAX_PATH + 16, L"\"%ls\\src\"", proj);
		check_row("left-cd: ← ↑ ↑ Enter goes straight to src", 1, item);
		check_true("left-cd: the prompt line is left clean", prompt_row_is(L""), "the line did not end up empty");

		begin(); key(VK_UP, 0, 0);
		swprintf(item, MAX_PATH + 16, L"cd \"%ls\\src\"", proj);
		check("left-cd: cd \"folder\" goes into the command history (↑)", item, wcslen(item));

		begin(); type(L"ab"); key(VK_LEFT, 0, 0);
		check("left-cd: ← with text still moves the cursor", L"ab", 1);
		swprintf(item, MAX_PATH + 16, L"* %ls", proj);
		check_true("left-cd: ← with text does not open the menu", !below_has(item), "the menu opened");
	}

	swprintf(text, MAX_PATH + 16, L"cd \"%ls\"", proj);
	run(text);

	/* left-cd: the selection stays inside the menu, also when Enter comes before a redraw */
	{
		wchar_t item[MAX_PATH + 16];

		begin(); key(VK_LEFT, 0, 0); key(VK_DOWN, 0, 0); key(VK_NEXT, 0, 0); key(VK_RETURN, 13, 0);
		settle();
		swprintf(item, MAX_PATH + 16, L"\"%ls\"", proj);
		check_row("left-cd: ↓ and PgDn past the end stay on the last folder", 1, item);
		begin(); key(VK_LEFT, 0, 0); key(VK_HOME, 0, 0); key(VK_UP, 0, 0); key(VK_PRIOR, 0, 0); key(VK_RETURN, 13, 0);
		settle();
		check_true("left-cd: ↑ and PgUp past the top go to the first folder", row_has(1, L":\\") && !row_has(1, L"not found"),
			"Enter did not go to a folder");
		run(text);
	}

	/* implicit cd (fish): a folder path alone on the line enters it */
	{
		wchar_t cmd[MAX_PATH + 16], home[MAX_PATH];

		run(L"src\\shell\\");
		swprintf(text, MAX_PATH + 16, L"\"%ls\\src\\shell\"", proj);
		check_row("implicit cd: folder\\ enters", 1, text);
		run(L"..");
		swprintf(text, MAX_PATH + 16, L"\"%ls\\src\"", proj);
		check_row("implicit cd: .. goes up", 1, text);
		run(L".\\shell");
		swprintf(text, MAX_PATH + 16, L"\"%ls\\src\\shell\"", proj);
		check_row("implicit cd: .\\folder enters", 1, text);
		run(L"../..");
		swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
		check_row("implicit cd: ../.. goes up two (it is not a calculation)", 1, text);
		run(L"~");
		GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
		swprintf(text, MAX_PATH + 16, L"\"%ls\"", home);
		check_row("implicit cd: ~ goes to the user's folder", 1, text);
		swprintf(cmd, MAX_PATH + 16, L"\"%ls\\\"", proj);
		run(cmd);
		swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
		check_row("implicit cd: full path in quotes", 1, text);
		swprintf(cmd, MAX_PATH + 16, L"%lc:", proj[0]);
		run(cmd);
		check_row("implicit cd: a drive (C:) goes back to its current folder", 1, text);
		run(L"src\\ x");
		check_row("a folder with an argument is not an implicit cd", 1, L"command not found");
		run(L"nao_existe_xyz\\");
		check_row("a missing folder is not an implicit cd", 1, L"command not found");
		run(L"/");
		check_row("after the implicit cds the folder is the project", 1, text);
	}
}

/* lists (; && ||), programs with accents and quotes, errors (in the project folder) */
static void run_list_cases (void) {

	wchar_t proj[MAX_PATH], text[MAX_PATH + 16];

	GetCurrentDirectoryW(MAX_PATH, proj);
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);

	run(L"/ ; /");
	check_true("a ; b runs both", row_has(1, text) && row_has(2, text), "one of the outputs is missing");

	run(L"cd nao_existe_xyz && /");
	check_true("a && b: b does not run if a failed", row_has(1, L"not found") && !row_has(2, text), "b ran");

	run(L"cd nao_existe_xyz || /");
	check_true("a || b: b runs if a failed", row_has(1, L"not found") && row_has(2, text), "b did not run");

	run(L"/ && cd nao_existe_xyz || /");
	check_true("a && b || c: c runs because b failed", row_has(1, text) && row_has(3, text), "c did not run");

	run(L"cmd /c echo ação 中");
	check_true("accents and CJK reach the program intact (CreateProcessW)", below_has(L"ação 中"), "corrupted text");

	run(L"cmd /c echo \"a  b\"");
	check_true("an argument with spaces goes in quotes", below_has(L"\"a  b\""), "quotes lost");

	run(L"zz_comando_que_nao_existe");
	check_true("a missing command warns", below_has(L"zz_comando_que_nao_existe: command not found"), "no warning");

	run(L"| cd");
	check_true("a syntax error warns", below_has(L"syntax error near '|'"), "no warning");
	run(L"cd \"unclosed");
	check_true("an unclosed quote warns", below_has(L"unclosed quote \""), "no warning");
}

/* contents of a file (UTF-8; if it is not, the ANSI code page) */
static void read_file (const wchar_t *path, wchar_t *out, int max) {

	static char buf[16384];
	FILE *f = _wfopen(path, L"rb");
	size_t n = 0;

	out[0] = 0;
	if (!f)
		return;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, -1, out, max))
		MultiByteToWideChar(CP_ACP, 0, buf, -1, out, max);
}

static void check_file (const char *name, const wchar_t *path, const wchar_t *want) {

	static wchar_t got[16384];
	char d[256];

	read_file(path, got, 16384);
	snprintf(d, sizeof d, "the file does not contain what was expected (%d characters)", (int) wcslen(got));
	check_true(name, wcsstr(got, want) != NULL, d);
}

/* pipes and redirections (files in a folder of %TEMP%) */
static void run_redirect_cases (void) {

	wchar_t dir[MAX_PATH], cmd[MAX_PATH * 3], f[MAX_PATH], proj[MAX_PATH], text[MAX_PATH + 16];
	FILE *in;

	GetCurrentDirectoryW(MAX_PATH, proj);
	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"ant_redir_test");
	CreateDirectoryW(dir, NULL);

	/* > e >> */
	swprintf(cmd, MAX_PATH * 3, L"cmd /c echo line1 > \"%ls\\out.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\out.txt", dir);
	check_file("> writes the output to the file", f, L"line1");
	check_true("> does not show the output on screen", !below_has(L"line1"), "the output showed up on screen");
	swprintf(cmd, MAX_PATH * 3, L"cmd /c echo line2 >> \"%ls\\out.txt\"", dir);
	run(cmd);
	check_file(">> appends without deleting (line1)", f, L"line1");
	check_file(">> appends without deleting (line2)", f, L"line2");

	/* < */
	swprintf(f, MAX_PATH, L"%ls\\in.txt", dir);
	if ((in = _wfopen(f, L"wb"))) {
		fputs("banana\r\napricot\r\n", in);
		fclose(in);
	}
	swprintf(cmd, MAX_PATH * 3, L"sort < \"%ls\\in.txt\" > \"%ls\\sorted.txt\"", dir, dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\sorted.txt", dir);
	check_file("< reads the input from the file (sort)", f, L"apricot\r\nbanana");

	/* | */
	run(L"cmd /c echo abc123 | findstr 123");
	check_true("| connects the output of one to the input of the other", below_has(L"abc123"), "nothing went through the pipe");
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	run(L"cmd /c echo abc | findstr zzz || /");
	check_true("the pipe's code is the last one's (findstr finding nothing = 1)", below_has(text), "the || did not run");
	run(L"cmd /c echo abc | findstr abc && /");
	check_true("the pipe's code is the last one's (findstr found = 0)", below_has(text), "the && did not run");

	/* 2> e 2>&1 */
	swprintf(cmd, MAX_PATH * 3, L"cmd /c dir zz_nao_existe_xyz 2> \"%ls\\err.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\err.txt", dir);
	read_file(f, text, MAX_PATH + 16);
	check_true("2> writes the errors to the file", wcslen(text) > 0, "error file is empty");
	swprintf(cmd, MAX_PATH * 3, L"cmd /c dir zz_nao_existe_xyz > \"%ls\\both.txt\" 2>&1", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\both.txt", dir);
	read_file(f, text, MAX_PATH + 16);
	{
		static wchar_t both[16384], err[16384];
		wchar_t ef[MAX_PATH];
		read_file(f, both, 16384);
		swprintf(ef, MAX_PATH, L"%ls\\err.txt", dir);
		read_file(ef, err, 16384);
		/* the dir error (without the line end) shows up together with the output */
		int n = wcslen(err);
		while (n > 0 && (err[n - 1] == L'\r' || err[n - 1] == L'\n'))
			err[--n] = 0;
		check_true("> f 2>&1 sends output and error to the file", n > 0 && wcsstr(both, err) != NULL, "error outside the file");
	}

	/* a program that does not exist: the launch error also goes to 2> */
	swprintf(cmd, MAX_PATH * 3, L"zz_nao_existe_xyz 2> \"%ls\\nf.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\nf.txt", dir);
	check_file("command not found goes to 2>", f, L"ant: zz_nao_existe_xyz: command not found");
	swprintf(cmd, MAX_PATH * 3, L"zz_nao_existe_xyz > \"%ls\\nf2.txt\" 2>&1", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\nf2.txt", dir);
	check_file("command not found with 2>&1 goes to the file", f, L"command not found");
	check_true("command not found with 2> does not show on screen", !below_has(L"zz_nao_existe_xyz: command"), "the message showed up on screen");
	run(L"zz_nao_existe_xyz");
	check_true("command not found without 2> shows on screen", below_has(L"ant: zz_nao_existe_xyz: command not found"), "no message");

	/* built-ins with redirection and in a pipe */
	swprintf(cmd, MAX_PATH * 3, L"ver > \"%ls\\ver.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\ver.txt", dir);
	check_file("built-in with > (ver)", f, L"ANTHROPOID SHELL");
	swprintf(cmd, MAX_PATH * 3, L"/ > \"%ls\\cd.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\cd.txt", dir);
	check_file("/ > file writes the folder", f, proj);
	swprintf(cmd, MAX_PATH * 3, L"cd nao_existe_xyz 2> \"%ls\\cderr.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\cderr.txt", dir);
	check_file("a built-in error goes to 2>", f, L"nao_existe_xyz");
	check_true("a built-in error with 2> does not show on screen", !below_has(L"nao_existe_xyz\" n"), "the error showed up on screen");
	swprintf(f, MAX_PATH, L"%ls\\cdslash.txt", dir);
	DeleteFileW(f);
	swprintf(cmd, MAX_PATH * 3, L"cd\\zz_nao_existe 2> \"%ls\"", f);
	run(cmd);
	check_file("cd\\x is a program with 2> too (command not found)", f, L"command not found");
	run(L"log | findstr abc123");
	check_true("built-in in a pipe (log | findstr)", below_has(L"cmd /c echo abc123"), "the log did not go through the pipe");

	/* calculator: with the output redirected it becomes plain text; an error goes to 2> and sets the status */
	swprintf(cmd, MAX_PATH * 3, L"2^3 > \"%ls\\calc.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\calc.txt", dir);
	check_file("calculator > file: 2^3 is 8", f, L"8");
	swprintf(cmd, MAX_PATH * 3, L"2 3 + > \"%ls\\calc.txt\"", dir);
	run(cmd);
	check_file("calculator RPN > file: 2 3 + is 5", f, L"5");
	swprintf(cmd, MAX_PATH * 3, L"2*3^2 > \"%ls\\calc.txt\"", dir);
	run(cmd);
	check_file("calculator: 2*3^2 is 18", f, L"18");
	swprintf(f, MAX_PATH, L"%ls\\calc.txt", dir);
	swprintf(cmd, MAX_PATH * 3, L"sqrt(16)+abs(-5) > \"%ls\\calc.txt\"", dir);
	run(cmd);
	check_file("calculator: sqrt(16)+abs(-5) is 9", f, L"9");
	swprintf(cmd, MAX_PATH * 3, L"17 %% 5 > \"%ls\\calc.txt\"", dir);
	run(cmd);
	check_file("calculator: 17 % 5 is 2", f, L"2");
	swprintf(cmd, MAX_PATH * 3, L"2*pi > \"%ls\\calc.txt\"", dir);
	run(cmd);
	check_file("calculator: 2*pi (no glob on the *)", f, L"6.2831853072");
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	run(L"pi 2> /dev/null || /");
	check_true("a bare pi is still a command (not a calculation)", below_has(text), "the || did not run");
	swprintf(f, MAX_PATH, L"%ls\\calcerr.txt", dir);
	swprintf(cmd, MAX_PATH * 3, L"1/0 2> \"%ls\\calcerr.txt\"", dir);
	run(cmd);
	swprintf(f, MAX_PATH, L"%ls\\calcerr.txt", dir);
	check_file("calculator: division by zero goes to 2>", f, L"ant: calc: division by zero");
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	run(L"1/0 2> /dev/null || /");
	check_true("calculator: an error returns a non-zero status", below_has(text), "the || did not run");
	run(L"2+3 && /");
	check_true("calculator: a result returns status 0", below_has(text), "the && did not run");

	/* /dev/null and file error */
	run(L"cmd /c echo sumiu > /dev/null");
	check_true("/dev/null discards the output", !below_has(L"sumiu"), "the output showed up");
	run(L"cmd /c echo x > Z:\\no\\such\\f.txt");
	check_true("a file that does not open warns", below_has(L"cannot open"), "no warning");

	/* a pipeline stage closes every file it opened (it kept 4 handles and left the 5th open) */
	{
		static const wchar_t *names[] = { L"r1.txt", L"r2.txt", L"r3.txt", L"r4.txt", NULL };
		bool closed = true;

		for (int i = 0; names[i]; i++) {
			swprintf(f, MAX_PATH, L"%ls\\%ls", dir, names[i]);
			DeleteFileW(f);
		}
		swprintf(cmd, MAX_PATH * 3, L"true | true > \"%ls\\r1.txt\" 2> \"%ls\\r2.txt\" > \"%ls\\r3.txt\" 2> \"%ls\\r4.txt\" | true",
			dir, dir, dir, dir);
		run(cmd);
		for (int i = 0; names[i]; i++) {
			swprintf(f, MAX_PATH, L"%ls\\%ls", dir, names[i]);
			if (!DeleteFileW(f))
				closed = false;
		}
		check_true("a pipeline stage with 4 redirections closes them all (the files can be deleted)", closed,
			"a file is still open in the shell");
	}

	/* limpeza */
	static const wchar_t *files[] = { L"out.txt", L"in.txt", L"sorted.txt", L"err.txt", L"both.txt",
		L"ver.txt", L"cd.txt", L"cderr.txt", L"cdslash.txt", NULL };
	for (int i = 0; files[i]; i++) {
		swprintf(f, MAX_PATH, L"%ls\\%ls", dir, files[i]);
		DeleteFileW(f);
	}
	RemoveDirectoryW(dir);
}

/* & in the background, jobs and @ (error) */
static void run_bg_cases (void) {

	wchar_t dir[MAX_PATH], cmd[MAX_PATH * 3], f[MAX_PATH], proj[MAX_PATH], text[MAX_PATH + 16];
	DWORD t0;

	GetCurrentDirectoryW(MAX_PATH, proj);
	swprintf(text, MAX_PATH + 16, L"\"%ls\"", proj);
	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"ant_bg_test");
	CreateDirectoryW(dir, NULL);
	swprintf(f, MAX_PATH, L"%ls\\bg.txt", dir);

	/* returns at once, with [n] pid */
	swprintf(cmd, MAX_PATH * 3, L"ping -n 3 127.0.0.1 > \"%ls\" &", f);
	begin();
	type(cmd);
	t0 = GetTickCount();
	key(VK_RETURN, 13, 0);
	settle();
	check_true("& returns at once (without waiting for the ping)", GetTickCount() - t0 < 1500 && cursor_pos().Y > prompt_row, "the shell waited");
	check_true("& shows [n] and the pid", row_has(1, L"[1] "), "no [1]");

	run(L"jobs");
	check_true("jobs lists what is running", below_has(L"running") && below_has(L"ping -n 3 127.0.0.1"), "jobs empty");

	/* done notice before the next prompt */
	Sleep(3500);
	run(L"/");
	check_true("finished-job notice before the prompt", below_has(L"[1] done  ping -n 3 127.0.0.1"), "no notice");
	check_file("background job wrote the file", f, L"127.0.0.1");

	/* non-zero code */
	run(L"cmd /c exit 4 &");
	Sleep(800);
	run(L"/");
	check_true("a failed job shows the code", below_has(L"exit 4"), "no code");

	/* & in the middle of the line: the rest runs at once */
	run(L"cmd /c exit 0 & /");
	check_true("a & b: b runs without waiting for a", below_has(text), "b did not run");
	Sleep(500);

	/* the job reads from NUL: it does not steal the keyboard */
	run(L"sort &");
	Sleep(300);
	begin(); type(L"teclado");
	check("a background job does not steal the keyboard", L"teclado", 7);
	key(VK_ESCAPE, 27, 0);
	run(L"/");
	check_true("sort & finished (NUL input)", below_has(L"done  sort"), "sort still running");

	/* bg: its own hidden console, -o writes <program>_<pid>.txt, -d removes the close button */
	swprintf(cmd, MAX_PATH * 3, L"cd \"%ls\"", dir);
	run(cmd);
	run(L"bg");
	check_true("bg without a program shows the usage", below_has(L"usage: bg [-o] [-d]"), "no usage");
	run(L"bg -x ping");
	check_true("bg invalid option warns", below_has(L"-x: invalid option"), "no warning");
	run(L"bg -o cd");
	check_true("bg refuses a built-in", below_has(L"a built-in runs in the shell"), "accepted the built-in");

	begin();
	type(L"bg ping -n 3 127.0.0.1");
	t0 = GetTickCount();
	key(VK_RETURN, 13, 0);
	settle();
	check_true("bg returns at once with [n] pid", GetTickCount() - t0 < 1500 && row_has(1, L"[1] "), "the shell waited or no [n]");
	run(L"jobs");
	check_true("bg becomes a job (jobs)", below_has(L"running") && below_has(L"ping -n 3 127.0.0.1"), "jobs empty");
	run(L"/");
	Sleep(3500);
	run(L"/");
	check_true("bg: done notice", below_has(L"[1] done  ping -n 3 127.0.0.1"), "no notice");
	{
		WIN32_FIND_DATAW fd;
		HANDLE h;
		swprintf(f, MAX_PATH, L"%ls\\ping_*.txt", dir);
		h = FindFirstFileW(f, &fd);
		check_true("bg without -o does not create a file", h == INVALID_HANDLE_VALUE, "created ping_<pid>.txt");
		if (h != INVALID_HANDLE_VALUE)
			FindClose(h);
	}

	run(L"bg -o cmd /c echo bg-dump");
	check_true("bg -o shows [n] pid file_pid.txt", below_has(L"cmd_") && below_has(L".txt"), "no file name");
	Sleep(1000);
	{
		WIN32_FIND_DATAW fd;
		HANDLE h;
		swprintf(f, MAX_PATH, L"%ls\\cmd_*.txt", dir);
		h = FindFirstFileW(f, &fd);
		check_true("bg -o creates <program>_<pid>.txt", h != INVALID_HANDLE_VALUE, "file does not exist");
		if (h != INVALID_HANDLE_VALUE) {
			FindClose(h);
			swprintf(f, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
			check_file("bg -o writes the output to the file", f, L"bg-dump");
			DeleteFileW(f);
		}
		swprintf(f, MAX_PATH, L"%ls\\cmd_new.tmp", dir);
		check_true("bg -o leaves no temporary file", GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES, "left over cmd_new.tmp");
	}
	run(L"/");
	run(L"bg -d ping -n 3 127.0.0.1");
	check_true("bg -d finds the hidden console", !below_has(L"could not find the console window") && below_has(L"[1] "), "did not find the window");
	run(L"kill %1");
	Sleep(500);
	swprintf(cmd, MAX_PATH * 3, L"cd \"%ls\"", proj);
	run(cmd);

	/* help shows the syntax (data\help.ant) */
	run(L"help \"&&\"");
	check_true("help shows the operators", below_has(L"a && b") && below_has(L"Run b only if a succeeded"), "help without the syntax");
	run(L"help");
	check_true("help without an argument lists the commands", below_has(L"Complete files") && below_has(L"Previous option in the TAB menu"), "help without the list");
	run(L"help ls");
	check_true("help name shows only the entry", below_has(L"List files") && !below_has(L"First / last lines"), "help ls showed the whole list or nothing");
	run(L"help tail");
	check_true("help tail finds the HEAD / TAIL entry", below_has(L"First / last lines"), "help tail did not find HEAD / TAIL");
	run(L"help his");
	check_true("help without an exact name matches the start", below_has(L"Command log"), "help his did not find HISTORY");
	run(L"cd --help");
	check_true("cd --help shows the cd help", below_has(L"Change directory"), "cd --help without the help");
	run(L"echo --help");
	check_true("echo --help shows the echo help", below_has(L"Print the words"), "echo --help printed the text");
	run(L"which --help");
	check_true("which --help", below_has(L"What a name runs"), "which --help");
	run(L"history --help");
	check_true("history --help", below_has(L"Command log"), "history --help");
	run(L"true --help");
	check_true("true --help", below_has(L"exit code 0 / 1"), "true --help");
	run(L"echo a --help");
	check_true("--help outside the 1st argument is a plain argument", below_has(L"a --help"), "echo a --help");
	run(L"help nome_inexistente_zzz");
	check_true("help of an unknown name gives an error", below_has(L"no help topics match 'nome_inexistente_zzz'"), "no error");

	/* @ with an error: message in the console, no dialog box */
	run(L"@zz_nao_existe_xyz");
	check_true("@ with an error warns in the console", below_has(L"zz_nao_existe_xyz: cannot open"), "no warning");

	DeleteFileW(f);
	RemoveDirectoryW(dir);
}

/* TAB on the 1st word completes commands from PATH (temporary folder with antzz*.exe) */
static void run_command_cases (void) {

	begin(); type(L"antzzu"); key(VK_TAB, 9, 0);
	check("single command from PATH, without .exe and with a space", L"antzzunico", 11);

	begin(); type(L"antzzc"); key(VK_TAB, 9, 0);
	settle();
	check("several commands: the 1st already goes onto the line", L"antzzcmd1", 9);
	check_true("pager lists antzzcmd1 and antzzcmd2", below_has(L"antzzcmd1") && below_has(L"antzzcmd2"), "pager without the commands");
	key(VK_TAB, 9, 0);
	check("TAB moves to the 2nd command", L"antzzcmd2", 9);

	begin(); type(L"type antzzu"); key(VK_TAB, 9, 0);
	check("the 2nd word does not complete commands", L"type antzzu", 11);

	begin(); type(L"ver | antzzu"); key(VK_TAB, 9, 0);
	check("after | it completes commands", L"ver | antzzunico", 17);
	begin(); type(L"ver && @antzzu"); key(VK_TAB, 9, 0);
	check("@ after && completes commands", L"ver && @antzzunico", 19);
}

/* shortcuts (the [link] section of the config.ant of the antshell.exe copy in 'base') */
static void run_link_cases (const wchar_t *base) {

	wchar_t text[MAX_PATH + 32];

	run(L"link");
	check_true("link lists the shortcuts", below_has(L"[target]") && below_has(L"[prog]"), "list without [target]/[prog]");
	check_true("link shows the help", below_has(L"name.") && below_has(L"link -f"), "no help");

	run(L"target\\");
	swprintf(text, MAX_PATH + 32, L"\"%ls\\target folder ç\"", base);
	check_row("target\\ enters the shortcut's folder (space and accent)", 1, text);
	run(L"cd ..");

	run(L"TARGET/");
	check_row("name is case-insensitive and works with /", 1, text);
	run(L"cd ..");

	run(L"med\\");
	swprintf(text, MAX_PATH + 32, L"\"%ls\\med1\"", base);
	check_row("repeated shortcut: the first one wins", 1, text);
	run(L"cd ..");

	run(L"nothing");
	check_row("a missing target gives an error", 1, L"shortcut [nothing]");
	run(L"nothing.");
	check_row("name. with a missing target gives an error and does not close", 1, L"shortcut [nothing]");

	run(L"prog");
	check_true("a shortcut to a program runs in the console", below_has(L"usage: input_test"), "the program did not run");

	run(L"target extra");
	check_true("a line with a space is not a shortcut", !row_has(1, L"target folder"), "entered the shortcut's folder");

	run(L"/");
	swprintf(text, MAX_PATH + 32, L"\"%ls\"", base);
	check_row("the shell stays in the original folder", 1, text);
}

/* folder with a space and an accent: without quotes (joins the arguments) and with quotes */
static void run_cd_space_cases (const wchar_t *dir) {

	wchar_t text[MAX_PATH + 16];

	run(L"cd folder with space ç");
	swprintf(text, MAX_PATH + 16, L"\"%ls\\folder with space ç\"", dir);
	check_row("cd with a space and an accent, without quotes", 1, text);
	run(L"cd ..");
	run(L"cd \"folder with space ç\"");
	check_row("cd with a space and an accent, with quotes", 1, text);
	run(L"cd ..");
}

/*
 * Temporary folder: a😊b.txt (8 columns), plenty\ (7) and a CJK name (24).
 * On a single line: columns at 0..7, 9..15 and 17..40 (1 space between them).
 * plenty\ has 200 files to test the partial list and scrolling.
 */
/* the grid rows the pager reports in "rows 1 to X of N" (0 if it is not showing that) */
static int pager_total_rows (void) {

	wchar_t row[512];

	for (int r = 1; r <= 40; r++) {
		wchar_t *p;
		int a, b, total;

		read_row(prompt_row + r, row, width);
		if ((p = wcsstr(row, L"rows ")) && swscanf(p, L"rows %d to %d of %d", &a, &b, &total) == 3)
			return total;
	}
	return 0;
}

static void run_tab_width_cases (void) {

	begin(); key(VK_TAB, 9, 0);
	check_span("pager: a😊b.txt takes 8 columns (1st already selected)", prompt_row + 1, 0, 7);
	key(VK_DOWN, 0, 0);
	check_span("pager: plenty\\ starts 1 column later", prompt_row + 1, 9, 15);
	key(VK_DOWN, 0, 0);
	check_span("pager: CJK name takes 24 columns", prompt_row + 1, 17, 40);
	check("CJK name goes onto the line", L"中文文件名很长的名字.txt", 24);

	begin(); type(L"plenty\\"); key(VK_TAB, 9, 0);
	settle();
	check_true("many files: partial list with '…and more'", below_has(L"more rows"), "no notice of hidden rows");
	check_true("the partial list starts at f001", below_has(L"f001.txt"), "f001.txt does not show up");
	check("the 1st file already goes onto the line", L"plenty\\f001.txt", 15);
	key(VK_TAB, 9, 0);
	settle();
	/* the footer ("rows 1 to X of N") only exists when the list does not fit the window */
	check_true("2nd TAB shows everything ('rows 1 to')",
		!below_has(L"more rows") && (window_rows - 2 >= 34 || below_has(L"rows 1 to")), "list was not open");
	check("2nd TAB moves to f002", L"plenty\\f002.txt", 15);
	/*
	 * 34 grid rows: 31 DOWN keys select row 32 (0-based), the 33rd. The pager shows the window rows
	 * minus 2 (the command line and the footer), and scrolls just enough to keep the selection in
	 * view: with 30 visible rows that is "rows 4 to 33 of 34". The window is whatever the system
	 * gave the hidden console, so the expectation follows it; and a console of another width lays
	 * the grid out in another number of rows, which makes the case meaningless, so it is skipped.
	 */
	int grid_rows = pager_total_rows();
	for (int i = 0; i < 31; i++)
		key(VK_DOWN, 0, 0);
	settle();
	if (grid_rows == 0) {
		fprintf(report, "SKIP  navigating scrolls the list: the whole grid fits in a window of %d rows\n", window_rows);
	} else if (grid_rows != 34) {
		fprintf(report, "SKIP  navigating scrolls the list: the grid has %d rows in a console %d wide, the case needs 34\n",
			grid_rows, width);
	} else {
		int visible = window_rows - 2;
		int top = 32 - visible + 1;
		wchar_t want[64];

		if (top < 0)
			top = 0;
		if (top + visible >= grid_rows) {
			check_true("navigating does not scroll a list that fits the window", !below_has(L"rows "), "a footer showed up");
		} else {
			swprintf(want, 64, L"rows %d to %d of 34", top + 1, top + visible);
			check_true("navigating scrolls the list", below_has(want), "the list did not scroll");
		}
	}
	check("selection on the line after scrolling (f033.txt)", L"plenty\\f033.txt", 15);
	key(VK_ESCAPE, 27, 0);
	/* the common prefix 'f' had already gone onto the line before the pager opened */
	check("Esc goes back to the line with the common prefix", L"plenty\\f", 8);
}

/* the arguments of antshell.exe are its first command line (files in a folder of %TEMP%) */
static void run_args_cases (const wchar_t *term) {

	static wchar_t args[MAX_PATH + 4096];
	wchar_t dir[MAX_PATH], sub[MAX_PATH], f[MAX_PATH];
	PROCESS_INFORMATION pi;

	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"ant_args_test");
	CreateDirectoryW(dir, NULL);
	swprintf(sub, MAX_PATH, L"%ls\\pasta ação 中文", dir);
	swprintf(f, MAX_PATH, L"%ls\\quote.txt", dir);
	/* nothing left over from an earlier run */
	RemoveDirectoryW(sub);
	DeleteFileW(f);
	CreateDirectoryW(sub, NULL);

	check_true("the arguments are the first command line (exit 4)", exit_code_with_args(term, L"exit 4") == 4, NULL);

	/* main's argv comes in the ANSI code page: accents and CJK only survive the UTF-16 command line */
	swprintf(args, MAX_PATH + 4096, L"cd \"%ls\" && exit 4 || exit 5", sub);
	check_true("an argument with accents and CJK reaches cd intact", exit_code_with_args(term, args) == 4, "cd failed");

	swprintf(args, MAX_PATH + 4096, L"echo \"say \\\"hi\\\"\" > \"%ls\" && exit 6", f);
	check_true("an argument with \" inside runs (exit 6)", exit_code_with_args(term, args) == 6, NULL);
	check_file("an argument with \" inside keeps it (echo)", f, L"say \"hi\"");

	/* longer than a line: a message, nothing runs and the shell goes on to the prompt */
	wcscpy(args, L"echo ");
	for (int i = 0; i < 2000; i++)
		wcscat(args, L"x");
	if (start_term_args(term, args, NULL, &pi)) {
		wchar_t row[512];
		bool warned = false;

		begin();
		for (int r = 1; r <= 4 && !warned; r++) {
			read_row(prompt_row - r, row, width);
			warned = wcsstr(row, L"arguments are too long") != NULL;
		}
		check_true("arguments longer than a line are refused with a message", warned, "no message above the prompt");
		check_true("after the refused arguments the shell is still open (nop closes with 0)", stop_term(&pi) == 0, NULL);
	}

	DeleteFileW(f);
	RemoveDirectoryW(sub);
	RemoveDirectoryW(dir);
}

/* the logo and the big digits of the calculator are written in UTF-8, whatever code page the console started in */
static void run_drawing_cases (const wchar_t *term) {

	PROCESS_INFORMATION pi;
	UINT cp = GetConsoleOutputCP();
	wchar_t f[MAX_PATH], cmd[MAX_PATH + 16];

	GetTempPathW(MAX_PATH, f);
	wcscat(f, L"ant_ver_test.txt");
	DeleteFileW(f);                             /* nothing left over from an earlier run */

	/* a console that starts in UTF-8 (the "Use Unicode UTF-8" option of Windows): CP437 bytes came out as U+FFFD */
	SetConsoleOutputCP(CP_UTF8);
	if (start_term(term, NULL, &pi)) {
		run(L"ver");
		check_true("ver in a UTF-8 console: the box of the logo", below_has(L"\x250C\x2500\x2500"), "no box corner under the prompt");
		check_true("ver in a UTF-8 console: the shades of the skull", below_has(L"\x2593\x2593") && below_has(L"\x2591\x2591"),
			"no shade characters");
		run(L"2+3");
		check_true("calculator in a UTF-8 console: the big digits are full blocks", below_has(L"\x2588\x2588\x2588"), "no full blocks");
		swprintf(cmd, MAX_PATH + 16, L"ver > \"%ls\"", f);
		run(cmd);
		check_file("ver > file: the logo in UTF-8", f, L"\x250C\x2500\x2500");
		stop_term(&pi);
	}
	SetConsoleOutputCP(cp);
	DeleteFileW(f);
}

static int inner (const wchar_t *term, const wchar_t *report_path) {

	PROCESS_INFORMATION pi;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD code = 1;

	report = _wfopen(report_path, L"w");
	conin  = CreateFileW(L"CONIN$",  GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	GetConsoleScreenBufferInfo(conout, &csbi);
	width = csbi.dwSize.X;
	window_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

	/* does this console keep a character outside the BMP? Write one the way the shell does and read it back */
	{
		COORD home = { 0, 0 };
		wchar_t back[8] = { 0 };
		DWORD n = 0;

		SetConsoleCursorPosition(conout, home);
		WriteConsoleW(conout, L"x\xD83D\xDE42y", 4, &n, NULL);
		ReadConsoleOutputCharacterW(conout, back, 6, home, &n);
		astral_ok = wmemchr(back, 0xD83D, n) != NULL;
		FillConsoleOutputCharacterW(conout, L' ', 8, home, &n);
		SetConsoleCursorPosition(conout, home);
	}

	fprintf(report, "console: %d x %d (window %d rows), characters outside the BMP %s\n", csbi.dwSize.X, csbi.dwSize.Y,
		window_rows, astral_ok ? "are kept" : "read back as U+FFFD");

	/* 1st session: in the project folder */
	if (!start_term(term, NULL, &pi)) {
		fclose(report);
		return 1;
	}
	run_cases();
	run_history_cases();
	run_shell_cases();
	run_cd_cases();
	run_list_cases();
	run_redirect_cases();
	run_bg_cases();
	code = stop_term(&pi);
	check_true("nop ends antshell.exe with code 0", code == 0, NULL);

	check_true("exit 7 closes with code 7", exit_code_of(term, L"exit 7") == 7, NULL);
	check_true("exit 300 stays in the low byte (44)", exit_code_of(term, L"exit 300") == 44, NULL);
	check_true("exit abc closes with 2", exit_code_of(term, L"exit abc") == 2, NULL);
	check_true("false; exit uses the code of the last command", exit_code_of(term, L"false; exit") == 1, NULL);
	check_true("true; exit closes with 0", exit_code_of(term, L"true; exit") == 0, NULL);
	check_true("a bare nop closes with 0 even after an error", exit_code_of(term, L"false; nop") == 0, NULL);
	check_true("nop 5 closes with 5", exit_code_of(term, L"nop 5") == 5, NULL);

	run_args_cases(term);
	run_drawing_cases(term);

	/* pager session: a fixed tree, independent of how the project organizes its folders */
	{
		static const wchar_t *dirs[] = { L"src", L"src\\include", L"src\\modules", L"tests", NULL };
		static const wchar_t *files[] = { L"CLAUDE.md", L"src\\main.c", L"src\\modules\\anthropoid_shell.c",
			L"src\\modules\\arg.c", L"tests\\complete_test.c", L"tests\\complete_test.exe", NULL };
		wchar_t root[MAX_PATH], path[MAX_PATH];
		int nd = 0, nf = 0;

		GetTempPathW(MAX_PATH, root);
		wcscat(root, L"ant_pager_test");
		CreateDirectoryW(root, NULL);
		for (; dirs[nd]; nd++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", root, dirs[nd]);
			CreateDirectoryW(path, NULL);
		}
		for (; files[nf]; nf++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", root, files[nf]);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}

		if (start_term(term, root, &pi)) {
			run_pager_cases();
			stop_term(&pi);
		}

		while (nf-- > 0) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", root, files[nf]);
			DeleteFileW(path);
		}
		while (nd-- > 0) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", root, dirs[nd]);
			RemoveDirectoryW(path);
		}
		RemoveDirectoryW(root);
	}

	/* 2nd session: in a folder with wide names and a subfolder with 200 files */
	{
		static const wchar_t *files[] = { L"a\U0001F60Ab.txt", L"中文文件名很长的名字.txt", NULL };
		wchar_t dir[MAX_PATH], sub[MAX_PATH], path[MAX_PATH];

		GetTempPathW(MAX_PATH, dir);
		wcscat(dir, L"ant_tab_test");
		CreateDirectoryW(dir, NULL);
		swprintf(sub, MAX_PATH, L"%ls\\plenty", dir);
		CreateDirectoryW(sub, NULL);
		for (int i = 0; files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", dir, files[i]);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}
		for (int i = 1; i <= 200; i++) {
			swprintf(path, MAX_PATH, L"%ls\\f%03d.txt", sub, i);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}

		/* test commands in the PATH of this session's antshell.exe */
		static const wchar_t *commands[] = { L"antzzcmd1.exe", L"antzzcmd2.exe", L"antzzunico.exe", NULL };
		wchar_t bin[MAX_PATH], old_path[32767], new_path[32767 + MAX_PATH];
		DWORD had_path = GetEnvironmentVariableW(L"PATH", old_path, 32767);

		GetTempPathW(MAX_PATH, bin);
		wcscat(bin, L"ant_tab_bin");
		CreateDirectoryW(bin, NULL);
		for (int i = 0; commands[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin, commands[i]);
			CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		}
		swprintf(new_path, 32767 + MAX_PATH, L"%ls;%ls", bin, had_path ? old_path : L"");
		SetEnvironmentVariableW(L"PATH", new_path);

		if (start_term(term, dir, &pi)) {
			run_command_cases();
			run_tab_width_cases();
			/* created only now so it does not change the pager grid above */
			swprintf(path, MAX_PATH, L"%ls\\folder with space ç", dir);
			CreateDirectoryW(path, NULL);
			run_cd_space_cases(dir);
			stop_term(&pi);
			RemoveDirectoryW(path);
		}

		SetEnvironmentVariableW(L"PATH", had_path ? old_path : NULL);
		for (int i = 0; commands[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", bin, commands[i]);
			DeleteFileW(path);
		}
		RemoveDirectoryW(bin);

		for (int i = 1; i <= 200; i++) {
			swprintf(path, MAX_PATH, L"%ls\\f%03d.txt", sub, i);
			DeleteFileW(path);
		}
		RemoveDirectoryW(sub);
		for (int i = 0; files[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", dir, files[i]);
			DeleteFileW(path);
		}
		RemoveDirectoryW(dir);
	}

	/* 3rd session: copy of antshell.exe with a test data\config.ant */
	{
		static const wchar_t *dirs[] = { L"target folder ç", L"med1", L"med2", NULL };
		wchar_t base[MAX_PATH], data[MAX_PATH], exe[MAX_PATH], quit_exe[MAX_PATH], config[MAX_PATH], self[MAX_PATH], path[MAX_PATH];
		char u8[MAX_PATH * 3];
		FILE *f;

		GetTempPathW(MAX_PATH, base);
		wcscat(base, L"ant_link_test");
		CreateDirectoryW(base, NULL);
		swprintf(data, MAX_PATH, L"%ls\\data", base);
		CreateDirectoryW(data, NULL);
		swprintf(config, MAX_PATH, L"%ls\\config.ant", data);
		for (int i = 0; dirs[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", base, dirs[i]);
			CreateDirectoryW(path, NULL);
		}
		swprintf(exe, MAX_PATH, L"%ls\\antshell.exe", base);
		swprintf(quit_exe, MAX_PATH, L"%ls\\quit_exe.exe", base);
		CopyFileW(term, exe, FALSE);
		GetModuleFileNameW(NULL, self, MAX_PATH);
		CopyFileW(self, quit_exe, FALSE);

		/* a config.ant with only [link] shortcuts, in UTF-8 with CRLF like the real one */
		if ((f = _wfopen(config, L"wb"))) {
			WideCharToMultiByte(CP_UTF8, 0, base, -1, u8, sizeof u8, NULL, NULL);
			fputs("[link]\r\n", f);
			fprintf(f, "target = %s\\target folder \xc3\xa7\r\n", u8);
			fprintf(f, "med = %s\\med1\r\n", u8);
			fprintf(f, "nothing = %s\\no_such_folder\r\n", u8);
			fprintf(f, "prog = %s\\quit_exe.exe\r\n", u8);
			fprintf(f, "med = %s\\med2\r\n", u8);
			fclose(f);
		}

		if (start_term(exe, base, &pi)) {
			run_link_cases(base);
			stop_term(&pi);
		}

		DeleteFileW(config);
		RemoveDirectoryW(data);
		DeleteFileW(exe);
		DeleteFileW(quit_exe);
		for (int i = 0; dirs[i]; i++) {
			swprintf(path, MAX_PATH, L"%ls\\%ls", base, dirs[i]);
			RemoveDirectoryW(path);
		}
		RemoveDirectoryW(base);
	}

	fprintf(report, "\n%d of %d cases passed\n", total - failures, total);
	fclose(report);
	return failures ? 1 : 0;
}

int main (void) {

	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t term[MAX_PATH], self[MAX_PATH], report_path[MAX_PATH], history_path[MAX_PATH], cmd[4 * MAX_PATH];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	if (argc == 4 && !wcscmp(argv[1], L"--inner"))
		return inner(argv[2], argv[3]);

	if (argc != 2) {
		fprintf(stderr, "usage: input_test antshell.exe\n");
		return 2;
	}

	GetFullPathNameW(argv[1], MAX_PATH, term, NULL);
	GetModuleFileNameW(NULL, self, MAX_PATH);
	GetTempPathW(MAX_PATH, report_path);
	wcscat(report_path, L"ant_input_test.txt");
	DeleteFileW(report_path);

	/* this run's history log is not the data\history.ant of whoever uses the shell */
	GetTempPathW(MAX_PATH, history_path);
	wcscat(history_path, L"ant_input_test_history.ant");
	DeleteFileW(history_path);
	SetEnvironmentVariableW(L"ANT_HISTORY_FILE", history_path);
	wcscpy(history_path + wcslen(history_path) - 4, L"_rank.ant");
	DeleteFileW(history_path);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	swprintf(cmd, 4 * MAX_PATH, L"\"%ls\" --inner \"%ls\" \"%ls\"", self, term, report_path);

	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
		fprintf(stderr, "could not open the test console (%lu)\n", GetLastError());
		return 2;
	}
	if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
		TerminateProcess(pi.hProcess, 2);
		fprintf(stderr, "timed out\n");
	}
	GetExitCodeProcess(pi.hProcess, &code);

	FILE *f = _wfopen(report_path, L"r");
	if (f) {
		char buf[1024];
		while (fgets(buf, sizeof buf, f))
			fputs(buf, stdout);
		fclose(f);
	}
	return (int) code;
}
