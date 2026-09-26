#include "pch.h"
#include "EngineSession.h"
#include "EngineSession.g.cpp"
#include "NavigationEventArgs.g.cpp"

// ---------------------------------------------------------------------------
// TODO — replace this block with `#include "WebCoreDriver.h"` from port/,
// once this project has an include path to it. Signatures below are
// ILLUSTRATIVE placeholders reconstructed from the architecture notes
// (persistent Page/Frame session, C ABI boundary) — swap for the real ones.
// ---------------------------------------------------------------------------
extern "C"
{
    void* WebCoreDriver_CreateSession();
    void  WebCoreDriver_DestroySession(void* session);

    void  WebCoreDriver_LoadUrl(void* session, const wchar_t* url);
    void  WebCoreDriver_GoBack(void* session);
    void  WebCoreDriver_GoForward(void* session);
    void  WebCoreDriver_Reload(void* session);
    void  WebCoreDriver_Stop(void* session);
    bool  WebCoreDriver_CanGoBack(void* session);
    bool  WebCoreDriver_CanGoForward(void* session);

    void  WebCoreDriver_Resize(void* session, float width, float height);
    // Returns the DXGI swap chain WebCore's ANGLE/TextureMapper GPU path
    // renders into, once a size is known. Null before the first Resize().
    IDXGISwapChain1* WebCoreDriver_GetSwapChain(void* session);

    enum class PointerEventType : int { Pressed = 0, Moved = 1, Released = 2 };
    void  WebCoreDriver_HandlePointer(void* session, PointerEventType type, float x, float y);
    void  WebCoreDriver_HandlePinchZoom(void* session, float centerX, float centerY, float scaleDelta);

    using WebCoreDriver_NavCallback = void(CALLBACK*)(void* context, const wchar_t* url, bool success);
    using WebCoreDriver_TitleCallback = void(CALLBACK*)(void* context, const wchar_t* title);
    using WebCoreDriver_LoadStateCallback = void(CALLBACK*)(void* context, int state);

    // IMPORTANT (per the port/ architecture notes): these fire on WebCore's
    // own engine thread, not the caller's thread. That is exactly why this
    // whole class exists — see the dispatcher hop in each Handle* method below.
    void WebCoreDriver_SetNavigationCallback(void* session, WebCoreDriver_NavCallback cb, void* context);
    void WebCoreDriver_SetTitleCallback(void* session, WebCoreDriver_TitleCallback cb, void* context);
    void WebCoreDriver_SetLoadStateCallback(void* session, WebCoreDriver_LoadStateCallback cb, void* context);
}
// ---------------------------------------------------------------------------

namespace winrt::Apotheosis::EngineWinRT::implementation
{
    EngineSession::EngineSession() = default;

    EngineSession::~EngineSession()
    {
        Shutdown();
    }

    void EngineSession::Initialize()
    {
        // Capture the UI thread's dispatcher now, while we know for certain
        // we're being called from it. Every native callback marshals back
        // through this, never through a freshly-fetched dispatcher later.
        m_uiDispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        m_nativeSession = WebCoreDriver_CreateSession();

        WebCoreDriver_SetNavigationCallback(m_nativeSession, &EngineSession::OnNativeNavigationCallback, this);
        WebCoreDriver_SetTitleCallback(m_nativeSession, &EngineSession::OnNativeTitleCallback, this);
        WebCoreDriver_SetLoadStateCallback(m_nativeSession, &EngineSession::OnNativeLoadStateCallback, this);
    }

    void EngineSession::Shutdown()
    {
        if (m_nativeSession)
        {
            WebCoreDriver_DestroySession(m_nativeSession);
            m_nativeSession = nullptr;
        }
    }

    void EngineSession::SetSwapChainPanel(Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
    {
        // This QueryInterface is the one thing C# genuinely cannot do on its
        // own — ISwapChainPanelNative is not WinRT-projectable. Everything
        // else in this class could theoretically be P/Invoke from C#; this
        // method is the actual reason EngineWinRT exists as a native project.
        m_swapChainPanelNative = panel.as<::ISwapChainPanelNative>();

        // TODO: once a size is available (see Resize below), fetch the
        // real swap chain from WebCoreDriver and attach it:
        //
        //   if (auto swapChain = WebCoreDriver_GetSwapChain(m_nativeSession))
        //   {
        //       winrt::check_hresult(m_swapChainPanelNative->SetSwapChain(swapChain));
        //   }
        //
        // Port the exact call site from the existing C++/CX harness's
        // equivalent code (this is a direct line-for-line port, not new logic).
    }

    void EngineSession::Resize(float width, float height)
    {
        WebCoreDriver_Resize(m_nativeSession, width, height);

        // Re-attach the swap chain after resize if this is the first time
        // one becomes available (mirrors the harness's existing behavior of
        // deferring GPU setup until the panel has a non-zero size).
        if (m_swapChainPanelNative)
        {
            if (auto swapChain = WebCoreDriver_GetSwapChain(m_nativeSession))
            {
                winrt::check_hresult(m_swapChainPanelNative->SetSwapChain(swapChain));
            }
        }
    }

    void EngineSession::Navigate(hstring const& url)
    {
        m_currentUrl = url;
        WebCoreDriver_LoadUrl(m_nativeSession, url.c_str());
    }

    void EngineSession::GoBack() { WebCoreDriver_GoBack(m_nativeSession); }
    void EngineSession::GoForward() { WebCoreDriver_GoForward(m_nativeSession); }
    void EngineSession::Reload() { WebCoreDriver_Reload(m_nativeSession); }
    void EngineSession::Stop() { WebCoreDriver_Stop(m_nativeSession); }

    bool EngineSession::CanGoBack() { return m_canGoBack; }
    bool EngineSession::CanGoForward() { return m_canGoForward; }
    hstring EngineSession::CurrentUrl() { return m_currentUrl; }
    Apotheosis::EngineWinRT::EngineLoadState EngineSession::LoadState() { return m_loadState; }

    void EngineSession::OnPointerPressed(float x, float y)
    {
        WebCoreDriver_HandlePointer(m_nativeSession, PointerEventType::Pressed, x, y);
    }

    void EngineSession::OnPointerMoved(float x, float y)
    {
        WebCoreDriver_HandlePointer(m_nativeSession, PointerEventType::Moved, x, y);
    }

    void EngineSession::OnPointerReleased(float x, float y)
    {
        WebCoreDriver_HandlePointer(m_nativeSession, PointerEventType::Released, x, y);
    }

    void EngineSession::OnPinchZoom(float centerX, float centerY, float scaleDelta)
    {
        WebCoreDriver_HandlePinchZoom(m_nativeSession, centerX, centerY, scaleDelta);
    }

    // -----------------------------------------------------------------------
    // Native callback trampolines (engine thread) → dispatcher hop → WinRT
    // event (UI thread). This split is the single most important thing in
    // this file. Do not shortcut it, even for a "just testing" build — the
    // text-input and image-refresh bugs fixed upstream in v0.1.7.29 were
    // exactly this class of bug (state mutated / repaint triggered from the
    // wrong place at the wrong time).
    // -----------------------------------------------------------------------

    void CALLBACK EngineSession::OnNativeNavigationCallback(void* context, const wchar_t* url, bool success)
    {
        auto* self = static_cast<EngineSession*>(context);
        self->HandleNativeNavigation(hstring{ url }, success);
    }

    void CALLBACK EngineSession::OnNativeTitleCallback(void* context, const wchar_t* title)
    {
        auto* self = static_cast<EngineSession*>(context);
        self->HandleNativeTitleChanged(hstring{ title });
    }

    void CALLBACK EngineSession::OnNativeLoadStateCallback(void* context, int state)
    {
        auto* self = static_cast<EngineSession*>(context);
        self->HandleNativeLoadStateChanged(static_cast<Apotheosis::EngineWinRT::EngineLoadState>(state));
    }

    void EngineSession::HandleNativeNavigation(hstring url, bool success)
    {
        auto strong = get_strong();
        m_uiDispatcher.TryEnqueue([strong, url, success]()
        {
            strong->m_currentUrl = url;
            strong->m_canGoBack = WebCoreDriver_CanGoBack(strong->m_nativeSession);
            strong->m_canGoForward = WebCoreDriver_CanGoForward(strong->m_nativeSession);

            auto args = winrt::make<Apotheosis::EngineWinRT::implementation::NavigationEventArgs>(url, success);
            strong->m_navigationCompleted(*strong, args);
        });
    }

    void EngineSession::HandleNativeTitleChanged(hstring title)
    {
        auto strong = get_strong();
        m_uiDispatcher.TryEnqueue([strong, title]()
        {
            strong->m_currentTitle = title;
            strong->m_titleChanged(*strong, title);
        });
    }

    void EngineSession::HandleNativeLoadStateChanged(Apotheosis::EngineWinRT::EngineLoadState state)
    {
        auto strong = get_strong();
        m_uiDispatcher.TryEnqueue([strong, state]()
        {
            strong->m_loadState = state;
            strong->m_loadStateChanged(*strong, state);
        });
    }

    // Event plumbing (standard C++/WinRT boilerplate)
    winrt::event_token EngineSession::NavigationCompleted(
        Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::NavigationEventArgs> const& handler)
    {
        return m_navigationCompleted.add(handler);
    }
    void EngineSession::NavigationCompleted(winrt::event_token const& token) noexcept
    {
        m_navigationCompleted.remove(token);
    }

    winrt::event_token EngineSession::TitleChanged(
        Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, hstring> const& handler)
    {
        return m_titleChanged.add(handler);
    }
    void EngineSession::TitleChanged(winrt::event_token const& token) noexcept
    {
        m_titleChanged.remove(token);
    }

    winrt::event_token EngineSession::LoadStateChanged(
        Windows::Foundation::TypedEventHandler<Apotheosis::EngineWinRT::EngineSession, Apotheosis::EngineWinRT::EngineLoadState> const& handler)
    {
        return m_loadStateChanged.add(handler);
    }
    void EngineSession::LoadStateChanged(winrt::event_token const& token) noexcept
    {
        m_loadStateChanged.remove(token);
    }
}
