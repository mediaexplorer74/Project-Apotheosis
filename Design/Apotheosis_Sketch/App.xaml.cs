using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Apotheosis.Shell
{
    /// <summary>
    /// Standard UWP bootstrap. Deliberately does nothing engine-related —
    /// EngineSession lifetime is owned by EngineHost (see Controls/EngineHost),
    /// not by App, so the shell can eventually support multiple tabs/windows
    /// each with their own engine session without touching this file.
    /// </summary>
    public sealed partial class App : Application
    {
        public App()
        {
            InitializeComponent();
            Suspending += OnSuspending;
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            Frame rootFrame = Window.Current.Content as Frame;

            if (rootFrame == null)
            {
                rootFrame = new Frame();
                rootFrame.NavigationFailed += OnNavigationFailed;
                Window.Current.Content = rootFrame;
            }

            if (e.PrelaunchActivated == false)
            {
                if (rootFrame.Content == null)
                {
                    rootFrame.Navigate(typeof(MainPage), e.Arguments);
                }
                Window.Current.Activate();
            }
        }

        private void OnNavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            throw new System.Exception("Failed to load Page " + e.SourcePageType.FullName);
        }

        private void OnSuspending(object sender, Windows.ApplicationModel.SuspendingEventArgs e)
        {
            // TODO: once EngineHost exposes a Suspend/Resume story on top of
            // WebCoreDriver (if the native session needs to release GPU
            // resources on suspend — check how the C++/CX harness currently
            // handles OS suspend before assuming this is a no-op).
        }
    }
}
