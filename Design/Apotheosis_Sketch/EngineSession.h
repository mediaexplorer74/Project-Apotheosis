#pragma once
#include "EngineSession.g.h"
#include <winrt/Microsoft.UI.Dispatching.h>

namespace winrt::Apotheosis::EngineWinRT::implementation
{
    struct EngineSession : EngineSessionT<EngineSession>
    {
        EngineSession();
        ~EngineSession();

        void Initialize();
        void Shutdown();

        void SetSwapChainPanel(Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);
        void Resize(float width, float height);

        void Navigate(hstring const& url);
        void GoBack();
        void GoForward();
        void Reload();
        void Stop();

        bool CanGoBack();
        bool CanGoForward();
        hstring CurrentUrl();
        Apotheosis::EngineWinRT::EngineLoadState LoadState();

        void OnPointerPressed(float x, float y);
        void OnPointerMoved(float x, float y);
        void OnPointerReleased(float x, float y);
        void OnPinchZoom(float centerX, float centerY, float scaleDelta);

        winrt::event_token NavigationCompleted(
            Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::NavigationEventArgs> const& handler);
        void NavigationCompleted(winrt::event_token const& token) noexcept;

        winrt::event_token TitleChanged(
            Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, hstring> const& handler);
        void TitleChanged(winrt::event_token const& token) noexcept;

        winrt::event_token LoadStateChanged(
            Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::EngineLoadState> const& handler);
        void LoadStateChanged(winrt::event_token const& token) noexcept;

    private:
        // Static trampolines: WebCoreDriver's C ABI takes plain function
        // pointers + a context void*, not member functions. These convert
        // the C callback back into a call on `this`, still on the engine
        // thread — do NOT raise WinRT events from inside these directly.
        static void CALLBACK OnNativeNavigationCallback(void* context, const wchar_t* url, bool success);
        static void CALLBACK OnNativeTitleCallback(void* context, const wchar_t* title);
        static void CALLBACK OnNativeLoadStateCallback(void* context, int state);

        // Instance-side handlers. These run on the engine thread and are
        // responsible for hopping to the UI thread before touching any
        // WinRT event or member state that C# might be observing.
        void HandleNativeNavigation(hstring url, bool success);
        void HandleNativeTitleChanged(hstring title);
        void HandleNativeLoadStateChanged(Apotheosis::EngineWinRT::EngineLoadState state);

        // TODO: replace void* with the real WebCoreDriver session handle
        // type once this project references port/WebCoreDriver.h directly.
        void* m_nativeSession{ nullptr };

        // Captured at Initialize() time on the UI thread. Every native
        // callback must marshal through this before raising events.
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_uiDispatcher{ nullptr };

        winrt::com_ptr<::ISwapChainPanelNative> m_swapChainPanelNative;

        hstring m_currentUrl;
        hstring m_currentTitle;
        Apotheosis::EngineWinRT::EngineLoadState m_loadState{ Apotheosis::EngineWinRT::EngineLoadState::Idle };
        bool m_canGoBack{ false };
        bool m_canGoForward{ false };

        winrt::event<Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::NavigationEventArgs>> m_navigationCompleted;
        winrt::event<Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, hstring>> m_titleChanged;
        winrt::event<Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::EngineLoadState>> m_loadStateChanged;
    };
}

namespace winrt::Apotheosis::EngineWinRT::factory_implementation
{
    struct EngineSession : EngineSessionT<EngineSession, implementation::EngineSession>
    {
    };
}
