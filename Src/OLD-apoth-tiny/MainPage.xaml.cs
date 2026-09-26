using System;
using System.Runtime.InteropServices;
using Windows.UI.Xaml.Controls;

namespace ApothTiny
{
    public sealed partial class MainPage : Page
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string name);

        public MainPage()
        {
            App.BootMark("mainpage-ctor");
            InitializeComponent();
            App.BootMark("mainpage-xaml-ok");
            IntPtr h = LoadLibraryW("WebCoreDriver.dll");
            int err = Marshal.GetLastWin32Error();
            App.BootMark("engine-load handle=" + h.ToInt64() + " err=" + err);
            Txt.Text = h == IntPtr.Zero ? ("engine load failed err=" + err) : "engine load OK";
        }
    }
}
