/*
	build: bin\Makefile (from bin\: make vplay.exe; from the root: make bin).
	It is a GUI program (-mwindows), so it has no console of its own.
*/

/*
	vplay - borderless always-on-top video overlay, via MCI (mciSendString).

	About MCI on Windows 10/11:
	  mciSendString was NOT removed. winmm.dll still exports mciSendStringW and
	  the MPEGVideo driver (mciqtz32.dll, DirectShow based) is still installed.
	  What breaks in practice is resolving the device from the file extension: MCI
	  looks in HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\MCI Extensions and, if
	  the extension is not there (.mkv, .webm, ...), treats the whole path as a device name
	  and fails with error 310 ("specify a device name with fewer
	  than 79 characters") - which has nothing to do with the real problem.
	  The way out is to open it naming the driver: open "file" type mpegvideo alias X.
	  Then DirectShow takes over and plays any format that has a filter.
	  (The mpegvideo!"file" syntax used in C# examples does NOT go through the
	  mciSendString parser - it returns error 294, missing quotes.)
*/

#define _WIN32_WINNT 0x0600

#undef  UNICODE
#define UNICODE
#undef  _UNICODE
#define _UNICODE

#include <wchar.h>
#include <tchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dwmapi.h>
#include <stdbool.h>
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>

#define BUFLEN   256   /* resposta do MCI                       */
#define PATHLEN 1024   /* file path                            */
#define CMDLEN  2048   /* assembled MCI command (fits PATHLEN)  */
#define NKEYS    256   /* virtual-key codes validos: 0x00..0xFF */

#define BAR_H     13   /* height of the time bar, in pixels     */
#define BAR_FONT_H 12  /* height of the bar font                  */
#define MIN_W    128   /* minimum window width                 */
#define DRAG_TOL   3   /* px before a click becomes a drag     */
#define SEEK_MS 10000  /* step of the left/right arrows         */

#define BLACK RGB(0x0,0x0,0x0)
#define GRAY  RGB(128,128,128)
#define WHITE RGB(255,255,255)
#define RED   RGB(255,0x0,0x0)
#define BLUE  RGB(0x0,128,255)

const int ID_TIMER = 1;

/* ---- window state ---------------------------------------------------- */

int  win_x = 925;          /* position (screen)                              */
int  win_y =  38;
int  win_w = 320;          /* video area                                     */
int  win_h = 180;
int  win_z =   0;          /* extra height taken by the time bar            */
int  win_alpha = 255;
bool visible    = true;    /* window on screen (HOME hides, END shows)      */
bool show_timer = false;
bool fullscreen = false;
RECT saved_rect;           /* geometry before fullscreen                    */

/* ---- estado do player ---------------------------------------------------- */

bool player_ok = false;    /* an MCI device is open                          */
bool paused    = false;
bool muted     = false;
int  volume    = 1000;     /* 0..1000                                         */
int  vid_w = 0, vid_h = 0; /* native video dimensions                       */
int  ms = 0, _ms = 0;      /* posicao e duracao, em milissegundos             */
int  hor, min, seg;
wchar_t s = ':';           /* state glyph shown on the bar                   */

/* ---- estado do mouse ----------------------------------------------------- */

bool  dragging   = false;
bool  drag_armed = false;
POINT drag_from;           /* point where the button was pressed (screen)   */
POINT drag_off;            /* offset cursor -> window corner                */

/* ---- buffers ------------------------------------------------------------- */

wchar_t str[BUFLEN];
wchar_t buf[BUFLEN];
wchar_t time_1[64];
wchar_t time_2[64];
wchar_t file_name[PATHLEN];
wchar_t mci_err[BUFLEN];   /* last MCI error message                        */

int KEY[NKEYS];
int KEYP[NKEYS];

HFONT font = NULL;
HBRUSH back_brush = NULL;

/* ========================================================================== */
/* MCI                                                                        */
/* ========================================================================== */

/*
	Sends a command to MCI and keeps the translated error message.
	Returns 0 on success, like mciSendString itself.
*/
MCIERROR mci (wchar_t *reply, size_t reply_len, const wchar_t *fmt, ...) {

	wchar_t cmd[CMDLEN];
	va_list ap;

	va_start(ap, fmt);
	vswprintf(cmd, CMDLEN, fmt, ap);
	va_end(ap);

	if (reply && reply_len)
		reply[0] = L'\0';

	MCIERROR e = mciSendStringW(cmd, reply, (UINT) reply_len, NULL);

	if (e) {
		if (!mciGetErrorStringW(e, mci_err, BUFLEN))
			swprintf(mci_err, BUFLEN, L"MCI error %lu", (unsigned long) e);
	} else
		mci_err[0] = L'\0';

	return e;
}

/* numeric player status; -1 when the command fails */
long mci_status (const wchar_t *what) {

	if (mci(buf, BUFLEN, L"status _player_ %ls", what))
		return -1;
	return _wtol(buf);
}

void player_close (void) {

	if (player_ok) {
		mci(NULL, 0, L"close _player_");
		player_ok = false;
	}
}

/*
	Opens the file trying, in order:
	  1. letting MCI pick the driver by extension      (.avi -> avivideo,
	     .wav -> waveaudio, .mid -> sequencer: each on its native driver);
	  2. forcing DirectShow with "type mpegvideo"        (fixes .mkv, .webm and
	     any extension missing from the registry);
	  3. retrying with the 8.3 path, for the rare case where the MCI parser
	     trips on some character of the long name.
*/
bool player_load (const wchar_t *file) {

	wchar_t shortp[MAX_PATH];

	player_close();

	if (!mci(NULL, 0, L"open \"%ls\" alias _player_", file))
		player_ok = true;
	else if (!mci(NULL, 0, L"open \"%ls\" type mpegvideo alias _player_", file))
		player_ok = true;
	else if (GetShortPathNameW(file, shortp, MAX_PATH)
	      && !mci(NULL, 0, L"open \"%ls\" type mpegvideo alias _player_", shortp))
		player_ok = true;

	return player_ok;
}

/* reads the native video dimensions.
   "where source" is stable; "where destination" changes on every "put" */
void player_measure (void) {

	int x, y, w, h;

	vid_w = vid_h = 0;

	if (!mci(buf, BUFLEN, L"where _player_ source")
	 && swscanf(buf, L"%d %d %d %d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
		vid_w = w;
		vid_h = h;
	} else if (!mci(buf, BUFLEN, L"where _player_ destination")
	        && swscanf(buf, L"%d %d %d %d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
		vid_w = w;
		vid_h = h;
	}
}

/* derives the height from the width, preserving the native aspect ratio.
   avoids the accumulated drift of scaling width and height separately */
void size_from_width (int w) {

	win_w = (w < MIN_W ? MIN_W : w);

	/* without an image the height is not derived from anything: the window is just the bar */
	if (vid_w <= 0 || vid_h <= 0)
		return;

	win_h = (int) ((double) win_w * vid_h / vid_w + 0.5);

	if (win_h < 1)
		win_h = 1;
}

/* applies position/size to the window and to the MCI destination rectangle */
void player_layout (HWND hwnd) {

	SetWindowPos(hwnd, HWND_TOPMOST, win_x, win_y, win_w, win_h + win_z,
	             SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : 0));

	if (player_ok)
		mci(NULL, 0, L"put _player_ destination at 0 0 %d %d", win_w, win_h);
}

void set_alpha (HWND hwnd, int a) {

	win_alpha = (a > 255 ? 255 : (a < 5 ? 5 : a));
	SetLayeredWindowAttributes(hwnd, 0, (BYTE) win_alpha, LWA_ALPHA);
}

void set_volume (int v) {

	volume = (v > 1000 ? 1000 : (v < 0 ? 0 : v));
	if (player_ok)
		mci(NULL, 0, L"setaudio _player_ volume to %d", volume);
}

/*
	Plays from a position keeping the loop.
	"play ... repeat" makes the driver itself restart at the end, with no gap -
	so there is no manual loop here comparing position with length.
*/
void player_play_from (int at) {

	if (!player_ok)
		return;

	if (at < 0)
		at = 0;
	if (_ms > 0 && at >= _ms)
		at = 0;

	ms = at;
	paused = false;
	mci(NULL, 0, L"play _player_ from %d repeat", at);
}

void player_pause (void) {

	if (player_ok) {
		paused = true;
		s = 'X';
		mci(NULL, 0, L"pause _player_");
	}
}

void player_resume (void) {

	if (!player_ok)
		return;

	paused = false;
	s = ':';

	/* "play repeat" without "from" resumes at the current position and re-arms the loop;
	   "resume" would fail when there was never a pause before */
	mci(NULL, 0, L"play _player_ repeat");
}

/* ========================================================================== */
/* util                                                                       */
/* ========================================================================== */

void get_input (void) {

	for (int i = 0; i < NKEYS; i++) {
		bool k = !!(GetAsyncKeyState(i) & 0x8000);
		/* key down */
		KEY[i] = k;
		/* key press */
		if (KEYP[i] <= 1)
			KEYP[i] = k;
		/* key up */
		if (k == 0 && KEYP[i] == 2)
			KEYP[i] = -1;
	}
}

int key (int k) {

	return (k >= 0 && k < NKEYS) ? KEY[k] : 0;
}

int key_release (int k) {

	if (k >= 0 && k < NKEYS && KEYP[k] == -1) {
		KEYP[k] = 0;
		return 1;
	}
	return 0;
}

int key_press (int k) {

	if (k >= 0 && k < NKEYS && KEYP[k] == 1) {
		KEYP[k] = 2;
		return 1;
	}
	return 0;
}

void set_color (HDC xdc, COLORREF text, COLORREF bground) {

	SetBkColor(xdc, bground);
	SetTextColor(xdc, text);
}

wchar_t *barra (double x, double m) {

	static wchar_t v[0XB];
	int fill = 0;

	if (m > 0) {
		fill = (int) ((x / m) * 0XA + 0.5);
		fill = (fill > 0XA ? 0XA : (fill < 0 ? 0 : fill));
	}

	for (int i = 0; i < 0XA; i++)
		v[i] = (i < fill ? L'#' : L'.');
	v[0XA] = L'\0';

	return v;
}

/* time bar rectangle, in client coordinates */
RECT bar_rect (HWND hwnd) {

	RECT r;
	GetClientRect(hwnd, &r);
	r.top = win_h;
	if (r.bottom < r.top)
		r.bottom = r.top;
	return r;
}

/*
	Writes to the console that launched the process. The binary is built with
	-mwindows, so it has no stdout of its own: it must attach to the parent console.
	A console handle only accepts WriteConsoleW; if the output was redirected
	to a file or pipe, the handle is a plain file and needs bytes.
*/
void console_print (const wchar_t *text) {

	bool   attached = AttachConsole(ATTACH_PARENT_PROCESS);
	HANDLE opened   = INVALID_HANDLE_VALUE;
	HANDLE h        = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD  mode, n  = 0;

	if ((h == NULL || h == INVALID_HANDLE_VALUE) && attached) {
		opened = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
		                     NULL, OPEN_EXISTING, 0, NULL);
		h = opened;
	}

	if (h != NULL && h != INVALID_HANDLE_VALUE) {
		if (GetConsoleMode(h, &mode))
			WriteConsoleW(h, text, (DWORD) wcslen(text), &n, NULL);
		else {
			int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
			char *utf8 = (len > 0 ? malloc((size_t) len) : NULL);
			if (utf8) {
				WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, len, NULL, NULL);
				WriteFile(h, utf8, (DWORD) (len - 1), &n, NULL);
				free(utf8);
			}
		}
	}

	if (opened != INVALID_HANDLE_VALUE)
		CloseHandle(opened);
	if (attached)
		FreeConsole();
}

/* ========================================================================== */
/* player                                                                     */
/* ========================================================================== */

bool create_player (HWND hwnd, const wchar_t *file) {

	if (!player_load(file)) {
		wchar_t msg[CMDLEN];
		swprintf(msg, CMDLEN, L"Could not open:\n%ls\n\nMCI: %ls", file, mci_err);
		MessageBoxW(hwnd, msg, L"vplay", MB_ICONEXCLAMATION | MB_OK);
		return false;
	}

	/* without this length/position may come in frames, and all the arithmetic breaks */
	mci(NULL, 0, L"set _player_ time format milliseconds");

	player_measure();

	/* initial size: 1/6 of native, never smaller than MIN_W.
	   the window is WS_POPUP: there is no border or title to subtract, so
	   AdjustWindowRect here would only distort the aspect ratio */
	if (!fullscreen)
		size_from_width(vid_w > 0 ? vid_w / 6 : win_w);

	/* video drawn straight into our window.
	   HWND is truncated to 32 bits on purpose: the MCI parser only accepts
	   a decimal integer, and Windows handles have 32 significant bits */
	mci(NULL, 0, L"window _player_ handle %lu state show",
	    (unsigned long) (ULONG_PTR) hwnd);

	player_layout(hwnd);

	/* duracao */
	_ms = (int) mci_status(L"length");
	if (_ms < 0)
		_ms = 0;
	hor = (_ms/3600000) % 60; min = (_ms/60000) % 60; seg = (_ms/1000) % 60;
	swprintf(time_2, 64, L"%.2d:%.2d:%.2d", hor, min, seg);

	set_volume(volume);
	if (muted)
		mci(NULL, 0, L"setaudio _player_ off");

	s = ':';
	player_play_from(0);

	/* audio-only file: no image to show, the window becomes just the bar */
	if (vid_w == 0 || vid_h == 0) {
		show_timer = true;
		win_h = 1;
		win_z = BAR_H;
		player_layout(hwnd);
	}

	return true;
}

void drop_file (HWND hwnd, WPARAM wparam) {

	wchar_t path[PATHLEN] = {0};
	HDROP drop = (HDROP) wparam;

	UINT files = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);

	/* several files dropped at once: only the first matters,
	   abrir os demais em sequencia so reabriria o device a toa */
	if (files > 0 && DragQueryFileW(drop, 0, path, PATHLEN) > 0) {
		if (create_player(hwnd, path)) {
			wcsncpy(file_name, path, PATHLEN - 1);
			file_name[PATHLEN - 1] = L'\0';
			visible = true;
			ShowWindow(hwnd, SW_SHOW);
			player_layout(hwnd);
		}
	}

	DragFinish(drop);
}

void display_timer (HWND hwnd, HDC xdc) {

	RECT r = bar_rect(hwnd);

	hor = (ms/3600000) % 60; min = (ms/60000) % 60; seg = (ms/1000) % 60;
	swprintf(time_1, 64, L"%.2d:%.2d:%.2d", hor, min, seg);

	swprintf(str, BUFLEN, L"[%lc] %ls [%ls] %ls", s, time_1, barra(ms, _ms), time_2);
	if ((size_t) win_w < wcslen(str) * ((BAR_FONT_H / 2) + 1))
		swprintf(str, BUFLEN, L"[%ls]", barra(ms, _ms));

	FillRect(xdc, &r, back_brush);
	set_color(xdc, GRAY, BLACK);
	DrawTextW(xdc, str, -1, &r, DT_CENTER | DT_SINGLELINE);
}

void toggle_fullscreen (HWND hwnd) {

	if (!fullscreen) {
		GetWindowRect(hwnd, &saved_rect);

		/* monitor where the window is, not just the primary one */
		MONITORINFO mi = { .cbSize = sizeof(MONITORINFO) };
		if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
			return;

		fullscreen = true;
		win_x = mi.rcMonitor.left;
		win_y = mi.rcMonitor.top;
		win_w = mi.rcMonitor.right  - mi.rcMonitor.left;
		win_h = mi.rcMonitor.bottom - mi.rcMonitor.top - win_z;
	} else {
		fullscreen = false;
		win_x = saved_rect.left;
		win_y = saved_rect.top;
		size_from_width(saved_rect.right - saved_rect.left);
	}

	player_layout(hwnd);
	InvalidateRect(hwnd, NULL, FALSE);
}

void zoom (HWND hwnd, int dir) {

	if (fullscreen)
		return;

	int step = win_w / 16;
	if (step < 1)
		step = 1;

	size_from_width(win_w + (dir > 0 ? step : -step));
	player_layout(hwnd);
}

/* ========================================================================== */
/* window                                                                     */
/* ========================================================================== */

LRESULT CALLBACK WndProc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {

	PAINTSTRUCT ps;

	switch (msg) {
		case WM_CREATE:
			DragAcceptFiles(hwnd, TRUE);
			SetLayeredWindowAttributes(hwnd, 0, (BYTE) win_alpha, LWA_ALPHA);

			/* one font for the whole process: recreating it on every WM_PAINT
			   would cost 20 CreateFont calls per second for no gain */
			font = CreateFontW(
				BAR_FONT_H, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
				FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				DEFAULT_PITCH | FF_ROMAN, L"Lucida Console"
			);

			if (*file_name)
				create_player(hwnd, file_name);

			if (SetTimer(hwnd, ID_TIMER, 50, NULL) == 0)
				MessageBoxW(hwnd, L"Could not SetTimer()!", L"Error",
				            MB_OK | MB_ICONEXCLAMATION);
			break;

		case WM_MOUSEWHEEL: {
				int dir = ((short) HIWORD(wparam) < 0 ? -1 : 1);

				if (GetKeyState(VK_CONTROL) & 0x8000)
					set_volume(volume + dir * 50);
				else if (GetKeyState(VK_SHIFT) & 0x8000)
					set_alpha(hwnd, win_alpha + dir * 10);
				else
					zoom(hwnd, dir);
			}
			break;

		case WM_LBUTTONDOWN: {
				RECT r;
				GetWindowRect(hwnd, &r);
				GetCursorPos(&drag_from);
				drag_off.x = drag_from.x - r.left;
				drag_off.y = drag_from.y - r.top;
				drag_armed = true;
				dragging   = false;
				SetCapture(hwnd);
			}
			break;

		case WM_MOUSEMOVE:
			if (drag_armed) {
				POINT p;
				GetCursorPos(&p);

				/* it only becomes a drag after leaving the tolerance:
				   assim um clique parado continua sendo um clique */
				if (!dragging
				 && (abs(p.x - drag_from.x) > DRAG_TOL || abs(p.y - drag_from.y) > DRAG_TOL))
					dragging = true;

				if (dragging && !fullscreen) {
					win_x = p.x - drag_off.x;
					win_y = p.y - drag_off.y;
					MoveWindow(hwnd, win_x, win_y, win_w, win_h + win_z, FALSE);
				}
			}
			break;

		case WM_LBUTTONUP:
			if (drag_armed) {
				ReleaseCapture();

				/* click without drag on the bottom strip: toggles the bar.
				   only makes sense with video: with audio the bar is the whole window */
				if (!dragging && vid_h > 0) {
					POINT p = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
					if (p.x >= 0 && p.x < win_w && p.y >= (win_h - BAR_H) && p.y < (win_h + win_z)) {
						show_timer = !show_timer;
						win_z = (show_timer ? BAR_H : 0);
						player_layout(hwnd);
						InvalidateRect(hwnd, NULL, TRUE);
					}
				}

				drag_armed = false;
				dragging   = false;
			}
			break;

		case WM_DROPFILES:
			drop_file(hwnd, wparam);
			drag_armed = false;
			dragging   = false;
			break;

		case WM_KEYDOWN:
			switch (wparam) {
				case VK_LEFT: /* back */
					s = '<';
					player_play_from(ms - SEEK_MS);
					break;
				case VK_RIGHT: /* forward */
					s = '>';
					player_play_from(ms + SEEK_MS);
					break;
				case VK_UP: /* play */
					player_resume();
					break;
				case VK_DOWN: /* pause */
					player_pause();
					break;
				case VK_SPACE: /* play / pause */
					if (paused)
						player_resume();
					else
						player_pause();
					break;
				case 'M': /* mute */
					muted = !muted;
					if (player_ok)
						mci(NULL, 0, L"setaudio _player_ %ls", muted ? L"off" : L"on");
					break;
				case VK_PRIOR: /* page_up (Alpha up) */
					set_alpha(hwnd, win_alpha + 10);
					break;
				case VK_NEXT: /* page_down (Alpha down) */
					set_alpha(hwnd, win_alpha - 10);
					break;
				case VK_OEM_PLUS:
				case VK_ADD:
					zoom(hwnd, +1);
					break;
				case VK_OEM_MINUS:
				case VK_SUBTRACT:
					zoom(hwnd, -1);
					break;
				case '0': /* reset original size */
					if (!fullscreen) {
						size_from_width(vid_w > 0 ? vid_w / 6 : MIN_W);
						player_layout(hwnd);
					}
					break;
				case VK_ESCAPE:
					if (fullscreen)
						toggle_fullscreen(hwnd);
					break;
				case 'Q':
					PostMessageW(hwnd, WM_CLOSE, 0, 0);
					break;
				default: break;
			}
			break;

		case WM_SYSKEYDOWN:
			if (wparam == VK_RETURN) {
				toggle_fullscreen(hwnd);
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wparam, lparam);

		case WM_TIMER: {

				get_input();

				/* gives the glyph back to play after showing << or >> */
				static int count_s = 0;
				if ((s == '>' || s == '<') && ++count_s >= 2) {
					count_s = 0;
					s = (paused ? 'X' : ':');
				}

				if (player_ok && visible && !paused) {
					long p = mci_status(L"position");
					if (p >= 0)
						ms = (int) p;
				}

				/* global keys: they work even without window focus -
				   that is why they come from GetAsyncKeyState and not from WM_KEYDOWN */
				if (visible && key_press(VK_HOME)) {
					visible = false;
					show_timer = false;
					win_z = 0;
					player_pause();
					ShowWindow(hwnd, SW_HIDE);
				}
				if (!visible && key_press(VK_END)) {
					visible = true;
					ShowWindow(hwnd, SW_SHOW);
					player_layout(hwnd);
					player_resume();
				}
				if (key_press(VK_F12)) {
					s = ':';
					player_play_from(0);
				}

				/* repaints the bar only when the text really changes:
				   invalidating on every tick would give 20 repaints per second for nothing */
				if (show_timer && visible) {
					static int     last_sec = -1;
					static wchar_t last_s   = 0;
					int now = ms / 1000;
					if (now != last_sec || s != last_s) {
						last_sec = now;
						last_s   = s;
						RECT r = bar_rect(hwnd);
						InvalidateRect(hwnd, &r, FALSE);
					}
				}
			}
			break;

		/* without this the black background brush flickers over the video 20x per second */
		case WM_ERASEBKGND: {
				HDC dc = (HDC) wparam;
				RECT r;
				GetClientRect(hwnd, &r);
				if (player_ok && vid_h > 0)
					r.top = win_h;     /* the video covers the rest */
				if (r.bottom > r.top)
					FillRect(dc, &r, back_brush);
			}
			return 1;

		case WM_PAINT: {
				HDC dc = BeginPaint(hwnd, &ps);
				if (show_timer && font) {
					HGDIOBJ old = SelectObject(dc, font);
					display_timer(hwnd, dc);
					SelectObject(dc, old);
				}
				EndPaint(hwnd, &ps);
			}
			break;

		case WM_CLOSE:
			DestroyWindow(hwnd);
			break;

		case WM_DESTROY:
			KillTimer(hwnd, ID_TIMER);
			DragAcceptFiles(hwnd, FALSE);
			player_close();
			if (font) {
				DeleteObject(font);
				font = NULL;
			}
			PostQuitMessage(0);
			break;

		default:
			return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
	return 0;
}

/* ========================================================================== */
/* entrada                                                                    */
/* ========================================================================== */

/*
	Reads the file path from the command line.
	No wcstombs/mbstowcs: converting to the ANSI code page and back
	truncates any name with a character outside it - and Unicode MCI
	(mciSendStringW) opens those paths without any problem.
*/
bool get_args (void) {

	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (argv == NULL) {
		console_print(L"\n vplay: CommandLineToArgvW falhou\n");
		return false;
	}

	if (argc >= 2) {
		if (!wcscmp(argv[1], L"-h") || !wcscmp(argv[1], L"--help")) {
			console_print(
				L"\n vplay - video overlay via MCI\n\n"
				L" usage: vplay [file]\n\n"
				L"   drag and drop       opens another file\n"
				L"   drag                moves the window\n"
				L"   click on the base   toggles the time bar\n"
				L"   left/right arrows   -10s / +10s\n"
				L"   up/down arrows      play / pause\n"
				L"   space               play / pause\n"
				L"   F12                 restarts\n"
				L"   wheel               zoom     (Shift: opacity, Ctrl: volume)\n"
				L"   + / - / 0           zoom / original size\n"
				L"   PgUp / PgDn         opacity\n"
				L"   M                   mute\n"
				L"   Alt+Enter / Esc     fullscreen\n"
				L"   Home / End          hide / show (global)\n"
				L"   Q                   quit\n\n"
			);
			LocalFree(argv);
			return false;
		}

		wcsncpy(file_name, argv[1], PATHLEN - 1);
		file_name[PATHLEN - 1] = L'\0';
	}

	LocalFree(argv);
	return true;
}

/* 1:1 pixels on scaled displays, otherwise Windows enlarges and blurs the video */
void set_dpi_aware (void) {

	HMODULE u32 = GetModuleHandleW(L"user32.dll");
	if (u32) {
		BOOL (WINAPI *set_ctx)(HANDLE) =
			(BOOL (WINAPI *)(HANDLE)) (void *) GetProcAddress(u32, "SetProcessDpiAwarenessContext");
		if (set_ctx && set_ctx((HANDLE) -4)) /* PER_MONITOR_AWARE_V2 */
			return;
	}
	SetProcessDPIAware();
}

int WINAPI WinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {

	(void) hPrevInstance;
	(void) lpCmdLine;
	(void) nCmdShow;

	if (!get_args())
		return 0;

	set_dpi_aware();

	const wchar_t *appname = L"Vplayer Light";
	WNDCLASSW wc;
	HWND hwnd;
	MSG msg = {0};

	back_brush = CreateSolidBrush(BLACK);

	wc.cbClsExtra    = 0;
	wc.cbWndExtra    = 0;
	wc.hbrBackground = back_brush;
	wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
	wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
	wc.hInstance     = hInstance;
	wc.lpfnWndProc   = WndProc;
	wc.lpszClassName = appname;
	wc.lpszMenuName  = NULL;
	wc.style         = CS_HREDRAW | CS_VREDRAW;

	if (!RegisterClassW(&wc)) {
		MessageBoxW(NULL, L"Window Registration Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

	/* WS_EX_TOOLWINDOW at creation time: no taskbar icon and no
	   hide/re-show flicker after the window is created */
	hwnd = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST,
		appname, appname, WS_POPUP,
		win_x, win_y, win_w, win_h,
		NULL, NULL, hInstance, NULL
	);

	if (hwnd == NULL) {
		MessageBoxW(NULL, L"Window Creation Failed!", L"Error!", MB_ICONEXCLAMATION | MB_OK);
		return 0;
	}

	player_layout(hwnd);
	UpdateWindow(hwnd);

	BOOL r;
	while ((r = GetMessageW(&msg, NULL, 0, 0)) != 0) {
		if (r == -1)
			break;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	player_close();

	if (back_brush)
		DeleteObject(back_brush);

	return (int) msg.wParam;
}
