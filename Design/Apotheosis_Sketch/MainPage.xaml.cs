using Apotheosis.EngineWinRT;
using Apotheosis.Shell.ViewModels;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;

namespace Apotheosis.Shell
{
    /// <summary>
    /// The mediator between chrome (BrowserViewModel) and engine (EngineHost).
    /// Neither of those two classes knows about the other — that's on purpose,
    /// so BrowserViewModel stays unit-testable and EngineHost stays reusable
    /// if a future tabbed-browsing version needs more than one of them.
    /// </summary>
    public sealed partial class MainPage : Page
    {
        public BrowserViewModel ViewModel { get; } = new BrowserViewModel();

        public MainPage()
        {
            InitializeComponent();

            ViewModel.NavigateRequested += url => Engine.Navigate(NormalizeUrl(url));
            ViewModel.GoBackRequested += () => Engine.GoBack();
            ViewModel.GoForwardRequested += () => Engine.GoForward();
            ViewModel.ReloadRequested += () => Engine.Reload();
            ViewModel.StopRequested += () => Engine.Stop();

            Engine.TitleChanged += (s, title) => ViewModel.PageTitle = title;

            Engine.NavigationCompleted += (s, args) =>
            {
                ViewModel.AddressBarText = args.Url;
                ViewModel.CanGoBack = Engine.Session.CanGoBack;
                ViewModel.CanGoForward = Engine.Session.CanGoForward;
                ViewModel.IsLoading = false;

                // TODO: on args.IsSuccess == false, surface a real error state
                // (e.g. a simple inline banner) rather than silently no-op'ing —
                // there's no error-page concept in this skeleton yet.
            };

            Engine.LoadStateChanged += (s, state) =>
            {
                ViewModel.IsLoading = state == EngineLoadState.Loading;
            };
        }

        private void OnAddressBarKeyDown(object sender, KeyRoutedEventArgs e)
        {
            // Ordinary XAML TextBox input handling — no engine/IME concerns
            // here at all. This box never touches EngineSession directly;
            // see the note in EngineHost.xaml.cs about why that boundary matters.
            if (e.Key == VirtualKey.Enter)
            {
                ViewModel.NavigateCommand.Execute(null);
            }
        }

        private static string NormalizeUrl(string input)
        {
            if (string.IsNullOrWhiteSpace(input)) return input;
            return input.Contains("://") ? input : $"https://{input}";
        }
    }
}
