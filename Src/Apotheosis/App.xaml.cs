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
            // Apotheosis 0.2.3.43: a terminating app must leave a cause behind. The .42 device run
            // ended with the engine idle and its RunLoop still ticking, no VEH line, no abort and no
            // crash report -- which is what BOTH an external lifecycle termination and a managed
            // fatal error look like. These two markers separate them: a suspension writes
            // `suspending`, a .NET Native unhandled exception writes `EXC <type>: <message>` before
            // the process goes down. Neither changes behaviour -- the exception is not marked
            // handled, so the app still terminates the way it did before.
            this.UnhandledException += OnUnhandledException;
            this.Resuming += OnResuming;
        }

        void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            try { BootMark("EXC " + e.Exception.GetType().Name + ": " + e.Exception.Message); } catch { }
        }

        void OnResuming(object sender, object e)
        {
            BootMark("resuming");
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
            BootMark("suspending");
            var deferral = e.SuspendingOperation.GetDeferral();
            deferral.Complete();
        }
    }
}
