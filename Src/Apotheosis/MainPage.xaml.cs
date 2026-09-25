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
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI;
using Windows.UI.Core;
using Windows.UI.Popups;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Media.Imaging;

namespace Apotheosis
{
    public sealed partial class MainPage : Page
    {
        const int kW = 720, kH = 1080;
        static readonly string[] kSearchPrefixes = {
            "https://cn.bing.com/search?q=",
            "https://www.google.com/search?q=",
            "https://duckduckgo.com/?q=",
            "https://www.baidu.com/s?wd="
        };

        // --- Engine thread singleton ---
        static class WebEngine
        {
            public static readonly WebEngine Instance = new WebEngine();
            readonly BlockingCollection<Action> _queue = new BlockingCollection<Action>(new ConcurrentQueue<Action>());

            WebEngine()
            {
                var t = new Thread(Loop) { IsBackground = true, Name = "WebEngine" };
                t.Start();
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

        static void SetupRuntimeEnv()
        {
            try
            {
                string installDir = Package.Current.InstalledLocation.Path;
                string localDir = ApplicationData.Current.LocalFolder.Path;
                if (string.IsNullOrEmpty(installDir) || string.IsNullOrEmpty(localDir)) return;

                string fontsDir = Path.Combine(installDir, "Assets", "fonts");
                string cacheDir = Path.Combine(localDir, "fontconfig-cache");
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
                Environment.SetEnvironmentVariable("FONTCONFIG_FILE", confPath);

                string caPath = Path.Combine(installDir, "Assets", "cacert.pem");
                if (File.Exists(caPath))
                {
                    byte[] caBytes = File.ReadAllBytes(caPath);
                    WebCoreDriver.WebCoreSetCACertBlob(caBytes, caBytes.Length);
                }
            }
            catch { }
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
        bool _loading;
        bool _interacting;
        long _opSeq;

        // --- Data ---
        List<Entry> _bookmarks = new List<Entry>();
        List<Entry> _historyList = new List<Entry>();
        List<Entry> _downloads = new List<Entry>();
        DrawerTab _tab = DrawerTab.Favorites;

        string _currentUrl = "";
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
        bool _setUaDesktop;
        int _defaultZoom = 100;
        int _tabMode;
        string _uaCustom = "";
        string _searchPrefix = kSearchPrefixes[0];
        string _homeUrl = "about:home";
        int _uiLang;

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
        bool _liveBusy;
        int _liveBusyAge;
        uint _lastFrameHash;
        int _liveStaticTicks;
        int _liveTotalTicks;

        // Scroll
        int _scrollAccum;
        int _scrollAccumX;
        bool _scrollBusy;

        // Pinch zoom
        bool _pinching;
        float _liveScale = 1.0f;
        float _pageScale = 1.0f;
        double _focalX = 360, _focalY = 540;

        // Load watchdog
        DispatcherTimer _loadWatchdog;

        // IME
        string _lastImeText = "";
        bool _imeOpen;
        bool _imeSyncing;

        bool _appForeground = true;

        public MainPage()
        {
            InitializeComponent();
            LoadData();
            LoadSettings();
            _tabs.Add(new Tab { CurrentUrl = _homeUrl });
            _activeTab = 0;
            UpdateTabCount();

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
            var out = new List<Entry>();
            if (!File.Exists(path)) return out;
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
            {
                if (string.IsNullOrEmpty(line)) continue;
                var parts = line.Split('\t');
                var e = new Entry();
                if (parts.Length > 0) e.Url = parts[0];
                if (parts.Length > 1) e.Title = parts[1];
                if (parts.Length > 2) e.Extra = parts[2];
                out.Add(e);
            }
            return out;
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
            try
            {
                string d = LocalStateDir();
                string path = Path.Combine(d, "settings.ini");
                if (!File.Exists(path)) { ApplySettings(); return; }
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
                    }
                }
            }
            catch { }
            if (_setSearch < 0 || _setSearch > 3) _setSearch = 0;
            if (_uiLang < 0 || _uiLang > 2) _uiLang = 0;
            Str.SetLang(_uiLang);
            ApplySettings();
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
                File.WriteAllText(Path.Combine(d, "settings.ini"), sb.ToString(), Encoding.UTF8);
            }
            catch { }
        }

        void ApplySettings()
        {
            _searchPrefix = SearchPrefixFor(_setSearch);
            _searchPrefixStatic = _searchPrefix;
            _defaultZoom = Math.Max(50, Math.Min(200, _defaultZoom));
            _uaMobile = !_setUaDesktop;
            int mobile = _uaMobile ? 1 : 0;
            string ua = _uaCustom;
            WebEngine.Instance.Post(() =>
            {
                try { WebCoreDriver.WebCoreSetUserAgentMobile(mobile); } catch { }
                try { WebCoreDriver.WebCoreSetUserAgentString(string.IsNullOrEmpty(ua) ? null : ua); } catch { }
            });
            if (UaBtn != null)
                UaBtn.Content = _uaMobile ? Str.Get("MobileSite") : Str.Get("DesktopSite");
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
            if (BackBtn != null) BackBtn.IsEnabled = (_navIndex > 0);
            if (FwdBtn != null) FwdBtn.IsEnabled = (_navIndex >= 0 && _navIndex < _navStack.Count - 1);
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
        }

        void NavigateTo(string url, bool pushHistory)
        {
            if (_loading) return;
            string wurl = string.IsNullOrEmpty(url) ? "about:home" : url;
            bool isHome = wurl == "about:home";
            _currentUrl = isHome ? "about:home" : wurl;

            // Reset zoom
            _pinching = false; _liveScale = 1.0f; _pageScale = 1.0f;
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
            HideSuggestions();
            _urlSyncing = true;
            if (UrlBox != null) UrlBox.Text = isHome ? "" : url;
            _urlSyncing = false;
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
                        WebCoreDriver.WebCoreCloseSession();
                        GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                        try { rc = WebCoreDriver.WebCoreRenderHtml(homeHtml, kW, kH, h.AddrOfPinnedObject()); }
                        finally { h.Free(); }
                        loadOk = (rc == 0);
                        title = "Home";
                    }
                    else
                    {
                        WriteStage("before-load " + surl);
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
                        WriteStage($"after-load url={surl} rc={netRc} compositing={comp}\nERR: {err}\ndiag: {diag}");
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

                var links = ExtractLinks();
                string titleCopy = title;
                bool ok = (rc == 0);
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        if (ok)
                        {
                            BlitToBitmap(rgba);
                            _pageLinks = links;
                        }
                        _sessionActive = sessionActive;
                        if (ScrollFab != null)
                            ScrollFab.Visibility = sessionActive ? Visibility.Visible : Visibility.Collapsed;
                        _lastFrameHash = 0;
                        if (sessionActive) StartLiveMode(); else StopLiveMode();
                        OnNavDone(titleCopy, ok, loadOk);
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
                _urlSyncing = true;
                if (UrlBox != null) UrlBox.Text = _currentUrl;
                _urlSyncing = false;
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
            if (_sessionActive && _defaultZoom != 100)
                PinchCommit(_defaultZoom / 100.0f, kW / 2, kH / 2);
        }

        void OnLoadWatchdog(object s, object e)
        {
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            if (_loading || _interacting)
            {
                ++_opSeq;
                _interacting = false;
                _sessionActive = false;
                if (ScrollFab != null) ScrollFab.Visibility = Visibility.Collapsed;
                if (TitleText != null) TitleText.Text = Str.Get("LoadTimeout");
                SetLoading(false);
                UpdateNavButtons();
            }
        }

        // ===== Click handling =====
        void MapTapToEngine(double dipX, double dipY, out int outPx, out int outPy)
        {
            if (dipX < 0 || dipY < 0) { outPx = -1; outPy = -1; return; }
            double aw = ContentArea.ActualWidth, ah = ContentArea.ActualHeight;
            if (_gpuPresent && aw > 1 && ah > 1)
            {
                outPx = (int)(dipX * kW / aw + 0.5);
                outPy = (int)(dipY * kH / ah + 0.5);
            }
            else
            {
                outPx = (int)(dipX + 0.5);
                outPy = (int)(dipY + 0.5);
            }
        }

        void OnPageTapped(object sender, TappedRoutedEventArgs e)
        {
            HideSuggestions();
            if (_loading || _interacting) return;
            var pt = e.GetPosition(ContentArea);
            MapTapToEngine(pt.X, pt.Y, out int px, out int py);
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
                                if (ScrollFab != null) ScrollFab.Visibility = Visibility.Collapsed;
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

        void ApplyEngineFrame(byte[] rgba, string title, string navUrl, List<PageLink> links)
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
            if (navUrl != null)
            {
                _currentUrl = navUrl;
                _urlSyncing = true;
                if (UrlBox != null) UrlBox.Text = navUrl;
                _urlSyncing = false;
                UpdateLockIcon();
                UpdateUrlActionGlyph();
                bool dup = (_navIndex >= 0 && _navIndex < _navStack.Count && _navStack[_navIndex] == navUrl);
                if (!dup)
                {
                    if (_navIndex >= 0 && _navIndex < _navStack.Count - 1)
                        _navStack.RemoveRange(_navIndex + 1, _navStack.Count - _navIndex - 1);
                    _navStack.Add(navUrl);
                    _navIndex = _navStack.Count - 1;
                    UpdateNavButtons();
                }
                AddHistory(_currentUrl, _currentTitle);
            }
        }

        // ===== Scroll =====
        void EngineScroll(int dy)
        {
            if (!_sessionActive || _loading || _interacting) return;
            _interacting = true;
            SetLoading(true);
            if (_loadWatchdog != null) _loadWatchdog.Start();
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
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
            if (!_sessionActive || (dx == 0 && dy == 0)) return;
            _scrollAccumX += dx;
            _scrollAccum += dy;
            if (!_scrollBusy) PumpScroll();
        }

        void PumpScroll()
        {
            if ((_scrollAccum == 0 && _scrollAccumX == 0) || !_sessionActive) { _scrollBusy = false; return; }
            int dy = _scrollAccum; _scrollAccum = 0;
            int dx = _scrollAccumX; _scrollAccumX = 0;
            _scrollBusy = true;
            CoreDispatcher disp = Dispatcher;
            long mySeq = _opSeq;
            bool present = _gpuPresent;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try { rc = WebCoreDriver.WebCoreScrollBy(dx, dy, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
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

        // ===== Pinch zoom =====
        void OnImageManipDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            if (!_sessionActive) return;
            float ds = e.Delta.Scale;
            if (_pinching || (ds > 0 && (ds > 1.002f || ds < 0.998f)))
            {
                _pinching = true;
                if (ds > 0) _liveScale *= ds;
                float total = _pageScale * _liveScale;
                if (total < 0.5f) _liveScale = 0.5f / _pageScale;
                if (total > 6.0f) _liveScale = 6.0f / _pageScale;
                var fp = e.Position;
                _focalX = fp.X; _focalY = fp.Y;
                ApplyLiveZoom();
                return;
            }
            double dx = -e.Delta.Translation.X;
            double dy = -e.Delta.Translation.Y;
            int idx = (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
            int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
            if (idx != 0 || idy != 0) FreeScrollBy(idx, idy);
        }

        void ApplyLiveZoom()
        {
            var t = new ScaleTransform { ScaleX = _liveScale, ScaleY = _liveScale, CenterX = _focalX, CenterY = _focalY };
            if (_gpuPresent) GpuPanel.RenderTransform = t;
            else RenderImage.RenderTransform = t;
        }

        void OnImageManipCompleted(object sender, ManipulationCompletedRoutedEventArgs e)
        {
            if (!_pinching) return;
            _pinching = false;
            float live = _liveScale; _liveScale = 1.0f;
            float newScale = _pageScale * live;
            newScale = Math.Max(0.5f, Math.Min(6.0f, newScale));
            MapTapToEngine(_focalX, _focalY, out int fpx, out int fpy);
            PinchCommit(newScale, fpx, fpy);
        }

        void PinchCommit(float newScale, int focalX, int focalY)
        {
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;
            bool present = _gpuPresent;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                int rc = -999;
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                try { rc = WebCoreDriver.WebCoreSetPageScale(newScale, focalX, focalY, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        if (rcCopy == 0)
                        {
                            _pageScale = newScale;
                            if (!present) BlitToBitmap(rgba);
                        }
                        if (GpuPanel != null) GpuPanel.RenderTransform = null;
                        if (RenderImage != null) RenderImage.RenderTransform = null;
                        StartLiveMode();
                    });
                }
                catch { }
            });
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
            _liveBusyAge = 0;
            _liveBusy = true;
            long mySeq = _opSeq;
            bool present = _gpuPresent;
            CoreDispatcher disp = Dispatcher;

            WebEngine.Instance.Post(() =>
            {
                if (!_appForeground) return;
                byte[] rgba = new byte[kW * kH * 4];
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
                int rcCopy = rc; uint hashCopy = hash; int pendingCopy = pending;
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
                                if (ScrollFab != null) ScrollFab.Visibility = Visibility.Collapsed;
                                StopLiveMode();
                            }
                            return;
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
                            if (++_liveStaticTicks >= 40) StopLiveMode();
                            return;
                        }
                        _liveStaticTicks = 0;
                        _lastFrameHash = hashCopy;
                        if (!present) BlitToBitmap(rgba);
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
        void OnBack(object s, RoutedEventArgs e)
        {
            if (_loading || _navIndex <= 0) return;
            --_navIndex;
            NavigateTo(_navStack[_navIndex], false);
        }

        void OnForward(object s, RoutedEventArgs e)
        {
            if (_loading || _navIndex >= _navStack.Count - 1) return;
            ++_navIndex;
            NavigateTo(_navStack[_navIndex], false);
        }

        void OnHome(object s, RoutedEventArgs e) => NavigateTo(_homeUrl, true);

        void OnUrlKeyDown(object s, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter) NavigateTo(NormalizeUrl(UrlBox?.Text), true);
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
                if (ScrollFab != null) ScrollFab.Visibility = Visibility.Collapsed;
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
            if (_urlSyncing) { HideSuggestions(); return; }
            if (!_urlFocused) { HideSuggestions(); return; }
            string q = UrlBox?.Text ?? "";
            if (string.IsNullOrEmpty(q)) { HideSuggestions(); return; }
            ShowSuggestions(q);
        }

        void OnUrlGotFocus(object s, RoutedEventArgs e) { _urlFocused = true; }
        void OnUrlLostFocus(object s, RoutedEventArgs e) { _urlFocused = false; }

        // ===== Suggestions =====
        void ShowSuggestions(string query)
        {
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

            foreach (var e in matches)
            {
                string u = e.Url;
                var row = MakeRow(string.IsNullOrEmpty(e.Title) ? e.Url : e.Title, e.Url, Colors.White);
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
                    _urlSyncing = true;
                    if (UrlBox != null) UrlBox.Text = u;
                    _urlSyncing = false;
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
            if (ActFavLabel != null)
                ActFavLabel.Text = (!string.IsNullOrEmpty(_currentUrl) && IsBookmarked(_currentUrl)) ? "Bookmarked" : "Bookmark";
            if (ActUaLabel != null)
                ActUaLabel.Text = _uaMobile ? "Mobile Site" : "Desktop Site";
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
                if (TitleText != null) TitleText.Text = "Bookmark Removed";
            }
            else
            {
                _bookmarks.Insert(0, new Entry { Url = _currentUrl, Title = string.IsNullOrEmpty(_currentTitle) ? _currentUrl : _currentTitle });
                if (TitleText != null) TitleText.Text = "Bookmarked";
            }
            SaveBookmarks();
            if (Drawer.Visibility == Visibility.Visible && _tab == DrawerTab.Favorites)
                RebuildDrawerList();
        }

        void DoShare()
        {
            if (string.IsNullOrEmpty(_currentUrl) || _currentUrl == "about:home")
            {
                if (TitleText != null) TitleText.Text = "Nothing to Share";
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
                if (TitleText != null) TitleText.Text = "Link Copied";
            }
            catch { }
        }

        void DoToggleUA()
        {
            _uaMobile = !_uaMobile;
            if (UaBtn != null) UaBtn.Content = _uaMobile ? "Mobile Site" : "Desktop Site";
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
            if (tab == DrawerTab.Favorites)
                ActionBtn.Content = (!string.IsNullOrEmpty(_currentUrl) && IsBookmarked(_currentUrl)) ? "Remove Bookmark" : "Bookmark This Page";
            else if (tab == DrawerTab.History)
                ActionBtn.Content = "Clear";
            else
                ActionBtn.Content = "Download This Page";
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
            var sp = new StackPanel { Margin = new Thickness(14, 10, 14, 10) };
            var t = new TextBlock
            {
                Text = title, FontSize = 20,
                Foreground = new SolidColorBrush(titleColor),
                TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1
            };
            var u = new TextBlock
            {
                Text = sub, FontSize = 15,
                Foreground = new SolidColorBrush(Color.FromArgb(255, 0x80, 0x86, 0x8b)),
                TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1,
                Margin = new Thickness(0, 2, 0, 0)
            };
            sp.Children.Add(t); sp.Children.Add(u);
            return new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(255, 0x2B, 0x2D, 0x31)),
                CornerRadius = new CornerRadius(10),
                Margin = new Thickness(0, 0, 0, 8),
                Child = sp
            };
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
                string emptyText = _tab == DrawerTab.Favorites ? "No Bookmarks" : (_tab == DrawerTab.History ? "No History" : "No Downloads");
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
                        Content = "Remove Bookmark", FontSize = 15,
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
            if (VersionText != null)
            {
                var pv = Package.Current.Id.Version;
                VersionText.Text = $"{Str.Get("Version")} {pv.Major}.{pv.Minor}.{pv.Build}.{pv.Revision}";
            }
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
            ApplySettings();
            SaveSettings();
            SettingsPage.Visibility = Visibility.Collapsed;
            StartLiveMode();
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
                SaveSettings();
            }
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
                case "export": ExportDebug(); break;
                case "gpu": HideSettings(); OnToggleGpu(null, null); break;
                case "checkupdate": CheckForUpdate(true); break;
            }
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
                                if (ScrollFab != null) ScrollFab.Visibility = Visibility.Collapsed;
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
            t.PageScale = _pageScale;
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
            _pageScale = t.PageScale;
            ++_opSeq;
            _interacting = false;
            if (_loadWatchdog != null) _loadWatchdog.Stop();
            _loading = false;
            UpdateNavButtons();
            UpdateLockIcon();
            _urlSyncing = true;
            if (UrlBox != null) UrlBox.Text = (_currentUrl == "about:home") ? "" : _currentUrl;
            _urlSyncing = false;
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
            _pageScale = 1.0f;
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
                _pageScale = 1.0f;
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
                try { rc = WebCoreDriver.WebCoreDownload("https://api.github.com/repos/Jimmyxiao2009/Project-Apotheosis/releases/latest", jsonPath); } catch { }
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
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, async () =>
                    {
                        _updateChecking = false;
                        if (!ok) { if (manual && TitleText != null) TitleText.Text = "Update Check Failed"; return; }
                        if (!newer) { if (manual && TitleText != null) TitleText.Text = "Already Up to Date " + tagW; return; }
                        if (TitleText != null) TitleText.Text = "New Version " + tagW;
                        string target = string.IsNullOrEmpty(pageW)
                            ? "https://github.com/Jimmyxiao2009/Project-Apotheosis/releases/latest"
                            : pageW;
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
            h.Append(".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:46px 26px 38px}");
            h.Append(".hero h1{margin:0;font-size:46px;letter-spacing:-1px}.hero p{margin:10px 0 0;font-size:20px;opacity:.92}");
            h.Append(".wrap{padding:24px}.sec{font-size:17px;color:#5f6368;margin:0 0 14px}");
            h.Append(".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}");
            h.Append("a.tile{display:block;text-decoration:none;background:#fff;border-radius:16px;padding:18px 18px 20px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}");
            h.Append(".tile .t{font-size:20px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}");
            h.Append(".tile .u{font-size:15px;color:#80868b;margin-top:7px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}");
            h.Append("</style></head><body>");
            h.Append("<div class='hero'><h1>EdgeHTML Reborn</h1><p>Modern browser engine \u00b7 Windows 10 Mobile \u00b7 ARM32</p></div>");
            h.Append("<div class='wrap'>");
            if (tiles.Count == 0)
            {
                h.Append("<p class='sec'>Enter a URL in the address bar above.</p><div class='grid'>");
                string[][] defs = { new[] { "https://example.com", "example.com" }, new[] { "https://github.com", "github.com" }, new[] { "https://cn.bing.com", "bing.com" }, new[] { "https://en.wikipedia.org", "wikipedia.org" } };
                foreach (var d in defs) { h.Append($"<a class='tile' href='{d[0]}'><div class='t'>{d[1]}</div><div class='u'>{d[0]}</div></a>"); }
                h.Append("</div>");
            }
            else
            {
                h.Append("<p class='sec'>Frequent Sites</p><div class='grid'>");
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
                    int lx = 0, ly = 0, lw = 0, lh = 0;
                    byte[] lu = new byte[1200];
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

        void BlitToBitmap(byte[] rgba)
        {
            if (_gpuPresent) return;
            try
            {
                var wb = new WriteableBitmap(kW, kH);
                using (var stream = wb.PixelBuffer.AsStream())
                {
                    // RGBA -> BGRA conversion
                    byte[] bgra = new byte[kW * kH * 4];
                    for (int i = 0, n = kW * kH; i < n; i++)
                    {
                        bgra[i * 4 + 0] = rgba[i * 4 + 2]; // B
                        bgra[i * 4 + 1] = rgba[i * 4 + 1]; // G
                        bgra[i * 4 + 2] = rgba[i * 4 + 0]; // R
                        bgra[i * 4 + 3] = rgba[i * 4 + 3]; // A
                    }
                    stream.Write(bgra, 0, bgra.Length);
                }
                wb.Invalidate();
                RenderImage.Source = wb;
            }
            catch { }
        }

        void OnScrollUp(object s, RoutedEventArgs e) => EngineScroll(-900);
        void OnScrollDown(object s, RoutedEventArgs e) => EngineScroll(900);
        void OnGpuPanelLoaded(object s, RoutedEventArgs e) { }
    }
}
