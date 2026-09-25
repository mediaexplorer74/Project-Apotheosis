// ============================================================================
// WebCoreDriver.h  — C ABI for the WebCore headless software-render driver.
//
// This is the canonical C-ABI boundary between the UWP Harness (C++/CX, MSVC)
// and the engine (Src/port, clang-cl → WebCoreDriver-gpu.lib). It must stay in
// sync with Src/harness/WebCoreDriver.h — WebKit has two copies, one per side,
// and any change to an export must be mirrored in both or the ABI breaks.
//
// Most calls render into a caller-provided w*h RGBA8888 buffer (>= w*h*4 bytes)
// and return 0 on success, negative on failure.
//
// Error codes (negative returns):
//   -1  bad args            -7  cairo surface create failed
//   -2  page create failed  -8  cairo context create failed
//   -3  no main frame       -9  bad/invalid URL          (WebCoreLoadUrl)
//   -4  no view            -10  load failed              (WebCoreLoadUrl)
//   -5  no document loader -11  load timed out (watchdog)(WebCoreLoadUrl)
//   -6  no document
//
// All interactive-session calls must be serialized on the single engine thread.
// ============================================================================

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Render a fragment of UTF-8 HTML into a width×height pixel buffer (RGBA8888, opaque white background).
int WebCoreRenderHtml(const char* utf8Html, int width, int height, uint8_t* outBuf);

// Verification: only init + Page::create + solid fill, verifies C ABI + display pipeline (minimal engine risk).
int WebCoreRenderHtmlStub(const char* utf8Html, int width, int height, uint8_t* outBuf);

// Phase 1b networking: load a real URL (curl + OpenSSL TLS 1.3) and render. (-9/-10/-11 errors, else same as render.)
int WebCoreLoadUrl(const char* url, int width, int height, uint8_t* outBuf);

// Inject CA root certificate bundle (PEM) for curl/OpenSSL. App Container has no system trust store; call once
// before the first WebCoreLoadUrl(). path is the UTF-8 path to cacert.pem.
void WebCoreSetCACertPath(const char* path);

// Inject CA root certificate using an in-memory PEM blob (CURLOPT_CAINFO_BLOB). App Container blocks OpenSSL
// file-based CA loading (curl 77), so on device use the blob. Must be called before the first WebCoreLoadUrl.
void WebCoreSetCACertBlob(const uint8_t* data, int len);
// Content settings read at session build time (buildSession). 1 = on, 0 = off, negative =
// "not configured yet", which the session treats as ON -- that is the browser default, so the
// first navigation is right even when the harness's queued apply-settings job has not run yet.
void WebCoreConfigure(int jsEnabled, int imagesEnabled);

// Retrieve the network error recorded on the last WebCoreLoadUrl failure (curl code + description + URL).
// Writes into buf (at most len bytes incl. NUL), returns bytes written (excl. NUL). Empty string if no error.
int WebCoreGetLastError(char* buf, int len);

// Retrieves render diagnostics from the last WebCoreLoadUrl (final URL/title/content size/non-white pixel count).
int WebCoreGetDiag(char* buf, int len);

// Title (UTF-8) of the most recently loaded page, for history/bookmark display. Returns bytes written.
int WebCoreGetTitle(char* buf, int len);

// Final URL (UTF-8) of the most recently rendered document. After click navigation within a session, use this
// to detect URL changes for the address bar and forward/back history stack. Returns bytes written.
int WebCoreGetUrl(char* buf, int len);

// Directly download url to outPath (standalone curl, no render, reuses CA blob). Returns HTTP status (e.g. 200)
// on success, negative on failure. curl global init must have occurred first.
int WebCoreDownload(const char* url, const char* outPath);

// Current page link hit table (extracted during render): count + get rectangle (bitmap coords) and URL of i-th link.
int WebCoreGetLinkCount();
int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len);

// ---- persistent interactive session (live Page + event forwarding) ----
// Load a URL into a persistent session (replaces WebCoreLoadUrl for pages needing interaction). 0 on success.
int WebCoreSessionLoad(const char* url, int width, int height, uint8_t* outBuf);
void WebCoreCloseSession();
int WebCoreClickAt(int x, int y, uint8_t* outBuf);     // (x,y) = bitmap/viewport px; hit test + default action
// Apotheosis: the page's own verdict on the tap WebCoreClickAt just dispatched, as the `click` event's
// defaultPrevented read *after* the dispatch settled. 1 = the page called preventDefault(), i.e. it
// refused the default action; 0 = the click happened and nothing refused it; -1 = no `click` event was
// recorded at all. Read it on the same thread, immediately after WebCoreClickAt; the next click resets
// it. The harness needs it because "no navigation started && nothing repainted" does NOT mean the page
// ignored the tap -- a page that preventDefaults and updates asynchronously looks identical, and the
// frame-hash heuristic navigated such a tap against the page's explicit wish. See Doc/TAP-DISPATCH.md.
int WebCoreLastClickDefaultPrevented();
int WebCoreScrollBy(int dx, int dy, uint8_t* outBuf);  // dx>0 right, dy>0 down; triggers lazy image load
int WebCoreSyncLinks();                                // refresh link hit-table after scroll settles (layout+extract, no paint)
int WebCoreEditDebug(char* out, int cap);              // diag: last type-text canEdit/focus/insert state
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outBuf); // M4 pinch zoom (clamped [0.5,6.0])
int WebCoreGetPageScale();                             // M4: current pageScaleFactor ×1000 (1000 = 1.0x)
// Apotheosis 2026-09-19: CSS page zoom (device px per CSS px), NOT pinch scale. Re-lays out, so the
// layout viewport becomes viewport/zoom CSS px -- the GPU path renders at the panel's physical size
// and needs zoom = CompositionScale to keep the layout at DIP width. Does NOT repaint; the caller
// must (the harness sets it immediately before a resize). See the definition for the measurements.
int WebCoreSetPageZoom(float zoom);
int WebCoreSessionPaint(uint8_t* outBuf);             // no interaction, redraw only
// Resize the live session's viewport to w x h and repaint at the new size (relayout + link
// hit-table refresh included). outBuf must be at least w*h*4 bytes; every later paint call
// writes w*h*4 until the next resize. 0 on success.
int WebCoreSessionResize(int w, int h, uint8_t* outBuf);

// ---- IME / keyboard ----
int WebCoreFocusedEditable();                          // 1 if an editable element is focused (input/textarea/contenteditable)
int WebCoreTypeText(const char* utf8, uint8_t* outBuf); // insert UTF-8 into focused editable; 0 on success
int WebCoreKeyAction(int action, uint8_t* outBuf);     // 0=Backspace, 1=Enter (may submit form); 0 on success

// User agent switch: mobile=1 mobile iPhone UA (default), 0 desktop Edge UA. Requires reload.
void WebCoreSetUserAgentMobile(int mobile);
// Custom UA override (non-empty wins over mobile/desktop; empty string clears → reverts). Requires reload.
void WebCoreSetUserAgentString(const char* ua);

// Whether GPU compositing is live (root GraphicsLayer attached). Check after load; returns 1/0.
int WebCoreEnableCompositing();

// M2 GPU compositing presentation (engine thread). nativeWindow = IInspectable* of SwapChainPanel's PropertySet
// (direct rendering); nullptr = off-screen (readback only). Enables compositing for network sessions on success.
int WebCoreGpuInit(void* nativeWindow, int w, int h);
// Apotheosis: resize a live GPU session's render surface + viewport to w x h and repaint.
// Recreates the ANGLE window surface at the new size (engine thread; ANGLE marshals to the
// panel dispatcher, same as GpuInit). nativeWindow = NEW IInspectable* of a PropertySet with
// EGLNativeWindowTypeProperty (same panel) + EGLRenderSurfaceSizeProperty = (w,h). The caller
// must keep that PropertySet alive (harness stores it in m_gpuProps). 0 on success.
int WebCoreGpuResize(void* nativeWindow, int w, int h, uint8_t* outBuf);
int WebCoreComposite();                                 // direct present (eglSwapBuffers); only after GpuInit(nativeWindow!=null)
int WebCoreCompositeReadback(uint8_t* outBuf);        // off-screen composite + readback RGBA
void WebCoreGpuSetFlip(int flipH, int flipV);         // set readback flip; applies on next frame
// Tell the driver whether the host is showing the GPU swapchain surface RIGHT NOW: 1 after switching
// to it, 0 the moment the host reverts to the software bitmap. Defaults to 0.
//
// Without this the driver only knows that a swapchain is *possible* (a native window was passed to
// WebCoreGpuInit) and takes the direct-present branch regardless -- returning success while leaving
// the caller's RGBA buffer untouched. If the host has meanwhile hidden the panel, the frame is
// presented into a surface nobody sees and the visible bitmap stays blank: a fully loaded page in a
// white window, with no error anywhere. Measured on x64 on 2026-08-29; see
// Doc/PUMPLOOP-SILENT-DEATH.md. With 0 the driver composites through TextureMapper and reads the
// result back into the buffer instead, so layered content still reaches a software blit.
void WebCoreSetDirectPresent(int on);
int WebCoreGpuLayerInfo(char* outBuf, int len);       // FrameView scroll/content + compositing layer tree text
// Apotheosis: set a file path the driver appends per-step GPU init markers to (crash pinpointing).
// Pass a writable path (harness LocalState). Empty clears. Only affects WebCoreGpuInit.
void WebCoreSetGpuInitLogFile(const char* path);

// Execute JS in the main world of the current session, convert result to string and write to out. 0 on success.
int WebCoreEvalJS(const char* script, char* out, int len);

// Live tick: advance animation/rAF/SPA one frame and redraw (low-fps timer keeps animations/SPA mounting).
int WebCoreLiveTick(uint8_t* outBuf);
// Number of cached resources still Pending/Unknown for the current document (keep ticking while incomplete).
int WebCoreGetPendingResourceCount();
// Which step of WebCoreLiveTick is executing right now: 0 = not in a tick, 10 = returned,
// 1..3 = RunLoop::cycle (queued loader callbacks land here), 4 = rAF/rendering update,
// 5 = frame/view/document lookups, 6 = microtasks, 7 = layout, 8 = pending-resource count, 9 = paint.
//
// Read from the UI thread and printed in the heartbeat line. The engine thread has been observed to
// hang inside this job on the device (heartbeat: busy=1 job=live-tick), and this names the operation
// that never returns. A marker file was rejected deliberately: the tick runs several times a second,
// and per-step file I/O on the device's flash would alter the timing it is meant to measure. See
// Doc/PUMPLOOP-SILENT-DEATH.md.
int WebCoreGetLiveTickStep(void);
// Frame hash of the most recent frame: in live mode compare consecutive frames; stop ticking when static.
unsigned WebCoreGetFrameHash();
// Top-level document fetch progress, or -1 when no top-level fetch is in flight. **The sign is the
// contract**: -1 and only -1 means "no such fetch, the engine thread is free"; any non-negative value
// means one IS in flight, in which case the engine thread is *supposed* to be blocked and the caller
// must not judge a stalled counter as a wedge on the same timescale. The pre-body phase (connect, TLS,
// TTFB, a redirect chain) reports a legitimate 0 -- it is not a movement claim.
// The value is NOT a byte count: it is body bytes with the number of completed response hops folded in
// above them, because a redirect chain reports 0 bytes through its whole life while working perfectly.
// The caller compares it only against its own previous reading and prints it as `fetchprog=`: it lets
// the wedge watchdog tell "the engine is blocked on a slow but healthy fetch" from "the engine made no
// progress at all" without hardcoding a duration.
// Both counters start at 0 and are reset to 0 per fetch -- never to -1 -- because the fold
// `dl + (hops << 40)` turns a -1 sentinel into a large negative number, and the watchdog reads that as
// "no fetch in flight" during a fetch. That inversion cost one false WEDGE dump (2026-09-19 04:35).
// See the re-arming fetch budget in Doc/TOPLEVEL-FETCH-BUDGET.md, §9.
long long WebCoreGetFetchProgress(void);
// Engine liveness gauge: a monotonic count of the work the engine thread has visibly performed -- the
// load job's stage boundaries, every DocumentWriter chunk, every settle tick of the load pump, and the
// live tick's step index. Never negative, and meaningful only as a CHANGE against the caller's own
// previous reading, exactly like WebCoreGetFetchProgress and used the same way by the wedge watchdog.
// It exists because `finished` (completed jobs) is too coarse to answer the question: a single
// `nav-load` job holds the engine thread for ~7 s on a 3.6 MB page (parse + inline JS + first paint),
// during which `finished` cannot move at all, and a watchdog reading only `finished` dumped a full
// engine stack for two such pages that loaded successfully two seconds later (2026-09-19, 04:35 and
// 04:50). A frozen gauge means the engine made no visible progress -- it does NOT mean the engine is
// idle (a cold engine with no session yet also reads frozen).
long long WebCoreGetEngineActivity(void);

// ---- find-in-page ----
// Mark + highlight all matches, select the first, scroll to it. matchCase!=0 case-sensitive, wrap!=0 wrap.
// Empty string = clear highlights. Returns match count (>=0) or negative error.
int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outBuf);
int WebCoreFindNext(int forward, uint8_t* outBuf);   // 1=found / 0=none / neg=error (reuses last term, no re-mark)
int WebCoreFindClear(uint8_t* outBuf);               // clear highlights + selection; 0 on success

// Memory pressure: 1=critical, 0=gentle. Flushes caches, back-forward cache, JSC GC, font cache.
void WebCoreReleaseMemory(int critical);

#ifdef __cplusplus
} // extern "C"
#endif