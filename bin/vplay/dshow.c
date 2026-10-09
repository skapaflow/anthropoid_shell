/*****************************************************************************
	dshow - DirectShow fallback for vplay.

	Graph: source -> (splitter) -> decoder -> Sample Grabber -> Null Renderer,
	plus the audio stream to the default audio renderer. The Null Renderer
	still waits for each sample's time, so the audio clock paces the frames;
	the grabber callback copies each one for the render thread.

	The grabber asks for NV12 first: the decoder's own format, so no color
	conversion on the CPU; the GPU video processor converts and scales it.
	RGB32 (drawn with GDI) is the fallback for decoders without NV12.

	The main thread builds, controls and tears down the graph; the render
	thread calls ds_fresh, ds_draw and ds_time. The filter graph manager is
	free threaded; the frame buffer has its own lock.
*****************************************************************************/

#define COBJMACROS
#define _WIN32_WINNT 0x0A00

#undef  UNICODE
#define UNICODE
#undef  _UNICODE
#define _UNICODE

#include <windows.h>
#include <dshow.h>
#include <qedit.h>
#include <d3d11.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dshow.h"

/* in libstrmiids, but missing from the mingw headers */
extern const CLSID CLSID_NullRenderer;

static IGraphBuilder  *graph;
static IMediaControl  *control;
static IMediaSeeking  *seeking;
static IMediaEventEx  *events;
static IBasicAudio    *audio;
static IBaseFilter    *grab_f;
static ISampleGrabber *grab;
static HANDLE          wake;
static bool            has_video;

/* latest frame, guarded by flock */
static CRITICAL_SECTION flock;
static bool             flock_ready;
static BYTE            *frame;
static size_t           frame_cap, frame_len;
static BITMAPINFOHEADER frame_bih;
static RECT             frame_src;   /* visible part, top-left origin */
static bool             frame_nv12;  /* else RGB32 */
static volatile LONG    fresh;
static volatile LONG    uploaded;    /* the GPU texture has the latest frame */

/* the base classes' DeleteMediaType, which C does not have */
static void free_media_type (AM_MEDIA_TYPE *mt) {

	if (mt->cbFormat && mt->pbFormat)
		CoTaskMemFree(mt->pbFormat);
	if (mt->pUnk)
		IUnknown_Release(mt->pUnk);
	mt->cbFormat = 0;
	mt->pbFormat = NULL;
	mt->pUnk     = NULL;
}

/* caller holds flock */
static void take_format (const AM_MEDIA_TYPE *mt) {

	if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo) || mt->cbFormat < sizeof(VIDEOINFOHEADER))
		return;

	const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER *) mt->pbFormat;
	frame_bih  = vih->bmiHeader;
	frame_nv12 = IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_NV12);
	if (!frame_nv12) {
		frame_bih.biCompression = BI_RGB;  /* RGB32 may come as BI_BITFIELDS */
		frame_bih.biSizeImage   = 0;
	}

	frame_src = vih->rcSource;
	if (IsRectEmpty(&frame_src))
		frame_src = (RECT) { 0, 0, frame_bih.biWidth, abs(frame_bih.biHeight) };
}

/* ---- sample grabber callback (streaming thread) -------------------------- */

static HRESULT STDMETHODCALLTYPE cb_query (ISampleGrabberCB *self, REFIID riid, void **out) {

	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ISampleGrabberCB)) {
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

/* static object: reference counting does nothing */
static ULONG STDMETHODCALLTYPE cb_ref (ISampleGrabberCB *self) {

	(void) self;
	return 1;
}

static HRESULT STDMETHODCALLTYPE cb_sample (ISampleGrabberCB *self, double t, IMediaSample *s) {

	(void) self;
	(void) t;
	BYTE *p;
	AM_MEDIA_TYPE *mt = NULL;

	if (FAILED(IMediaSample_GetPointer(s, &p)))
		return S_OK;
	long n = IMediaSample_GetActualDataLength(s);
	if (n <= 0)
		return S_OK;

	EnterCriticalSection(&flock);

	/* the decoder may change the format (stride) in the middle of the stream */
	if (IMediaSample_GetMediaType(s, &mt) == S_OK && mt) {
		take_format(mt);
		free_media_type(mt);
		CoTaskMemFree(mt);
	}

	if ((size_t) n > frame_cap) {
		BYTE *nb = realloc(frame, (size_t) n);
		if (!nb) {
			LeaveCriticalSection(&flock);
			return S_OK;
		}
		frame     = nb;
		frame_cap = (size_t) n;
	}
	memcpy(frame, p, (size_t) n);
	frame_len = (size_t) n;

	LeaveCriticalSection(&flock);

	InterlockedExchange(&uploaded, 0);
	InterlockedExchange(&fresh, 1);
	if (wake)
		SetEvent(wake);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_buffer (ISampleGrabberCB *self, double t, BYTE *b, LONG n) {

	(void) self; (void) t; (void) b; (void) n;
	return S_OK;
}

static ISampleGrabberCBVtbl cb_vtbl = { cb_query, cb_ref, cb_ref, cb_sample, cb_buffer };
static ISampleGrabberCB     cb      = { &cb_vtbl };

/* ---- graph --------------------------------------------------------------- */

void ds_close (void) {

	if (control)
		IMediaControl_Stop(control);
	if (events)
		IMediaEventEx_SetNotifyWindow(events, 0, 0, 0);
	if (grab)
		ISampleGrabber_SetCallback(grab, NULL, 0);

	if (audio)   { IBasicAudio_Release(audio);       audio   = NULL; }
	if (events)  { IMediaEventEx_Release(events);    events  = NULL; }
	if (seeking) { IMediaSeeking_Release(seeking);   seeking = NULL; }
	if (control) { IMediaControl_Release(control);  control = NULL; }
	if (grab)    { ISampleGrabber_Release(grab);     grab    = NULL; }
	if (grab_f)  { IBaseFilter_Release(grab_f);      grab_f  = NULL; }
	if (graph)   { IGraphBuilder_Release(graph);     graph   = NULL; }

	if (flock_ready) {
		EnterCriticalSection(&flock);
		free(frame);
		frame     = NULL;
		frame_cap = frame_len = 0;
		LeaveCriticalSection(&flock);
	}

	has_video = false;
	InterlockedExchange(&fresh, 0);
}

bool ds_open (const wchar_t *path, HWND notify, UINT msg, HANDLE wake_event) {

	ICaptureGraphBuilder2 *cgb    = NULL;
	IBaseFilter           *src    = NULL;
	IBaseFilter           *null_f = NULL;
	HRESULT hr;

	if (!flock_ready) {
		InitializeCriticalSection(&flock);
		flock_ready = true;
	}

	ds_close();
	wake = wake_event;

	hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
	                      &IID_IGraphBuilder, (void **) &graph);
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER,
		                      &IID_ICaptureGraphBuilder2, (void **) &cgb);
	if (SUCCEEDED(hr))
		hr = ICaptureGraphBuilder2_SetFiltergraph(cgb, graph);
	if (SUCCEEDED(hr))
		hr = IGraphBuilder_AddSourceFilter(graph, path, L"source", &src);

	/* video: decoder -> grabber that only accepts RGB32 -> null renderer */
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,
		                      &IID_IBaseFilter, (void **) &grab_f);
	if (SUCCEEDED(hr))
		hr = IBaseFilter_QueryInterface(grab_f, &IID_ISampleGrabber, (void **) &grab);
	if (SUCCEEDED(hr)) {
		ISampleGrabber_SetBufferSamples(grab, FALSE);
		ISampleGrabber_SetOneShot(grab, FALSE);
		ISampleGrabber_SetCallback(grab, &cb, 0);
		hr = IGraphBuilder_AddFilter(graph, grab_f, L"grabber");
	}
	if (SUCCEEDED(hr))
		hr = CoCreateInstance(&CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER,
		                      &IID_IBaseFilter, (void **) &null_f);
	if (SUCCEEDED(hr))
		hr = IGraphBuilder_AddFilter(graph, null_f, L"null");

	if (SUCCEEDED(hr)) {
		/* NV12 first (GPU conversion), RGB32 when the decoder has no NV12 */
		const GUID *subtypes[2] = { &MEDIASUBTYPE_NV12, &MEDIASUBTYPE_RGB32 };
		for (int i = 0; i < 2 && !has_video; i++) {
			AM_MEDIA_TYPE mt = { 0 };
			mt.majortype  = MEDIATYPE_Video;
			mt.subtype    = *subtypes[i];
			mt.formattype = FORMAT_VideoInfo;
			ISampleGrabber_SetMediaType(grab, &mt);
			has_video = SUCCEEDED(ICaptureGraphBuilder2_RenderStream(cgb, NULL, &MEDIATYPE_Video,
			                                                         (IUnknown *) src, grab_f, null_f));
		}
		bool has_audio = SUCCEEDED(ICaptureGraphBuilder2_RenderStream(cgb, NULL, &MEDIATYPE_Audio,
		                                                              (IUnknown *) src, NULL, NULL));
		if (!has_video && !has_audio)
			hr = VFW_E_CANNOT_RENDER;
	}

	if (SUCCEEDED(hr) && has_video) {
		AM_MEDIA_TYPE mt;
		if (SUCCEEDED(ISampleGrabber_GetConnectedMediaType(grab, &mt))) {
			EnterCriticalSection(&flock);
			take_format(&mt);
			LeaveCriticalSection(&flock);
			free_media_type(&mt);
		}
	}

	if (SUCCEEDED(hr))
		hr = IGraphBuilder_QueryInterface(graph, &IID_IMediaControl, (void **) &control);
	if (SUCCEEDED(hr))
		hr = IGraphBuilder_QueryInterface(graph, &IID_IMediaSeeking, (void **) &seeking);
	if (SUCCEEDED(hr))
		hr = IGraphBuilder_QueryInterface(graph, &IID_IMediaEventEx, (void **) &events);
	if (SUCCEEDED(hr)) {
		/* no audio stream: no IBasicAudio either, and that is fine */
		if (FAILED(IGraphBuilder_QueryInterface(graph, &IID_IBasicAudio, (void **) &audio)))
			audio = NULL;
		IMediaEventEx_SetNotifyWindow(events, (OAHWND) notify, (long) msg, 0);
		hr = IMediaControl_Run(control);
	}

	if (null_f)
		IBaseFilter_Release(null_f);
	if (src)
		IBaseFilter_Release(src);
	if (cgb)
		ICaptureGraphBuilder2_Release(cgb);

	if (FAILED(hr)) {
		ds_close();
		return false;
	}
	return true;
}

bool ds_has_video (void) {

	return has_video;
}

void ds_video_size (int *w, int *h) {

	EnterCriticalSection(&flock);
	*w = frame_src.right - frame_src.left;
	*h = frame_src.bottom - frame_src.top;
	LeaveCriticalSection(&flock);
}

double ds_duration (void) {

	LONGLONG d = 0;
	if (!seeking || FAILED(IMediaSeeking_GetDuration(seeking, &d)))
		return 0;
	return d / 1e7;
}

double ds_time (void) {

	LONGLONG t = 0;
	if (!seeking || FAILED(IMediaSeeking_GetCurrentPosition(seeking, &t)))
		return 0;
	return t / 1e7;
}

void ds_play (void) {

	if (control)
		IMediaControl_Run(control);
}

void ds_pause (void) {

	if (control)
		IMediaControl_Pause(control);
}

/* approximate goes to the nearest keyframe: fast while dragging the bar */
void ds_seek (double t, bool exact) {

	if (!seeking)
		return;

	LONGLONG pos = (LONGLONG) (t * 1e7);
	DWORD flags  = AM_SEEKING_AbsolutePositioning | (exact ? 0 : AM_SEEKING_SeekToKeyFrame);
	IMediaSeeking_SetPositions(seeking, &pos, flags, NULL, AM_SEEKING_NoPositioning);
}

/* IBasicAudio takes hundredths of a decibel: 0 is full, -10000 is silence */
void ds_volume (double v, bool muted) {

	if (!audio)
		return;

	long db = -10000;
	if (!muted && v > 0.0001) {
		db = (long) (2000.0 * log10(v));
		if (db < -10000)
			db = -10000;
	}
	IBasicAudio_put_Volume(audio, db);
}

HRESULT ds_events (void) {

	HRESULT  err = S_OK;
	long     code;
	LONG_PTR p1, p2;

	if (!events)
		return S_OK;

	while (IMediaEventEx_GetEvent(events, &code, &p1, &p2, 0) == S_OK) {
		if (code == EC_COMPLETE) {
			/* loop: back to the start, the graph keeps running */
			ds_seek(0, true);
			IMediaControl_Run(control);
		} else if (code == EC_ERRORABORT || code == EC_ERRORABORTEX)
			err = (HRESULT) p1;
		IMediaEventEx_FreeEventParams(events, code, p1, p2);
	}
	return err;
}

bool ds_fresh (void) {

	return InterlockedExchange(&fresh, 0) != 0;
}

bool ds_draw (HDC dc, int w, int h) {

	bool drawn = false;

	EnterCriticalSection(&flock);

	int fw     = frame_bih.biWidth;
	int fh     = abs(frame_bih.biHeight);
	size_t need = (size_t) fw * 4 * (size_t) fh;
	int sw     = frame_src.right - frame_src.left;
	int sh     = frame_src.bottom - frame_src.top;

	if (frame && !frame_nv12 && frame_len >= need && fw > 0 && sw > 0 && sh > 0) {

		/* fit keeping the aspect, centered */
		int dw = w, dh = (int) ((double) w * sh / sw + 0.5);
		if (dh > h) {
			dh = h;
			dw = (int) ((double) h * sw / sh + 0.5);
		}
		int dx = (w - dw) / 2, dy = (h - dh) / 2;

		SetDCBrushColor(dc, RGB(0, 0, 0));
		HBRUSH black = (HBRUSH) GetStockObject(DC_BRUSH);
		RECT bars[4] = { { 0, 0, w, dy }, { 0, dy + dh, w, h },
		                 { 0, dy, dx, dy + dh }, { dx + dw, dy, w, dy + dh } };
		for (int i = 0; i < 4; i++)
			if (!IsRectEmpty(&bars[i]))
				FillRect(dc, &bars[i], black);

		/* a bottom-up DIB counts the source y from the bottom */
		int sy = (frame_bih.biHeight > 0 ? fh - frame_src.bottom : frame_src.top);

		SetStretchBltMode(dc, HALFTONE);
		SetBrushOrgEx(dc, 0, 0, NULL);
		drawn = StretchDIBits(dc, dx, dy, dw, dh, frame_src.left, sy, sw, sh,
		                      frame, (BITMAPINFO *) &frame_bih, DIB_RGB_COLORS, SRCCOPY) != 0;
	}

	LeaveCriticalSection(&flock);
	return drawn;
}

/* ---- GPU path for NV12 (render thread) ----------------------------------- */

static ID3D11Device                   *gdev;
static ID3D11DeviceContext            *dctx;
static ID3D11VideoDevice              *vdev;
static ID3D11VideoContext             *vctx;
static ID3D11VideoProcessorEnumerator *venum;
static ID3D11VideoProcessor           *vproc;
static ID3D11Texture2D                *tex_def;    /* video processor input   */
static ID3D11Texture2D                *tex_stage;  /* CPU writes the frame here */
static ID3D11VideoProcessorInputView  *in_view;
static int                             tex_w, tex_h;

#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

void ds_gpu_release (void) {

	RELEASE(in_view);
	RELEASE(vproc);
	RELEASE(venum);
	RELEASE(tex_stage);
	RELEASE(tex_def);
	RELEASE(vctx);
	RELEASE(vdev);
	RELEASE(dctx);
	gdev  = NULL;
	tex_w = tex_h = 0;
}

/* textures and video processor for frames of fw x fh; kept while the size holds */
static bool gpu_setup (ID3D11Device *dev, int fw, int fh) {

	if (dev == gdev && fw == tex_w && fh == tex_h && in_view)
		return true;

	ds_gpu_release();
	gdev = dev;

	if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **) &vdev)))
		return false;
	ID3D11Device_GetImmediateContext(dev, &dctx);
	if (FAILED(ID3D11DeviceContext_QueryInterface(dctx, &IID_ID3D11VideoContext, (void **) &vctx)))
		return false;

	D3D11_TEXTURE2D_DESC td = { 0 };
	td.Width            = (UINT) fw;
	td.Height           = (UINT) fh;
	td.MipLevels        = 1;
	td.ArraySize        = 1;
	td.Format           = DXGI_FORMAT_NV12;
	td.SampleDesc.Count = 1;
	td.Usage            = D3D11_USAGE_DEFAULT;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex_def)))
		return false;
	td.Usage            = D3D11_USAGE_STAGING;
	td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex_stage)))
		return false;

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = { 0 };
	cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	cd.InputWidth       = (UINT) fw;
	cd.InputHeight      = (UINT) fh;
	cd.OutputWidth      = (UINT) fw;
	cd.OutputHeight     = (UINT) fh;
	cd.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(ID3D11VideoDevice_CreateVideoProcessorEnumerator(vdev, &cd, &venum))
	 || FAILED(ID3D11VideoDevice_CreateVideoProcessor(vdev, venum, 0, &vproc)))
		return false;

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = { 0 };
	iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	if (FAILED(ID3D11VideoDevice_CreateVideoProcessorInputView(vdev, (ID3D11Resource *) tex_def,
	                                                           venum, &iv, &in_view)))
		return false;

	/* plain conversion and scaling, no driver "enhancements" */
	ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(vctx, vproc, 0, FALSE);

	tex_w = fw;
	tex_h = fh;
	InterlockedExchange(&uploaded, 0);
	return true;
}

/* frame (NV12: Y rows, then interleaved UV rows, same stride) -> texture;
   caller holds flock */
static bool gpu_upload (void) {

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(ID3D11DeviceContext_Map(dctx, (ID3D11Resource *) tex_stage, 0, D3D11_MAP_WRITE, 0, &m)))
		return false;

	size_t      stride = (size_t) frame_bih.biWidth;
	BYTE       *dst    = m.pData;
	const BYTE *src    = frame;

	for (int y = 0; y < tex_h; y++)
		memcpy(dst + (size_t) y * m.RowPitch, src + (size_t) y * stride, (size_t) tex_w);

	/* the UV plane follows the Y plane in the mapped texture too */
	dst += (size_t) m.RowPitch * (size_t) tex_h;
	src += stride * (size_t) tex_h;
	for (int y = 0; y < tex_h / 2; y++)
		memcpy(dst + (size_t) y * m.RowPitch, src + (size_t) y * stride, (size_t) tex_w);

	ID3D11DeviceContext_Unmap(dctx, (ID3D11Resource *) tex_stage, 0);
	ID3D11DeviceContext_CopyResource(dctx, (ID3D11Resource *) tex_def, (ID3D11Resource *) tex_stage);
	return true;
}

bool ds_draw_gpu (ID3D11Device *dev, ID3D11Texture2D *target, int w, int h) {

	EnterCriticalSection(&flock);

	int    fw   = frame_bih.biWidth;
	int    fh   = abs(frame_bih.biHeight) & ~1;
	size_t need = (size_t) fw * (size_t) fh * 3 / 2;
	RECT   src  = frame_src;

	bool ok = frame && frame_nv12 && fw > 0 && fh > 0 && frame_len >= need
	       && gpu_setup(dev, fw, fh);
	if (ok && !uploaded) {
		ok = gpu_upload();
		if (ok)
			InterlockedExchange(&uploaded, 1);
	}

	LeaveCriticalSection(&flock);
	if (!ok)
		return false;

	int sw = src.right - src.left, sh = src.bottom - src.top;
	if (sw <= 0 || sh <= 0)
		return false;

	/* fit keeping the aspect, centered; the background color fills the bars */
	int dw = w, dh = (int) ((double) w * sh / sw + 0.5);
	if (dh > h) {
		dh = h;
		dw = (int) ((double) h * sw / sh + 0.5);
	}
	RECT dst  = { (w - dw) / 2, (h - dh) / 2, (w - dw) / 2 + dw, (h - dh) / 2 + dh };
	RECT full = { 0, 0, w, h };

	ID3D11VideoProcessorOutputView *out = NULL;
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = { 0 };
	ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if (FAILED(ID3D11VideoDevice_CreateVideoProcessorOutputView(vdev, (ID3D11Resource *) target,
	                                                            venum, &ov, &out)))
		return false;

	D3D11_VIDEO_COLOR black = { 0 };
	black.RGBA.A = 1.0f;

	/* BT.709 for HD, BT.601 below it; limited range in, full range out */
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = { 0 }, out_cs = { 0 };
	in_cs.YCbCr_Matrix  = (sh >= 720);
	in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

	ID3D11VideoContext_VideoProcessorSetOutputTargetRect(vctx, vproc, TRUE, &full);
	ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(vctx, vproc, FALSE, &black);
	ID3D11VideoContext_VideoProcessorSetOutputColorSpace(vctx, vproc, &out_cs);
	ID3D11VideoContext_VideoProcessorSetStreamColorSpace(vctx, vproc, 0, &in_cs);
	ID3D11VideoContext_VideoProcessorSetStreamSourceRect(vctx, vproc, 0, TRUE, &src);
	ID3D11VideoContext_VideoProcessorSetStreamDestRect(vctx, vproc, 0, TRUE, &dst);

	D3D11_VIDEO_PROCESSOR_STREAM st = { 0 };
	st.Enable        = TRUE;
	st.pInputSurface = in_view;
	HRESULT hr = ID3D11VideoContext_VideoProcessorBlt(vctx, vproc, out, 0, 1, &st);

	ID3D11VideoProcessorOutputView_Release(out);
	return SUCCEEDED(hr);
}
