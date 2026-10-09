# AGENT.md — vplay (bin/vplay of AntShell)

## Purpose

A small, light and fast video player for Windows: a **borderless, always-on-top** window that sits in a corner of the screen playing a local video while you work on other things. It is not a full player: exotic files are left to mpv.

What it does, and nothing more:
- plays the video in a loop (mostly `.mp4`);
- a time bar that shows up on mouse over, with the current/total time and the time under the cursor, to pick a part of the video;
- opens by drag and drop or from the terminal (`vplay file`; with a vplay already open, the file goes to the existing window);
- resizes from the edges keeping the aspect, moves by dragging the video, fullscreen inside the window;
- global **Home/End** keys to hide and show it (the author's choice: kept from the old vplay).

**Platform:** Windows 10+ (Media Foundation, D3D11, DirectShow)  
**Compiler:** LLVM-MinGW (clang), through AntShell's `bin/Makefile`  
**Output:** `bin/vplay.exe` (~100 KB, system DLLs only)

---

## History

1. **The MCI vplay**: `bin/vplay/vplay.c` up to this rewrite (it is in the git history), and an older version before it: MCI (`mciSendString`, `mpegvideo` driver → DirectShow). They work, but:
   - DirectShow draws into a child window of its own: GDI drawn on top flickers, and `WM_NCHITTEST` for edge resizing never reaches us;
   - imprecise seeking (`play from` on every mouse move stutters);
   - the topmost window falls behind with Alt+Tab. The older version held up a bit better only because it called `SetWindowPos(HWND_TOPMOST)` without `SWP_NOACTIVATE` on almost every action; neither recovered.
2. **mpv study** (its source tree): mpv's `--ontop` on Windows is a single `SetWindowPos(HWND_TOPMOST)` (`video/out/w32_common.c`) with no reinforcement, so it does not hold up better than vplay. Good window ideas came from it (`borderless_nchittest`, `handle_sizing`, letting the system move the window). Rejected as a base: libmpv is a DLL of tens of MB (FFmpeg inside), and linking it statically brings in the GPL and means building FFmpeg, libplacebo, libass...
3. **Media Foundation** (`IMFMediaEngine`, the modern API that replaced DirectShow): chosen because it ships with the system, decodes in hardware and seeks exactly. *Frame server* mode: each frame goes into the back buffer of our own swap chain and the bar is drawn on top with GDI. One window, no child window.
4. **Finding:** `.mp4` files downloaded from YouTube are often **AV1**, and Media Foundation has no AV1 decoder unless the Store extension is installed. The MCI vplay played them because MCI goes through DirectShow, where a codec pack (K-Lite) had registered **LAV Filters**.
5. **MPC-HC study** (its source tree): its decoder is LAV Filters itself (`src/thirdparty/LAVFilters`). The source is no use here (C++/MFC/Visual Studio); the useful piece was already installed on the system.
6. **DirectShow fallback** (`dshow.c`): when Media Foundation fails, the same file goes to a DirectShow graph that uses the installed filters (LAV). The first version, RGB32 + `StretchDIBits`, cost 1.65 cores on 1080p AV1; the NV12 + GPU video processor version costs 1.0 (the MCI vplay, 0.93). What is left is LAV itself decoding AV1 in software (the test machine's GPU, an Intel HD 5500, has no AV1 hardware decoding).
7. **Move into the tree:** developed outside the repository with its own Makefile, then moved to `bin/vplay` in place of the MCI vplay; it builds through `bin/Makefile` like the other programs.

---

## Layout

```
vplay.c    window, input, rendering, Media Foundation, backend choice
dshow.c    DirectShow fallback: graph, Sample Grabber, NV12 path on the GPU
dshow.h    interface of dshow.c
```

No Makefile of its own (project rule): the `vplay.exe` rule lives in `bin/Makefile` (`VPLAY_SRCS`, `VPLAY_LIBS`).

### Threads

| Thread | Does |
|---|---|
| main | window, mouse/keyboard, commands to the backend, MF events (`WM_ENGINE`) and DirectShow events (`WM_DSHOW`), 50 ms timer (Home/End, hiding the bar, pending seek, OSD) |
| render | waits for the compositor (`DwmFlush`), asks for a new frame (`OnVideoStreamTick` or `ds_fresh`), resizes the swap chain and draws; keeps running during the modal move/resize loops |
| DirectShow streaming | `cb_sample` copies each frame into the buffer of `dshow.c` |

Synchronization: `lock` guards the `UI` struct (the render thread takes a copy each pass); `elock` serializes calls into Media Foundation and the graph; `flock` (in `dshow.c`) guards the frame. No lock is taken while holding another.

### Backends

| | Media Foundation (`BK_MF`) | DirectShow (`BK_DS`) |
|---|---|---|
| When | always first | MF error on open (`try_dshow` in `on_engine_error`) |
| Decodes with | Windows decoders, hardware when available | installed filters (e.g. LAV) |
| Frame → screen | `TransferVideoFrame` straight into the back buffer | NV12: staging texture → `ID3D11VideoProcessor` (converts and scales); RGB32: `StretchDIBits` (fallback) |
| Loop | `SetLoop(TRUE)` | `EC_COMPLETE` → back to 0 |
| Seek | `SetCurrentTimeEx`, approximate while dragging, exact on release | `SetPositions`, `AM_SEEKING_SeekToKeyFrame` while dragging |
| Volume | `SetVolume`/`SetMuted` | `IBasicAudio` (hundredths of a dB) |

### Window

- `WS_POPUP` + `WS_EX_TOOLWINDOW | WS_EX_TOPMOST`: no border, out of the taskbar and of Alt+Tab. Opens without taking the focus (`SW_SHOWNOACTIVATE`), in the bottom right corner.
- **Reinforced topmost:** `SetWinEventHook` (foreground change, end of Alt+Tab, restore) raises the window again, and `WM_WINDOWPOSCHANGING` keeps anything from taking it out of the topmost band.
- `WM_NCHITTEST`: a 6 px band on the edges = resize; visible bar = `HTCLIENT`; the rest of the video = `HTCAPTION` (the system moves the window). Double click on the video (`WM_NCLBUTTONDBLCLK`) = fullscreen.
- `WM_SIZING` keeps the aspect; `resize_anchored` changes the size pinned to the nearest screen corner.
- Opacity only turns on `WS_EX_LAYERED` below 100%; the swap chain uses the blt model (`DXGI_SWAP_EFFECT_DISCARD`) and is GDI compatible (`GetDC` on the back buffer).
- Single instance: `FindWindow` by the `vplay` class + `WM_COPYDATA` with the full path (`GetFullPathNameW`: the other instance has another current directory).

---

## Build

```bat
make bin                REM from the root: every program of bin/
make vplay.exe          REM inside bin/: vplay only
make vplay.exe WERROR=1 REM as in CI (-Werror)
```

A running `vplay.exe` blocks the link (`Permission denied`): close it first (`taskkill /im vplay.exe` sends `WM_CLOSE`; it takes ~0.5 s).

Libraries: `mfplat mfuuid` (MF), `d3d11 dxgi dxguid` (swap chain and video processor), `dwmapi` (`DwmFlush`), `msimg32` (`AlphaBlend` for the bar), `strmiids` (DirectShow), `gdi32 user32 shell32 ole32 oleaut32 uuid`.

---

## Keys

| Action | Keys |
|---|---|
| Play/pause | space, ↑ / ↓, the bar's button |
| Pick a part | click/drag on the bar |
| −10 s / +10 s | ← / → |
| Restart | F12 (focused window only; as a global key it got in the way of browser DevTools) |
| Size | wheel, `+` / `-`, `0` = initial size (1/6 of native) |
| Volume / opacity | Ctrl+wheel / Shift+wheel or PgUp/PgDn |
| Mute | M |
| Fullscreen | Alt+Enter, F, double click; Esc leaves |
| Hide / show (global) | Home / End (Home pauses; End resumes if Home paused it) |
| Quit | Q |

`vplay -h` prints the list in the terminal (the program is `-mwindows`; it writes to the caller's console through `AttachConsole`).

---

## Pitfalls already solved (do not repeat)

- The Media Engine's `GetVideoAspectRatio` returns the **picture** aspect (4:3, 16:9), not the pixel aspect.
- `winbase.h` defines a `GetCurrentTime` macro: `#undef` it before `mfmediaengine.h`.
- `DrawText` treats `&` as a mnemonic prefix: every text uses `DT_NOPREFIX` (file names with `&`).
- DXGI grabs Alt+Enter: `MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES)`.
- `ResizeBuffers` fails while any reference to the back buffer is left: only update the known size when it succeeds (the next pass tries again). That is also why the video processor's output view is created and released every frame.
- `CLSID_NullRenderer` is in `libstrmiids` but not in the mingw headers: declared by hand in `dshow.c`.
- `DeleteMediaType` belongs to the (C++) base classes: `free_media_type` does the same in C.
- The Sample Grabber only understands `VIDEOINFOHEADER`: the pixel aspect never reaches the DirectShow path (only width and height).
- Dragging with `ReleaseCapture` + `SendMessage(WM_NCLBUTTONDOWN, HTCAPTION)` from `WM_MOUSEMOVE` was not reliable; returning `HTCAPTION` from `WM_NCHITTEST` itself works.
- Videos with black borders recorded inside the frame (camera stabilization, for example) are not a bug.

## Tests

There is no automatic suite. Tested with PowerShell scripts (window capture with `CopyFromScreen`, synthetic mouse and keyboard with `mouse_event`/`keybd_event`):
- synthetic keys must stay down for ~150 ms: the 50 ms timer reads `GetAsyncKeyState`, and an instant tap falls between two reads;
- moving the mouse with `SetCursorPos` does not feed the modal move/resize loop properly: use relative moves (`mouse_event(MOUSEEVENTF_MOVE)`);
- the user may be handling the window during a test: synthetic and real mouse input fight, so check before concluding something failed.

Measurements (dual-core laptop, Intel HD 5500): H.264 960x720 through MF ~0.2 core; 1080p AV1 through DirectShow + LAV ~1.0 core; ~90–125 MB of memory.

## Known limits

- The DirectShow path needs the filters installed (LAV, e.g. from K-Lite); without them, AV1 shows "no decoder for this video". Alternatives with no code: the Store's "AV1 Video Extension" (`winget install 9MVZQVXJBQ9V --source msstore`), or downloading in H.264 (`yt-dlp -S vcodec:h264`).
- No subtitles, playlists, audio track selection or settings: out of scope (use mpv for that).
- Topmost does not go over Windows' own UI (Start, Alt+Tab, notifications); only `uiAccess="true"` with a signed executable in `Program Files` would, and that was left out.

## Conventions

Same as AntShell: C99, tabs, `/* */` comments; code, names, comments and user messages in English. No dependencies beyond the CRT and the Windows API.
