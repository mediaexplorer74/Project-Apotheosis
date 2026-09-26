using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Apotheosis
{
    sealed partial class App : Application
    {
        // Startup breadcrumbs. After a launch attempt, LocalState\boot.txt must contain
        // "app-ctor"; if the file is missing entirely, the process died before managed
        // code ran (.NET Native / loader stage), not inside our code.
        internal static void BootMark(string stage)
        {
            try
            {
                string dir = Windows.Storage.ApplicationData.Current.LocalFolder.Path;
                System.IO.File.AppendAllText(System.IO.Path.Combine(dir, "boot.txt"),
                    System.DateTime.Now.ToString("HH:mm:ss.fff") + " " + stage + System.Environment.NewLine);
            }
            catch { }
        }

        public App()
        {
            BootMark("app-ctor");
            this.InitializeComponent();
            this.Suspending += OnSuspending;
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            BootMark("onlaunched");
            var rootFrame = Window.Current.Content as Frame;
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

        void OnNavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            throw new System.Exception("Failed to load Page " + e.SourcePageType.FullName);
        }

        private void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            deferral.Complete();
        }
    }
}
