using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.DataTransfer;
using Windows.Foundation;
using Windows.Storage;
using Windows.System;
using Windows.UI.Core;
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
                    try { job(); } catch { }
            }
            public void Post(Action job) => _queue.Add(job);
        }

        static void SetupRuntimeEnv()
        {
            try
            {
                string installDir = Package.Current.InstalledLocation.Path;
                string localDir = ApplicationData.Current.LocalFolder.Path;
                string fontsDir = Path.Combine(installDir, "Assets", "fonts");
                string cacheDir = Path.Combine(localDir, "fontconfig-cache");
                string confPath = Path.Combine(localDir, "fonts.conf");
                File.WriteAllText(confPath,
                    "<?xml version=\"1.0\"?>\n<fontconfig>\n" +
                    $"  <dir>{fontsDir}</dir>\n" +
                    $"  <cachedir>{cacheDir}</cachedir>\n" +
                    "  <match target=\"pattern\"><test name=\"family\"><string>sans-serif</string></test>" +
                    "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Segoe UI</string><string>Arial</string></edit></match>\n" +
                    "</fontconfig>\n");
                Environment.SetEnvironmentVariable("FONTCONFIG_FILE", confPath);
                string caPath = Path.Combine(installDir, "Assets", "cacert.pem");
                if (File.Exists(caPath))
                {
                    byte[] ca = File.ReadAllBytes(caPath);
                    WebCoreDriver.WebCoreSetCACertBlob(ca, ca.Length);
                }
            }
            catch { }
        }

        static void WriteStage(string s)
        {
            try { File.WriteAllText(Path.Combine(ApplicationData.Current.LocalFolder.Path, "stage.txt"), s + "\n"); } catch { }
        }

        // Navigation state
        List<string> _navStack = new List<string>();
        int _navIndex = -1;
        bool _loading, _interacting;
        long _opSeq;
        string _currentUrl = "", _currentTitle = "";
        bool _sessionActive;

        public MainPage()
        {
            InitializeComponent();
            NavigateTo("about:home", true);
        }

        void NavigateTo(string url, bool pushHistory)
        {
            if (_loading) return;
            string wurl = string.IsNullOrEmpty(url) ? "about:home" : url;
            bool isHome = wurl == "about:home";
            _currentUrl = wurl;

            if (pushHistory)
            {
                if (_navIndex >= 0 && _navIndex < _navStack.Count - 1)
                    _navStack.RemoveRange(_navIndex + 1, _navStack.Count - _navIndex - 1);
                _navStack.Add(wurl);
                _navIndex = _navStack.Count - 1;
            }
            BackBtn.IsEnabled = _navIndex > 0;

            _urlSyncing = true;
            if (UrlBox != null) UrlBox.Text = isHome ? "" : wurl;
            _urlSyncing = false;

            if (TitleText != null)
                TitleText.Text = isHome ? "Home" : $"Loading  {wurl}";
            SetLoading(true);

            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;
            string homeHtml = isHome ? BuildHomeHtml() : "";

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
                        WriteStage("before-load " + wurl);
                        GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                        int netRc;
                        try { netRc = WebCoreDriver.WebCoreSessionLoad(wurl, kW, kH, h.AddrOfPinnedObject()); }
                        finally { h.Free(); }
                        byte[] tBuf = new byte[512];
                        WebCoreDriver.WebCoreGetTitle(tBuf, tBuf.Length);
                        string t = Encoding.UTF8.GetString(tBuf).TrimEnd('\0');
                        byte[] errBuf = new byte[512];
                        WebCoreDriver.WebCoreGetLastError(errBuf, errBuf.Length);
                        string err = Encoding.UTF8.GetString(errBuf).TrimEnd('\0');
                        int comp = 0;
                        try { comp = WebCoreDriver.WebCoreEnableCompositing(); } catch { }
                        WriteStage($"after-load rc={netRc} comp={comp} err={err}");
                        if (netRc == 0)
                        {
                            rc = 0; loadOk = true; sessionActive = true;
                            title = string.IsNullOrEmpty(t) ? wurl : t;
                        }
                        else
                        {
                            string eh = MakeErrorHtml(wurl, err);
                            GCHandle h2 = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                            try { rc = WebCoreDriver.WebCoreRenderHtml(eh, kW, kH, h2.AddrOfPinnedObject()); }
                            finally { h2.Free(); }
                            title = "Load Failed";
                        }
                    }
                }
                catch { rc = -1000; title = "Render Error"; }

                string titleCopy = title;
                bool ok = (rc == 0);
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        if (ok) BlitToBitmap(rgba);
                        _sessionActive = sessionActive;
                        _currentTitle = titleCopy;
                        if (TitleText != null) TitleText.Text = string.IsNullOrEmpty(_currentTitle) ? "EdgeHTML Reborn" : titleCopy;
                        SetLoading(false);
                        BackBtn.IsEnabled = _navIndex > 0;
                        if (ok && !isHome) _urlSyncing = true;
                        if (UrlBox != null && ok && !isHome) UrlBox.Text = _currentUrl;
                        _urlSyncing = false;
                    });
                }
                catch { }
            });
        }

        void SetLoading(bool loading)
        {
            _loading = loading;
            if (Progress != null)
            {
                Progress.IsIndeterminate = loading;
                Progress.Visibility = loading ? Visibility.Visible : Visibility.Collapsed;
            }
            if (GoBtn != null) GoBtn.Content = loading ? "\x2715" : "\x21BB";
        }

        void BlitToBitmap(byte[] rgba)
        {
            try
            {
                var wb = new WriteableBitmap(kW, kH);
                using (var stream = wb.PixelBuffer.AsStream())
                {
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

        static string MakeErrorHtml(string url, string err)
        {
            return "<html><head><meta charset='utf-8'></head>" +
                "<body style='margin:0;background:#fff;font-family:sans-serif'>" +
                "<div style='background:#d93025;color:#fff;padding:32px 24px'><h1 style='margin:0;font-size:38px'>Cannot access this page</h1></div>" +
                $"<div style='padding:24px;color:#333;font-size:24px'><p style='word-break:break-all;color:#1a73e8'>{url}</p>" +
                $"<p style='color:#d93025;font-size:22px;word-break:break-all'>{err ?? ""}</p></div></body></html>";
        }

        static string BuildHomeHtml()
        {
            return "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>" +
                "<style>*{box-sizing:border-box}body{margin:0;background:#f5f6f8;font-family:sans-serif;color:#202124}" +
                ".hero{background:linear-gradient(135deg,#00aa77,#0088cc);color:#fff;padding:46px 26px 38px}" +
                ".hero h1{margin:0;font-size:46px}.hero p{margin:10px 0 0;font-size:20px;opacity:.92}" +
                ".wrap{padding:24px}.sec{font-size:17px;color:#5f6368;margin:0 0 14px}" +
                ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:14px}" +
                "a.tile{display:block;text-decoration:none;background:#fff;border-radius:16px;padding:18px;box-shadow:0 2px 10px rgba(0,0,0,.08);color:#202124}" +
                ".tile .t{font-size:20px;font-weight:600}.tile .u{font-size:15px;color:#80868b;margin-top:7px}" +
                "</style></head><body>" +
                "<div class='hero'><h1>EdgeHTML Reborn</h1><p>Modern browser engine</p></div>" +
                "<div class='wrap'><p class='sec'>Enter a URL above</p>" +
                "<div class='grid'>" +
                "<a class='tile' href='https://example.com'><div class='t'>example.com</div><div class='u'>https://example.com</div></a>" +
                "<a class='tile' href='https://github.com'><div class='t'>github.com</div><div class='u'>https://github.com</div></a>" +
                "<a class='tile' href='https://cn.bing.com'><div class='t'>bing.com</div><div class='u'>https://cn.bing.com</div></a>" +
                "<a class='tile' href='https://en.wikipedia.org'><div class='t'>wikipedia.org</div><div class='u'>https://en.wikipedia.org</div></a>" +
                "</div></div></body></html>";
        }

        // Click handling
        void MapTapToEngine(double dipX, double dipY, out int outPx, out int outPy)
        {
            double aw = ContentArea.ActualWidth, ah = ContentArea.ActualHeight;
            outPx = (int)(dipX * kW / aw + 0.5);
            outPy = (int)(dipY * kH / ah + 0.5);
        }

        void OnPageTapped(object sender, TappedRoutedEventArgs e)
        {
            if (_loading || _interacting || !_sessionActive) return;
            var pt = e.GetPosition(ContentArea);
            MapTapToEngine(pt.X, pt.Y, out int px, out int py);
            if (px < 0 || py < 0 || px >= kW || py >= kH) return;
            ForwardClickToEngine(px, py);
        }

        void ForwardClickToEngine(int px, int py)
        {
            if (_interacting) return;
            _interacting = true;
            SetLoading(true);
            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;

            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                int rc = -999;
                try { rc = WebCoreDriver.WebCoreClickAt(px, py, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        _interacting = false;
                        SetLoading(false);
                        if (rcCopy == 0) BlitToBitmap(rgba);
                    });
                }
                catch { }
            });
        }

        // Scroll
        void OnImageManipDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            if (!_sessionActive || _loading || _interacting) return;
            double dy = -e.Delta.Translation.Y;
            if (Math.Abs(dy) < 1) return;
            int idy = (int)(dy < 0 ? dy - 0.5 : dy + 0.5);
            if (idy == 0) return;

            CoreDispatcher disp = Dispatcher;
            long mySeq = ++_opSeq;
            WebEngine.Instance.Post(() =>
            {
                byte[] rgba = new byte[kW * kH * 4];
                GCHandle h = GCHandle.Alloc(rgba, GCHandleType.Pinned);
                int rc = -999;
                try { rc = WebCoreDriver.WebCoreScrollBy(0, idy, h.AddrOfPinnedObject()); }
                finally { h.Free(); }
                int rcCopy = rc;
                try
                {
                    disp.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (_opSeq != mySeq) return;
                        if (rcCopy == 0) BlitToBitmap(rgba);
                    });
                }
                catch { }
            });
        }

        void OnImageManipCompleted(object sender, ManipulationCompletedRoutedEventArgs e) { }

        // URL handling
        bool _urlSyncing;
        void OnUrlKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter) NavigateTo(NormalizeUrl(UrlBox?.Text), true);
        }

        void OnUrlChanged(object sender, TextChangedEventArgs e) { }

        static string NormalizeUrl(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return "about:home";
            raw = raw.Trim();
            if (string.IsNullOrEmpty(raw)) return "about:home";
            if (raw.StartsWith("about:")) return raw;
            bool looksUrl = raw.Contains("://") || (raw.Contains('.') && !raw.Contains(' '));
            if (!looksUrl) return "https://cn.bing.com/search?q=" + Uri.EscapeDataString(raw);
            if (!raw.Contains("://")) raw = "https://" + raw;
            return raw;
        }

        // Toolbar
        void OnBack(object s, RoutedEventArgs e)
        {
            if (_loading || _navIndex <= 0) return;
            --_navIndex;
            NavigateTo(_navStack[_navIndex], false);
        }

        void OnGo(object s, RoutedEventArgs e)
        {
            if (_loading)
            {
                ++_opSeq; _interacting = false;
                WebEngine.Instance.Post(() => { try { WebCoreDriver.WebCoreCloseSession(); } catch { } });
                _sessionActive = false;
                SetLoading(false);
                if (TitleText != null) TitleText.Text = "Cancelled";
                return;
            }
            NavigateTo(NormalizeUrl(UrlBox?.Text), true);
        }
    }
}
