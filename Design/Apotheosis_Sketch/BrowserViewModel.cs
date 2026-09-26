using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Input;

namespace Apotheosis.Shell.ViewModels
{
    /// <summary>
    /// Deliberately hand-rolled instead of pulling in an MVVM toolkit —
    /// this skeleton should build with nothing beyond the UWP SDK and the
    /// EngineWinRT reference. Swap for CommunityToolkit.Mvvm's RelayCommand
    /// later if/when the shell grows past "one page, one engine host".
    /// </summary>
    public sealed class RelayCommand : ICommand
    {
        private readonly Action _execute;
        private readonly Func<bool> _canExecute;

        public RelayCommand(Action execute, Func<bool> canExecute = null)
        {
            _execute = execute ?? throw new ArgumentNullException(nameof(execute));
            _canExecute = canExecute;
        }

        public bool CanExecute(object parameter) => _canExecute?.Invoke() ?? true;
        public void Execute(object parameter) => _execute();
        public event EventHandler CanExecuteChanged;
        public void RaiseCanExecuteChanged() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>
    /// Holds only chrome state (address bar text, nav-button enablement,
    /// loading indicator, window title). It does NOT hold a reference to
    /// EngineSession — MainPage mediates between this view model and the
    /// EngineHost control, so this class stays testable without any WinRT
    /// projection in scope.
    /// </summary>
    public sealed class BrowserViewModel : INotifyPropertyChanged
    {
        private string _addressBarText = "https://";
        private string _pageTitle = "Apotheosis";
        private bool _isLoading;
        private bool _canGoBack;
        private bool _canGoForward;

        public string AddressBarText
        {
            get => _addressBarText;
            set => SetField(ref _addressBarText, value);
        }

        public string PageTitle
        {
            get => _pageTitle;
            set => SetField(ref _pageTitle, value);
        }

        public bool IsLoading
        {
            get => _isLoading;
            set => SetField(ref _isLoading, value);
        }

        public bool CanGoBack
        {
            get => _canGoBack;
            set { SetField(ref _canGoBack, value); GoBackCommand.RaiseCanExecuteChanged(); }
        }

        public bool CanGoForward
        {
            get => _canGoForward;
            set { SetField(ref _canGoForward, value); GoForwardCommand.RaiseCanExecuteChanged(); }
        }

        // Commands are exposed here and wired to real EngineHost calls in
        // MainPage's constructor — keeps this view model free of WinRT types.
        public RelayCommand NavigateCommand { get; }
        public RelayCommand GoBackCommand { get; }
        public RelayCommand GoForwardCommand { get; }
        public RelayCommand ReloadCommand { get; }
        public RelayCommand StopCommand { get; }

        public event Action<string> NavigateRequested;

        public BrowserViewModel()
        {
            NavigateCommand = new RelayCommand(() => NavigateRequested?.Invoke(AddressBarText));
            GoBackCommand = new RelayCommand(() => GoBackRequested?.Invoke(), () => CanGoBack);
            GoForwardCommand = new RelayCommand(() => GoForwardRequested?.Invoke(), () => CanGoForward);
            ReloadCommand = new RelayCommand(() => ReloadRequested?.Invoke());
            StopCommand = new RelayCommand(() => StopRequested?.Invoke());
        }

        public event Action GoBackRequested;
        public event Action GoForwardRequested;
        public event Action ReloadRequested;
        public event Action StopRequested;

        public event PropertyChangedEventHandler PropertyChanged;

        private void SetField<T>(ref T field, T value, [CallerMemberName] string propertyName = null)
        {
            if (Equals(field, value)) return;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
