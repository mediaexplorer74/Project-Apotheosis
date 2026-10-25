using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.Security.Cryptography;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.System.Profile;
using Windows.UI;
using Windows.UI.Core;
using Windows.UI.Popups;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;
using System.Runtime.InteropServices.WindowsRuntime;

namespace Apotheosis
{
    public sealed partial class MainPage : Page
    {
        // The engine viewport, in device pixels.
        //
        // These two numbers are the ONE size shared by the engine session, the GPU surface and the
        // WriteableBitmap: the session is created at kW x kH, the GPU texture is allocated at
        // kW x kH, and the RGBA buffer every present path blits is kW * kH * 4 bytes. A change here
        // is not a layout tweak -- it is a change to the present path, including its stride, its
        // `contents=` accounting and the GPU DIP/device-pixel mapping.
        //
        // Apotheosis 2026-10-03 (second pass): no longer `const`. With `fitwindow=1` in settings.ini
        // a SizeChanged on ContentArea rewrites them to the window's own size, which is what removes
        // the white fields: measured on this bench, a 1368-DIP-wide content area showed a 720-DIP
        // page and 647 DIP of white beside it. They are fields rather than a second "live viewport"
        // pair because every existing use site already means "the current viewport", and a parallel
        // pair would be a second truth to keep in step.
        //
        // With the setting OFF nothing writes them and the port behaves exactly as it did. That is
        // still the desktop default; on Windows.Mobile the setting is ON by default since
        // 2026-10-25, because a fixed 720-px frame is a Lumia 950-shaped canvas and halves every
        // glyph on the 360-DIP Lumia 640. See the field comment at `_fitWindow`. The measurement
        // that made the fitted path safe to try is in Doc/Archive/Plan_MVP.md §0 item 4.
        static int kW = 720, kH = 1080;
        // S7 (2026-10-22): ContentArea DIP per frame pixel -- the display scale of the fixed
        // frame (FrameFitScale). 1.0 whenever the frame is drawn 1:1: the fitted viewport
        // (fitwindow=1, area == frame) always computes 1.0, which is why the bench probes do
        // not see a difference. Maintained by ApplyDisplayScale(); the input paths read it
        // only through that function so the number they divide by is the number on screen.
        double _fitScale = 1.0;

        // The window-fitted viewport. See ApplyViewport().
        //
        // The geometry is one sentence: the CSS viewport is the content area divided by the page
        // zoom, so at the default 100% one CSS px is one DIP and the layout width is the window's
        // own width. The user agent does NOT enter into it -- mobile and desktop differ by the UA
        // header and by nothing else, which is also what the switch means on a phone.
        //
        // Apotheosis 2026-10-03: until today `mobile` ALSO re-laid the page out at a fixed phone
        // CSS width (412) and magnified it by `window / 412`, capped at 3.5, to keep a phone-shaped
        // "морда" in a window that is not phone-shaped. Measured on the bench (1355x751 DIP,
        // CompositionScale 2.00, `settings.ini ua=1`): `fitwindow: resize rc=0 zoom rc=0
        // 1355x751 z=3,29`, and because the frame is drawn 1:1 in DIP, 16 CSS px of text came out
        // 53 DIP = 106 physical px -- 3.3x the desktop render of the same page, which the user read
        // as "масштаб конский" the moment dzen.ru rendered. Two numbers decide the result and they
        // multiply: this zoom and the page-zoom slider in Settings (50..200 %); at 200 % and 3.29
        // the page was painted at 6.58 engine px per CSS px, 13 physical px.
        //
        // The magnification bought nothing. `zoom = width / cssWidth` fills the window for ANY
        // cssWidth, so a narrow layout is not what fills it -- legibility is what the magnification
        // costs. A narrow "морда" on a wide screen is therefore an explicit choice of the page-zoom
        // setting, not a side effect of the UA switch.
        //
        // One rule is enough for every device in the target set, and DIP is the reason: the Lumia
        // 950 (CompositionScale 2.5, 576x1024 DIP) and the 640 (2.0, 360x640 DIP) differ 4x in
        // pixels, and at the same zoom they render text at the SAME physical size, because that is
        // what a DIP is. The 640 sees less of the page, which is what a smaller screen is. The
        // 1280x800 Windows tablet at 100% (1280 DIP) and the old hybrid laptop (1366 DIP) are the
        // same rule with more room. The whole set spans 360..1366 DIP wide, 3.8x, and one number
        // covers it.
        //
        // Above this the RGBA buffer (w * h * 4, contiguous) stops being cheap: 2048 x 2048 is
        // 16 MB. A 4K window at 100% would ask for 3840 and is left with the old fixed viewport
        // rather than being silently served a 60 MB allocation.
        const int kViewportCeiling = 2048;

        // The page-zoom range, as fractions. One pair for every lever that sets a zoom -- the
        // Settings slider, ApplySettings' clamp, a pinch's preview clamp and its commit clamp -- so
        // that a gesture can never preview a factor the release would refuse. These are the bounds
        // of the setting, not of the engine: WebCoreSetPageZoom itself accepts 0.1..8.0.
        const float kZoomMin = 0.5f;    // layout twice as wide as the window
        const float kZoomMax = 2.0f;    // layout at half the window width

        // --- window-fitted viewport state ---
        bool _vpDirty;            // a size arrived while the engine was busy; adopt it when free
        int _vpW, _vpH;           // the wanted size, in DIP (software present draws 1:1)
        float _vpZoom = 1f;
        float _pageZoom = 1f;     // what the engine was last told

        static readonly string[] kSearchPrefixes = {
            "https://cn.bing.com/search?q=",
            "https://www.google.com/search?q=",
            "https://duckduckgo.com/?q=",
            "https://www.baidu.com/s?wd="
        };

        // CA PEM handed to WebCoreSetCACertBlob; see SetupRuntimeEnv for why it is retained.
        static byte[] _caBlob;

        // --- Engine thread singleton ---
        sealed class WebEngine
        {
            public static readonly WebEngine Instance = new WebEngine();
            readonly BlockingCollection<Action> _queue = new BlockingCollection<Action>(new ConcurrentQueue<Action>());

            WebEngine()
            {
                Task.Factory.StartNew(Loop, TaskCreationOptions.LongRunning); // .NET Native has no System.Threading.Thread (ILT0003)

            }

            void Loop()
            {
                SetupRuntimeEnv();
                foreach (var job in _queue.GetConsumingEnumerable())
                {
                    try { job(); } catch { }
                }
            }

            public void Post(Action job) => _queue.Add(job);
        }

        // Apotheosis 2026-09-27: append-only breadcrumbs for SetupRuntimeEnv. The C++ harness had a
        // full log here; this line ships without one, and a swallowed exception inside that method is
        // invisible from the outside -- it surfaces much later as a page that will not load, or as
        // missing text, and both look like engine faults. WriteStage() overwrites stage.txt by
        // design, so it cannot answer "how far did setup actually get".
        static void Crumb(string text)
        {
            if (!_traceOn) return;
            try
            {
                string dir = ApplicationData.Current.LocalFolder.Path;
                File.AppendAllText(Path.Combine(dir, "setup-trace.txt"),
                    DateTime.Now.ToString("HH:mm:ss.fff") + "  " + text + Environment.NewLine);
            }
            catch { }
        }

        static void SetupRuntimeEnv()
        {
            try
            {
                WriteStage("env-start");
                string installDir = Package.Current.InstalledLocation.Path;
                string localDir = ApplicationData.Current.LocalFolder.Path;
                if (string.IsNullOrEmpty(installDir) || string.IsNullOrEmpty(localDir)) return;

                string fontsDir = Path.Combine(installDir, "Assets", "fonts");
                string cacheDir = Path.Combine(localDir, "fontconfig-cache");
                // Apotheosis: the probe reported cachedirWritable=no because the directory was
                // named in fonts.conf but never created. fontconfig falls back to an in-memory
                // cache when it cannot write one, but creating it up front keeps the on-disk
                // cache path (and its timing) the same as the C++ harness line.
                Directory.CreateDirectory(cacheDir);
                string confPath = Path.Combine(localDir, "fonts.conf");
                File.WriteAllText(confPath,
                    "<?xml version=\"1.0\"?>\n<fontconfig>\n" +
                    $"  <dir>{fontsDir}</dir>\n" +
                    $"  <cachedir>{cacheDir}</cachedir>\n" +
                    "  <match target=\"pattern\"><test name=\"family\"><string>sans-serif</string></test>" +
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Segoe UI</string><string>Arial</string><string>SimHei</string></edit></match>\n" +
                    "  <match target=\"pattern\"><test name=\"family\"><string>serif</string></test>" +
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Times New Roman</string><string>SimHei</string></edit></match>\n" +
                    "  <match target=\"pattern\"><test name=\"family\"><string>monospace</string></test>" +
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Courier New</string><string>SimHei</string></edit></match>\n" +
                    "  <match target=\"pattern\"><edit name=\"family\" mode=\"append\" binding=\"weak\"><string>Segoe UI</string><string>SimHei</string></edit></match>\n" +
                    "</fontconfig>\n");
                // Apotheosis: Environment.SetEnvironmentVariable does not reach the native process
                // environment block in this App Container, so the driver never saw its own
                // fonts.conf -- and the first paint faulted in cairo_scaled_font_create with
                // fontconfig having matched nothing. Set it natively instead, like the C++ harness.
                Crumb("fontconf:begin");
                try { WebCoreDriver.WebCoreSetFontConfigFile(confPath); Crumb("fontconf:ok"); }
                catch { Environment.SetEnvironmentVariable("FONTCONFIG_FILE", confPath); }

                // Apotheosis 2026-09-27: WebCoreSetFontConfigFile above also adopts APOTHEOSIS_FONTS_DIR,
                // by reading the <dir> out of the fonts.conf it was just handed. That variable is the
                // one FontPlatformData.cpp's apotheosisBundledFontFaceForName() checks before it will
                // load a bundled TTF through FreeType into a cairo FT face, and it is why NOTHING used
                // to render text: unset, the bundled face is skipped, cairo falls back to its toy
                // face, which resolves through fontconfig -- not deployed -- and
                // cairo_scaled_font_create returns an in-error scaled font (CAIRO_STATUS_NO_MEMORY)
                // on which every glyph operation is a silent no-op. Layout, colour and paint all work,
                // and not one glyph appears, on any page, ever.
                //
                // A separate WebCoreSetFontsDir P/Invoke was tried first and cannot work from here:
                // .NET Native rejects a newly added import with "method 'WebCoreSetFontsDir' has no
                // implementation (no RVA)" -- a TypeLoadException on the first call. Letting the one
                // call that already works carry both settings also keeps a single source of truth, so
                // the two paths can never disagree about where the fonts are.
                Crumb("fontsdir:delegated-to-fontconf");

                // Apotheosis 2026-10-03: turn on the port's own step-marker file. Until this call
                // the C++ side had a whole diagnostic channel -- gpuLogMarker, one flushed line per
                // step, written so that a step which never finishes still leaves its own line --
                // and its path was an empty static char[1024], so every marker went to
                // OutputDebugString and nothing else. The markers that were invisible from here
                // include the measurement this project spent a day unable to take:
                // WebCoreSetPageZoom logs "(layout %dx%d CSS-ish, viewport %dx%d)", i.e. whether
                // the page zoom reached the layout or the call returned 0 having changed nothing.
                // The file is appended to, never rotated, and only the resize path turns on the
                // per-frame markers (g_stageTrace), so the cost is one line per event.
                try { WebCoreDriver.WebCoreSetGpuInitLogFile(Path.Combine(localDir, "gpu-steps.txt")); }
                catch (Exception ex) { Crumb("gpusteps: " + ex.GetType().Name); }
                // Opt in to the driver's SEH guard before any engine call: a native fault then
                // returns rc = -17 (kErrSeh) plus WebCoreGetSehInfo instead of killing the process
                // with a FailFast the managed side cannot catch.
                try { WebCoreDriver.WebCoreSetSehGuard(1); } catch { }
                byte[] fInfo = new byte[1024];
                try
                {
                    WebCoreDriver.WebCoreDumpFontConfig();
                    WebCoreDriver.WebCoreGetFontInfo(fInfo, fInfo.Length);
                }
                catch { }
                WriteStage("env-fonts-ok " + Encoding.UTF8.GetString(fInfo).TrimEnd('\0'));

                // M1: the cookie jar the session will open. LocalState is writable from the App
                // Container, unlike the curl port's own default under localUserSpecificStorageDirectory,
                // and the session is created on the first navigation -- well after this job.
                try
                {
                    WebCoreDriver.WebCoreSetCookieJarPath(Path.Combine(localDir, "cookie.jar.db"));
                    WriteStage("env-cookiejar-ok");
                }
                catch (Exception ex) { WriteStage("env-cookiejar: " + ex.GetType().Name); }

                // M2: localStorage gets its own SQLite file in the same writable directory.
                // The local namespace is created on the first document, so this call has to
                // land before the first navigation -- SetupRuntimeEnv is that place.
                try
                {
                    WebCoreDriver.WebCoreSetLocalStoragePath(Path.Combine(localDir, "localstorage.db"));
                    WriteStage("env-localstorage-ok");
                }
                catch (Exception ex) { WriteStage("env-localstorage: " + ex.GetType().Name); }

                string caPath = Path.Combine(installDir, "Assets", "cacert.pem");
                if (File.Exists(caPath))
                {
                    byte[] caBytes = File.ReadAllBytes(caPath);
                    // WebCoreSetCACertBlob publishes this buffer to curl as a raw pointer
                    // (CURL_BLOB_NOCOPY), so it must stay alive for the whole process: a
                    // managed byte[] would be collected and OpenSSL would then parse freed
                    // memory. The C++ harness gets the same guarantee from a leaked vector.
                    _caBlob = caBytes;
                    WriteStage("env-ca-calling len=" + caBytes.Length);
                    WebCoreDriver.WebCoreSetCACertBlob(caBytes, caBytes.Length);
                    WriteStage("env-ca-done");
                }
                WriteStage("env-done");
                // SetupRuntimeEnv is the engine Loop's FIRST job, so everything that must come after
                // process init is ordered by this flag rather than by hope: WebCoreReleaseMemory ahead
                // of init is the intermittent startup death the harness recorded, and the
                // memory-pressure handler is registered from the UI thread, which can run while this
                // method is still going.
                _engineReady = true;
            }
            catch { }
        }

        // rc -17 is kErrSeh: the driver's guard caught a native fault before it could reach the
        // CLR. Without the guard the same fault is what .NET Native turns into FailFast(7), so
        // these two lines are the only place the managed side can learn what killed the engine.
        static string EngineFaultInfo()
        {
            try
            {
                byte[] s = new byte[512];
                WebCoreDriver.WebCoreGetSehInfo(s, s.Length);
                byte[] f = new byte[1024];
                WebCoreDriver.WebCoreGetFontInfo(f, f.Length);
                return "\nSEH: " + Encoding.UTF8.GetString(s).TrimEnd('\0')
                     + "\nfonts: " + Encoding.UTF8.GetString(f).TrimEnd('\0');
            }
            catch { return ""; }
        }

        static void WriteStage(string stage)
        {
            try
            {
                string d = ApplicationData.Current.LocalFolder.Path;
                File.WriteAllText(Path.Combine(d, "stage.txt"), stage + "\n");
            }
            catch { }
        }

        // --- Navigation state ---
        List<string> _navStack = new List<string>();
        int _navIndex = -1;
        // The ENGINE's own back/forward answer, cached from the engine thread so the UI thread can
        // enable the toolbar without a P/Invoke (every engine call must run on the engine thread).
        // Plain ints on purpose: written where the engine calls happen, read in UpdateNavButtons.
        // They are why Back can be enabled for a link click the host never saw: `_navIndex > 0`
        // alone asks only whether the APP navigated, which is not the same question.
        int _engineBack;
        int _engineFwd;
        bool _loading;
        bool _interacting;
        long _opSeq;

        // --- Data ---
        List<Entry> _bookmarks = new List<Entry>();
        List<Entry> _historyList = new List<Entry>();
        List<Entry> _downloads = new List<Entry>();
        DrawerTab _tab = DrawerTab.Favorites;

        string _currentUrl = "";
        // S2 (2026-10-21): a navigation that arrived while a load owned the shell is HELD here
        // instead of being dropped (the old `nav DROPPED, shell busy` line). Last one wins -- a
        // user who asks twice while the page hangs means the second ask. Delivered from
        // SetLoading(false), the single point every load ends at.
        string _pendingNav;
        bool _pendingNavPush;
        string _currentTitle = "";
        List<PageLink> _pageLinks = new List<PageLink>();
        bool _sessionActive;
        bool _uaMobile = true;
        bool _urlSyncing;
        bool _urlFocused;
        bool _updateChecking;
        bool _updateAutoChecked;

        // Settings
        int _setSearch;
        // The UA default follows the device: a desktop window is a desktop browser,
        // a Windows.Mobile phone is a mobile one. The old default was the iPhone UA
        // everywhere; on the x64 bench that made dzen.ru serve desktop HTML to a
        // "phone" and cover the page with its browser-switch-trap. `ua_migrated`
        // below replaces that old automatic value once, without touching an
        // explicit choice the user made later.
        bool _setUaDesktop = !IsMobileDevice();
        int _defaultZoom = 100;
        int _tabMode;
        string _uaCustom = "";
        // The touch-emulation scroll pair over the page. On by default: the platforms cannot
        // reliably tell "has a touchscreen" from "does not", and a missing scroll button on a
        // device with no working touch path is worse than two buttons on one that has. The switch
        // is the answer, not the default.
        bool _showFab = true;
        // Fit the engine viewport to the window instead of leaving the fixed 720x1080 one.
        //
        // 2026-10-25: ON by default on Windows.Mobile, and a one-time migration turns it on for
        // installs whose settings.ini carries the old automatic `fitwindow=0`. The fixed frame is
        // a Lumia 950-shaped canvas (720 CSS px wide); on the 360-DIP Lumia 640 the display scale
        // shrinks every CSS px to 0.46 DIP, so the home page and all text render at half size
        // (measured: `frame fit scale=0,457 frame=720x1080 area=360x494`). Fitting makes one CSS
        // px one DIP on every panel, which is the only rule that survives the 360..576 DIP range
        // of the target phones. The user can still turn it off in Settings; the marker below
        // makes that choice stick. Desktop keeps the historical default.
        bool _fitWindow;
        // `fitwindow_migrated=1` in settings.ini: the old automatic 0 has already been replaced
        // by the mobile default. Without it, every launch would re-enable fitwindow and the
        // Settings switch would appear not to work.
        bool _fitWindowMigrated;
        // `ua_migrated=1` in settings.ini: the old automatic `ua=0` has already been replaced
        // by the device default. Same marker discipline as fitwindow_migrated: without it,
        // every launch would rewrite a deliberate `ua=` choice.
        bool _uaMigrated;
        // The address-bar dropdown (history + site matches). On by default, because that is what the
        // history tiles and the requirement 7 measurement were built around -- but it opens over the
        // page, and the page is what the user came for, so it needs an off switch. Read in
        // ShowSuggestions(), the one function that can put the panel on screen.
        bool _suggestOn = true;
        // Headless mode (ROADMAP.md, "Beyond 1.0"): 0 = off, 1 = summary, 2 = speak. A
        // register-only placeholder — shown in Settings/Advanced, saved to settings.ini as
        // headless=, and read by nothing. The later work (fetch → analyse → present without
        // paint) attaches here.
        int _headlessMode;
        string _searchPrefix = kSearchPrefixes[0];
        string _homeUrl = "about:home";
        int _uiLang;
        // Which language the home document currently on screen was built in. The home page is
        // engine-side HTML, so it cannot be repainted the way a XAML caption can -- it has to be
        // built again through the engine. This is what lets HideSettings notice that the language
        // changed while the user was in the settings overlay and rebuild the page once.
        int _homeHtmlLang = -1;

        // Verbose tracing. On by default for the bench, because on this build a LocalState file
        // is the only proof of what the engine did. Off for a person who just wants to read pages:
        // every Crumb() is a synchronous File.AppendAllText on the engine thread.
        // Static because Crumb() is called from the static SetupRuntimeEnv() on the engine thread,
        // which runs before any MainPage instance exists.
        static bool _traceOn = true;

        // ── Memory pressure and the ledger behind it (2026-10-03) ──────────────────────────────
        //
        // The C++ harness has reacted to AppMemoryUsageIncreased since 2026-09-24; this app had no
        // MemoryManager call anywhere, so the live line shipped without its entire remedy. The failure
        // that exposed it, measured on the Lumia: scrolling dzen.ru drove the working set from 100 to
        // 298 MB in 47 s and the process was *reaped* -- no crash dump even though CrashDumpEnabled was
        // true, no WER record, and no dangling line in the driver's per-line trace, i.e. destroyed
        // between two statements rather than inside one. A UWP app is killed by the platform when it
        // busts its memory limit, and that kill is silent, so nothing inside the process can report it.
        //
        // Two different things are needed here. The LIMIT, which nothing in this project had ever read
        // -- that is what mem.txt answers, and it is the number that turns "we suspect a quota kill"
        // into a measurement. And a release that can run *between* navigations: the port already called
        // WebCoreReleaseMemory(1) on every navigation, but the runaway happens while one page is being
        // scrolled, so the cadence was wrong, not the callee.
        //
        // Gated at Medium and above, copying the harness and for its measured reason: the platform
        // counts a process's first reading as a rise, and on a fresh launch that rise is into `low`,
        // where releasing costs a re-decode of everything and once posted a job ahead of the engine's
        // first navigation -- which is what killed the app at startup. _engineReady is the second
        // guard, and it is needed because here the handler is registered from the UI thread while the
        // engine thread is still inside SetupRuntimeEnv.
        static volatile bool _engineReady;
        static int _memLastLevel = -1;
        static DateTime _memLastLine = DateTime.MinValue;

        // Tabs
        List<Tab> _tabs = new List<Tab>();
        int _activeTab;

        // GPU
        bool _gpuOn;
        bool _gpuPresent;
        bool _gpuDefault = true;
        bool _gpuAutoTried;
        PropertySet _gpuProps;
        int _gpuOrient;

        // Live rendering
        DispatcherTimer _liveTimer;
        DispatcherTimer _memTimer;   // the 1 s memory ledger; see the block at _traceOn for why it exists
        bool _liveBusy;
        int _liveBusyAge;
        uint _lastFrameHash;
        int _liveStaticTicks;
        int _liveTotalTicks;

        // Scroll
        int _scrollAccum;
        int _scrollAccumX;
        bool _scrollBusy;
        // The point the current accumulation is about, in engine-surface px DIP. A page can scroll
        // an inner overflow container, and which one is decided by where the finger is -- so the
        // pump needs the point, not only the deltas. Captured with the accumulation like the
        // deltas themselves, so a coalesced pump uses the latest finger position. The initial
        // value is the surface's center, which is what a caller without a point (the scroll
        // buttons) means.
        int _scrollPointX = kW / 2;
        int _scrollPointY = kH / 2;

        // Pinch and other zooms.
        //
        // Apotheosis 2026-10-03: there is no page-scale state here any more, on purpose. The removal
        // is the fix for a defect a finger found and no file recorded: after a pinch on dzen.ru the
        // user reported an overlay that "lives in another layer -- scrolling inside the popup does
        // not move the popup, it scrolls the site underneath". That is what WebCoreSetPageScale
        // produces by construction: it MAGNIFIES the finished layer tree without re-running layout
        // (the port's own note on WebCoreSetPageZoom), so the engine's idea of the layout viewport
        // and the pixels on screen stop agreeing -- fixed-position elements, hit testing and the tap
        // mapping each pick a different one. CSS page zoom re-lays the page out instead, so there is
        // exactly one geometry and it is the one the user sees.
        //
        // _liveScale remains: it is the gesture's own preview factor, applied as a XAML
        // ScaleTransform and gone by the time the commit blits.
        bool _pinching;
        float _liveScale = 1.0f;
        double _focalX = 360, _focalY = 540;

        // ===== Double-tap to fit =====
        //
        // Two facts about the UWP gesture stack decide this design, and both are easy to get wrong:
        //   * when a double tap occurs, `Tapped` has ALREADY fired for the first tap. A naive
        //     implementation therefore forwards a click into the page and only then zooms -- and on
        //     a feed page the first tap of the pair follows a card link, so the "zoom" gesture would
        //     navigate away;
        //   * the second tap raises `DoubleTapped` and NOT a second `Tapped`, so cancelling the
        //     *pending* first tap is enough to make the pair do exactly one thing.
        // A tap is therefore held for the double-tap window before it is forwarded. That delays
        // every link tap in the app by kDoubleTapMs, which is the price every mobile browser pays
        // for this gesture; it is small next to the engine's own click latency (seconds on a heavy
        // page) and it is the only way the pair can be told apart. The window is the interval UWP
        // itself uses to decide the pair.
        const int kDoubleTapMs = 250;
        DispatcherTimer _tapTimer;
        double _tapX, _tapY;
        // Set when a double tap is handled. If the input stack also reports the second tap of the
        // pair as a `Tapped` -- the documentation says it does not, but the guarantee is the
        // platform's to keep and not ours to assume -- that stray tap is swallowed instead of
        // becoming a click 250 ms later. Zero means "never", and the subtraction wraps, so the
        // comparison is safe across `Environment.TickCount` going negative on a long uptime.
        uint _lastDoubleTapMs;

        // The zoom double-tap-to-fit returns to: the FIT zoom, where the layout width IS the window
        // width (see the geometry note on kViewportCeiling). CSS page zoom has no focal point to
        // zoom *around* -- it re-lays the page out rather than magnifying it -- so this gesture is a
        // level toggle and not a zoom-to-point: double-tap leaves the fit zoom, double-tap returns
        // to it. The level it leaves for is the last one the user chose that was not the fit zoom.
        const float kFitZoom = 1.0f;
        float _lastFitOffZoom = kZoomMax;

        // Load watchdog
        DispatcherTimer _loadWatchdog;

        // IME
        string _lastImeText = "";
        bool _imeOpen;
        bool _imeSyncing;

        bool _appForeground = true;

        public MainPage()
        {
            App.BootMark("mainpage-ctor");
            InitializeComponent();
            App.BootMark("mainpage-xaml-ok");
            LoadData();
            LoadSettings();
            _tabs.Add(new Tab { CurrentUrl = _homeUrl });
            _activeTab = 0;
            UpdateTabCount();
            // Language is known only after LoadSettings(), and the XAML no longer carries x:Uid,
            // so every caption in the tree has to be applied once here.
            RetranslateUi();

            // Apotheosis 2026-10-03: the LocalState command channel is started HERE, not only when
            // a session goes live. It used to be started from the navigation-completion path
            // (`if (sessionActive) StartAutoCmd();`), which made the channel depend on the app
            // having opened a session first -- and a scripted batch whose first line is `goto` can
            // never run, because nothing reads the batch until a navigation has already happened.
            // That is a deadlock the caller cannot see: the batch file is written, the app ignores
            // it, and the only symptom is an autocmd-log.txt that was never created.
            //
            // Starting it in the ctor costs a 250 ms timer that reads a file, and it makes the
            // first navigation scriptable, which is what every test harness here needs.
            StartAutoCmd();

            // Hardware / title-bar Back. The Lumia 950 has a physical Back key and Windows 11
            // surfaces a back affordance in the title bar for a UWP app, and both arrive here as
            // SystemNavigationManager.BackRequested rather than as a click on BackBtn. Without this
            // the key does nothing until the user reaches the first navigation, which reads as a
            // broken phone. The handler is a stack: it peels one overlay per press, and only then
            // navigates, and only then lets the shell close the app.
            try
            {
                var nav = SystemNavigationManager.GetForCurrentView();
                nav.BackRequested += OnSystemBackRequested;
                nav.AppViewBackButtonVisibility =
                    (SettingsPage.Visibility == Visibility.Visible || _navIndex > 0)
                        ? AppViewBackButtonVisibility.Visible
                        : AppViewBackButtonVisibility.Collapsed;
            }
            catch { }

            // Memory pressure. Registered in the ctor rather than in OnLaunched so it is live before
            // the first navigation, and here rather than elsewhere because it is the same kind of
            // app-lifetime subscription as the Back handler above. OnMemTick is the ledger, the
            // increased event is the remedy. Neither may block the UI thread, so the release itself
            // goes through WebEngine.Instance.Post.
            try
            {
                Windows.System.MemoryManager.AppMemoryUsageIncreased += OnMemoryUsageIncreased;
                _memTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
                _memTimer.Tick += OnMemTick;
                _memTimer.Start();
                MemLog("started limit=" + Mb(Windows.System.MemoryManager.AppMemoryUsageLimit)
                    + " used=" + Mb(Windows.System.MemoryManager.AppMemoryUsage)
                    + " level=" + MemLevelName((int)Windows.System.MemoryManager.AppMemoryUsageLevel));
            }
            catch (Exception ex) { MemLog("registration FAILED: " + ex.Message); }

            // GPU crash loop protection
            try
            {
                string d = ApplicationData.Current.LocalFolder.Path;
                string flag = Path.Combine(d, "gpu-crash.flag");
                if (File.Exists(flag))
                {
                    _gpuDefault = false;
                    File.Delete(flag);
                    SaveSettings();
                }
            }
            catch { }

            // Foreground/background
            Window.Current.VisibilityChanged += (s, e) =>
            {
                _appForeground = e.Visible;
                if (e.Visible) StartLiveMode(); else StopLiveMode();
            };

            // Soft keyboard
            try
            {
                var ip = InputPane.GetForCurrentView();
                ip.Showing += (sender, e) =>
                {
                    if (_urlFocused && RootShift != null)
                    {
                        RootShift.Y = -e.OccludedRect.Height;
                        e.EnsuredFocusedElementInView = true;
                    }
                };
                ip.Hiding += (sender, e) =>
                {
                    if (RootShift != null && RootShift.Y != 0)
                    {
                        RootShift.Y = 0;
                        e.EnsuredFocusedElementInView = true;
                    }
                };
            }
            catch { }

            // Test URL hook
            try
            {
                string d = ApplicationData.Current.LocalFolder.Path;
                string testPath = Path.Combine(d, "testurl.txt");
                if (File.Exists(testPath))
                {
                    string testUrl = File.ReadAllText(testPath).Trim();
                    if (!string.IsNullOrEmpty(testUrl))
                    {
                        NavigateTo(testUrl, true);
                        return;
                    }
                }
            }
            catch { }

            NavigateTo(_homeUrl, true);
        }

        // ===== Data persistence =====
        static string LocalStateDir() => ApplicationData.Current.LocalFolder.Path;

        static List<Entry> ReadEntries(string path)
        {
            var list = new List<Entry>();
            if (!File.Exists(path)) return list;
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
            {
                if (string.IsNullOrEmpty(line)) continue;
                var parts = line.Split('\t');
                var e = new Entry();
                if (parts.Length > 0) e.Url = parts[0];
                if (parts.Length > 1) e.Title = parts[1];
                if (parts.Length > 2) e.Extra = parts[2];
                list.Add(e);
            }
            return list;
        }

        static void WriteEntries(string path, List<Entry> v)
        {
            var sb = new StringBuilder();
            foreach (var e in v)
            {
                sb.Append(e.Url); sb.Append('\t');
                sb.Append(e.Title); sb.Append('\t');
                sb.Append(e.Extra); sb.Append('\n');
            }
            File.WriteAllText(path, sb.ToString(), Encoding.UTF8);
        }

        void LoadData()
        {
            string d = LocalStateDir();
            _bookmarks = ReadEntries(Path.Combine(d, "bookmarks.tsv"));
            _historyList = ReadEntries(Path.Combine(d, "history.tsv"));
            _downloads = ReadEntries(Path.Combine(d, "downloads.tsv"));
        }

        void SaveBookmarks()
        {
            string d = LocalStateDir();
            WriteEntries(Path.Combine(d, "bookmarks.tsv"), _bookmarks);
        }
        void SaveHistory()
        {
            string d = LocalStateDir();
            WriteEntries(Path.Combine(d, "history.tsv"), _historyList);
        }
        void SaveDownloads()
        {
            string d = LocalStateDir();
            WriteEntries(Path.Combine(d, "downloads.tsv"), _downloads);
        }

        void AddHistory(string url, string title)
        {
            if (string.IsNullOrEmpty(url) || url == "about:home") return;
            _historyList.RemoveAll(e => e.Url == url);
            _historyList.Insert(0, new Entry { Url = url, Title = string.IsNullOrEmpty(title) ? url : title });
            if (_historyList.Count > 300) _historyList.RemoveRange(300, _historyList.Count - 300);
            SaveHistory();
        }

        bool IsBookmarked(string url) => _bookmarks.Any(e => e.Url == url);

        // ===== Settings =====
        void LoadSettings()
        {
            bool settingsChanged = false;
            try
            {
                string d = LocalStateDir();
                string path = Path.Combine(d, "settings.ini");
                if (File.Exists(path))
                {
                    foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
                    {
                        int eq = line.IndexOf('=');
                        if (eq < 0) continue;
                        string k = line.Substring(0, eq), v = line.Substring(eq + 1);
                        switch (k)
                        {
                            case "search": _setSearch = int.Parse(v); break;
                            case "home": _homeUrl = string.IsNullOrEmpty(v) ? "about:home" : v; break;
                            case "ua": _setUaDesktop = (int.Parse(v) != 0); break;
                            case "zoom": _defaultZoom = int.Parse(v); break;
                            case "tabmode": _tabMode = int.Parse(v); break;
                            case "gpudefault": _gpuDefault = (int.Parse(v) != 0); break;
                            case "ua_custom": _uaCustom = v; break;
                            case "lang": _uiLang = int.Parse(v); break;
                            case "log": _traceOn = (int.Parse(v) != 0); break;
                            case "touchfab": _showFab = (int.Parse(v) != 0); break;
                            case "fitwindow": _fitWindow = (int.Parse(v) != 0); break;
                            case "fitwindow_migrated": _fitWindowMigrated = (int.Parse(v) != 0); break;
                            case "ua_migrated": _uaMigrated = (int.Parse(v) != 0); break;
                            case "suggest": _suggestOn = (int.Parse(v) != 0); break;
                            case "headless": _headlessMode = int.Parse(v); break;
                        }
                    }
                }
            }
            catch { }
            if (_setSearch < 0 || _setSearch > 3) _setSearch = 0;
            if (_headlessMode < 0 || _headlessMode > 2) _headlessMode = 0;
            if (_uiLang < 0 || _uiLang > 2) _uiLang = 0;
            Str.SetLang(_uiLang);

            // 2026-10-25: the mobile viewport migration. Every earlier build wrote
            // `fitwindow=0` into settings.ini on its first save even though the user never made
            // that choice, so the value itself cannot tell "old default" from "the user turned
            // it off". The marker splits the two cases: absent means the file predates this
            // rule (or the app has never saved), and the mobile default is applied once; once
            // written, a later explicit 0 is the user's and is left alone.
            if (IsMobileDevice() && !_fitWindowMigrated)
            {
                if (!_fitWindow) _fitWindow = true;
                _fitWindowMigrated = true;
                settingsChanged = true;
            }

            // 2026-10-25: the user-agent migration. The old build wrote the mobile iPhone
            // UA on every device; on a desktop window that is exactly the mismatch
            // dzen.ru's browser-switch-trap reports as an outdated browser. Replace the
            // old automatic value once on non-mobile devices. On Windows.Mobile the
            // mobile value is correct, and a desktop value there was an explicit choice,
            // so leave both alone. The marker makes a later explicit `ua=` stick.
            if (!_uaMigrated)
            {
                if (!IsMobileDevice() && !_setUaDesktop)
                    _setUaDesktop = true;
                _uaMigrated = true;
                settingsChanged = true;
            }

            ApplySettings();
            if (settingsChanged) SaveSettings();
        }

        // Windows.Mobile is the target this rule was written for: the phone panels in the
        // supported set are 360..576 DIP wide, and the fixed 720-px frame shrinks text by
        // 0.46..0.80 on them. The desktop bench (and every windowed build) keeps the old
        // default, so its 720x1080 measurements stay comparable.
        static bool IsMobileDevice()
        {
            try { return AnalyticsInfo.VersionInfo.DeviceFamily == "Windows.Mobile"; }
            catch { return false; }
        }

        void SaveSettings()
        {
            try
            {
                string d = LocalStateDir();
                var sb = new StringBuilder();
                sb.AppendLine($"search={_setSearch}");
                sb.AppendLine($"home={(_homeUrl == "about:home" ? "" : _homeUrl)}");
                sb.AppendLine($"ua={(_setUaDesktop ? 1 : 0)}");
                sb.AppendLine($"zoom={_defaultZoom}");
                sb.AppendLine($"tabmode={_tabMode}");
                sb.AppendLine($"gpudefault={(_gpuDefault ? 1 : 0)}");
                sb.AppendLine($"ua_custom={_uaCustom}");
                sb.AppendLine($"lang={_uiLang}");
                sb.AppendLine($"log={(_traceOn ? 1 : 0)}");
                sb.AppendLine($"touchfab={(_showFab ? 1 : 0)}");
                sb.AppendLine($"fitwindow={(_fitWindow ? 1 : 0)}");
                sb.AppendLine($"fitwindow_migrated={(_fitWindowMigrated ? 1 : 0)}");
                sb.AppendLine($"ua_migrated={(_uaMigrated ? 1 : 0)}");
                sb.AppendLine($"suggest={(_suggestOn ? 1 : 0)}");
                sb.AppendLine($"headless={_headlessMode}");
                File.WriteAllText(Path.Combine(d, "settings.ini"), sb.ToString(), Encoding.UTF8);
            }
            catch { }
        }

        // ── The memory ledger and the release it feeds (2026-10-03) ────────────────────────────
        //
        // mem.txt is a per-line flushed marker, the same kind of channel the driver uses and for the
        // same reason: it is the only thing that survives the death being investigated. The 1 s
        // cadence is the point. The WDP-side sampler this replaced could manage a reading only every
        // 3-4 s, which is coarse enough to lose the event itself -- the last thing it saw was 298 MB,
        // and the kill came within the next second, so its curve can say "it exploded" but not what it
        // exploded from.

        static string Mb(ulong bytes) => (bytes / 1048576.0).ToString("N1") + "MB";

        static string MemLevelName(int level)
        {
            switch (level)
            {
                case 0: return "low";
                case 1: return "medium";
                case 2: return "high";
                case 3: return "overlimit";
                default: return "?" + level;
            }
        }

        // Opens and closes the file every line: a buffered writer would lose the tail exactly when it
        // matters, which is the lesson the driver's own marker channel is built on.
        static void MemLog(string text)
        {
            try
            {
                File.AppendAllText(Path.Combine(LocalStateDir(), "mem.txt"),
                    DateTime.Now.ToString("HH:mm:ss.fff") + "  " + text + Environment.NewLine);
            }
            catch { }
        }

        void OnMemTick(object s, object e)
        {
            try
            {
                ulong used = Windows.System.MemoryManager.AppMemoryUsage;
                ulong limit = Windows.System.MemoryManager.AppMemoryUsageLimit;
                int level = (int)Windows.System.MemoryManager.AppMemoryUsageLevel;
                // Every second while anything moves, every fifth while the app sits idle: this file
                // has to stay short enough to read its tail on a phone, and a flat stretch says
                // nothing. A level change is always written, whatever the clock says.
                bool levelChanged = level != _memLastLevel;
                if (levelChanged || (DateTime.Now - _memLastLine).TotalSeconds >= 5)
                {
                    _memLastLine = DateTime.Now;
                    if (levelChanged) _memLastLevel = level;
                    MemLog("used=" + Mb(used) + " limit=" + Mb(limit)
                        + (limit > 0 ? " pct=" + (used * 100 / limit) : " pct=?")
                        + " level=" + MemLevelName(level) + (levelChanged ? " (changed)" : ""));
                }
            }
            catch { }
        }

        // Fires on an upward *crossing* of the memory level. The platform counts a process's first
        // reading as one, and on a fresh launch that rise is into `low` -- so the gate below is not
        // defensive, it is the fix for a real startup death; the full argument is at _traceOn.
        void OnMemoryUsageIncreased(object sender, object e)
        {
            try
            {
                int level = (int)Windows.System.MemoryManager.AppMemoryUsageLevel;
                ulong used = Windows.System.MemoryManager.AppMemoryUsage;
                ulong limit = Windows.System.MemoryManager.AppMemoryUsageLimit;
                MemLog("PRESSURE level=" + MemLevelName(level) + " used=" + Mb(used) + " limit=" + Mb(limit));
                if (level < 1)
                    return;                 // low: a fresh launch's first reading, not pressure
                if (!_engineReady)
                {
                    MemLog("PRESSURE ignored: engine not ready yet");
                    return;
                }
                PostMemRelease("pressure:" + MemLevelName(level));
            }
            catch (Exception ex) { MemLog("PRESSURE handler failed: " + ex.Message); }
        }

        // Posts, never calls: every C ABI call runs on the engine thread and the UI thread must not
        // block on the engine. This is the same queue every other engine call in the app uses, and it
        // is what makes the release land between jobs instead of in the middle of one.
        void PostMemRelease(string why)
        {
            ulong used = Windows.System.MemoryManager.AppMemoryUsage;
            ulong limit = Windows.System.MemoryManager.AppMemoryUsageLimit;
            MemLog("release REQUEST (" + why + ") used=" + Mb(used) + " limit=" + Mb(limit));
            WebEngine.Instance.Post(() =>
            {
                try
                {
                    if (!_engineReady) { MemLog("release SKIPPED: engine not ready"); return; }
                    WebCoreDriver.WebCoreReleaseMemory(1);
                    MemLog("release DONE (engine thread) used=" + Mb(Windows.System.MemoryManager.AppMemoryUsage));
                }
                catch (Exception ex) { MemLog("release FAILED: " + ex.Message); }
            });
        }

        void ApplySettings()
        {
            _searchPrefix = SearchPrefixFor(_setSearch);
            _searchPrefixStatic = _searchPrefix;
            // The one clamp for the SETTING. The slider's own range is the same pair, and a pinch
            // clamps to it too (CommitPageZoom), so the three levers cannot disagree about what the
            // engine will accept.
            _defaultZoom = Math.Max((int)(kZoomMin * 100f), Math.Min((int)(kZoomMax * 100f), _defaultZoom));
            _uaMobile = !_setUaDesktop;
            int mobile = _uaMobile ? 1 : 0;
            string ua = _uaCustom;
            WebEngine.Instance.Post(() =>
            {
                try { WebCoreDriver.WebCoreSetUserAgentMobile(mobile); } catch { }
                try { WebCoreDriver.WebCoreSetUserAgentString(string.IsNullOrEmpty(ua) ? null : ua); } catch { }
            });
            if (UaBtn != null)
                UaBtn.Content = Str.Get(_uaMobile ? "MobileSite" : "DesktopSite");

            // The UA no longer decides any geometry, so this call is here only to re-derive the
            // viewport on its own terms -- AdoptViewport recomputes _vpZoom from the page-zoom
            // setting, and the UA switch is simply the most common moment for it to be worth a
            // repaint. It is kept (rather than dropped) because a UA change is also the moment a
            // page is most likely to want a re-layout, and because this is where the uaMobile= field
            // of the trace below is anchored.
            //
            // Apotheosis 2026-10-03: until today the zoom WAS a function of the UA -- desktop filled
            // the window at 1.0 while mobile re-laid the page out at a fixed 412 CSS px and
            // magnified it, which on this bench was 3.29. See the note on kViewportCeiling.
            //
            // This runs on the UI thread at every call site, and AdoptViewport is a no-op unless
            // fitwindow is on and a layout pass has happened, so it is safe on the ctor's path too.
            // Its work is posted after the UA Post above: one engine queue, same order.
            Crumb($"applysettings: uaMobile={(_uaMobile ? 1 : 0)} fit={(_fitWindow ? 1 : 0)}"
                  + $" vp={_vpW}x{_vpH} kW={kW}x{kH} vz={_vpZoom:0.00} pz={_pageZoom:0.00}"
                  + $" session={(_sessionActive ? 1 : 0)}");
            if (_fitWindow) AdoptViewport();
        }

        static string SearchPrefixFor(int idx)
        {
            if (idx >= 0 && idx < kSearchPrefixes.Length) return kSearchPrefixes[idx];
            return kSearchPrefixes[0];
        }

        static string NormalizeUrl(string raw)
        {
            if (raw == null) raw = "";
            raw = raw.Trim();
            if (string.IsNullOrEmpty(raw)) return "about:home";
            bool looksUrl = raw.Contains("://") || (raw.Contains('.') && !raw.Contains(' '));
            if (raw.StartsWith("about:")) return raw;
            if (!looksUrl) return _searchPrefixStatic + Uri.EscapeDataString(raw);
            if (!raw.Contains("://")) raw = "https://" + raw;
            return raw;
        }
        static string _searchPrefixStatic = kSearchPrefixes[0];


        static string HtmlEscape(string s)
        {
            return s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;").Replace("'", "&#39;");
        }

        static string HostOf(string u)
        {
            int p = u.IndexOf("://");
            int s = (p < 0) ? 0 : p + 3;
            int e = u.IndexOf('/', s);
            return u.Substring(s, (e < 0 ? u.Length : e) - s);
        }

        // ===== Navigation =====
        void UpdateNavButtons()
        {
            // Back is available either because the ENGINE has an entry behind the current one (which
            // covers link clicks, in-page navigations and redirects -- none of which the host drives)
            // or because the host's own stack has somewhere to go back to. Forward is the same in
            // the other direction. Before the engine pair existed only the host stack was consulted,
            // so Back stayed greyed out on a session that had followed a link, and "back returns to
            // the previous page" could not be satisfied at all.
            if (BackBtn != null) BackBtn.IsEnabled = (_engineBack != 0) || (_navIndex > 0);
            if (FwdBtn != null) FwdBtn.IsEnabled = (_engineFwd != 0) || (_navIndex >= 0 && _navIndex < _navStack.Count - 1);
        }

        // Ask the engine where its back/forward list stands, and cache both answers. MUST run on the
        // engine thread: every WebCore* call is serialized there. `_engineBack`/`_engineFwd` are
        // plain ints precisely so that the UI thread may then read them without touching the engine.
        void RefreshEngineHistory()
        {
            try
            {
                _engineBack = WebCoreDriver.WebCoreCanGoBack();
                _engineFwd = WebCoreDriver.WebCoreCanGoForward();
            }
            catch { _engineBack = 0; _engineFwd = 0; }
        }

        void SetLoading(bool loading)
        {
            _loading = loading;
            if (Progress != null)
            {
                Progress.IsIndeterminate = loading;
                Progress.Visibility = loading ? Visibility.Visible : Visibility.Collapsed;
            }
            UpdateUrlActionGlyph();
            // One place to pick up a viewport that arrived while the engine was busy, because this
            // is the single point every load ends at. The first attempt put the pickup in the
            // after-load callback instead, ahead of OnNavDone -- which is what clears _loading -- so
            // ApplyViewport bailed on its own busy test, set _vpDirty again, and nothing ever asked
            // it a second time. The measurement was a page that never resized and a log with no
            // `fitwindow:` line in it at all.
            //
            // DrainPendingZoom and not AdoptViewport: the fitted mode has geometry to adopt, the
            // fixed 720x1080 line only has a pending zoom to re-commit, and AdoptViewport returns at
            // once when _fitWindow is off. See DrainPendingZoom().
            if (!loading && _vpDirty) DrainPendingZoom();
            // S2 (2026-10-21): deliver a navigation that arrived while this load owned the shell.
            // Deferred one dispatcher turn at Low priority on purpose: the ending load's own
            // epilogue (OnNavDone's zoom re-apply, the tap path's synchronous linkHit NavigateTo)
            // must finish against the OLD page before the queued navigation replaces it -- running
            // NavigateTo from inside SetLoading would re-enter it under OnNavDone's feet and the
            // zoom re-apply would land on the new, still-building document. Discard when a newer
            // navigation already took the shell: "last one wins", and the newer ask IS the last
            // one (measured order: tap path calls SetLoading(false) then NavigateTo(linkHit)
            // synchronously, so the queued URL must not outlive it).
            if (!loading && _pendingNav != null)
            {
                string qUrl = _pendingNav; bool qPush = _pendingNavPush;
                _pendingNav = null;
                try
                {
                    Dispatcher.RunAsync(CoreDispatcherPriority.Low, () =>
                    {
                        if (_loading) { AutoLog("nav QUEUED superseded: " + qUrl); return; }
                        AutoLog("nav QUEUED delivered: " + qUrl);
                        NavigateTo(qUrl, qPush);
                    });
                }
                catch { }
            }
        }

        void NavigateTo(string url, bool pushHistory)
        {
            // Apotheosis 2026-10-03: a navigation dropped here is a navigation the user asked for
            // and did not get, and it used to leave NO trace at all -- the scripted `goto` logged
            // "done", stage.txt kept the previous line, the engine diag kept the previous page.
            // Measured: a `goto` issued while the shell was still finishing the previous render
            // was dropped in silence and looked exactly like a navigation that had not been
            // delivered yet. Say which it was. (2026-10-21, S2: the drop below became a QUEUE --
            // "nav QUEUED" -- delivered from SetLoading(false); the log-line habit is what made
            // the drop measurable in the first place.)
            // A navigation attempt closes the dropdown, delivered or not. This used to sit below the
            // "shell busy" early-return, so a navigation issued while a load was in flight was
            // dropped in silence AND left the suggestion list hanging over the page -- measured on
            // the bench 2026-10-03: typing dzen.ru and pressing Enter while the previous dzen load
            // was still running left a full-height list over the content area, and the shell was
            // loading, so no later event was going to close it either. The list belongs to the
            // address box, so every way out of the box closes it.
            HideSuggestions();
            if (_loading)
            {
                // S2 (2026-10-21): HOLD, don't drop. The old behaviour was a silent drop -- the
                // measured case was a `goto` swallowed for the whole life of one hung Usercentrics
                // subresource (curlcode=28, >60 s on the Lumia; "nav DROPPED, shell busy" in
                // autocmd-log). A queued navigation is delivered by SetLoading(false), which every
                // load end passes through -- including the 40 s watchdog, so a wedged load cannot
                // hold the queue forever. Last one wins: a second ask replaces the first.
                _pendingNav = url;
                _pendingNavPush = pushHistory;
                AutoLog("nav QUEUED, shell busy: " + url);
                return;
            }
            string wurl = string.IsNullOrEmpty(url) ? "about:home" : url;
            bool isHome = wurl == "about:home";
            _currentUrl = isHome ? "about:home" : wurl;

            // End the gesture's preview, not the zoom. The page zoom is a user-level setting now, so
            // a navigation must not clear it: the port's teardown DOES forget the factor behind our
            // back (teardownSession resets g_pageZoom to 1.0), and that is why the after-load path
            // re-applies it -- see the note in OnNavDone. Clearing the value here as well would make
            // a zoomed page snap back to 100 % on the next link instead of being re-armed.
            _pinching = false; _liveScale = 1.0f;
            if (GpuPanel != null) GpuPanel.RenderTransform = null;
            if (RenderImage != null) RenderImage.RenderTransform = null;

            if (pushHistory)
            {
                if (_navIndex >= 0 && _navIndex < _navStack.Count - 1)
                    _navStack.RemoveRange(_navIndex + 1, _navStack.Count - _navIndex - 1);
                _navStack.Add(wurl);
                _navIndex = _navStack.Count - 1;
            }
            UpdateNavButtons();
            UpdateLockIcon();
            SetUrlBoxText(isHome ? "" : url);
            if (TitleText != null)
                TitleText.Text = isHome ? Str.Get("Home") : $"{Str.Get("Loading")}  {wurl}";
            SetLoading(true);

            // Load watchdog
            if (_loadWatchdog == null)
            {
                _loadWatchdog = new DispatcherTimer();
                _loadWatchdog.Interval = TimeSpan.FromSeconds(40);
                _loadWatchdog.Tick += OnLoadWatchdog;
            }
            _loadWatchdog.Start();

            CoreDispatcher disp = Dispatcher;
            string surl = url ?? "";
            long mySeq = ++_opSeq;
            string homeHtml = isHome ? BuildHomeHtml(_bookmarks, _historyList) : "";
            if (isHome) _homeHtmlLang = Str.Lang;
            // Read on the UI thread and carried into the posted body: the engine thread must not read
            // the shell's settings fields, and this one decides the zoom the new document is built at.
            int zoomPctForLoad = _defaultZoom;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                bool loadOk = false;
                bool sessionActive = false;
                string title = "";
                try
                {
                    if (isHome)
                    {
                        WriteStage("before-close-session");
                        WebCoreDriver.WebCoreCloseSession();
                        WriteStage("before-home-render");
                        GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                        try { rc = WebCoreDriver.WebCoreRenderHtml(homeHtml, kW, kH, h.AddrOfPinnedObject()); }
                        finally { h.Free(); }
                        WriteStage("after-home-render rc=" + rc + (rc == 0 ? "" : EngineFaultInfo()));
                        loadOk = (rc == 0);
                        title = "Home";
                    }
                    else
                    {
                        WriteStage("before-load " + surl);
                        // Arm the zoom for the document that is about to be built, BEFORE the session
                        // exists. The post-load re-apply in OnNavDone is too late for a page that
                        // reads its width once, at load: the document is laid out at the viewport's
                        // CSS default first, and a site whose JS gate breaks at 870 then decides
                        // "mobile" before any zoom exists (the Nokia archive is exactly this -- its
                        // Desktop-mode screen override reports innerWidth, which is 720 at that
                        // moment). This call records the value only; buildSession applies it before
                        // the document is created. Unconditional: on the fixed-viewport line the
                        // viewport is 720 for the document's whole life, and on the fit-window line
                        // the boot load still sees the default 720x1080 because the fitwindow resize
                        // arrives AFTER the load -- measured, both lines started the first document
                        // at 720 and left the Nokia gate closed (document at layout width = viewport
                        // / zoom is the shared contract of the two lines, so the arm is too).
                        try { WebCoreDriver.WebCoreSetNextSessionZoom(zoomPctForLoad / 100f); }
                        catch { }
                        GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                        int netRc;
                        try { netRc = WebCoreDriver.WebCoreSessionLoad(surl, kW, kH, h.AddrOfPinnedObject()); }
                        finally { h.Free(); }
                        byte[] tBuf = new byte[512];
                        WebCoreDriver.WebCoreGetTitle(tBuf, tBuf.Length);
                        string t = Encoding.UTF8.GetString(tBuf).TrimEnd('\0');
                        byte[] diagBuf = new byte[4096];
                        WebCoreDriver.WebCoreGetDiag(diagBuf, diagBuf.Length);
                        string diag = Encoding.UTF8.GetString(diagBuf).TrimEnd('\0');
                        byte[] errBuf = new byte[512];
                        WebCoreDriver.WebCoreGetLastError(errBuf, errBuf.Length);
                        string err = Encoding.UTF8.GetString(errBuf).TrimEnd('\0');
                        int comp = 0;
                        try { comp = WebCoreDriver.WebCoreEnableCompositing(); } catch { }
                        WriteStage($"after-load url={surl} rc={netRc} compositing={comp}\nERR: {err}\ndiag: {diag}"
                                  + (netRc == 0 ? "" : EngineFaultInfo()));
                        if (netRc == 0)
                        {
                            rc = 0; loadOk = true; sessionActive = true;
                            title = string.IsNullOrEmpty(t) ? surl : t;
                        }
                        else
                        {
                            string eh = MakeErrorHtml(surl, err);
                            GCHandle h2 = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                            try { rc = WebCoreDriver.WebCoreRenderHtml(eh, kW, kH, h2.AddrOfPinnedObject()); }
                            finally { h2.Free(); }
                            loadOk = false;
                            title = Str.Get("LoadFailed");
                        }
                    }
                }
                catch { rc = -1000; loadOk = false; title = Str.Get("RenderError"); }

                // Both branches above may have replaced the session (a load creates a fresh Page, an
                // about:home render closes it), so the cached back/forward answer is stale by
                // construction. Re-read it here rather than in either branch: one place, both paths.
                RefreshEngineHistory();

                var links = ExtractLinks();
                string titleCopy = title;
                bool ok = (rc == 0);
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq)
                        {
                            // A newer operation owns the engine now, and _loading is ITS flag to
                            // clear -- so it must not be cleared here. But this used to return
                            // without a word, and when the superseding operation did not clear the
                            // flag either, _loading stayed true and every later NavigateTo was
                            // dropped in silence. Say it out loud; the flag is now also visible in
                            // the `diag` line, so the two halves of that story meet in one file.
                            AutoLog($"nav SUPERSEDED seq {mySeq} -> {_opSeq} ({_currentUrl})");
                            return;
                        }
                        // OnNavDone is what clears _loading, so it runs even when a step above it
                        // throws. This try/finally is the second half of the same fix: the body
                        // used to be one bare sequence under an empty catch, and an exception in
                        // StopLiveMode/StartLiveMode/StartAutoCmd left the shell permanently busy
                        // with the page already blitted on screen.
                        try
                        {
                            if (ok)
                            {
                                BlitToBitmap(rgba);
                                _pageLinks = links;
                            }
                            _sessionActive = sessionActive;
                            RefreshFab();
                            _lastFrameHash = 0;

                            // Apotheosis 2026-10-03: the engine's page zoom does NOT survive a
                            // navigation, and the shell's cache does -- which is a bug that hides
                            // itself. Every navigation here goes through WebCoreSessionLoad, which
                            // tears the session down, and teardownSession resets the port's
                            // remembered zoom to 1.0 ("the requested zoom belonged to the frame
                            // that just went away"). The new session is therefore laid out
                            // unzoomed, while _pageZoom still holds the zoom the last frame was
                            // given -- so AdoptViewport/ApplyViewport compare two equal numbers and
                            // answer "nothing to do", and the page is presented across the full
                            // window for the rest of the session.
                            //
                            // Measured on the 1355-DIP bench, 0.2.3.8, when the wanted zoom was
                            // still `dipW / 412` in mobile mode: `ua mobile` logged
                            // `fitwindow: 1355x751 zoom=3,29` and the zoom reached the engine
                            // (`[GPU] page zoom 1.00 -> 3.29`), then the next `goto` presented an
                            // UNZOOMED frame that was byte-identical to the desktop one, and no
                            // further `fitwindow:` line was ever written. Two frames, 2736x1728,
                            // zero differing pixels.
                            //
                            // The same hole opens for any page-zoom setting other than 100%, so the
                            // guard below is now that condition rather than the mobile mode.
                            //
                            // Forget the cache at the only moment it can become wrong, and let the
                            // load's own pickup re-derive it: _vpDirty makes SetLoading(false) ->
                            // AdoptViewport recompute _vpZoom as well, so a setting that changed
                            // while this load was in flight is covered by the same line. Guarded on
                            // the WANTED zoom rather than the cached one, because the cached one is
                            // exactly what may be stale here. At the default 100% the wanted zoom IS
                            // the engine's default, so nothing is asked for and no repaint is spent.
                            if (_vpW >= 16 && Math.Abs(ViewportZoom() - 1f) > 0.001f)
                            {
                                _pageZoom = -1f;   // unknown to the shell; the engine has 1.0
                                _vpDirty = true;
                            }
                            if (sessionActive) StartLiveMode(); else StopLiveMode();
                            // The command channel is started in the ctor (see there); this is a belt
                            // for a session that somehow went live without it. StartAutoCmd is
                            // idempotent, so the second call is free.
                            if (sessionActive) StartAutoCmd();
                        }
                        catch (Exception ex)
                        {
                            AutoLog("nav done threw " + ex.GetType().Name + ": " + ex.Message);
                        }
                        finally
                        {
                            // A size that arrived while this load owned the engine is picked up by
                            // SetLoading(false) inside OnNavDone -- not earlier, because _loading is
                            // still set at that point and ApplyViewport would only defer to itself.
                            OnNavDone(titleCopy, ok, loadOk);
                        }
                    });
                }
                catch { }
            });
        }

        void OnNavDone(string finalTitle, bool ok, bool loadOk)
        {
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            _currentTitle = finalTitle ?? "";
            if (TitleText != null)
                TitleText.Text = string.IsNullOrEmpty(_currentTitle) ? Str.Get("AppTitle") : finalTitle;
            if (loadOk && _currentUrl != "about:home")
                AddHistory(_currentUrl, _currentTitle);
            if (_currentUrl != "about:home")
            {
                SetUrlBoxText(_currentUrl);
            }
            UpdateLockIcon();
            SetLoading(false);
            UpdateNavButtons();
            if (!_updateAutoChecked && loadOk && _currentUrl != "about:home")
            {
                _updateAutoChecked = true;
                CheckForUpdate(false);
            }
            if (_gpuDefault && !_gpuOn && !_gpuAutoTried && _sessionActive && _currentUrl != "about:home")
            {
                _gpuAutoTried = true;
                EnableGpu();
                return;
            }
            // Re-apply the user's page zoom after a load, because a navigation resets the engine's
            // factor behind our back (see the note in the nav-done block above).
            //
            // Both geometries now apply the SAME lever -- CSS page zoom -- and the paths differ only
            // in who does the repaint:
            //   fitwindow on  -- the nav-done sentinel sets _vpDirty, so SetLoading(false) ->
            //                    AdoptViewport -> ApplyViewport re-applies the zoom and repaints.
            //                    Doing it here as well would post a second zoom+resize through
            //                    PostZoomAndRepaint, bump _opSeq and make the first frame superseded:
            //                    an extra ~1.2 s repaint per load for nothing.
            //   fitwindow off -- the fixed 720x1080 viewport, which is the ARM line as it stands.
            //                    Nothing above this line re-applies the zoom there (AdoptViewport
            //                    returns at once when _fitWindow is off), so it is done here, at the
            //                    current size: WebCoreSessionResize repaints even when the size is
            //                    unchanged, which is what makes a page zoom possible without a resize
            //                    at all.
            // Before 2026-10-03 the fitwindow-off branch went in as page SCALE, because SetPageZoom
            // does not paint and there was no resize to borrow a repaint from. With the resize
            // repainting at any size, that reason is gone and the scale path is gone with it.
            if (!_fitWindow && _sessionActive && _defaultZoom != 100)
                CommitPageZoom(_defaultZoom / 100.0f, "reload");
        }

        void OnLoadWatchdog(object s, object e)
        {
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            if (_loading || _interacting)
            {
                ++_opSeq;
                _interacting = false;
                _sessionActive = false;
                RefreshFab();
                if (TitleText != null) TitleText.Text = Str.Get("LoadTimeout");
                SetLoading(false);
                UpdateNavButtons();
            }
        }

        // ===== Click handling =====
        void MapTapToEngine(double dipX, double dipY, out int outPx, out int outPy)
        {
            if (dipX < 0 || dipY < 0) { outPx = -1; outPy = -1; return; }
            ApplyDisplayScale();   // S7: divide by the scale the screen is showing, not a stale one
            double aw = ContentArea.ActualWidth, ah = ContentArea.ActualHeight;
            if (_gpuPresent && aw > 1 && ah > 1)
            {
                outPx = (int)(dipX * kW / aw + 0.5);
                outPy = (int)(dipY * kH / ah + 0.5);
            }
            else
            {
                // S7: the finger (and the scripted `tap`) speaks ContentArea DIP; the frame is
                // drawn at _fitScale, so the frame pixel under the finger is dip / _fitScale.
                // At 1.0 this is the pre-S7 1:1 line unchanged.
                outPx = (int)(dipX / _fitScale + 0.5);
                outPy = (int)(dipY / _fitScale + 0.5);
            }
        }

        // The inverse of MapTapToEngine, and it has to stay next to it.
        //
        // `taplink` picks a link out of _pageLinks, whose X/Y/W/H are in ENGINE pixels -- the space
        // ForwardClickToEngine compares against -- while HandleTapAt takes DIP, the space a finger
        // reports. The two only differ on the GPU path (see the DPI note on MapTapToEngine), which
        // is exactly why the pair has to be read together: one of them changed alone is a tap that
        // lands somewhere else.
        void MapEngineToDip(int px, int py, out double outDipX, out double outDipY)
        {
            ApplyDisplayScale();   // S7: keep the pair symmetric with MapTapToEngine
            double aw = ContentArea.ActualWidth, ah = ContentArea.ActualHeight;
            if (_gpuPresent && aw > 1 && ah > 1)
            {
                outDipX = px * aw / kW;
                outDipY = py * ah / kH;
            }
            else
            {
                outDipX = px * _fitScale;
                outDipY = py * _fitScale;
            }
        }

        // S7 (2026-10-22): recompute and apply the fixed frame's display scale.
        //
        // The complaint this answers, measured by the .57 batch's own tap row on the Lumia:
        // ContentArea is 360x494 DIP while the engine frame is a fixed 720x1080 bitmap drawn
        // Stretch=None at 1:1 -- the user saw the top-left QUARTER of the page, the rest of the
        // window was white, and each engine pixel covered ~4x4 physical pixels (the reported
        // "softness"). The frame itself must NOT shrink to the window: 360 / kZoomMin(0.50) =
        // 720 CSS < 870 and the desktop gate would never open. So the geometry stays and only
        // the PRESENTATION scales: FrameFit draws the bitmap as large as fits, never above 1:1
        // (a window the frame already fits keeps today's exact 1:1), top-left anchored.
        //
        // Both inputs feed it -- area (SizeChanged) and frame (kW/kH, rotation, fit toggles,
        // every repaint) -- so it is RECOMPUTED at each call site rather than carried by
        // events: on entry to the two coordinate-mapping functions (so a finger can never
        // divide by a stale number), at every blit (the frame geometry is settled by then),
        // and on every size change. The fitted viewport computes 1.0 here (area == frame by
        // construction), which is why fitwindow=1 probes are untouched.
        //
        // GPU present is deliberately not a case here: its surface is the panel's own physical
        // pixels, MapTapToEngine's GPU branch maps against aw/ah directly, and BlitToBitmap
        // returns before this function is ever reached on that path.
        void ApplyDisplayScale()
        {
            double aw = ContentArea.ActualWidth, ah = ContentArea.ActualHeight;
            if (aw < 1 || ah < 1 || kW < 1 || kH < 1) return;
            double d = Math.Min(1.0, Math.Min(aw / kW, ah / kH));
            if (Math.Abs(d - _fitScale) < 0.0005) return;
            _fitScale = d;
            if (FrameFitScale != null) { FrameFitScale.ScaleX = d; FrameFitScale.ScaleY = d; }
            // One line per CHANGE, not per call: the tap probes and the rotation markers get
            // their scale without a per-frame write. `frame fit scale=` is the S7 marker.
            Crumb($"frame fit scale={d:0.000} frame={kW}x{kH} area={(int)aw}x{(int)ah}");
        }

        // The tap the user's finger produces and the tap a script produces enter HERE, and that is
        // the point of this function existing.
        //
        // Apotheosis 2026-10-03: OnPageTapped used to hold this body directly, which made the app's
        // only tap path reachable solely from XAML's manipulation stack -- and that stack cannot be
        // driven synthetically on this bench (memory: `synthetic-input-cannot-reach-uwp`). The C++
        // harness closed that gap for itself long ago with `TapFromScript` -> `HandleTapAt`; the
        // shipping C# line had no equivalent, so "does a tap follow a link?" could be answered only
        // by reading code. This is that equivalent: the autocmd verbs `tap` and `taplink` call this
        // function, and so does the finger. A scripted tap that went anywhere else would measure a
        // different code path and prove nothing about the one a user has.
        void HandleTapAt(double dipX, double dipY)
        {
            HideSuggestions();
            if (_interacting) return;
            // S8 (2026-10-21): `_loading` used to refuse the tap SILENTLY here. Measured on the
            // Lumia (.59): a Nokia load held by one hung Usercentrics subresource made every tap
            // dead -- the maintainer's "Accept All" on the CMP overlay did nothing, however often
            // it was pressed. Same disease S2 cured for navigation: a hung third-party resource
            // must not dead-end INPUT. The engine takes clicks during a load by design;
            // WebCoreClickAt queues behind the in-flight work on the engine thread, and this
            // tap's own completion clears _loading (its SetLoading(true) re-arms the watchdog, so
            // the 40 s ceiling still applies). `_interacting` stays: it is the engine-ownership
            // guard, not a load-state gate.
            if (_loading) AutoLog($"tap during load: dip={dipX:0},{dipY:0}");
            MapTapToEngine(dipX, dipY, out int px, out int py);
            if (px < 0 || py < 0 || px >= kW || py >= kH) return;

            if (_sessionActive)
            {
                ForwardClickToEngine(px, py);
                return;
            }
            for (int i = _pageLinks.Count - 1; i >= 0; i--)
            {
                var l = _pageLinks[i];
                if (px >= l.X && px < l.X + l.W && py >= l.Y && py < l.Y + l.H)
                {
                    NavigateTo(l.Url, true);
                    return;
                }
            }
        }

        void OnPageTapped(object sender, TappedRoutedEventArgs e)
        {
            var pt = e.GetPosition(ContentArea);
            if (_lastDoubleTapMs != 0 &&
                unchecked((uint)Environment.TickCount - _lastDoubleTapMs) < (uint)kDoubleTapMs)
                return;   // the tail of a double tap, not a tap of its own
            // Held for the double-tap window rather than forwarded now -- see kDoubleTapMs. The
            // scripted `tap`/`taplink` verbs call HandleTapAt directly and are NOT delayed: a script
            // has no double taps to disambiguate, and paying the wait there would only make the
            // bench slower than the finger.
            _tapX = pt.X; _tapY = pt.Y;
            if (_tapTimer == null)
            {
                _tapTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(kDoubleTapMs) };
                _tapTimer.Tick += (s, a) => { _tapTimer.Stop(); HandleTapAt(_tapX, _tapY); };
            }
            _tapTimer.Stop();
            _tapTimer.Start();
        }

        void OnPageDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            // Cancel the pending single tap first, whatever else happens: the pair must do one
            // thing, and the pending tap is a click that has not been sent yet.
            if (_tapTimer != null) _tapTimer.Stop();
            _lastDoubleTapMs = (uint)Environment.TickCount;
            DoubleTapFit();
        }

        // The body of the gesture, kept separate from the event handler so the `dtap` autocmd verb
        // can drive THE SAME path a finger does. A script that re-implemented the toggle would
        // measure the verb rather than the feature.
        void DoubleTapFit()
        {
            if (!_sessionActive) { Crumb("dtapfit: no session"); return; }
            bool atFit = Math.Abs(_vpZoom - kFitZoom) < 0.001f;
            float want = atFit ? _lastFitOffZoom : kFitZoom;
            Crumb($"dtapfit: {_vpZoom:0.00} -> {want:0.00} ({(atFit ? "off fit" : "to fit")})");
            CommitPageZoom(want, "dtapfit");
        }

        void ForwardClickToEngine(int px, int py)
        {
            if (_interacting) return;
            _interacting = true;
            SetLoading(true);
            if (_loadWatchdog != null) _loadWatchdog.Start();
            if (TitleText != null)                 TitleText.Text = Str.Get("Loading") + "\u2026";

            string linkHit = "";
            for (int i = _pageLinks.Count - 1; i >= 0; i--)
            {
                var l = _pageLinks[i];
                if (px >= l.X && px < l.X + l.W && py >= l.Y && py < l.Y + l.H) { linkHit = l.Url; break; }
            }
            string prevUrl = _currentUrl;
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                uint hashBefore = WebCoreDriver.WebCoreGetFrameHash();
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try { rc = WebCoreDriver.WebCoreClickAt(px, py, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                uint hashAfter = (rc == 0) ? WebCoreDriver.WebCoreGetFrameHash() : hashBefore;
                bool changed = (hashAfter != hashBefore);
                int editable = 0;
                try { if (rc == 0) editable = WebCoreDriver.WebCoreFocusedEditable(); } catch { }
                // A tap is the main way the engine's history grows -- a link click, an in-page route
                // change, a redirect -- and none of it goes through the host. Ask the engine here so
                // the toolbar shows Back as available for a trail the host never saw.
                RefreshEngineHistory();

                string navUrl = "", title = "";
                var links = new List<PageLink>();
                if (rc == 0)
                {
                    byte[] tBuf = new byte[512];
                    WebCoreDriver.WebCoreGetTitle(tBuf, tBuf.Length);
                    title = Encoding.UTF8.GetString(tBuf).TrimEnd('\0');
                    byte[] uBuf = new byte[1024];
                    WebCoreDriver.WebCoreGetUrl(uBuf, uBuf.Length);
                    string newUrl = Encoding.UTF8.GetString(uBuf).TrimEnd('\0');
                    if (!string.IsNullOrEmpty(newUrl) && newUrl != prevUrl) navUrl = newUrl;
                    links = ExtractLinks();
                }
                string titleW = title, navW = navUrl;
                int rcCopy = rc; bool changedCopy = changed; int editableCopy = editable;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _interacting = false;
                        if (_loadWatchdog != null) _loadWatchdog.Stop();
                        // The engine answered above; make the toolbar show it. Needed as its own
                        // call because a tap that only repaints (navUrl empty) never reaches
                        // ApplyEngineFrame's URL branch, where the other UpdateNavButtons lives.
                        UpdateNavButtons();
                        if (rcCopy == 0)
                        {
                            if (string.IsNullOrEmpty(navW) && !changedCopy && !string.IsNullOrEmpty(linkHit))
                            {
                                SetLoading(false);
                                NavigateTo(linkHit, true);
                            }
                            else
                            {
                                ApplyEngineFrame(rgba, titleW, navW, links);
                                SetLoading(false);
                                if (string.IsNullOrEmpty(navW)) { if (editableCopy != 0) OpenKeyboard(); else CloseKeyboard(); }
                                else CloseKeyboard();
                            }
                        }
                        else if (!string.IsNullOrEmpty(linkHit))
                        {
                            SetLoading(false);
                            NavigateTo(linkHit, true);
                        }
                        else
                        {
                            if (rcCopy == -12 || rcCopy == -14)
                            {
                                _sessionActive = false;
                                RefreshFab();
                            }
                            SetLoading(false);
            if (TitleText != null)
                TitleText.Text = string.IsNullOrEmpty(_currentTitle) ? Str.Get("AppTitle") : _currentTitle;
                        }
                    });
                }
                catch { }
            });
        }

        // `historyDelta` says the engine's own back/forward list produced this frame, and it changes
        // only how the host stack is kept in step. Zero means a normal navigation (a tap, an
        // address-bar entry) started something NEW, so anything ahead of the cursor is discarded and
        // the URL is appended. Non-zero -- -1 Back, +1 Forward -- went to a page that already
        // existed, so the forward entries must SURVIVE: the cursor is moved to wherever that URL
        // already sits in the stack, and an append-and-truncate here would delete the very entries
        // Forward exists to reach -- which is how a Back would silently disarm Forward after a
        // single use.
        //
        // The search runs FROM THE OLD CURSOR IN THE DIRECTION OF TRAVEL, not from the front of the
        // stack, and that detail is the whole reason the parameter is a delta rather than a flag.
        // A stack can hold the same URL twice ([A, B, A], which is what a Back after a Back-to-A-of
        // two visits produces) and `IndexOf` would answer with the oldest A on the way forward as
        // well as on the way back. Measured on the x64 bench 2026-10-14: one Back out of a four-entry
        // stack with a repeated URL moved the cursor from 3 to 1 and jumped over the entry it had
        // actually landed on. Scanning outward from the cursor in the direction of travel names the
        // entry the engine really selected.
        // The entry a Back (delta < 0) or a Forward (delta > 0) just landed on, searched outward from
        // where the cursor was. See the note on ApplyEngineFrame for why the direction has to be part
        // of the question: a URL can appear more than once in the stack, and only the direction says
        // which of the copies the engine selected. Returns -1 when the URL is not in that direction,
        // which leaves the caller's append branch to add it -- the honest answer for a move whose
        // target the host never recorded.
        int NearestInDirection(int delta, string url)
        {
            if (delta < 0)
            {
                for (int i = _navIndex - 1; i >= 0; i--)
                    if (_navStack[i] == url) return i;
            }
            else
            {
                for (int i = _navIndex + 1; i < _navStack.Count; i++)
                    if (_navStack[i] == url) return i;
            }
            return -1;
        }

        void ApplyEngineFrame(byte[] rgba, string title, string navUrl, List<PageLink> links, int historyDelta = 0)
        {
            if (!_gpuPresent)
                BlitToBitmap(rgba);
            _pageLinks = links;
            _lastFrameHash = 0;
            StartLiveMode();
            if (!string.IsNullOrEmpty(title))
            {
                _currentTitle = title;
                if (TitleText != null) TitleText.Text = title;
            }
            // S4/S3 (2026-10-22): an EMPTY navUrl means "the engine could not name the
            // document", not "the document is called ''". The tap path passes "" when the
            // engine URL is unchanged or unreadable (WebCoreGetUrl answers "" until
            // writeDiag has run once), and the old `!= null` test then took the empty
            // string as a URL: it cleared _currentUrl AND the address box. Measured on the
            // 0.2.3.61 bench: tap a non-link spot on example.com and the box goes blank
            // while the page stays put and the engine diag still says url=https://example.com/
            // -- the same disappearance the .58/.59 device runs saw as `url= | url=...`.
            // Unknown must keep what the user last saw; only NavigateTo clears the box,
            // and it does so because it has a real target. Callers that mean "no change"
            // already pass null (the back/forward path at the `navW` construction).
            if (!string.IsNullOrEmpty(navUrl))
            {
                _currentUrl = navUrl;
                SetUrlBoxText(navUrl);
                UpdateLockIcon();
                UpdateUrlActionGlyph();
                int at = historyDelta == 0 ? -1 : NearestInDirection(historyDelta, navUrl);
                if (at >= 0)
                {
                    // Already in the stack: move the cursor, keep everything else (the forward tail
                    // included). No truncation, no append.
                    _navIndex = at;
                }
                else
                {
                    bool dup = (_navIndex >= 0 && _navIndex < _navStack.Count && _navStack[_navIndex] == navUrl);
                    if (!dup)
                    {
                        if (_navIndex >= 0 && _navIndex < _navStack.Count - 1)
                            _navStack.RemoveRange(_navIndex + 1, _navStack.Count - _navIndex - 1);
                        _navStack.Add(navUrl);
                        _navIndex = _navStack.Count - 1;
                    }
                }
                UpdateNavButtons();
                AddHistory(_currentUrl, _currentTitle);
            }
        }

        // Apotheosis 2026-09-27: per-frame frame-buffer allocation. A 3-buffer pool was tried here and
        // reverted. The reasoning for a pool was sound -- `new byte[kW * kH * 4]` is 2.97 MB and
        // lands on the never-compacted Large Object Heap, once per scroll step and once per live
        // tick -- but the array is captured by a dispatcher closure and read on the UI thread after
        // the P/Invoke returns, so a buffer can be recycled while a frame that still references it
        // is queued for blit. With three slots and a scroll plus a live tick both in flight that
        // pipeline can exceed the ring, and a recycled buffer is exactly how you get a frame that
        // shows a mix of two states. The remaining per-frame 2.97 MB is worth attacking at the
        // source instead: have paintToRGBA write BGRA so no conversion buffer is needed at all.
        static byte[] NewFrameBuffer() => new byte[kW * kH * 4];

        // ===== Scroll =====
        void EngineScroll(int dy)
        {
            // S8 (2026-10-21): `_loading` dropped from the gate, same rationale as HandleTapAt --
            // the FAB buttons died for the whole life of a hung subresource. `_sessionActive`
            // stays: it is the watchdog's own verdict on the page.
            if (!_sessionActive || _interacting) return;
            _interacting = true;
            SetLoading(true);
            if (_loadWatchdog != null) _loadWatchdog.Start();
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = NewFrameBuffer();
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try { rc = WebCoreDriver.WebCoreScrollBy(0, dy, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                var links = ExtractLinks();
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _interacting = false;
                        if (_loadWatchdog != null) _loadWatchdog.Stop();
                        SetLoading(false);
                        if (rcCopy == 0)
                        {
                            if (!_gpuPresent) BlitToBitmap(rgba);
                            _pageLinks = links;
                            _lastFrameHash = 0;
                            StartLiveMode();
                        }
                    });
                }
                catch { }
            });
        }

        void FreeScrollBy(int dx, int dy)
        {
            FreeScrollBy(dx, dy, _scrollPointX, _scrollPointY);
        }

        // The point matters as much as the delta: a page can scroll an inner overflow container,
        // and the container is chosen by what is under the finger. See FreeScrollBy's two-argument
        // wrapper for the callers that have no point of their own.
        void FreeScrollBy(int dx, int dy, int x, int y)
        {
            if (!_sessionActive || (dx == 0 && dy == 0)) return;
            _scrollPointX = x;
            _scrollPointY = y;
            _scrollAccumX += dx;
            _scrollAccum += dy;
            if (!_scrollBusy) PumpScroll();
        }

        void PumpScroll()
        {
            if ((_scrollAccum == 0 && _scrollAccumX == 0) || !_sessionActive) { _scrollBusy = false; return; }
            int dy = _scrollAccum; _scrollAccum = 0;
            int dx = _scrollAccumX; _scrollAccumX = 0;
            int px = _scrollPointX;
            int py = _scrollPointY;
            _scrollBusy = true;
            CoreDispatcher disp = Dispatcher;
            long mySeq = _opSeq;
            bool present = _gpuPresent;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = NewFrameBuffer();
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                // WebCoreScrollByAt, not WebCoreScrollBy: the point lets the port scroll the
                // innermost scrollable element under the finger and fall back to the frame. The
                // frame-only call silently did nothing on pages whose content scrolls inside an
                // overflow container (`nokiadesignarchive.aalto.fi`'s mobile view was the measured
                // case: contents 720x1080 == viewport, finger moved, nothing happened).
                try { rc = WebCoreDriver.WebCoreScrollByAt(px, py, dx, dy, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        // S8 (2026-10-21): one line per merged pump. The maintainer's "the page
                        // does not scroll" had no device-side evidence at all before this: a drag
                        // that reaches nothing, a scroll the engine refuses and a scroll whose
                        // blit was superseded all look identical from outside. rc, the point and
                        // the delta are what tell them apart.
                        AutoLog($"scroll dx={dx} dy={dy} at {px},{py} rc={rcCopy}");
                        if (_opSeq != mySeq) { _scrollBusy = false; _scrollAccum = 0; return; }
                        if (rcCopy == 0)
                        {
                            if (!present) BlitToBitmap(rgba);
                            _lastFrameHash = 0;
                        }
                        _scrollBusy = false;
                        if (_scrollAccum != 0 || _scrollAccumX != 0) PumpScroll();
                        else { SyncLinksAfterScroll(); StartLiveMode(); }
                    });
                }
                catch { }
            });
        }

        void SyncLinksAfterScroll()
        {
            if (!_sessionActive) return;
            CoreDispatcher disp = Dispatcher;
            long mySeq = _opSeq;
            WebEngine.Instance.Post(() =>
            {
                try { WebCoreDriver.WebCoreSyncLinks(); } catch { }
                var links = ExtractLinks();
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Low, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _pageLinks = links;
                    });
                }
                catch { }
            });
        }

        // ===== Pinch zoom, as page zoom =====
        //
        // The gesture keeps the committed page zoom as its base (_vpZoom, the value the engine was
        // last told and the one ViewportZoom() derives from the setting), and it previews by scaling
        // the frame it already has. The preview is a magnification of the OLD layout on purpose: page
        // zoom re-runs layout, and re-running it per finger delta would cost ~1.2 s of engine thread
        // each time. Chrome previews its pinch the same way -- magnify now, reflow on release -- and
        // the commit below is what makes the two agree.
        void OnImageManipDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            if (!_sessionActive) return;
            ApplyDisplayScale();   // S7: before the focal point AND the scroll delta read it
            float ds = e.Delta.Scale;
            if (_pinching || (ds > 0 && (ds > 1.002f || ds < 0.998f)))
            {
                _pinching = true;
                if (ds > 0) _liveScale *= ds;
                // Clamped to the range the commit can actually deliver, so the picture under the
                // finger is never a factor the release would silently refuse. kW/kZoomLo/hi are the
                // Settings slider's own bounds (ApplySettings clamps _defaultZoom to 50..200).
                float total = _vpZoom * _liveScale;
                if (total < kZoomMin) _liveScale = kZoomMin / _vpZoom;
                if (total > kZoomMax) _liveScale = kZoomMax / _vpZoom;
                var fp = e.Position;
                _focalX = fp.X; _focalY = fp.Y;   // ContentArea DIP; ApplyLiveZoom divides
                ApplyLiveZoom();
                return;
            }
            // S7: the finger moves DIP, FreeScrollBy and the engine speak frame pixels -- at
            // _fitScale the on-screen document moves exactly as far as the finger did. The GPU
            // path keeps the raw delta: its surface is the panel's physical px and that mapping
            // was already its own (unmeasured) rule.
            double sc = _gpuPresent ? 1.0 : _fitScale;
            double dx = -e.Delta.Translation.X / sc;
            double dy = -e.Delta.Translation.Y / sc;
            int idx = (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
            int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
            if (idx != 0 || idy != 0)
            {
                // The finger's own position decides which inner scroller takes the delta, so it
                // rides along with the accumulation (see FreeScrollBy). MapTapToEngine is the
                // same DIP -> engine-px mapping the tap path uses, which matters on the GPU path.
                var pos = e.Position;
                MapTapToEngine(pos.X, pos.Y, out int px, out int py);
                if (px < 0 || py < 0 || px >= kW || py >= kH) { px = _scrollPointX; py = _scrollPointY; }
                FreeScrollBy(idx, idy, px, py);
            }
        }

        void ApplyLiveZoom()
        {
            if (_gpuPresent)
            {
                GpuPanel.RenderTransform = new ScaleTransform
                { ScaleX = _liveScale, ScaleY = _liveScale, CenterX = _focalX, CenterY = _focalY };
                return;
            }
            // S7: the base display scale is the PARENT transform (FrameFit), the preview is the
            // child's (RenderImage) -- they compose, so the net is screen = d * (c + (i - c) * l)
            // and holding the picture under the finger at ContentArea position f needs the
            // child's focal point in IMAGE coordinates: c = f / _fitScale. Verified algebra:
            // image point i = f/d sits at f on screen at l = 1, and stays at f for any l.
            // With fitwindow=1 _fitScale is 1.0 and this is the pre-S7 math unchanged.
            double fs = _fitScale > 0.01 ? _fitScale : 0.01;
            RenderImage.RenderTransform = new ScaleTransform
            { ScaleX = _liveScale, ScaleY = _liveScale, CenterX = _focalX / fs, CenterY = _focalY / fs };
        }

        void OnImageManipCompleted(object sender, ManipulationCompletedRoutedEventArgs e)
        {
            if (!_pinching) return;
            _pinching = false;
            float live = _liveScale; _liveScale = 1.0f;
            // The focal point is dropped here and that is not an oversight: CSS page zoom has no
            // focal point. It re-lays the page out at `viewport / zoom` CSS px and paints that, so
            // the frame the user gets is anchored the way the engine anchors it. The focal point is
            // still what the live preview scales around, which is why _focalX/_focalY survive the
            // commit call -- but nothing downstream of the engine reads them any more.
            CommitPageZoom(_vpZoom * live, "pinch");
            // A pinch on a heavy page finishes while the load's own zoom re-apply still holds the
            // engine, so the commit above defers. Try the drain immediately -- it is a no-op when
            // the engine was free and the commit went straight through -- and let OnLiveTick's
            // safety net cover the case where it was not.
            if (_vpDirty) DrainPendingZoom();
        }

        // Apply a page zoom now, at the current viewport, and repaint.
        //
        // This is the replacement for the old PinchCommit/WebCoreSetPageScale path, and it is
        // deliberately the SAME lever the Settings slider drives (ViewportZoom -> _vpZoom ->
        // PostZoomAndRepaint). Two consequences worth stating, because both are behaviour the user
        // will see:
        //   * the pinch sets the zoom level itself (_defaultZoom), so the Settings slider follows the
        //     finger and the value persists like a browser's zoom level does;
        //   * the gesture cannot zoom past the slider's range (50..200 %), because the preview and
        //     the commit share one clamp. Widening that range is a one-line change in ApplySettings
        //     plus kZoomMin/kZoomMax here, and it is the only place to widen.
        void CommitPageZoom(float zoom, string why)
        {
            if (zoom < kZoomMin) zoom = kZoomMin;
            if (zoom > kZoomMax) zoom = kZoomMax;
            int pct = (int)(zoom * 100f + 0.5f);
            _defaultZoom = pct;
            _vpZoom = pct / 100f;
            // What double-tap-to-fit will zoom back to next time. Recorded here and not at the
            // gesture, because this is the one function every zoom lever goes through -- slider,
            // pinch and double-tap alike -- so "the last zoom the user chose" needs no second
            // bookkeeper.
            if (Math.Abs(_vpZoom - kFitZoom) > 0.001f) _lastFitOffZoom = _vpZoom;

            if (!_sessionActive) { Crumb($"{why}: no session, zoom {pct}% kept for the next load"); return; }
            if (_gpuPresent) { Crumb($"{why}: skipped, GPU frame live"); return; }
            // Same rule as ApplyViewport: a load or an interaction owns the engine at one size and
            // one buffer. _vpDirty is enough here -- the wanted zoom is now _defaultZoom, and every
            // deferred pickup re-derives _vpZoom from it through ViewportZoom(), so the pinch is not
            // lost, it is only late.
            if (_loading || _interacting) { _vpDirty = true; Crumb($"{why}: deferred (L{(_loading ? 1 : 0)}I{(_interacting ? 1 : 0)})"); return; }

            // Already there: nothing to re-lay out and nothing to repaint. This is not a
            // micro-optimisation -- a pinch that lands back on the zoom it started from used to pay
            // the whole price (one layout plus one repaint, ~1 s of engine thread) for a frame
            // identical to the one on screen; measured on this bench, a repeat pinch at the 0.50
            // floor did exactly that.
            if (!_vpDirty && pct == (int)(_pageZoom * 100f + 0.5f))
            {
                Crumb($"{why}: already at {pct}%");
                return;
            }

            // No geometry change: this is a zoom, so the viewport stays exactly as it was and the
            // resize inside PostZoomAndRepaint is there for its repaint, not for its size.
            PostZoomAndRepaint(kW, kH, _vpZoom, why);
        }

        // ===== Live rendering loop =====
        void StartLiveMode()
        {
            if (!_sessionActive || !_appForeground) return;
            if (Drawer.Visibility == Visibility.Visible) return;
            if (ActionMenu.Visibility == Visibility.Visible) return;
            if (SettingsPage.Visibility == Visibility.Visible) return;
            if (TabSwitcher.Visibility == Visibility.Visible) return;
            _liveBusy = false;
            _liveBusyAge = 0;
            _liveStaticTicks = 0;
            _liveTotalTicks = 0;
            if (_liveTimer == null)
            {
                _liveTimer = new DispatcherTimer();
                _liveTimer.Tick += OnLiveTick;
            }
            _liveTimer.Interval = TimeSpan.FromMilliseconds(200);
            _liveTimer.Start();
        }

        void StopLiveMode()
        {
            _liveTimer?.Stop();
        }

        void OnLiveTick(object s, object e)
        {
            if (!_sessionActive || !_appForeground || _loading || _interacting) return;
            if (Drawer.Visibility == Visibility.Visible
                || ActionMenu.Visibility == Visibility.Visible
                || SettingsPage.Visibility == Visibility.Visible
                || TabSwitcher.Visibility == Visibility.Visible)
            { StopLiveMode(); return; }
            if (_liveBusy)
            {
                if (++_liveBusyAge < 5) return;
                _liveBusy = false;
            }
            // Safety net for a resize or a zoom that arrived while something else owned the engine:
            // this is reached only when the session is live and nothing is loading or interacting,
            // which is exactly the state both drains need. Without it a size that landed during a
            // swipe waited for the next navigation, and a pinch that landed during a load was simply
            // lost on the fixed-viewport line.
            if (_vpDirty) { DrainPendingZoom(); return; }
            _liveBusyAge = 0;
            _liveBusy = true;
            long mySeq = _opSeq;
            bool present = _gpuPresent;
            CoreDispatcher disp = Dispatcher;

            WebEngine.Instance.Post(() =>
            {
                if (!_appForeground) return;
                byte[] rgba = NewFrameBuffer();
                int rc = -999; uint hash = 0; int pending = 0;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try
                {
                    rc = WebCoreDriver.WebCoreLiveTick(h.AddrOfPinnedObject());
                    if (rc == 0)
                    {
                        hash = WebCoreDriver.WebCoreGetFrameHash();
                        pending = WebCoreDriver.WebCoreGetPendingResourceCount();
                    }
                }
                finally { h.Free(); }
                // S3 (2026-10-22): the engine can change its document without the shell
                // asking -- a redirect chain's next hop, an SPA route change,
                // history.replaceState, a location.hash. Nothing in the shell writes
                // _currentUrl on those, so the box kept the REQUESTED url indefinitely
                // (measured on the harness 2026-09-19: box said dzen.ru while the document
                // was sso.dzen.ru/install). The live tick is the right place for the same
                // reason the harness learned it: it is the only thing that runs while a
                // page sits still. Read here, on the engine thread, in the same posted
                // body as the tick -- the work is a snprintf out of a static buffer, but
                // keeping every WebCore* call on this thread is the rule the rest of the
                // file follows. The UI side only compares and writes.
                //
                // Deliberately NOT pushed onto _navStack: whether an engine-initiated
                // navigation replaces the current history entry or appends one is a
                // question the harness left open (m-currenturl-never-follows-the-engine),
                // and answering it wrongly corrupts Back/Forward. The box and _currentUrl
                // are display state; the stack stays shell-authored.
                string engineUrl = "";
                if (rc == 0)
                {
                    byte[] uBuf = new byte[1024];
                    WebCoreDriver.WebCoreGetUrl(uBuf, uBuf.Length);
                    engineUrl = Encoding.UTF8.GetString(uBuf).TrimEnd('\0');
                }
                int rcCopy = rc; uint hashCopy = hash; int pendingCopy = pending;
                string engineUrlCopy = engineUrl;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Low, () =>
                    {
                        _liveBusy = false;
                        if (_opSeq != mySeq) return;
                        if (!_sessionActive || _loading || _interacting || !_appForeground) return;
                        if (rcCopy != 0)
                        {
                            if (rcCopy == -12 || rcCopy == -14)
                            {
                                _sessionActive = false;
                                RefreshFab();
                                StopLiveMode();
                            }
                            return;
                        }
                        // S3: the engine's document url wins, but never while the user is
                        // typing in the box -- an overwrite mid-keystroke moves the caret
                        // and, worse, rewrites what CommitUrlBox will parse. `_urlSyncing`
                        // is set by SetUrlBoxText itself; `_urlFocused` is the user's grab.
                        // This sits BEFORE the hash branch on purpose: an SPA route change
                        // often leaves the frame hash identical (same layout, new url), so
                        // a URL update that only ran with a changed frame would miss
                        // exactly the case this exists for.
                        //
                        // S3 (0.2.3.65): AutoLog FIRST, the box second. Measured on the .65
                        // bench: after clicklink hopped the engine to iana.org the box showed
                        // the new url while autocmd-log held NO `url follows engine` line --
                        // the update path between SetUrlBoxText and AutoLog (the two glyph
                        // refreshers) can throw, the enclosing catch{} eats it, and the one
                        // line that proves S3 works disappears with it. The observation must
                        // not depend on the cosmetics that follow it.
                        if (!string.IsNullOrEmpty(engineUrlCopy) && engineUrlCopy != _currentUrl
                            && !_urlFocused && !_urlSyncing)
                        {
                            AutoLog("url follows engine: " + engineUrlCopy);
                            _currentUrl = engineUrlCopy;
                            SetUrlBoxText(engineUrlCopy);
                            UpdateLockIcon();
                            UpdateUrlActionGlyph();
                        }
                        if (hashCopy == _lastFrameHash)
                        {
                            if (pendingCopy > 0)
                            {
                                _liveStaticTicks = 0;
                                int ticks = ++_liveTotalTicks;
                                if (ticks == 150 && _liveTimer != null) _liveTimer.Interval = TimeSpan.FromSeconds(1);
                                if (ticks >= 300) StopLiveMode();
                                return;
                            }
                            // S3 (0.2.3.65): was StopLiveMode() at 40 static ticks -- and that
                            // made the whole S3 mechanism vacuous: measured on the .65 bench, a
                            // page idle for 8 s had already stopped the tick before clicklink
                            // hopped the engine to iana.org (15:23:54 vs stop at ~15:23:44), so
                            // nothing noticed the hop: no `url follows engine`, no frame blit.
                            // The tick is the only thing that runs while a page sits still --
                            // it must not die, it must only slow down. One poll per second on a
                            // static page costs a RunLoop cycle and an update pass, and it is
                            // what keeps the box honest about engine-initiated hops (the S3
                            // contract). The fast cadence comes back the moment the frame moves.
                            if (++_liveStaticTicks >= 40)
                            {
                                if (_liveTimer != null && _liveTimer.Interval < TimeSpan.FromSeconds(1))
                                {
                                    _liveTimer.Interval = TimeSpan.FromSeconds(1);
                                    AutoLog("live tick: static, slowed to 1s (watch stays on)");
                                }
                                return;
                            }
                            return;
                        }
                        _liveStaticTicks = 0;
                        _lastFrameHash = hashCopy;
                        if (!present) BlitToBitmap(rgba);
                        if (_liveTimer != null && _liveTimer.Interval > TimeSpan.FromMilliseconds(200))
                            _liveTimer.Interval = TimeSpan.FromMilliseconds(200);   // frame moved: full cadence back
                        if (++_liveTotalTicks == 150 && _liveTimer != null)
                            _liveTimer.Interval = TimeSpan.FromSeconds(1);
                    });
                }
                catch { }
            });
        }

        // ===== IME =====
        void OpenKeyboard()
        {
            _imeOpen = true;
            _imeSyncing = true;
            if (ImeBox != null) ImeBox.Text = "";
            _lastImeText = "";
            _imeSyncing = false;
            ImeBox?.Focus(FocusState.Programmatic);
        }

        void CloseKeyboard()
        {
            if (!_imeOpen) return;
            _imeOpen = false;
            try { InputPane.GetForCurrentView().TryHide(); } catch { }
        }

        void OnImeTextChanged(object sender, TextChangedEventArgs e)
        {
            if (_imeSyncing || !_imeOpen || !_sessionActive) return;
            string cur = ImeBox?.Text ?? "";
            string prev = _lastImeText;
            if (cur == prev) return;
            if (cur.Length > prev.Length && cur.StartsWith(prev))
            {
                SendKeyToEngine(0, cur.Substring(prev.Length));
            }
            else if (cur.Length < prev.Length && prev.StartsWith(cur))
            {
                int n = prev.Length - cur.Length;
                for (int i = 0; i < n; i++) SendKeyToEngine(2, null);
            }
            else
            {
                for (int i = 0; i < prev.Length; i++) SendKeyToEngine(2, null);
                if (!string.IsNullOrEmpty(cur)) SendKeyToEngine(0, cur);
            }
            _lastImeText = cur;
        }

        void OnImeKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (!_imeOpen) return;
            if (e.Key == VirtualKey.Enter)
            {
                e.Handled = true;
                SendKeyToEngine(1, null);
                _imeSyncing = true;
                if (ImeBox != null) ImeBox.Text = "";
                _imeSyncing = false;
                _lastImeText = "";
            }
            else if (e.Key == VirtualKey.Back && string.IsNullOrEmpty(_lastImeText))
            {
                SendKeyToEngine(2, null);
            }
        }

        void SendKeyToEngine(int kind, string text)
        {
            if (!_sessionActive) return;
            CoreDispatcher disp = Dispatcher;
            string utf8 = (kind == 0 && text != null) ? text : "";

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try
                {
                    if (kind == 0) rc = WebCoreDriver.WebCoreTypeText(utf8, h.AddrOfPinnedObject());
                    else if (kind == 1) rc = WebCoreDriver.WebCoreKeyAction(1, h.AddrOfPinnedObject());
                    else rc = WebCoreDriver.WebCoreKeyAction(0, h.AddrOfPinnedObject());
                }
                finally { h.Free(); }
                string navUrl = "", title = "";
                var links = new List<PageLink>();
                if (rc == 0 && kind == 1)
                {
                    byte[] tBuf = new byte[512];
                    WebCoreDriver.WebCoreGetTitle(tBuf, tBuf.Length);
                    title = Encoding.UTF8.GetString(tBuf).TrimEnd('\0');
                    byte[] uBuf = new byte[1024];
                    WebCoreDriver.WebCoreGetUrl(uBuf, uBuf.Length);
                    navUrl = Encoding.UTF8.GetString(uBuf).TrimEnd('\0');
                    links = ExtractLinks();
                }
                string navW = navUrl, titleW = title;
                int rcCopy = rc; int kindCopy = kind;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (rcCopy != 0) return;
                        if (kindCopy == 1 && !string.IsNullOrEmpty(navW) && navW != _currentUrl)
                        {
                            ApplyEngineFrame(rgba, titleW, navW, links);
                            CloseKeyboard();
                        }
                        else
                        {
                            if (!_gpuPresent) BlitToBitmap(rgba);
                            _lastFrameHash = 0;
                            StartLiveMode();
                        }
                    });
                }
                catch { }
            });
        }

        // ===== Toolbar events =====
        // One press of the system Back key peels exactly one layer, in the order a person would
        // expect: the thing that is on top of the page goes away first, and history is only
        // consulted when nothing is covering the page. Returning without setting Handled on the
        // last step hands the key to the shell, which closes the app -- the correct behaviour at
        // the root of the navigation stack, and what a phone user expects.
        void OnSystemBackRequested(object sender, BackRequestedEventArgs e)
        {
            try
            {
                if (SettingsPage.Visibility == Visibility.Visible) { HideSettings(); e.Handled = true; return; }
                if (TabSwitcher.Visibility == Visibility.Visible) { TabSwitcher.Visibility = Visibility.Collapsed; e.Handled = true; return; }
                if (FindBar.Visibility == Visibility.Visible) { OnFindClose(this, null); e.Handled = true; return; }
                if (SuggestPanel.Visibility == Visibility.Visible) { SuggestPanel.Visibility = Visibility.Collapsed; e.Handled = true; return; }
                if (ActionMenu.Visibility == Visibility.Visible) { HideActionMenu(); e.Handled = true; return; }
                // "Is there anywhere to go back to" is asked of the engine first, because the host
                // stack alone answers a narrower question: it knows only the URLs the app itself
                // navigated to, so a session that reached its page by following a link looked like
                // the root of history and the key fell through to the shell, closing the app.
                if (!_loading && (_engineBack != 0 || _navIndex > 0)) { OnBack(this, null); e.Handled = true; }
            }
            catch { }
        }

        void OnBack(object s, RoutedEventArgs e) => NavigateHistory(-1);

        void OnForward(object s, RoutedEventArgs e) => NavigateHistory(+1);

        // Back/forward through the host's own stack. This is the fallback, not the main path: it is
        // reached when the ENGINE has nothing that way (a page rendered with WebCoreRenderHtml has
        // no history at all) or when there is no live session to ask. It navigates by URL, so it
        // does not preserve a forward tail -- NavigateTo(url, false) never pushes.
        void NavigateHostHistory(int delta)
        {
            if (_loading) return;
            int want = _navIndex + delta;
            if (want < 0 || want >= _navStack.Count) return;   // nothing that way; stay put
            _navIndex = want;
            NavigateTo(_navStack[_navIndex], false);
        }

        // One step of the ENGINE's own back/forward list, then blit the destination. This is what
        // requirement 5 ("back returns to the previous page") actually means: the list holds link
        // clicks, in-page navigations and redirects, none of which the host ever initiates, so a
        // Back driven from _navStack alone cannot follow the trail the user actually walked. The
        // host stack is consulted only when the engine reports nothing that way.
        //
        // Same shape as the tap path (see OnTapAt): _interacting + SetLoading(true) claim the engine
        // exclusively, the P/Invoke runs on the engine thread, and only the finished frame reaches
        // the UI thread. WebCoreGoBack RELOADS the entry rather than restoring a snapshot (the
        // back/forward cache is disabled process-wide), so this may take as long as a navigation.
        void NavigateHistory(int delta)
        {
            // Every way out of this method writes a line, including the three that do nothing.
            //
            // Measured on the bench 2026-10-14, running the acceptance list: the rows whose load
            // had failed came back from `back` with NO line at all -- the engine's `history back
            // rc=` is written inside the posted body below, and these early returns never reach it.
            // A Back that leaves no record is indistinguishable from a Back that never arrived,
            // which is the same failure mode the scripted-channel work was built to remove from
            // the navigation path (see the note on NavigateTo's "nav QUEUED" line). The reason
            // is named rather than implied, because "no session", "still loading" and "a gesture
            // owns the engine" are three different states that need three different answers.
            const string skip = "history ";
            if (_loading || _interacting)
            {
                AutoLog(skip + (delta < 0 ? "back" : "fwd") + " skipped busy loading="
                        + (_loading ? 1 : 0) + " interacting=" + (_interacting ? 1 : 0));
                return;
            }
            if (!_sessionActive)
            {
                // No live session: no engine list exists to consult, so the host stack is all there is.
                AutoLog(skip + (delta < 0 ? "back" : "fwd") + " skipped reason=no-session");
                NavigateHostHistory(delta);
                return;
            }

            _interacting = true;
            SetLoading(true);
            if (_loadWatchdog != null) _loadWatchdog.Start();
            if (TitleText != null) TitleText.Text = Str.Get("Loading") + "…";

            string prevUrl = _currentUrl;
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -99;
                bool moved = false;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try
                {
                    int can = (delta < 0) ? WebCoreDriver.WebCoreCanGoBack() : WebCoreDriver.WebCoreCanGoForward();
                    if (can != 0)
                    {
                        rc = (delta < 0) ? WebCoreDriver.WebCoreGoBack(h.AddrOfPinnedObject())
                                         : WebCoreDriver.WebCoreGoForward(h.AddrOfPinnedObject());
                        moved = (rc == 0);
                    }
                    RefreshEngineHistory();
                }
                finally { h.Free(); }
                int canBack = _engineBack, canFwd = _engineFwd;

                string navUrl = "", title = "";
                var links = new List<PageLink>();
                if (moved)
                {
                    byte[] tBuf = new byte[512];
                    WebCoreDriver.WebCoreGetTitle(tBuf, tBuf.Length);
                    title = Encoding.UTF8.GetString(tBuf).TrimEnd('\0');
                    byte[] uBuf = new byte[1024];
                    WebCoreDriver.WebCoreGetUrl(uBuf, uBuf.Length);
                    string newUrl = Encoding.UTF8.GetString(uBuf).TrimEnd('\0');
                    if (!string.IsNullOrEmpty(newUrl) && newUrl != prevUrl) navUrl = newUrl;
                    links = ExtractLinks();
                }
                // One line per move, always: the engine's answer (rc), whether a frame actually
                // arrived, where the list now stands, and the URL the engine reports. A `back` that
                // did nothing and a `back` that moved to the same URL look identical on screen.
                AutoLog("history " + (delta < 0 ? "back" : "fwd") + " rc=" + rc
                        + " moved=" + (moved ? 1 : 0) + " can=" + canBack + "/" + canFwd
                        + " url=" + navUrl);

                int rcCopy = rc;
                bool movedCopy = moved;
                string titleW = title;
                string navW = string.IsNullOrEmpty(navUrl) ? null : navUrl;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _interacting = false;
                        if (_loadWatchdog != null) _loadWatchdog.Stop();
                        if (movedCopy)
                        {
                            ApplyEngineFrame(rgba, titleW, navW, links, delta);
                            CloseKeyboard();
                            UpdateNavButtons();
                            SetLoading(false);
                        }
                        else
                        {
                            SetLoading(false);
                            if (rcCopy == -12 || rcCopy == -16)
                            {
                                // The session went away under us; the engine half of history is gone
                                // with it. Say so -- otherwise Back silently turns into a URL load.
                                _sessionActive = false;
                                RefreshFab();
                            }
                            NavigateHostHistory(delta);
                        }
                    });
                }
                catch { }
            });
        }

        void OnHome(object s, RoutedEventArgs e) => NavigateTo(_homeUrl, true);

        // What Enter on the address box means, in one place: the key handler and the `enter` autocmd
        // verb both land here, so the scripted run measures the same thing the keyboard does.
        void CommitUrlBox()
        {
            HideSuggestions();
            NavigateTo(NormalizeUrl(UrlBox?.Text), true);
        }

        void OnUrlKeyDown(object s, KeyRoutedEventArgs e)
        {
            // Enter must dismiss the dropdown itself. It used to navigate without touching it, so
            // the page loaded UNDERNEATH a list that stayed on screen -- measured on the bench
            // 2026-10-03: typing dzen.ru and pressing Enter left the panel covering the content area
            // with the page already loaded behind it, and the only way out was to click the page.
            // The Go button path (OnUrlAction) always hid it, which is why this survived: the two
            // ways into a navigation disagreed about whether the dropdown was theirs to close.
            if (e.Key == VirtualKey.Enter)
            {
                CommitUrlBox();
                e.Handled = true;
            }
            else if (e.Key == VirtualKey.Escape)
            {
                HideSuggestions();
                e.Handled = true;
            }
        }

        void OnMenu(object s, RoutedEventArgs e) => ShowActionMenu();

        void OnUrlAction(object s, RoutedEventArgs e)
        {
            if (_loading)
            {
                ++_opSeq;
                _interacting = false;
                if (_loadWatchdog != null) _loadWatchdog.Stop();
                WebEngine.Instance.Post(() => { try { WebCoreDriver.WebCoreCloseSession(); } catch { } });
                _sessionActive = false;
                RefreshFab();
                SetLoading(false);
                if (TitleText != null) TitleText.Text = Str.Get("Cancelled");
                return;
            }
            string boxText = UrlBox?.Text ?? "";
            bool pendingEdit = (_currentUrl == "about:home") ? !string.IsNullOrEmpty(boxText) : (boxText != _currentUrl);
            HideSuggestions();
            if (pendingEdit) NavigateTo(NormalizeUrl(UrlBox?.Text), true);
            else Reload();
        }

        void Reload()
        {
            if (string.IsNullOrEmpty(_currentUrl) || _currentUrl == "about:home")
                NavigateTo("about:home", false);
            else
                NavigateTo(_currentUrl, false);
        }

        void UpdateUrlActionGlyph()
        {
            if (UrlActionBtn == null) return;
            if (_loading) { UrlActionBtn.Content = "\x2715"; return; }
            string boxText = UrlBox?.Text ?? "";
            bool pendingEdit = (_currentUrl == "about:home") ? !string.IsNullOrEmpty(boxText) : (boxText != _currentUrl);
            UrlActionBtn.Content = pendingEdit ? "\x2192" : "\x21BB";
        }

        void UpdateLockIcon()
        {
            if (LockIcon == null) return;
            string u = _currentUrl ?? "";
            if (string.IsNullOrEmpty(u) || u == "about:home" || u.StartsWith("about:"))
            {
                LockIcon.Text = "";
            }
            else if (u.StartsWith("https://"))
            {
                LockIcon.Text = "\xE72E";
                LockIcon.Foreground = new SolidColorBrush(Color.FromArgb(255, 0x5C, 0xB8, 0x5C));
            }
            else if (u.StartsWith("http://"))
            {
                LockIcon.Text = "\xE7BA";
                LockIcon.Foreground = new SolidColorBrush(Color.FromArgb(255, 0xE0, 0xA0, 0x30));
            }
            else
            {
                LockIcon.Text = "";
            }
        }

        void OnUrlChanged(object sender, TextChangedEventArgs e)
        {
            UpdateUrlActionGlyph();
            string q = UrlBox?.Text ?? "";
            // A shell write is not typing. `_urlSyncing` alone is not enough: measured on the bench
            // 2026-10-03 the TextChanged for a committed URL is delivered AFTER the flag has been
            // cleared, so the handler saw `_urlFocused == true`, matched the URL against the history
            // the commit had just appended, and reopened the dropdown -- one row reading
            // `dzen - поиск / cn.bing.com/search?q=dzen` under a page that was already loading.
            // The remembered string is what survives the delay. See SetUrlBoxText.
            if (_urlSyncing || q == _lastSyncedText) { HideSuggestions(); return; }
            if (!_urlFocused) { HideSuggestions(); return; }
            if (string.IsNullOrEmpty(q)) { HideSuggestions(); return; }
            ShowSuggestions(q);
        }

        // Every place the SHELL puts text in the address box goes through here. `_lastSyncedText`
        // is the part that survives an asynchronous TextChanged; `_urlSyncing` stays for the
        // synchronous case, which is the common one.
        string _lastSyncedText;
        void SetUrlBoxText(string t)
        {
            _urlSyncing = true;
            _lastSyncedText = t;
            if (UrlBox != null) UrlBox.Text = t;
            _urlSyncing = false;
        }

        void OnUrlGotFocus(object s, RoutedEventArgs e) { _urlFocused = true; }
        void OnUrlLostFocus(object s, RoutedEventArgs e) { _urlFocused = false; }

        // ===== Suggestions =====
        void ShowSuggestions(string query)
        {
            // The one gate that matters: ShowSuggestions is the only function that can put the
            // panel on screen, so the switch is read here and nowhere else. HideSuggestions() (not
            // a bare return) is deliberate -- the box may already be open from a keystroke sent
            // before the setting was turned off in the sheet.
            if (!_suggestOn) { HideSuggestions(); return; }
            if (SuggestPanel == null || SuggestList == null) return;
            SuggestList.Children.Clear();
            string ql = query.ToLowerInvariant();
            var matches = new List<Entry>();
            var seen = new List<string>();

            void Consider(List<Entry> src)
            {
                foreach (var e in src)
                {
                    if (matches.Count >= 8) break;
                    string ul = e.Url.ToLowerInvariant(), tl = e.Title.ToLowerInvariant();
                    if (!ul.Contains(ql) && !tl.Contains(ql)) continue;
                    if (seen.Contains(e.Url)) continue;
                    seen.Add(e.Url);
                    matches.Add(e);
                }
            }
            Consider(_bookmarks);
            Consider(_historyList);
            if (matches.Count == 0) { HideSuggestions(); return; }

            // The desktop ceiling in XAML (250 DIP) is most of a 360x640 phone window, and the
            // phone is the shipping target. Clamp to a third of the content area, so the list is
            // never more than a minority of the page on any window size. 120 DIP is the floor: two
            // rows plus a scroll affordance is the smallest list still worth opening.
            if (SuggestScroll != null && ContentArea != null && ContentArea.ActualHeight > 0)
                SuggestScroll.MaxHeight = Math.Max(120, Math.Min(250, ContentArea.ActualHeight * 0.34));

            foreach (var e in matches)
            {
                string u = e.Url;
                // `DisplayUrl` for the secondary line (and for the primary when there is no title):
                // a list of eight `https://` prefixes is eight wasted columns on a 360-DIP phone,
                // which is the shipping target.
                var row = MakeRow(string.IsNullOrEmpty(e.Title) ? DisplayUrl(e.Url) : e.Title,
                                  DisplayUrl(e.Url), Colors.White);
                var btn = new Button
                {
                    Background = new SolidColorBrush(Colors.Transparent),
                    BorderThickness = new Thickness(0),
                    Padding = new Thickness(0),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    HorizontalContentAlignment = HorizontalAlignment.Stretch,
                    Content = row
                };
                btn.Click += (ss, ee) =>
                {
                    HideSuggestions();
                    SetUrlBoxText(u);
                    NavigateTo(u, true);
                };
                SuggestList.Children.Add(btn);
            }
            SuggestPanel.Visibility = Visibility.Visible;
        }

        void HideSuggestions()
        {
            if (SuggestPanel != null) SuggestPanel.Visibility = Visibility.Collapsed;
        }

        // ===== Action menu =====
        void ShowActionMenu()
        {
            HideSuggestions();
            UpdateActionLabels();
            ActionMenu.Visibility = Visibility.Visible;
            StopLiveMode();
        }

        void HideActionMenu()
        {
            ActionMenu.Visibility = Visibility.Collapsed;
            StartLiveMode();
        }

        void OnActionScrimTap(object s, TappedRoutedEventArgs e) => HideActionMenu();
        void OnSheetTap(object s, TappedRoutedEventArgs e) => e.Handled = true;

        void OnAction(object sender, RoutedEventArgs e)
        {
            string t = "";
            if (sender is Button btn) t = btn.Tag?.ToString() ?? "";
            HideActionMenu();
            switch (t)
            {
                case "reload": Reload(); break;
                case "share": DoShare(); break;
                case "copylink": DoCopyLink(); break;
                case "bookmark": ToggleBookmark(); break;
                case "newtab": NewTab(); break;
                case "home": NavigateTo(_homeUrl, true); break;
                case "ua": DoToggleUA(); break;
                case "find": ShowFindBar(); break;
                case "download":
                    if (!string.IsNullOrEmpty(_currentUrl) && _currentUrl != "about:home")
                        StartDownload(_currentUrl);
                    break;
                case "bookmarks": ShowDrawer(DrawerTab.Favorites); break;
                case "history": ShowDrawer(DrawerTab.History); break;
                case "downloads": ShowDrawer(DrawerTab.Downloads); break;
                case "settings": ShowSettings(); break;
            }
        }

        void ToggleBookmark()
        {
            if (string.IsNullOrEmpty(_currentUrl) || _currentUrl == "about:home") return;
            if (IsBookmarked(_currentUrl))
            {
                _bookmarks.RemoveAll(e => e.Url == _currentUrl);
                if (TitleText != null) TitleText.Text = Str.Get("BookmarkRemoved");
            }
            else
            {
                _bookmarks.Insert(0, new Entry { Url = _currentUrl, Title = string.IsNullOrEmpty(_currentTitle) ? _currentUrl : _currentTitle });
                if (TitleText != null) TitleText.Text = Str.Get("Bookmarked");
            }
            SaveBookmarks();
            UpdateActionLabels();
            if (Drawer.Visibility == Visibility.Visible && _tab == DrawerTab.Favorites)
                RebuildDrawerList();
        }

        void DoShare()
        {
            if (string.IsNullOrEmpty(_currentUrl) || _currentUrl == "about:home")
            {
                if (TitleText != null) TitleText.Text = Str.Get("NothingToShare");
                return;
            }
            try { DataTransferManager.ShowShareUI(); } catch { }
        }

        void DoCopyLink()
        {
            if (string.IsNullOrEmpty(_currentUrl) || _currentUrl == "about:home") return;
            try
            {
                var dp = new DataPackage();
                dp.SetText(_currentUrl);
                Clipboard.SetContent(dp);
                if (TitleText != null) TitleText.Text = Str.Get("LinkCopied");
            }
            catch { }
        }

        void DoToggleUA()
        {
            _uaMobile = !_uaMobile;
            UpdateActionLabels();
            int mobile = _uaMobile ? 1 : 0;
            WebEngine.Instance.Post(() => { try { WebCoreDriver.WebCoreSetUserAgentMobile(mobile); } catch { } });
            if (!string.IsNullOrEmpty(_currentUrl) && _currentUrl != "about:home")
                NavigateTo(_currentUrl, false);
        }

        // ===== Drawer =====
        void OnDrawerClose(object s, RoutedEventArgs e) => HideDrawer();
        void OnTabFav(object s, RoutedEventArgs e) => ShowDrawer(DrawerTab.Favorites);
        void OnTabHist(object s, RoutedEventArgs e) => ShowDrawer(DrawerTab.History);
        void OnTabDl(object s, RoutedEventArgs e) => ShowDrawer(DrawerTab.Downloads);

        void OnPrimaryAction(object sender, RoutedEventArgs e)
        {
            if (_tab == DrawerTab.Favorites) ToggleBookmark();
            else if (_tab == DrawerTab.History) { _historyList.Clear(); SaveHistory(); RebuildDrawerList(); }
            else { if (!string.IsNullOrEmpty(_currentUrl) && _currentUrl != "about:home") StartDownload(_currentUrl); }
        }

        void ShowDrawer(DrawerTab tab)
        {
            _tab = tab;
            HideSuggestions();
            Drawer.Visibility = Visibility.Visible;
            // Title and the right-hand primary button both depend on which tab this is, so they
            // are set together (and retranslated with the rest if the language changed).
            UpdateDrawerChrome();
            RebuildDrawerList();
            StopLiveMode();
        }

        void HideDrawer()
        {
            Drawer.Visibility = Visibility.Collapsed;
            StartLiveMode();
        }

        static Border MakeRow(string title, string sub, Color titleColor)
        {
            // `TextWrapping` is set EXPLICITLY and it is not decoration. `MaxLines` is documented as
            // a cap on wrapped lines, but the runtime takes it as a licence to wrap: measured on the
            // bench 2026-10-03, a suggestion row with `MaxLines = 1` and
            // `TextTrimming = CharacterEllipsis` rendered a long page title as TWO full lines with no
            // ellipsis at all, so eight history entries filled the whole content area -- reported as
            // "гигантское паразитное окно с кривым показом истории". NoWrap plus trimming gives the
            // one clipped line that was always intended.
            var sp = new StackPanel { Margin = new Thickness(14, 4, 14, 4) };
            var t = new TextBlock
            {
                Text = title, FontSize = 15,
                Foreground = new SolidColorBrush(titleColor),
                TextWrapping = TextWrapping.NoWrap,
                TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1
            };
            var u = new TextBlock
            {
                Text = sub, FontSize = 12,
                Foreground = new SolidColorBrush(Color.FromArgb(255, 0x80, 0x86, 0x8b)),
                TextWrapping = TextWrapping.NoWrap,
                TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1,
                Margin = new Thickness(0, 1, 0, 0)
            };
            sp.Children.Add(t); sp.Children.Add(u);
            return new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(255, 0x2B, 0x2D, 0x31)),
                CornerRadius = new CornerRadius(8),
                Margin = new Thickness(0, 0, 0, 4),
                Child = sp
            };
        }

        // The address as it is worth SHOWING: the scheme is noise in a list where every entry has
        // one. `about:` and `file:` keep theirs, because there the scheme is the whole address.
        static string DisplayUrl(string url)
        {
            if (string.IsNullOrEmpty(url)) return "";
            if (url.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) return url.Substring(8);
            if (url.StartsWith("http://", StringComparison.OrdinalIgnoreCase)) return url.Substring(7);
            return url;
        }

        void RebuildDrawerList()
        {
            DrawerList.Children.Clear();
            List<Entry> list = null;
            if (_tab == DrawerTab.Favorites) list = _bookmarks;
            else if (_tab == DrawerTab.History) list = _historyList;
            else list = _downloads;

            if (list.Count == 0)
            {
                string emptyText = Str.Get(_tab == DrawerTab.Favorites ? "NoBookmarks"
                                         : _tab == DrawerTab.History ? "NoHistory" : "NoDownloads");
                DrawerList.Children.Add(new TextBlock
                {
                    Text = emptyText,
                    Foreground = new SolidColorBrush(Color.FromArgb(255, 0x80, 0x86, 0x8b)),
                    FontSize = 18, Margin = new Thickness(14, 20, 0, 0)
                });
                return;
            }

            bool isDownloads = (_tab == DrawerTab.Downloads);
            bool isFav = (_tab == DrawerTab.Favorites);
            for (int i = 0; i < list.Count; i++)
            {
                var e = list[i];
                string titleS = string.IsNullOrEmpty(e.Title) ? e.Url : e.Title;
                string subS = isDownloads ? $"{e.Extra}  \u00b7  {e.Url}" : e.Url;
                var row = MakeRow(titleS, subS, Color.FromArgb(255, 0xF0, 0xF0, 0xF0));
                if (isDownloads) { DrawerList.Children.Add(row); continue; }

                string u = e.Url;
                var btn = new Button
                {
                    Background = new SolidColorBrush(Colors.Transparent),
                    BorderThickness = new Thickness(0), Padding = new Thickness(0),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    HorizontalContentAlignment = HorizontalAlignment.Stretch,
                    Content = row
                };
                btn.Click += (ss, ee) => { HideDrawer(); NavigateTo(u, true); };
                DrawerList.Children.Add(btn);

                if (isFav)
                {
                    string favUrl = u;
                    var del = new Button
                    {
                        Content = Str.Get("RemoveBookmark"), FontSize = 15,
                        Background = new SolidColorBrush(Colors.Transparent),
                        Foreground = new SolidColorBrush(Color.FromArgb(255, 0xD9, 0x30, 0x25)),
                        BorderThickness = new Thickness(0),
                        Margin = new Thickness(8, -6, 0, 8)
                    };
                    del.Click += (ss, ee) =>
                    {
                        _bookmarks.RemoveAll(x => x.Url == favUrl);
                        SaveBookmarks(); RebuildDrawerList();
                    };
                    DrawerList.Children.Add(del);
                }
            }
        }

        // ===== Settings =====
        void ShowSettings()
        {
            HideActionMenu();
            if (SetSearchCombo != null) SetSearchCombo.SelectedIndex = _setSearch;
            if (SetHomeBox != null) SetHomeBox.Text = (_homeUrl == "about:home") ? "" : _homeUrl;
            if (SetUaSwitch != null) SetUaSwitch.IsOn = _setUaDesktop;
            if (SetTabModeSwitch != null) SetTabModeSwitch.IsOn = (_tabMode == 1);
            if (SetZoomSlider != null) SetZoomSlider.Value = _defaultZoom;
            if (SetZoomLabel != null) SetZoomLabel.Text = _defaultZoom + "%";
            if (SetGpuSwitch != null) SetGpuSwitch.IsOn = _gpuDefault;
            if (SetUaCustomBox != null) SetUaCustomBox.Text = _uaCustom;
            if (SetLangCombo != null) SetLangCombo.SelectedIndex = _uiLang;
            if (SetFabSwitch != null) SetFabSwitch.IsOn = _showFab;
            if (SetFitSwitch != null) SetFitSwitch.IsOn = _fitWindow;
            if (SetSuggestSwitch != null) SetSuggestSwitch.IsOn = _suggestOn;
            if (SetHeadlessCombo != null) SetHeadlessCombo.SelectedIndex = _headlessMode;
            // Captions, pivot headers and the version line all come from one pass -- see
            // RetranslateUi(). It used to be three hand-written Str.Get calls here, which is
            // how the rest of the page ended up with captions nobody had ever set.
            RetranslateUi();
            SettingsPage.Visibility = Visibility.Visible;
            StopLiveMode();
        }

        void HideSettings()
        {
            if (SetSearchCombo != null && SetSearchCombo.SelectedIndex >= 0) _setSearch = SetSearchCombo.SelectedIndex;
            if (SetHomeBox != null)
            {
                string h = (SetHomeBox.Text ?? "").Trim();
                if (string.IsNullOrEmpty(h) || h == "about:home") _homeUrl = "about:home";
                else { if (!h.StartsWith("http") && !h.StartsWith("about:")) h = "https://" + h; _homeUrl = h; }
            }
            if (SetUaSwitch != null) _setUaDesktop = SetUaSwitch.IsOn;
            if (SetTabModeSwitch != null) _tabMode = SetTabModeSwitch.IsOn ? 1 : 0;
            if (SetZoomSlider != null) _defaultZoom = (int)(SetZoomSlider.Value + 0.5);
            if (SetGpuSwitch != null) _gpuDefault = SetGpuSwitch.IsOn;
            if (SetUaCustomBox != null) _uaCustom = (SetUaCustomBox.Text ?? "").Trim();
            if (SetLangCombo != null && SetLangCombo.SelectedIndex >= 0) _uiLang = SetLangCombo.SelectedIndex;
            if (SetLogSwitch != null) _traceOn = SetLogSwitch.IsOn;
            if (SetFabSwitch != null) _showFab = SetFabSwitch.IsOn;
            if (SetSuggestSwitch != null) _suggestOn = SetSuggestSwitch.IsOn;
            if (SetHeadlessCombo != null && SetHeadlessCombo.SelectedIndex >= 0) _headlessMode = SetHeadlessCombo.SelectedIndex;
            bool wantFit = (SetFitSwitch != null) && SetFitSwitch.IsOn;
            ApplySettings();
            SaveSettings();
            // The switch takes effect here rather than at the next navigation: the page behind the
            // Settings sheet is already loaded and will not fire the branch that used to own this.
            RefreshFab();
            // Fitting is not a flag it reads later -- it is a resize now, and turning it off is a
            // resize back to 720x1080. SetFitWindow is a no-op when nothing changed.
            SetFitWindow(wantFit);
            SettingsPage.Visibility = Visibility.Collapsed;
            // The home page is HTML built by BuildHomeHtml(), so the ordinary XAML repaint cannot
            // reach it: a language chosen while its document is on screen leaves the hero and the
            // section heading in the old language. Rebuild it once, and only when it is what the
            // user is looking at.
            if (_currentUrl == "about:home" && _homeHtmlLang != Str.Lang)
                NavigateTo(_homeUrl, true);
            StartLiveMode();
        }

        void OnLogSwitchToggled(object sender, RoutedEventArgs e)
        {
            if (SetLogSwitch == null) return;
            _traceOn = SetLogSwitch.IsOn;
            UpdateLogHint();
            if (_traceOn) Crumb("logging enabled from settings");
        }

        void UpdateLogHint()
        {
            if (SetLogHint == null) return;
            SetLogHint.Text = Str.Get(_traceOn ? "LoggingOnHint" : "LoggingOffHint");
        }

        // The UA switch applies the moment it moves, like the language picker, instead of waiting
        // for Settings to close. Two reasons. It is what a person expects from a switch that sits
        // next to one which already behaves that way; and it makes the scripted `ua` verb a
        // measurement of the shipping path rather than of a private field -- the controller sets
        // the real control's IsOn and nothing else.
        //
        // It does not re-navigate: the session keeps the document it has. The switch changes the UA
        // of the *next* request, so a test that wants the other layout has to `ua` and then `goto`.
        void OnUaSwitchToggled(object sender, RoutedEventArgs e)
        {
            if (SetUaSwitch == null) return;
            _setUaDesktop = SetUaSwitch.IsOn;
            ApplySettings();
            Crumb("ua -> " + (_setUaDesktop ? "desktop" : "mobile"));
        }

        void OnSettingsBack(object s, RoutedEventArgs e) => HideSettings();

        void OnZoomChanged(object s, RangeBaseValueChangedEventArgs e)
        {
            if (SetZoomLabel != null) SetZoomLabel.Text = ((int)(e.NewValue + 0.5)) + "%";
        }

        void OnLangChanged(object s, SelectionChangedEventArgs e)
        {
            if (SetLangCombo != null && SetLangCombo.SelectedIndex >= 0)
            {
                _uiLang = SetLangCombo.SelectedIndex;
                Str.SetLang(_uiLang);
                // This is the whole point of the pass below: the user just changed the language
                // of a UI that is already on screen. Storing the index and saving it (which is
                // all this handler used to do) left every caption in the previous language, and
                // the .resw route could not have repainted them anyway -- ResourceLoader is bound
                // to the view language, en-US here, and ignores this setting entirely.
                RetranslateUi();
                // The status line is translated too ("Home", "Loading", "Load Failed"), so when it
                // shows one of those and not a page title it has to follow the switch.
                if (TitleText != null && _currentUrl == "about:home")
                    TitleText.Text = Str.Get("Home");
                SaveSettings();
            }
        }

        // ===== Localisation =====
        //
        // Apotheosis 2026-10-03. One idempotent pass over everything the user can read, called
        // from the constructor, from OnLangChanged, and from every overlay's Show* method --
        // most of these captions are shared by several overlays, and whichever one happened to
        // be open when the language changed has to come back correct.
        //
        // The XAML deliberately carries x:Name and no longer x:Uid. Two measured reasons: an
        // x:Uid lookup asks for "SettingsTitle.Text" while the .resw files here define the bare
        // name "SettingsTitle", so every x:Uid caption rendered EMPTY (the settings page shipped
        // with uncaptioned controls); and the .resw is bound to the view language, so it can
        // never be repainted at runtime. Str.cs now owns the strings. See its header.
        //
        // The null guards are not defensive noise: a control inside the unselected Pivot item may
        // be absent, and a stale caption is a far better outcome than a crash in a repaint pass.
        static void Loc(TextBlock t, string key) { if (t != null) t.Text = Str.Get(key); }
        static void Loc(Button b, string key) { if (b != null) b.Content = Str.Get(key); }
        // A ToggleSwitch's On/Off captions come from the OS, not from the app: they follow the
        // system language. A Russian UI on an English system showed "Откл." under an English
        // header, so a switch is given all three strings here.
        static void LocSwitch(ToggleSwitch t, string headerKey)
        {
            if (t == null) return;
            t.Header = Str.Get(headerKey);
            t.OnContent = Str.Get("On");
            t.OffContent = Str.Get("Off");
        }

        internal void RetranslateUi()
        {
            // Values that live in the same pass as the captions. The log switch is here rather
            // than in ShowSettings() because removing it from there while moving the labels was a
            // real regression: the caption flipped to the current language and the switch itself
            // stayed off, so the page said "Verbose logging / Off" next to a hint that tracing was
            // on. Anything the repaint pass owns has to be set here.
            if (SetLogSwitch != null) SetLogSwitch.IsOn = _traceOn;
            UpdateLogHint();

            // Address / find fields: the placeholder is a caption too.
            if (UrlBox != null) UrlBox.PlaceholderText = Str.Get("UrlPlaceholder");
            if (FindBox != null) FindBox.PlaceholderText = Str.Get("FindInPage");

            // Action sheet -- the four icon buttons, then the rows.
            Loc(BackLabel, "Back");
            Loc(FwdLabel, "Forward");
            Loc(ReloadLabel, "Reload");
            Loc(NewTabLabel, "NewTab");
            Loc(HomeLabel, "Home");
            Loc(FindInPageLabel, "FindInPageAction");
            Loc(ShareLabel, "Share");
            Loc(CopyLinkLabel, "CopyLink");
            Loc(DownloadLabel, "DownloadPage");
            Loc(BookmarksLabel, "Bookmarks");
            Loc(HistoryLabel, "History");
            Loc(DownloadsLabel, "Downloads");
            Loc(SettingsLabel, "Settings");
            UpdateActionLabels();

            // Drawer.
            Loc(TabFav, "Favorites");
            Loc(TabHist, "History");
            Loc(TabDl, "Downloads");
            UpdateDrawerChrome();

            // Settings, General.
            Loc(SettingsTitle, "Settings");
            Loc(SettingsDefaultSearch, "DefaultSearchEngine");
            Loc(SettingsLanguage, "InterfaceLanguage");
            Loc(SettingsHomepage, "HomepageUrl");
            LocSwitch(SetUaSwitch, "RequestDesktopSite");
            Loc(SettingsCustomUA, "CustomUserAgent");
            Loc(SettingsDefaultZoom, "DefaultZoom");
            LocSwitch(SetFabSwitch, "ShowScrollButtons");
            LocSwitch(SetFitSwitch, "FitPageToWindow");
            LocSwitch(SetSuggestSwitch, "AddressBarSuggestions");

            // Settings, Advanced.
            LocSwitch(SetTabModeSwitch, "ConcurrentTabs");
            Loc(SettingsHeadlessSection, "HeadlessSection");
            Loc(SetHeadlessHint, "HeadlessHint");
            if (SetHeadlessCombo != null && SetHeadlessCombo.Items.Count >= 3)
            {
                (SetHeadlessCombo.Items[0] as ComboBoxItem).Content = Str.Get("Off");
                (SetHeadlessCombo.Items[1] as ComboBoxItem).Content = Str.Get("HeadlessSummary");
                (SetHeadlessCombo.Items[2] as ComboBoxItem).Content = Str.Get("HeadlessSpeak");
            }
            Loc(SettingsGpuSection, "GPU");
            LocSwitch(SetGpuSwitch, "EnableGPU");
            Loc(SetGpuNowBtn, "EnableGPUNow");
            LocSwitch(SetLogSwitch, "VerboseLogging");
            Loc(SettingsClearData, "ClearData");
            Loc(SetClearHistBtn, "ClearHistory");
            Loc(SetClearFavBtn, "ClearAllBookmarks");
            Loc(SetClearDlBtn, "ClearDownloads");
            Loc(SettingsDiagnostics, "Diagnostics");
            Loc(SetExportBtn, "ExportDebugLogs");
            Loc(SettingsAboutUpdate, "AboutUpdate");
            Loc(SetCheckUpdateBtn, "CheckForUpdate");
            Loc(SettingsFooter, "Footer");
            if (SettingsGeneralItem != null) SettingsGeneralItem.Header = Str.Get("General");
            if (SettingsAdvancedItem != null) SettingsAdvancedItem.Header = Str.Get("Advanced");
            if (VersionText != null)
            {
                var pv = Package.Current.Id.Version;
                VersionText.Text = $"{Str.Get("Version")} {pv.Major}.{pv.Minor}.{pv.Build}.{pv.Revision}";
            }

            // Tab switcher.
            Loc(TabSwitcherTitle, "Tabs");
            Loc(TabSwitcherDone, "Done");
            Loc(NewTabButton, "NewTab");
        }

        // The two captions that depend on state rather than on the language alone.
        void UpdateActionLabels()
        {
            bool marked = !string.IsNullOrEmpty(_currentUrl) && IsBookmarked(_currentUrl);
            if (ActFavLabel != null) ActFavLabel.Text = Str.Get(marked ? "Bookmarked" : "Bookmark");
            string ua = Str.Get(_uaMobile ? "MobileSite" : "DesktopSite");
            if (ActUaLabel != null) ActUaLabel.Text = ua;
            if (UaBtn != null) UaBtn.Content = ua;
        }

        void UpdateDrawerChrome()
        {
            if (DrawerTitle != null)
            {
                DrawerTitle.Text = _tab == DrawerTab.Favorites ? Str.Get("Bookmarks")
                                 : _tab == DrawerTab.History ? Str.Get("History")
                                 : Str.Get("Downloads");
            }
            if (ActionBtn == null) return;
            if (_tab == DrawerTab.Favorites)
            {
                bool marked = !string.IsNullOrEmpty(_currentUrl) && IsBookmarked(_currentUrl);
                ActionBtn.Content = Str.Get(marked ? "RemoveBookmark" : "BookmarkThisPage");
            }
            else if (_tab == DrawerTab.History)
                ActionBtn.Content = Str.Get("Clear");
            else
                ActionBtn.Content = Str.Get("DownloadThisPage");
        }

        void OnSettingsBtn(object sender, RoutedEventArgs e)
        {
            string t = (sender as Button)?.Tag?.ToString() ?? "";
            switch (t)
            {
                case "clearhist":
                    _historyList.Clear(); SaveHistory();
                    if (TitleText != null) TitleText.Text = Str.Get("HistoryCleared");
                    break;
                case "clearfav":
                    _bookmarks.Clear(); SaveBookmarks();
                    if (TitleText != null) TitleText.Text = Str.Get("BookmarksCleared");
                    break;
                case "cleardl":
                    _downloads.Clear(); SaveDownloads();
                    if (TitleText != null) TitleText.Text = Str.Get("DownloadsCleared");
                    break;
                case "export": 
                    ExportDebug(); 
                    break;
                case "gpu": 
                    HideSettings(); 
                    OnToggleGpu(null, null); 
                    break;
                case "ua": 
                    HideSettings(); 
                    OnToggleUA(null, null); 
                    break;
                case "checkupdate": 
                    CheckForUpdate(true); 
                    break;
            }
        }

        private void OnToggleUA(object value1, object value2)
        {
            //TODO
        }

        async void ExportDebug()
        {
            string d = LocalStateDir();
            var sb = new StringBuilder($"=== Apotheosis Debug Report ===\nharness / WebCore 2.52.4 / {Str.Get("Footer")}\n\n");
            string[] names = { "stage.txt", "gpuinit.txt", "gpuresult.txt", "layertree.txt", "autodump.txt", "imedebug.txt", "jitresult.txt", "diag.txt" };
            foreach (var fn in names)
            {
                string fp = Path.Combine(d, fn);
                if (File.Exists(fp))
                {
                    sb.AppendLine($"---------- {fn} ----------");
                    sb.AppendLine(File.ReadAllText(fp));
                    sb.AppendLine();
                }
            }
            try { File.WriteAllText(Path.Combine(d, "debug-report.txt"), sb.ToString()); } catch { }

            try
            {
                var picker = new FileSavePicker();
                picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;
                picker.SuggestedFileName = "apotheosis-debug";
                picker.FileTypeChoices.Add("Text file", new List<string> { ".txt" });
                var file = await picker.PickSaveFileAsync();
                if (file != null)
                    await FileIO.WriteTextAsync(file, sb.ToString());
                if (TitleText != null) TitleText.Text = "Export done";
            }
            catch
            {
                if (TitleText != null) TitleText.Text = "Export Failed";
            }
        }

        // ===== Find in page =====
        void ShowFindBar()
        {
            if (!_sessionActive) { if (TitleText != null) TitleText.Text = "Cannot Find in This Page"; return; }
            HideActionMenu();
            HideSuggestions();
            FindBar.Visibility = Visibility.Visible;
            if (FindCount != null) FindCount.Text = "";
            if (FindBox != null) FindBox.Text = "";
            FindBox?.Focus(FocusState.Programmatic);
        }

        void OnFindChanged(object s, TextChangedEventArgs e) => DoFind(0);
        void OnFindKeyDown(object s, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter) { e.Handled = true; DoFind(1); }
        }
        void OnFindNext(object s, RoutedEventArgs e) => DoFind(1);
        void OnFindPrev(object s, RoutedEventArgs e) => DoFind(2);

        void OnFindClose(object s, RoutedEventArgs e)
        {
            FindBar.Visibility = Visibility.Collapsed;
            if (FindBox != null) FindBox.Text = "";
        }

        void DoFind(int mode)
        {
            if (!_sessionActive) { if (FindCount != null) FindCount.Text = ""; return; }
            if (_loading || _interacting) return;
            string query = FindBox?.Text ?? "";
            bool clear = (mode == 0 && string.IsNullOrEmpty(query));
            _interacting = true;
            SetLoading(true);
            if (_loadWatchdog != null) _loadWatchdog.Start();
            string q = query;
            bool present = _gpuPresent;
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try
                {
                    if (clear) rc = WebCoreDriver.WebCoreFindClear(h.AddrOfPinnedObject());
                    else if (mode == 0) rc = WebCoreDriver.WebCoreFindString(q, 0, 1, h.AddrOfPinnedObject());
                    else rc = WebCoreDriver.WebCoreFindNext(mode == 1 ? 1 : 0, h.AddrOfPinnedObject());
                }
                finally { h.Free(); }
                int rcCopy = rc; int modeCopy = mode; bool clearCopy = clear;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _interacting = false;
                        if (_loadWatchdog != null) _loadWatchdog.Stop();
                        SetLoading(false);
                        if (rcCopy < 0)
                        {
                            if (rcCopy == -12 || rcCopy == -14)
                            {
                                _sessionActive = false;
                                RefreshFab();
                            }
                            if (FindCount != null) FindCount.Text = "";
                            return;
                        }
                        if (!present) BlitToBitmap(rgba);
                        _lastFrameHash = 0;
                        if (clearCopy) { if (FindCount != null) FindCount.Text = ""; }
                        else if (modeCopy == 0) { if (FindCount != null) FindCount.Text = rcCopy > 0 ? $"{rcCopy} matches" : "No Results"; }
                        else { if (FindCount != null) FindCount.Text = rcCopy != 0 ? "" : "No More"; }
                    });
                }
                catch { }
            });
        }

        // ===== Tabs =====
        void UpdateTabCount()
        {
            if (TabCountText != null) TabCountText.Text = _tabs.Count.ToString();
        }

        void SaveActiveTab()
        {
            if (_activeTab < 0 || _activeTab >= _tabs.Count) return;
            var t = _tabs[_activeTab];
            t.NavStack = new List<string>(_navStack);
            t.NavIndex = _navIndex;
            t.CurrentUrl = string.IsNullOrEmpty(_currentUrl) ? "about:home" : _currentUrl;
            t.CurrentTitle = _currentTitle;
        }

        void RestoreTab(int i)
        {
            if (i < 0 || i >= _tabs.Count) return;
            _activeTab = i;
            var t = _tabs[i];
            _navStack = new List<string>(t.NavStack);
            _navIndex = t.NavIndex;
            _currentUrl = t.CurrentUrl;
            _currentTitle = t.CurrentTitle;
            // The zoom is not restored with the tab, on purpose: it is one user-level setting (see
            // CommitPageZoom), and the after-load re-apply re-arms the engine from it. A tab that
            // carried its own copy would be a second authority for the same number.
            ++_opSeq;
            _interacting = false;
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            _loading = false;
            UpdateNavButtons();
            UpdateLockIcon();
            SetUrlBoxText((_currentUrl == "about:home") ? "" : _currentUrl);
            NavigateTo(_currentUrl, false);
        }

        void NewTab()
        {
            SaveActiveTab();
            _tabs.Add(new Tab { CurrentUrl = _homeUrl });
            _activeTab = _tabs.Count - 1;
            ++_opSeq; _interacting = false;
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            _loading = false;
            _navStack.Clear(); _navIndex = -1;
            _currentUrl = ""; _currentTitle = "";
            UpdateTabCount();
            NavigateTo(_homeUrl, true);
        }

        void CloseTab(int i)
        {
            if (i < 0 || i >= _tabs.Count) return;
            bool wasActive = (i == _activeTab);
            _tabs.RemoveAt(i);
            if (_tabs.Count == 0)
            {
                _tabs.Add(new Tab { CurrentUrl = _homeUrl });
                _activeTab = 0;
                ++_opSeq; _interacting = false;
                if (_loadWatchdog != null) _loadWatchdog.Stop();
                _loading = false;
                _navStack.Clear(); _navIndex = -1;
                _currentUrl = ""; _currentTitle = "";
                UpdateTabCount();
                NavigateTo(_homeUrl, true);
                return;
            }
            if (_activeTab >= _tabs.Count) _activeTab = _tabs.Count - 1;
            else if (i < _activeTab) _activeTab--;
            UpdateTabCount();
            if (wasActive) RestoreTab(_activeTab);
        }

        void SwitchTab(int i)
        {
            if (i == _activeTab) return;
            SaveActiveTab();
            RestoreTab(i);
        }

        void OnTabs(object s, RoutedEventArgs e) => ShowTabSwitcher();
        void OnNewTab(object s, RoutedEventArgs e) { HideTabSwitcher(); NewTab(); }
        void OnTabSwitcherDone(object s, RoutedEventArgs e) => HideTabSwitcher();

        void ShowTabSwitcher()
        {
            HideActionMenu();
            HideSuggestions();
            SaveActiveTab();
            RebuildTabSwitcher();
            TabSwitcher.Visibility = Visibility.Visible;
            StopLiveMode();
        }

        void HideTabSwitcher()
        {
            TabSwitcher.Visibility = Visibility.Collapsed;
            StartLiveMode();
        }

        void RebuildTabSwitcher()
        {
            UpdateTabCount();
            if (TabSwitcherTitle != null) TabSwitcherTitle.Text = $"Tabs ({_tabs.Count})";
            TabList.Children.Clear();
            Color blue = Color.FromArgb(255, 0x3A, 0xA0, 0xFF);
            Color white = Color.FromArgb(255, 0xF0, 0xF0, 0xF0);
            for (int i = 0; i < _tabs.Count; i++)
            {
                int idx = i;
                var t = _tabs[i];
                bool active = (idx == _activeTab);
                string title = string.IsNullOrEmpty(t.CurrentTitle)
                    ? (t.CurrentUrl == "about:home" ? "Home" : t.CurrentUrl)
                    : t.CurrentTitle;
                string sub = (t.CurrentUrl == "about:home") ? "about:home" : t.CurrentUrl;

                var cell = new Grid { Margin = new Thickness(0, 0, 0, 8) };
                var sw = new Button
                {
                    Background = new SolidColorBrush(Color.FromArgb(255, 0x2B, 0x2D, 0x31)),
                    BorderThickness = active ? new Thickness(2) : new Thickness(0),
                    BorderBrush = new SolidColorBrush(blue),
                    Padding = new Thickness(0, 0, 40, 0),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    HorizontalContentAlignment = HorizontalAlignment.Stretch,
                    Content = MakeRow(title, sub, active ? blue : white)
                };
                sw.Click += (ss, ee) => { HideTabSwitcher(); SwitchTab(idx); };
                cell.Children.Add(sw);

                var cb = new Button
                {
                    Content = "\x2715",
                    Background = new SolidColorBrush(Colors.Transparent),
                    Foreground = new SolidColorBrush(Color.FromArgb(255, 0x9A, 0xA0, 0xA6)),
                    BorderThickness = new Thickness(0),
                    Width = 44, Height = 44,
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Center
                };
                cb.Click += (ss, ee) => { CloseTab(idx); RebuildTabSwitcher(); };
                cell.Children.Add(cb);

                TabList.Children.Add(cell);
            }
        }

        // ===== GPU =====
        void OnToggleGpu(object s, RoutedEventArgs e)
        {
            if (!_gpuOn) { HideDrawer(); EnableGpu(); return; }
            HideDrawer();
            CoreDispatcher disp = Dispatcher;
            WebEngine.Instance.Post(() =>
            {
                byte[] buf = new byte[65536];
                try { WebCoreDriver.WebCoreGpuLayerInfo(buf, buf.Length); } catch { }
                string info = Encoding.UTF8.GetString(buf).TrimEnd('\0');
                try { File.WriteAllText(Path.Combine(LocalStateDir(), "layertree.txt"), info); } catch { }
                string head = info.Split('\n').FirstOrDefault() ?? "";
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (TitleText != null) TitleText.Text = head;
                    });
                }
                catch { }
            });
        }

        void EnableGpu()
        {
            if (_gpuOn) return;
            CoreDispatcher disp = Dispatcher;
            GpuPanel.Visibility = Visibility.Visible;
            _gpuProps = new PropertySet();
            _gpuProps["EGLNativeWindowTypeProperty"] = GpuPanel;
            _gpuProps["EGLRenderSurfaceSizeProperty"] = PropertyValue.CreateSize(new Size(kW, kH));
            IntPtr win = Marshal.GetIUnknownForObject(_gpuProps);

            // Crash loop protection
            try { File.WriteAllText(Path.Combine(LocalStateDir(), "gpu-crash.flag"), "1"); } catch { }

            WebEngine.Instance.Post(() =>
            {
                int rc = -999;
                try { rc = WebCoreDriver.WebCoreGpuInit(win, kW, kH); } catch { rc = -1000; }
                try { File.WriteAllText(Path.Combine(LocalStateDir(), "gpuinit.txt"), $"WebCoreGpuInit(window) rc={rc}\n"); } catch { }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        try { File.Delete(Path.Combine(LocalStateDir(), "gpu-crash.flag")); } catch { }
                        if (rcCopy == 0)
                        {
                            _gpuOn = true;
                            _gpuPresent = true;
                            RenderImage.Visibility = Visibility.Collapsed;
                            if (GpuBtn != null)
                            {
                                GpuBtn.Content = "GPU\u00b7";
                                GpuBtn.Foreground = new SolidColorBrush(Colors.LimeGreen);
                            }
                            if (!string.IsNullOrEmpty(_currentUrl) && _currentUrl != "about:home")
                                NavigateTo(_currentUrl, false);
                        }
                        else
                        {
                            GpuPanel.Visibility = Visibility.Collapsed;
                            if (GpuBtn != null)
                            {
                                GpuBtn.Content = "GPU\u2717";
                                GpuBtn.Foreground = new SolidColorBrush(Colors.OrangeRed);
                            }
                        }
                    });
                }
                catch { }
            });
        }

        // ===== Downloads =====
        void StartDownload(string url)
        {
            string wurl = url ?? "";
            string fn = wurl;
            int q = fn.IndexOf('?'); if (q >= 0) fn = fn.Substring(0, q);
            int sl = fn.LastIndexOf('/');
            fn = (sl < 0) ? fn : fn.Substring(sl + 1);
            if (string.IsNullOrEmpty(fn) || !fn.Contains('.')) fn = "index.html";

            string dlDir = Path.Combine(LocalStateDir(), "Downloads");
            Directory.CreateDirectory(dlDir);
            string outPath = Path.Combine(dlDir, fn);
            if (File.Exists(outPath))
            {
                string stem = fn, ext = "";
                int dot = fn.LastIndexOf('.');
                if (dot >= 0) { stem = fn.Substring(0, dot); ext = fn.Substring(dot); }
                for (int i = 1; i < 1000; i++)
                {
                    string cand = $"{stem}({i}){ext}";
                    string candPath = Path.Combine(dlDir, cand);
                    if (!File.Exists(candPath)) { fn = cand; outPath = candPath; break; }
                }
            }

            if (TitleText != null) TitleText.Text = $"Downloading  {fn}";
            CoreDispatcher disp = Dispatcher;
            string u8url = wurl, u8out = outPath;
            string fnCopy = fn, urlCopy = wurl;
            int uiLang = _uiLang;

            Task.Run(() =>
            {
                int code = WebCoreDriver.WebCoreDownload(u8url, u8out);
                long sz = 0;
                try { if (File.Exists(u8out)) sz = new FileInfo(u8out).Length; } catch { }
                string status = (code >= 200 && code < 400)
                    ? $"Done  {sz / 1024} KB"
                    : $"Failed({code})";
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        _downloads.Insert(0, new Entry { Url = urlCopy, Title = fnCopy, Extra = status });
                        if (_downloads.Count > 100) _downloads.RemoveRange(100, _downloads.Count - 100);
                        SaveDownloads();
                        if (TitleText != null) TitleText.Text = $"{fnCopy}  {status}";
                        if (Drawer.Visibility == Visibility.Visible && _tab == DrawerTab.Downloads)
                            RebuildDrawerList();
                    });
                }
                catch { }
            });
        }

        // ===== Update checker =====
        // Apotheosis 2026-10-23: the checker watches THIS fork's releases, not the upstream the
        // original author publishes to. Upstream's "latest release" is a different product line;
        // comparing our build number against it produced a "new version available" prompt for a
        // package this tree cannot install, which is noise at best. The fork has no releases yet,
        // so check_latest answers 404 -- treated below as "no releases", not as a failure.
        const string UpdateApiUrl = "https://api.github.com/repos/mediaexplorer74/Project-Apotheosis/releases/latest";
        const string UpdatePageUrl = "https://github.com/mediaexplorer74/Project-Apotheosis/releases/latest";

        static bool ParseDottedVersion(string s, int[] outArr)
        {
            outArr[0] = outArr[1] = outArr[2] = outArr[3] = 0;
            int idx = 0; long cur = 0; bool any = false;
            for (int i = 0; i <= s.Length && idx < 4; i++)
            {
                if (i < s.Length && s[i] >= '0' && s[i] <= '9') { cur = cur * 10 + (s[i] - '0'); any = true; }
                else if (i == s.Length || s[i] == '.') { outArr[idx++] = (int)cur; cur = 0; if (i == s.Length) break; }
                else break;
            }
            return any;
        }

        void CheckForUpdate(bool manual)
        {
            if (_updateChecking) return;
            _updateChecking = true;
            if (manual && TitleText != null) TitleText.Text = "Checking for Update\u2026";

            var pv = Package.Current.Id.Version;
            int[] cur = { pv.Major, pv.Minor, pv.Build, pv.Revision };
            string dir = LocalStateDir();
            string jsonPath = Path.Combine(dir, "update.json");
            CoreDispatcher disp = Dispatcher;

            Task.Run(() =>
            {
                int rc = -1;
                string body = "";
                try { rc = WebCoreDriver.WebCoreDownload(UpdateApiUrl, jsonPath); } catch { }
                if (rc == 200 && File.Exists(jsonPath)) body = File.ReadAllText(jsonPath);

                string pick(string key)
                {
                    string pat = $"\"{key}\"";
                    int p = body.IndexOf(pat); if (p < 0) return "";
                    p = body.IndexOf(':', p + pat.Length); if (p < 0) return "";
                    int a = body.IndexOf('"', p); if (a < 0) return "";
                    int b = body.IndexOf('"', a + 1); if (b < 0) return "";
                    return body.Substring(a + 1, b - a - 1);
                }
                string tag = pick("tag_name");
                string page = pick("html_url");
                bool ok = (rc == 200 && !string.IsNullOrEmpty(tag));
                bool noReleases = (rc == 404);   // the fork exists but has published no release yet
                bool newer = false;
                string verStr = tag;
                if (!string.IsNullOrEmpty(verStr) && (verStr[0] == 'v' || verStr[0] == 'V')) verStr = verStr.Substring(1);
                if (ok)
                {
                    int[] rel = new int[4];
                    ParseDottedVersion(verStr, rel);
                    for (int i = 0; i < 4; i++) { if (rel[i] != cur[i]) { newer = rel[i] > cur[i]; break; } }
                }
                string tagW = tag, pageW = page;
                bool noReleasesW = noReleases;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, async () =>
                    {
                        _updateChecking = false;
                        if (!ok) { if (manual && TitleText != null) TitleText.Text = noReleasesW ? "No Releases Yet" : "Update Check Failed"; return; }
                        if (!newer) { if (manual && TitleText != null) TitleText.Text = "Already Up to Date " + tagW; return; }
                        if (TitleText != null) TitleText.Text = "New Version " + tagW;
                        string target = string.IsNullOrEmpty(pageW) ? UpdatePageUrl : pageW;
                        var dlg = new MessageDialog($"New version {tagW}\nGo to download?", "Update Available");
                        dlg.Commands.Add(new UICommand("Go to Download"));
                        dlg.Commands.Add(new UICommand("Later"));
                        dlg.DefaultCommandIndex = 0;
                        dlg.CancelCommandIndex = 1;
                        var chosen = await dlg.ShowAsync();
                        if (chosen.Label == "Go to Download")
                        {
                            if (SettingsPage.Visibility == Visibility.Visible) HideSettings();
                            NavigateTo(target, true);
                        }
                    });
                }
                catch { }
            });
        }

        // ===== Helpers =====
        static string MakeErrorHtml(string url, string err)
        {
            return "<html><head><meta charset='utf-8'></head>" +
                "<body style='margin:0;background:#fff;font-family:sans-serif'>" +
                "<div style='background:#d93025;color:#fff;padding:32px 24px'><h1 style='margin:0;font-size:38px'>Cannot access this page</h1></div>" +
                $"<div style='padding:24px;color:#333;font-size:24px'><p style='word-break:break-all;color:#1a73e8'>{HtmlEscape(url)}</p>" +
                $"<p style='color:#d93025;font-size:22px;word-break:break-all'>{HtmlEscape(err ?? "")}</p></div></body></html>";
        }

        static string BuildHomeHtml(List<Entry> bookmarks, List<Entry> history)
        {
            var tiles = new List<Entry>();
            var seen = new List<string>();
            void Add(List<Entry> src)
            {
                foreach (var e in src)
                {
                    if (tiles.Count >= 8) break;
                    if (string.IsNullOrEmpty(e.Url) || e.Url == "about:home") continue;
                    if (seen.Contains(e.Url)) continue;
                    seen.Add(e.Url);
                    tiles.Add(e);
                }
            }
            Add(bookmarks);
            Add(history);

            var h = new StringBuilder();
            h.Append("<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>");
            h.Append("*{box-sizing:border-box}body{margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124}");
            // 2026-10-25: the home page is laid out at the window's CSS width now (fitwindow), not
            // at a fixed 720 px, so its sizes have to be responsive. The old fixed 46/20/17 px
            // values were tuned for 720 and become giants at 360 CSS px on the Lumia 640; the
            // clamp() pairs keep the 720-px look at the top end and settle at 30/15/14 on the
            // narrow panel. The grid follows the same rule: one column at 360, two from 576.
            h.Append(".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:clamp(26px,6.4vw,46px) clamp(18px,3.6vw,26px) clamp(22px,5.3vw,38px)}");
            h.Append(".hero h1{margin:0;font-size:clamp(30px,6.4vw,46px);letter-spacing:-1px}.hero p{margin:8px 0 0;font-size:clamp(15px,2.8vw,20px);opacity:.92}");
            h.Append(".wrap{padding:clamp(14px,3.3vw,24px)}.sec{font-size:clamp(14px,2.4vw,17px);color:#5f6368;margin:0 0 14px}");
            h.Append(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}");
            // min-width:0 is load-bearing and is not a style choice. A grid item's automatic
            // minimum size is `auto`, i.e. its MIN-CONTENT width, and the titles below are
            // `white-space:nowrap` -- so one long title made its `1fr` track as wide as the
            // whole sentence. The grid then laid itself out past the viewport and the second
            // column was clipped at the right edge, which is what the fitted-window screenshot
            // of 0.2.3.4 showed. `overflow:hidden` alone would also do it (it zeroes the
            // automatic minimum), but min-width:0 says what is meant: this item may shrink.
            h.Append("a.tile{display:block;min-width:0;overflow:hidden;text-decoration:none;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}");
            h.Append(".tile .t{font-size:clamp(15px,2.8vw,20px);font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}");
            h.Append(".tile .u{font-size:clamp(12px,2.1vw,15px);color:#80868b;margin-top:7px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}");
            h.Append("</style></head><body>");
            h.Append("<div class='hero'><h1>EdgeHTML Reborn</h1><p>Modern browser engine \u00b7 Windows 10 Mobile \u00b7 ARM32</p></div>");
            h.Append("<div class='wrap'>");
            if (tiles.Count == 0)
            {
                h.Append("<p class='sec'>" + Str.Get("HomeHint") + "</p><div class='grid'>");
                string[][] defs = { new[] { "https://example.com", "example.com" }, new[] { "https://github.com", "github.com" }, new[] { "https://cn.bing.com", "bing.com" }, new[] { "https://en.wikipedia.org", "wikipedia.org" } };
                foreach (var d in defs) { h.Append($"<a class='tile' href='{d[0]}'><div class='t'>{d[1]}</div><div class='u'>{d[0]}</div></a>"); }
                h.Append("</div>");
            }
            else
            {
                h.Append("<p class='sec'>" + Str.Get("FrequentSites") + "</p><div class='grid'>");
                foreach (var e in tiles)
                {
                    string href = HtmlEscape(e.Url);
                    string title = HtmlEscape(string.IsNullOrEmpty(e.Title) ? e.Url : e.Title);
                    string host = HtmlEscape(HostOf(e.Url));
                    h.Append($"<a class='tile' href='{href}'><div class='t'>{title}</div><div class='u'>{host}</div></a>");
                }
                h.Append("</div>");
            }
            h.Append("</div></body></html>");
            return h.ToString();
        }

        List<PageLink> ExtractLinks()
        {
            var links = new List<PageLink>();
            try
            {
                int lc = WebCoreDriver.WebCoreGetLinkCount();
                for (int i = 0; i < lc; i++)
                {
                    // Apotheosis 2026-09-27: ONE buffer for every link, not one per link. At 1200 bytes
                    // a page with a few thousand anchors allocated megabytes per scroll gesture, and each
                    // iteration also crosses the P/Invoke boundary -- this runs after every scroll.
                    byte[] lu = new byte[1200];
                    int lx = 0, ly = 0, lw = 0, lh = 0;
                    if (WebCoreDriver.WebCoreGetLink(i, ref lx, ref ly, ref lw, ref lh, lu, lu.Length) != 0)
                    {
                        links.Add(new PageLink
                        {
                            X = lx, Y = ly, W = lw, H = lh,
                            Url = Encoding.UTF8.GetString(lu).TrimEnd('\0')
                        });
                    }
                }
            }
            catch { }
            return links;
        }

        // Apotheosis 2026-09-27: BlitToBitmap runs on EVERY presented frame, and it was the single
        // most allocation-heavy thing in the app. It created a fresh WriteableBitmap (a new GPU
        // surface) per frame, a fresh 2.97 MB BGRA scratch array per frame, and then swizzled
        // RGBA->BGRA one BYTE AT A TIME -- 3.1 million managed array stores per frame, on the UI
        // thread, for a 720x1080 surface. Combined with the per-frame RGBA allocation that fed it,
        // that was ~6 MB of Large Object Heap garbage per frame, which is the stutter-and-grind the
        // touch bench reported on long pages.
        //
        // Apotheosis 2026-09-27: the per-frame 2.97 MB BGRA scratch array is now allocated once.
        // That was real money: BlitToBitmap runs on every presented frame, and at 720x1080x4 it was
        // 2.97 MB of Large Object Heap garbage per frame, which is what made long-page scrolling
        // progressively stutter and grind to a halt.
        //
        // REVERTED once, and this is the interesting part: a first attempt also cached the
        // WriteableBitmap itself and re-assigned it as RenderImage.Source every frame. That made
        // scrolling feel much better, but it made IMAGES FLICKER on ya.ru -- cards blinked out and
        // back. The compositor holds a reference to the surface it is presenting, so overwriting one
        // bitmap in place while it is on screen is not safe; a fresh WriteableBitmap per frame is
        // precisely what makes the old one stay intact until the new one is ready. See
        // Doc/LESSONS.md.
        //
        // Apotheosis 2026-10-03: the fresh bitmap per frame is now a RING, not a cache of one.
        // Neither extreme is right. One cached bitmap overwritten in place is the ya.ru flicker
        // above. A new bitmap per frame costs a whole-window surface replacement per frame, and that
        // is what the eye sees: measured on dzen.ru, the engine's frame changes ~1x/s (hash sampled
        // every 500 ms: 1c7764ee ... 01617366, 01617366, 5d0428c1, 5d0428c1, 63ab9a26), and every
        // one of those changes re-uploaded the entire window. Reported by a finger as "all the
        // site's images twitch, as if they reload 1000 times", with the blank itself felt as half a
        // second -- the gap between the Source swap and the new texture arriving.
        //
        // kBlitRing bitmaps are allocated once per geometry and written round-robin, so the surface
        // being written is never the one the compositor is presenting (at ring size 3 there are two
        // frames of slack) and everything assigned as Source is already complete. The blank window
        // goes away; the in-place overwrite that broke ya.ru does not come back. The number to argue
        // about later is kBlitRing, not "cache it or not".
        //
        // Cost of the ring: up to 3 x kW*kH*4 bytes held steady instead of 1, i.e. ~12 MB at the
        // fitted bench viewport, against 3 fresh 4 MB surfaces and ~1.2 GB of LOH garbage across a
        // 300-tick live run. The ring drops this in favour of the garbage.
        const int kBlitRing = 3;
        static byte[] _blitScratch;
        static WriteableBitmap[] _blitRing;
        static int _blitRingNext;
        static int _blitRingW, _blitRingH;   // the geometry the ring was built for
        bool _blitErrLogged;

        void BlitToBitmap(byte[] rgba)
        {
            if (_gpuPresent) return;
            try
            {
                // The scratch buffer is keyed on kW*kH, and kW/kH are no longer a constant: the
                // window-fitted viewport changes them on every resize. Allocating only when the
                // array is null therefore sizes it for the FIRST viewport and then writes past
                // the end of it for every larger one -- IndexOutOfRange, thrown inside a bare
                // catch, so the frame was silently never presented. Measured: the engine
                // reported contents=1355x751 and nonwhite=1017605/1017605 while the window still
                // showed the old 720-wide bitmap. Grow it whenever it is too short.
                int need = kW * kH * 4;
                if (_blitScratch == null || _blitScratch.Length < need)
                    _blitScratch = new byte[need];

                byte[] dst = _blitScratch;
                int n = kW * kH;
                for (int i = 0, o = 0; i < n; i++, o += 4)
                {
                    dst[o] = rgba[o + 2];     // B
                    dst[o + 1] = rgba[o + 1]; // G
                    dst[o + 2] = rgba[o];     // R
                    dst[o + 3] = rgba[o + 3]; // A
                }

                // Round-robin over the ring -- see the note above. The ring is rebuilt whenever the
                // geometry changes: a WriteableBitmap's size is fixed at construction, and kW/kH
                // are rewritten by every fitted resize. Slots are filled lazily, so a ring that is
                // only ever used for one frame costs one bitmap.
                if (_blitRing == null || _blitRingW != kW || _blitRingH != kH)
                {
                    _blitRing = new WriteableBitmap[kBlitRing];
                    _blitRingW = kW;
                    _blitRingH = kH;
                    _blitRingNext = 0;
                }

                int slot = _blitRingNext;
                _blitRingNext = (_blitRingNext + 1) % kBlitRing;

                var wb = _blitRing[slot];
                if (wb == null)
                {
                    wb = new WriteableBitmap(kW, kH);
                    _blitRing[slot] = wb;
                }

                using (var stream = wb.PixelBuffer.AsStream())
                    stream.Write(dst, 0, need);
                wb.Invalidate();
                ApplyDisplayScale();   // S7: frame half of the scale; UI thread (all callers RunAsync)
                RenderImage.Source = wb;
            }
            catch (Exception ex)
            {
                // Once, not per frame: this runs on every live tick.
                if (!_blitErrLogged)
                {
                    _blitErrLogged = true;
                    AutoLog($"blit failed {ex.GetType().Name}: {ex.Message}" +
                            $" (kW={kW} kH={kH} scratch={( _blitScratch == null ? -1 : _blitScratch.Length)})");
                }
            }
        }

        // ---- window-fitted viewport (fitwindow=1) --------------------------------------------
        //
        // The complaint this answers, measured on this bench: a 1368-DIP-wide content area showed a
        // 720-DIP page with 647 DIP of white beside it. One rule replaced the two:
        //
        //   every mode -- the CSS viewport is the content area divided by the page zoom, so at the
        //                 default 100% the layout width is the window's own width and one CSS px is
        //                 one DIP. The zoom is the user's, not the UA's; see ViewportZoom().
        //
        // The two-mode version of this comment (desktop fills at 1.0, mobile re-lays out at 412 CSS
        // px and magnifies) is what produced the 3.29 measured on 2026-10-03; it is spelled out on
        // kViewportCeiling so that the trade does not have to be rediscovered.
        //
        // Software present draws the frame 1:1 in DIPs (RenderImage is Stretch="None"), so the
        // viewport is in DIPs. The GPU path is the opposite case -- its surface is physical pixels
        // and its zoom has to equal CompositionScale -- and since WebCoreGpuInit returns rc=-20 on
        // x64 it is not the path this bench measures. The setting is refused while a GPU frame is
        // live rather than doing half of each rule.
        void OnContentAreaSizeChanged(object s, SizeChangedEventArgs e)
        {
            int w = (int)(ContentArea.ActualWidth + 0.5);
            int h = (int)(ContentArea.ActualHeight + 0.5);
            if (w < 16 || h < 16) return;                  // no layout pass yet
            ApplyDisplayScale();   // S7: the area half of the scale; the frame half arrives at
                                   // the next blit (rotation defers the engine-side resize)
            if (!_fitWindow)
            {
                // The fixed-viewport line ignores size changes by design -- except one: ROTATION.
                // The viewport is a portrait 720x1080 frame and RenderImage is Stretch=None at
                // Top/Left, so in a landscape window the page keeps its portrait shape in the left
                // part of the panel and every gesture started to the right of it is outside the
                // page. Reported from the Lumia as "in landscape scroll and pinch do not work"
                // (2026-10-21) while the log showed `pinch: ... rc=0`: the engine was alive, the
                // page never turned with the window. The adopt is the SWAP of the same fixed pair
                // (720x1080 portrait, 1080x720 landscape) through the posted body the pinch and the
                // fitted line already use, so the zoom-then-resize order and the frame buffer
                // cannot drift from the other callers. The wanted geometry goes into _vpW/_vpH
                // BEFORE the busy test, so a rotation that lands mid-load is adopted by
                // DrainPendingZoom instead of being lost.
                bool wantLandscape = w > h;
                if (wantLandscape != (kW > kH))
                {
                    _vpW = wantLandscape ? kH : kW;
                    _vpH = wantLandscape ? kW : kH;
                    if (_gpuPresent) return;               // the GPU line presents on its own surface
                    if (_loading || _interacting) { _vpDirty = true; return; }
                    PostZoomAndRepaint(_vpW, _vpH, _pageZoom, "rotate");
                }
                return;
            }
            if (w > kViewportCeiling || h > kViewportCeiling) return;
            if (w == _vpW && h == _vpH) return;
            _vpW = w; _vpH = h;
            _vpZoom = ViewportZoom();

            // The busy test comes FIRST, and it is not a style choice -- getting this order wrong
            // produced a window full of sheared garbage on the first run of this feature.
            //
            // A load allocates its RGBA buffer, calls the engine and blits all under the size kW/kH
            // had when it started; kW/kH is read again at the blit. Adopting a new size in that
            // window makes the blit build a 1368x758 WriteableBitmap and read a 720x1080 buffer
            // through it -- rows interleaved, the tail of the bitmap past the end of the data, and
            // `diag` still reporting contents=720x1080 because the engine was never resized. A load
            // or an interaction owns the engine at one size; it is adopted when that finishes.
            if (_loading || _interacting) { _vpDirty = true; return; }

            AdoptViewport();
        }

        // The one entry point for "the content area is a new size and the engine may be free now".
        // OnContentAreaSizeChanged calls it when it is not busy, and every deferred pickup calls it
        // once the load that owned the engine has finished.
        //
        // The deferred case is why this function exists at all. Those pickups used to call
        // ApplyViewport() directly, which skipped the no-session branch below and asked a
        // sessionless engine to resize itself -- WebCoreSessionResize failed, nothing repainted,
        // and the first thing the user saw kept the geometry of whatever viewport had been
        // current when the load started.
        void AdoptViewport()
        {
            if (!_fitWindow) return;
            if (_vpW < 16 || _vpH < 16) return;                 // no layout pass yet
            if (_loading || _interacting) { _vpDirty = true; Crumb($"adopt: deferred (L{(_loading ? 1 : 0)}I{(_interacting ? 1 : 0)})"); return; }
            _vpDirty = false;

            // Computed here, not only in OnContentAreaSizeChanged: the zoom is the page-zoom
            // setting, and that can change while the window size does not (the Settings slider, or
            // the settings file being re-read), so the size-change path is not enough to keep it
            // current.
            _vpZoom = ViewportZoom();

            if (_sessionActive) { ApplyViewport(); return; }

            // No session, so there is nothing for WebCoreSessionResize to resize: the frame on
            // screen came from a one-shot WebCoreRenderHtml, and the engine keeps no page for it.
            // kW/kH are what the NEXT load reads, so adopt them -- and rebuild the one one-shot
            // page worth rebuilding, because the home page is the first thing the user sees and a
            // stale geometry there is exactly the white fields beside a 720-DIP home page.
            bool same = (kW == _vpW && kH == _vpH && Math.Abs(_pageZoom - _vpZoom) < 0.001f);
            kW = _vpW; kH = _vpH; _pageZoom = _vpZoom;
            Crumb($"adopt: no session {_vpW}x{_vpH} z={_vpZoom:0.00} same={same} url={_currentUrl}");
            if (same) return;
            if (_currentUrl == "about:home") NavigateTo("about:home", false);
        }

        // Every deferred pickup, in one place, for both viewport modes.
        //
        // _vpDirty is the engine-busy flag that CommitPageZoom, ApplyViewport, AdoptViewport and
        // OnContentAreaSizeChanged all set when they find a load or an interaction in flight. In the
        // fitted mode the drain was AdoptViewport, and it re-derived the zoom and the geometry. The
        // fixed 720x1080 line had no drain at all: AdoptViewport and ApplyViewport both return at
        // once when _fitWindow is off, so a commit that landed while the engine was busy -- which is
        // exactly a pinch on a heavy page, since the load's own zoom re-apply is still in flight --
        // was dropped outright and the page stayed at the stale zoom. Reported from the Lumia as
        // "питч не отрабатывает вообще" on a heavy page at the 0.50 floor.
        //
        // Fit-off has no geometry to adopt (the viewport is the constant kW x kH), so "drain" means
        // re-run the zoom commit that was deferred. CommitPageZoom is the same body, late: it is
        // idempotent here because _vpDirty is cleared first, so its already-at test can fire and
        // there is no second copy of the clamp or the zoom-then-repaint order.
        void DrainPendingZoom()
        {
            if (!_vpDirty) return;
            if (_fitWindow) { AdoptViewport(); return; }
            if (_gpuPresent) { Crumb("drain: skipped, GPU frame live"); return; }
            if (_loading || _interacting)
            {
                Crumb($"drain: still busy (L{(_loading ? 1 : 0)}I{(_interacting ? 1 : 0)})");
                return;
            }
            // Cleared before the commit, not after: CommitPageZoom's busy branch would set it again,
            // and with it clear its already-at short-circuit can tell a real late commit from a
            // re-post of one that has already reached the engine.
            _vpDirty = false;
            // A rotation that arrived while the engine was busy wrote its wanted geometry into
            // _vpW/_vpH and left _vpDirty set; the zoom-only commit below would repaint at the OLD
            // size and the turn would be lost. The >= 16 test keeps a never-written _vpW/_vpH
            // (a cold start that never called SetFitWindow) out of this branch. See
            // OnContentAreaSizeChanged's fixed-line rotation adopt.
            if (_vpW >= 16 && _vpH >= 16 && (kW != _vpW || kH != _vpH))
            {
                Crumb($"drain: deferred rotate {kW}x{kH} -> {_vpW}x{_vpH}");
                PostZoomAndRepaint(_vpW, _vpH, _vpZoom, "rotate");
                return;
            }
            Crumb($"drain: applying deferred zoom {_vpZoom:0.00} (fit off)");
            CommitPageZoom(_vpZoom, "drain");
        }

        void SetFitWindow(bool on)
        {
            if (_fitWindow == on) return;
            _fitWindow = on;
            if (!on)
            {
                // "Off" has to mean the state the ARM line runs: the fixed 720x1080 viewport at
                // zoom 1. kW/kH and _pageZoom are deliberately NOT written here -- ApplyViewport's
                // no-op test compares them against the wanted values, and pre-writing them would
                // make it conclude there is nothing to do while the engine is still fitted.
                //
                // _vpZoom does follow the setting, because it IS the wanted value: the ApplyViewport
                // call below is made directly rather than through AdoptViewport, so nothing else
                // would recompute it first. (That call is inert either way -- ApplyViewport returns
                // at once when _fitWindow is off -- but a wrong _vpZoom left here would be read by
                // the next adopt.)
                _vpW = 720; _vpH = 1080; _vpZoom = ViewportZoom();
                if (_sessionActive) ApplyViewport();
                else { kW = 720; kH = 1080; _pageZoom = 1f; RefreshFab(); }
                return;
            }
            OnContentAreaSizeChanged(null, null);
        }

        // The wanted page zoom for the fitted viewport: the user's Settings value, and nothing else.
        //
        // It is CSS page zoom, so it is a WIDTH lever, not a magnifier: the engine re-lays the page
        // out at `viewport / zoom` CSS px. 100% lays out at the window's own width, 50% at twice
        // that (a desktop layout on a small screen), 200% at half (a phone-shaped "морда" on a wide
        // one). Clamped to the slider's own range.
        //
        // Apotheosis 2026-10-03: this used to read the user agent -- `mobile` returned
        // `dipW / 412` capped at 3.5, to hold a phone-width layout at any window width. That is the
        // measured 3.29 above, and it is why the bench showed blown-up text. The UA is not a
        // geometry input any more; see the note on kViewportCeiling.
        float ViewportZoom()
        {
            float z = _defaultZoom / 100f;
            if (z < 0.5f) z = 0.5f;
            if (z > 2.0f) z = 2.0f;
            return z;
        }

        // Must run on the UI thread, and only when the engine is idle. Decides whether the fitted
        // page is out of date at all, then hands the work to PostZoomAndRepaint, which is where the
        // zoom-then-resize order and the repaint are stated once for both callers.
        void ApplyViewport()
        {
            if (!_fitWindow) return;
            if (_gpuPresent) { Crumb("fitwindow: skipped, GPU frame live"); return; }
            int w = _vpW, h = _vpH;
            if (w < 16 || h < 16) return;
            // Same rule as everywhere else in this file: a load or an interaction owns the engine
            // at one size, and its buffer, viewport and blit have to agree on it.
            if (_loading || _interacting) { _vpDirty = true; Crumb($"apply: deferred (L{(_loading ? 1 : 0)}I{(_interacting ? 1 : 0)})"); return; }
            _vpDirty = false;
            if (w == kW && h == kH && Math.Abs(_pageZoom - _vpZoom) < 0.001f)
            {
                Crumb($"apply: nothing to do, {w}x{h} z={_vpZoom:0.00} (current {_pageZoom:0.00})");
                return;
            }
            PostZoomAndRepaint(w, h, _vpZoom, "fitwindow");
        }

        // The one posted body that applies a page zoom and repaints at w x h.
        //
        // Two callers and one implementation on purpose: ApplyViewport, when the fitted geometry or
        // the settings zoom changed, and CommitPageZoom, when a pinch ended. The old pinch path had
        // its own copy of this sequence (WebCoreSetPageScale + blit + a sequence guard that returned
        // early and left the engine magnified), and that copy is exactly what the 2026-10-03 report
        // "the popup lives in another layer" came out of. One body means the zoom-then-resize order,
        // the buffer size, the supersede test and the repaint cannot drift between the two.
        void PostZoomAndRepaint(int w, int h, float zoom, string why)
        {
            kW = w; kH = h;
            _pageZoom = zoom;
            _interacting = true;
            long mySeq = ++_opSeq;
            byte[] buf = new byte[w * h * 4];
            var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
            Crumb($"{why}: {w}x{h} zoom={zoom:0.00}");

            WebEngine.Instance.Post(() =>
            {
                // ZOOM FIRST, RESIZE SECOND, and the order is the whole difference between a
                // fitted page and a re-laid-out page that is never seen. The contract is the
                // port's own, stated at WebCoreSetPageZoom: "It does NOT repaint, deliberately:
                // both callers in the harness are a resize that repaints anyway." The resize is
                // therefore the only repaint in this pair, so it has to be the LAST call -- with
                // the previous order the resize painted the page at the OLD zoom into the buffer,
                // SetPageZoom then re-laid it out correctly, and nothing ever photographed the
                // result. Measured on 0.2.3.5: `fitwindow: resize rc=0 zoom rc=0 1355x751 z=3.29`
                // in the log while the window kept showing the unzoomed layout.
                //
                // The resize is also how a pinch zooms WITHOUT changing the size: w x h arrives
                // equal to the current viewport, and WebCoreSessionResize has no same-size early
                // out -- it sets g_session->w/h and paints. A resize is the only repaint this ABI
                // offers, so that is the one used.
                int zrc = -1000;
                try { zrc = WebCoreDriver.WebCoreSetPageZoom(zoom); }
                catch (Exception ex)
                {
                    AutoLog($"{why}: SetPageZoom threw {ex.GetType().Name}: {ex.Message}");
                }
                // The zoom decides the CSS width the layout is made at (layout width = viewport /
                // zoom), and the resize re-lays out at the new viewport and paints -- which is why
                // the engine remembers the zoom across navigations (g_pageZoom plus
                // apoReassertPageZoom from the present paths) but this call must still be re-armed
                // after every RESIZE.
                int rc = -1000;
                try { rc = WebCoreDriver.WebCoreSessionResize(w, h, pin.AddrOfPinnedObject()); }
                catch (Exception ex)
                {
                    // Name the exception, do not just set the sentinel. The first run of this
                    // feature logged "resize rc=-1000 zoom rc=-1000" for a whole build cycle:
                    // the cause was EntryPointNotFoundException, because the .def did not list
                    // either name, and a bare `catch { }` had thrown the only evidence away.
                    AutoLog($"{why}: SessionResize threw {ex.GetType().Name}: {ex.Message}");
                }

                var disp = Dispatcher;
                disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                {
                    try
                    {
                        if (_opSeq != mySeq)
                        {
                            // The blit is skipped -- a newer op owns the engine and its own frame --
                            // but _interacting is still cleared below, because this call is what set
                            // it and nothing else will. The supersede case that motivated the old
                            // pinch instrument is not reachable from here any more: this body posts
                            // its engine work and its UI work as ONE unit, so a superseding op cannot
                            // land between the zoom and the blit of the SAME frame. What can land is
                            // an op posted earlier by someone else, which is what this test is for.
                            AutoLog($"{why}: frame superseded seq {mySeq} -> {_opSeq} rc={rc}");
                        }
                        else if (rc == 0)
                        {
                            BlitToBitmap(buf);
                        }
                        // A live pinch preview may still be on the Image; the frame that replaces it
                        // is the committed one, so the transform goes with the blit. ApplyViewport
                        // has no preview pending and the write is a no-op there.
                        if (GpuPanel != null) GpuPanel.RenderTransform = null;
                        if (RenderImage != null) RenderImage.RenderTransform = null;
                        _lastFrameHash = 0;
                        _interacting = false;
                        AutoLog($"{why}: resize rc={rc} zoom rc={zrc} {w}x{h} z={zoom:0.00}");
                        // A size that landed while this resize owned the engine. AdoptViewport,
                        // not ApplyViewport: it re-derives the zoom from the current setting and
                        // re-checks _sessionActive, both of which ApplyViewport assumes.
                        if (_vpDirty) AdoptViewport();
                        StartLiveMode();
                    }
                    catch { }
                    finally { pin.Free(); }
                });
            });
        }


        // reasons to hide it -- no live session, or the Settings switch is off -- so the decision
        // cannot live at each call site; that is how it ends up half-applied. Every previous
        // ScrollFab.Visibility assignment went through this instead.
        void RefreshFab()
        {
            if (ScrollFab == null) return;
            ScrollFab.Visibility = (_sessionActive && _showFab) ? Visibility.Visible : Visibility.Collapsed;
        }

        void OnScrollUp(object s, RoutedEventArgs e) => EngineScroll(-900);
        void OnScrollDown(object s, RoutedEventArgs e) => EngineScroll(900);
        void OnGpuPanelLoaded(object s, RoutedEventArgs e) { }
    }
}
