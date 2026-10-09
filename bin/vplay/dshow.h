#ifndef DSHOW_H
#define DSHOW_H

/*
	DirectShow fallback: plays what Media Foundation has no decoder for, through
	the DirectShow filters installed on the system (LAV Filters from K-Lite,
	for example). Frames come back as RGB32 through a Sample Grabber and are
	drawn with GDI; audio goes to the default renderer.
*/

#include <windows.h>
#include <stdbool.h>

/* builds and starts the graph. Graph events are posted to `notify` as `msg`
   (call ds_events then); every new frame sets `wake` */
bool    ds_open (const wchar_t *path, HWND notify, UINT msg, HANDLE wake);
void    ds_close (void);

bool    ds_has_video (void);
void    ds_video_size (int *w, int *h);
double  ds_duration (void);
double  ds_time (void);

void    ds_play (void);
void    ds_pause (void);
void    ds_seek (double t, bool exact);
void    ds_volume (double v, bool muted);

/* handles the queued graph events: restarts at the end (loop);
   returns the error code of an aborted playback, S_OK otherwise */
HRESULT ds_events (void);

/* true once per new frame */
bool    ds_fresh (void);

/* NV12 frames: the GPU video processor converts and scales the latest frame
   into `target` (w x h, black bars). Render thread only; false when the frame
   is not NV12 or the GPU path failed */
struct ID3D11Device;
struct ID3D11Texture2D;
bool    ds_draw_gpu (struct ID3D11Device *dev, struct ID3D11Texture2D *target, int w, int h);
void    ds_gpu_release (void);

/* RGB32 frames: draws the latest frame into dc with GDI, fitted to w x h with
   black bars; false when there is no RGB32 frame */
bool    ds_draw (HDC dc, int w, int h);

#endif
