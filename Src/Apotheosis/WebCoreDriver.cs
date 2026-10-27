using System;
using System.Runtime.InteropServices;

namespace Apotheosis
{
    /// <summary>
    /// P/Invoke declarations for WebCoreDriver.dll (30 essential C ABI exports).
    /// Build: pwsh -File Src/port/build-driver-dll.ps1 -Arch arm|x64
    ///
    /// Apotheosis 2026-10-03 -- every string parameter is marshalled BY HAND, as UTF-8.
    ///
    /// The declarations used to say CharSet = CharSet.Ansi, which is what a `const char*`
    /// parameter looks like when you read it off the header -- and it is wrong twice over.
    /// The driver parses every one of these with String::fromUTF8, so the bytes must be
    /// UTF-8; and CharSet.Ansi means "convert using the process ANSI code page", not
    /// "pass the bytes through". On this bench the ACP is Windows-1252, so a Cyrillic
    /// title arrived as a row of '?' -- measured, not theorised: history.tsv held correct
    /// UTF-8 (od -c showed D0 AF D0 BD for "Яндекс") and the home page tile for the same
    /// row read "?????? ? ???????? ?????? ? ??????????".
    ///
    /// The fix is not [MarshalAs(UnmanagedType.LPUTF8Str)]: that attribute's behaviour on
    /// the .NET Native toolchain is exactly the kind of thing this project has already been
    /// burned by assuming. A byte[] is blittable and pinned for the duration of the call by
    /// the runtime, with no charset conversion anywhere in the path, so it behaves the same
    /// under CoreCLR and under .NET Native. Each string-taking export therefore has a private
    /// Raw declaration (EntryPoint = the real symbol) and a public wrapper that keeps the
    /// friendly `string` signature its callers already use.
    /// </summary>
    internal static class WebCoreDriver
    {
        private const string Dll = "WebCoreDriver.dll";

        /// <summary>
        /// UTF-8 encode with a terminator. The last byte of a fresh byte[] is already 0, so
        /// only the payload has to be written.
        /// </summary>
        private static byte[] Utf8z(string s)
        {
            if (string.IsNullOrEmpty(s)) return new byte[1];
            int n = System.Text.Encoding.UTF8.GetByteCount(s);
            byte[] b = new byte[n + 1];
            System.Text.Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        // ── TLS / CA certificates ──────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreSetCACertBlob(byte[] data, int len);

        // ── Managed-host support (Apotheosis) ──────────────────────────────
        // These exist because of a P/Invoke's blunt edge on the managed side: a native
        // exception crossing back into .NET Native is turned into FailFast(7) (WER
        // 0x00001007 in SharedLibrary.dll), with no catchable managed exception and no
        // return code. The driver can now set FONTCONFIG_FILE in the native environment
        // (the managed one does not reach it) and catch native faults as a return code.

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSetFontConfigFile")]
        private static extern void WebCoreSetFontConfigFileRaw(byte[] path);

        public static void WebCoreSetFontConfigFile(string path) => WebCoreSetFontConfigFileRaw(Utf8z(path));

        // Apotheosis M1: where the session's cookie jar lives. Without this call the jar is
        // ":memory:" -- the curl port's default path is unwritable in the App Container -- and
        // every login is lost on restart. Must run before the first network use; SetupRuntimeEnv
        // (the engine Loop's first job) is early enough.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSetCookieJarPath")]
        private static extern void WebCoreSetCookieJarPathRaw(byte[] path);

        public static void WebCoreSetCookieJarPath(string path) => WebCoreSetCookieJarPathRaw(Utf8z(path));

        // Apotheosis M2: where localStorage lives. Like the cookie jar above, the
        // path must be set before the first Page is built; SetupRuntimeEnv runs
        // before any navigation. Without it the local namespace stays in memory
        // and every value disappears on restart.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSetLocalStoragePath")]
        private static extern void WebCoreSetLocalStoragePathRaw(byte[] path);

        public static void WebCoreSetLocalStoragePath(string path) => WebCoreSetLocalStoragePathRaw(Utf8z(path));

        // The port's own step-marker channel: name the file it appends one flushed line to per
        // engine step (LocalState\gpu-steps.txt). It is the only reader of numbers the engine
        // computes for itself -- for one example that mattered: WebCoreSetPageZoom logs
        // "(layout %dx%d CSS-ish, viewport %dx%d)", which is the difference between "the page zoom
        // reached the layout" and "the call returned 0 and changed nothing". The native side wrote
        // nowhere until this call, so the markers the port already contained were invisible from
        // the C# line. Exported in WebCoreDriver.exports.def; a name missing from that list fails
        // at runtime with EntryPointNotFoundException, never at build time.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSetGpuInitLogFile")]
        private static extern void WebCoreSetGpuInitLogFileRaw(byte[] path);

        public static void WebCoreSetGpuInitLogFile(string path) => WebCoreSetGpuInitLogFileRaw(Utf8z(path));

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreSetSehGuard(int on);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetSehInfo(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetFontInfo(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreDumpFontConfig();

        // ── One-shot rendering ─────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreRenderHtml")]
        private static extern int WebCoreRenderHtmlRaw(byte[] utf8Html, int width, int height, IntPtr outBuf);

        public static int WebCoreRenderHtml(string utf8Html, int width, int height, IntPtr outBuf)
            => WebCoreRenderHtmlRaw(Utf8z(utf8Html), width, height, outBuf);

        // ── Persistent session lifecycle ───────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSessionLoad")]
        private static extern int WebCoreSessionLoadRaw(byte[] url, int width, int height, IntPtr outBuf);

        public static int WebCoreSessionLoad(string url, int width, int height, IntPtr outBuf)
            => WebCoreSessionLoadRaw(Utf8z(url), width, height, outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreCloseSession();

        // Memory pressure. The callee frees what nothing else in this port can: on Critical::Yes it
        // reaches MemoryCache::pruneLiveResourcesToSize(0, DestroyDecodedDataForAllLiveResources), i.e.
        // the decoded bitmaps. That is the only lever that ever moved this app's slope -- measured
        // 2026-10-03, scrolling dzen.ru drove the working set from 100 to 298 MB in 47 s and the
        // process was reaped with no dump and no dangling marker line, while a naive reading of the
        // resource-cache counters said `decodedB=0` throughout, because the Cairo image path never
        // calls CachedImage::decodedSizeChanged() and so the counters cannot see a decoded bitmap at
        // all. Critical::Yes rather than No: the non-critical branch purges only what is already dead.
        //
        // MUST be called on the engine thread, and never before the port's process init has run:
        // the C++ harness taught both lessons, the second as an intermittent startup death. Use
        // WebEngine.Instance.Post and check _engineReady.
        //
        // Reachable only because it is listed in Src/port/WebCoreDriver.exports.def. A function
        // declared in WebCoreDriver.h and implemented in WebCoreDriver.cpp is NOT visible to this
        // file without that line -- the C++ harness gets away without it by compiling the port into
        // Harness.exe, which is not how this app links.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreReleaseMemory(int critical);

        // ── Interaction ────────────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreClickAt(int x, int y, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreScrollBy(int dx, int dy, IntPtr outBuf);

        // The finger's path: the innermost scrollable element under (x,y) takes the delta, the
        // frame is the fallback. WebCoreScrollBy above moves only the frame's view, so an inner
        // overflow container (a mobile SPA's scroll element, a side panel) never moves.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreScrollByAt(int x, int y, int dx, int dy, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSyncLinks();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreTypeText")]
        private static extern int WebCoreTypeTextRaw(byte[] utf8, IntPtr outBuf);

        public static int WebCoreTypeText(string utf8, IntPtr outBuf) => WebCoreTypeTextRaw(Utf8z(utf8), outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreKeyAction(int action, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFocusedEditable();

        // ── Back/forward history of the live session ───────────────────────
        // A step through the ENGINE's back/forward list -- the one that link clicks, in-page
        // navigations and redirects enter. The app's own _navStack only ever holds URLs the app
        // itself navigated to, so it cannot answer "back to where the user came from".
        // 0 on success; -18 (kErrNoHistory) when there is nothing that way (the current page is
        // left painted and untouched); -12 (kErrNoSession); -13 (kErrBusy).
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGoBack(IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGoForward(IntPtr outBuf);

        // 1 if a step that way is available, else 0. Reads only; does not touch the frame buffer.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreCanGoBack();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreCanGoForward();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSetPageScale(float scale, int focalX, int focalY, IntPtr outBuf);

        // ── Viewport / scale ───────────────────────────────────────────────
        // Two different levers, and they are not interchangeable:
        //
        //   WebCoreSessionResize  moves the session's viewport, so the page RE-LAYS OUT at the new
        //                         size. This is what removes the white fields beside a 720-DIP page
        //                         in a wider window; it is not a scale, it is a new layout.
        //   WebCoreSetPageZoom    CSS page zoom: device px per CSS px. Also re-lays out -- the CSS
        //                         layout width becomes viewport / zoom -- which is how a phone-width
        //                         layout is produced in a wide window without blurring anything.
        //
        // While a session lives, the zoom is remembered engine-side (`g_pageZoom` +
        // apoReassertPageZoom on the present paths), because a document commit can put the frame's
        // factor back. That memory does not survive a navigation: every load builds a fresh Page, so
        // the factor a new document is laid out at is armed separately -- see
        // WebCoreSetNextSessionZoom below.

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSetPageZoom(float zoom);

        // The zoom the NEXT session is BUILT at. WebCoreSetPageZoom needs a live session, so every
        // request it takes lands after the document has been parsed -- on the fixed-viewport line
        // that is OnNavDone, i.e. after the page was laid out at the CSS default. Call this on the
        // engine thread immediately before WebCoreSessionLoad: buildSession applies it before the
        // document is created, so the first layout is already at viewport/zoom and a page that reads
        // window.innerWidth once, at load, sees the zoomed width.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSetNextSessionZoom(float zoom);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreSessionResize(int width, int height, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreLiveTick(IntPtr outBuf);

        // ── Metadata / diagnostics ─────────────────────────────────────────
        // The byte[]-buffer exports read as if they took strings; they do not. The caller
        // passes a buffer and decodes it (UTF-8) itself, so there is nothing to marshal.

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetTitle(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetUrl(byte[] buf, int len);

        // L1 popup half: TAKE the queued popup URL (window.open / target=_blank) parked by the
        // engine funnels; 0 = none, and the slot clears on every take so a popup fires once.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreTakePopupUrl(byte[] buf, int len);

        // M6 kill switch: fire count and engine-heartbeat counter. UI-thread readable by
        // design -- two plain int loads, evaluated as a pair (see MainPage.M6Check).
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetRunawayFires();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetM6Heartbeat();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetLinkCount();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetLink(int i, ref int x, ref int y, ref int w, ref int h, byte[] url, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint WebCoreGetFrameHash();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetPendingResourceCount();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetLastError(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGetDiag(byte[] buf, int len);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreEditDebug(byte[] outBuf, int cap);

        // Live-page JS channel for bench diagnosis (M3/M4). The driver has had the export
        // since the SPA probe, but the .def deliberately omitted it; `js` in autocmd now
        // uses it to read DOM geometry and computed styles that a screenshot cannot state.
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreEvalJS")]
        private static extern int WebCoreEvalJSRaw(byte[] script, byte[] outBuf, int len);

        public static int WebCoreEvalJS(string script, byte[] outBuf)
            => WebCoreEvalJSRaw(Utf8z(script), outBuf, outBuf.Length);

        // ── Find in page ───────────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreFindString")]
        private static extern int WebCoreFindStringRaw(byte[] utf8, int matchCase, int wrap, IntPtr outBuf);

        public static int WebCoreFindString(string utf8, int matchCase, int wrap, IntPtr outBuf)
            => WebCoreFindStringRaw(Utf8z(utf8), matchCase, wrap, outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFindNext(int forward, IntPtr outBuf);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreFindClear(IntPtr outBuf);

        // ── Downloads ──────────────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreDownload")]
        private static extern int WebCoreDownloadRaw(byte[] url, byte[] outPath);

        public static int WebCoreDownload(string url, string outPath)
            => WebCoreDownloadRaw(Utf8z(url), Utf8z(outPath));

        // ── User agent ─────────────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern void WebCoreSetUserAgentMobile(int mobile);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "WebCoreSetUserAgentString")]
        private static extern void WebCoreSetUserAgentStringRaw(byte[] ua);

        // The driver memcpy's the bytes into its own static buffer, so the pin only has to
        // outlive the call -- which is exactly what a byte[] parameter guarantees.
        public static void WebCoreSetUserAgentString(string ua) => WebCoreSetUserAgentStringRaw(Utf8z(ua));

        // ── GPU compositing ────────────────────────────────────────────────

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGpuInit(IntPtr nativeWindow, int w, int h);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreEnableCompositing();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        public static extern int WebCoreGpuLayerInfo(byte[] outBuf, int len);
    }
}
