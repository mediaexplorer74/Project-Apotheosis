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

        // S6 (2026-10-21): bound the diagnostic traces by trimming the OLDEST HALF on open.
        //
        // The native side implements a 4 MB rotation (portTraceRotateIfNeeded in
        // Src/port/WebCoreDriver.cpp, mirrored in PortPlatformStrategies.cpp) and it has never
        // fired once in the life of an install: apoTraceAppend (WTF's RunLoopGeneric.cpp) holds
        // port-trace.txt open for the whole process, MSVC's fopen omits FILE_SHARE_DELETE, so the
        // rotation's rename() fails with a sharing violation and its handler is a bare `return`.
        // Measured on the Lumia 2026-10-09: port-trace.txt reached 6,223,695 B with no .1/.2 chunk
        // anywhere, still carrying sessions from four builds back. Fixing the native side means
        // editing WebKit sources, which costs a full rebuild on both lines; this trims the same
        // files from the managed side at the only moment they are provably unowned -- App() runs
        // before MainPage, before the first P/Invoke, i.e. before WebCoreDriver.dll is even
        // loaded. One halving per launch bounds the life-of-install growth the rotation was
        // meant to prevent; a single session still writes what it writes (measured ~2 MB worst
        // case), and the next launch halves the leftover.
        //
        // Trimmed: port-trace.txt (the measured offender), gpu-steps.txt (the marker channel,
        // same append-forever pattern) and autocmd-log.txt (appends ACROSS installs, so it is the
        // only file here whose history predates the current package). Each trim leaves a seam
        // marker naming the old size, so a pull can tell a trimmed file from a short one; the
        // per-launch record goes to boot.txt as `trim <file> <was> -> <now>`. Byte-exact rewrite:
        // the seam is ASCII, the tail is copied verbatim from just past the first newline at or
        // after the midpoint (or from the raw midpoint when the file has no newline there -- a
        // single giant line must not trim itself to nothing).
        const long kTraceTrimCeiling = 4 * 1024 * 1024;

        static void TrimTraceOldestHalf()
        {
            string dir;
            try { dir = Windows.Storage.ApplicationData.Current.LocalFolder.Path; }
            catch { return; }
            foreach (string name in new[] { "port-trace.txt", "gpu-steps.txt", "autocmd-log.txt" })
            {
                try
                {
                    string path = System.IO.Path.Combine(dir, name);
                    var fi = new System.IO.FileInfo(path);
                    if (!fi.Exists || fi.Length <= kTraceTrimCeiling) continue;
                    long was = fi.Length;
                    byte[] all = System.IO.File.ReadAllBytes(path);
                    int mid = all.Length / 2;
                    int start = mid;
                    while (start < all.Length && all[start] != (byte)'\n') start++;
                    if (start < all.Length) start++;            // keep whole lines only
                    else start = mid;                          // one giant line: raw half, no loss
                    byte[] head = System.Text.Encoding.ASCII.GetBytes(
                        "trim: oldest half dropped on open (was " + was + " bytes)" + "\r\n");
                    byte[] outb = new byte[head.Length + (all.Length - start)];
                    System.Buffer.BlockCopy(head, 0, outb, 0, head.Length);
                    System.Buffer.BlockCopy(all, start, outb, head.Length, all.Length - start);
                    System.IO.File.WriteAllBytes(path, outb);
                    BootMark("trim " + name + " " + was + " -> " + outb.Length);
                }
                catch { }   // a trim failure must never keep the app from starting
            }
        }

        public App()
        {
            BootMark("app-ctor");
            TrimTraceOldestHalf();
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
