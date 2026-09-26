using System;
using Apotheosis.EngineWinRT;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;

namespace Apotheosis.Shell.Controls
{
    /// <summary>
    /// The only class in the C# shell that talks to EngineWinRT. Everything
    /// it does is "forward this UI event to the native session, or forward
    /// this native event to a C# event MainPage can bind to" — no browser
    /// logic (history, favorites, etc.) belongs here.
    /// </summary>
    public sealed partial class EngineHost : UserControl
    {
        public EngineSession Session { get; } = new EngineSession();

        // Re-exposed as plain C# events so MainPage/BrowserViewModel don't
        // need an `Apotheosis.EngineWinRT` using directive at all — the
        // WinRT projection stays fully contained inside this control.
        public event EventHandler<string> TitleChanged;
        public event EventHandler<NavigationCompletedEventArgs> NavigationCompleted;
        public event EventHandler<EngineLoadState> LoadStateChanged;

        public EngineHost()
        {
            InitializeComponent();

            // Wiring happens in the constructor (cheap, no native calls yet);
            // the actual native Initialize() call is deferred to
            // OnEngineSurfaceLoaded, once we have a real SwapChainPanel with
            // a real size to hand the engine.
            Session.TitleChanged += (s, title) => TitleChanged?.Invoke(this, title);
            Session.LoadStateChanged += (s, state) => LoadStateChanged?.Invoke(this, state);
            Session.NavigationCompleted += (s, args) =>
                NavigationCompleted?.Invoke(this, new NavigationCompletedEventArgs(args.Url, args.IsSuccess));
        }

        private void OnEngineSurfaceLoaded(object sender, RoutedEventArgs e)
        {
            Session.Initialize();
            Session.SetSwapChainPanel(EngineSurface);
        }

        private void OnEngineSurfaceSizeChanged(object sender, SizeChangedEventArgs e)
        {
            Session.Resize((float)e.NewSize.Width, (float)e.NewSize.Height);
        }

        public void Navigate(string url) => Session.Navigate(url);
        public void GoBack() => Session.GoBack();
        public void GoForward() => Session.GoForward();
        public void Reload() => Session.Reload();
        public void Stop() => Session.Stop();

        // --- Input forwarding -----------------------------------------
        // NOTE: this control only ever forwards *page-content* input.
        // The address bar in MainPage is an ordinary XAML TextBox with
        // ordinary UWP focus/IME handling — it is NOT routed through here.
        // Only taps/typing that land inside actual web page content
        // (e.g. an <input> on the rendered page) go through EngineSession,
        // which is exactly the path the v0.1.7.29 upstream fix touched
        // (explicit focus + selection before accepting IME input). If a
        // future text-input bug shows up, check whether it's an address-bar
        // bug (pure XAML, unrelated to native) or a page-content bug
        // (native, and likely thread-marshaling-adjacent) before digging in.

        private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            var p = e.GetCurrentPoint(EngineSurface).Position;
            Session.OnPointerPressed((float)p.X, (float)p.Y);
        }

        private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            var p = e.GetCurrentPoint(EngineSurface).Position;
            Session.OnPointerMoved((float)p.X, (float)p.Y);
        }

        private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            var p = e.GetCurrentPoint(EngineSurface).Position;
            Session.OnPointerReleased((float)p.X, (float)p.Y);
        }

        private void OnManipulationDelta(object sender, ManipulationDeltaRoutedEventArgs e)
        {
            // ManipulationDelta.Scale is relative-to-last-event; WebCoreDriver's
            // pinch handler expects a scale delta per the EngineWinRT contract —
            // confirm that assumption against the actual native pinch-zoom
            // implementation (the harness's existing zoom-anchor-drift fix is
            // relevant here) before trusting this 1:1.
            var center = e.Position;
            Session.OnPinchZoom((float)center.X, (float)center.Y, (float)e.Delta.Scale);
        }
    }

    public sealed class NavigationCompletedEventArgs : EventArgs
    {
        public string Url { get; }
        public bool IsSuccess { get; }

        public NavigationCompletedEventArgs(string url, bool isSuccess)
        {
            Url = url;
            IsSuccess = isSuccess;
        }
    }
}
