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

// Apotheosis: set FONTCONFIG_FILE from the native side. A managed host cannot do this
// itself (Environment.SetEnvironmentVariable does not reach the native process
// environment block), and without it the App Container's font stack matches nothing and
// the first paint faults inside cairo_scaled_font_create.
void WebCoreSetFontConfigFile(const char* path);

// Apotheosis M1: point the process-default cookie jar at a file (UTF-8 path). Without it the
// session keeps the crash-free but ephemeral ":memory:" jar, and every login dies with the
// process. Call before the first network use (SetupRuntimeEnv does).
void WebCoreSetCookieJarPath(const char* path);

// Apotheosis M2: point localStorage at a SQLite database (UTF-8 path). Call before the
// first Page is built; SetupRuntimeEnv does. Without it the local namespace remains the
// old process-scoped map, and a page's values disappear on the next launch.
void WebCoreSetLocalStoragePath(const char* path);

// Apotheosis: opt in to the SEH guard around the engine entry points. With it on, a native
// fault becomes -17 (kErrSeh) plus a WebCoreGetSehInfo line instead of .NET Native's
// FailFast(7), which kills a managed host with no catchable exception. Off by default so
// the C++/CX harness keeps its own UEF filter and wedge dumps.
void WebCoreSetSehGuard(int on);

// Last native fault caught by the guard: "seh code=0xC0000005 at WebCoreDriver.dll+0x1234 guard=1".
// Empty when nothing was caught. Returns bytes written.
int WebCoreGetSehInfo(char* buf, int len);

// Last font probe summary (FONTCONFIG_FILE as the native side sees it, the <dir>/<cachedir>
// entries, per-file size + sfnt magic, FT_New_Face results, and on arches with fontconfig
// linked the font count and what "sans-serif" resolves to). Returns bytes written.
int WebCoreGetFontInfo(char* buf, int len);

// Re-run the font probe on demand; it also runs once automatically on the first render.
void WebCoreDumpFontConfig();

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

// L1 popup half (2026-10-26): popups are opened IN THE CURRENT WINDOW on this single-window
// browser. The two engine funnels that used to drop window.open()/target=_blank silently
// (PortChromeClient::createWindow, LoadingFrameLoaderClient's new-window policy) instead park
// the requested URL here and log a `POPUP:` marker; the shell polls this from the live tick
// (same engine thread as the writers) and feeds the URL into its normal NavigateTo/queue path.
// TAKE semantics: copies the URL once and clears the slot, so a popup cannot fire twice.
// Returns bytes copied (0 = nothing queued), which also clears buf.
int WebCoreTakePopupUrl(char* buf, int len);

// M6: watchdog fire count and engine-call heartbeat (%50, %51). Read from the shell's UI
// timer; fires across a FROZEN heartbeat = an unrecoverable runaway -> the M6 kill switch.
int WebCoreGetRunawayFires();
int WebCoreGetM6Heartbeat();

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
// Apotheosis 2026-10-20: a finger's scroll WITH its point. The innermost scrollable element under
// (x,y) owns the delta and the main frame is the fallback, which is what a browser does with a
// wheel event. (x,y) are engine-surface px -- the same space WebCoreClickAt takes. WebCoreScrollBy
// moves only the frame's LocalFrameView, so a page whose scrolling lives in an inner overflow
// container does not move at all; measured on the Lumia against nokiadesignarchive.aalto.fi, whose
// mobile UI is a `height:100vh; overflow-y:auto` element inside a frame whose contents equal the
// viewport. Paints into outBuf exactly like WebCoreScrollBy.
int WebCoreScrollByAt(int x, int y, int dx, int dy, uint8_t* outBuf);
int WebCoreSyncLinks();                                // refresh link hit-table after scroll settles (layout+extract, no paint)
int WebCoreEditDebug(char* out, int cap);              // diag: last type-text canEdit/focus/insert state
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outBuf); // M4 pinch zoom (clamped [0.5,6.0])
int WebCoreGetPageScale();                             // M4: current pageScaleFactor ×1000 (1000 = 1.0x)
// Apotheosis 2026-09-19: CSS page zoom (device px per CSS px), NOT pinch scale. Re-lays out, so the
// layout viewport becomes viewport/zoom CSS px -- the GPU path renders at the panel's physical size
// and needs zoom = CompositionScale to keep the layout at DIP width. Does NOT repaint; the caller
// must (the harness sets it immediately before a resize). See the definition for the measurements.
int WebCoreSetPageZoom(float zoom);
// Apotheosis 2026-10-21: the page zoom the NEXT session is BUILT at, applied in buildSession before
// the document is created, so the first layout is already at viewport/zoom. WebCoreSetPageZoom needs
// a live session and therefore lands after the document has parsed -- too late for a page that reads
// window.innerWidth once, at load. Call on the engine thread just before WebCoreSessionLoad. Does not
// touch the live session and does not repaint. 1.0 (the default) means "CSS default".
int WebCoreSetNextSessionZoom(float zoom);
int WebCoreSessionPaint(uint8_t* outBuf);             // no interaction, redraw only
// Resize the live session's viewport to w x h and repaint at the new size (relayout + link
// hit-table refresh included). outBuf must be at least w*h*4 bytes; every later paint call
// writes w*h*4 until the next resize. 0 on success.
int WebCoreSessionResize(int w, int h, uint8_t* outBuf);

// ---- IME / keyboard ----
int WebCoreFocusedEditable();                          // 1 if an editable element is focused (input/textarea/contenteditable)
int WebCoreTypeText(const char* utf8, uint8_t* outBuf); // insert UTF-8 into focused editable; 0 on success
int WebCoreKeyAction(int action, uint8_t* outBuf);     // 0=Backspace, 1=Enter (may submit form); 0 on success

// ---- back/forward history of the live session ----
// One step through the ENGINE's history list, then repaint the destination. This is deliberately not
// the host's own URL stack: it follows link clicks, in-page navigations and redirects the host never
// initiated, which is exactly what "back returns to the previous page" means. BackForwardCache is
// disabled process-wide, so an entry reloads rather than restoring a snapshot.
// Returns 0 on success; kErrNoHistory (-18) when there is nothing that way, in which case the
// current page is left painted and untouched; kErrNoSession (-12); kErrBusy (-13).
int WebCoreGoBack(uint8_t* outBuf);
int WebCoreGoForward(uint8_t* outBuf);
// 1 if a step that way is available, else 0. Reads only -- safe between interactions, does not repaint.
int WebCoreCanGoBack();
int WebCoreCanGoForward();

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
