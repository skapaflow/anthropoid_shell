/*****************************************************************************
	vplay - small borderless always-on-top video player.
	build: bin\Makefile (from bin\: make vplay.exe; from the root: make bin).

	Media Foundation (IMFMediaEngine) in frame-server mode: the engine decodes
	(hardware when available) and every new frame is copied into the back
	buffer of our own D3D11 swap chain. The swap chain is GDI compatible, so
	the time bar is drawn with plain GDI on top of the frame before Present.
	One window, no child window from the decoder: hit testing for resizing
	and the overlay bar just work.

	When Media Foundation has no decoder for a file (AV1 without the Store
	extension, ...), the DirectShow filters installed on the system take over
	(dshow.c: LAV Filters from K-Lite, for example), drawn into the same
	buffer with GDI.

	Threads:
	  main    window, input, engine commands
	  render  waits for the compositor (DwmFlush), polls the engine for a new
	          frame and draws; it keeps running while the window is being
	          moved or resized (modal loops of the main thread)

	usage: vplay [file]     (a running vplay receives the file instead)
*****************************************************************************/

#define COBJMACROS
#define _WIN32_WINNT 0x0A00

#undef  UNICODE
#define UNICODE
#undef  _UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#undef GetCurrentTime /* winbase.h macro clashes with IMFMediaEngine::GetCurrentTime */
#include <mfapi.h>
#include <mfmediaengine.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>

#include "dshow.h"

#define APP_CLASS    L"vplay"
#define WM_ENGINE    (WM_APP + 1)  /* wparam: engine event, lparam: param2      */
#define WM_DSHOW     (WM_APP + 2)  /* DirectShow graph events are queued        */
#define CD_OPEN      0x76706F70    /* WM_COPYDATA: open the path in lpData      */
#define CD_SHOW      0x76707368    /* WM_COPYDATA: show the window              */

#define PATHLEN      1024
#define MIN_W        160           /* minimum window width (96 dpi)             */
#define EDGE         6             /* resize band inside the window (96 dpi)    */
#define BAR_H        28            /* height of the time bar (96 dpi)           */
#define FONT_H       13
#define SEEK_STEP    10.0          /* left/right arrows, in seconds             */
#define HIDE_MS      1500          /* bar hides after this long without moving  */
#define TIMER_MS     50

/* ---- shared with the render thread (under `lock`) ----------------------- */

/* who decodes the current file: Media Foundation first; DirectShow (LAV
   Filters, ...) when Media Foundation has no decoder for it */
enum { BK_MF, BK_DS };

typedef struct {
	int     backend;
	bool    visible;       /* window on screen (Home hides, End shows)        */
	bool    loaded;        /* source has metadata                             */
	bool    has_video;
	bool    paused;
	bool    bar;           /* time bar shown                                  */
	bool    seeking;       /* dragging on the bar                             */
	double  seek_to;       /* target while dragging, seconds                  */
	double  duration;      /* seconds; 0 when unknown                         */
	int     hover_x;       /* cursor x over the track, -1 when not            */
	bool    hover_btn;     /* cursor over the play/pause button               */
	float   scale;         /* dpi / 96                                        */
	int     tw_short;      /* width of "00:00" and "00:00:00" in the bar font */
	int     tw_long;
	wchar_t osd[160];      /* message at the top left                         */
	wchar_t idle[PATHLEN]; /* centered text when there is no image            */
	bool    dirty;         /* something changed: draw again                   */
} UI;

static UI ui;
static CRITICAL_SECTION lock;    /* guards ui                              */
static CRITICAL_SECTION elock;   /* serializes calls into the media engine */
static HANDLE wake;              /* wakes the render thread                */
static HANDLE render_thread;
static volatile LONG quitting;

/* ---- main thread only --------------------------------------------------- */

static HWND hwnd;
static IMFMediaEngine        *engine;
static IMFMediaEngineEx      *engine_ex;
static IMFDXGIDeviceManager  *dxgi_mgr;
static ID3D11Device          *device;
static IDXGISwapChain        *swap;
static HWINEVENTHOOK          hook;

static wchar_t   file_name[PATHLEN];
static double    aspect;          /* display aspect of the video, 0 = none */
static int       vid_w;           /* native width, for the initial size     */
static bool      first_size = true;
static bool      fullscreen;
static RECT      saved_rect;      /* geometry before fullscreen            */
static int       win_alpha = 255;
static double    volume = 1.0;
static bool      muted;
static bool      hidden_paused;   /* Home paused it: End resumes           */
static bool      tracking;        /* TrackMouseEvent armed: client area    */
static bool      nc_tracking;     /* ... and the "title bar" (the video)   */
static POINT     last_mouse = { -1, -1 };
static ULONGLONG last_activity;
static ULONGLONG osd_until;
static bool      seek_pending;

/* ========================================================================== */
/* helpers                                                                    */
/* ========================================================================== */

static void ui_changed (void) {

	ui.dirty = true;
	SetEvent(wake);
}

static void osd (ULONGLONG ms, const wchar_t *fmt, ...) {

	va_list ap;
	EnterCriticalSection(&lock);
	va_start(ap, fmt);
	vswprintf(ui.osd, 160, fmt, ap);
	va_end(ap);
	ui_changed();
	LeaveCriticalSection(&lock);
	osd_until = GetTickCount64() + ms;
}

static const wchar_t *base_name (const wchar_t *path) {

	const wchar_t *b = path;
	for (const wchar_t *p = path; *p; p++)
		if (*p == L'\\' || *p == L'/')
			b = p + 1;
	return b;
}

static void fmt_time (wchar_t *out, size_t n, double t, bool hours) {

	if (!(t >= 0))
		t = 0;
	long s = (long) t;
	if (hours)
		swprintf(out, n, L"%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
	else
		swprintf(out, n, L"%02ld:%02ld", s / 60, s % 60);
}

static int px (float scale, int v) {

	return (int) (v * scale + 0.5f);
}

/*
	Bar layout, shared by drawing and hit testing:
	[ play/pause ][ 00:12 ][==========track==========][ 03:45 ]
	The times go away when the window is too narrow for them.
*/
typedef struct {
	RECT bar, btn, track, cur, tot;
	bool show_cur, show_tot, hours;
} LAYOUT;

static LAYOUT layout (int w, int h, const UI *u) {

	LAYOUT l;
	int bh  = px(u->scale, BAR_H);
	int pad = px(u->scale, 8);
	int min = px(u->scale, 60);

	l.hours = (u->duration >= 3600);
	int tw  = (l.hours ? u->tw_long : u->tw_short);

	l.bar = (RECT) { 0, h - bh, w, h };
	l.btn = (RECT) { 0, h - bh, bh, h };

	int x0 = l.btn.right, x1 = w - pad;
	l.show_cur = l.show_tot = false;
	if (x1 - x0 - 2 * (tw + pad) >= min) {
		l.show_cur = l.show_tot = true;
		x0 += tw + pad;
		x1 -= tw + pad;
	} else if (x1 - x0 - (tw + pad) >= min) {
		l.show_cur = true;
		x0 += tw + pad;
	}

	l.cur   = (RECT) { l.btn.right, h - bh, l.btn.right + tw, h };
	l.track = (RECT) { x0, h - bh, x1, h };
	l.tot   = (RECT) { x1 + pad, h - bh, x1 + pad + tw, h };
	return l;
}

static bool in_rect (const RECT *r, int x, int y) {

	return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

/*
	Writes to the console that launched the process (built with -mwindows,
	so there is no stdout of its own). A redirected handle needs UTF-8 bytes.
*/
static void console_print (const wchar_t *text) {

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
/* render thread                                                              */
/* ========================================================================== */

static HDC     dim_dc;    /* 1x1 black bitmap, stretched with constant alpha */
static HBITMAP dim_bmp;
static HFONT   font;
static float   font_scale;
static int     font_h;

static void fill (HDC dc, const RECT *r, COLORREF c) {

	SetDCBrushColor(dc, c);
	FillRect(dc, r, (HBRUSH) GetStockObject(DC_BRUSH));
}

static void dim (HDC dc, const RECT *r, BYTE alpha) {

	BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
	if (!AlphaBlend(dc, r->left, r->top, r->right - r->left, r->bottom - r->top,
	                dim_dc, 0, 0, 1, 1, bf))
		fill(dc, r, RGB(0, 0, 0));
}

static void make_font (float scale) {

	if (font)
		DeleteObject(font);

	font = CreateFontW(-px(scale, FONT_H), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	font_scale = scale;

	HDC dc = CreateCompatibleDC(NULL);
	HGDIOBJ old = SelectObject(dc, font);
	SIZE s1, s2;
	TEXTMETRICW tm;
	GetTextExtentPoint32W(dc, L"00:00", 5, &s1);
	GetTextExtentPoint32W(dc, L"0:00:00", 7, &s2);
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	DeleteDC(dc);

	font_h = tm.tmHeight;
	EnterCriticalSection(&lock);
	ui.tw_short = s1.cx;
	ui.tw_long  = s2.cx;
	LeaveCriticalSection(&lock);
}

static void draw_bar (HDC dc, const UI *u, int w, int h, double t) {

	LAYOUT l = layout(w, h, u);
	float  s = u->scale;
	wchar_t txt[32];

	dim(dc, &l.bar, 150);

	/* button: shows what a click does (play when paused) */
	int cx = (l.btn.left + l.btn.right) / 2, cy = (l.btn.top + l.btn.bottom) / 2;
	int k  = px(s, 5);
	COLORREF ic = (u->hover_btn ? RGB(255, 255, 255) : RGB(210, 210, 210));
	SelectObject(dc, GetStockObject(NULL_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	SetDCBrushColor(dc, ic);
	if (u->paused) {
		POINT tri[3] = { { cx - k + 1, cy - k }, { cx - k + 1, cy + k }, { cx + k, cy } };
		Polygon(dc, tri, 3);
	} else {
		RECT a = { cx - k, cy - k, cx - k + px(s, 3), cy + k };
		RECT b = { cx + k - px(s, 3), cy - k, cx + k, cy + k };
		fill(dc, &a, ic);
		fill(dc, &b, ic);
	}

	/* track */
	bool   hot  = (u->hover_x >= 0 || u->seeking);
	int    th   = px(s, hot ? 5 : 3);
	int    ty   = (l.track.top + l.track.bottom) / 2;
	int    tw   = l.track.right - l.track.left;
	double frac = (u->duration > 0 ? t / u->duration : 0);
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;

	RECT back = { l.track.left, ty - th / 2, l.track.right, ty - th / 2 + th };
	RECT done = back;
	done.right = l.track.left + (int) (tw * frac + 0.5);
	fill(dc, &back, RGB(90, 90, 90));
	fill(dc, &done, RGB(255, 255, 255));

	if (hot && u->duration > 0) {
		int r = px(s, 6);
		SetDCBrushColor(dc, RGB(255, 255, 255));
		Ellipse(dc, done.right - r, ty - r, done.right + r, ty + r);
	}

	/* times */
	HGDIOBJ old = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(230, 230, 230));

	if (l.show_cur) {
		fmt_time(txt, 32, t, l.hours);
		DrawTextW(dc, txt, -1, &l.cur, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	if (l.show_tot) {
		fmt_time(txt, 32, u->duration, l.hours);
		DrawTextW(dc, txt, -1, &l.tot, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	/* time under the cursor, above the bar */
	if (u->hover_x >= 0 && u->duration > 0 && tw > 0) {
		double ht = (double) (u->hover_x - l.track.left) / tw * u->duration;
		if (ht < 0) ht = 0;
		if (ht > u->duration) ht = u->duration;
		fmt_time(txt, 32, ht, l.hours);

		SIZE sz;
		GetTextExtentPoint32W(dc, txt, (int) wcslen(txt), &sz);
		int bw = sz.cx + px(s, 10), bh = font_h + px(s, 4);
		int x0 = u->hover_x - bw / 2;
		if (x0 < 0) x0 = 0;
		if (x0 + bw > w) x0 = w - bw;
		RECT box = { x0, l.bar.top - bh - px(s, 4), x0 + bw, l.bar.top - px(s, 4) };
		if (box.top >= 0) {
			dim(dc, &box, 190);
			DrawTextW(dc, txt, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
	}

	SelectObject(dc, old);
}

static void draw_text_box (HDC dc, const UI *u, const wchar_t *text, bool center, int w, int h) {

	HGDIOBJ old = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(230, 230, 230));

	if (center) {
		RECT r = { px(u->scale, 8), 0, w - px(u->scale, 8), h - (u->bar ? px(u->scale, BAR_H) : 0) };
		SetTextColor(dc, RGB(150, 150, 150));
		DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	} else {
		RECT m = { 0, 0, w - px(u->scale, 16), h };
		DrawTextW(dc, text, -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		int pad = px(u->scale, 6);
		RECT box = { px(u->scale, 8), px(u->scale, 8), 0, 0 };
		box.right  = box.left + (m.right - m.left) + 2 * pad;
		box.bottom = box.top + font_h + pad;
		if (box.right > w - px(u->scale, 8))
			box.right = w - px(u->scale, 8);
		dim(dc, &box, 170);
		DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	}

	SelectObject(dc, old);
}

/* playback position, from either backend */
static double be_time (int backend) {

	EnterCriticalSection(&elock);
	double t = (backend == BK_DS ? ds_time() : IMFMediaEngine_GetCurrentTime(engine));
	LeaveCriticalSection(&elock);
	return t;
}

static void draw (const UI *u, int w, int h) {

	ID3D11Texture2D *tex  = NULL;
	IDXGISurface1   *surf = NULL;
	HDC dc = NULL;
	bool framed = false;
	double t = 0;

	if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **) &tex)))
		return;

	/* Media Foundation: the GPU copies (and scales) the frame into the buffer */
	if (u->loaded && u->has_video && u->backend == BK_MF) {
		RECT   dst   = { 0, 0, w, h };
		MFARGB black = { 0, 0, 0, 255 };
		EnterCriticalSection(&elock);
		framed = SUCCEEDED(IMFMediaEngine_TransferVideoFrame(engine, (IUnknown *) tex, NULL, &dst, &black));
		LeaveCriticalSection(&elock);
	}

	/* DirectShow, NV12: the GPU converts and scales the frame */
	if (u->loaded && u->has_video && u->backend == BK_DS)
		framed = ds_draw_gpu(device, tex, w, h);
	if (u->loaded)
		t = be_time(u->backend);
	if (u->seeking)
		t = u->seek_to;

	if (SUCCEEDED(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGISurface1, (void **) &surf))
	 && SUCCEEDED(IDXGISurface1_GetDC(surf, FALSE, &dc))) {

		/* DirectShow, RGB32: the frame from the grabber, stretched with GDI */
		if (!framed && u->loaded && u->has_video && u->backend == BK_DS)
			framed = ds_draw(dc, w, h);

		if (!framed) {
			RECT all = { 0, 0, w, h };
			fill(dc, &all, RGB(0, 0, 0));
			if (u->idle[0])
				draw_text_box(dc, u, u->idle, true, w, h);
		}
		if (u->bar && u->loaded)
			draw_bar(dc, u, w, h, t);
		if (u->osd[0])
			draw_text_box(dc, u, u->osd, false, w, h);

		IDXGISurface1_ReleaseDC(surf, NULL);
	}

	if (surf)
		IDXGISurface1_Release(surf);
	ID3D11Texture2D_Release(tex);

	IDXGISwapChain_Present(swap, 0, 0);
}

static DWORD WINAPI render_main (LPVOID arg) {

	(void) arg;
	int    bw = 0, bh = 0;      /* back buffer size */
	long   last_sec = -1;
	UI     u;

	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	BITMAPINFO bi = { .bmiHeader = { sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
	void *bits;
	dim_dc  = CreateCompatibleDC(NULL);
	dim_bmp = CreateDIBSection(dim_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	SelectObject(dim_dc, dim_bmp);

	while (!quitting) {

		EnterCriticalSection(&lock);
		u = ui;
		ui.dirty = false;
		LeaveCriticalSection(&lock);

		if (u.scale != font_scale) {
			make_font(u.scale);
			u.dirty = true;
			continue;  /* takes the new text widths */
		}

		RECT cr;
		GetClientRect(hwnd, &cr);
		int  w = cr.right, h = cr.bottom;
		bool active = u.visible && u.loaded;

		if (!u.visible || (!active && !u.dirty && w == bw && h == bh)) {
			WaitForSingleObject(wake, INFINITE);
			continue;
		}

		/* one pass per composition: paces the loop at the refresh rate */
		if (active && FAILED(DwmFlush()))
			Sleep(5);

		bool redraw = u.dirty;

		/* any reference left to the back buffer makes this fail: then the old
		   size stays and the next pass tries again */
		if ((w != bw || h != bh) && w > 0 && h > 0
		 && SUCCEEDED(IDXGISwapChain_ResizeBuffers(swap, 0, (UINT) w, (UINT) h, DXGI_FORMAT_UNKNOWN,
		                                           DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE))) {
			bw = w;
			bh = h;
			redraw = true;
		}

		if (active && u.has_video) {
			if (u.backend == BK_DS) {
				if (ds_fresh())
					redraw = true;
			} else {
				LONGLONG pts;
				EnterCriticalSection(&elock);
				if (IMFMediaEngine_OnVideoStreamTick(engine, &pts) == S_OK)
					redraw = true;
				LeaveCriticalSection(&elock);
			}
		}

		/* audio only, or a paused bar: the clock still has to move */
		if (active && u.bar && !redraw) {
			long sec = (long) be_time(u.backend);
			if (sec != last_sec) {
				last_sec = sec;
				redraw = true;
			}
		}

		if (redraw && bw > 0)
			draw(&u, bw, bh);
	}

	ds_gpu_release();
	if (font)
		DeleteObject(font);
	DeleteDC(dim_dc);
	DeleteObject(dim_bmp);
	CoUninitialize();
	return 0;
}

static void render_stop (void) {

	if (render_thread) {
		InterlockedExchange(&quitting, 1);
		SetEvent(wake);
		WaitForSingleObject(render_thread, INFINITE);
		CloseHandle(render_thread);
		render_thread = NULL;
	}
}

/* ========================================================================== */
/* media engine                                                               */
/* ========================================================================== */

static HRESULT STDMETHODCALLTYPE nt_query (IMFMediaEngineNotify *self, REFIID riid, void **out) {

	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFMediaEngineNotify)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* static object: reference counting does nothing */
static ULONG STDMETHODCALLTYPE nt_ref (IMFMediaEngineNotify *self) {

	(void) self;
	return 1;
}

/* runs on a Media Foundation thread: hand the event to the window */
static HRESULT STDMETHODCALLTYPE nt_event (IMFMediaEngineNotify *self, DWORD ev, DWORD_PTR p1, DWORD p2) {

	(void) self;
	(void) p1;
	if (ev == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || ev == MF_MEDIA_ENGINE_EVENT_ERROR
	 || ev == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE)
		PostMessageW(hwnd, WM_ENGINE, ev, (LPARAM) p2);
	return S_OK;
}

static IMFMediaEngineNotifyVtbl notify_vtbl = { nt_query, nt_ref, nt_ref, nt_event };
static IMFMediaEngineNotify     notify      = { &notify_vtbl };

static bool d3d_init (int w, int h) {

	DXGI_SWAP_CHAIN_DESC sd = { 0 };
	sd.BufferDesc.Width  = (UINT) w;
	sd.BufferDesc.Height = (UINT) h;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count  = 1;
	sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount       = 1;
	sd.OutputWindow      = hwnd;
	sd.Windowed          = TRUE;
	sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;  /* blt model: works with layered windows */
	sd.Flags             = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;

	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
	                                           NULL, 0, D3D11_SDK_VERSION, &sd, &swap, &device, NULL, NULL);
	if (FAILED(hr))
		hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
		                                   NULL, 0, D3D11_SDK_VERSION, &sd, &swap, &device, NULL, NULL);
	if (FAILED(hr))
		return false;

	/* the engine uses the device from its own threads */
	ID3D10Multithread *mt;
	if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_ID3D10Multithread, (void **) &mt))) {
		ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
		ID3D10Multithread_Release(mt);
	}

	/* Alt+Enter is ours (borderless fullscreen), not DXGI's */
	IDXGIFactory *f;
	if (SUCCEEDED(IDXGISwapChain_GetParent(swap, &IID_IDXGIFactory, (void **) &f))) {
		IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		IDXGIFactory_Release(f);
	}
	return true;
}

static bool engine_init (void) {

	UINT token;
	IMFAttributes *attr = NULL;
	IMFMediaEngineClassFactory *cf = NULL;
	HRESULT hr;

	hr = MFCreateDXGIDeviceManager(&token, &dxgi_mgr);
	if (SUCCEEDED(hr))
		hr = IMFDXGIDeviceManager_ResetDevice(dxgi_mgr, (IUnknown *) device, token);
	if (SUCCEEDED(hr))
		hr = MFCreateAttributes(&attr, 3);
	if (SUCCEEDED(hr)) {
		IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_DXGI_MANAGER, (IUnknown *) dxgi_mgr);
		IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *) &notify);
		IMFAttributes_SetUINT32(attr, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
		hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
		                      &IID_IMFMediaEngineClassFactory, (void **) &cf);
	}
	if (SUCCEEDED(hr))
		hr = IMFMediaEngineClassFactory_CreateInstance(cf, 0, attr, &engine);
	if (SUCCEEDED(hr)) {
		IMFMediaEngine_QueryInterface(engine, &IID_IMFMediaEngineEx, (void **) &engine_ex);
		IMFMediaEngine_SetLoop(engine, TRUE);
		IMFMediaEngine_SetAutoPlay(engine, TRUE);
	}

	if (cf)
		IMFMediaEngineClassFactory_Release(cf);
	if (attr)
		IMFAttributes_Release(attr);
	return SUCCEEDED(hr);
}

static void open_file (const wchar_t *path) {

	wcsncpy(file_name, path, PATHLEN - 1);
	file_name[PATHLEN - 1] = L'\0';

	EnterCriticalSection(&lock);
	ui.backend   = BK_MF;
	ui.loaded    = false;
	ui.has_video = false;
	ui.paused    = false;
	ui.duration  = 0;
	wcsncpy(ui.idle, base_name(file_name), PATHLEN - 1);
	ui_changed();
	LeaveCriticalSection(&lock);

	SetWindowTextW(hwnd, base_name(file_name));

	BSTR url = SysAllocString(file_name);
	EnterCriticalSection(&elock);
	ds_close();
	IMFMediaEngine_SetSource(engine, url);
	IMFMediaEngine_Play(engine);
	LeaveCriticalSection(&elock);
	SysFreeString(url);
}

static void set_paused (bool p) {

	if (!ui.loaded)
		return;

	EnterCriticalSection(&elock);
	if (ui.backend == BK_DS)
		p ? ds_pause() : ds_play();
	else if (p)
		IMFMediaEngine_Pause(engine);
	else
		IMFMediaEngine_Play(engine);
	LeaveCriticalSection(&elock);

	EnterCriticalSection(&lock);
	ui.paused = p;
	ui_changed();
	LeaveCriticalSection(&lock);
}

static double current_time (void) {

	return be_time(ui.backend);
}

/* approximate: nearest keyframe, for dragging; exact: where it was released */
static void seek (double t, bool exact) {

	if (!ui.loaded || ui.duration <= 0)
		return;

	if (t < 0)
		t = 0;
	if (t > ui.duration - 0.1)
		t = ui.duration - 0.1;

	EnterCriticalSection(&elock);
	if (ui.backend == BK_DS)
		ds_seek(t, exact);
	else if (engine_ex)
		IMFMediaEngineEx_SetCurrentTimeEx(engine_ex, t, exact ? MF_MEDIA_ENGINE_SEEK_MODE_NORMAL
		                                                      : MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE);
	else
		IMFMediaEngine_SetCurrentTime(engine, t);
	LeaveCriticalSection(&elock);
}

/* DirectShow seeks are synchronous: never busy */
static bool engine_busy_seeking (void) {

	if (ui.backend == BK_DS)
		return false;

	EnterCriticalSection(&elock);
	bool b = IMFMediaEngine_IsSeeking(engine);
	LeaveCriticalSection(&elock);
	return b;
}

static void apply_volume (void) {

	EnterCriticalSection(&elock);
	IMFMediaEngine_SetVolume(engine, volume);
	IMFMediaEngine_SetMuted(engine, muted);
	if (ui.backend == BK_DS)
		ds_volume(volume, muted);
	LeaveCriticalSection(&elock);
}

/* ========================================================================== */
/* window                                                                     */
/* ========================================================================== */

static RECT work_area (void) {

	MONITORINFO mi = { .cbSize = sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	return mi.rcWork;
}

static int min_w (void) {

	return px(ui.scale, MIN_W);
}

/* new size keeping the corner of the screen the window is closest to */
static void resize_anchored (int w, int h) {

	RECT r, wa = work_area();
	GetWindowRect(hwnd, &r);

	int x = ((r.left + r.right) / 2 > (wa.left + wa.right) / 2 ? r.right - w : r.left);
	int y = ((r.top + r.bottom) / 2 > (wa.top + wa.bottom) / 2 ? r.bottom - h : r.top);

	SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void size_from_width (int w) {

	if (w < min_w())
		w = min_w();

	RECT r;
	GetWindowRect(hwnd, &r);
	int h = (aspect > 0 ? (int) (w / aspect + 0.5) : r.bottom - r.top);
	resize_anchored(w, h);
}

static void zoom (int dir) {

	if (fullscreen)
		return;

	RECT r;
	GetWindowRect(hwnd, &r);
	int w = r.right - r.left;
	int step = w / 10;
	size_from_width(w + (dir > 0 ? step : -step));
}

static void toggle_fullscreen (void) {

	if (!fullscreen) {
		MONITORINFO mi = { .cbSize = sizeof(mi) };
		if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
			return;
		GetWindowRect(hwnd, &saved_rect);
		fullscreen = true;
		SetWindowPos(hwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top,
		             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
		             SWP_NOACTIVATE);
	} else {
		fullscreen = false;
		SetWindowPos(hwnd, HWND_TOPMOST, saved_rect.left, saved_rect.top,
		             saved_rect.right - saved_rect.left, saved_rect.bottom - saved_rect.top,
		             SWP_NOACTIVATE);
	}
}

static void set_alpha (int a) {

	win_alpha = (a > 255 ? 255 : (a < 25 ? 25 : a));
	LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	/* fully opaque: no layered window at all, the common path stays plain */
	if (win_alpha == 255)
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
	else {
		if (!(ex & WS_EX_LAYERED))
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
		SetLayeredWindowAttributes(hwnd, 0, (BYTE) win_alpha, LWA_ALPHA);
	}
	osd(1000, L"opacity %d%%", win_alpha * 100 / 255);
}

static void set_volume (double v) {

	volume = (v > 1 ? 1 : (v < 0 ? 0 : v));
	apply_volume();
	osd(1000, L"volume %d%%", (int) (volume * 100 + 0.5));
}

static void toggle_mute (void) {

	muted = !muted;
	apply_volume();
	osd(1000, muted ? L"muted" : L"sound on");
}

static void show_bar (void) {

	last_activity = GetTickCount64();
	if (!ui.bar) {
		EnterCriticalSection(&lock);
		ui.bar = true;
		ui_changed();
		LeaveCriticalSection(&lock);
	}
}

static void hide_player (void) {

	if (!ui.visible)
		return;

	hidden_paused = (ui.loaded && !ui.paused);
	if (hidden_paused)
		set_paused(true);

	EnterCriticalSection(&lock);
	ui.visible = false;
	ui.bar     = false;
	ui_changed();
	LeaveCriticalSection(&lock);
	ShowWindow(hwnd, SW_HIDE);
}

static void show_player (void) {

	if (ui.visible)
		return;

	EnterCriticalSection(&lock);
	ui.visible = true;
	ui_changed();
	LeaveCriticalSection(&lock);

	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (hidden_paused)
		set_paused(false);
	hidden_paused = false;
}

/* the file is open (either backend): window size, bar, duration */
static void apply_media (bool video, int vw, int vh, double pic_aspect, double dur) {

	if (!isfinite(dur) || dur < 0)
		dur = 0;

	video = video && vw > 0 && vh > 0;

	EnterCriticalSection(&lock);
	ui.loaded    = true;
	ui.has_video = video;
	ui.duration  = dur;
	if (!video)
		ui.bar = true;  /* audio: the bar is all there is to see */
	ui_changed();
	LeaveCriticalSection(&lock);

	if (video) {
		aspect = (pic_aspect > 0 ? pic_aspect : (double) vw / vh);
		vid_w  = (int) (vh * aspect + 0.5);
	} else
		aspect = 0;

	if (fullscreen)
		return;

	RECT r;
	GetWindowRect(hwnd, &r);
	if (video)
		size_from_width(first_size ? vid_w / 6 : r.right - r.left);
	else
		resize_anchored(r.right - r.left, px(ui.scale, BAR_H) * 3);
	first_size = false;
}

static void on_metadata (void) {

	DWORD vw = 0, vh = 0, ax = 0, ay = 0;

	EnterCriticalSection(&elock);
	BOOL   video = IMFMediaEngine_HasVideo(engine);
	double dur   = IMFMediaEngine_GetDuration(engine);
	if (video) {
		IMFMediaEngine_GetNativeVideoSize(engine, &vw, &vh);
		/* the picture aspect (4:3, 16:9), not the pixel one */
		IMFMediaEngine_GetVideoAspectRatio(engine, &ax, &ay);
	}
	LeaveCriticalSection(&elock);

	apply_volume();
	apply_media(video, (int) vw, (int) vh, (ax > 0 && ay > 0 ? (double) ax / ay : 0), dur);
}

/* Media Foundation gave up on the file: the DirectShow filters of the system get a try */
static bool try_dshow (void) {

	EnterCriticalSection(&elock);
	IMFMediaEngine_Pause(engine);
	bool ok = ds_open(file_name, hwnd, WM_DSHOW, wake);
	LeaveCriticalSection(&elock);
	if (!ok)
		return false;

	int vw = 0, vh = 0;
	bool video = ds_has_video();
	if (video)
		ds_video_size(&vw, &vh);

	EnterCriticalSection(&lock);
	ui.backend = BK_DS;
	ui.paused  = false;
	LeaveCriticalSection(&lock);

	apply_volume();
	apply_media(video, vw, vh, 0, ds_duration());
	return true;
}

static void on_engine_error (HRESULT hr) {

	if (ui.backend == BK_MF && try_dshow())
		return;

	EnterCriticalSection(&elock);
	ds_close();
	LeaveCriticalSection(&elock);

	EnterCriticalSection(&lock);
	ui.backend   = BK_MF;
	ui.loaded    = false;
	ui.has_video = false;
	ui_changed();
	LeaveCriticalSection(&lock);
	switch ((unsigned long) hr) {
		case 0xC00D5212: /* MF_E_TOPO_CODEC_NOT_FOUND */
		case 0x80040265: /* VFW_E_CANNOT_RENDER */
			osd(6000, L"no decoder for this video");
			break;
		case 0xC00D36C4: /* MF_E_UNSUPPORTED_BYTESTREAM_TYPE */
			osd(6000, L"unsupported file format");
			break;
		case 0x80070002: /* ERROR_FILE_NOT_FOUND */
		case 0x80070003:
			osd(6000, L"file not found");
			break;
		default:
			osd(6000, L"cannot play (0x%08lx)", (unsigned long) hr);
			break;
	}
}

/* re-raise to the top of the topmost band whenever another window takes the
   foreground (Alt+Tab included): other topmost windows otherwise win */
static void CALLBACK on_winevent (HWINEVENTHOOK h, DWORD ev, HWND w, LONG obj, LONG child,
                                  DWORD thread, DWORD ms) {

	(void) h; (void) w; (void) obj; (void) child; (void) thread; (void) ms;

	if (ui.visible && (ev == EVENT_SYSTEM_FOREGROUND || ev == EVENT_SYSTEM_SWITCHEND
	                || ev == EVENT_SYSTEM_MINIMIZEEND))
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void update_hover (int x, int y) {

	RECT c;
	GetClientRect(hwnd, &c);

	EnterCriticalSection(&lock);
	LAYOUT l = layout(c.right, c.bottom, &ui);
	int  hx  = -1;
	bool hb  = false;
	if (ui.bar && ui.loaded && in_rect(&l.bar, x, y)) {
		if (x >= l.track.left && x < l.track.right)
			hx = x;
		hb = in_rect(&l.btn, x, y);
	}
	if (hx != ui.hover_x || hb != ui.hover_btn) {
		ui.hover_x   = hx;
		ui.hover_btn = hb;
		ui_changed();
	}
	LeaveCriticalSection(&lock);
}

/* WM_MOUSEMOVE also comes when the window moves under a still cursor:
   only a real movement shows the bar */
static void mouse_moved (int x, int y) {

	if (x != last_mouse.x || y != last_mouse.y) {
		last_mouse = (POINT) { x, y };
		show_bar();
	}
}

static void seek_drag (int x) {

	RECT c;
	GetClientRect(hwnd, &c);
	LAYOUT l = layout(c.right, c.bottom, &ui);

	int tw = l.track.right - l.track.left;
	double frac = (tw > 0 ? (double) (x - l.track.left) / tw : 0);
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;

	EnterCriticalSection(&lock);
	ui.seek_to = frac * ui.duration;
	ui.hover_x = (x < l.track.left ? l.track.left : (x >= l.track.right ? l.track.right - 1 : x));
	ui_changed();
	LeaveCriticalSection(&lock);

	/* one seek in flight at a time: the timer sends the latest target */
	if (engine_busy_seeking())
		seek_pending = true;
	else {
		seek(ui.seek_to, false);
		seek_pending = false;
	}
}

static void on_timer (void) {

	/* global keys: they work without focus, read every tick */
	static bool was_home, was_end;
	bool home = !!(GetAsyncKeyState(VK_HOME) & 0x8000);
	bool end  = !!(GetAsyncKeyState(VK_END)  & 0x8000);
	if (home && !was_home)
		hide_player();
	if (end && !was_end)
		show_player();
	was_home = home;
	was_end  = end;

	ULONGLONG now = GetTickCount64();

	if (ui.seeking && seek_pending && !engine_busy_seeking()) {
		seek(ui.seek_to, false);
		seek_pending = false;
	}

	/* the bar goes away as soon as the cursor leaves the window, or after a
	   while without moving; it stays while paused, seeking or under the cursor */
	if (ui.bar && ui.loaded && ui.has_video && !ui.paused && !ui.seeking) {
		POINT p;
		RECT  c;
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		GetClientRect(hwnd, &c);
		LAYOUT l = layout(c.right, c.bottom, &ui);
		bool out = !in_rect(&c, p.x, p.y);
		if (out || (now - last_activity > HIDE_MS && !in_rect(&l.bar, p.x, p.y))) {
			EnterCriticalSection(&lock);
			ui.bar = false;
			ui_changed();
			LeaveCriticalSection(&lock);
			last_mouse = (POINT) { -1, -1 };
		}
	}

	if (ui.osd[0] && now > osd_until) {
		EnterCriticalSection(&lock);
		ui.osd[0] = L'\0';
		ui_changed();
		LeaveCriticalSection(&lock);
	}
}

static bool bar_click (int x, int y) {

	if (!ui.bar || !ui.loaded)
		return false;

	RECT c;
	GetClientRect(hwnd, &c);
	LAYOUT l = layout(c.right, c.bottom, &ui);
	if (!in_rect(&l.bar, x, y))
		return false;

	if (in_rect(&l.btn, x, y))
		set_paused(!ui.paused);
	else if (ui.duration > 0 && x >= l.track.left - px(ui.scale, 6) && x < l.track.right + px(ui.scale, 6)) {
		EnterCriticalSection(&lock);
		ui.seeking = true;
		LeaveCriticalSection(&lock);
		SetCapture(hwnd);
		seek_drag(x);
	}
	return true;  /* clicks on the bar never move the window */
}

static void on_key (WPARAM k) {

	switch (k) {
		case VK_SPACE:
			set_paused(!ui.paused);
			show_bar();
			break;
		case VK_UP:
			set_paused(false);
			break;
		case VK_DOWN:
			set_paused(true);
			show_bar();
			break;
		case VK_LEFT:
		case VK_RIGHT:
			seek(current_time() + (k == VK_LEFT ? -SEEK_STEP : SEEK_STEP), true);
			show_bar();
			break;
		case VK_F12:
			seek(0, true);
			set_paused(false);
			show_bar();
			break;
		case 'M':
			toggle_mute();
			break;
		case VK_PRIOR:
			set_alpha(win_alpha + 25);
			break;
		case VK_NEXT:
			set_alpha(win_alpha - 25);
			break;
		case VK_OEM_PLUS:
		case VK_ADD:
			zoom(+1);
			break;
		case VK_OEM_MINUS:
		case VK_SUBTRACT:
			zoom(-1);
			break;
		case '0':
			if (!fullscreen && vid_w > 0)
				size_from_width(vid_w / 6);
			break;
		case 'F':
			toggle_fullscreen();
			break;
		case VK_ESCAPE:
			if (fullscreen)
				toggle_fullscreen();
			break;
		case 'Q':
			PostMessageW(hwnd, WM_CLOSE, 0, 0);
			break;
		default:
			break;
	}
}

static LRESULT CALLBACK WndProc (HWND w, UINT msg, WPARAM wparam, LPARAM lparam) {

	switch (msg) {

		case WM_NCHITTEST: {
				if (fullscreen)
					return HTCLIENT;

				POINT p = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				RECT  c;
				ScreenToClient(w, &p);
				GetClientRect(w, &c);

				int  e = px(ui.scale, EDGE);
				bool l = p.x < e, r = p.x >= c.right - e;
				bool t = p.y < e, b = p.y >= c.bottom - e;

				if (t && l) return HTTOPLEFT;
				if (t && r) return HTTOPRIGHT;
				if (b && l) return HTBOTTOMLEFT;
				if (b && r) return HTBOTTOMRIGHT;
				if (l)      return HTLEFT;
				if (r)      return HTRIGHT;
				if (t)      return HTTOP;
				if (b)      return HTBOTTOM;

				/* the bar takes clicks; the rest of the video works as a title
				   bar, so the system moves the window */
				if (ui.bar && ui.loaded) {
					LAYOUT lay = layout(c.right, c.bottom, &ui);
					if (in_rect(&lay.bar, p.x, p.y))
						return HTCLIENT;
				}
				return HTCAPTION;
			}

		case WM_NCMOUSEMOVE: {
				if (!nc_tracking) {
					TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, w, 0 };
					nc_tracking = TrackMouseEvent(&tme);
				}
				POINT p = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				ScreenToClient(w, &p);
				mouse_moved(p.x, p.y);
				update_hover(p.x, p.y);
			}
			break;

		case WM_NCMOUSELEAVE:
			nc_tracking = false;
			update_hover(-1, -1);
			break;

		/* a double click on the video (the "title bar") is fullscreen, not maximize */
		case WM_NCLBUTTONDBLCLK:
			if (wparam == HTCAPTION) {
				toggle_fullscreen();
				return 0;
			}
			break;

		/* keeps the video aspect while an edge is dragged */
		case WM_SIZING:
			if (aspect > 0) {
				RECT *r = (RECT *) lparam;
				int   ww = r->right - r->left, hh = r->bottom - r->top;

				if (wparam == WMSZ_TOP || wparam == WMSZ_BOTTOM) {
					ww = (int) (hh * aspect + 0.5);
					r->right = r->left + ww;
				} else {
					hh = (int) (ww / aspect + 0.5);
					if (wparam == WMSZ_TOP || wparam == WMSZ_TOPLEFT || wparam == WMSZ_TOPRIGHT)
						r->top = r->bottom - hh;
					else
						r->bottom = r->top + hh;
				}
				return TRUE;
			}
			break;

		case WM_GETMINMAXINFO: {
				MINMAXINFO *mm = (MINMAXINFO *) lparam;
				mm->ptMinTrackSize.x = min_w();
				mm->ptMinTrackSize.y = (aspect > 0 ? (int) (min_w() / aspect + 0.5) : px(ui.scale, BAR_H) * 2);
			}
			return 0;

		/* nothing takes it out of the topmost band */
		case WM_WINDOWPOSCHANGING: {
				WINDOWPOS *wp = (WINDOWPOS *) lparam;
				if (!(wp->flags & SWP_NOZORDER) && wp->hwndInsertAfter != HWND_TOPMOST)
					wp->hwndInsertAfter = HWND_TOPMOST;
			}
			break;

		case WM_SIZE:
			EnterCriticalSection(&lock);
			ui_changed();
			LeaveCriticalSection(&lock);
			return 0;

		case WM_DPICHANGED: {
				RECT *r = (RECT *) lparam;
				EnterCriticalSection(&lock);
				ui.scale = HIWORD(wparam) / 96.0f;
				ui_changed();
				LeaveCriticalSection(&lock);
				SetWindowPos(w, HWND_TOPMOST, r->left, r->top, r->right - r->left, r->bottom - r->top,
				             SWP_NOACTIVATE);
			}
			return 0;

		case WM_MOUSEMOVE: {
				int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);

				if (!tracking) {
					TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, w, 0 };
					tracking = TrackMouseEvent(&tme);
				}

				mouse_moved(x, y);

				if (ui.seeking)
					seek_drag(x);
				else
					update_hover(x, y);
			}
			break;

		/* moving between the bar (client) and the video (caption) also "leaves":
		   the timer hides the bar once the cursor is really out of the window */
		case WM_MOUSELEAVE:
			tracking = false;
			update_hover(-1, -1);
			break;

		case WM_LBUTTONDOWN:
			bar_click(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
			break;

		case WM_LBUTTONDBLCLK: {
				int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
				if (!bar_click(x, y))
					toggle_fullscreen();
			}
			break;

		case WM_LBUTTONUP:
			if (ui.seeking) {
				EnterCriticalSection(&lock);
				ui.seeking = false;
				double t = ui.seek_to;
				ui_changed();
				LeaveCriticalSection(&lock);
				seek_pending = false;
				seek(t, true);
				last_activity = GetTickCount64();
			}
			if (GetCapture() == w)
				ReleaseCapture();
			break;

		case WM_MOUSEWHEEL: {
				int dir = ((short) HIWORD(wparam) < 0 ? -1 : 1);
				if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)
					set_volume(volume + dir * 0.05);
				else if (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT)
					set_alpha(win_alpha + dir * 25);
				else
					zoom(dir);
			}
			break;

		case WM_KEYDOWN:
			on_key(wparam);
			break;

		case WM_SYSKEYDOWN:
			if (wparam == VK_RETURN) {
				toggle_fullscreen();
				return 0;
			}
			break;

		case WM_DROPFILES: {
				wchar_t path[PATHLEN];
				HDROP drop = (HDROP) wparam;
				/* several files at once: only the first is played */
				if (DragQueryFileW(drop, 0, path, PATHLEN) > 0)
					open_file(path);
				DragFinish(drop);
			}
			break;

		case WM_COPYDATA: {
				COPYDATASTRUCT *cd = (COPYDATASTRUCT *) lparam;
				if (cd->dwData == CD_OPEN && cd->lpData && cd->cbData >= sizeof(wchar_t)) {
					wchar_t path[PATHLEN];
					size_t  n = cd->cbData / sizeof(wchar_t);
					if (n > PATHLEN - 1)
						n = PATHLEN - 1;
					memcpy(path, cd->lpData, n * sizeof(wchar_t));
					path[n] = L'\0';
					open_file(path);
				}
				if (cd->dwData == CD_OPEN || cd->dwData == CD_SHOW) {
					hidden_paused = false;
					show_player();
				}
			}
			return TRUE;

		case WM_DSHOW:
			if (ui.backend == BK_DS) {
				EnterCriticalSection(&elock);
				HRESULT hr = ds_events();
				LeaveCriticalSection(&elock);
				if (FAILED(hr))
					on_engine_error(hr);
			}
			return 0;

		case WM_ENGINE:
			/* late events of a source that went over to DirectShow */
			if (ui.backend == BK_DS)
				return 0;
			if (wparam == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || wparam == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE)
				on_metadata();
			else if (wparam == MF_MEDIA_ENGINE_EVENT_ERROR)
				on_engine_error((HRESULT) lparam);
			return 0;

		case WM_TIMER:
			on_timer();
			return 0;

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
			ValidateRect(w, NULL);
			EnterCriticalSection(&lock);
			ui_changed();
			LeaveCriticalSection(&lock);
			return 0;

		case WM_CLOSE:
			render_stop();
			DestroyWindow(w);
			return 0;

		case WM_DESTROY:
			KillTimer(w, 1);
			if (hook)
				UnhookWinEvent(hook);
			PostQuitMessage(0);
			return 0;
	}

	return DefWindowProcW(w, msg, wparam, lparam);
}

/* ========================================================================== */
/* entry                                                                      */
/* ========================================================================== */

static const wchar_t *HELP =
	L"\n vplay - small always-on-top video player (Media Foundation)\n\n"
	L" usage: vplay [file]      a running vplay plays the file instead\n\n"
	L"   drop a file           plays it\n"
	L"   drag                  moves the window\n"
	L"   drag an edge          resizes (keeps the aspect)\n"
	L"   mouse over            shows the time bar (click/drag to seek)\n"
	L"   double click          fullscreen\n"
	L"   space / up / down     play-pause / play / pause\n"
	L"   left / right          -10s / +10s\n"
	L"   F12                   restart\n"
	L"   wheel                 zoom     (Shift: opacity, Ctrl: volume)\n"
	L"   + / - / 0             zoom / initial size\n"
	L"   PgUp / PgDn           opacity\n"
	L"   M                     mute\n"
	L"   Alt+Enter / F / Esc   fullscreen / leave fullscreen\n"
	L"   Home / End            hide / show (global)\n"
	L"   Q                     quit\n\n";

int WINAPI wWinMain (HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {

	(void) prev;
	(void) cmdline;
	(void) show;

	wchar_t path[PATHLEN] = { 0 };
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (argv && argc >= 2) {
		if (!wcscmp(argv[1], L"-h") || !wcscmp(argv[1], L"--help")) {
			console_print(HELP);
			LocalFree(argv);
			return 0;
		}
		/* full path: a running instance has another current directory */
		if (!GetFullPathNameW(argv[1], PATHLEN, path, NULL))
			wcsncpy(path, argv[1], PATHLEN - 1);
	}
	if (argv)
		LocalFree(argv);

	/* one player: hand the file to the running one */
	HWND other = FindWindowW(APP_CLASS, NULL);
	if (other) {
		COPYDATASTRUCT cd = { CD_SHOW, 0, NULL };
		if (path[0])
			cd = (COPYDATASTRUCT) { CD_OPEN, (DWORD) ((wcslen(path) + 1) * sizeof(wchar_t)), path };
		SendMessageW(other, WM_COPYDATA, 0, (LPARAM) &cd);
		return 0;
	}

	if (path[0] && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		wchar_t msg[PATHLEN + 64];
		swprintf(msg, PATHLEN + 64, L"vplay: %ls: No such file or directory\n", path);
		console_print(msg);
		return 1;
	}

	/* 1:1 pixels on scaled displays */
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
		MessageBoxW(NULL, L"Media Foundation is not available.", L"vplay", MB_ICONERROR);
		return 1;
	}

	InitializeCriticalSection(&lock);
	InitializeCriticalSection(&elock);
	wake = CreateEventW(NULL, FALSE, FALSE, NULL);

	ui.scale   = GetDpiForSystem() / 96.0f;
	ui.hover_x = -1;
	ui.visible = true;
	wcscpy(ui.idle, L"drop a video here");

	WNDCLASSW wc = { 0 };
	wc.style         = CS_DBLCLKS;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
	wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
	wc.lpszClassName = APP_CLASS;
	RegisterClassW(&wc);

	/* bottom right corner of the primary work area */
	RECT wa;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	int w = px(ui.scale, 320), h = px(ui.scale, 180), m = px(ui.scale, 16);

	/* WS_EX_TOOLWINDOW: no taskbar button and out of Alt+Tab */
	hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, APP_CLASS, L"vplay", WS_POPUP,
	                       wa.right - w - m, wa.bottom - h - m, w, h, NULL, NULL, inst, NULL);
	if (!hwnd)
		return 1;

	ui.scale = GetDpiForWindow(hwnd) / 96.0f;

	if (!d3d_init(w, h) || !engine_init()) {
		MessageBoxW(NULL, L"Could not start Direct3D 11 / the media engine.", L"vplay", MB_ICONERROR);
		return 1;
	}

	DragAcceptFiles(hwnd, TRUE);
	SetTimer(hwnd, 1, TIMER_MS, NULL);
	hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, NULL, on_winevent,
	                       0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

	render_thread = CreateThread(NULL, 0, render_main, NULL, 0, NULL);

	/* shown without taking the focus: the work in front keeps the keyboard */
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);

	if (path[0])
		open_file(path);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	render_stop();
	ds_close();
	if (engine) {
		IMFMediaEngine_Shutdown(engine);
		if (engine_ex)
			IMFMediaEngineEx_Release(engine_ex);
		IMFMediaEngine_Release(engine);
	}
	if (dxgi_mgr)
		IMFDXGIDeviceManager_Release(dxgi_mgr);
	if (swap)
		IDXGISwapChain_Release(swap);
	if (device)
		ID3D11Device_Release(device);
	MFShutdown();
	CoUninitialize();
	return (int) msg.wParam;
}
