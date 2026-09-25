#pragma once
#include "MainPage.g.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>

// Apotheosis: crash verdict, called from App::OnLaunched before the engine thread exists. Declared
// at global scope to match the definition in MainPage.xaml.cpp (file-scope functions there are
// global, not Harness:: -- a namespace mismatch would only surface at link time).
void RunCrashVerdict();

namespace Harness {

    // Bookmark/history/download entry (native struct, not WinRT).
    struct Entry {
        std::wstring url;
        std::wstring title;
        std::wstring extra;   // History: time; Download: filename/status
    };

    enum class DrawerTab { Favorites, History, Downloads };

    // Web page link hit rectangle (bitmap coords) + URL, for click interaction.
    struct PageLink { int x, y, w, h; std::wstring url; };

    // Tab (Mode A: single hot session + snapshot). Only the active tab has a live engine session; others store state only, reload on switch.
    // The active tab's "live state" is represented by MainPage's existing global members; swapped with this struct on switch.
    struct Tab {
        std::vector<std::wstring> navStack;
        int navIndex { -1 };
        std::wstring currentUrl { L"about:home" };
        std::wstring currentTitle;
        float pageScale { 1.0f };
    };

    public ref class MainPage sealed {
    public:
        MainPage();

    private:
        // Apotheosis: crash verdict, called from App::OnLaunched before this page exists
        // (defined in MainPage.xaml.cpp).
        void OnHeartbeat(Platform::Object^ sender, Platform::Object^ e);
        // ---- Toolbar ----
        void OnBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnForward(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnHome(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnGo(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnUrlKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnMenu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // URL bar right context button: loading=Stop ✕ (cancel in-flight + cancel network), uncommitted input=Go →, otherwise=Refresh ⟳.
        void OnUrlAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Reload();
        void UpdateUrlActionGlyph();   // Switch ✕/→/⟳ based on m_loading / whether there is uncommitted input
        void UpdateLockIcon();         // Set security indicator per m_currentUrl protocol (https=lock/http=warning)
        // URL bar input changes: refresh context button + show/hide history+bookmark suggestion dropdown.
        void OnUrlChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void ShowSuggestions(const std::wstring& query);
        void HideSuggestions();
        void OnUrlGotFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnUrlLostFocus(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

        // ---- Action panel (action sheet from menu key)----
        void ShowActionMenu();
        void HideActionMenu();
        void OnActionScrimTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnSheetTap(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);   // Dispatch by Button.Tag
        void ToggleBookmark();
        void DoShare();
        void DoCopyLink();
        void DoToggleUA();

        // ---- Settings page ----
        void ShowSettings();
        void HideSettings();
        void OnSettingsBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSettingsBtn(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);   // tag: clearhist/clearfav/cleardl/export/gpu
        void OnZoomChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs^ e);
        void OnLangChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void LoadSettings();
        void SaveSettings();
        void ApplySettings();
        void ExportDebug();
        void ApplyLanguage();   // Apply UI translation per m_uiLang
        // Apotheosis: take or drop the DisplayRequest that keeps the screen awake, per m_keepAwake and
        // whether the app currently has the foreground. Called from the constructor and from
        // VisibilityChanged, so the request follows focus rather than outliving it.
        void UpdateKeepAwake(bool foreground);
        // Apotheosis: watch LocalState\nav.txt and navigate when it changes, so the *harness* navigation
        // path -- NavigateTo, the parking logic, NavRetry, the GPU-enable callback -- can be driven from
        // a script. autodiag.txt already drives WebCoreSessionLoad directly, which bypasses all of that
        // and therefore cannot reproduce defects that live in it. Established on 2026-08-21: four loads
        // through autodiag all painted, while the same sequence by hand on the device went blank after
        // the first, which is what pointed at the harness path rather than the engine.
        void StartNavWatch();
        // Apotheosis: play the packaged navigation sequence (Assets\navseq.txt) through the harness path.
        // Exists because Device Portal on the Lumia cannot write to LocalState, so nav.txt -- fine on the
        // bench -- is undeliverable there, and the appx is the only channel that reaches the device.
        void StartNavSeq();
        // Check for update: background thread fetches GitHub Releases API, compares version; when manual=true, prompt even if no update/failure.
        void CheckForUpdate(bool manual);

        // ---- Diagnostics viewer (full-screen) ----
        // Apotheosis: on a real Lumia there is no debugger (VS 2022 dropped ARM32 device debugging)
        // and the App Container hides LocalState from the user, so this page is the only way to read
        // the engine's own logs on the device. Clipboard and share need no extra capability, which is
        // why they are the primary export routes; the file picker is kept for the SD card.
        void ShowDiagPage();
        void HideDiagPage();
        void OnDiagBack(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnDiagFileChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void OnDiagBtn(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);   // tag: refresh/copy/share/save
        void LoadDiagFile();                    // fill DiagText from the selected file, tail only
        std::string ReadDiagTail(const char* name, size_t maxBytes, size_t* outTotal);
        std::string BuildDiagReport();          // every log concatenated, for copy/share/save

        // ---- Find in page ----
        void ShowFindBar();
        void OnFindChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void OnFindKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OnFindNext(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnFindPrev(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnFindClose(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void DoFind(int mode);   // 0=find (mark all) 1=next 2=previous

        // ---- Tabs (Mode A)----
        void OnTabs(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnNewTab(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabSwitcherDone(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ShowTabSwitcher();
        void HideTabSwitcher();
        void RebuildTabSwitcher();
        void SaveActiveTab();       // Current global state → m_tabs[m_activeTab]
        void RestoreTab(int i);     // m_tabs[i] → global state + reload that tab's URL (rebuild session)
        void NewTab();
        void CloseTab(int i);
        void SwitchTab(int i);
        void UpdateTabCount();
        // UA toggle: mobile/desktop, reloads current page after switching (use for sites that misbehave on mobile UA).
        void OnToggleUA(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // GPU compositing toggle (M2): one-shot enable (engine thread WebCoreGpuInit offscreen success → reload current page via TextureMapper compositing).
        void OnToggleGpu(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void EnableGpu();   // Enable GPU direct rendering (shared by OnToggleGpu first click + default GPU auto-trigger; includes crash loop protection)
        // Apotheosis 2026-09-19: ask the engine whether the CURRENT document can be presented through the
        // GPU at all, and hand the screen back to software if it cannot. The GPU is a property of the
        // document, not of the process -- see the definition in the .cpp for the photograph that proved it.
        void ReevaluateGpuForDocument();

        // ---- Drawer ----
        void OnDrawerClose(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabFav(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabHist(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTabDl(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnPrimaryAction(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

        // ---- Navigation ----
        void NavigateTo(Platform::String^ url, bool pushHistory);
        // Apotheosis: arm the retry that replays m_pendNavUrl once the engine stops being busy.
        void ScheduleNavRetry();
        void UpdateNavButtons();
        void SetLoading(bool loading);
        // Apotheosis 2026-09-19: `documentUrl` is the url of the DOCUMENT THE ENGINE ACTUALLY LOADED,
        // read on the engine thread next to the same WebCoreGetUrl call that feeds the empty-page
        // notice. It differs from m_currentUrl whenever the site redirected or the page navigated
        // itself, and the GPU auto-probe keys on it -- see the probe's comment in the .cpp.
        void OnNavDone(Platform::String^ finalTitle, bool ok, bool loadOk, Platform::String^ documentUrl);
        void OnLoadWatchdog(Platform::Object^ sender, Platform::Object^ e);
        // Page tap: if session exists, forward tap to engine (buttons/forms/links all go through real events); no session (home page) use link table.
        void OnPageTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        // Apotheosis: the whole body of a tap, split out of OnPageTapped so it can also be driven from a
        // script (`tap:`/`taplink:` lines in LocalState\nav.txt). The two entry points must stay one
        // function -- a tap that a script reaches by a different route is a tap that verifies nothing.
        void HandleTapAt(double dipX, double dipY, bool scripted);
        // Apotheosis: parse and run one scripted tap command. Logs its own outcome on every path,
        // including refusal, because a scripted tap that silently does nothing is indistinguishable
        // from a fix that did not work.
        void TapFromScript(const std::string& cmd);
        // Map content area display coordinates (DIP) back to engine pixel space (direct rendering surface is stretched + device resolution scaling), fix click/focus offset.
        void MapTapToEngine(double dipX, double dipY, int& outPx, int& outPy);
        // Forward click at bitmap pixel (px,py) to engine live session (WebCoreClickAt), after completion sync URL bar/history/link table.
        void ForwardClickToEngine(int px, int py);
        // Engine scroll dy pixels (triggers lazy image loading) then redraw. dy>0 scrolls down.
        void EngineScroll(int dy);
        void OnScrollUp(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnScrollDown(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // Free scroll: content area ManipulationDelta (single finger drag ΔY) → accumulate displacement → merge into engine scroll (no spinner, with inertia).
        void FreeScrollBy(int dx, int dy);   // Accumulate dx/dy and flush when engine is idle
        void PumpScroll();           // Dispatch accumulated displacement as one WebCoreScrollBy (re-dispatch if still accumulated after completion)
        void SyncLinksAfterScroll(); // One-time refresh of link hit table after scroll stops (extractLinks was skipped during scroll)
        void OnImageManipDelta(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs^ e);
        // M4 pinch zoom: during pinch, apply real-time ScaleTransform to display layer (zero engine), on release commit to engine to re-raster at new scale (text sharp).
        void OnImageManipCompleted(Platform::Object^ sender, Windows::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs^ e);
        void ApplyLiveZoom();
        void PinchCommit(float newScale, int focalX, int focalY);
        // Live render loop: drive engine WebCoreLiveTick at low framerate, animate CSS/JS animations, SPA multi-frame progressive mount.
        // Auto-stop frames when image is static to save power, restart on interaction/scroll/navigation.
        void StartLiveMode();
        void StopLiveMode();
        void OnLiveTick(Platform::Object^ sender, Platform::Object^ e);
        // IME: tap on editable element to invoke on-screen keyboard; keystrokes forwarded to engine live session.
        void OnImeTextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
        void OnImeKeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void OpenKeyboard();
        void CloseKeyboard();
        // kind: 0=insert text, 1=enter, 2=backspace. Forward to engine and redraw.
        void SendKeyToEngine(int kind, Platform::String^ text);
        // GPU path1 probe: after SwapChainPanel is ready, launch ANGLE triangle probe (verify GPU pipeline works in App Container).
        void OnGpuPanelLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        // Paste one frame of engine render result (rgba) to bitmap + sync title/URL/link table; navUrl non-null indicates navigation occurred within the session.
        void ApplyEngineFrame(const std::shared_ptr<std::vector<uint8_t>>& rgba,
                              Platform::String^ title, Platform::String^ navUrl,
                              const std::shared_ptr<std::vector<PageLink>>& links);

        // ---- Viewport sizing ----
        // ContentArea changed size: restart the debounce timer (a window drag fires this
        // continuously and each resize costs the engine a full relayout + repaint).
        void OnContentAreaSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
        // Push ContentArea's size into the engine viewport and repaint at the new size.
        void ApplyViewportSize();
        // Apotheosis 2026-09-19: the panel's DPI scale, 1.0 when it has none yet. The GPU path sizes
        // its surface, the engine viewport and kW/kH in PHYSICAL pixels; software stays in DIPs.
        double GpuPixelScaleX();
        double GpuPixelScaleY();
        // One line of every number that decides how the frame is sized, so a stretched or cropped
        // page can be read from the log instead of deduced from a photograph.
        void LogViewportMetrics(const char* tag);
        // Apotheosis 2026-09-19: tell the engine what CSS page zoom its viewport implies -- the
        // panel's CompositionScale in GPU mode, 1.0 in software. Never repaints; the caller resizes
        // and repaints right after (see the definition).
        void PushPageZoom();

        // ---- Drawer UI ----
        void ShowDrawer(DrawerTab tab);
        void HideDrawer();
        void RebuildDrawerList();
        void StartDownload(Platform::String^ url);

        // ---- Data + Persistence ----
        void LoadData();
        void SaveBookmarks();
        void SaveHistory();
        void SaveDownloads();
        void AddHistory(const std::wstring& url, const std::wstring& title);
        bool IsBookmarked(const std::wstring& url);

        // Browser's own history (back/forward URL stack, distinct from the "history" list)
        std::vector<std::wstring> m_navStack;
        int m_navIndex { -1 };
        bool m_loading { false };

        std::vector<Entry> m_bookmarks;   // Bookmarks
        std::vector<Entry> m_historyList; // History (newest first)
        std::vector<Entry> m_downloads;   // Downloads
        DrawerTab m_tab { DrawerTab::Favorites };

        std::wstring m_currentUrl;
        std::wstring m_currentTitle;
        std::vector<PageLink> m_pageLinks;   // Current page link hit table (click interaction)
        bool m_sessionActive { false };      // Whether there is an engine resident session (web page=yes, home/error page=no)
        bool m_interacting { false };        // Currently forwarding click/scroll to engine (anti-reentrancy, UI side)
        // Operation sequence: incremented on each navigation/click/scroll. Callback on UI thread checks whether captured sequence still equals latest; if outdated (watchdog
        // forced reset then new operation started, or superseded by new operation) discard, avoiding late callback overwriting new operation state → permanent freeze.
        unsigned long long m_opSeq { 0 };
        // IME state
        std::wstring m_lastImeText;   // ImeBox last text (for incremental forwarding)
        bool m_imeOpen { false };     // Whether on-screen keyboard is open for current input
        Windows::UI::Xaml::Controls::TextBlock^ m_startupOverlay { nullptr };
        // Apotheosis (2026-09-18): "empty page" notice. A load that produced no content must not be
        // shown as a bare white window -- to a user that is indistinguishable from a crash. Built in
        // code (never in XAML) so MainPage.g.hpp stays generated and verify-xaml-connect stays green.
        Windows::UI::Xaml::Controls::Border^ m_emptyOverlay { nullptr };
        Windows::UI::Xaml::Controls::TextBlock^ m_emptyText { nullptr };
        bool m_emptyNoticeShown { false };
        void ShowEmptyPageNotice(const std::wstring& url, const std::wstring& requested, int bodyKids);
        void HideEmptyPageNotice();
        bool m_imeSyncing { false };  // Programmatically changing ImeBox.Text (avoid TextChanged loopback)
        bool m_uaMobile { true };     // UA mode: true=mobile (default), false=desktop
        bool m_urlSyncing { false };  // Programmatically changing UrlBox.Text (navigation/callback sync URL bar) → suppress suggestion dropdown loopback
        bool m_urlFocused { false };  // Whether URL bar is focused (editing) → only show suggestions when focused, prevent "unexpected popups"
        bool m_updateChecking { false };  // Update check in progress (prevent concurrent duplicate clicks)
        // Apotheosis: when non-empty, the DataRequested handler shares this text instead of the page
        // link. Set right before ShowShareUI from the diagnostics page and cleared once consumed, so
        // the ordinary "share this page" path keeps working untouched.
        std::wstring m_pendingShareText;
        bool m_updateAutoChecked { false };  // Silent auto-check done once after startup (triggered when first web page finishes loading, CA is ready by then)
        // Settings (persisted to LocalState\settings.ini; search prefix/homepage are global, see .cpp)
        int  m_setSearch { 0 };       // Search engine index (0 Bing/1 Google/2 DuckDuckGo/3 Baidu)
        bool m_setUaDesktop { false };// Start with desktop-site request by default
        int  m_defaultZoom { 100 };   // Default zoom percentage (50–200)
        int  m_tabMode { 0 };         // 0=single hot session / 1=concurrent multi-engine (experimental); incremental 5/7 usage
        std::wstring m_uaCustom;      // Custom UA (empty=use mobile/desktop toggle); settings.ini ua_custom
        // Tab collection (Mode A: only active tab has engine session).
        std::vector<Tab> m_tabs;
        int m_activeTab { 0 };
        bool m_gpuOn { false };       // Whether GPU compositing is enabled (one-shot; engine-side g_gpuActive has no teardown, restart reverts to software)
        bool m_gpuPresent { false };  // GPU direct rendering mode (composite directly to GpuPanel, saves readback+blit)
        int  m_uiLang { 1 };          // 0=Chinese 1=English 2=Russian
        bool m_gpuDefault { true };   // GPU enabled by default (can be turned off in settings; automatically enabled after first web page loads)
        // Apotheosis: keepawake=1 in settings.ini. Holds a DisplayRequest so the screen does not blank
        // while the browser runs, which is what makes remote testing on the Lumia possible at all: a
        // suspended app is reaped by the OS and its log then claims a clean exit for a run that never
        // rendered. Debug aid, off by default. The request is released when the app leaves the
        // foreground so it cannot pin the screen on behind the user's back.
        bool m_keepAwake { false };
        Windows::System::Display::DisplayRequest^ m_displayRequest { nullptr };
        bool m_displayRequestActive { false };
        // Apotheosis: content toggles (Settings page). JS off = lightweight mode for heavy SPA
        // sites that would otherwise kill the process; images off saves bandwidth and decode time.
        bool m_jsEnabled { true };
        bool m_imagesEnabled { true };
        // Apotheosis: nav.txt watcher state. The stamp is the file's last-write time, seeded at startup
        // so that an existing nav.txt does not fire a navigation on launch -- only changes made while
        // the app runs do.
        Windows::UI::Xaml::DispatcherTimer^ m_navWatch { nullptr };
        unsigned long long m_navWatchStamp { 0 };
        // Apotheosis: navseq player state. Each entry is a delay in seconds and the URL to go to after it.
        Windows::UI::Xaml::DispatcherTimer^ m_navSeqTimer { nullptr };
        std::vector<std::pair<int, std::wstring>> m_navSeq;
        size_t m_navSeqIndex { 0 };
        // Apotheosis 2026-08-29: replaces `bool m_gpuAutoTried`, which latched once per process and so
        // let the first page loaded decide for the whole session whether the GPU could present. Whether
        // a root compositing layer exists is a property of the page, so the probe is retried per url.
        std::wstring m_gpuTriedForUrl;  // url the GPU auto-probe last ran for (empty = never)
        Windows::Foundation::Collections::PropertySet^ m_gpuProps;  // ANGLE native window (SwapChainPanel wrapper), keep-alive
        int  m_gpuOrient { 0 };       // Offscreen readback orientation (bit0=H,bit1=V): 0=none (confirmed correct on real device), 1=H, 2=V, 3=HV
        // Free scroll state
        int  m_scrollAccum { 0 };     // Unflushed accumulated vertical scroll displacement (pixels, >0 scrolls down)
        int  m_scrollAccumX { 0 };    // Unflushed accumulated horizontal scroll displacement (pixels, >0 scrolls right)
        bool m_scrollBusy { false };  // A WebCoreScrollBy task is in-flight on the engine thread
        // M4 pinch zoom state
        bool   m_pinching { false };   // Pinching (two-finger Scale gesture); during pinch only transform display layer, commit to engine on release
        float  m_liveScale { 1.0f };   // Real-time scale relative to "committed scale" during pinch (for RenderTransform)
        float  m_pageScale { 1.0f };   // Page scale factor committed to engine (Page::pageScaleFactor)
        double m_focalX { 360 }, m_focalY { 540 };  // Pinch focal point (ContentArea/viewport coordinates)
        bool m_pointerDown { false }; // Pointer is pressed (drag tracking)
        bool m_dragging { false };    // Exceeded threshold, determined to be a drag (not a click)
        double m_dragLastY { 0 };     // Last pointer Y (for delta calculation)
        double m_dragStartY { 0 };    // Pointer Y at press (to check if drag threshold exceeded)

        // Load watchdog: ensures m_loading can always be reset (even if completion callback is lost due to dispatcher disconnect/low memory,
        // prevents navigation from being permanently locked).
        Windows::UI::Xaml::DispatcherTimer^ m_loadWatchdog;
        // Apotheosis: 2 s heartbeat. A UI-thread timer writes heartbeat.txt every tick; the crash
        // verdict at the next launch (RunCrashVerdict) reads its timestamp together with exit-ok.txt
        // to tell a crash from a clean exit, and a stale heartbeat localises the death to a window.
        Windows::UI::Xaml::DispatcherTimer^ m_heartbeatTimer;
        // Viewport resize debounce: a window drag raises SizeChanged continuously, and every
        // distinct size costs the engine a relayout + full repaint, so only the size the window
        // settles at is pushed through.
        Windows::UI::Xaml::DispatcherTimer^ m_resizeDebounce;
        // The size the window wants, as opposed to kW/kH which is the size the engine is actually
        // painting at. Applied only once the engine has accepted it -- see ApplyViewportSize.
        int m_pendW { 0 }, m_pendH { 0 };
        // Apotheosis 2026-09-19: the DIP size last accepted by the engine. kW/kH cannot serve as
        // that record because in GPU mode they are PHYSICAL pixels (DIP x the panel's
        // CompositionScale), so `w == kW` never holds there and every resize would re-run forever.
        int m_appliedDipW { 0 }, m_appliedDipH { 0 };
        // Apotheosis: a navigation requested while the engine was busy. NavigateTo used to drop
        // such a request outright (`if (m_loading) return;`), so an Enter or a link tap during a
        // slow load or resize vanished with no feedback -- and once an engine job stopped
        // returning at all, every later attempt vanished too, which is exactly how a wedged
        // resize turned into an unusable browser. The request is parked here and replayed by
        // m_navRetry as soon as the engine is free. Navigation outranks resizing: a resize is
        // idempotent and discardable, a navigation is neither.
        std::wstring m_pendNavUrl;
        bool m_pendNavPush { true };
        Windows::UI::Xaml::DispatcherTimer^ m_navRetry;
        // Live render loop state
        Windows::UI::Xaml::DispatcherTimer^ m_liveTimer;
        bool m_liveBusy { false };           // Previous LiveTick engine task hasn't returned, prevent accumulation
        int m_liveBusyAge { 0 };             // Number of ticks m_liveBusy has persisted; > threshold triggers self-heal (RunAsync lost, not infinite loop)
        std::atomic<bool> m_appForeground { true };  // App is in foreground (paused in background); also read by engine thread, hence atomic
        unsigned m_lastFrameHash { 0 };      // Previous frame hash (determine if image changed)
        int m_liveStaticTicks { 0 };         // Consecutive static frames count, stop frames when threshold reached
        int m_liveTotalTicks { 0 };          // Cumulative frames of continuous animation; exceed threshold reduces framerate (prevent permanent animation power drain)
        // Apotheosis: ticks since the last StartLiveMode, i.e. since the last thing the user did.
        // Within this window the cost-based slow-down is suppressed, so a page whose repaint is slow
        // still gets several back-to-back frames to finish reacting to a tap (a cookie banner closing
        // with a CSS transition needs more than one).
        int m_liveSettleTicks { 0 };
    };
}
