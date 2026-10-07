/*************************************************************
 * Test of note.                                             *
 *                                                           *
 * usage: note_test.exe note.exe                             *
 *                                                           *
 * The process relaunches itself in a new hidden console,    *
 * opens note.exe there, injects keys and mouse events       *
 * (WriteConsoleInputW), and checks the screen               *
 * (ReadConsoleOutputCharacterW) and the bytes written.      *
 * The clipboard is saved and restored around the run.       *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>
#include <wctype.h>
#include <windows.h>

#define CTRL   LEFT_CTRL_PRESSED
#define SHIFT  SHIFT_PRESSED
#define ALT    LEFT_ALT_PRESSED
#define GUTTER 5   /* line numbers: " 123 " */

static HANDLE conin;
static FILE *report;
static int failures = 0, total = 0;
static wchar_t note[MAX_PATH], dir[MAX_PATH];
static PROCESS_INFORMATION child;

/* ---------- input ---------- */

static void settle (void) {

	DWORD pending = 1;

	for (int i = 0; i < 400 && pending; i++) {
		GetNumberOfConsoleInputEvents(conin, &pending);
		if (pending)
			Sleep(5);
	}
	Sleep(80);
}

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

/* one key press (note redraws between presses) */
static void key (WORD vk, wchar_t ch, DWORD mods) {

	if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE)
		mods |= ENHANCED_KEY;
	send_record(vk, ch, mods, true);
	send_record(vk, ch, mods, false);
	settle();
}

static void ctrl (char letter) {

	key((WORD) letter, (wchar_t) (letter - 'A' + 1), CTRL);
}

/* Alt pressed and released alone */
static void alt_tap (void) {

	send_record(VK_MENU, 0, ALT, true);
	send_record(VK_MENU, 0, 0, false);
	settle();
}

/* text as a paste brings it: all queued at once; '\r' is Enter and '\t' is Tab */
static void paste (const wchar_t *s) {

	static INPUT_RECORD buf[4096];
	DWORD n;
	int k = 0;

	for (; *s && k < 4094; s++) {
		WORD vk = *s == L'\r' ? VK_RETURN : *s == L'\t' ? VK_TAB : 0;
		if (!vk && *s < 128)
			vk = LOBYTE(VkKeyScanW(*s));
		memset(&buf[k], 0, sizeof buf[k]);
		buf[k].EventType = KEY_EVENT;
		buf[k].Event.KeyEvent.bKeyDown = TRUE;
		buf[k].Event.KeyEvent.wRepeatCount = 1;
		buf[k].Event.KeyEvent.wVirtualKeyCode = vk;
		buf[k].Event.KeyEvent.uChar.UnicodeChar = *s;
		buf[k+1] = buf[k];
		buf[k+1].Event.KeyEvent.bKeyDown = FALSE;
		k += 2;
	}
	WriteConsoleInputW(conin, buf, k, &n);
	settle();
}

/* text typed key by key */
static void type (const wchar_t *s) {

	for (; *s; s++) {
		if (*s == L'\r')
			key(VK_RETURN, L'\r', 0);
		else {
			wchar_t one[2] = { *s, 0 };
			paste(one);
		}
	}
}

static HANDLE open_screen (void) {

	return CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static void mouse (int x, int y, DWORD buttons, DWORD flags) {

	HANDLE h = open_screen();
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	INPUT_RECORD ir;
	DWORD n;

	GetConsoleScreenBufferInfo(h, &csbi);
	CloseHandle(h);
	memset(&ir, 0, sizeof ir);
	ir.EventType = MOUSE_EVENT;
	ir.Event.MouseEvent.dwMousePosition.X = (SHORT) (csbi.srWindow.Left + x);
	ir.Event.MouseEvent.dwMousePosition.Y = (SHORT) (csbi.srWindow.Top + y);
	ir.Event.MouseEvent.dwButtonState = buttons;
	ir.Event.MouseEvent.dwEventFlags = flags;
	WriteConsoleInputW(conin, &ir, 1, &n);
	settle();
}

static void click (int x, int y) {

	mouse(x, y, FROM_LEFT_1ST_BUTTON_PRESSED, 0);
	mouse(x, y, 0, 0);
}

/* ---------- screen ---------- */

/* row of the visible window (negative: from the bottom), without trailing spaces */
static void screen_row (int row, wchar_t *out, int max) {

	HANDLE h = open_screen();
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	COORD c;
	DWORD n = 0;
	int width;

	GetConsoleScreenBufferInfo(h, &csbi);
	width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	c.X = csbi.srWindow.Left;
	c.Y = (SHORT) (row >= 0 ? csbi.srWindow.Top + row : csbi.srWindow.Bottom + 1 + row);
	ReadConsoleOutputCharacterW(h, out, width < max - 1 ? width : max - 1, c, &n);
	while (n > 0 && out[n-1] == L' ')
		n--;
	out[n] = 0;
	CloseHandle(h);
}

/* legacy attribute of one cell (the console maps 24-bit colors to the nearest of 16) */
static WORD cell_attr (int x, int row) {

	HANDLE h = open_screen();
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	COORD c;
	WORD a = 0;
	DWORD n;

	GetConsoleScreenBufferInfo(h, &csbi);
	c.X = (SHORT) (csbi.srWindow.Left + x);
	c.Y = (SHORT) (csbi.srWindow.Top + row);
	ReadConsoleOutputAttribute(h, &a, 1, c, &n);
	CloseHandle(h);
	return a;
}

static bool screen_has (const wchar_t *s) {

	wchar_t row[1024];

	for (int r = 0; r < 60; r++) {
		screen_row(r, row, 1024);
		if (wcsstr(row, s))
			return true;
	}
	return false;
}

/* ---------- checks ---------- */

static void result (const char *name, bool ok, const wchar_t *got) {

	total++;
	if (ok)
		fprintf(report, "  ok    %s\n", name);
	else {
		failures++;
		fprintf(report, "  FAIL  %s\n", name);
		if (got) {
			char u8[2048];
			WideCharToMultiByte(CP_UTF8, 0, got, -1, u8, sizeof u8, NULL, NULL);
			fprintf(report, "        screen: [%s]\n", u8);
		}
	}
	fflush(report);
}

/* whole screen row */
static void check_row (const char *name, int row, const wchar_t *expected) {

	wchar_t got[1024];

	screen_row(row, got, 1024);
	result(name, !wcscmp(got, expected), got);
}

/* text of screen row 'row' after the line numbers */
static void check_text (const char *name, int row, const wchar_t *expected) {

	wchar_t got[1024];

	screen_row(row, got, 1024);
	result(name, !wcscmp(wcslen(got) > GUTTER ? got + GUTTER : L"", expected), got);
}

static void check_row_has (const char *name, int row, const wchar_t *part) {

	wchar_t got[1024];

	screen_row(row, got, 1024);
	result(name, wcsstr(got, part) != NULL, got);
}

/* status bar */
/* status bar, with "Ln 3/10" read as "Ln 3" (the total has its own checks) */
static void check_status (const char *name, const wchar_t *part) {

	wchar_t got[1024], *ln, *slash;

	screen_row(-1, got, 1024);
	if ((ln = wcsstr(got, L"Ln ")) != NULL && (slash = wcschr(ln, L'/')) != NULL && slash < wcschr(ln, L',')) {
		wchar_t *end = slash + 1;
		while (iswdigit(*end))
			end++;
		memmove(slash, end, (wcslen(end) + 1) * sizeof(wchar_t));
	}
	result(name, wcsstr(got, part) != NULL, got);
}

static void check_screen (const char *name, const wchar_t *part, bool expected) {

	result(name, screen_has(part) == expected, NULL);
}

static void check_file (const char *name, const wchar_t *file, const char *expected, int len) {

	char got[4096];
	wchar_t path[MAX_PATH];
	int n = 0;
	FILE *f;

	swprintf(path, MAX_PATH, L"%ls\\%ls", dir, file);
	f = _wfopen(path, L"rb");
	if (f) {
		n = (int) fread(got, 1, sizeof got, f);
		fclose(f);
	}
	total++;
	if (f && n == len && !memcmp(got, expected, len))
		fprintf(report, "  ok    %s\n", name);
	else {
		failures++;
		fprintf(report, "  FAIL  %s\n        file (%d bytes): [", name, f ? n : -1);
		for (int i = 0; i < n; i++)
			fprintf(report, got[i] >= 32 && got[i] < 127 ? "%c" : "\\x%02x", (unsigned char) got[i]);
		fprintf(report, "]\n");
	}
	fflush(report);
}

static void check_clipboard (const char *name, const wchar_t *expected) {

	wchar_t got[1024] = L"";

	if (OpenClipboard(NULL)) {
		HANDLE h = GetClipboardData(CF_UNICODETEXT);
		const wchar_t *p = h ? GlobalLock(h) : NULL;
		if (p) {
			wcsncpy(got, p, 1023);
			GlobalUnlock(h);
		}
		CloseClipboard();
	}
	result(name, !wcscmp(got, expected), got);
}

static void write_file (const wchar_t *file, const char *data, int len) {

	wchar_t path[MAX_PATH];
	FILE *f;

	swprintf(path, MAX_PATH, L"%ls\\%ls", dir, file);
	f = _wfopen(path, L"wb");
	fwrite(data, 1, len, f);
	fclose(f);
}

/* ---------- process ---------- */

static void start_exe (const wchar_t *exe, const wchar_t *file) {

	STARTUPINFOW si;
	wchar_t cmd[3 * MAX_PATH];

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (file)
		swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" %ls", exe, file);
	else
		swprintf(cmd, 3 * MAX_PATH, L"\"%ls\"", exe);
	FlushConsoleInputBuffer(conin);
	CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &child);
	Sleep(300);
	settle();
}

static void start (const wchar_t *file) {

	start_exe(note, file);
}

static bool finished (void) {

	return WaitForSingleObject(child.hProcess, 1000) == WAIT_OBJECT_0;
}

static void stop (void) {

	if (!finished())
		TerminateProcess(child.hProcess, 1);
	CloseHandle(child.hProcess);
	CloseHandle(child.hThread);
}

/* Esc and "Don't Save" / "Exit" */
static void quit (void) {

	key(VK_ESCAPE, 27, 0);
	if (screen_has(L"Save changes"))
		key('N', L'n', 0);
	else
		key('E', L'e', 0);
	stop();
}

/* ---------- cases ---------- */

static void test_new_file (void) {

	fprintf(report, "new file, typing, indentation and Ctrl+S\n");
	start(L"new.txt");

	check_row_has("menu bar shows the menus", 0, L"File  Edit  Search  View  Help");
	check_row_has("menu bar shows the file name", 0, L"new.txt");
	check_status("status: New file", L"New file");
	check_status("status: CRLF by default", L"CRLF");
	check_text("line numbers", 1, L"");
	check_row("gutter of line 1", 1, L"   1");

	type(L"if (x) {\r");
	type(L"    y = 1;\r");
	check_text("Enter repeats the indentation", 2, L"    y = 1;");
	check_status("cursor at line 3, column 5", L"Ln 3, Col 5");
	check_row_has("status shows the line out of the total", -1, L"Ln 3/3, Col 5");
	check_row_has("menu bar marks the change", 0, L"new.txt *");

	for (int i = 0; i < 4; i++)
		key(VK_BACK, 8, 0);
	type(L"}");
	ctrl('S');
	check_status("Ctrl+S: Saved", L"Saved new.txt (3 lines)");
	check_screen("Ctrl+S removes the *", L"new.txt *", false);
	check_file("file written as UTF-8 with CRLF", L"new.txt", "if (x) {\r\n    y = 1;\r\n}", 23);

	key(VK_ESCAPE, 27, 0);
	check_screen("Esc without changes: Exit note?", L"Exit note?", true);
	key(VK_ESCAPE, 27, 0);
	check_screen("Esc in the dialog cancels", L"Exit note?", false);
	result("note is still open", !finished(), NULL);

	key(VK_ESCAPE, 27, 0);
	key('E', L'e', 0);
	result("E in the dialog exits", finished(), NULL);
	stop();
}

static void test_lf_utf8 (void) {

	/* "ação\tfim" + ESC + "\n" + "segunda\n" */
	static const char data[] = "a\xc3\xa7\xc3\xa3o\tfim\x1b\nsegunda\n";

	fprintf(report, "LF file with accents, a tab and a control character\n");
	write_file(L"lf.txt", data, sizeof data - 1);
	start(L"lf.txt");

	check_text("accents, tab (4) and control shown as '?'", 1, L"ação    fim?");
	check_text("second line", 2, L"segunda");
	check_status("status: LF", L"LF");
	check_status("status: UTF-8", L"UTF-8");

	key(VK_END, 0, 0);
	check_status("End: column 13 (the tab counts 4)", L"Col 13");
	key(VK_DOWN, 0, 0);
	check_status("Down keeps the column up to the end of a shorter line", L"Ln 2, Col 8");
	key(VK_HOME, 0, 0);
	key(VK_BACK, 8, 0);
	check_text("Backspace at the start joins the lines", 1, L"ação    fim?segunda");
	key(VK_DELETE, 0, 0);
	check_text("Delete removes the next character", 1, L"ação    fim?egunda");
	key(VK_HOME, 0, CTRL);
	key(VK_DELETE, 0, 0);
	key(VK_DELETE, 0, 0);
	check_text("Ctrl+Home and Delete on accented letters", 1, L"ão  fim?egunda");

	key(VK_ESCAPE, 27, 0);
	check_screen("Esc with changes asks to save", L"Save changes to lf.txt?", true);
	key(VK_RIGHT, 0, 0);
	key(VK_RETURN, L'\r', 0);
	result("Don't Save exits", finished(), NULL);
	stop();
	check_file("file untouched", L"lf.txt", data, sizeof data - 1);

	start(L"lf.txt");
	key(VK_END, 0, CTRL);
	type(L"x");
	ctrl('S');
	check_file("saving keeps LF", L"lf.txt", "a\xc3\xa7\xc3\xa3o\tfim\x1b\nsegunda\nx", sizeof data);
	quit();
}

static void test_ansi (void) {

	fprintf(report, "ANSI file (not UTF-8)\n");
	write_file(L"ansi.txt", "caf\xe9\r\n", 6);
	start(L"ansi.txt");
	check_text("read in the ANSI code page", 1, L"café");
	check_status("status: ANSI", L"ANSI");
	key(VK_END, 0, 0);
	type(L"!");
	ctrl('S');
	check_file("written in ANSI", L"ansi.txt", "caf\xe9!\r\n", 7);
	quit();
}

static void test_paste (void) {

	fprintf(report, "a paste does not repeat the indentation\n");
	start(L"paste.txt");
	paste(L"a {\r    b;\r}");
	check_text("line 1", 1, L"a {");
	check_text("line 2 keeps the pasted indentation, not doubled", 2, L"    b;");
	check_text("line 3 without indentation", 3, L"}");
	ctrl('Z');
	check_text("one Ctrl+Z undoes the whole paste", 1, L"");
	ctrl('Y');
	check_text("one Ctrl+Y brings it back", 3, L"}");
	key(VK_ESCAPE, 27, 0);
	key('N', L'n', 0);
	result("N exits without saving", finished(), NULL);
	stop();
}

static void test_save_as (void) {

	fprintf(report, "untitled: Ctrl+S asks for the name\n");
	start(NULL);
	check_row_has("menu bar: Untitled", 0, L"Untitled");
	type(L"hi");
	ctrl('S');
	check_screen("Save As dialog", L"File name:", true);
	type(L"untitled.txt");
	check_screen("the name shows in the field", L"untitled.txt", true);
	key(VK_RETURN, L'\r', 0);
	check_row_has("the name goes to the menu bar", 0, L"untitled.txt");
	check_file("written", L"untitled.txt", "hi", 2);
	key(VK_ESCAPE, 27, 0);
	key(VK_RETURN, L'\r', 0);
	result("Enter on Exit exits", finished(), NULL);
	stop();
}

static void test_undo (void) {

	fprintf(report, "undo and redo\n");
	start(L"undo.txt");
	type(L"abc def\r");
	type(L"ghi");
	ctrl('Z');
	check_text("Ctrl+Z undoes the last typed run", 2, L"");
	check_status("the cursor goes back", L"Ln 2, Col 1");
	ctrl('Z');
	check_text("Ctrl+Z undoes the Enter", 2, L"");
	check_status("back on line 1", L"Ln 1, Col 8");
	ctrl('Z');
	check_text("Ctrl+Z undoes the first run", 1, L"");
	ctrl('Z');
	check_status("nothing more to undo", L"Nothing to undo");
	ctrl('Y');
	ctrl('Y');
	ctrl('Y');
	check_text("Ctrl+Y redoes line 1", 1, L"abc def");
	check_text("Ctrl+Y redoes line 2", 2, L"ghi");
	ctrl('S');
	check_screen("saved: no *", L"undo.txt *", false);
	ctrl('Z');
	check_row_has("undo after saving: modified again", 0, L"undo.txt *");
	ctrl('Y');
	check_screen("redo back to the saved state: not modified", L"undo.txt *", false);
	type(L"!");
	check_row_has("typing after saving is a new change", 0, L"undo.txt *");
	quit();
}

static void test_selection (void) {

	fprintf(report, "selection, clipboard and indentation\n");
	start(L"sel.txt");
	type(L"hello world");
	key(VK_HOME, 0, 0);
	key(VK_RIGHT, 0, CTRL | SHIFT);
	check_status("Shift+Ctrl+Right selects a word", L"6 selected");
	ctrl('X');
	check_text("Ctrl+X cuts", 1, L"world");
	check_clipboard("the clipboard has the text", L"hello ");
	key(VK_END, 0, 0);
	ctrl('V');
	check_text("Ctrl+V pastes", 1, L"worldhello");
	ctrl('C');
	check_clipboard("Ctrl+C without selection copies the line", L"worldhello \r\n");
	ctrl('V');
	check_text("pasting a copied line puts it above", 1, L"worldhello");
	check_text("... and keeps the original below", 2, L"worldhello");
	check_status("the cursor stays on its line", L"Ln 2");

	ctrl('A');
	check_status("Ctrl+A selects everything", L"2 lines selected");
	type(L"a");
	check_text("typing replaces the selection", 1, L"a");
	check_text("... all of it", 2, L"");

	paste(L"\rb\rc");
	key(VK_UP, 0, 0);
	key(VK_UP, 0, 0);
	key(VK_HOME, 0, 0);
	key(VK_DOWN, 0, SHIFT);
	key(VK_DOWN, 0, SHIFT);
	key(VK_TAB, L'\t', 0);
	check_text("Tab indents the selected lines", 1, L"    a");
	check_text("... the second one", 2, L"    b");
	check_text("... not the line where the selection ends at column 1", 3, L"c");
	key(VK_TAB, L'\t', SHIFT);
	check_text("Shift+Tab unindents", 1, L"a");
	check_text("... the second one", 2, L"b");
	quit();
}

static void test_find_replace (void) {

	fprintf(report, "find, replace and go to line\n");
	write_file(L"find.txt", "one two\r\nthree two\r\nfour", 24);
	start(L"find.txt");

	ctrl('F');
	check_screen("Find dialog", L"Find what:", true);
	type(L"two");
	key(VK_RETURN, L'\r', 0);
	check_status("finds the first match", L"3 selected   Ln 1, Col 8");
	key(VK_F3, 0, 0);
	check_status("F3 finds the next", L"Ln 2, Col 10");
	key(VK_F3, 0, 0);
	check_status("F3 wraps around", L"Search wrapped to the beginning");
	check_status("... to the first match", L"Ln 1, Col 8");
	key(VK_F3, 0, SHIFT);
	check_status("Shift+F3 finds the previous (wrapping)", L"Ln 2, Col 10");

	ctrl('F');
	key(VK_END, 0, 0);
	for (int i = 0; i < 3; i++)
		key(VK_BACK, 8, 0);
	type(L"xyz");
	key(VK_RETURN, L'\r', 0);
	check_status("not found", L"Not found: xyz");

	key(VK_HOME, 0, CTRL);
	ctrl('H');
	check_screen("Replace dialog", L"Replace with:", true);
	for (int i = 0; i < 3; i++)
		key(VK_BACK, 8, 0);
	type(L"TWO");
	key(VK_BACK, 8, 0);
	key(VK_BACK, 8, 0);
	key(VK_BACK, 8, 0);
	type(L"two");
	key(VK_TAB, L'\t', 0);
	type(L"2");
	key(VK_RETURN, L'\r', 0);
	check_screen("asks at each match", L"Replace this occurrence?", true);
	key('R', L'r', 0);
	check_text("Replace replaced the first", 1, L"one 2");
	key('S', L's', 0);
	check_text("Skip left the second", 2, L"three two");
	check_status("reports the count", L"1 occurrence replaced");

	ctrl('H');
	key(VK_RETURN, L'\r', 0);
	key('A', L'a', 0);
	check_text("All replaces the rest", 2, L"three 2");
	ctrl('Z');
	check_text("one Ctrl+Z undoes a replace session", 2, L"three two");

	ctrl('G');
	check_screen("Go to Line dialog", L"Line number (1-3):", true);
	type(L"3");
	key(VK_RETURN, L'\r', 0);
	check_status("Ctrl+G goes to the line", L"Ln 3, Col 1");
	quit();
}

static void test_menu (void) {

	fprintf(report, "menu bar\n");
	write_file(L"menu.txt", "alpha\r\nbeta", 11);
	start(L"menu.txt");

	alt_tap();
	key(VK_DOWN, 0, 0);
	check_screen("Alt, Down opens the File menu", L"Save As...", true);
	key(VK_ESCAPE, 27, 0);
	check_screen("Esc closes it", L"Save As...", false);
	result("... and does not exit", !screen_has(L"Exit note?"), NULL);

	key('E', L'e', ALT);
	check_screen("Alt+E opens the Edit menu", L"Select All", true);
	key('A', L'a', 0);
	check_status("the item letter runs it (Select All)", L"2 lines selected");

	key(VK_F10, 0, 0);
	key(VK_RIGHT, 0, 0);
	key(VK_RIGHT, 0, 0);
	key(VK_RIGHT, 0, 0);
	key(VK_RETURN, L'\r', 0);
	check_screen("F10 and arrows reach the View menu", L"Line Numbers", true);
	key(VK_DOWN, 0, 0);
	key(VK_RETURN, L'\r', 0);
	check_row("Down skips the separator, Enter runs Line Numbers", 1, L"alpha");

	key('V', L'v', ALT);
	key('L', L'l', 0);
	check_text("and shows them again", 1, L"alpha");

	key(VK_F1, 0, 0);
	check_screen("F1 shows the shortcuts", L"Keyboard Shortcuts", true);
	key(VK_ESCAPE, 27, 0);
	quit();
}

static void test_mouse (void) {

	fprintf(report, "mouse\n");
	write_file(L"mouse.txt", "first line\r\nsecond line\r\nthird", 30);
	start(L"mouse.txt");

	click(GUTTER + 3, 2);
	check_status("click puts the cursor there", L"Ln 2, Col 4");
	mouse(GUTTER + 2, 2, FROM_LEFT_1ST_BUTTON_PRESSED, 0);
	mouse(GUTTER + 4, 3, FROM_LEFT_1ST_BUTTON_PRESSED, MOUSE_MOVED);
	mouse(GUTTER + 4, 3, 0, 0);
	check_status("dragging selects", L"2 lines selected   Ln 3, Col 5");
	mouse(GUTTER + 8, 1, FROM_LEFT_1ST_BUTTON_PRESSED, 0);
	mouse(GUTTER + 8, 1, 0, 0);
	mouse(GUTTER + 8, 1, FROM_LEFT_1ST_BUTTON_PRESSED, DOUBLE_CLICK);
	mouse(GUTTER + 8, 1, 0, 0);
	check_status("double click selects the word", L"4 selected");

	click(1, 0);
	check_screen("click on File opens the menu", L"Save As...", true);
	click(1, 0);
	check_screen("a second click closes it", L"Save As...", false);

	click(21, 0);
	check_screen("click on View", L"Line Numbers", true);
	click(24, 4);
	check_row("clicking the item runs it", 1, L"first line");
	quit();
}

static void test_syntax (void) {

	fprintf(report, "C syntax colors\n");
	write_file(L"code.c", "int x = 1; // note\r\n/* a\r\nb */ return", 36);
	start(L"code.c");
	check_status("status: C", L"   C   ");

	/* foreground only: the cursor line has its own background */
	WORD keyword = cell_attr(GUTTER + 0, 1) & 15, name = cell_attr(GUTTER + 4, 1) & 15;
	WORD number = cell_attr(GUTTER + 8, 1) & 15, comment = cell_attr(GUTTER + 11, 1) & 15;
	result("type and identifier have different colors", keyword != name, NULL);
	result("number has its own color", number != name, NULL);
	result("comment has its own color", comment != name, NULL);
	result("a block comment goes on in the next line", (cell_attr(GUTTER, 3) & 15) == comment, NULL);
	result("and ends at */", (cell_attr(GUTTER + 5, 3) & 15) != comment, NULL);
	quit();

	fprintf(report, "config file colors\n");
	write_file(L"x.cfg", "[colors]\r\ntext = #FFFFFF on #000000\r\n# comment", 44);
	start(L"x.cfg");
	check_status("status: Config", L"   Config   ");
	WORD section = cell_attr(GUTTER, 1) & 15, key = cell_attr(GUTTER, 2) & 15;
	WORD color = cell_attr(GUTTER + 7, 2) & 15, word = cell_attr(GUTTER + 15, 2) & 15;
	WORD remark = cell_attr(GUTTER, 3) & 15;
	/* Monokai green (key) and yellow (#color) both become legacy color 6: not compared */
	result("section differs from key and #color", section != key && section != color, NULL);
	result("'on' is a constant, not a color", word != color, NULL);
	result("# at the start of a line is a comment", remark != color && remark != key, NULL);
	quit();
}

static void test_config (void) {

	wchar_t exe[MAX_PATH];

	fprintf(report, "note.cfg next to note.exe\n");
	swprintf(exe, MAX_PATH, L"%ls\\note.exe", dir);
	CopyFileW(note, exe, FALSE);
	write_file(L"note.cfg", "[editor]\r\nline_numbers = false\r\nexpand_tabs = true\r\nfoo = 1\r\n", 61);
	write_file(L"cfg.txt", "text", 4);
	start_exe(exe, L"cfg.txt");
	check_row("line_numbers = false hides the numbers", 1, L"text");
	check_status("a bad line is reported", L"note.cfg:4: bad setting 'foo'");
	key(VK_END, 0, 0);
	key(VK_TAB, L'\t', 0);
	type(L"x");
	ctrl('S');
	check_file("expand_tabs: Tab types spaces up to the next stop", L"cfg.txt", "text    x", 9);
	quit();
}

/* screen size and cursor, relative to the window */
static void window_info (int *width, int *cx, int *cy) {

	HANDLE h = open_screen();
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	GetConsoleScreenBufferInfo(h, &csbi);
	CloseHandle(h);
	*width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	*cx = csbi.dwCursorPosition.X - csbi.srWindow.Left;
	*cy = csbi.dwCursorPosition.Y - csbi.srWindow.Top;
}

static void test_wrap (void) {

	char data[400];
	wchar_t row[1024], expected[64];
	int width, cx, cy, per_row, rows, i, n = 0;

	fprintf(report, "word wrap\n");
	for (i = 0; i < 60; i++) {
		memcpy(data + n, "word ", 5);
		n += 5;
	}
	memcpy(data + n, "\r\nend", 5);
	write_file(L"wrap.txt", data, n + 5);
	start(L"wrap.txt");

	window_info(&width, &cx, &cy);
	per_row = (width - GUTTER) / 5;        /* "word " takes 5 columns */
	rows = (60 + per_row - 1) / per_row;

	check_text("without wrap the long line takes one row", 2, L"end");
	key('Z', L'z', ALT);
	screen_row(2, row, 1024);
	result("Alt+Z: the line goes on in the next row, without a number",
		!wcsncmp(row, L"     word", 9), row);
	check_text("... breaking between words", rows + 1, L"end");
	screen_row(rows + 1, row, 1024);
	result("... and the next line keeps its number", !wcsncmp(row, L"   2", 4), row);

	key(VK_DOWN, 0, 0);
	swprintf(expected, 64, L"Ln 1, Col %d", per_row * 5 + 1);
	check_status("Down goes to the next row of the same line", expected);
	window_info(&width, &cx, &cy);
	result("... with the cursor at the start of screen row 2", cx == GUTTER && cy == 2, NULL);

	key(VK_END, 0, 0);
	check_status("End goes to the end of the line", L"Ln 1, Col 301");
	window_info(&width, &cx, &cy);
	result("... on its last row", cy == rows, NULL);

	click(GUTTER + 2, 3);
	swprintf(expected, 64, L"Ln 1, Col %d", 2 * per_row * 5 + 3);
	check_status("clicking a wrapped row", expected);

	key(VK_HOME, 0, 0);
	key('Z', L'z', ALT);
	check_text("Alt+Z again turns it off", 2, L"end");
	quit();
}

/* foreground of a text cell (x after the line numbers); the background changes on the cursor line */
static int fg (int x, int row) {

	return cell_attr(GUTTER + x, row) & 15;
}

static void test_languages (void) {

	fprintf(report, "languages\n");

	write_file(L"t.py", "# comment\r\ndef f(x):\r\n    return \"s\" + x\r\nt = \"\"\"a\r\nb\"\"\"\r\nc", 56);
	start(L"t.py");
	check_status("Python: detected", L"   Python   ");
	result("Python: comment differs from keyword", fg(0, 1) != fg(0, 2), NULL);
	result("Python: return is a keyword like def", fg(4, 3) == fg(0, 2), NULL);
	result("Python: string differs from a name", fg(11, 3) != fg(17, 3), NULL);
	result("Python: \"\"\" string goes on in the next line", fg(0, 5) == fg(11, 3), NULL);
	result("... and ends there", fg(0, 6) != fg(11, 3), NULL);
	quit();

	write_file(L"t.json", "{\"key\": \"v\", \"n\": 1, \"b\": true}", 31);
	start(L"t.json");
	check_status("JSON: detected", L"   JSON   ");
	result("JSON: key differs from the braces", fg(1, 1) != fg(0, 1), NULL);
	result("JSON: true is a constant like the number", fg(26, 1) == fg(18, 1), NULL);
	quit();

	write_file(L"Makefile", "CC = clang\r\nall: $(CC)\r\n\techo hi # c", 34);
	start(L"Makefile");
	check_status("Makefile: detected by name", L"   Makefile   ");
	result("Makefile: target differs from text", fg(0, 2) != fg(0, 1), NULL);
	result("Makefile: $(CC) is a variable", fg(5, 2) != fg(0, 2) && fg(5, 2) != fg(0, 1), NULL);
	result("Makefile: # comment on a recipe line", fg(12, 3) != fg(4, 3), NULL);
	quit();

	write_file(L"t.html", "<a href=\"x\">&amp; t</a>\r\n<!-- c\r\nd -->\r\n<b>", 43);
	start(L"t.html");
	check_status("HTML: detected", L"   HTML   ");
	result("HTML: tag differs from attribute", fg(1, 1) != fg(3, 1), NULL);
	result("HTML: entity differs from text", fg(12, 1) != fg(18, 1), NULL);
	result("HTML: comment goes on in the next line", fg(0, 3) == fg(0, 2), NULL);
	result("... and a tag follows it", fg(1, 4) == fg(1, 1), NULL);
	quit();

	write_file(L"t.md", "# Title\r\ntext\r\n```\r\ncode\r\n```\r\nafter", 38);
	start(L"t.md");
	check_status("Markdown: detected", L"   Markdown   ");
	result("Markdown: heading differs from text", fg(0, 1) != fg(0, 2), NULL);
	result("Markdown: fenced code is one string", fg(0, 4) == fg(0, 3), NULL);
	result("... up to the closing fence", fg(0, 6) != fg(0, 4), NULL);
	quit();

	write_file(L"t.rs", "fn f<'a>(c: char) -> &'a str { 'x' }", 36);
	start(L"t.rs");
	check_status("Rust: detected", L"   Rust   ");
	/* type and string colors merge in the 16-color console: check that 'a did not open a string */
	result("Rust: a lifetime does not open a string", fg(9, 1) == fg(3, 1), NULL);
	result("Rust: 'x' is a char", fg(32, 1) == fg(31, 1), NULL);
	quit();

	write_file(L"t.lua", "--[[ a\r\nb ]] x\r\nlocal y", 21);
	start(L"t.lua");
	check_status("Lua: detected", L"   Lua   ");
	result("Lua: --[[ comment goes on", fg(0, 2) == fg(0, 1), NULL);
	result("... and ends at ]]", fg(5, 2) != fg(0, 2), NULL);
	result("Lua: local is a keyword", fg(0, 3) != fg(5, 2), NULL);
	quit();

	write_file(L"t.bat", "@echo off\r\nrem note\r\nset X=%PATH%", 34);
	start(L"t.bat");
	check_status("Batch: detected", L"   Batch   ");
	result("Batch: rem starts a comment", fg(0, 2) != fg(1, 1), NULL);
	result("Batch: %PATH% is a variable", fg(7, 3) != fg(4, 3), NULL);
	quit();
}

static void test_complete (void) {

	wchar_t sub[MAX_PATH];

	fprintf(report, "Tab completes file names in Open\n");
	write_file(L"comp1.txt", "one", 3);
	write_file(L"comp2.txt", "two", 3);
	swprintf(sub, MAX_PATH, L"%ls\\sub", dir);
	CreateDirectoryW(sub, NULL);
	write_file(L"sub\\alpha.txt", "alpha", 5);
	write_file(L"sub\\beta.txt", "beta", 4);
	start(NULL);

	ctrl('O');
	check_screen("the Open dialog says Tab completes", L"Tab completes", true);
	type(L"comp");
	key(VK_TAB, L'\t', 0);
	check_screen("Tab completes to the first match", L"comp1.txt", true);
	check_screen("... and counts the matches", L"1 of 2", true);
	key(VK_TAB, L'\t', 0);
	check_screen("Tab again: the next one", L"comp2.txt", true);
	key(VK_TAB, L'\t', 0);
	check_screen("... wrapping around", L"comp1.txt", true);
	key(VK_TAB, L'\t', SHIFT);
	check_screen("Shift+Tab goes back", L"comp2.txt", true);
	key(VK_RETURN, L'\r', 0);
	check_row_has("Enter opens it", 0, L"comp2.txt");
	check_text("... with its text", 1, L"two");

	ctrl('O');
	type(L"su");
	key(VK_TAB, L'\t', 0);
	check_screen("a folder completes with a backslash", L"sub\\", true);
	key(VK_END, 0, 0);
	key(VK_TAB, L'\t', 0);
	check_screen("End, then Tab lists inside it", L"sub\\alpha.txt", true);
	key(VK_TAB, L'\t', 0);
	key(VK_RETURN, L'\r', 0);
	check_row_has("and opens the file there", 0, L"sub\\beta.txt");
	quit();
}

static void test_encodings (void) {

	fprintf(report, "UTF-16 and characters missing in ANSI\n");
	write_file(L"le.txt", "\xFF\xFEh\0\xE9\0\r\0\n\0x\0", 12);
	start(L"le.txt");
	check_text("UTF-16 LE is read", 1, L"hé");
	check_text("... both lines", 2, L"x");
	check_status("status: UTF-16 LE", L"UTF-16 LE");
	key(VK_END, 0, CTRL);
	type(L"!");
	ctrl('S');
	check_file("saved back as UTF-16 LE with CRLF", L"le.txt", "\xFF\xFEh\0\xE9\0\r\0\n\0x\0!\0", 14);
	quit();

	write_file(L"be.txt", "\xFE\xFF\0h\0i", 6);
	start(L"be.txt");
	check_text("UTF-16 BE is read", 1, L"hi");
	check_status("status: UTF-16 BE", L"UTF-16 BE");
	key(VK_END, 0, 0);
	type(L"!");
	ctrl('S');
	check_file("saved back as UTF-16 BE", L"be.txt", "\xFE\xFF\0h\0i\0!", 8);
	quit();

	write_file(L"lossy.txt", "caf\xe9", 4);
	start(L"lossy.txt");
	key(VK_END, 0, 0);
	paste(L"\x4E2D");
	ctrl('S');
	check_screen("a character missing in ANSI asks first", L"Some characters do not exist in ANSI", true);
	key('U', L'u', 0);
	check_file("UTF-8 keeps every character", L"lossy.txt", "caf\xc3\xa9\xe4\xb8\xad", 8);
	check_status("status: UTF-8", L"UTF-8");
	quit();
}

static void test_position (void) {

	int col = 0, at;
	wchar_t row[1024], status[1024], *p;

	fprintf(report, "file:line:col, brackets and cell widths\n");
	write_file(L"pos.txt", "one\r\ntwo words\r\nthree", 20);
	start(L"pos.txt:2:5");
	check_status("file:2:5 opens at line 2, column 5", L"Ln 2, Col 5");
	check_row_has("the menu bar shows the name", 0, L"pos.txt");
	check_screen("... without :2:5", L"pos.txt:2", false);
	quit();

	write_file(L"br.c", "f(a[1]);", 8);
	start(L"br.c");
	key(VK_RIGHT, 0, 0);
	result("the bracket at the cursor is highlighted", cell_attr(GUTTER + 1, 1) != cell_attr(GUTTER + 3, 1), NULL);
	result("... and its partner", cell_attr(GUTTER + 6, 1) == cell_attr(GUTTER + 1, 1), NULL);
	key(VK_RIGHT, 0, 0);
	key(VK_RIGHT, 0, 0);
	result("the inner pair when the cursor moves", cell_attr(GUTTER + 3, 1) != cell_attr(GUTTER + 1, 1), NULL);
	quit();

	/* "e" + combining acute: note must put "ab" where the console draws it (1 or 2 cells) */
	write_file(L"mark.txt", "e\xcc\x81" "ab", 5);
	start(L"mark.txt");
	key(VK_END, 0, 0);
	key(VK_LEFT, 0, 0);
	key(VK_LEFT, 0, 0);
	screen_row(-1, status, 1024);
	p = wcsstr(status, L"Col ");
	if (p)
		col = _wtoi(p + 4);
	screen_row(1, row, 1024);
	at = (int) (wcschr(row, L'a') ? wcschr(row, L'a') - row : -1);
	result("columns of a combining mark match the console", at == GUTTER + col - 1, row);
	quit();
}

static void test_recovery (void) {

	fprintf(report, "recovery copy\n");
	write_file(L"rec.txt", "old", 3);
	write_file(L"rec.txt.note-recovery", "new text", 8);
	start(L"rec.txt");
	check_screen("opening offers the recovery copy", L"Unsaved changes to rec.txt were kept", true);
	key('O', L'o', 0);
	check_text("Open loads the copy", 1, L"new text");
	check_row_has("... as unsaved changes", 0, L"rec.txt *");
	ctrl('S');
	check_file("saving writes it to the file", L"rec.txt", "new text", 8);
	{
		wchar_t path[MAX_PATH];
		swprintf(path, MAX_PATH, L"%ls\\rec.txt.note-recovery", dir);
		result("... and removes the copy", GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, NULL);
	}
	quit();
}

/* last test: it leaves the test console for the console of a note of its own, then closes that window */
static void test_close (void) {

	STARTUPINFOW si;
	wchar_t cmd[2 * MAX_PATH];
	HWND window;

	fprintf(report, "closing the window keeps unsaved changes\n");
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" close.txt", note);
	CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &child);
	Sleep(500);

	FreeConsole();
	if (!AttachConsole(child.dwProcessId)) {
		result("attach to the console of note", false, NULL);
		stop();
		return;
	}
	conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	settle();
	type(L"unsaved");
	window = GetConsoleWindow();
	FreeConsole();
	PostMessageW(window, WM_CLOSE, 0, 0);

	result("note ends when its window closes", WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, NULL);
	check_file("the changes went to close.txt.note-recovery", L"close.txt.note-recovery", "unsaved", 7);
	CloseHandle(child.hProcess);
	CloseHandle(child.hThread);
}

static int inner (const wchar_t *report_path) {

	report = _wfopen(report_path, L"w");
	conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

	test_new_file();
	test_lf_utf8();
	test_ansi();
	test_paste();
	test_save_as();
	test_undo();
	test_selection();
	test_find_replace();
	test_menu();
	test_mouse();
	test_syntax();
	test_config();
	test_wrap();
	test_complete();
	test_languages();
	test_encodings();
	test_position();
	test_recovery();
	test_close();

	fprintf(report, "\n%d/%d ok\n", total - failures, total);
	fclose(report);
	return failures != 0;
}

/* ---------- clipboard kept for the user ---------- */

static wchar_t *saved_clipboard;

static void save_clipboard (void) {

	if (!OpenClipboard(NULL))
		return;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	const wchar_t *p = h ? GlobalLock(h) : NULL;
	if (p) {
		saved_clipboard = _wcsdup(p);
		GlobalUnlock(h);
	}
	CloseClipboard();
}

static void restore_clipboard (void) {

	if (!saved_clipboard || !OpenClipboard(NULL))
		return;
	size_t size = (wcslen(saved_clipboard) + 1) * sizeof(wchar_t);
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
	memcpy(GlobalLock(mem), saved_clipboard, size);
	GlobalUnlock(mem);
	EmptyClipboard();
	SetClipboardData(CF_UNICODETEXT, mem);
	CloseClipboard();
}

int main (void) {

	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t self[MAX_PATH], report_path[MAX_PATH], cmd[4 * MAX_PATH];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	if (argc == 5 && !wcscmp(argv[1], L"--inner")) {
		wcscpy(note, argv[2]);
		wcscpy(dir, argv[3]);
		return inner(argv[4]);
	}

	if (argc != 2) {
		fprintf(stderr, "usage: note_test note.exe\n");
		return 2;
	}

	GetFullPathNameW(argv[1], MAX_PATH, note, NULL);
	GetModuleFileNameW(NULL, self, MAX_PATH);
	GetTempPathW(MAX_PATH, report_path);
	wcscat(report_path, L"note_test.txt");
	DeleteFileW(report_path);

	/* fresh working folder for the test files */
	GetTempPathW(MAX_PATH, dir);
	wcscat(dir, L"note_test");
	{
		wchar_t pattern[MAX_PATH], path[MAX_PATH];
		WIN32_FIND_DATAW fd;
		HANDLE h;
		CreateDirectoryW(dir, NULL);
		swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
		h = FindFirstFileW(pattern, &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					DeleteFileW(path);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}

	save_clipboard();

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	swprintf(cmd, 4 * MAX_PATH, L"\"%ls\" --inner \"%ls\" \"%ls\" \"%ls\"", self, note, dir, report_path);

	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
		fprintf(stderr, "could not open the test console (%lu)\n", GetLastError());
		return 2;
	}
	if (WaitForSingleObject(pi.hProcess, 300000) != WAIT_OBJECT_0) {
		TerminateProcess(pi.hProcess, 2);
		fprintf(stderr, "timed out\n");
	}
	GetExitCodeProcess(pi.hProcess, &code);
	restore_clipboard();

	FILE *f = _wfopen(report_path, L"r");
	if (f) {
		char buf[1024];
		while (fgets(buf, sizeof buf, f))
			fputs(buf, stdout);
		fclose(f);
	}
	return (int) code;
}
