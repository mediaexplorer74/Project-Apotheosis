// ============================================================================
// WebCoreDriver.cpp  --  WebCore headless software-render driver (Phase 1b)
//
// Target: clang-cl --target=thumbv7-unknown-windows-msvc /std:c++23
//         App Container (WINAPI_FAMILY_APP), -DWK_WINUWP=1, exceptions OFF.
//         Software rendering only (USE_CAIRO/USE_FREETYPE/USE_FONTCONFIG/
//         USE_HARFBUZZ on; TEXTURE_MAPPER/ANGLE/SKIA off).
//
// Exposes one C entry point that turns a UTF-8 HTML string into an
// RGBA8888 pixel buffer rendered by WebCore through a Cairo image surface.
//
//   extern "C" int WebCoreRenderHtml(const char* utf8Html, int w, int h,
//                                    uint8_t* outRGBA);
//
// Returns 0 on success, negative on failure (see error codes below).
//
// Pipeline (mirrors WebCore::SVGImage::dataChanged + ::draw, the canonical
// in-tree headless render path, see Source/WebCore/svg/graphics/SVGImage.cpp):
//
//   1. process init: JSC::initialize / WTF::initializeMainThread /
//      WebCore::initializeCommonAtomStrings  (once, guarded)
//   2. PageConfiguration via pageConfigurationWithEmptyClients(...)
//   3. Page::create(...)  -> main LocalFrame is created by the empty-client
//      MainFrameCreationParameters inside the helper
//   4. localMainFrame()->setView(LocalFrameView::create(*frame)); frame->init()
//   5. feed HTML through the active DocumentLoader's DocumentWriter
//      (setMIMEType / begin / addData / end)
//   6. size the view, updateLayout()
//   7. cairo_image_surface(ARGB32) -> GraphicsContextCairo -> view->paint(...)
//   8. copy/swizzle pixels into caller's RGBA8888 buffer
//
// NOTE on pixel format: Cairo CAIRO_FORMAT_ARGB32 is, in memory on a
// little-endian machine, premultiplied BGRA bytes (B,G,R,A). The caller asked
// for RGBA8888, so step 8 swizzles B<->R and un-premultiplies alpha.
// ============================================================================

// config.h MUST be first, exactly like every WebCore TU. It pulls in
// cmakeconfig.h (HAVE_CONFIG_H + BUILDING_WITH_CMAKE are defined on the
// command line) and all of WTF/Platform.h + the WEBCORE_EXPORT export macros.
// Header on -I path: E:\Apotheosis\WebKit\Source\WebCore\config.h
#include "config.h"

#include "WebCoreDriver.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>                            // Apotheosis: gpuLogMarkerF (formatted stage markers)
#include <cstdlib>                            // Apotheosis: _putenv_s, arming the WTF text trace
#include <string>                             // Apotheosis: std::string for the trace-probe path
#include <vector>
#if defined(WK_WINUWP)
#include <winsock2.h>    // UWP: with WIN32_LEAN_AND_MEAN defined, winsock2 has to be included before windows.h,
#endif                   // which is where curl's SOCKET type comes from.
#include <curl/curl.h>   // libcurl easy API -- used by WebCoreDownload
#if defined(WK_WINUWP)
// Apotheosis: background main-document fetcher (see apoFetchChannelGet below).
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

// ---- Windows debug logging (lightweight, no windows.h) ----
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);

// ---- Cairo (vcpkg arm-uwp, reached via -imsvc ...\include\cairo) ----
#include <cairo.h>

// ---- WTF ----
// E:\Apotheosis\build-clang-webcore\WTF\Headers\wtf\...
#include <wtf/MainThread.h>          // WTF::initializeMainThread
#include <wtf/RefPtr.h>              // RefPtr, adoptRef
#include <wtf/Ref.h>                 // Ref
#include <wtf/StdLibExtras.h>        // (also pulled by config.h) WTF::move lives in <wtf/StdLibExtras.h>/<wtf/MainThread.h> chain
#include <wtf/text/WTFString.h>      // WTF::String, _s literal
#include <wtf/text/CString.h>        // String::utf8() for render diagnostics
#include <wtf/text/MakeString.h>     // makeString() for building WTF::String values (IME text path)
#include <wtf/URL.h>                 // WTF::URL

// ---- JavaScriptCore ----
// E:\Apotheosis\build-clang-webcore\JavaScriptCore\PrivateHeaders\JavaScriptCore\...
#include <JavaScriptCore/InitializeThreading.h>   // JSC::initialize
#include <JavaScriptCore/JSCJSValue.h>            // JSC::JSValue(WebCoreEvalJS)
#include <JavaScriptCore/JSCJSValueInlines.h>     // JSValue::toWTFString(inline)
#include <JavaScriptCore/JSGlobalObject.h>        // JSGlobalObject::vm()
#include <JavaScriptCore/JSLock.h>                // JSC::JSLockHolder
#if defined(WK_WINUWP)
// Apotheosis: the js-watchdog arming in buildSession (see the comment there), and the runaway-stack
// probe that hangs off its callback.
#include <JavaScriptCore/Watchdog.h>
#include <JavaScriptCore/StackVisitor.h>
#include <JavaScriptCore/VM.h>
#include <WebCore/CommonVM.h>
#endif

// ---- PAL ----
// E:\Apotheosis\build-clang-webcore\PAL\Headers\pal\...
#include <pal/SessionID.h>          // PAL::SessionID

// ---- WebCore public (PrivateHeaders symlink to source) ----
// E:\Apotheosis\build-clang-webcore\WebCore\PrivateHeaders\WebCore\...
#include <WebCore/CommonAtomStrings.h>   // WebCore::initializeCommonAtomStrings
#include <WebCore/WebCoreJITOperations.h>// WebCore::populateJITOperations (no-op w/ C_LOOP)
#include <WebCore/EmptyClients.h>        // pageConfigurationWithEmptyClients
#include <WebCore/BackForwardCache.h>    // Apotheosis: disable back-forward cache (OOM prevention)
#include <WebCore/MemoryCache.h>         // Apotheosis: resource cache caps
#include <WebCore/MemoryRelease.h>       // Apotheosis: WebCore::releaseMemory on pressure
#include <WebCore/PerformanceLogging.h> // Apotheosis: memoryUsageStatistics -- the JSC-heap reading
#include <WebCore/PageConfiguration.h>   // WebCore::PageConfiguration
#include <WebCore/CookieJar.h>           // WebCore::CookieJar, installed on the PageConfiguration below
#include <WebCore/StorageSessionProvider.h>  // Ref<StorageSessionProvider> needs the complete type
#include "PortNetworkStorageSession.h"   // WebCorePort::makeStorageSessionProvider / ensureDefaultPortStorageSession
// Apotheosis 2026-09-18 (ST-4): the complete session type, so the top-level fetch can file its
// Set-Cookie lines into the ONE jar WebCore's own loader reads. NetworkStorageSession.h only
// forward-declares SameSiteInfo and IncludeSecureCookies; the latter's enumerators come from
// CookieJar.h, which is already included below.
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/SameSiteInfo.h>
#include "PortChromeClient.h"            // WebCorePort::PortChromeClient -- the compositing ChromeClient
// Apotheosis 2026-09-18: the complete StorageNamespaceProvider type, not the forward
// declaration PageConfiguration.h carries -- the assignment in buildSession constructs and
// destroys a Ref<StorageNamespaceProvider>, and ~Ref needs the complete type.
#include <WebCore/StorageNamespaceProvider.h>
#include "PortStorage.h"                 // WebCorePort::createPortStorageNamespaceProvider
#include <WebCore/Page.h>                // WebCore::Page
#include <WebCore/Settings.h>            // Page::settings()
#include <WebCore/LocalFrame.h>          // WebCore::LocalFrame
#include <WebCore/LocalFrameInlines.h>   // inline LocalFrame::document()/protectedDocument()
#include <WebCore/LocalFrameView.h>      // WebCore::LocalFrameView
#include <WebCore/DocumentView.h>        // inline LocalFrame::view()/protectedView()
#include <WebCore/FrameLoader.h>         // FrameLoader::activeDocumentLoader
#include <WebCore/DocumentLoader.h>      // DocumentLoader::writer()
#include <WebCore/DocumentWriter.h>      // DocumentWriter setMIMEType/begin/addData/end
#include <WebCore/Document.h>            // Document::updateLayout / updateLayoutIgnorePendingStylesheets
#include <WebCore/EventLoop.h>           // Document::eventLoop().performMicrotaskCheckpoint()
#include <WebCore/SharedBuffer.h>        // WebCore::SharedBuffer::create(span)
#include <WebCore/IntRect.h>             // WebCore::IntRect
#include <WebCore/IntSize.h>             // WebCore::IntSize
#include <WebCore/FloatRect.h>           // boundingClientRect()
#include <WebCore/HTMLCollection.h>      // Document::links()
#include <WebCore/CachedResourceLoader.h> // cachedResourceLoader(): the pending-resource census writeDiag reports
#include <WebCore/CachedResource.h>      // CachedResource::url()/status()
#include <WebCore/DocumentResourceLoader.h> // Document::cachedResourceLoader() (inline accessor)
#include <WebCore/HTMLAnchorElement.h>   // href()
#include <WebCore/HTMLBodyElement.h>     // document.body()->childElementCount() for the SPA probe
#include <WebCore/ElementInlines.h>      // Element::boundingClientRect()
#include <WebCore/Color.h>               // WebCore::Color, Color::white
#include <WebCore/GraphicsContextCairo.h>// WebCore::GraphicsContextCairo
#include <WebCore/RefPtrCairo.h>         // RefPtr<cairo_t> deref traits

// ---- network-load path (WebCoreLoadUrl) ----
#include <wtf/RunLoop.h>                    // RunLoop::run / currentSingleton / Timer
#include <wtf/Seconds.h>                    // 30_s
#include <wtf/Function.h>                   // WTF::Function
#include <wtf/UniqueRef.h>                  // makeUniqueRefWithoutRefCountedCheck
#include <wtf/Variant.h>                    // std::get on the MainFrameCreationParameters variant
#include <WebCore/FrameLoadRequest.h>       // FrameLoadRequest
#include <WebCore/ResourceRequest.h>        // ResourceRequest
#include <WebCore/SubstituteData.h>         // SubstituteData
#include <WebCore/LocalFrameLoaderClient.h> // base of LoadingFrameLoaderClient
#include "LoadingFrameLoaderClient.h"       // WebCorePort::LoadingFrameLoaderClient

// ---- live interactive session (WebCoreSessionLoad/ClickAt/ScrollBy/Paint) ----
// Input and rendering entry points the resident session needs: EventHandler, Editor,
// FindOptions, and the rAF / IntersectionObserver path isolatedUpdateRendering drives.
#include <WebCore/EventHandler.h>            // LocalFrame::eventHandler() -- mouse press/release dispatch
#include <WebCore/HandleUserInputEventResult.h> // EventHandler's result type -- wasHandled()
#include <WebCore/FocusController.h>         // page->focusController().setActive/setFocused (a headless page must be active for JS focus)
#include <WebCore/Editor.h>                  // editor().canEdit() / insertText() / command() -- the IME text path
#include <WebCore/FindOptions.h>             // WebCore::FindOption / FindOptions
#include <WebCore/SimpleRange.h>             // Page::FindStringData's std::optional<SimpleRange>
#include <WebCore/HTMLInputElement.h>        // IME path: input.value() / dispatchInputEvent()
#include <WebCore/HTMLTextAreaElement.h>     // the same for <textarea>
#include <WebCore/HTMLElement.h>             // isContentEditable() -- contenteditable targeting
#include <WebCore/Document.h>                // elementFromPoint / focusedElement -- the tap hit-test and the focus probe
// Apotheosis: writeDiag reports why a parser is still open, and the tap probe names the one script
// it is stuck on -- ScriptElement's public flags say which script the parser intends to run itself
// and whether its load has come back yet. Document.h forward-declares all of these.
#include <WebCore/DocumentParser.h>
#include <WebCore/HTMLScriptElement.h>
// Apotheosis: for LoadableScript::isLoaded() / hasError(), the only script state that actually
// moves for an external <script src=...>. See the comment in writeDiag on why the flags that were
// printed before it measured nothing.
#include <WebCore/LoadableScript.h>
#include <WebCore/HTMLNames.h>
#include <WebCore/PlatformKeyboardEvent.h>   // PlatformKeyboardEvent -- Enter and typed characters
#include <WebCore/ScriptController.h>        // frame->script().canExecuteScripts / executeScriptInWorldIgnoringException
#include <WebCore/DOMWrapperWorld.h>         // mainThreadNormalWorldSingleton()(WebCoreEvalJS)
#include <WebCore/PlatformMouseEvent.h>      // PlatformMouseEvent
#include <WebCore/MouseEventTypes.h>         // MouseButton / SyntheticClickType
#include <WebCore/ScrollView.h>              // setScrollPosition / maximumScrollPosition, reached through LocalFrameView
#include <WebCore/DoublePoint.h>             // DoublePoint for PlatformMouseEvent
#include <wtf/MonotonicTime.h>               // MonotonicTime timestamps for the synthetic events
#include <wtf/OptionSet.h>                   // OptionSet<PlatformEvent::Modifier>
#include <optional>
#include <algorithm>

// ---- curl TLS root-certificate injection (WebCoreSetCACertPath) ----
// App Container processes cannot reach the Windows system trust store, so we
// point curl/OpenSSL at a bundled Mozilla CA file (cacert.pem) instead.
#include <WebCore/CurlContext.h>            // CurlContext::singleton().sslHandle()
#include <WebCore/CurlSSLHandle.h>          // CurlSSLHandle::setCACertPath/setCACertData
#include <WebCore/CertificateInfo.h>        // CertificateInfo::Certificate == Vector<uint8_t>
#include <wtf/Vector.h>

// ---- M2 GPU path (TextureMapper + ANGLE) ----
// Two ways to get a composited frame out of GraphicsLayerTextureMapper through
// TextureMapper in GL: render into a BitmapTexture and glReadPixels it into the
// caller's RGBA buffer, or present straight to the SwapChainPanel (eglSwapBuffers).
// Both are gated on g_gpuActive (set by WebCoreGpuInit); otherwise paintToRGBA falls
// through to Cairo. TextureMapper::create() needs a current GL context
// (TextureMapper.cpp:216 tests GLContext::current() != null), which comes from the EGL
// window surface GLContext::create(display, nativeWindow) builds.
// - x64-gpu build currently has USE_TEXTURE_MAPPER=0 in cmakeconfig.h; TextureMapper code is
//   conditionally compiled. build-x64-gpu was configured with Cairo only (no TextureMapper).
#define GL_GLEXT_PROTOTYPES 1               // ms-master ANGLE's gl2.h omits the GL prototypes (glReadPixels/glViewport -> C3861)
#include <WebCore/GraphicsLayer.h>          // GraphicsLayer -- chrome->rootLayer(), static_cast to the TextureMapper layer type
#include <WebCore/RenderView.h>             // view->renderView()->compositor()
#include <WebCore/RenderLayerCompositor.h>  // compositor().frameViewDidScroll() -- the TextureMapper's scroll hook
#if USE(TEXTURE_MAPPER)
#include <WebCore/PlatformDisplay.h>        // PlatformDisplay::sharedDisplay()(WIN?PlatformDisplayWin,? ANGLE EGLDisplay)
#include <WebCore/GLContext.h>              // GLContext::create/createOffscreen + makeContextCurrent + swapBuffers
#include "texmap/TextureMapper.h"           // TextureMapper::create / beginPainting / endPainting (platform/graphics include dir)
#include "texmap/TextureMapperLayer.h"      // TextureMapperLayer::paint/applyAnimationsRecursively
#include "texmap/GraphicsLayerTextureMapper.h" // the TextureMapper GraphicsLayer: .layer() / updateBackingStoreIncludingSubLayers
#include "texmap/BitmapTexture.h"           // the composited frame's texture + bindAsSurface
#endif // USE(TEXTURE_MAPPER)
#include <memory>                           // std::unique_ptr

// Installs the PlatformStrategies singleton (loader strategy = WebResourceLoadScheduler).
// Defined in port/PortPlatformStrategies.cpp. Idempotent.
extern void installPortPlatformStrategies();

namespace {

using namespace WebCore;

// Error codes returned through WebCoreRenderHtml.
enum : int {
    kOK              =  0,
    kErrBadArgs      = -1,
    kErrPageCreate   = -2,
    kErrNoMainFrame  = -3,
    kErrNoView       = -4,
    kErrNoLoader     = -5,
    kErrNoDocument   = -6,
    kErrCairoSurface = -7,
    kErrCairoContext = -8,
    kErrBadUrl       = -9,    // URL{url} parsed invalid
    kErrLoadFailed   = -10,   // terminal load state was a failure
    kErrLoadTimeout  = -11,   // watchdog fired before terminal state
    kErrNoSession    = -12,   // no live session (see WebCoreSessionLoad)
    kErrBusy         = -13,   // busy: another pump is running
    kErrFrameGone    = -14,   // the main frame went away during the call
    kErrCurlInit     = -15,   // curl_easy_init failed
    kErrCurlDownload = -16,   // curl_easy_perform failed
};

// Run the WebCore one-time process initialization exactly once.
// Sequence taken from Source/WebKit/Shared/WebKit2Initialize.cpp
// (the !PLATFORM(COCOA) branch -- our case).
// Diagnostic stage marker: writes to stage-port.txt in the same dir as FONTCONFIG_FILE
void WebCoreStage(const char* stage)
{
    if (!stage || !*stage) return;
    // Try to derive LocalState dir from FONTCONFIG_FILE env var
    const char* fcc = getenv("FONTCONFIG_FILE");
    if (fcc) {
        std::string path(fcc);
        auto pos = path.rfind('\\');
        if (pos != std::string::npos) {
            path.resize(pos + 1);
            path += "stage-port.txt";
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fputs(stage, f);
                fputc('\n', f);
                fclose(f);
            }
        }
    }
}

// Apotheosis: crash-surviving driver trace. WebCoreStage above truncates on every call
// (it deliberately records only the LAST stage); this appends and flushes per line so a
// whole sequence survives a hard AV that produces no dump. Needed because DBG_STAGE only
// reaches OutputDebugStringA, which nothing inside the AppContainer can observe -- a
// crash in the driver used to leave no trail at all.
void WebCorePortTrace(const char* fmt, ...)
{
    if (!fmt)
        return;
    const char* fcc = getenv("FONTCONFIG_FILE");
    if (!fcc)
        return;
    std::string path(fcc);
    auto pos = path.rfind('\\');
    if (pos == std::string::npos)
        return;
    path.resize(pos + 1);
    path += "port-trace.txt";
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (FILE* f = fopen(path.c_str(), "ab")) {
        fputs(buf, f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}
bool ensureWebCoreInitialized()
{
    static bool initialized = [] {
        OutputDebugStringA("[INIT] enter\n");
        // Apotheosis: JIT disabled unconditionally on both architectures. On W10M/ARM32, executing
        // JIT-generated code inside the AppContainer kills the process instantly and silently (no
        // exception, no dump) -- proven by bisection: useJIT=false (.73) eliminates the death on
        // ya.ru/dzen.ru. On x64 the JIT is blocked by DEP/ACG anyway, so forcing the interpreter
        // makes both build lines behave identically -- which is what any rendering or stability
        // comparison between them requires. Re-enable only after a proper W^X page-provisioning
        // mechanism is implemented (see Doc/PLAN.md future work).
        JSC::Options::setOption("useJIT", "false");
        OutputDebugStringA("[INIT] useJIT=false\n");
        JSC::initialize();                       // JSC heap/threading/options
        OutputDebugStringA("[INIT] JSC::initialize done\n");
        WTF::initializeMainThread();             // pins this thread as the WebKit main thread + RunLoop::main
        OutputDebugStringA("[INIT] WTF::initializeMainThread done\n");
        WebCore::initializeCommonAtomStrings();  // interns "auto", "all", content types, etc.
        OutputDebugStringA("[INIT] WebCore::initializeCommonAtomStrings done\n");
        installPortPlatformStrategies();         // PlatformStrategies (loader strategy) ? required before any load
        OutputDebugStringA("[INIT] installPortPlatformStrategies done\n");
        WebCore::populateJITOperations();        // no-op under ENABLE(C_LOOP) (header has inline {} fallback)

        // Apotheosis: 32-bit low-memory (Lumia) OOM prevention -- disable
        // back-forward cache (whole-page DOM+render tree is huge in 32-bit
        // address space), tighten resource cache caps. harness calls
        // WebCoreReleaseMemory() under memory pressure.
        WebCore::BackForwardCache::singleton().setMaxSize(0);
        OutputDebugStringA("[INIT] BackForwardCache done\n");
        WebCore::MemoryCache::singleton().setCapacities(0, 8u * 1024 * 1024, 16u * 1024 * 1024);
        OutputDebugStringA("[INIT] MemoryCache done — all init complete\n");
        return true;
    }();
    return initialized;
}

} // anonymous namespace

// Apotheosis: the last network error, recorded by LoadingFrameLoaderClient's dispatchDidFail*
// hooks as a ResourceError (curl code + domain + description + failing URL). MainPage reads
// it back through WebCoreLoadUrl and logs it into LocalFolder -- in the App Container that
// log is the only channel through which WebKit's own error path can be seen.
static char g_lastNetError[512] = "";
static char g_lastDiag[8192] = "";   // the diag line: URL, contents size, counts, parser state, resource statuses
// Apotheosis 2026-09-24: the last *measured* non-white pixel count -- and the buffer it was counted on
// -- or -1 if nothing has measured one yet. `writeDiag` takes the count as a parameter and every
// dedicated caller measured it in that same call, but the two periodic engine activities (the pump's
// settle tick during a load, the live tick between loads) need a diag too and produce no pixels of
// their own. Carrying the last real measurement is honest; inventing a 0 for the field the harness
// parses (`DiagLooksEmpty` reads `nonwhite=`) would not be. The width and height are carried with it so
// the `nonwhite=N/T` ratio always compares a numerator and denominator from the *same* buffer -- a
// carried count over a fresh denominator is a meaningless ratio, which is worse than a stale one.
// Kept here, and filled by writeDiag itself, so no future caller has to remember to update it.
static int g_lastNonWhite = -1;
static int g_lastNonWhiteW = 0;
static int g_lastNonWhiteH = 0;
// Apotheosis 2026-09-24: was 4096, which cut the `res:` list of a resource-heavy page (lenta.ru:
// 19 entries) long before the reader could see which of them were stuck. Raised so the list normally
// fits outright; the list itself now declares any remainder it could not fit (`+N more]`), so this
// number is a comfort margin and not the thing the answer depends on.
static char g_lastTitle[512] = "";   // last document title (for the tab/UI)
static char g_lastUrl[1024] = "";    // last document URL (requested vs effective -- see the address-bar note)
static uint32_t g_lastFrameHash = 0; // hash of the last painted frame, for blit de-duplication
static int g_lastPendingResources = 0; // pending/unknown resources as of the last census
extern "C" bool g_apoUaMobile = true;  // mobile UA switch: true = iPhone-style UA, false = desktop (LoadingFrameLoaderClient::userAgent)
// Apotheosis: content toggles set from the harness Settings page via WebCoreConfigure().
static int g_apoJsEnabled = -1;       // -1 = not configured, use buildSession default
static int g_apoImagesEnabled = -1;
extern "C" char g_apoCustomUA[2048] = {0};  // custom UA override, set by WebCoreSetUserAgentString
static char g_spaProbe[512] = "";     // the SPA module probe's last verdict (import started / eval-ok / error)
static std::vector<uint8_t> g_caBytes;  // the packaged CA bundle, handed to curl by WebCoreDownload

// GPU active flag: true once WebCoreGpuInit has both a live GL context and a TextureMapper.
// This is the only gate on the GPU path (plus PortChromeClient); with it false everything
// falls through to Cairo. Enabling it unconditionally once cost a silent crash on the device.
static bool g_gpuActive = false;
// content pixels counted by the last composite (0 = nothing painted); reset by WebCoreGpuInit
// Everything else that needs TextureMapper lives under USE(TEXTURE_MAPPER).
// g_gpuScrollFast: a fast scroll skips forceDirtyTree; also USE(TEXTURE_MAPPER)-only.
static int g_lastContentPx = 0;
static bool g_gpuScrollFast = false;
// Apotheosis 2026-09-19: the CSS page zoom this session is supposed to be running at.
//
// This is session state, not a one-shot setting, because **a navigation resets it**. Measured on the
// bench with the GPU presenting (Doc/GPU-LIVENESS.md §9): dzen.ru/news established
// `[GPU] page zoom 1.00 -> 2.00` at arming time, and after the next navigation the hand-back found
// `page zoom already 1.00` -- the engine had put the factor back by itself before any of the port's
// code ran again. Reproduced twice: 2.00 set at 07:18:38 and 07:21:27, found back at 1.00 during the
// hand-backs at 07:20:03 and 07:25:42.
//
// The harness only re-applies it from EnableGpu, which runs *after* the load settles. So everything
// between the document commit and that call was laid out at the un-zoomed width -- and with the GPU
// surface sized in physical pixels (§9) that is a layout twice too wide, i.e. exactly the
// "stretched" page the maintainer reported when following links. Remembering the request here and
// re-asserting it at present time closes that window without the harness having to know when a
// commit happened, and it costs one float compare per frame.
static float g_pageZoom = 1.0f;
#if USE(TEXTURE_MAPPER)
static WebCore::GLContext* g_glContext = nullptr;
static WebCore::TextureMapper* g_textureMapper = nullptr;
static int g_gpuW = 0, g_gpuH = 0;
static bool g_gpuFlipH = false;
static bool g_gpuFlipV = false;
static bool g_gpuPresentMode = false;
// Apotheosis 2026-08-29: is the host ACTUALLY showing the GPU surface right now?
//
// g_gpuPresentMode only records that WebCoreGpuInit was given a native window, i.e. that a swapchain
// present is *possible*. It says nothing about whether the harness kept the panel visible, and that
// gap is what produced the blank x64 window: the harness probed compositing on a session built before
// WebCoreGpuInit ran, got kErrNoSession because that session has no chrome client, decided to stay on
// software present and collapsed GpuPanel -- while the driver went on taking the direct-present branch
// in paintToRGBA and returning kOK without ever filling outRGBA. The page was rendered and presented
// into a hidden surface; the visible WriteableBitmap kept the zero-filled buffer, hence
// nonwhite=0/710656 on a document at readyState Complete.
//
// This is exactly the flag the comment in buildSession asks for -- "we are presenting through the GPU
// RIGHT NOW", set after a successful present, as opposed to "the GPU was initialised at some point,"
// which is all g_gpuActive says. It defaults to false so software rendering stays the baseline: the
// driver fills the buffer unless the host has explicitly said it is showing the swapchain.
static bool g_gpuDirectPresent = false;
#endif // USE(TEXTURE_MAPPER)

// Load counters (main document + CSS/JS/subresources) that WebCorePortBumpLoad would feed
// from ResourceHandle.cpp; WebCoreLoadUrl resets them and writeDiag prints them.
static int g_loadStarted = 0, g_loadResponse = 0, g_loadComplete = 0, g_loadFail = 0;
static inline void apoEngineTick();   // Apotheosis 2026-09-19: engine liveness gauge, defined below
// Apotheosis 2026-09-19: **this function has no callers.** Nothing in WebKit (no `WK_WINUWP` hook in
// ResourceHandle.cpp either) and nothing in Src/port calls it, so the four counters below are
// structurally zero and the diag's `loads=S0/R0/C0/F0` field has never reported anything but zeros --
// a dead diagnostic column that reads exactly like "no resources were requested", which is a sentence
// someone will eventually believe. It is NOT the signal the wedge watchdog wants: the live load
// traffic is visible in `port-trace.txt`/`gpuinit-steps.txt` (`loader: CALL/serve started`), which is
// where a reader should look. Left in place rather than deleted because reviving it is the right fix
// (the hook is Src/port/PortLoaderStrategy.cpp, through WebCoreDriverInternal.h's declaration) and
// that is a separate change from the watchdog work that found this.
extern "C" void WebCorePortBumpLoad(int kind)
{
    switch (kind) {
    case 0: ++g_loadStarted; break;
    case 1: ++g_loadResponse; break;
    case 2: ++g_loadComplete; break;
    case 3: ++g_loadFail; break;
    }
}

// Apotheosis 2026-09-19: the engine's own liveness gauge -- a monotonic count of the work the engine
// thread has visibly done, published for the harness's wedge watchdog.
//
// Why it exists: the watchdog's only signal used to be `finished` (completed jobs) plus the
// top-level fetch's byte counter. Measured on the bench, both are too coarse for a real page. A cold
// dzen.ru navigation took 6.9 s inside ONE job (`nav-load`): the transfer finished in ~1-2 s
// (WebCoreGetFetchProgress correctly returned -1 again, "no fetch in flight"), and the remaining
// seconds went into parsing 3.6 MB of HTML and running its inline JS inside `writer.addData`, then
// into the pump's rendering ticks. `finished` cannot move during any of that -- it counts jobs, and
// this was the same job -- so the watchdog dumped a full engine stack of a page that was loading
// perfectly, twice in a row (04:35:35 and 04:50:09), for `[STAGE] after-load rc=0 compositing=1`
// two seconds later.
//
// The fix is to stop inferring and publish the engine's own progress at the points where it actually
// makes some: every settle tick of pumpLoop (the load job's dominant phase), each step of
// WebCoreLiveTick, and the load job's own stage boundaries. A frozen counter then means what the
// watchdog wants it to mean.
//
// One candidate signal is deliberately NOT here: `WebCorePortBumpLoad`'s resource counters, because
// that function has no callers at all (see the comment above it). Note also that a *fetch* of a
// subresource cannot help during the phase this gauge was built for -- while the engine is inside
// `writer.addData` no RunLoop runs, so nothing can deliver a response into it.
//
// Deliberately a plain counter and NOT a marker file, for the same reason g_liveTickStep is: pumpLoop's
// settle tick runs every 50 ms and an fopen/fwrite/fclose per tick on the device's slow flash would
// change the timing it measures. One relaxed fetch_add costs nothing, and only two threads ever touch
// it (the engine thread writes, the UI thread reads the published long long).
static std::atomic<long long> g_engineActivity { 0 };
static inline void apoEngineTick() { g_engineActivity.fetch_add(1, std::memory_order_relaxed); }

// ---- extracted link table (rebuilt on every layout/paint) ----
// Every <a href> with its viewport-relative boundingClientRect (so scrolling moves it)
// plus its resolved URL. The harness hit-tests taps against this table.
struct LinkRect { int x, y, w, h; std::string url; };
static std::vector<LinkRect> g_links;
static std::string g_imeDiag;   // the last WebCoreTypeText diagnostic (read back by WebCoreEditDebug)
// find-in-page state: the query text and its options, kept for WebCoreFindNext
static WTF::String g_findText;
static WebCore::FindOptions g_findOpts;

static int countPendingResources(WebCore::Document& document)
{
    int pending = 0;
    for (auto& kv : document.cachedResourceLoader().allCachedResources()) {
        WebCore::CachedResource* res = kv.value.get();
        if (!res)
            continue;
        auto status = res->status();
        if (status == WebCore::CachedResource::Unknown || status == WebCore::CachedResource::Pending)
            ++pending;
    }
    return pending;
}

static void extractLinks(WebCore::Document* document, int renderH)
{
    // built with -fno-exceptions: this path has no try/catch at all
    g_links.clear();
    if (!document)
        return;
    // Apotheosis: coarse stage markers only. The per-iteration EL:i=* markers that isolated
    // the anchors-only 0xC0000005 are gone: WebCoreStage truncates and rewrites the file on
    // every call, so at up to 4000 links x 4 markers they were a five-figure file-write storm
    // per page load. begin/done still answer "did extractLinks complete" for free.
    WebCoreStage("EL:begin");
    Ref<WebCore::HTMLCollection> links = document->links();
    unsigned n = links->length();
    for (unsigned i = 0; i < n && g_links.size() < 4000; ++i) {
        WebCore::Element* el = links->item(i);
        if (!el || !is<WebCore::HTMLAnchorElement>(*el))
            continue;
        auto href = downcast<WebCore::HTMLAnchorElement>(*el).href();
        if (href.isEmpty() || !href.isValid() || !href.protocolIsInHTTPFamily())
            continue;   // http(s) only: javascript:, #fragment and mailto: are not tap targets
        // the rect is viewport-relative (scroll-affected); the caller forces layout first
        WebCore::FloatRect r = el->boundingClientRect();
        if (r.width() <= 0 || r.height() <= 0)
            continue;
        if (r.maxY() < 0 || r.y() > static_cast<float>(renderH))
            continue;   // skip anchors scrolled out of the rendered area
        LinkRect lr;
        lr.x = static_cast<int>(r.x());
        lr.y = static_cast<int>(r.y());
        lr.w = static_cast<int>(r.width());
        lr.h = static_cast<int>(r.height());
        // Apotheosis: CString::data() is null for a null String; assigning that to
        // std::string is UB. Keep the temporary alive and null-check it.
        WTF::CString hrefUtf8 = href.string().utf8();
        lr.url = hrefUtf8.data() ? hrefUtf8.data() : "";
        g_links.push_back(std::move(lr));
    }
    WebCoreStage("EL:done");
}

static size_t webcoreDownloadWrite(void* ptr, size_t size, size_t nmemb, void* stream)
{
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
}

// ============================================================================
// ============================================================================
// Live interactive session: one resident Page with real input and real rendering.
// EventHandler press/release/move drives hover, click, onclick and SPA handlers;
// isolatedUpdateRendering drives rAF and IntersectionObserver; the run loop is
// pumped from here. Invariants: one session at a time (a stale teardown would be a
// use-after-free), frame/view re-read after each pump, one pump at a time (g_inPump).
// ============================================================================
struct DriverLoadState {
    bool mainDone = false;   // load reached a terminal state (success or failure)
    bool failed   = false;
};

struct Session {
    RefPtr<WebCore::Page> page;            // keeps frame/view/document alive
    RefPtr<WebCore::LocalFrame> mainFrame; // the frame, re-read after every pump; owns the view
    WebCorePort::LoadingFrameLoaderClient* client = nullptr; // the frame's loader client, borrowed -- carries the load-completion handler
    WebCorePort::PortChromeClient* chrome = nullptr;          // borrowed from the Page's UniqueRef; rootLayer()/present hooks (GPU path)
    int w = 0, h = 0;
    DriverLoadState load;                  // per-job load state; didFinishLoad captures a pointer into it
};
static std::optional<Session> g_session;
static bool g_inPump = false;              // set while a pump is live -- the single-pump guard

// clears g_inPump on every exit path, early returns included
struct PumpGuard { ~PumpGuard() { g_inPump = false; } };

// Apotheosis 2026-09-24, PLAN 0s: writes the diag from an engine-thread point that has no pixels of its
// own, carrying the last measured non-white count. Defined below writeDiag(), declared here because
// pumpLoop's settle tick is the first caller and it sits above the definition.
static void apoRefreshDiagCarried(WebCore::LocalFrame& frame, const char* src);

// Pump the run loop until the document is idle (resources and XHR finished, timers drained):
//   mainDone: the load's terminal flag; null = there is no navigation to wait for
//   allowEarlyStopWithoutNav: for taps and scrolls, stop after ~0.5 s of quiet
//   settleCapTicks: hard cap on settle ticks (each one 50 ms)
//   pageForRendering: when set, every tick runs isolatedUpdateRendering (rAF/IO, SPA pages)
static void pumpLoop(WebCore::LocalFrame& frame, const bool* mainDone, bool allowEarlyStopWithoutNav,
                     int settleCapTicks, double watchdogSeconds, WebCore::Page* pageForRendering)
{
    using namespace WebCore;
    bool stopped = false;
    // Apotheosis: TEMPORARY (task #7). pumpLoop AVs inside its first RunLoop::run() for
    // every real load, trivial local HTML included, so the fault is in the pump itself and
    // not in page content. Log which RunLoop we are about to pump versus the one WTF pinned
    // as "main" in ensureWebCoreInitialized -- WebCore schedules its timers on the latter,
    // and if the two differ we are running a loop nobody feeds.
    WebCorePortTrace("pump: tid=%lu cur=%p main=%p isMainThread=%d isMain=%d",
        (unsigned long)GetCurrentThreadId(),
        (void*)&RunLoop::currentSingleton(),
        (void*)&RunLoop::mainSingleton(),
        (int)WTF::isMainThread(),
        (int)RunLoop::isMain());
    auto stopLoop = [&stopped] {
        if (stopped)
            return;
        stopped = true;
        RunLoop::currentSingleton().stop();
    };
    int settleTicks = 0;
    int quietTicks = 0;
    RefPtr<LocalFrame> frameRef = &frame;
    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.settle"_s,
        WTF::Function<void()> { [&stopLoop, &settleTicks, &quietTicks, frameRef, mainDone, allowEarlyStopWithoutNav, settleCapTicks, pageForRendering] {
            // EmptyChromeClient's RenderingUpdateScheduler is a no-op, so rAF and
            // IntersectionObserver never fire on their own -- which is why this tick drives them.
            if (pageForRendering) {
                // Apotheosis 2026-09-19: engine liveness. This tick runs every 50 ms while the loop is
                // up, so its arrival is the strongest statement the port can make that the engine thread
                // is working -- and it is published before the two operations below, each of which can
                // take seconds on a large document, so a tick that HANGS still leaves the tick before it
                // counted. See apoEngineTick.
                apoEngineTick();
                WebCorePortTrace("settle: tick enter (quiet=%d settle=%d)", quietTicks, settleTicks);
                pageForRendering->isolatedUpdateRendering();   // drives rAF/IntersectionObserver (and the JS/DOM work they schedule)
                WebCorePortTrace("settle: isolatedUpdateRendering ok");
                // isolatedUpdateRendering can replace the main frame (a page-initiated navigation), so
                // the frame is re-read below and the pump stops if it is no longer the one it came for.
                RefPtr<LocalFrame> mf = pageForRendering->localMainFrame();
                if (mf.get() != frameRef.get()) {
                    stopLoop();
                    return;
                }
            }
            // Apotheosis: microtask and promise work (ES module evaluation and its follow-ups) needs
            // a run-loop cycle of its own; isolatedUpdateRendering alone does not drain it.
            // Apotheosis 2026-09-24, PLAN 0s: the settle tick is the engine thread's only guaranteed
            // periodic activity while a load is in flight (the harness does not present then -- its live
            // tick returns early on m_loading), and until now nothing on this path touched the diag, so
            // the line a reader saw was the one the load job wrote seconds earlier. Placed outside the
            // `if (pageForRendering)` block on purpose: the diag is wanted whether or not this pump was
            // given a page to run rendering updates on.
            apoRefreshDiagCarried(*frameRef, "pump");
            if (RefPtr<Document> doc = frameRef->document())
                doc->eventLoop().performMicrotaskCheckpoint();
            WebCorePortTrace("settle: microtaskCheckpoint ok");

            RefPtr<DocumentLoader> dl = frameRef->loader().activeDocumentLoader();
            bool loading = dl && dl->isLoadingInAPISense();
            bool navDone = mainDone && *mainDone;
            bool ready = navDone || allowEarlyStopWithoutNav;

            // Note: isLoadingInAPISense goes false while a module script (<script type=module>) is
            // still evaluating, because ScriptRunner/WindowEventLoop only posts a 0 s timer.
            // The run-loop cycles this tick performs are what let that work finish, and the quiet
            // window below is what makes stopping safe rather than a race with the evaluator.
            // Apotheosis: "not loading" is not the same as "paintable". While a render-blocking
            // stylesheet is outstanding the Document keeps
            // VisualUpdatesPreventedReason::RenderBlocking, and RenderLayer's
            // shouldSuppressPaintingLayer() then bails out of the *entire* layer tree, so the
            // surface comes back zero-filled rather than partially drawn.
            // news.ycombinator.com hit precisely this: isLoadingInAPISense() had already gone
            // false for the main resource while news.css was still in flight, so the quiet counter
            // ran to 16 and we stopped pumping before WebCore was ever willing to draw a pixel.
            // Treat "visual updates still prevented" as not-quiet. This cannot hang: the
            // suppression timeout set in buildSession bounds it, and settleCapTicks/watchdog
            // bound it again.
            RefPtr<Document> settleDoc = frameRef->document();
            bool suppressed = settleDoc && !settleDoc->visualUpdatesAllowed();
            if (loading || suppressed)
                quietTicks = 0;
            else
                ++quietTicks;
            if (navDone)
                ++settleTicks;
            if (ready && quietTicks >= 16) {            // ~0.8 s of quiet (16 x 50 ms) with no navigation -> the page is settled
                stopLoop();
                return;
            }
            if (navDone && settleTicks > settleCapTicks)  // hard cap (8 s by default): the settle loop cannot run forever
                stopLoop();
            WebCorePortTrace("settle: tick exit (loading=%d vua=%d quiet=%d)",
                (int)loading, (int)!suppressed, quietTicks);
        } });
    settle.startRepeating(0.05_s);
    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop(); } });
    watchdog.startOneShot(WTF::Seconds(watchdogSeconds));
    // Apotheosis: TEMPORARY (task #7). The AV inside RunLoop::run() lands on
    // RELEASE_ASSERT(a.m_type == b.m_type) in WTF::operator<=>(TimeWithDynamicClockType),
    // dereferencing what is provably a raw QPC tick count rather than a pointer. A dangling
    // ScheduledTask could only corrupt the time *value*, never the reference *address*, so
    // the defect must be structural (struct-return ABI on the by-value nowWithSameClock()).
    // If so it reproduces outside the loop entirely -- which this probe settles.
    {
        WTF::MonotonicTime probeNow = WTF::MonotonicTime::now();
        WTF::TimeWithDynamicClockType probe(probeNow + WTF::Seconds(1));
        WebCorePortTrace("twdct probe: built value=%.3f type=%d",
            probe.secondsSinceEpoch().value(), (int)probe.clockType());
        WTF::TimeWithDynamicClockType probeNowSame = probe.nowWithSameClock();
        WebCorePortTrace("twdct probe: nowWithSameClock=%.3f type=%d",
            probeNowSame.secondsSinceEpoch().value(), (int)probeNowSame.clockType());
        bool lessThan = probe < probeNowSame;
        WebCorePortTrace("twdct probe: compare ok, less=%d", (int)lessThan);
    }
    WebCorePortTrace("pump: timers armed, entering RunLoop::run()");
    RunLoop::run();
    WebCorePortTrace("pump: RunLoop::run() returned");
    // Apotheosis: per-statement markers for the wedge window. The engine thread has died between
    // "run() returned" and DBG_STAGE("pumpLoop done") three runs in a row; these four statements
    // are all that is in between, and until now nothing told us WHICH of them wedges. Paired with
    // WTF's rllock: probe (RunLoopGeneric.cpp) this pins the statement and names the lock holder,
    // or shows a wedge with no lock contention at all -- which would mean a spin, not a block.
    WebCorePortTrace("pump: settle.stop enter");
    settle.stop();
    WebCorePortTrace("pump: settle.stop done");
    watchdog.stop();
    WebCorePortTrace("pump: watchdog.stop done");
}

// Notes on the two settle numbers. pumpLoop waits for 16 quiet ticks (~0.8 s at
// 50 ms) before declaring a page settled; in practice that window covers the one or
// two leftover resource/JS events a real page emits, and anything past it is ordinary
// idle behaviour (a setTimeout-driven tick, or work started by input). Between loads
// the harness's own 200 ms live tick (StartLiveMode/WebCoreLiveTick) covers the gap.
static void pumpQuick(WebCore::LocalFrame& frame, WebCore::Page* pageForRendering)
{
    using namespace WebCore;
    for (int i = 0; i < 2; ++i)
        RunLoop::cycle();
    if (pageForRendering)
        pageForRendering->isolatedUpdateRendering();
    if (RefPtr<Document> doc = frame.document())
        doc->eventLoop().performMicrotaskCheckpoint();
}

// ===================== M2 GPU recipe: forcing a full repaint =====================
// Everything here is inside USE(TEXTURE_MAPPER) (the Cairo-only builds skip it).
#if USE(TEXTURE_MAPPER)
// Mark every GraphicsLayer dirty (setNeedsDisplay). A tile whose m_needsDisplay /
// m_needsDisplayRect is clear is skipped by updateBackingStoreIfNeeded, so without
// this a pumpLoop-driven rendering-update can repaint nothing and the readback
// returns the clear colour (contentPx = 0). setNeedsDisplay does nothing at all
// for a layer with drawsContent = false, which is why the recursion visits every one.
static void forceDirtyTree(WebCore::GraphicsLayer& l)
{
    l.setNeedsDisplay();
    for (const auto& c : l.children())
        forceDirtyTree(c.get());
    if (auto* r = l.replicaLayer())
        forceDirtyTree(*r);
    if (auto* m = l.maskLayer())
        m->setNeedsDisplay();
}

// Apotheosis: forward declarations. The marker writers are defined further down (near
// WebCoreSetGpuInitLogFile, which owns the path they write to), but the GPU present path above needs
// them to record why a present or readback fell back -- and that is the one thing this log never said,
// which is why "presented into a hidden surface" and "present failed" looked identical.
static void gpuLogMarker(const char* step);
static void gpuLogMarkerF(const char* fmt, ...);

// Rebuild the TextureMapperLayer tree and re-upload dirty tiles; requires g_glContext
// to be current. The Apple ports' WCScene::update, for this layer type instead.
static void gpuPrepare(WebCore::LocalFrameView& view, WebCore::GraphicsLayerTextureMapper& glRoot)
{
    using namespace WebCore;
    view.updateLayoutAndStyleIfNeededRecursive();
    {
        Color docBg = view.documentBackgroundColor();
        glRoot.setBackgroundColor(docBg.isValid() ? docBg : Color::white);
    }
    view.flushCompositingStateIncludingSubframes();
    if (auto* renderView = view.renderView())
        renderView->compositor().frameViewDidScroll();
    if (!g_gpuScrollFast)
        forceDirtyTree(glRoot);
    g_gpuScrollFast = false;
    glRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);
    glRoot.layer().applyAnimationsRecursively(MonotonicTime::now());
}

static int gpuCompositeReadback(WebCore::LocalFrameView& view, int w, int h,
                                WebCore::GraphicsLayer& root, uint8_t* outRGBA, int& nonWhiteOut)
{
    using namespace WebCore;
    if (!g_glContext || !g_textureMapper)
        return kErrNoView;
    // Apotheosis 2026-09-04: step markers. "died somewhere in paintToRGBA" is too coarse to act
    // on, and this function is where the 0.1.9.84 death is expected: it is entered only when a
    // root layer exists, which in the packaged sequence first happens on layertest.html -- the
    // entry the process did not survive. gpuPrepare / paint / glReadPixels each touch GL state
    // set up by a WebCoreGpuInit that may have early-returned on its second call, so they need
    // to be distinguishable from one another rather than lumped together.
    WebCorePortTrace("rb: enter %dx%d", w, h);
    // Apotheosis 2026-09-04: the return value used to be discarded. It is the one thing that
    // decides whether every GL call below has a context, and a null current context is what makes
    // BitmapTexture's depthBufferFormat() fault (see the WK_WINUWP guard there).
    //
    // GLContext::current() is deliberately NOT read here: it is not exported from WebCore.dll, and
    // calling it costs an LNK2019 on the harness link -- which is where this went first, producing
    // an appx that never built while the previously installed one kept running and kept printing
    // the older markers. A trace read from a build that did not link is worse than no trace.
    const bool madeCurrent = g_glContext->makeContextCurrent();
    WebCorePortTrace("rb: makeCurrent=%d self=%p", madeCurrent ? 1 : 0, (void*)g_glContext);
    // Apotheosis 2026-09-17: fail closed. Measured on 0.1.9.93: after the surface under ANGLE
    // is lost, makeContextCurrent() returns false, yet every GL call afterwards "succeeds" --
    // glGenFramebuffers/genGenTextures return name 0, glGetString(GL_VERSION) returns NULL,
    // glReadPixels reads whatever buffer was there -- and the empty test passes, so a dead frame
    // is handed to the harness as kOK and the Cairo fallback is suppressed. A contextless GL
    // sequence is undefined behaviour, and it is the best suspect for the 0xc0000005 in
    // WebCore.dll+0x204901f that ended the .93 run. Do not try to re-create the context here:
    // the TextureMapper and all backing stores belong to the dead context. Return the caller's
    // "no view" error; paintToRGBA falls through to Cairo, which paints without GL.
    if (!madeCurrent) {
        WebCorePortTrace("rb: no live GL context -- falling back to Cairo");
        return kErrNoView;
    }
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    WebCorePortTrace("rb: gpuPrepare enter");
    gpuPrepare(view, glRoot);

    // Apotheosis 2026-09-04: this five-statement stretch is where the x64 process dies -- the
    // trace of 0.1.9.85 ends exactly at the "BitmapTexture::create" marker below and the next
    // one never appears. Each statement therefore gets its own marker plus a glGetError read,
    // because they fail in ways that are indistinguishable without one: BitmapTexture::create
    // allocates a 1024x694 RGBA texture *and* a depth buffer through ANGLE, beginPainting binds
    // it as an FBO while the context's own surface is a window surface, and either can leave the
    // driver in a state the next GL call turns into a hard fault rather than an error code.
    WebCorePortTrace("rb: BitmapTexture::create");
    Ref<BitmapTexture> texture = BitmapTexture::create(IntSize(w, h),
        { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
    WebCorePortTrace("rb: texture ok glErr=0x%04X", glGetError());
    Color docBg = view.documentBackgroundColor();
    if (!docBg.isValid())
        docBg = Color::white;
    WebCorePortTrace("rb: docBg ok");
    // The prime suspect sits one statement below. beginPainting ends in bindSurface ->
    // BitmapTexture::bindAsSurface -> createFboIfNeeded -> initializeDepthBuffer ->
    // depthBufferFormat(), and that last function dereferenced GLContext::current() with no null
    // check until the WK_WINUWP guard was added to it. If the guard's own
    // "bt: depthBufferFormat GLContext::current()==NULL" line appears in this trace between here
    // and the next marker, that is the confirmation.
    g_textureMapper->beginPainting(TextureMapper::FlipY::No, texture.ptr());
    WebCorePortTrace("rb: beginPainting ok glErr=0x%04X", glGetError());
    g_textureMapper->clearColor(docBg);
    WebCorePortTrace("rb: layer paint enter");
    glRoot.layer().paint(*g_textureMapper);
    WebCorePortTrace("rb: readPixels enter");
    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    g_textureMapper->endPainting();
    WebCorePortTrace("rb: readPixels done");

    auto [bgrf, bggf, bgbf, bgaf] = docBg.toColorTypeLossy<SRGBA<float>>().resolved();
    const int bgR = (int)(bgrf * 255 + 0.5f), bgG = (int)(bggf * 255 + 0.5f), bgB = (int)(bgbf * 255 + 0.5f);
    int nonWhite = 0, contentPx = 0;
    uint32_t hash = 2166136261u;
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = tmp.data() + static_cast<size_t>(g_gpuFlipV ? (h - 1 - y) : y) * w * 4;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = srow + static_cast<size_t>(g_gpuFlipH ? (w - 1 - x) : x) * 4;
            const uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
            drow[x * 4 + 0] = r; drow[x * 4 + 1] = g; drow[x * 4 + 2] = b; drow[x * 4 + 3] = a;
            if (r != 255 || g != 255 || b != 255)
                ++nonWhite;
            if (std::abs((int)r - bgR) + std::abs((int)g - bgG) + std::abs((int)b - bgB) > 24)
                ++contentPx;
            if (((x | y) & 3) == 0) {
                hash = (hash ^ r) * 16777619u;
                hash = (hash ^ g) * 16777619u;
                hash = (hash ^ b) * 16777619u;
            }
        }
    }
    nonWhiteOut = nonWhite;
    g_lastContentPx = contentPx;
    // Apotheosis 2026-08-29: an empty composite is a FAILURE, not a success.
    //
    // This used to `return kOK` unconditionally, and paintToRGBA treats kOK as "frame delivered, stop
    // here" -- so a run through TextureMapper that painted nothing handed back a white buffer and
    // suppressed the Cairo fallback entirely. On the bench that produced exactly the reported symptom:
    // the first load renders through Cairo (GPU not yet initialised) and appears on screen; then
    // WebCoreGpuInit runs, the next session gets a root layer, and the very next live tick reads back
    // an empty composite whose hash differs from the last frame -- so the harness accepts it as new
    // pixels and blits white over a good page. Reloading appeared to fix it only because it rebuilt
    // the session.
    //
    // contentPx is the right test rather than nonWhite: it counts pixels that differ from the
    // document's own background colour, so a legitimately white page is not misjudged as empty, and a
    // dark page whose every pixel is non-white is not misjudged as full. When it is zero, report
    // kErrNoView and let the caller fall through to Cairo, which knows how to paint unlayered content.
    // g_lastFrameHash is deliberately NOT updated in that case: publishing the hash of a frame we are
    // rejecting would make the next tick believe the page had changed.
    if (!contentPx)
        return kErrNoView;
    g_lastFrameHash = hash;
    return kOK;
}

static void gpuLogMarkerF(const char* fmt, ...);
static void apoReassertPageZoom(WebCore::LocalFrame& frame);
// PLAN 0s: defined below writeDiag(), called from gpuPresent() just below.
static void writeDiagGpuPresent(WebCore::Document& document, WebCore::LocalFrameView& view, int w, int h);
static int gpuPresent(WebCore::LocalFrameView& view, int w, int h, WebCore::GraphicsLayer& root)
{
    using namespace WebCore;
    if (!g_glContext || !g_textureMapper)
        return kErrNoView;
    apoReassertPageZoom(view.frame());
    // Apotheosis 2026-09-17: same fail-closed rule as gpuCompositeReadback. In 0.1.9.93 the
    // readback variant measured makeContextCurrent()==false with every later GL call reporting
    // success against a dead context. Presenting into it is the same undefined behaviour.
    if (!g_glContext->makeContextCurrent()) {
        gpuLogMarker("[GPU] present: no live GL context -- falling back");
        return kErrNoView;
    }
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    gpuPrepare(view, glRoot);
    glViewport(0, 0, w, h);
    g_textureMapper->beginPainting(TextureMapper::FlipY::No, nullptr);
    {
        Color docBg = view.documentBackgroundColor();
        g_textureMapper->clearColor(docBg.isValid() ? docBg : Color::white);
    }
    glRoot.layer().paint(*g_textureMapper);
    g_textureMapper->endPainting();
    // Apotheosis 2026-09-24, PLAN 0s: the diag is a CACHED string and every writeDiag caller is a
    // software path, so under direct present nothing refreshed it -- measured on lenta.ru still reading
    // `rs=I` two minutes after `readyState` had become Complete, which is what produced the false PLAN
    // item 0r. Same thread, same point in the frame as finishInteractionPaint's own writeDiag.
    if (RefPtr<Document> diagDoc = view.frame().document())
        writeDiagGpuPresent(*diagDoc, view, w, h);
    g_glContext->swapBuffers();
    return kOK;
}
#endif // USE(TEXTURE_MAPPER)

// Re-assert the requested page zoom if the engine has drifted from it (see g_pageZoom).
//
// Called from the present paths, which are the one place where a wrong factor is about to become
// visible pixels, and the reason it is here rather than at the navigation callbacks: a commit is
// exactly when the factor gets reset, and by the time the load callback runs the frames in between
// have already been presented. This makes the port self-correcting instead of dependent on the
// harness re-arming in time.
//
// It must NOT call page->isolatedUpdateRendering() the way WebCoreSetPageZoom does: the caller is
// already inside a paint, and the forced layout below is what makes the corrected factor take effect
// before this very frame is drawn. The layout runs at most once per document -- the next frame finds
// the factors equal and returns.
//
// Software present is covered too, not just the GPU. There the requested factor is 1.0 and normally
// matches, so the call costs one float compare; and if a hand-back ever leaves the request at the
// GPU's 2.00, this is what stops software from rendering a magnified page.
static void gpuLogMarkerF(const char* fmt, ...);
static void apoReassertPageZoom(WebCore::LocalFrame& frame)
{
    using namespace WebCore;
    if (frame.pageZoomFactor() == g_pageZoom && frame.textZoomFactor() == 1.0f)
        return;
    const float before = frame.pageZoomFactor();
    frame.setPageZoomFactor(g_pageZoom);
    if (RefPtr<Document> doc = frame.document())
        doc->updateLayoutIgnorePendingStylesheets();
    gpuLogMarkerF("[GPU] page zoom re-asserted %.2f -> %.2f (a document change had reset it)",
        (double)before, (double)g_pageZoom);
}

// view->paint has produced Cairo ARGB32; swizzle it into the caller's RGBA8888 buffer
// (B<->R plus un-premultiplied alpha -- the conversion every paint path in this file shares)
// Apotheosis 2026-09-19: name the path that actually painted the frame, once per change.
//
// paintToRGBA has three outcomes and only one of them was audible. A failed present logs, a failed
// readback logs -- but "there is no root layer, so this document cannot be presented through the GPU
// at all" is a silent walk into Cairo. That is the correct and constant answer for a page without
// promoted layers, so logging it per tick would drown the file; logging the TRANSITION is what makes
// "the GPU went quiet because the document changed" visible. It matters because the harness's
// m_gpuPresent stays true across such a navigation (the probe is not re-run once the GPU is on) and it
// keeps the swapchain panel on screen, so a driver that has quietly switched to Cairo and a driver
// that is still presenting look identical from the outside -- and from inside, the swapchain keeps
// showing the previous document's frame. Measured 2026-09-19 on the bench: after the first real page
// presented through the GPU, a navigation to a non-compositing page produced no `[GPU]` line at all.
static void apoNotePaintPath(int path)
{
    static int s_last = -1;
    if (path == s_last)
        return;
    s_last = path;
    static const char* const names[] = {
        "cairo (no root layer, or gpu inactive)",
        "gpu direct present",
        "gpu readback -> software blit",
    };
    gpuLogMarkerF("[GPU] paint path -> %s", names[(path >= 0 && path < 3) ? path : 0]);
}

static int paintToRGBA(WebCore::LocalFrameView& view, int w, int h, uint8_t* outRGBA, int& nonWhiteOut)
{
    using namespace WebCore;
    nonWhiteOut = 0;
    apoReassertPageZoom(view.frame());

    // M2: GPU present path. Taken only when the GPU is active, a TextureMapper exists and
    // this view is the current session's main frame's view; it composites through the
    // TextureMapper and reads the result back. Every other case falls through to Cairo.
    // Apotheosis 2026-08-29: the line that used to sit here claimed "x64-gpu: USE(TEXTURE_MAPPER)=0,
    // GPU code path excluded; always falls through to Cairo". That is FALSE and it cost a morning:
    // build-x64-gpu/cmakeconfig.h has USE_TEXTURE_MAPPER 1, and WebCoreDriver.x64.obj contains the
    // gpuPresent/gpuCompositeReadback symbols. Both architectures compile and take this branch.
#if USE(TEXTURE_MAPPER)
    if (g_gpuActive && g_textureMapper && g_session && g_session->chrome
        && g_session->mainFrame && g_session->mainFrame->view() == &view) {
        if (WebCore::GraphicsLayer* root = g_session->chrome->rootLayer()) {
            // Apotheosis: direct present only when the host is REALLY showing the swapchain
            // (g_gpuDirectPresent, set over the C ABI by the harness). Presenting to a collapsed
            // panel returns kOK with outRGBA untouched, which is a blank window rather than an error
            // -- the worst kind of failure. Otherwise composite through TextureMapper and read the
            // result BACK into outRGBA, so layered content still reaches the software blit.
            if (g_gpuPresentMode && g_gpuDirectPresent) {
                const int prc = gpuPresent(view, w, h, *root);
                if (prc == kOK) {
                    apoNotePaintPath(1);
                    return kOK;
                }
                // Fall through to readback, then Cairo. A failed present must never leave the frame
                // unpainted; nothing logged this return before, which made "presented into a hidden
                // surface" and "present failed" indistinguishable in the log.
                gpuLogMarkerF("[GPU] present failed rc=%d -- falling back", prc);
            }
            if (gpuCompositeReadback(view, w, h, *root, outRGBA, nonWhiteOut) == kOK) {
                apoNotePaintPath(2);
                return kOK;
            }
            gpuLogMarker("[GPU] readback failed -- falling back to Cairo");
        }
    }
#endif

    apoNotePaintPath(0);
    const IntSize size(w, h);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return kErrCairoSurface;
    }
    cairo_t* cr = cairo_create(surface);
    if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr) cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return kErrCairoContext;
    }
    // Apotheosis: start from opaque white. cairo_image_surface_create zero-fills, i.e. fully
    // transparent black, and WebCore does not always paint a base background over it: when visual
    // updates are prevented, RenderLayer skips the whole tree and we would hand the harness a
    // transparent buffer, which the WriteableBitmap renders as an empty window. A browser's base
    // canvas is white anyway, so this is also what a healthy page expects; a page that paints its
    // own background simply overwrites it, at the cost of one fill.
    // NOTE for the diag reader: this changes what nonwhite=0 means. It used to distinguish
    // "painted a white background" (0) from "painted nothing at all" (w*h, because transparent
    // black counts as non-white); both now read 0. Use the vua= field in the diag string instead,
    // which reports Document::visualUpdatesAllowed() directly.
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    {
        GraphicsContextCairo context(adoptRef(cr));
        // ScrollView::paint offsets by -scrollPosition itself, so a software paint needs no
        // scroll compensation: the buffer is already at the current scroll position. When a
        // root GraphicsLayer exists this paint is only the fallback, and it is
        // FlattenCompositingLayers/Snapshotting that make this single pass complete.
        auto oldBehavior = view.paintBehavior();
        view.setPaintBehavior(oldBehavior | PaintBehavior::FlattenCompositingLayers | PaintBehavior::Snapshotting);
        view.paint(context, IntRect(IntPoint(), size));
        view.setPaintBehavior(oldBehavior);
    }
    cairo_surface_flush(surface);

    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    int nonWhite = 0;
    uint32_t hash = 2166136261u;   // FNV-1a style hash of every 4th pixel: cheap change detection for the harness
    for (int y = 0; y < h; ++y) {
        const unsigned char* srow = src + static_cast<size_t>(y) * stride;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char b = srow[x * 4 + 0];
            const unsigned char g = srow[x * 4 + 1];
            const unsigned char r = srow[x * 4 + 2];
            const unsigned char a = srow[x * 4 + 3];
            if (a == 0 || a == 255) {
                drow[x * 4 + 0] = r;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = b;
                drow[x * 4 + 3] = a;
            } else {
                drow[x * 4 + 0] = static_cast<uint8_t>((r * 255 + a / 2) / a);
                drow[x * 4 + 1] = static_cast<uint8_t>((g * 255 + a / 2) / a);
                drow[x * 4 + 2] = static_cast<uint8_t>((b * 255 + a / 2) / a);
                drow[x * 4 + 3] = a;
            }
            if (drow[x * 4 + 0] != 255 || drow[x * 4 + 1] != 255 || drow[x * 4 + 2] != 255)
                ++nonWhite;
            // Sample every 4th pixel in each axis (one pixel per 16 px block) and hash R/G/B only:
            // cheap, and stable enough to notice a real repaint rather than sub-pixel noise.
            if (((x | y) & 3) == 0) {
                hash = (hash ^ drow[x * 4 + 0]) * 16777619u;
                hash = (hash ^ drow[x * 4 + 1]) * 16777619u;
                hash = (hash ^ drow[x * 4 + 2]) * 16777619u;
            }
        }
    }
    cairo_surface_destroy(surface);
    nonWhiteOut = nonWhite;
    g_lastFrameHash = hash;
    return kOK;
}

// Fills g_lastDiag/g_lastTitle (URL, counts, parser state); read by WebCoreGetDiag/GetTitle
// `src`, when given, names the engine activity that asked for this diag even though it has no pixels of
// its own (`pump`), and records that `nonWhite`/`w`/`h` are carried. The dedicated callers -- the load
// job, a click, a resize, a software present -- all pass a triple they measured in that same call and
// leave it null.
static void writeDiag(WebCore::Document& document, WebCore::LocalFrameView& view, int w, int h, int nonWhite,
                      const char* src = nullptr)
{
    using namespace WebCore;
    auto urlStr = document.url().string().utf8();
    auto titleStr = document.title().utf8();
    IntSize cs = view.contentsSize();
    // JS state: jsEnabled = the Settings switch; canExec = ScriptController's answer (0 with no page)
    // scriptCount = how many <script> elements the SPA-probe heuristic below saw
    int jsEnabled = document.settings().isScriptEnabled() ? 1 : 0;
    int canExec = view.frame().script().canExecuteScripts(ReasonForCallingCanExecuteScripts::NotAboutToExecuteScript) ? 1 : 0;
    unsigned scriptCount = document.scripts()->length();
    // Apotheosis: vua = Document::visualUpdatesAllowed(). 0 means WebCore is refusing to paint
    // anything at all -- RenderLayer::shouldSuppressPaintingLayer() bails on the whole layer tree
    // -- so the surface holds only the white pre-fill from paintToRGBA. *This* is the definitive
    // "blank page" tell; nonwhite cannot distinguish it from a legitimately white page.
    // sheets = haveStylesheetsLoaded(). vua=0 together with sheets=0 means a render-blocking
    // stylesheet is still outstanding, i.e. we stopped pumping too early (or the site is slow).
    int visualOK = document.visualUpdatesAllowed() ? 1 : 0;
    int sheetsOK = document.haveStylesheetsLoaded() ? 1 : 0;
    // SPA probe, DOM side (no JS eval): #root with children > 0 means React/Vue mounted. A
    // zero with loads=/js= healthy is "the bundle never mounted", not "the load failed".
    int rootKids = -1;
    if (RefPtr root = document.getElementById(AtomString { "root"_s }))
        rootKids = static_cast<int>(root->childElementCount());
    int bodyKids = document.body() ? static_cast<int>(document.body()->childElementCount()) : -1;
    int pendingResources = countPendingResources(document);
    // Apotheosis: the parser's own state, because "every resource is Cached yet nothing runs" has
    // several very different causes and the fields above cannot tell them apart. rs = readyState:
    // a document that never leaves L(oading) never fires DOMContentLoaded, so every deferred script
    // -- i.e. every modern bundle -- stays unexecuted no matter that it downloaded fine. The rest
    // says why the parser is still open: p=Document::parsing, ig=isIgnoringPendingStylesheets (0 in
    // steady state, and only then does the sheets= field above mean "no pending stylesheet" -- a
    // parser-blocking script is held back until the last pending sheet resolves), pr=the parser is
    // mid-chunk or has scheduled itself to resume on a timer, st=the parser was stopped, le=the load
    // event already ran. sheets=0 forever means the pending-sheet count leaked; pr=1 forever means
    // the resume timer never fires; p=1 with everything else 0 means the parser is waiting on a
    // script load that was never signalled back to it -- the scr= field below names it.
    // Style::Scope::hasPendingSheets would be the direct answer but it is not exported from
    // WebCore.lib, and haveStylesheetsLoaded is exactly it once ig is known to be 0.
    const char* rsName = "?";
    switch (document.readyState()) {
    case Document::ReadyState::Loading:     rsName = "L"; break;
    case Document::ReadyState::Interactive: rsName = "I"; break;
    case Document::ReadyState::Complete:    rsName = "C"; break;
    }
    WebCore::DocumentParser* parser = document.parser();
    int parserProcessing = parser && parser->processingData() ? 1 : 0;
    int parserStopped = parser && parser->isStopped() ? 1 : 0;
    // Apotheosis: which script the parser is waiting on, in the diag line rather than only in the
    // tap probe, because reproducing the stall needs no tap -- it is there from the first paint.
    // willBeParserExecuted() means the parser intends to run this script itself and will not move
    // past it, so blk lists exactly the scripts the parser owns. Each one now carries l/e/f (loaded,
    // error, fired its load event) instead of the old /r flag, which measured nothing -- see the
    // comment on those flags below. defer counts the scripts that only run from
    // Document::finishedParsing -- i.e. everything a modern bundle ships -- which is why a document
    // stuck at rs=L paints as a dead page even with every resource cached.
    char scriptState[240];
    {
        auto scripts = document.scripts();
        const unsigned scriptsLen = scripts->length();
        unsigned deferred = 0, errored = 0, blocking = 0;
        char names[130] = "";
        size_t namesLen = 0;
        bool namesFull = false;
        for (unsigned i = 0; i < scriptsLen; ++i) {
            RefPtr<HTMLScriptElement> script = dynamicDowncast<HTMLScriptElement>(scripts->item(i));
            if (!script)
                continue;
            if (script->willExecuteWhenDocumentFinishedParsing())
                ++deferred;
            if (script->errorOccurred())
                ++errored;
            if (!script->willBeParserExecuted())
                continue;
            ++blocking;
            // Apotheosis: once the 130-byte name buffer is full, stop naming scripts but keep
            // counting them. This used to `break` out of the whole loop, so defer=, err= and blk=
            // silently stopped short on any script-heavy page -- hh.ru reported defer=4 blk=4 while
            // actually having seven parser-owned bundles among 43 scripts, and the numbers were
            // being used to reason about the page.
            if (namesFull)
                continue;
            auto srcU8 = script->attributeWithoutSynchronization(HTMLNames::srcAttr).string().utf8();
            const char* srcFull = srcU8.data() ? srcU8.data() : "";
            const char* srcSlash = std::strrchr(srcFull, '/');
            const char* srcName = (srcSlash && srcSlash[1]) ? srcSlash + 1 : (srcFull[0] ? srcFull : "inline");
            // Apotheosis: this field used to print readyToBeParserExecuted() as /r0 or /r1, and it
            // measured nothing at all. WebCore assigns m_readyToBeParserExecuted in exactly one
            // place -- ScriptElement.cpp:334, the branch for an *inline* parser-inserted script held
            // up only by stylesheets -- and never for a <script src=...>. So it read 0 for every
            // external bundle whether that bundle had loaded and run or not, and reading it as "the
            // script is not ready" is what pointed the hh.ru investigation at a phantom for weeks.
            // These three flags come from state that actually moves: l = the resource finished
            // loading, e = it failed, f = the element fired its load event, i.e. it executed. The
            // interesting case is l1e0f0: downloaded, no error, never ran. A dash means the element
            // has no LoadableScript at all, which is normal for an inline script.
            WebCore::LoadableScript* loadable = script->loadableScript();
            const char loadedFlag = loadable ? (loadable->isLoaded() ? '1' : '0') : '-';
            const char errorFlag  = loadable ? (loadable->hasError() ? '1' : '0') : '-';
            const char firedFlag  = script->haveFiredLoadEvent() ? '1' : '0';
            int wn = std::snprintf(names + namesLen, sizeof names - namesLen, "#%u:%.20s/l%ce%cf%c ",
                i, srcName, loadedFlag, errorFlag, firedFlag);
            if (wn < 0 || static_cast<size_t>(wn) >= sizeof names - namesLen) {
                names[namesLen] = '\0';
                namesFull = true;
                continue;
            }
            namesLen += static_cast<size_t>(wn);
        }
        std::snprintf(scriptState, sizeof scriptState, "defer=%u err=%u blk=%u[%s%s]",
            deferred, errored, blocking, names, namesFull ? "..." : "");
    }
    g_lastPendingResources = pendingResources;
    // Apotheosis 2026-09-24, PLAN 0s: this is the one place that ever sees a real count, so it is where
    // the carry-value cache is filled -- all three fields together, so the ratio can never pair a
    // carried numerator with a fresh denominator.
    if (nonWhite >= 0 && w > 0 && h > 0) {
        g_lastNonWhite = nonWhite;
        g_lastNonWhiteW = w;
        g_lastNonWhiteH = h;
    }
    // The field's PRESENCE carries the semantics: `diagsrc=pump` means this diag was written by an
    // engine activity that produced no pixels of its own, so `nonwhite=` is the last measurement carried
    // forward -- every diag *without* the field measured it in that same call. Built into the single
    // snprintf below rather than appended afterwards: appending was the first version of this patch and
    // it was silently erased by the next dedicated writeDiag, which rewrites the whole string.
    char srcField[40] = "";
    if (src)
        std::snprintf(srcField, sizeof srcField, " diagsrc=%s", src);
    std::snprintf(g_lastTitle, sizeof g_lastTitle, "%s", titleStr.data());
    std::snprintf(g_lastUrl, sizeof g_lastUrl, "%s", urlStr.data());
    int mainLen = std::snprintf(g_lastDiag, sizeof g_lastDiag,
        "url=%s title=%s contents=%dx%d body=%d nonwhite=%d/%d loads=S%d/R%d/C%d/F%d pending=%d js=%d/%d vua=%d sheets=%d scripts=%u rootKids=%d bodyKids=%d rs=%s/p%d/ig%d/pr%d/st%d/le%d scr=%s spa=[%.220s] lasterr=[%.150s]%s",
        urlStr.data(), titleStr.data(), cs.width(), cs.height(),
        document.body() ? 1 : 0, nonWhite, w * h,
        g_loadStarted, g_loadResponse, g_loadComplete, g_loadFail, pendingResources,
        jsEnabled, canExec, visualOK, sheetsOK, scriptCount, rootKids, bodyKids,
        rsName, document.parsing() ? 1 : 0, document.isIgnoringPendingStylesheets() ? 1 : 0,
        parserProcessing, parserStopped, document.loadEventFinished() ? 1 : 0,
        scriptState, g_spaProbe, g_lastNetError, srcField);

    // The res: list below. N in name(sN) is CachedResource::Status, not a stage number:
    // 0 Unknown, 1 Pending, 2 Cached (loaded), 3 LoadError, 4 DecodeError.
    // (s1) entries are therefore in flight, (s3) entries are the ones that failed.
    //
    // Apotheosis 2026-09-24: this list used to truncate in the worst possible way -- the loop
    // `break`ed when the buffer filled and the code then closed the bracket anyway, so a CUT list was
    // indistinguishable from a complete one. That is what made item 0r's question ("are the Pending
    // entries one family of resources, or just the last ones in flight?") unanswerable from a log
    // that looked like it held the whole answer. A diagnostic that silently drops the part you are
    // asking about is worse than one that reports nothing.
    // The whole list is built first, then as much of it as fits is written, then -- if anything did
    // not fit -- how many entries were left out, INSIDE the brackets.
    if (mainLen > 0 && mainLen < static_cast<int>(sizeof g_lastDiag) - 8) {
        std::vector<std::string> entries;
        for (auto& kv : document.cachedResourceLoader().allCachedResources()) {
            WebCore::CachedResource* res = kv.value.get();
            if (!res)
                continue;
            auto u8 = res->url().string().utf8();
            const char* full = u8.data() ? u8.data() : "";
            const char* slash = std::strrchr(full, '/');
            const char* name = (slash && slash[1]) ? slash + 1 : full;
            char one[64];
            std::snprintf(one, sizeof one, "%.44s(s%d) ", name, static_cast<int>(res->status()));
            entries.emplace_back(one);
        }

        char* p = g_lastDiag + mainLen;
        int rem = static_cast<int>(sizeof g_lastDiag) - mainLen;
        int n = std::snprintf(p, rem, " res:[");
        if (n > 0 && n < rem) { p += n; rem -= n; }

        // Enough room for " +4294967295 more]" -- the marker must fit or the cut is silent again.
        const int markerRoom = 32;
        const int budget = rem > markerRoom ? rem - markerRoom : 0;
        unsigned written = 0;
        int used = 0;
        for (const auto& e : entries) {
            if (used + static_cast<int>(e.size()) > budget)
                break;
            std::memcpy(p + used, e.data(), e.size());
            used += static_cast<int>(e.size());
            ++written;
        }
        p += used; rem -= used;
        if (written < entries.size())
            std::snprintf(p, rem, "+%u more]", static_cast<unsigned>(entries.size() - written));
        else if (rem > 1) { *p++ = ']'; *p = '\0'; }
    }
}

// Apotheosis 2026-09-26: define the forward declaration used by the settle tick in pumpLoop. The
// settle tick has no pixels of its own, so it carries the last measured non-white count instead of
// inventing a fresh value; writeDiag() remains the single place that formats the diagnostic string.
static void apoRefreshDiagCarried(WebCore::LocalFrame& frame, const char* src)
{
    using namespace WebCore;
    if (g_lastNonWhite < 0 || g_lastNonWhiteW <= 0 || g_lastNonWhiteH <= 0)
        return;
    RefPtr<Document> document = frame.document();
    RefPtr<LocalFrameView> view = frame.view();
    if (!document || !view)
        return;
    writeDiag(*document, *view, g_lastNonWhiteW, g_lastNonWhiteH, g_lastNonWhite, src);
}


// Apotheosis 2026-09-24, PLAN 0s: refresh the diag from the GPU present path, which is the path the
// device runs (`gpudefault=1`) and the one on which nothing used to refresh it at all. See g_lastNonWhite
// for why the pixel count is carried rather than measured, and `gpu=1` below for how a reader tells.
//
// Throttled: this runs per presented frame and writeDiag walks every cached resource (113 leaves on
// lenta.ru) to build a ~5 KB string, which is not work to do 60 times a second on a phone. A diag that is
// 200 ms old is honest; one that is two minutes old is not.
#if USE(TEXTURE_MAPPER)
static void writeDiagGpuPresent(WebCore::Document& document, WebCore::LocalFrameView& view, int w, int h)
{
    using namespace WebCore;
    if (g_lastNonWhite < 0) {
        // Unreachable in practice: the load path writes a diag -- and so fills the carry value -- before
        // the GPU can present anything. Reported rather than silently skipped, because "the refresh
        // quietly did nothing" is the exact failure mode this item exists to remove.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            gpuLogMarker("[GPU] diag refresh skipped: no non-white count has been measured yet");
        }
        return;
    }
    static bool s_haveRefreshed = false;
    static MonotonicTime s_lastRefresh;
    const MonotonicTime now = MonotonicTime::now();
    if (s_haveRefreshed && (now - s_lastRefresh).milliseconds() < 200.0)
        return;
    s_haveRefreshed = true;
    s_lastRefresh = now;

    writeDiag(document, view, w, h, g_lastNonWhite);
    // The marker's PRESENCE is the whole semantics: a diag carrying `gpu=1` has a *carried* `nonwhite=`,
    // and every other diag measured it in that same call. Appended rather than woven in, so nothing that
    // reads the line by field name has to change, and only if it fits -- same discipline as `+N more]`.
    if (std::strlen(g_lastDiag) + sizeof " gpu=1" <= sizeof g_lastDiag)
        std::strcat(g_lastDiag, " gpu=1");
}
#endif // USE(TEXTURE_MAPPER)

// Evaluate JS in the page's main world and copy the string result into out (SPA probe)
static int evalJS(WebCore::LocalFrame& frame, const char* script, char* out, int len)
{
    using namespace WebCore;
    if (!out || len <= 0)
        return kErrBadArgs;
    out[0] = '\0';
    DOMWrapperWorld& world = mainThreadNormalWorldSingleton();
    auto* globalObject = frame.script().globalObject(world);
    if (!globalObject)
        return kErrNoDocument;
    JSC::JSValue result = frame.script().executeScriptInWorldIgnoringException(
        world, String::fromUTF8(script), JSC::SourceTaintedOrigin::Untainted);
    // Apotheosis: an empty JSValue is JSC's "the script did not run" answer -- scripts disabled
    // for the frame, no global object for the world, or a throw that ...IgnoringException
    // swallowed. It MUST be filtered out before toWTFString(): JSValue::isCell() is
    // !(bits & NotCellMask) and the empty value is all-zero bits, so it answers *true*, and
    // toWTFString() then reads JSCell::m_type (offset 5) off a null pointer -- 0xC0000005 on
    // address 0x5, from `cmpb $0x2, 0x5(%r14)` where 2 == JSType::StringType. probeSpaModule
    // runs on every load, so this killed every page the moment pumpLoop started completing.
    if (!result)
        return kErrNoDocument;              // out is already "", caller distinguishes by rc
    JSC::JSLockHolder lock(globalObject->vm());
    String s = result.toWTFString(globalObject);
    auto u8 = s.utf8();
    std::snprintf(out, static_cast<size_t>(len), "%s", u8.data() ? u8.data() : "");
    return kOK;
}

// SPA probe: find the page's <script type=module> and kick its import, so a bundle that
// only ever runs on an import promise actually starts; the outcome lands in g_spaProbe.
// frame/view/document are re-read afterwards -- the import can navigate the page.
static void probeSpaModule(WebCore::Page& page, WebCore::LocalFrame& frame)
{
    using namespace WebCore;
    char kick[80] = "";
    int krc = evalJS(frame,
        "(function(){try{var s=document.querySelector('script[type=\"module\"][src]');"
        "if(!s)return 'no-mod';window.__spaProbe='importing';"
        "import(s.src).then(function(){window.__spaProbe='eval-ok rootCh='+((document.getElementById('root')||{children:[]}).children.length);})"
        ".catch(function(e){window.__spaProbe='EVAL-ERR:'+(e&&(e.message||e.name||String(e))||'?');});"
        "return 'kicked';}catch(e){return 'PROBE-EX:'+(e.message||e);}})()",
        kick, sizeof kick);
    WebCorePortTrace("spa: probe rc=%d kick='%s'", krc, kick);
    // Apotheosis: rc != kOK means the probe never executed (no global object / scripts off /
    // swallowed throw). Treat that exactly like "no module script": there is nothing for the
    // extra 4s import pump to wait for, and paying it on every load would stall each
    // navigation for no reason.
    if (krc != kOK || std::strcmp(kick, "no-mod") == 0) {
        g_spaProbe[0] = '\0';   // nothing to wait for: clear the probe so writeDiag cannot report a stale verdict
        return;
    }
    // an import was kicked, so pump with no navigation to wait for and let the promise settle
    pumpLoop(frame, nullptr, true, 0, 4.0, &page);
    RefPtr<LocalFrame> lf = page.localMainFrame();
    if (lf)
        evalJS(*lf, "window.__spaProbe||'no-probe'", g_spaProbe, sizeof g_spaProbe);
}

// Teardown order, chosen so nothing can touch a freed session (a late didFinishLoad or curl
//  callback is the hazard): (b) detach the client's load handler -- it becomes a no-op;
//  (c) stopAllLoaders -- late dispatches become no-ops as well; (d) drop the frame and
//  client handles; (e) release the Page so ~Page can detach from the session;
//  (f) drain: cycle the run loop so any straggler loader/curl callback retires.
// Teardown is not a pump, so g_inPump is deliberately left alone here.
// Apotheosis: loader-side tracing hooks, defined in PortPlatformStrategies.cpp. Declared here
// rather than in a shared header because there is no port-internal header yet and introducing one
// would force every port translation unit to recompile. See the definitions for what the session
// generation is for.
namespace WebCorePort {
void portLoaderMark(const char* tag);
void portLoaderNextGeneration();
}

static void teardownSession()
{
    using namespace WebCore;
    if (!g_session)
        return;
    // Apotheosis: the loader state at the three points that matter. The stale-callback question is
    // "how much was still in flight when the session went away, and did stopping the loaders plus
    // four run-loop cycles actually retire it" -- and until these marks existed the trace showed
    // loader traffic before and after teardown with nothing separating the two.
    WebCorePort::portLoaderMark("teardown enter");
    if (g_session->client)
        g_session->client->setLoadCompletionHandler({});       // (b)
    // Apotheosis 2026-09-19: the loader strategy's own counters (`inflight=`/`pending=` on the mark
    // above) are blind to a navigation that is still at the POLICY check -- no request has been handed
    // to loadResource yet, so there is nothing to count. On dzen.ru that is exactly the state a
    // page-initiated navigation occupies when this teardown runs, and stopAllLoaders() below then kills
    // it with the bare platform cancellation (`type=Cancellation code=0 domain= url=`, i.e. NOT one of
    // this port's own factories, which all return `{}`). Measured: the whole
    // `docloader create -> policy navigation -> FAIL didFailProvisionalLoad` chain with no
    // `loader: CALL` line at all. `provisionalDocumentLoader()` is inline in FrameLoader.h, so asking
    // it costs no export -- see Doc/ENGINE-INITIATED-NAV-CANCEL.md.
    if (g_session->mainFrame && g_session->mainFrame->loader().provisionalDocumentLoader())
        WebCorePort::portLoaderMark("teardown PROVISIONAL-IN-FLIGHT (stopAllLoaders is about to cancel it)");
    if (g_session->mainFrame)
        g_session->mainFrame->loader().stopAllLoaders();        // (c)
    WebCorePort::portLoaderMark("teardown after stopAllLoaders");
    g_session->client = nullptr;
    g_session->mainFrame = nullptr;                              // (d)
    // (e) Order matters: ~Page runs while g_session is still alive, so the load state the
    //     loader client points at stays valid for the whole of the destructor. Releasing the
    //     Page after g_session.reset() would let ~Page touch a freed Session -- a UAF.
    g_session->page = nullptr;
    g_session.reset();                                          // (f) drop the Session (the Page itself was released just above)
    // The requested zoom belonged to the frame that just went away; a fresh session starts at the
    // CSS default until WebCoreSetPageZoom says otherwise (see g_pageZoom).
    g_pageZoom = 1.0f;
    for (int i = 0; i < 4; ++i)                                  // (g) drain: cycle the run loop so any straggler loader/curl callback retires
        RunLoop::cycle();
    WebCorePort::portLoaderMark("teardown after drain");
    // Everything started from here belongs to the next session.
    WebCorePort::portLoaderNextGeneration();
}

// Apotheosis: per-step stage marker file (harness passes its LocalState path via
// WebCoreSetGpuInitLogFile, which writes LocalState\gpuinit-steps.txt). Appends one line per
// step; survives a hard crash or a hang because each marker is opened, written and closed
// immediately. Originally added to pinpoint the GPU post-init crash, now also used to time
// the resize path, so it lives here — above finishInteractionPaint and outside
// USE(TEXTURE_MAPPER) — rather than next to the GPU entry points.
//
// Deliberately no stack buffer in the caller: finishInteractionPaint sits inside the
// strict_gs_check(push, off) region, where a local array once cost a 0xc0000409 stack-canary
// crash. Formatting happens in gpuLogMarkerF's own frame instead.
static char g_gpuInitLog[1024];
// Apotheosis: gate for the fine-grained markers inside finishInteractionPaint. That function is
// on the hot interaction path — every tap and scroll goes through it — and each marker costs an
// fopen/fwrite/fclose, which is unacceptable per frame on the device's slow flash. Only the
// resize path, the one that actually wedged, switches tracing on around its single call.
static bool g_stageTrace = false;
static void gpuLogMarker(const char* step)
{
    if (g_gpuInitLog[0]) {
        FILE* f = fopen(g_gpuInitLog, "a");
        if (f) { fprintf(f, "%s\n", step); fclose(f); }
    }
    OutputDebugStringA(step);
    OutputDebugStringA("\n");
}
// Apotheosis: printf-style variant of the above, for markers that must carry a measurement
// (step duration, geometry, return code). Same immediate-flush guarantee, which is the whole
// point: a step that never finishes still leaves its own line on disk, so the last line in
// the file names the step that hung.
static void gpuLogMarkerF(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gpuLogMarker(buf);
}
// Apotheosis: the marker file is the only diagnostic channel that survives a hang, but gpuLogMarker
// is static and the Port clients each live in their own translation unit. PortChromeClient needs it
// to forward the page's JS console output: an uncaught exception in a site's bundle used to vanish
// without trace, which made "this button has no handler" indistinguishable from "this button's
// handler was never attached because the bundle threw on the way in".
namespace WebCorePort {
void portDiagLog(const char* text) { gpuLogMarker(text); }
}

#if defined(WK_WINUWP)
// Apotheosis: walk the JavaScript stack and write it out, one line per frame.
//
// Shared by the watchdog's runaway probe below and by WebCorePort::portDumpJsStack, the console
// self-test. The split is deliberate: the runaway probe can only fire during a hang, so its
// mechanism would otherwise be unverified until the one run that matters. portDumpJsStack reaches
// exactly the same code from a healthy page (FrameConsoleClient::addMessage calls
// ChromeClient::addMessageToConsole with JSExecState::currentState() on the stack), so the walker,
// the frame accessors and the formatting can be proven before they are needed.
//
// Read-only and bounded. A probe that floods the trace is a probe that gets misread, and this one
// fires during a hang where the trace is the only witness. It neither faults nor mutates VM state;
// the only allocation is inside the WTF::String helper accessors. The walk stops itself rather than
// trusting the frame count: a stack that has been walking in circles for minutes may be many
// thousand frames deep, and the interesting part is always the top.
static int apoWalkJsStack(JSC::VM& vm, const char* tag, int maxFrames)
{
    char b[192];
    if (!vm.topCallFrame)
    {
        sprintf_s(b, sizeof b, "%s: no top call frame -- JS is not on this thread's stack", tag);
        gpuLogMarker(b);
        return 0;
    }

    int logged = 0;
    JSC::StackVisitor::visit(vm.topCallFrame, vm, [&](JSC::StackVisitor& visitor) {
        if (logged >= maxFrames)
            return WTF::IterationStatus::Done;

        // A native frame has no script position and functionName() is not meaningful for it. It is
        // also the frame that tells us WHERE the engine entered JS -- keep it, labelled.
        if (visitor->isNativeFrame())
        {
            sprintf_s(b, sizeof b, "%s #%d <native> %.80s", tag, logged,
                visitor->functionName().utf8().data());
            gpuLogMarker(b);
            logged++;
            return WTF::IterationStatus::Continue;
        }

        WTF::CString fn = visitor->functionName().utf8();
        WTF::CString url = visitor->sourceURL().utf8();

        // hasLineAndColumnInfo() is the honest test, but it is NOT exported from JavaScriptCore.dll
        // (no JS_EXPORT_PRIVATE on it, so the link fails). Its upstream body is literally
        // `return !!codeBlock();` -- StackVisitor.cpp:503 -- and codeBlock() is an inline accessor,
        // so the test is reproduced here rather than linked.
        if (visitor->codeBlock())
        {
            JSC::LineColumn lc = visitor->computeLineAndColumn();
            sprintf_s(b, sizeof b, "%s #%d %s:%u:%u :: %.80s", tag, logged,
                url.data(), lc.line, lc.column, fn.data());
        }
        else
        {
            sprintf_s(b, sizeof b, "%s #%d %.100s :: %.80s", tag, logged,
                url.data(), fn.data());
        }
        gpuLogMarker(b);
        logged++;
        return WTF::IterationStatus::Continue;
    });

    sprintf_s(b, sizeof b, "%s: logged %d frame(s)%s", tag, logged,
        logged >= maxFrames ? " (capped)" : "");
    gpuLogMarker(b);
    return logged;
}

namespace WebCorePort {
// Apotheosis: the console self-test entry point. Called from PortChromeClient::addMessageToConsole
// only when LocalState\jstack.txt exists, so it costs nothing in a normal run and can be switched
// off without a rebuild. See apoWalkJsStack above for why this exists at all.
//
// The gate is a file rather than a build flag for the usual reason: the switch has to be usable
// against an already-installed build, and Device Portal cannot write LocalState on the Lumia.
void portDumpJsStack(const char* tag)
{
    JSC::VM* vm = WebCore::commonVMOrNull();
    if (!vm)
    {
        gpuLogMarker("jstack: no VM yet -- the engine has not initialised");
        return;
    }
    char b[128];
    sprintf_s(b, sizeof b, "%s: vm=%p topCallFrame=%p", tag, (void*)vm, (void*)vm->topCallFrame);
    gpuLogMarker(b);
    apoWalkJsStack(*vm, tag, 24);
}
}

// Apotheosis: name the JavaScript that is running away, from inside the watchdog callback.
//
// Why here and nowhere else: this callback is the only hook that is guaranteed to run on the engine
// thread WHILE a runaway is in progress. It is invoked from VMTraps' NeedWatchdogCheck handler, on
// that thread, with the API lock held, before any termination is requested. Once a live tick stops
// returning, nothing else can reach the state: the port's own tick cannot run again, and a debugger
// cannot walk the stack -- the LLInt is offlineasm-generated assembly with no unwind info past its
// entry, so every `dps` sample so far bottomed out in C++ (WebCore event dispatch, microtask
// checkpoints) and never once named a script. See Doc/DZEN-SCROLL-DEATH.md section 10c.
//
// Once per process: the callback can be re-entered, and the first fire is the interesting one.
static void apoDumpRunawayJsStack(JSC::JSGlobalObject* globalObject)
{
    static bool alreadyDumped = false;
    if (alreadyDumped)
        return;
    alreadyDumped = true;
    if (!globalObject)
    {
        gpuLogMarker("js runaway: watchdog fired without a JSGlobalObject");
        return;
    }
    JSC::VM& vm = globalObject->vm();
    char b[192];
    sprintf_s(b, sizeof b, "js runaway: vm=%p topCallFrame=%p",
        (void*)&vm, (void*)vm.topCallFrame);
    gpuLogMarker(b);
    apoWalkJsStack(vm, "js runaway", 24);
}

// Apotheosis: arm the JSC watchdog for THIS script-execution window.
//
// Why re-armed everywhere instead of set once: Watchdog::startTimer computes the deadline from
// the moment of the CALL (enteredVM/exitedVM restart it per JS entry only relative to an already
// armed deadline). Armed once outside JS the deadline is absolute wall time -- 0.1.9.56 proved it
// the hard way: home loaded fine, and fifteen seconds after arming every script died instantly,
// which read as "crash at the very start of the next navigation". Re-arming at buildSession and
// at each live-tick top makes the 15 s count from the beginning of the window that can actually
// run long site code. Cheap by design: startTimer keeps an existing earlier deadline instead of
// restarting (see its early return in Watchdog.cpp).
static void apoArmJsWatchdog()
{
    JSC::VM* vm = WebCore::commonVMOrNull();
    char b[96];
    sprintf_s(b, sizeof b, "[BUILD] js watchdog arm: vm=%p", (void*)vm);
    gpuLogMarker(b);
    if (!vm)
        return;
    // Apotheosis: ensureWatchdog() CREATES the watchdog on first use -- getIfExists() stayed
    // null for the whole process in every previous build, because nothing else in this embedded
    // configuration ever touches it. Without creation there is no watchdog and no protection.
    JSC::Watchdog& wd = vm->ensureWatchdog();
    sprintf_s(b, sizeof b, "[BUILD] js watchdog arm: wd=%p", (void*)&wd);
    gpuLogMarker(b);
    wd.setTimeLimit(WTF::Seconds(15),
        [](JSC::JSGlobalObject* globalObject, void*, void*) -> bool {
            WebCorePort::portDiagLog("js watchdog fired: runaway script terminated");
            // Apotheosis: the callback returns true (= terminate), but that only sets a pending JS
            // exception -- it does not unwind C++. Whether the pending exception ever clears a frame
            // is the open question (PLAN.md item 0a), so record WHAT was interrupting us here, while
            // it is still on the stack.
            apoDumpRunawayJsStack(globalObject);
            return true;
        });
}
#endif
extern "C" void WebCoreSetGpuInitLogFile(const char* path)
{
    g_gpuInitLog[0] = 0;
    if (path && path[0]) {
        size_t n = std::strlen(path);
        if (n >= sizeof(g_gpuInitLog)) n = sizeof(g_gpuInitLog) - 1;
        std::memcpy(g_gpuInitLog, path, n);
        g_gpuInitLog[n] = 0;
        // Apotheosis: truncate ONCE per process, not on every call. The harness sets this path twice --
        // at startup and again inside EnableGpu just before WebCoreGpuInit -- and while that was harmless
        // when the file only carried GPU-init steps, it now carries every DBG_STAGE marker from the load
        // path. Truncating on the second call threw away everything the first navigation had recorded,
        // and "the first load behaves differently from the ones after it" has already been the shape of
        // two separate defects here, so losing exactly those markers is the worst possible trade.
        static bool truncated = false;
        if (!truncated) {
            truncated = true;
            FILE* f = fopen(g_gpuInitLog, "w");
            if (f) fclose(f);
        }
        // Apotheosis: arm the WTF text trace (`apotheosisWebTrace`, gated on APO_TRACE_TEXT in
        // wtf/text/TextBreakIterator.cpp) whenever a sibling `texttrace.txt` exists in the same
        // directory as this log, i.e. LocalState. Without this the trace needs an environment
        // variable set before the app starts, and that is impossible from the bench: x64-cycle.ps1
        // launches through `Start-Process "shell:AppsFolder\..."`, so the app is activated by the
        // shell and inherits the *shell's* environment, not the PowerShell session's. The port
        // therefore arms itself -- the same file-existence pattern `jstack.txt` uses in
        // PortChromeClient, and for the same two reasons: the switch has to work against an
        // already-installed build, and Device Portal cannot write LocalState on the Lumia.
        //
        // It has to happen HERE, not at first use: apotheosisWebTrace caches the flag in a
        // function-local static on its first call, and the first text lookup happens during the
        // first navigation. The cross-module `_putenv_s` -> `getenv` path is proven by
        // APOTHEOSIS_GLYPH_LOG, which the harness sets exactly this way and WebCore reads back.
        static bool traceChecked = false;
        if (!traceChecked) {
            traceChecked = true;
            std::string dir(g_gpuInitLog);
            const size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                const std::string probe = dir.substr(0, slash + 1) + "texttrace.txt";
                if (FILE* tf = fopen(probe.c_str(), "rb")) {
                    fclose(tf);
                    _putenv_s("APO_TRACE_TEXT", "1");
                    const std::string banner = "texttrace: armed by " + probe +
                        " (apotheosisWebTrace -> glyph.log)";
                    gpuLogMarker(banner.c_str());
                }
            }
        }
    }
}

// Reset g_session first: this destroys any previous Page (and pump residue) before a
// new one is built. emplace() plus w/h/load is all a session needs to report kOK --
// page/mainFrame/client are filled in by buildSession, and the caller tears down on failure.
// Apotheosis: debug logging via OutputDebugStringA (works in UWP, no windows.h needed)
// Apotheosis: debug stage marker written to diag buffer (read by harness via WebCoreGetDiag)
// Apotheosis: no local buffers in DBG_STAGE to avoid triggering /GS on buildSession
// Apotheosis: stage markers now reach the DEVICE, not just an attached debugger. They used to be
// OutputDebugStringA only, which on the phone means nowhere -- so every carefully placed marker inside
// buildSession and WebCoreSessionLoad was invisible exactly where it was needed. That is why three
// crashes in a row could only be located to "somewhere after before-load": the harness writes that line,
// the driver wrote the next twenty markers into a void.
//
// gpuLogMarkerF is the right sink and satisfies the constraint that makes this delicate: buildSession
// runs inside `#pragma strict_gs_check(push, off)` with __declspec(safebuffers), where a local array
// would reintroduce the security cookie this port had to remove to stop a 0xc0000409 during
// frame->init(). The formatting buffer lives in gpuLogMarkerF's own frame, in another function, so the
// caller gains nothing on its stack. And gpuLogMarker opens, writes and closes per line, so a step that
// never returns still leaves its own line on disk -- which is the whole point when the process dies
// without an exception, without a UEF line and without a minidump, as this one does.
//
// Cost: one fopen/fclose per marker. Acceptable for a diagnostic build and already how the GPU and [HIT]
// markers work; the file is gpuinit-steps.txt, readable over Device Portal.
#define DBG_STAGE(fmt, ...) do { \
    OutputDebugStringA(fmt "\n"); \
    gpuLogMarkerF("SL: " fmt, ##__VA_ARGS__); \
} while(0)
extern "C" void WebCorePortRecordNetError(const char* phase, const char* type, int code, const char* domain, const char* desc, const char* url);
// Apotheosis: GS check OFF to avoid 0xc0000409 stack canary crash during frame->init()
#pragma strict_gs_check(push, off)

#if defined(WK_WINUWP)
// Apotheosis: background main-document fetcher.
//
// Every silent-death unwind (.65-.70) was the same picture: WebCoreSessionLoad -> curl_easy_
// perform -> libcurl wait -> syscall, on the engine thread. The stall source is the phone's
// documented radio power-save (drops WiFi mid-transfer; no RST, no FIN, only silence), and no
// set of CURLOPT_TIMEOUT values is airtight across every libcurl phase -- threaded-resolver DNS
// and parts of the TLS handshake do not run our abort callbacks at all. The engine thread must
// simply never sit inside a transfer.
//
// So the transfer runs here, on one dedicated thread that owns one persistent easy handle (CA
// store parsed once per process, connection + TLS session reuse preserved from the persistent-
// handle fix). The engine posts a URL, waits BOUNDED (2.5 s) for completion, and on timeout
// returns an error page while this thread keeps going to its own 8 s budget and discards the
// result. Overlapping requests are rejected, not queued.
// Apotheosis 2026-09-18 (ST-4): one HTTP response hop, as the header callback saw it.
//
// Why this exists: ApoFetchChannel used curl's own cookie engine, which put the top-level document's
// cookies in a jar nothing else in the process can read. The engine's own loader
// (PortLoaderStrategy -> ResourceHandle -> NetworkStorageSession) reads WebCore's CookieJarDB.
// Measured 2026-09-18: dzen.ru -> sso.passport.yandex.ru/push answered Set-Cookie nine times
// (cookies=9), and the very next navigation -- the one the page's own JS issued, and therefore the
// one WebCore loaded -- went out with none of them, landing on an empty sso.dzen.ru/install. The
// fix is not a second jar but this: collect each response's Set-Cookie lines per hop and hand them
// to the jar that already exists, with the URL that sent them.
struct ApoFetchHop {
    long status { 0 };
    std::string location;
    std::vector<std::string> setCookies;
    // Apotheosis 2026-09-19: the hop's own `Content-Type`. Kept for the same reason Set-Cookie is kept
    // per hop -- the header belongs to the hop that sent it, and the charset of the LAST hop is the one
    // that decodes the body this port feeds as the document. Without it the decoder falls back to
    // `Settings::defaultTextEncodingName()`; see the feed site and the settings block for the measured
    // cost on news.ycombinator.com.
    std::string contentType;
};

// Case-insensitive "Name:" test that also returns the value. curl does not normalise header case
// for us, and a plain strncmp would miss "set-cookie:".
static bool apoHeaderField(const std::string& line, const char* name, std::string& valueOut)
{
    size_t n = std::strlen(name);
    if (line.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    // Require the colon immediately after the name: without this, "Set-Cookie2:" would match.
    if (line.size() <= n || line[n] != ':')
        return false;
    size_t v = n + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
        ++v;
    valueOut.assign(line, v, std::string::npos);
    return true;
}

// Apotheosis 2026-09-19: pull the charset out of a response `Content-Type` so it can be handed to
// `DocumentWriter::setEncoding`. Returns an empty string when the header carries no usable charset, and
// the caller then leaves the writer's encoding alone (the `Settings` default applies).
//
// WHY NOT PUT IT IN THE MIME TYPE. The obvious shape -- `setMIMEType("text/html; charset=utf-8")` --
// was written first, measured, and it DESTROYS the document. Both consumers of that string compare it
// for EXACT equality with the bare media type:
//
//   * `DOMImplementation::createDocument` (DOMImplementation.cpp:164):
//       `if (equalLettersIgnoringASCIICase(contentType, "text/html"_s)) return HTMLDocument::create(...)`
//     A parameterised string misses every branch, including the XHTML and text/plain ones, and falls
//     through to a generic `Document` -- no HTMLDocument, hence no HTML parser.
//   * `TextResourceDecoder::determineContentType` (TextResourceDecoder.cpp:265):
//       `if (equalLettersIgnoringASCIICase(mimeType, "text/html"_s)) return HTML;`
//     which likewise misses and returns `PlainText`.
//
// Measured 2026-09-19, appx 0.1.10.33: news.ycombinator.com rendered as its own **source listing**,
// every `<` swallowed, because the document was a plain `Document` with a plain-text decoder. So the
// MIME type stays bare and the charset travels in its own channel, which is exactly what the real
// loader does: `DocumentLoader::commitData` (DocumentLoader.cpp:1377) takes
// `response().textEncodingName()` and calls
// `m_writer.setEncoding(encoding, IsEncodingUserChosen::No)` before the first `addData`.
//
// That channel lands as `EncodingFromHTTPHeader` (DocumentWriter.cpp:294), so the header wins over the
// default but a `<meta charset>` in the body still overrides it -- this WebKit's meta path
// (`TextResourceDecoder::checkForMetaCharset`) calls `setEncoding(..., EncodingFromMetaTag)`
// unconditionally, with no source comparison (TextResourceDecoder.cpp:530).
//
// The media type in the header is deliberately IGNORED: this port feeds whatever it fetched as an HTML
// document, and claiming `image/png` here would not make that true.
static std::string apoCharsetForFeed(const std::string& contentType)
{
    if (contentType.empty())
        return { };
    // Find `charset` case-insensitively. A quoted value ("utf-8") is legal and common; strip the quotes
    // rather than passing them on, which would yield an encoding name ICU does not recognise -- i.e. the
    // fallback by accident rather than by decision.
    size_t at = std::string::npos;
    for (size_t i = 0; i + 7 <= contentType.size(); ++i) {
        char c = contentType[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != 'c')
            continue;
        std::string tail = contentType.substr(i, 7);
        for (char& t : tail)
            if (t >= 'A' && t <= 'Z') t = static_cast<char>(t - 'A' + 'a');
        if (tail == "charset") {
            at = i;
            break;
        }
    }
    if (at == std::string::npos)
        return { };
    size_t v = at + 7;
    while (v < contentType.size() && (contentType[v] == ' ' || contentType[v] == '\t'))
        ++v;
    if (v >= contentType.size() || contentType[v] != '=')
        return { };   // "charset" as a bare word is not a declaration
    ++v;
    while (v < contentType.size() && (contentType[v] == ' ' || contentType[v] == '\t'))
        ++v;
    size_t end = v;
    if (v < contentType.size() && contentType[v] == '"') {
        ++v;
        end = v;
        while (end < contentType.size() && contentType[end] != '"')
            ++end;
    } else {
        while (end < contentType.size() && contentType[end] != ';' && contentType[end] != ' '
               && contentType[end] != '\t')
            ++end;
    }
    if (end == v)
        return { };   // `charset=` with nothing after it
    std::string charset = contentType.substr(v, end - v);
    // A charset name is a token; anything with a control character, a quote or a backslash in it came
    // from a malformed header, and handing that to the decoder is worse than leaving the default.
    for (char c : charset)
        if (static_cast<unsigned char>(c) < 0x21 || c == '"' || c == '\\')
            return { };
    return charset;
}

// Apotheosis 2026-09-18: the worker's own activity watch, at file scope rather than inside run(), so
// the XFERINFO callback -- which has to be a captureless function pointer -- can hand it to the helper
// below. What it measures had to change, and the reason is a measurement rather than a preference.
//
// CURLOPT_XFERINFOFUNCTION's dlnow is a BODY byte count, and a redirect hop carries no body: a chain
// of 302s reports dlnow=0 for its whole life. A stall clock driven by dlnow alone therefore cannot tell
// "this hop's server is thinking" from "the radio is dead". Measured 2026-09-18 23:40 on the bench:
// https://dzen.ru/ answered 302 to https://login.vk.com/?act=autologin&... , ONE hop was recorded,
// dl stayed 0, and the callback returned 1 after 2.5 s -- i.e. curlcode=42, CURLE_ABORTED_BY_CALLBACK.
// That code reads like a network fault and is not one: the port aborted its own transfer, and it did so
// *before* curl's own CONNECTTIMEOUT(3 s)/TIMEOUT(8 s)/LOW_SPEED(1 B, 2 s) could report a reason of
// their own. A completed response header block IS activity, so the hop count is now part of the same
// test, and the callback no longer aborts at all -- it reports and returns 0, leaving every timeout
// with the component that can name it.
struct ApoFetchWatch {
    WTF::MonotonicTime start;
    WTF::MonotonicTime lastProgress;
    curl_off_t lastDl { -1 };
    // Apotheosis 2026-09-18: the engine thread reads this while it waits, so it has to be atomic and it
    // has to be a pointer to something with the channel's lifetime -- the watch itself is a stack local
    // of the worker's job.
    std::atomic<long long>* progressOut { nullptr };
    // Apotheosis 2026-09-18: the same publication for the hop count, which is the second kind of
    // activity the engine's stall detector has to see (see the wait loop in buildSession).
    std::atomic<long long>* hopsOut { nullptr };
    // Apotheosis 2026-09-18: read here to decide whether a response header block has arrived since the
    // last check. Written by the HEADERFUNCTION and read by XFERINFO, both on the worker thread inside
    // the same curl_easy_perform, so this one needs no atomic -- unlike progressOut/hopsOut above,
    // which the engine thread reads concurrently.
    std::vector<ApoFetchHop>* hops { nullptr };
    size_t lastHops { 0 };
    bool silenceReported { false };
    CURL* handle { nullptr };   // for curl_easy_getinfo, at report time only
};

// Apotheosis 2026-09-18: say what the transfer was actually doing when a rule fired, because the two
// numbers that were reported before (crc=42 on the SL: fetch line) name no cause, and 42 is the code
// for "our own callback aborted this". Formatted inside gpuLogMarkerF's frame -- no stack buffer is
// added here, and this region is not one of WebCoreDriver.cpp's strict_gs_check(push, off) blocks
// (CLAUDE.md).
static void apoFetchStallReport(const char* why, const ApoFetchWatch* w, curl_off_t dlnow)
{
    double conn = -1, tls = -1, ttfb = -1, total = -1;
    long hdrBytes = 0, connects = 0;
    if (w->handle) {
        curl_easy_getinfo(w->handle, CURLINFO_CONNECT_TIME, &conn);
        curl_easy_getinfo(w->handle, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(w->handle, CURLINFO_STARTTRANSFER_TIME, &ttfb);
        curl_easy_getinfo(w->handle, CURLINFO_TOTAL_TIME, &total);
        curl_easy_getinfo(w->handle, CURLINFO_HEADER_SIZE, &hdrBytes);
        curl_easy_getinfo(w->handle, CURLINFO_NUM_CONNECTS, &connects);
    }
    gpuLogMarkerF("SL: fetch-silent why=%s dl=%lld hops=%zu total=%.2f conn=%.2f tls=%.2f ttfb=%.2f "
                  "hdr=%ld conns=%ld -- reported, not aborted: curl's own timeouts will name the cause",
        why, (long long)dlnow, w->hops ? w->hops->size() : 0, total, conn, tls, ttfb, hdrBytes, connects);
}

struct ApoFetchChannel {
    std::thread thr;
    std::mutex mtx;
    std::condition_variable cvWork;
    std::condition_variable cvDone;
    // guarded by mtx:
    bool hasWork = false;
    bool shutdown = false;
    bool done = true;
    // Apotheosis 2026-09-18: set by the engine thread when it gives up on the 2500 ms budget, read
    // by the worker when the transfer it is still running finally finishes. Without it the log held
    // `curl bg timeout -- error page` and then, seconds later, a plain successful `SL: fetch eff=…
    // size=1016600 http=200 crc=0` with nothing tying the two together -- which reads as a second,
    // successful request rather than as the same one arriving too late to be used.
    bool gaveUp = false;
    std::string url;
    std::string html;
    std::string err;
    long httpCode = 0;
    int crc = 0;
    // Apotheosis 2026-09-18 (ST-4): the response hops of the last transfer, and the URL curl
    // actually landed on. Published to the engine thread with the body; the worker only fills them.
    std::vector<ApoFetchHop> hops;
    std::string effUrl;
    // Apotheosis 2026-09-19: the LAST hop's `Content-Type`, i.e. the one belonging to the body that is
    // about to be fed as the document. Published with the body for the same reason effUrl is: the
    // decision it feeds (which charset the decoder starts with) is made on the engine thread, long
    // after the worker's handle is gone.
    std::string effContentType;

    // Apotheosis 2026-09-18: bytes downloaded so far, published by the worker's XFERINFO callback and
    // read by the engine thread while it waits. This is what turns a fixed 2500 ms deadline into a
    // stall detector, so the engine only has to tell "moving but slow" from "not moving", and "slow but
    // moving" is the ordinary case for a 1 MB page. Advanced by the write callback (every body chunk)
    // and by XFERINFO (as a backstop during the header phase); it is also what WebCoreGetFetchProgress
    // publishes to the harness. See the sink's comment in run().
    //
    // Apotheosis 2026-09-19: 0, NOT -1, is its value before the first byte. The negative sentinel was
    // removed from BOTH counters because the export below folds them together, and a -1 in either place
    // made the folded value negative -- which the harness reads as "no fetch in flight" (`fetchprog=-1`).
    // Measured 04:35:35: a 3.4 MB page whose TTFB took longer than the harness's six beats was dumped as
    // a WEDGE (`completed fetchprog=-1` for six beats) and then loaded fine 2.6 s later,
    // `after-load url=https://dzen.ru/ rc=0 compositing=1`. "-1 means no fetch" is only useful if it is
    // the ONE thing -1 can mean.
    std::atomic<long long> progressDl { 0 };

    // Apotheosis 2026-09-18: completed response header blocks so far -- the second kind of activity, and
    // the one that was missing from the engine's stall detector. A redirect chain reports dl=0 for its
    // entire life, so a detector that watches only progressDl reads a healthy SSO chain as a dead radio
    // and gives up on it at the same 2500 ms the old fixed deadline used. Measured on dzen.ru's
    // 302 -> login.vk.com hop, 23:40. 0 completed hops is a truthful reading, so this starts at 0 too --
    // see progressDl above for why nothing here may be negative.
    std::atomic<long long> progressHops { 0 };

    // Apotheosis 2026-09-18: true from the moment the engine posts a job until the worker has
    // published its result -- i.e. it mirrors `!done`, which is a plain bool guarded by mtx and
    // therefore not readable from the UI thread. Published through WebCoreGetFetchProgress so the
    // harness's wedge watchdog can ask "is the engine blocked inside the top-level fetch, and are
    // bytes still arriving?" instead of inferring it from a hardcoded duration. That inference is
    // what broke when the fetch budget stopped being a fixed 2.5 s deadline: MainPage.xaml.cpp's
    // six-beat threshold was calibrated against that number (see its comment at the WEDGE site).
    std::atomic<bool> inFlight { false };

    CURL* h = nullptr;   // owned by, and only ever touched from, the worker thread

    void ensureWorker()
    {
        if (!thr.joinable())
            thr = std::thread(&ApoFetchChannel::run, this);
    }

    void run()
    {
        h = curl_easy_init();
        std::unique_lock<std::mutex> lk(mtx);
        for (;;) {
            cvWork.wait(lk, [this] { return hasWork || shutdown; });
            if (shutdown)
                return;
            std::string jobUrl = std::move(url);
            hasWork = false;
            done = false;
            lk.unlock();

            html.clear();
            err.clear();
            httpCode = 0;
            crc = CURLE_FAILED_INIT;
            char errbuf[CURL_ERROR_SIZE] = { 0 };
            std::string body;
            // Apotheosis 2026-09-18 (ST-4): the member, not a local -- the header callback below
            // and the engine thread both go through it, and the submit path cleared it under mtx.
            hops.clear();
            effUrl.clear();

            if (h) {
                // Apotheosis: reset clears OPTIONS but not caches -- parsed CA store, live
                // connections and TLS sessions survive, which is why the handle is persistent.
                curl_easy_reset(h);
                // Apotheosis 2026-09-18 (ST-2 experiment): enable curl's cookie engine on this
                // handle. It is THIS channel, not WebCore's ResourceHandle, that performs the
                // top-level document transfer (see the wp-feed below), and it used to run with the
                // cookie engine off: no Cookie went out, no Set-Cookie was stored, so
                // PortNetworkStorageSession's jar never saw the main document's cookies at all and
                // curl walked dzen.ru's SSO redirect chain with no session -- which is exactly the
                // shape of the loop (sso.dzen.ru/install?uuid=..., a fresh uuid every navigation).
                // NOTE the accept policy was never the cause: ensureDefaultPortStorageSession() is
                // not called from anywhere, so CookieJarDB's own default (CookieAcceptPolicy::Always)
                // is what is in force. Empty filename = in-memory jar only; curl_easy_reset does not
                // clear cookies, so they survive both the hops inside one transfer and the
                // navigations after it, while nothing is written to disk in the App Container.
                curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");
                // Apotheosis 2026-09-18: ApoFetchWatch is declared at file scope now -- the XFERINFO
                // callback is a captureless function pointer and hands it to apoFetchStallReport, which
                // cannot name a type declared inside this frame. The shape, and the reason it watches
                // response hops as well as body bytes, live with the struct.
                ApoFetchWatch watch;
                watch.start = watch.lastProgress = WTF::MonotonicTime::now();
                watch.progressOut = &progressDl;
                watch.hopsOut = &progressHops;
                watch.hops = &hops;
                watch.handle = h;
                // Apotheosis: deliberately NO local 'body' here -- the outer declaration above is
                // the one CURLOPT_WRITEDATA fills and the one body.swap(html) publishes. A shadow
                // declaration at this scope was exactly the .71 white-page bug: libpng... rather,
                // curl wrote every byte into the inner string, which died unseen at the closing
                // brace, while the channel published an empty document.

                curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
                curl_easy_setopt(h, CURLOPT_URL, jobUrl.c_str());
                curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
                curl_easy_setopt(h, CURLOPT_TIMEOUT, 8L);
                curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 3L);
                curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
                curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 2L);
                curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
                curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
                // Apotheosis 2026-09-18: this callback no longer aborts anything, and that is the fix
                // rather than a simplification. Returning 1 is the abort mechanism -- curlcode=42,
                // CURLE_ABORTED_BY_CALLBACK, a code that names how the transfer died and not why -- and
                // it used to fire after 2.5 s of an unchanged BODY byte count, which is what any
                // redirect hop looks like while its server is thinking: measured on dzen.ru's 302 to
                // login.vk.com, 23:40, one hop recorded, dl=0, killed at 2.5 s. Activity now means body
                // bytes OR a completed response header block; silence is reported instead of punished,
                // and CURLOPT_TIMEOUT (8 s), CURLOPT_CONNECTTIMEOUT (3 s) and CURLOPT_LOW_SPEED (1 B over
                // 2 s) stay as the rules that end a transfer, each able to name its own cause.
                curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION,
                    +[](void* clientp, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t) -> int {
                        auto* w = static_cast<ApoFetchWatch*>(clientp);
                        if (w->progressOut)
                            w->progressOut->store(dlnow, std::memory_order_relaxed);
                        const size_t hops = w->hops ? w->hops->size() : 0;
                        if (w->hopsOut)
                            w->hopsOut->store((long long)hops, std::memory_order_relaxed);
                        WTF::MonotonicTime now = WTF::MonotonicTime::now();
                        if (dlnow != w->lastDl || hops != w->lastHops) {
                            w->lastDl = dlnow;
                            w->lastHops = hops;
                            w->lastProgress = now;
                            w->silenceReported = false;
                            return 0;
                        }
                        if (!w->silenceReported && now - w->lastProgress > WTF::Seconds(3)) {
                            w->silenceReported = true;
                            apoFetchStallReport("silent-3s", w, dlnow);
                        }
                        return 0;
                    });
                curl_easy_setopt(h, CURLOPT_XFERINFODATA, &watch);
                curl_easy_setopt(h, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Safari/605.1.15");
                // Apotheosis 2026-09-18: the byte counter is advanced from the WRITE callback, not only
                // from XFERINFO. The progress meter runs at curl's own cadence (~1 Hz), which is slower
                // than the harness heartbeat (700 ms), so a counter driven by it alone reads "unchanged"
                // on roughly every other beat and the harness's stuck counter oscillates 0->1->0->1
                // instead of staying at 0. Measured exactly that: two `beat-stuck #1` lines eight
                // seconds apart, during an 8 s transfer that was making steady progress. The write
                // callback fires on every body chunk, so the counter then tracks the bytes themselves
                // and one beat of silence means one beat of silence.
                struct ApoWriteSink { std::string* body; std::atomic<long long>* progress; };
                ApoWriteSink sink { &body, &progressDl };
                curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t nmemb, void* userp) -> size_t {
                    auto* s = static_cast<ApoWriteSink*>(userp);
                    s->body->append(data, size * nmemb);
                    s->progress->store((long long)s->body->size(), std::memory_order_relaxed);
                    return size * nmemb;
                });
                curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
                // Apotheosis 2026-09-18 (ST-4): record the response headers hop by hop. This is also
                // the redirect chain ST-3 asked for -- the diag line carries only the final document
                // URL, and "redirects=1" without the hops cannot tell an SSO bounce from a
                // canonical-host rewrite. Every hop's Set-Cookie is kept with the hop that sent it,
                // because sso.passport.yandex.ru's cookie is not sso.dzen.ru's.
                curl_easy_setopt(h, CURLOPT_HEADERFUNCTION,
                    +[](char* data, size_t size, size_t nmemb, void* userp) -> size_t {
                        size_t n = size * nmemb;
                        auto* out = static_cast<std::vector<ApoFetchHop>*>(userp);
                        std::string line(data, n);
                        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                            line.pop_back();
                        if (line.compare(0, 5, "HTTP/") == 0) {
                            out->emplace_back();
                            // "HTTP/1.1 302 Found": the code follows the version, so find the first
                            // space rather than reading digits straight after "HTTP/".
                            auto sp = line.find(' ');
                            if (sp != std::string::npos)
                                out->back().status = std::strtol(line.c_str() + sp + 1, nullptr, 10);
                        } else if (!out->empty()) {
                            std::string value;
                            if (apoHeaderField(line, "Set-Cookie", value))
                                out->back().setCookies.push_back(WTF::move(value));
                            else if (apoHeaderField(line, "Location", value))
                                out->back().location = WTF::move(value);
                            // Apotheosis 2026-09-19: the body's declared encoding lives here and
                            // nowhere else for a page like news.ycombinator.com (no `<meta charset>`
                            // in the HTML at all). `apoHeaderField` is the same case-insensitive
                            // matcher used above, so "content-type:" matches too.
                            else if (apoHeaderField(line, "Content-Type", value))
                                out->back().contentType = WTF::move(value);
                        }
                        return n;
                    });
                curl_easy_setopt(h, CURLOPT_HEADERDATA, &hops);
                curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // accept gzip/deflate
                if (!g_caBytes.empty()) {
                    curl_blob blob;
                    blob.data = g_caBytes.data();
                    blob.len = g_caBytes.size();
                    blob.flags = CURL_BLOB_COPY;
                    curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
                }

                crc = (int)curl_easy_perform(h);
                curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);
                if (crc != CURLE_OK && errbuf[0])
                    err = errbuf;

                // Apotheosis 2026-09-18 (ST-2 experiment): what the transfer actually did, because
                // the diag line carries only the FINAL document URL and a redirect chain had to be
                // inferred from it. eff= is the last hop curl landed on, redirects= the number of
                // hops, cookies= how many cookies the handle holds for all domains after the
                // transfer. Those three numbers decide the dzen.ru hypothesis without guessing:
                // a chain whose cookies stay at 0 across a navigation that answered with
                // Set-Cookie is the SSO loop, and a cookies= that grows after this fix is the
                // proof that the engine now sees them. All formatting happens in gpuLogMarkerF's
                // own frame -- no stack buffer is added inside this strict_gs_check(push, off)
                // region (CLAUDE.md).
                char* effUrl = nullptr;
                curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effUrl);
                long redirects = 0;
                curl_easy_getinfo(h, CURLINFO_REDIRECT_COUNT, &redirects);
                struct curl_slist* jar = nullptr;
                int cookieCount = 0;
                if (curl_easy_getinfo(h, CURLINFO_COOKIELIST, &jar) == CURLE_OK && jar) {
                    for (struct curl_slist* e = jar; e; e = e->next)
                        ++cookieCount;
                    curl_slist_free_all(jar);
                }
                gpuLogMarkerF("SL: fetch eff=%s redirects=%ld cookies=%d size=%zu http=%ld crc=%d",
                    effUrl ? effUrl : "(null)", redirects, cookieCount, body.size(), httpCode, crc);
                // Apotheosis 2026-09-18 (ST-4): the chain itself (ST-3), and the effective URL kept
                // for the engine thread -- it is what the jar is asked about after the write.
                this->effUrl = effUrl ? effUrl : "";
                // Apotheosis 2026-09-19: the last hop that carried headers is the hop whose body we
                // hold. Logged next to eff= for the same reason eff= is logged: it is the input to a
                // rendering decision, and without it a wrong decode has no paper trail at all.
                if (!hops.empty())
                    this->effContentType = hops.back().contentType;
                gpuLogMarkerF("SL: fetch ctype=%.120s",
                    this->effContentType.empty() ? "(none)" : this->effContentType.c_str());
                for (size_t i = 0; i < hops.size(); ++i) {
                    gpuLogMarkerF("SL: hop[%zu] status=%ld setcookie=%zu loc=%.80s", i,
                        hops[i].status, hops[i].setCookies.size(),
                        hops[i].location.empty() ? "-" : hops[i].location.c_str());
                }
            }

            body.swap(html);
            lk.lock();
            done = true;
            inFlight.store(false, std::memory_order_release);
            if (gaveUp) {
                // Apotheosis 2026-09-18: name the pair explicitly. This is not a second request --
                // it is the same one, arriving after the engine had already given up on its budget
                // and put an error page on screen. The engine thread cannot log this itself: by the
                // time the bytes land it is long past the return, and nothing is waiting on them.
                size_t discarded = html.size();
                int discardedHttp = httpCode;
                int discardedCrc = crc;
                lk.unlock();
                gpuLogMarkerF("SL: DISCARDED -- engine gave up on the budget before this transfer finished; "
                              "the %zu bytes (http=%d crc=%d) are dropped, no second request was made. "
                              "If this line is present, the error page was wrong.",
                              discarded, discardedHttp, discardedCrc);
                lk.lock();
            }
            lk.unlock();
            cvDone.notify_all();
            lk.lock();
        }
    }
};

// Heap singleton: deliberately never destroyed, so the worker thread never races process exit
// (a function-local static's destructor would join-or-terminate against a blocked worker).
static ApoFetchChannel* apoFetchChannelGet()
{
    static ApoFetchChannel* c = new ApoFetchChannel;
    return c;
}
#endif

// Apotheosis 2026-09-18: a movement gauge for the top-level document fetch, so the harness's wedge
// watchdog can tell "the engine is blocked because a fetch is running" from "the engine is stuck".
//
// This exists because the fetch budget stopped being a fixed deadline, and the watchdog was calibrated
// against that deadline. MainPage.xaml.cpp dumps a wedged engine after six beats (~4.2 s) of an
// unchanged `finished` counter, and its threshold was chosen to sit "comfortably past the 2.5 s fetch
// ceiling" -- a real comment, and a real coupling between two files. With the wait now re-arming for as
// long as bytes keep arriving (up to a 9.5 s ceiling), that calibration is stale: a healthy 3 MB page
// blocks the engine thread for ~4.4 s and the watchdog dumps a stack of a page that is loading
// correctly. Measured 2026-09-18, the first slow fetch after the budget change produced exactly that
// false WEDGE.
//
// Apotheosis 2026-09-19: **the sign is the interface.** `-1` means, and may now ONLY mean, "no
// top-level fetch is in flight" -- the engine thread is free and a stalled `finished` counter is a real
// stall. Any non-negative value means a fetch IS in flight: the engine thread is *supposed* to be
// blocked, and how long it may stay blocked is bounded by the worker (CONNECTTIMEOUT 3 s, TIMEOUT 8 s,
// LOW_SPEED 1 B / 2 s) and by the 9.5 s ceiling above -- not by the watchdog's guess. During the
// pre-body phase (connect, TLS, TTFB, redirect chain) the value is a legitimate `0`.
//
// Measured 04:35:35 on a cold dzen.ru navigation, the run that forced this: TTFB took longer than six
// beats, so nothing was publishable in any form, and the two counters' shared `-1` sentinel folded into
// `dl + (hops << 40)` as a large NEGATIVE number -- which the harness reads as "no fetch in flight",
// i.e. as the opposite of the truth. The engine was never wedged: the fetch landed 2.6 s later and the
// same lines read `SL: fetch eff=https://dzen.ru/ redirects=0 size=3427391 http=200 crc=0` and
// `[STAGE] after-load url=https://dzen.ru/ rc=0 compositing=1`. The counters are therefore initialised
// and reset to 0, never to -1: a negative reading is now reachable only through `inFlight == false`,
// where it is the truth.
//
// Apotheosis 2026-09-18: the returned value is "did the transfer move", not "how many bytes" -- the hop
// count is folded in above any byte count that a document could plausibly reach, because a redirect
// chain reports 0 bytes while it is working perfectly (see progressHops). The caller compares the value
// against its own previous reading, so only a change has to be visible; the fold just keeps the two
// counters from ever aliasing into an unchanged number.
long long WebCoreGetFetchProgress(void)
{
#if defined(WK_WINUWP)
    ApoFetchChannel* c = apoFetchChannelGet();
    if (!c->inFlight.load(std::memory_order_acquire))
        return -1;
    return c->progressDl.load(std::memory_order_relaxed)
        + (c->progressHops.load(std::memory_order_relaxed) << 40);
#else
    return -1;
#endif
}

extern "C" void WebCorePortRecordNetError(const char* phase, const char* type, int code, const char* domain, const char* desc, const char* url);
// Apotheosis: GS check OFF to avoid 0xc0000409 stack canary crash during frame->init()
#pragma strict_gs_check(push, off)
__declspec(safebuffers)

static int buildSession(const char* url, int w, int h, uint8_t* outRGBA)
{
    DBG_STAGE("enter url=%s w=%d h=%d", url ? url : "null", w, h);
    using namespace WebCore;
    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid()) {
        DBG_STAGE("exit: bad URL");
        return kErrBadUrl;
    }
    DBG_STAGE("URL ok");

    OutputDebugStringA("[BUILD] pageConfigurationWithEmptyClients\n");
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());
    OutputDebugStringA("[BUILD] pageConfiguration ok\n");

    // The cookie jar. DOM access (document.cookie) and HTTP headers (Cookie/Set-Cookie, seen
    // by LoadingFrameLoaderClient::createNetworkingContext) have to share this one store.
    OutputDebugStringA("[BUILD] CookieJar::create\n");
    pageConfiguration.cookieJar = WebCore::CookieJar::create(WebCorePort::makeStorageSessionProvider());
    OutputDebugStringA("[BUILD] CookieJar ok\n");

    // Apotheosis 2026-09-18: Web Storage that actually stores. Without this line the Page
    // keeps the EmptyStorageNamespaceProvider that pageConfigurationWithEmptyClients()
    // installs (WebCore/loader/EmptyClients.cpp:560): setItem is accepted and dropped,
    // getItem is always null, length is always 0, and nothing throws. localStorage
    // therefore *looks* present -- which is why it was enabled below -- while being a
    // silent no-op. dzen.ru's settings-sync module is a read-after-write loop whose
    // convergence condition is exactly "the write is visible to the next read", so it
    // re-enters itself synchronously forever: the freeze in Doc/DZEN-SCROLL-DEATH.md
    // section 10f. The backend is in-memory (Src/port/PortStorage.h explains why it
    // cannot be durable here); it is process-scoped, not absent.
    pageConfiguration.storageNamespaceProvider = WebCorePort::createPortStorageNamespaceProvider();

    // GPU path: a GPU session (GL context + TextureMapper) needs a ChromeClient that can
    // hand back a root GraphicsLayer -- PortChromeClient::attachRootGraphicsLayer -- plus
    // the settings below. The empty-client default is EmptyChromeClient, i.e. Cairo only.
    // Apotheosis 2026-08-29: installed UNCONDITIONALLY. This used to be `if (g_gpuActive)`, and that
    // condition made the GPU path unreachable in practice:
    //
    //   * the ChromeClient is a UniqueRef fixed at Page::create, so whether a session has one is
    //     decided once and forever when the session is built;
    //   * the harness builds its home-page session BEFORE it calls WebCoreGpuInit (measured in
    //     gpuinit-steps.txt: the first two session builds occupy lines 1-38, "[GPU] enter" is line 39),
    //     so g_gpuActive was still false and chrome stayed null;
    //   * WebCoreComposite guards on `!g_session->chrome` and returns kErrNoSession, so the harness's
    //     first-frame probe read -12, concluded the GPU was unusable and latched onto software present
    //     permanently -- for every later session too, including ones that DID get a chrome client.
    //
    // Installing it always is safe, and safe by construction rather than by hope: WebCore reaches the
    // client only *inside* the setting. RenderLayerCompositor::cacheAcceleratedCompositingFlags reads
    // `settings->acceleratedCompositingEnabled()` first and only then asks
    // `client().allowedCompositingTriggers()` (RenderLayerCompositor.cpp:632-639). With the setting
    // false -- which is what setAcceleratedCompositingEnabled(g_gpuActive) below still gives on the
    // software path -- no layer is ever requested and PortChromeClient behaves exactly like
    // EmptyChromeClient. Software rendering therefore remains the baseline; the setting, not the
    // presence of the client, is the runtime gate.
    //
    // What this deliberately does NOT do: enable compositing unconditionally. That was tried once and
    // cost a silent __fastfail on the device with no dump; see CLAUDE.md.
    {
        auto chrome = WTF::makeUniqueRefWithoutRefCountedCheck<WebCorePort::PortChromeClient>();
        g_session->chrome = chrome.ptr();             // borrowed from the Page's UniqueRef -- owned by the Page, never freed here
        pageConfiguration.chromeClient = WTF::move(chrome);
    }

    DriverLoadState* loadPtr = &g_session->load;   // NOTE: g_session is never reset while these borrowed pointers are live
    WebCorePort::LoadingFrameLoaderClient** clientSlot = &g_session->client;
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        // Important: pageConfigurationWithEmptyClients() sets SandboxFlags::all() (which includes
        //   SandboxScripts), so ScriptController::canExecuteScripts() answers false however
        //   setScriptEnabled(true) is called -- the sandbox wins and no script ever runs, which
        //   breaks every SPA page. Clearing it here is what makes JS possible at all.
        params.effectiveSandboxFlags = { };
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [loadPtr, clientSlot](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
                *clientSlot = client.ptr();   // borrowed, so teardown can detach the load handler
                client->setLoadCompletionHandler([loadPtr](bool failed) {
                    if (loadPtr->mainDone)
                        return;
                    loadPtr->mainDone = true;
                    loadPtr->failed = failed;
                });
                return client;
            } };
    }

    OutputDebugStringA("[BUILD] Page::create start\n");
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    gpuLogMarkerF("SL: createPage @buildSession this=%p", (const void*)page.ptr());
    OutputDebugStringA("[BUILD] Page::create ok\n");
    g_session->page = page.ptr();
    DBG_STAGE("Page::create ok");
#if defined(WK_WINUWP)
    apoArmJsWatchdog();
#endif
    page->settings().setLoadsImagesAutomatically(g_apoImagesEnabled != 0);
    page->settings().setAcceleratedCompositingEnabled(g_gpuActive);   // GPU gate for this session: with it on, PortChromeClient gets a root layer to composite
#if defined(WK_WINUWP)
    // Apotheosis: move image decoding OFF the engine thread.
    //
    // The 0.1.9.62 wedgedump caught the engine inside PNGImageDecoder::decode -> rowAvailable ->
    // ScalableImageDecoderFrame::initialize (libpng below it), reached from
    // BitmapImageSource::nativeImageAtIndexForDrawing during paint: a large PNG being expanded
    // row-by-row into its backing store takes seconds-to-minutes on this Cortex-A57, and the
    // platform reaps an unresponsive app long before that finishes -- which finally explains why
    // every wedge landed wherever the current paint happened to be (fonts, curl, populateTasks):
    // they were all just what the engine was doing when the guillotine fell.
    //
    // Upstream's answer exists as a setting: large/animated images then decode on a background
    // queue while the old frame keeps painting, and completion invalidates back into the normal
    // rendering-update flow (which this port already services from its own pump).
    page->settings().setLargeImageAsyncDecodingEnabled(true);
    page->settings().setAnimatedImageAsyncDecodingEnabled(true);
#endif
    // Apotheosis: force-compositing must stay OFF, even when the GPU is active. This one line was the
    // cause of "the first page of a session paints, every page after it is blank", reproduced on both
    // lines on 2026-08-21:
    //   example.com (first load)   nonwhite=710656/710656  rs=C
    //   EnableGpu: first frame after init: EnableCompositing=0 Composite=-12
    //   example.com (second load)  nonwhite=0/710656       rs=C   <- same page, nothing painted
    //   news.ycombinator.com       nonwhite=0/710656       rs=C
    // The sequence matters: WebCoreGpuInit runs *between* the first and second load, so g_gpuActive is
    // false while the first session is built and true for every session after it. With
    // setForceCompositingMode(true) all content is promoted into GraphicsLayers, so the ordinary
    // FrameView paint that fills outRGBA produces an empty buffer -- and the GPU cannot show those
    // layers either, because the present path reports no root layer (EnableCompositing=0) and
    // kErrNoSession (-12). Worst of both worlds: the content leaves the software path without arriving
    // on the GPU one.
    // Enabling accelerated compositing is still right -- it *permits* layers, so a page that needs one
    // gets one and the TextureMapper path can present it. Forcing it is what removes the fallback, and
    // this port's stated rule is that software rendering is the base and GPU is a runtime opt-in.
    // If the GPU present path is ever made to deliver frames reliably, the honest way to use it is a
    // flag meaning "we are presenting through the GPU right now", set after a successful
    // WebCoreComposite -- not "the GPU was initialised at some point", which is all g_gpuActive says.
    page->settings().setForceCompositingMode(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
    // Apotheosis: JS was never enabled on the *session* path until setScriptEnabled(true) was
    // added. Now it is controlled from the harness Settings page via WebCoreConfigure().
    // JS off = "lightweight mode" for heavy SPA sites that would otherwise kill the process.
    //
    // Apotheosis 2026-08-25: the flags used to be read as g_apoJsEnabled != 0, and that was the
    // white-screen bug. The harness sets them through a QUEUED engine job (ApplySettings posts
    // "apply-settings" which calls WebCoreConfigure), so on a navigation the job can run AFTER the
    // nav-load job that builds the session -- the session then sees -1 "not configured" and sets
    // JS off, while settings.ini clearly says js=1. Hence scr=defer=0 on a page whose scripts were
    // supposed to run, a white body even though the document finished. Fixed by treating -1 as
    // "keep the default" rather than "off": if the harness never got its settings to us, the
    // sensible default is ON -- real pages need scripts, and the explicit off switch remains the
    // user's choice in Settings. (m_jsEnabled in the harness defaults to true, so when the queue
    // ordering works this is the same value anyway.)
    page->settings().setScriptEnabled(g_apoJsEnabled < 0 ? true : (g_apoJsEnabled != 0));
    page->settings().setLoadsImagesAutomatically(g_apoImagesEnabled != 0);
    // Apotheosis: bound the render-blocking suppression window. While a render-blocking resource
    // (a stylesheet) is outstanding, Document holds VisualUpdatesPreventedReason::RenderBlockingpdatesPreventedReason::RenderBlocking
    // and RenderLayer::shouldSuppressPaintingLayer() then refuses to paint *any* layer -- we get a
    // zero-filled surface, not a partially styled one. The reason clears either when the sheet
    // lands or when Document's suppression timer fires at this timeout, so this value is the hard
    // ceiling on "blank because WebCore will not draw yet". Upstream's 5s default is longer than
    // our entire pump budget, which is how news.ycombinator.com came back empty; 2s means a slow
    // or stalled stylesheet costs us the styling, never the whole page.
    page->settings().setIncrementalRenderingSuppressionTimeoutInSeconds(2);
    // DOM Storage: window.localStorage / sessionStorage only exist when the
    //   LocalStorageEnabled / SessionStorageEnabled settings are on. Without them the
    //   property is absent, so an SPA bundle dies on "Can't find variable: localStorage".
    page->settings().setLocalStorageEnabled(true);
    page->settings().setSessionStorageEnabled(true);
    // Apotheosis 2026-09-19: the document encoding, and this is the setting that decides it whenever
    // nothing else declares one.
    //
    // `DocumentWriter::decoder()` builds its decoder as
    // `TextResourceDecoder::create(m_mimeType, frame->settings().defaultTextEncodingName(), ...)`, and
    // `TextResourceDecoder::defaultEncoding` falls back to `PAL::Latin1Encoding()` when that string is
    // not a valid encoding. An unset `Settings` value is an EMPTY String, i.e. not valid, so the default
    // was Latin1 -- which ICU resolves as windows-1252. The cost was measured on news.ycombinator.com:
    // its HTML declares no charset at all and sends `charset=utf-8` only in the HTTP header, so
    // `E2 80 93` (U+2013) was decoded byte-by-byte as CP1252 and painted `â€"`. The header path is now
    // handled at the feed site via `DocumentWriter::setEncoding` (see `apoCharsetForFeed`); this
    // setting is the fallback for a document that declares nothing anywhere.
    //
    // UTF-8 is the right fallback for this port: there is no legacy-encoding user base to keep, Cyrillic
    // content is a project goal, and a `<meta charset>` or a header charset still wins -- this WebKit's
    // meta path calls `setEncoding(..., EncodingFromMetaTag)` unconditionally
    // (TextResourceDecoder.cpp:530). Measured before this change: a `file://` probe whose `<meta
    // charset>` overrode the same Latin1 default and rendered every non-ASCII line correctly.
    page->settings().setDefaultTextEncodingName("utf-8"_s);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    page->setIsVisible(true);

    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;
    DBG_STAGE("got mainFrame");
    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    DBG_STAGE("setView done");
    OutputDebugStringA("[BUILD] frame->init start\n");
    localMainFrame->init();
    OutputDebugStringA("[BUILD] frame->init done\n");
    g_session->mainFrame = localMainFrame;

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);
    view->resize(IntSize(w, h));

    // A headless page has to be active AND focused, or EventHandler drops input and the
    // focus APIs (focus events, document.hasFocus()) misreport. This must come after
    // setView()+init(): setActiveInternal calls selection().pageActivationChanged(), which
    // faults on a null document/view at null+0x858 (0.1.0.9, RVA 0x1E9309 -- measured).
    page->focusController().setActive(true);
    page->focusController().setFocused(true);

    // ---- Network load via curl (bypasses StubLoaderStrategy no-op) ----
    // Download HTML synchronously, then feed through DocumentWriter.
    // The FrameLoader::load() path is broken because StubLoaderStrategy
    // swallows all ResourceLoader requests - nothing ever loads.
    DBG_STAGE("curl download url=%s", url);
    std::string html;
    long httpCode = 0;
    // Apotheosis 2026-09-19: the last hop's `Content-Type` -- where the charset that decodes this body
    // is stated -- and the charset extracted from it. Declared OUTSIDE the WK_WINUWP guard on purpose:
    // the feed site that consumes them sits past the `#endif`, and a pair of std::strings needs nothing
    // from the platform -- keeping them guarded would compile here and break the non-UWP build for no
    // reason. An empty charset means "no declaration in the header"; the `Settings` default then applies.
    std::string fetchedContentType;
    std::string feedCharset;
#if defined(WK_WINUWP)
    // Apotheosis 2026-09-18 (ST-4): the response hops and the effective URL, out here because the
    // channel is out of scope past the block below and both are needed afterwards, on this thread.
    // ApoFetchHop is itself inside the WK_WINUWP guard, so these must be too.
    std::vector<ApoFetchHop> hops;
    std::string fetchedEffUrl;
#endif
#if defined(WK_WINUWP)
    // Apotheosis: the fetch runs on its own thread (apoFetchChannelGet above); the engine waits
    // BOUNDED. A stalled radio used to park this thread inside curl_easy_perform past the
    // platform's patience -- every silent death unwound to exactly that. Now: 2.5 s without a byte
    // moving (see the re-arming loop below -- it is a stall detector, not a deadline), then
    // an error page while the worker finishes its own teardown and discards the result.
    // Overlapping requests are rejected outright, not queued.
    char errbuf[CURL_ERROR_SIZE] = { 0 };
    int crc = (int)CURLE_OPERATION_TIMEDOUT;
    bool budgetExpired = false;
    {
        auto& ft = *apoFetchChannelGet();
        ft.ensureWorker();
        {
            std::lock_guard<std::mutex> lk(ft.mtx);
            if (!ft.done || ft.hasWork) {
                DBG_STAGE("curl bg busy -- previous fetch still running");
                WebCorePortRecordNetError("toplevel-fetch", "CurlBusy", 0, "curl-busy", "previous background fetch still running", url);
                return kErrCurlDownload;
            }
            ft.url = url;
            ft.html.clear();
            ft.err.clear();
            ft.httpCode = 0;
            ft.crc = (int)CURLE_OK;
            ft.hops.clear();
            ft.effUrl.clear();
            ft.effContentType.clear();
            ft.done = false;
            ft.gaveUp = false;
            ft.progressDl.store(0, std::memory_order_relaxed);
            ft.progressHops.store(0, std::memory_order_relaxed);
            ft.inFlight.store(true, std::memory_order_release);
            ft.hasWork = true;
        }
        ft.cvWork.notify_one();
        std::unique_lock<std::mutex> lk(ft.mtx);
        // Apotheosis 2026-09-18: a FIXED 2500 ms deadline could not tell a dead radio from a slow
        // transfer, and the second is the ordinary case -- a ~1 MB page (measured: the dzen.ru article
        // this replaced) does not fit, so the port showed an error page for a page it then fetched
        // successfully and discarded. The wait now RE-ARMS while bytes are still arriving.
        //
        // The protection the old deadline provided is not weakened, it is delegated: the worker ends a
        // real stall from its own side, and its rules are the ones that can NAME a cause --
        // CURLOPT_CONNECTTIMEOUT (3 s), CURLOPT_TIMEOUT (8 s) and CURLOPT_LOW_SPEED_LIMIT/TIME (1 B over
        // 2 s). An unchanged byte count over a full budget therefore means curl is already tearing the
        // transfer down. The ceiling sits just above the worker's own 8 s so that the worker's error --
        // which carries curl's reason -- is preferred to this function's guess.
        //
        // NOTE (Apotheosis 2026-09-18): the XFERINFO callback is NOT on that list any more, and this
        // comment claimed it was until it was fixed here. It used to return 1 after 2.5 s of an unchanged
        // body byte count, i.e. it aborted the transfer itself and produced crc=42
        // (CURLE_ABORTED_BY_CALLBACK -- a code naming how the transfer died, not why) for what a redirect
        // hop looks like while its server thinks. It now reports silence and aborts nothing.
        const std::chrono::milliseconds budget { 2500 };
        const std::chrono::milliseconds ceiling { 9500 };
        const auto waitStart = std::chrono::steady_clock::now();
        long long seenDl = ft.progressDl.load(std::memory_order_relaxed);
        long long seenHops = ft.progressHops.load(std::memory_order_relaxed);
        bool haveReading = false;
        bool finished = false;
        for (;;) {
            if (ft.cvDone.wait_for(lk, budget, [&] { return ft.done; })) {
                finished = true;
                break;
            }
            const long long nowDl = ft.progressDl.load(std::memory_order_relaxed);
            const long long nowHops = ft.progressHops.load(std::memory_order_relaxed);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStart);
            if (elapsed >= ceiling) {
                DBG_STAGE("curl bg: ceiling %lld ms reached while still progressing (dl=%lld hops=%lld)",
                    (long long)ceiling.count(), nowDl, nowHops);
                break;
            }
            if (!haveReading) {
                // Apotheosis 2026-09-18: the first reading is ADOPTED, with no movement claim attached.
                // That covers the connect + first-byte phase -- the one window in which a `moved` verdict
                // cannot exist yet -- and it costs one extra budget only when nothing at all has happened;
                // a dead radio is already being ended by curl's own CONNECTTIMEOUT (3 s), whose error this
                // thread then reports. The alternative this replaced was worse than useless: the counters
                // used to start at -1 and the first real reading (0, before any body byte) was compared
                // against that sentinel, so the log printed "still moving (dl=0)" for a transfer that had
                // not moved at all and bought it a re-arm it had not earned.
                seenDl = nowDl;
                seenHops = nowHops;
                haveReading = true;
                DBG_STAGE("curl bg: first reading dl=%lld hops=%lld at %lld ms, re-arming (nothing measured yet)",
                    nowDl, nowHops, (long long)elapsed.count());
                continue;
            }
            // Apotheosis 2026-09-18: a completed response header block is activity too. A redirect
            // chain reports dl=0 for its whole life, so a detector watching bytes alone gives up on a
            // healthy SSO chain exactly as the old fixed deadline did -- measured on dzen.ru's
            // 302 -> login.vk.com hop at 23:40, where one hop and dl=0 was read as a stall.
            const bool moved = (nowDl != seenDl) || (nowHops != seenHops);
            if (!moved) {
                DBG_STAGE("curl bg: no progress for %lld ms (dl=%lld hops=%lld), giving up",
                    (long long)budget.count(), nowDl, nowHops);
                break;
            }
            DBG_STAGE("curl bg: still moving (dl=%lld hops=%lld, %lld ms in), re-arming",
                nowDl, nowHops, (long long)elapsed.count());
            seenDl = nowDl;
            seenHops = nowHops;
        }
        if (finished) {
            html = std::move(ft.html);
            httpCode = ft.httpCode;
            crc = ft.crc;
            hops = std::move(ft.hops);
            fetchedEffUrl = std::move(ft.effUrl);
            fetchedContentType = std::move(ft.effContentType);
            // Apotheosis 2026-09-19: extracted HERE, while the header is in hand, rather than at the feed
            // site -- `apoCharsetForFeed` is inside the WK_WINUWP guard with the rest of the fetcher, and
            // the feed is past the `#endif`. The decision is the same either way.
            feedCharset = apoCharsetForFeed(fetchedContentType);
            strncpy_s(errbuf, sizeof errbuf, ft.err.c_str(), _TRUNCATE);
        } else {
            // Apotheosis 2026-09-18: this used to be filed as type=Curl, curlcode=28, i.e. as a
            // libcurl timeout -- and it was read as one, twice: in the field ("error curlcode=28"
            // reported as a network fault) and in the first write-up of it. It is not one. curl
            // never timed out; the 2500 ms budget above is the PORT's own, the worker is still
            // running, and it can deliver the page seconds later -- measured on dzen.ru, the very
            // URL this path had just reported as failed came back `size=1016600 http=200 crc=0`
            // and was discarded. The type now names the budget so the next reader does not go
            // looking for a network fault. Honest-diagnostic rule, CLAUDE.md.
            budgetExpired = true;
            ft.gaveUp = true;
            strcpy_s(errbuf, "no download progress for 2500 ms; transfer being torn down, result will be discarded");
            DBG_STAGE("curl bg timeout -- error page, worker continues");
        }
    }
    DBG_STAGE("curl done crc=%d http=%ld size=%zu", crc, httpCode, html.size());
    if (crc != CURLE_OK) {
        if (budgetExpired)
            WebCorePortRecordNetError("toplevel-fetch", "PortBudget", 0, "port-budget", errbuf, url);
        else
            WebCorePortRecordNetError("toplevel-fetch", "Curl", crc, "curl", errbuf[0] ? errbuf : curl_easy_strerror((CURLcode)crc), url);
        return kErrCurlDownload;
    }
    // Apotheosis: CURLINFO_RESPONSE_CODE is 0 for non-HTTP schemes (file:, data:), so gating on
    // it unconditionally rejected every such URL; only enforce the range when one was produced.
    if (httpCode != 0 && (httpCode < 200 || httpCode >= 400))
        return -100 - static_cast<int>(httpCode);

    // Apotheosis 2026-09-18 (ST-4): file this transfer's cookies into WebCore's jar -- the one
    // PortLoaderStrategy -> ResourceHandle -> NetworkStorageSession reads -- because the fetch
    // above, not ResourceHandle, performed this transfer, and until now that jar never saw them.
    // The direction implemented is the missing one: top-level -> engine. The reverse edge (an
    // engine-set cookie going out on the next top-level navigation) is NOT added here: the dzen
    // chain gets its cookies from top-level responses, so it is not needed for this defect, and
    // adding it speculatively would mean two writers on one header. Measure before adding it.
    //
    // On the ENGINE thread, which is the point: defaultPortStorageSession() asserts isMainThread()
    // and the worker that produced these headers is not it. Hop URLs are reconstructed by following
    // Location, because a Set-Cookie must be filed against the URL that sent it --
    // sso.passport.yandex.ru's cookie is not sso.dzen.ru's.
    if (!hops.empty()) {
        int applied = 0;
        std::string hopUrl = url;
        for (size_t i = 0; i < hops.size(); ++i) {
            URL hopParsed { String::fromUTF8(hopUrl.c_str()) };
            if (!hopParsed.isValid())
                break;
            for (const auto& cookie : hops[i].setCookies) {
                WebCorePort::defaultPortStorageSession().setCookiesFromHTTPResponse(
                    parsedURL, hopParsed, String::fromUTF8(cookie.c_str()));
                ++applied;
            }
            if (hops[i].location.empty())
                break;
            URL next { hopParsed, String::fromUTF8(hops[i].location.c_str()) };
            if (!next.isValid())
                break;
            hopUrl = next.string().utf8().data();
        }
        gpuLogMarkerF("SL: cookies applied=%d hops=%zu eff=%.120s", applied, hops.size(),
            fetchedEffUrl.empty() ? "-" : fetchedEffUrl.c_str());
    }
#else
    // Apotheosis 2026-09-03: DEAD CODE ON BOTH ARCHITECTURES. Kept only as the reference for what a
    // plain synchronous fetch looked like before the background channel above replaced it.
    //
    // The comment here used to read "x64 bench line: unchanged per-load synchronous fetch (untouched
    // by the ARM fix)", which is false and cost an hour: it says this branch is what x64 compiles,
    // and I believed it and announced a sixth divergence between the lines on the strength of it.
    // WK_WINUWP is defined for BOTH driver builds -- verified in the generated response file,
    // Src/port/_driver_x64_compile.rsp carries -DWK_WINUWP=1 -- because both compile scripts inherit
    // DEFINES from their build directory's build.ninja, and both trees set it. So x64 takes the
    // background-fetch branch too, and the engine thread is bounded on both.
    //
    // A comment that names the wrong architecture is worse than no comment: it reads as a measurement.
    {
        CURL* h = curl_easy_init();
        if (!h)
            return kErrCurlInit;
        curl_easy_setopt(h, CURLOPT_URL, url);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(h, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Safari/605.1.15");
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t nmemb, void* userp) -> size_t {
            static_cast<std::string*>(userp)->append(data, size * nmemb);
            return size * nmemb;
        });
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &html);
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // accept gzip/deflate
        char errbuf[CURL_ERROR_SIZE] = { 0 };
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
        if (!g_caBytes.empty()) {
            curl_blob blob;
            blob.data = g_caBytes.data();
            blob.len = g_caBytes.size();
            blob.flags = CURL_BLOB_COPY;
            curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
        }
        CURLcode crc = curl_easy_perform(h);
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(h);
        DBG_STAGE("curl done crc=%d http=%ld size=%zu", (int)crc, httpCode, html.size());
        if (crc != CURLE_OK) {
            WebCorePortRecordNetError("toplevel-fetch", "Curl", (int)crc, "curl", errbuf[0] ? errbuf : curl_easy_strerror(crc), url);
            return kErrCurlDownload;
        }
        if (httpCode != 0 && (httpCode < 200 || httpCode >= 400))
            return -100 - static_cast<int>(httpCode);
    }
#endif

    // Feed HTML through DocumentWriter (bypasses FrameLoader::load entirely)
    DBG_STAGE("DocumentWriter feed");
    {
        Ref<FrameLoader> loader = localMainFrame->loader();
        RefPtr<DocumentLoader> activeLoader = loader->activeDocumentLoader();
        if (!activeLoader)
            return kErrNoLoader;
        DocumentWriter& writer = activeLoader->writer();
        // Apotheosis 2026-09-19: the charset travels in its OWN channel, and the MIME type stays bare --
        // see `apoCharsetForFeed` for the measurement behind that (`text/html; charset=utf-8` in the MIME
        // type destroys the document, because both `DOMImplementation::createDocument` and
        // `TextResourceDecoder::determineContentType` compare it for exact equality with `text/html`).
        // This mirrors `DocumentLoader::commitData`: the response's charset goes to `setEncoding`, and the
        // decoder then starts as UTF-8 instead of Latin1. news.ycombinator.com is the case this fixes --
        // its HTML declares no charset at all and states `charset=utf-8` only in the header this port was
        // dropping, so `E2 80 93` (U+2013) painted as `â€"`. Logged because a wrong decode with no record
        // of its input is exactly the blind spot this closes.
        DBG_STAGE("DocumentWriter charset=%s (fetched Content-Type was %.100s)",
            feedCharset.empty() ? "(none, Settings default applies)" : feedCharset.c_str(),
            fetchedContentType.empty() ? "(none)" : fetchedContentType.c_str());
        writer.setMIMEType("text/html"_s);
        writer.begin(parsedURL);
        // Apotheosis 2026-09-19: `setEncoding` must come AFTER `begin()`, and that placement is not
        // cosmetic. `begin()` ends with `clear()` (DocumentWriter.cpp:199), and `clear()` resets
        // `m_encoding` (line 114) unless the encoding was marked user-chosen -- so setting it before the
        // call silently discards it. This is also exactly where upstream sets it: `DocumentLoader::
        // commitData` calls `m_writer.setEncoding(...)` and then `m_writer.addData(data)` on the SAME
        // first chunk. The decoder itself is created lazily on the first `addData`, so between the two
        // lines the value is read and honoured.
        if (!feedCharset.empty())
            writer.setEncoding(String::fromUTF8(feedCharset.c_str()), DocumentWriter::IsEncodingUserChosen::No);
        // Apotheosis 2026-09-19: feed in CHUNKS and count each one, instead of one addData of the whole
        // document. Both halves are the fix for the same blind window: measured on a cold dzen.ru
        // navigation, the 3.6 MB body was handed over in a single `addData`, inside which the engine
        // thread parsed it and ran its inline JS for several seconds with nothing published anywhere --
        // so the harness's wedge watchdog saw a frozen `finished` counter, no fetch in flight, and dumped
        // a full engine stack for a page that finished loading 1.9 s later. This is not a behaviour
        // change for the parser: WebKit's own DocumentLoader calls `addData` once per network chunk, so
        // arbitrary boundaries are the normal input, and `apoEngineTick` gives the watchdog a countable
        // statement of progress between them.
        constexpr size_t kFeedChunk = 256 * 1024;
        for (size_t off = 0; off < html.size(); off += kFeedChunk) {
            size_t n = kFeedChunk;
            if (off + n > html.size())
                n = html.size() - off;
            Ref<SharedBuffer> chunk = SharedBuffer::create(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(html.data()) + off, n));
            writer.addData(chunk.get());
            apoEngineTick();
        }
        if (html.empty()) {
            // Preserve the old empty-document path exactly: one addData with a zero-length buffer.
            Ref<SharedBuffer> empty = SharedBuffer::create();
            writer.addData(empty.get());
            apoEngineTick();
        }
        writer.end();
    }
    DBG_STAGE("DocumentWriter feed done");
    apoEngineTick();

    // Short pump for inline JS/synchronous layout
    pumpLoop(*localMainFrame, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ true,
             /*settleCapTicks*/ 40, /*watchdog*/ 5.0, /*pageForRendering*/ page.ptr());
    DBG_STAGE("pumpLoop done");
    apoEngineTick();
    // Apotheosis: what is still running at the moment the load is declared finished. From here the
    // engine thread returns to its job queue and stops cycling the RunLoop, so anything counted
    // here is a response that will be parsed on the curl worker and delivered whenever something
    // next happens to cycle the loop -- possibly after the next teardown.
    WebCorePort::portLoaderMark("pumpLoop done");

    // WebKit can replace the LocalFrameView during the parse, so re-read it here
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));

    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;
    document->updateLayoutIgnorePendingStylesheets();
    apoEngineTick();

    // SPA probe: kick the module import so a bundle that only runs on that promise
    // starts (React and friends). It can replace frame/view/document -- re-read below.
    probeSpaModule(page.get(), *localMainFrame);
    // Apotheosis 2026-09-04: markers across the post-probe tail. This stretch used to be
    // trace-free, and the 0.1.9.84 death landed inside it: port-trace.txt ends at
    // probeSpaModule's own "spa: probe rc=0" line and the harness never reached "after-load",
    // so the whole of the code below was one unlit window ~0.9 s wide. The heartbeat put the
    // process at busy=1 for less than one beat, which also rules the wedge dumper out -- it
    // needs six. Per-line flushed markers are the only channel that survives here, so the
    // window is now lit rather than reasoned about.
    WebCorePortTrace("tail: probe returned");
    localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrFrameGone;
    g_session->mainFrame = localMainFrame;
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));
    document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;
    WebCorePortTrace("tail: updateLayout enter");
    document->updateLayoutIgnorePendingStylesheets();
    WebCorePortTrace("tail: extractLinks enter");
    extractLinks(document.get(), h);

    int nonWhite = 0;
    // The suspect this bracket exists to convict or clear: layertest.html is the first page in
    // the sequence whose rootLayer() is non-null, so it is the first to take the TextureMapper
    // branch of paintToRGBA rather than falling through to Cairo.
    WebCorePortTrace("tail: paintToRGBA enter (rootLayer=%d)",
        (g_session && g_session->chrome && g_session->chrome->rootLayer()) ? 1 : 0);
    int prc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
    WebCorePortTrace("tail: paintToRGBA exit rc=%d nonWhite=%d", prc, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*document, *view, w, h, nonWhite);
    WebCorePortTrace("tail: writeDiag done");
    return kOK;
}

// Repaint after an interaction (click/scroll/resize) and hand back the new frame
static int finishInteractionPaint(uint8_t* outRGBA)
{
    using namespace WebCore;
    // Apotheosis: stage timing, enabled only by the resize path (see g_stageTrace). Scalar
    // locals only — no arrays — because this function sits inside strict_gs_check(push, off).
    const MonotonicTime traceT0 = MonotonicTime::now();
    MonotonicTime tracePrev = traceT0;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(g_session->w, g_session->h));
    if (g_stageTrace) {
        gpuLogMarkerF("[GPU]   fip: view->resize (+%.0f ms)", (MonotonicTime::now() - tracePrev).milliseconds());
        tracePrev = MonotonicTime::now();
    }
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    if (g_stageTrace) {
        gpuLogMarkerF("[GPU]   fip: updateLayout (+%.0f ms)", (MonotonicTime::now() - tracePrev).milliseconds());
        tracePrev = MonotonicTime::now();
    }
    extractLinks(doc.get(), g_session->h);
    if (g_stageTrace) {
        gpuLogMarkerF("[GPU]   fip: extractLinks (+%.0f ms)", (MonotonicTime::now() - tracePrev).milliseconds());
        tracePrev = MonotonicTime::now();
    }
    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (g_stageTrace) {
        gpuLogMarkerF("[GPU]   fip: paintToRGBA rc=%d (+%.0f ms)", prc, (MonotonicTime::now() - tracePrev).milliseconds());
        tracePrev = MonotonicTime::now();
    }
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    if (g_stageTrace)
        gpuLogMarkerF("[GPU]   fip: writeDiag (+%.0f ms, total %.0f ms)",
            (MonotonicTime::now() - tracePrev).milliseconds(), (MonotonicTime::now() - traceT0).milliseconds());
    DBG_STAGE("exit: kOK");
    return kOK;
}
#pragma strict_gs_check(pop)

extern "C" void WebCorePortRecordNetError(const char* phase, const char* type, int code, const char* domain, const char* desc, const char* url)
{
    // Apotheosis 2026-09-18 (ST-6): phase and type are new, and they are the whole point. The old
    // string was `curlcode=0 domain= desc= url=` when a failure dispatch carried a ResourceError of
    // type Null -- which the port's own LoaderStrategy error factories return -- and that was read as
    // "no error, therefore no request was made". It is neither: the dispatch fired, and the type says
    // the load was refused or cancelled before it left. Phase says *which* dispatch, because this
    // holds only the last failure of the session and a failed subresource overwrites a navigation one.
    std::snprintf(g_lastNetError, sizeof g_lastNetError,
        "phase=%s type=%s curlcode=%d domain=%s desc=%s url=%s",
        phase ? phase : "", type ? type : "", code,
        domain ? domain : "", desc ? desc : "", url ? url : "");
}

extern "C" {

// Point curl/OpenSSL at a CA-certificate bundle (PEM) for TLS verification.
// Required in the App Container sandbox, which cannot reach the Windows system
// trust store: without this, every HTTPS handshake fails server-trust eval.
// Must be called before the first network request; idempotent (last call wins).
// `path` is a UTF-8 filesystem path to a Mozilla-style cacert.pem.
void WebCoreSetCACertPath(const char* path)
{
    if (!path || !*path)
        return;

    // CurlContext::singleton() also boots libcurl + OpenSSL the same way
    // ResourceHandle::start() does, so this is safe to call standalone.
    WebCore::CurlContext::singleton().sslHandle().setCACertPath(String::fromUTF8(path));
}

// Apotheosis: configure content settings from the harness Settings page. Called before first
// load; the flags are read by buildSession when creating each Page.
extern "C" void WebCoreConfigure(int jsEnabled, int imagesEnabled)
{
    g_apoJsEnabled = jsEnabled;
    g_apoImagesEnabled = imagesEnabled;
}

// Inject the CA-certificate bundle as an in-memory PEM blob (CURLOPT_CAINFO_BLOB).
// App Container blocks OpenSSL's file-based CA loading (SSL_CTX_load_verify_locations
// fails even on a readable file in the app's own LocalState (curl 77), so the
// path-based WebCoreSetCACertPath does not work on device; the blob bypasses all
// file I/O. `data` is the raw cacert.pem bytes (PEM text). Call before first load.
void WebCoreSetCACertBlob(const uint8_t* data, int len)
{
    if (!data || len <= 0)
        return;
    WebCorePortTrace("SetCACertBlob: enter len=%d", len);
    Vector<uint8_t> bytes(static_cast<size_t>(len));
    std::memcpy(bytes.mutableSpan().data(), data, static_cast<size_t>(len));
    WebCorePortTrace("SetCACertBlob: vector built");
    // CACertInfo holds the Vector; curl_blob uses CURL_BLOB_NOCOPY, so the bytes must
    // outlive requests -- the singleton CurlSSLHandle owns them for the process lifetime.
    WebCore::CurlContext::singleton().sslHandle().setCACertData(WTF::move(bytes));
    WebCorePortTrace("SetCACertBlob: setCACertData done");
    // Keep a copy for the standalone WebCoreDownload curl handle (separate from the render bridge).
    g_caBytes.assign(data, data + len);
    WebCorePortTrace("SetCACertBlob: done");
}

// Copy the last recorded network-load error (set on WebCoreLoadUrl failure) into
// `buf`. Returns the number of bytes written (excluding NUL). Empty if no error.
int WebCoreGetLastError(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastNetError);
    return n < 0 ? 0 : n;
}

// Copy the last render diagnostic (final URL / title / contents size / non-white
// pixel count from the most recent WebCoreLoadUrl) into `buf`. For debugging blank
// renders: distinguishes "engine rendered nothing" from "bitmap not displayed".
int WebCoreGetDiag(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastDiag);
    return n < 0 ? 0 : n;
}

// Copy the most recent loaded page title (UTF-8) into buf. Empty if none.
int WebCoreGetTitle(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastTitle);
    return n < 0 ? 0 : n;
}

// Copy the most recently rendered document's final URL (UTF-8) into buf. Used by the
// harness to detect a navigation triggered inside the live session (click default action).
int WebCoreGetUrl(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastUrl);
    return n < 0 ? 0 : n;
}

// Evaluate JS in the page and copy the string result into out; 0 or an error code
int WebCoreEvalJS(const char* script, char* out, int len)
{
    if (!script || !out || len <= 0)
        return kErrBadArgs;
    out[0] = '\0';
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    return evalJS(*g_session->mainFrame, script, out, len);
}

// how many links the current table holds
int WebCoreGetLinkCount()
{
    return static_cast<int>(g_links.size());
}

// link i: viewport-relative rect plus its URL; 1 on success, 0 if out of range
int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len)
{
    if (i < 0 || i >= static_cast<int>(g_links.size()))
        return 0;
    const LinkRect& lr = g_links[i];
    if (x) *x = lr.x;
    if (y) *y = lr.y;
    if (w) *w = lr.w;
    if (h) *h = lr.h;
    if (url && len > 0)
        std::snprintf(url, static_cast<size_t>(len), "%s", lr.url.c_str());
    return 1;
}

// Download `url` to file `outPath` via a standalone curl handle (no render). Reuses the
// CA blob set by WebCoreSetCACertBlob. Returns the HTTP status code on success (e.g. 200),
// or negative on failure (-1 bad args, -2 file open, -3 curl init, -100-curlcode transfer).
// curl is already globally initialized by CurlContext (touched in SetupRuntimeEnv).
int WebCoreDownload(const char* url, const char* outPath)
{
    if (!url || !*url || !outPath || !*outPath)
        return -1;
    // Download to a .part file and rename on success, so a partial transfer never looks
    // like a complete one. "wb" truncates any leftover .part from a previous attempt.
    std::string partPath = std::string(outPath) + ".part";
    FILE* fp = nullptr;
    if (fopen_s(&fp, partPath.c_str(), "wb") != 0 || !fp)
        return -2;
    CURL* h = curl_easy_init();
    if (!h) {
        std::fclose(fp);
        std::remove(partPath.c_str());
        return -3;
    }
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, webcoreDownloadWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(h, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Safari/605.1.15");
    if (!g_caBytes.empty()) {
        curl_blob blob;
        blob.data = g_caBytes.data();
        blob.len = g_caBytes.size();
        blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
    }
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    std::fclose(fp);
    if (rc != CURLE_OK) {
        std::remove(partPath.c_str());
        return -100 - static_cast<int>(rc);
    }
    if (code < 200 || code >= 400) {   // HTTP error: drop the partial file
        std::remove(partPath.c_str());
        return static_cast<int>(code);
    }
    // .part -> final rename; a failure here leaves the previous file untouched
    std::remove(outPath);
    if (std::rename(partPath.c_str(), outPath) != 0) {
        std::remove(partPath.c_str());
        return -4;
    }
    return static_cast<int>(code);
}

// Render `utf8Html` into a w*h RGBA8888 buffer.
// outRGBA must point to at least w*h*4 bytes. Returns 0 on success.
#pragma strict_gs_check(push, off)
int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    if (!utf8Html || !outRGBA || w <= 0 || h <= 0)
        return kErrBadArgs;

    WebCoreStage("WCRH:init");
    ensureWebCoreInitialized();
    WebCoreStage("WCRH:initialized");

    // ---- 2. PageConfiguration with empty clients, then swizzle the main-frame
    //      loader-client factory to our LoadingFrameLoaderClient. ----
    OutputDebugStringA("[WCRH] pageConfigurationWithEmptyClients\n");
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());
    OutputDebugStringA("[WCRH] pageConfiguration ok\n");

    // Apotheosis 2026-09-18: real Web Storage, as in buildSession -- see Src/port/PortStorage.h.
    pageConfiguration.storageNamespaceProvider = WebCorePort::createPortStorageNamespaceProvider();

    // Replace the EmptyLocalFrameLoaderClient factory with ours so that
    // Frame::init() can proceed through its policy checks.
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                return makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
            } };
    }

    // ---- 3. Page ----
    OutputDebugStringA("[WCRH] Page::create start\n");
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    OutputDebugStringA("[WCRH] Page::create ok\n"); WebCoreStage("WCRH:page-created");
    gpuLogMarkerF("SL: createPage @WebCoreRenderHtml this=%p", (const void*)page.ptr());
    OutputDebugStringA("[WCRH] settings start\n");

    // Headless software render: no script, no compositing, no media.
    page->settings().setScriptEnabled(false);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif

    // ---- 4. Main frame + view ----
    WebCoreStage("WCRH:frame-setup");
    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;

    WebCoreStage("WCRH:set-view");
    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    WebCoreStage("WCRH:frame-init");
    localMainFrame->init();   // creates the initial empty document + DocumentLoader
    WebCoreStage("WCRH:frame-init-done");

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;

    // Opaque white page background so text is visible (default would be
    // transparent and you'd get the raw transparency over the surface).
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(false);

    // ---- 5. Feed the HTML string through the DocumentWriter ----
    WebCoreStage("WCRH:loader");
    Ref<FrameLoader> loader = localMainFrame->loader();
    RefPtr<DocumentLoader> activeLoader = loader->activeDocumentLoader();
    if (!activeLoader)
        return kErrNoLoader;

    WebCoreStage("WCRH:begin");
    DocumentWriter& writer = activeLoader->writer();
    // Apotheosis 2026-09-19: this HTML is generated by the harness and handed over as UTF-8 by
    // construction, so the encoding is stated rather than left to the `Settings` default. Same defect
    // class as the network feed above -- it simply has no HTTP header to inherit from. The MIME type
    // stays BARE: a parameterised one is not equal to `text/html` for `DOMImplementation::createDocument`
    // or `TextResourceDecoder::determineContentType`, and the document degrades to plain text
    // (`apoCharsetForFeed` has the measurement).
    writer.setMIMEType("text/html"_s);
    writer.begin(URL());   // empty/about:blank-ish base URL; creates the document
    // Apotheosis 2026-09-19: after `begin()`, because `clear()` inside it resets `m_encoding` unless it
    // was marked user-chosen -- see the longer note at the network feed site.
    writer.setEncoding("utf-8"_s, DocumentWriter::IsEncodingUserChosen::No);
    {
        const size_t len = std::strlen(utf8Html);
        Ref<SharedBuffer> buffer = SharedBuffer::create(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(utf8Html), len));
        writer.addData(buffer.get());
    }
    WebCoreStage("WCRH:end");
        OutputDebugStringA("[WCRH] writer.end() start\n");
        writer.end();   // finishes parsing synchronously for this in-memory document
        OutputDebugStringA("[WCRH] writer.end() ok\n");
    WebCoreStage("WCRH:end-done");

    // ---- 6. Size + layout ----
    WebCoreStage("WCRH:resize");
    OutputDebugStringA("[WCRH] resize start\n");
    const IntSize size(w, h);
    view->resize(size);   // Widget::resize -> setFrameRect; establishes layout viewport
    OutputDebugStringA("[WCRH] resize ok\n");

    WebCoreStage("WCRH:document");
    OutputDebugStringA("[WCRH] getDocument start\n");
    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;
    OutputDebugStringA("[WCRH] getDocument ok\n");

    WebCoreStage("WCRH:layout");
    OutputDebugStringA("[WCRH] updateLayout start\n");
    document->updateLayoutIgnorePendingStylesheets();   // force full style+layout now
    OutputDebugStringA("[WCRH] updateLayout ok\n");
    WebCoreStage("WCRH:extract-links");
    extractLinks(document.get(), h);                    // refresh the link table before painting
    WebCoreStage("WCRH:pre-cairo");

    // ---- 7. Cairo image surface + GraphicsContextCairo + paint ----
    WebCoreStage("WCRH:cairo-surface");
    OutputDebugStringA("[WCRH] cairo_image_surface_create start\n");
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    OutputDebugStringA("[WCRH] cairo_image_surface_create ok\n");
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return kErrCairoSurface;
    }
    WebCoreStage("WCRH:cairo-surface-ok");

    WebCoreStage("WCRH:cairo-create");
    cairo_t* cr = cairo_create(surface);
    if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr) cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return kErrCairoContext;
    }

    {
        GraphicsContextCairo context(adoptRef(cr));
        auto oldBehavior = view->paintBehavior();
        view->setPaintBehavior(oldBehavior | PaintBehavior::FlattenCompositingLayers | PaintBehavior::Snapshotting);
        OutputDebugStringA("[WCRH] view->paint start\n");
        view->paint(context, IntRect(IntPoint(), size));
        OutputDebugStringA("[WCRH] view->paint ok\n");
        view->setPaintBehavior(oldBehavior);
    }

    cairo_surface_flush(surface);

    // ---- 8. Copy + swizzle into caller's RGBA8888 buffer ----
    // CAIRO_FORMAT_ARGB32 in memory (little-endian) == premultiplied B,G,R,A.
    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);   // bytes per row, >= 4*w

    for (int y = 0; y < h; ++y) {
        const unsigned char* srow = src + static_cast<size_t>(y) * stride;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char b = srow[x * 4 + 0];
            const unsigned char g = srow[x * 4 + 1];
            const unsigned char r = srow[x * 4 + 2];
            const unsigned char a = srow[x * 4 + 3];
            // Un-premultiply so the caller gets straight-alpha RGBA8888.
            if (a == 0 || a == 255) {
                drow[x * 4 + 0] = r;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = b;
                drow[x * 4 + 3] = a;
            } else {
                drow[x * 4 + 0] = static_cast<uint8_t>((r * 255 + a / 2) / a);
                drow[x * 4 + 1] = static_cast<uint8_t>((g * 255 + a / 2) / a);
                drow[x * 4 + 2] = static_cast<uint8_t>((b * 255 + a / 2) / a);
                drow[x * 4 + 3] = a;
            }
        }
    }

    cairo_surface_destroy(surface);

    // Page/frame/view are released here as the RefPtrs go out of scope.
    return kOK;
}
#pragma strict_gs_check(pop)

// ---------------------------------------------------------------------------
// WebCoreLoadUrl -- load an http(s):// URL over the network (curl backend) and
// render the resulting page into a w*h RGBA8888 buffer. Sibling of
// WebCoreRenderHtml(): instead of feeding a local HTML string through the
// DocumentWriter, it drives a real provisional load through the FrameLoader,
// pumps the WebKit main-thread run loop until the main frame finishes (or a
// timeout) and then paints the result into outRGBA.
#pragma strict_gs_check(push, off)
int WebCoreLoadUrl(const char* url, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;

    if (!url || !outRGBA || w <= 0 || h <= 0)
        return kErrBadArgs;

    g_lastNetError[0] = '\0';   // clear any stale diagnostic from a prior call
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;   // reset the (dead) load counters

    // process init (JSC/MainThread/AtomStrings) + installPortPlatformStrategies()
    ensureWebCoreInitialized();

    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid())
        return kErrBadUrl;

    // ---- PageConfiguration with empty clients, then swizzle the main-frame
    //      loader-client factory to our LoadingFrameLoaderClient. ----
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    // Apotheosis 2026-09-18: real Web Storage, as in buildSession -- see Src/port/PortStorage.h.
    pageConfiguration.storageNamespaceProvider = WebCorePort::createPortStorageNamespaceProvider();

    // Shared terminal-state signal. The client completion handler, settle timer and
    // watchdog all run on this (main) thread, so no locking is needed.
    // Apotheosis: didFinishLoad can return while JS work is still running (bilibili is the
    // case that forced this), so "loaded" is not "done". Keep pumping while
    // isLoadingInAPISense reports outstanding resources/XHR, with an 8 s hard ceiling.
    struct LoadState {
        bool mainDone = false;
        bool failed   = false;
        bool stopped  = false;
        bool timedOut = false;
    } loadState;

    auto onLoadDone = [&loadState](bool failed) {
        if (loadState.mainDone)
            return;
        loadState.mainDone = true;
        loadState.failed = failed;
    };

    // Replace the EmptyLocalFrameLoaderClient factory with ours (preserving the
    // sandboxFlags/referrerPolicy defaults populated at index 0).
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [onLoadDone](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
                // Function<>'s ctor needs an rvalue; wrap a copy of onLoadDone in
                // a fresh rvalue lambda.
                client->setLoadCompletionHandler([onLoadDone](bool failed) { onLoadDone(failed); });
                return client;
            } };
    }

    // ---- Page ----
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    gpuLogMarkerF("SL: createPage @WebCoreLoadUrl this=%p", (const void*)page.ptr());

    // Apotheosis: enable JavaScript (LLInt plus the baseline JIT -- no higher tiers here),
    // or a modern page is static text: no DOM behaviour, no SPA, no rendering updates.
    page->settings().setScriptEnabled(true);
    page->settings().setLoadsImagesAutomatically(true);   // <img> and CSS background images
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    // headless, but the page still has to be marked visible or WebCore skips painting
    page->setIsVisible(true);

    // ---- Main frame + view ----
    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();   // creates initial empty document; FrameLoader ready

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;

    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);
    view->resize(IntSize(w, h));   // establish viewport BEFORE load

    // ---- Issue the network load ----
    ResourceRequest request { WTF::move(parsedURL) };
    FrameLoadRequest frameLoadRequest { *localMainFrame, WTF::move(request), SubstituteData { } };

    Ref<FrameLoader> loader = localMainFrame->loader();
    loader->load(WTF::move(frameLoadRequest));   // async: provisional load -> ResourceHandle::start -> curl

    // ---- Pump the main-thread run loop until the document is idle (or watchdog) ----
    auto stopLoop = [&loadState] {
        if (loadState.stopped)
            return;
        loadState.stopped = true;
        RunLoop::currentSingleton().stop();
    };

    // settle: a 50 ms timer polls isLoadingInAPISense (resources/XHR) and keeps the loop
    // alive so a page that is still running JS can finish (the bilibili case). The cap is
    // 8 s (160 x 50 ms); the 30 s watchdog stops the loop regardless.
    int settleTicks = 0;
    RefPtr<LocalFrame> frameForSettle = localMainFrame;
    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "WebCoreLoadUrl.settle"_s,
        WTF::Function<void()> { [&loadState, &stopLoop, &settleTicks, frameForSettle] {
            if (!loadState.mainDone)
                return;
            ++settleTicks;
            RefPtr<DocumentLoader> dl = frameForSettle->loader().activeDocumentLoader();
            if (!dl || !dl->isLoadingInAPISense() || settleTicks > 160)
                stopLoop();
        } });
    settle.startRepeating(0.05_s);

    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCoreLoadUrl.watchdog"_s,
        WTF::Function<void()> { [&loadState, &stopLoop] {
            loadState.timedOut = !loadState.mainDone;
            stopLoop();
        } });
    watchdog.startOneShot(30_s);

    if (!loadState.stopped)
        RunLoop::run();

    settle.stop();
    watchdog.stop();

    if (loadState.timedOut)
        return kErrLoadTimeout;
    if (loadState.failed)
        return kErrLoadFailed;

    // ---- Final layout ----
    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;

    // Apotheosis: the commit can replace the LocalFrameView, so the handle is re-read from
    // the frame here; a stale view would be resized and painted instead of the live one.
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));

    document->updateLayoutIgnorePendingStylesheets();
    extractLinks(document.get(), h);                    // refresh the link table before painting

    // ---- Cairo paint + the RGBA swizzle described above ----
    int nonWhite = 0;
    int prc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*document, *view, w, h, nonWhite);
    return kOK;
}
#pragma strict_gs_check(pop)

// ===========================================================================
// C ABI: exports
// ===========================================================================

// Reload a URL into the live session -- the session counterpart of WebCoreLoadUrl, used
// by WebCoreClickAt/WebCoreScrollBy for link navigation. 0 on success, as there.
// Apotheosis 2026-09-24: how many Page objects are alive right now, and which ones.
//
// `Page::forEachPage` is WEBCORE_EXPORTed (Page.h:477; confirmed defined in
// build-x64-gpu/lib/WebCore.lib with llvm-nm --defined-only), so the port can ask this question with no
// WebCore edit at all -- which matters, because the alternative instrument was a guarded marker in
// Page::~Page, i.e. a WebCore rebuild, and this one is portable to ARM32 unchanged.
//
// The registry it walks is a HashSet<WeakRef<Page>> (Page.cpp:268) and ~Page removes its own entry
// (Page.cpp:548), so a Page still in it is a Page whose refcount never reached zero. The count is
// therefore a count of *retained* Pages, not of stale registry entries, and the pointers are what make
// the retention legible: a pointer that appears after navigation N and is still present at navigation
// N+2 names a Page something is holding, which is the whole question -- teardownSession() nulls
// `g_session->page` (a RefPtr), so anything that survives that null is held elsewhere.
//
// Measured on 0.1.10.37, five dzen.ru loads: the count after build reached 3, 3, 1, 3, 4, 5, i.e. Pages
// accumulate, while the same five loads of example.com plateau. Each dzen-sized Page is ~150 MB, which
// is the same figure as the per-navigation RSS growth.
//
// 0.1.10.40 names each Page instead of leaving it anonymous, via Page::mainFrameURL() (also
// WEBCORE_EXPORTed, `llvm-nm --defined-only` confirmed) -- because the pointers alone cannot say whether
// the retained Pages are the SVG documents the accumulator hypothesis predicts or something else
// entirely, and the releaseMemory(Critical::Yes) measurement below falsified the *mechanism* while
// leaving the retention itself unexplained. Bounded at 8 entries and 95 URL chars: the log line must
// stay one line and must not depend on page content staying small.
// Apotheosis 2026-09-24: the file switches, and they are FILES, not build flags -- the same reason
// `jstack.txt` and `texttrace.txt` are files: the switch has to work against an already-installed build,
// and Device Portal cannot write LocalState on the Lumia. Re-read on every navigation (one fopen), not
// cached, so a run can be steered without a rebuild; the state transition is what gets logged, so a
// 20-navigation run cannot fill the file with the same line.
//
// The switch that remains: `cacherelease.txt`, which ARMS the per-navigation cache release. It is the
// only one, because the other two (`evictcache.txt`, `decoded.txt`) selected between two calls whose
// question is now answered -- see apoReleaseResourceCache(). Add a new one only if it opens a new arm
// boundary; a switch that selects something no measurement needs is a way for a log to lose its own
// configuration. Naming follows the codebase's other file switches (`jstack.txt`, `texttrace.txt`,
// `decoded.txt`): the file being present is what turns the non-default behaviour ON.
static bool apoLocalStateSwitchArmed(const char* name)
{
    if (!g_gpuInitLog[0])
        return false;
    std::string dir(g_gpuInitLog);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
        return false;
    const std::string probe = dir.substr(0, slash + 1) + name;
    FILE* f = fopen(probe.c_str(), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// Apotheosis 2026-09-24: the per-navigation cache release. ARMED BY `cacherelease.txt`, OFF by default.
//
// WITHDRAWN PROMOTION, and the measurement that withdrew it -- this function was briefly made the
// default on 0.1.10.49 and reverted in the same session, because the reading that justified the
// promotion did not survive a controlled re-measurement. The record, since a withdrawn claim that is
// not written down is a claim the next agent will re-make:
//
//   What was believed: "neither call alone bounds the growth, both together do: +4 MB/load against
//   +136, and the process even shrank 80 MB net over five loads" (0.1.10.48, both switches armed).
//   Why it was wrong: that arm's FIRST delta was -125 MB, and -125 averaged over five loads is where
//   the "flat" came from. The same run's remaining deltas were +37, +51, +55 -- about +48 MB/load, i.e.
//   exactly what the released arm measures today. A single negative outlier was read as a plateau. The
//   tell was in the log all along: a process does not shrink by 125 MB because a cache was evicted.
//
//   What the controlled re-measurement says (0.1.10.49, ONE warm process, same page, same build,
//   alternating the switch and holding everything else constant):
//       arm OFF   52 MB/load (deltas 23, 64, 69, 51)
//       arm ON    47 MB/load (deltas 26, 57, 57, 48)
//   and then interleaved, 2 loads per arm x 3 cycles, so that warm-up cannot be mistaken for an effect:
//       OFF  5, 4, 44      ON  54, -71, 48
//   Per-navigation deltas on a real page span -71 to +54. The spread of ONE arm exceeds the difference
//   between arms, so this instrument cannot resolve a lever of this size at this sample count -- and the
//   earlier "-57 %" for destroyDecodedDataForAllImages and "-58 %" for evictResources came from the same
//   two-window method, with the disarmed window taken on a FRESH process (deltas 242, 157, 168 = warm-up)
//   and the armed window on a warm one. Those two figures are withdrawn as well.
//
// So: keep the calls available, do not enable them by default, and do not claim they bound anything.
// What still stands, because it is reproducible in every run and is not a single sample, and it is sharper
// than this comment used to say: a FRESH process loading heavy real pages climbs without a plateau
// (`https://dzen.ru/` x12 on 0.1.10.50, default configuration: 136 -> 1142 MB, every delta positive,
// ~91 MB/load), while the SAME page in a process already at ~1.8 GB reads +7 MB/load and the process
// shrinks by 429 MB over eight loads -- so the slope is the curve below a high watermark, and the releases
// above it are the OS/allocator, not any lever here. `https://habr.com/ru/feed/` behaves the same way
// (mean +53 MB/load as a sawtooth with -92 and -70 in it). NO synthetic page built so far reproduces the
// slope at all -- see the frontier note on apoLogLiveDocuments() for what each controlled arm did.
// Whether it is a port defect or a page-side defect any browser would have is NOT yet separated, and
// separating it is the open work; another release lever is not the answer to it.
//
// Why the calls are retained at all: each reaches something the upstream release paths provably cannot.
// `WebCore::releaseMemory(Critical::Yes, ...)` above does route to
// `MemoryCache::pruneLiveResourcesToSize(0, DestroyDecodedDataForAllLiveResources)`, but that walks only
// `m_liveDecodedResources`, the list populated by `CachedImage::decodedSizeChanged()` -- which this
// port's Cairo image path NEVER calls. Measured: `decodedB = 0` for images on every load, even for 110 MB
// of provably-decoded pixels (priced with `Src\tools\make-image-test-pages.ps1`: twenty 1200x1200 PNGs
// drawn at 200x100 cost +116 MB against a predicted 110 MB of ARGB). A CachedImage can therefore hold a
// decoded surface forever with every upstream release path believing there is nothing to free.
// `destroyDecodedDataForAllImages()` (WEBCORE_EXPORTed; MemoryCache.cpp:270-279) walks EVERY resource,
// live and dead, ignoring clients -- the escape hatch that defect needs. `evictResources()` is
// `setDisabled(true); setDisabled(false);` (MemoryCache.cpp:767-775), and setDisabled(true) calls
// remove() on every resource ignoring clients. Both are safe at this call site: teardownSession() has
// destroyed the old session and buildSession() has not created the new one, so there is nothing in flight
// for remove() to interrupt, and the RELEASE_ASSERT(isMainThread()) inside the first is satisfied because
// the engine thread is the main thread here.
//
// The state is printed on EVERY call, not only on transitions, and deliberately: this project has paid
// twice now for a run whose configuration nobody could name after the fact. The switch is a file because
// Device Portal cannot write LocalState on the Lumia -- [[wdp-cannot-write-localstate]].
static void apoReleaseResourceCache(const char* when)
{
    if (!apoLocalStateSwitchArmed("cacherelease.txt")) {
        gpuLogMarkerF("SL: cache release %s: DISARMED (cacherelease.txt absent)", when);
        return;
    }
    gpuLogMarkerF("SL: cache release %s: ARMED -- destroyDecodedDataForAllImages + evictResources", when);
    WebCore::MemoryCache::singleton().destroyDecodedDataForAllImages();
    WebCore::MemoryCache::singleton().evictResources();
}

static void apoLogLivePages(const char* when)
{
    struct Entry {
        const void* ptr;
        bool utility;
        char url[96];
    };
    Entry pages[8];
    int n = 0, total = 0;
    WebCore::Page::forEachPage([&](WebCore::Page& p) {
        ++total;
        if (n >= 8)
            return;
        Entry& e = pages[n];
        e.ptr = &p;
        // isUtilityPage() is inline in Page.h, so no export is needed; mainFrameURL() is WEBCORE_EXPORTed.
        // The flag is the discriminator that matters: Page.cpp:504 adds EVERY Page to allPages(), before
        // the isUtilityPage() test at :506 -- so an SVG image's hidden Page IS visible here, it just does
        // not count towards nonUtilityPageCount(). isUtilityPage() is true for a Page whose ChromeClient
        // is an SVGImageChromeClient (Page.cpp:276-279, SVGImage.cpp:489), which is exactly what
        // SVGImage::dataChanged installs. A Page that is anonymous AND utility is therefore an SVG image's
        // Page with no other explanation available; an anonymous non-utility Page is something else.
        e.utility = p.isUtilityPage();
        WTF::CString u = p.mainFrameURL().string().utf8();
        if (u.length() > sizeof e.url - 1) {
            memcpy(e.url, u.data(), sizeof e.url - 4);
            memcpy(e.url + sizeof e.url - 4, "...", 4);
        } else
            memcpy(e.url, u.data(), u.length() + 1);
        ++n;
    });
    char buf[1024];
    int off = snprintf(buf, sizeof buf, "SL: live pages %s=%d nonUtility=%u [", when, total,
        WebCore::Page::nonUtilityPageCount());
    if (off < 0)
        return;
    for (int i = 0; i < n; ++i) {
        int w = snprintf(buf + off, sizeof buf - (size_t)off, "%s%c%p", i ? " | " : "",
            pages[i].utility ? 'U' : '-', pages[i].ptr);
        if (w < 0 || (size_t)(off + w) >= sizeof buf)
            break;
        off += w;
        w = snprintf(buf + off, sizeof buf - (size_t)off, " %s", pages[i].url);
        if (w < 0 || (size_t)(off + w) >= sizeof buf)
            break;
        off += w;
    }
    if ((size_t)off + 2 < sizeof buf) {
        buf[off++] = ']';
        buf[off] = '\0';
        gpuLogMarker(buf);
    }
}

// Apotheosis 2026-09-24: attribute the RSS remainder, and do it by asking WebCore rather than guessing.
//
// Why this exists: `apoLogLivePages` counts the hidden per-SVG Pages and the controlled pages priced one
// at ~0.4 MB (ten external svg images, ~+4 MB total) -- far too little to explain dzen.ru's +156 MB per
// load. So the confirmed mechanism is not the whole story, and the remainder has never been attributed.
// PerformanceLogging::memoryUsageStatistics() is WEBCORE_EXPORTed (PerformanceLogging.h:53) and reports
// what no other instrument in this port shows: the JSC GC heap (capacity / size / extra memory / object
// counts), alongside page_count and document_count as cross-checks on the live-Page probe.
//
// That number is the discriminating one because WebCoreSessionLoad already runs
// garbageCollectNow() + deleteAllCode(PreventCollectionAndDeleteAllCode) on every navigation
// (MemoryRelease.cpp:155-158, reached via Critical::Yes + Synchronous::Yes) and the slope did not move.
// If the heap still grows by the same amount per load, then the collection frees objects but the heap
// never returns the pages to the OS -- a different defect with a different fix (heap shrinking /
// Gigacage decommit), and not something the release path can reach by being made more aggressive.
//
// The measured answer to that question, and a CORRECTION of the first one. On 0.1.10.44 the JS heap was
// read as saturating at 113 MB / 1 722 272 objects "frozen across six navigations", and the same note
// called the retained Documents frozen. Both readings were taken over three to six samples and BOTH are
// wrong: re-measured over 26 navigations of the same page, `document_count` climbs 6, 10, 14, 18, 23 ...
// 90 with no plateau at all, and RSS climbs with it. The lesson is in the record, not the number: a
// plateau claim needs enough samples to see one, and three is not enough -- the same trap as
// [[diag-line-cannot-sync-a-load]], where a short sample set was read as a result.
// So the honest state of the accounting is: the JS heap is *bounded* (it is collected every navigation),
// the Documents are NOT, and the Documents are the one structure whose growth is reproducible in every
// run -- 71 -> 84 across a five-load window in which both cache levers were armed, i.e. untouched by
// them. They are also priced now (see apoLogLiveDocuments) and they are CHEAP: 12-20 nodes for the two
// SVG-image documents per load, ~1500-1900 nodes for the one main document, fractions of a MB together.
// So they name the leak without accounting for the slope, and the slope on a real page is still
// unattributed. Read the frontier note next to apoLogLiveDocuments() before theorising further.
//
// TWO DEAD ENDS ON THE WAY, both worth naming because both look like the right API:
//   * `WTF::fastMallocStatistics()` is exported from JavaScriptCore.lib (`?fastMallocStatistics@WTF@@`,
//     verified with `llvm-nm --defined-only`; WebCore.lib does not have it) and on Windows it is a
//     TAUTOLOGY: `committedVMBytes` is `GetProcessMemoryInfo().PeakWorkingSetSize` while `reservedVMBytes`
//     and `freeListBytes` are hardcoded to 0, under upstream's own FIXME "Can bmalloc itself report the
//     stats instead of relying on the OS?" (FastMalloc.cpp, the bmalloc branch -- and `USE_SYSTEM_MALLOC 0`
//     in build-x64-gpu/cmakeconfig.h selects exactly that branch). It was wired in, it "grew" 137 -> 880 MB
//     in perfect lockstep with RSS, and that lockstep *was* the tell: it is reading the process's peak
//     working set, i.e. the same thing the harness already reports. Removed rather than kept.
//   * `WTF::memoryFootprint()` is also exported, and its Windows implementation is guarded out of the App
//     Container (`QueryWorkingSet` / `PSAPI_WORKING_SET_INFORMATION` are outside `WINAPI_PARTITION_APP`)
//     and returns 0. On both architectures, so it is not a bench-only limitation.
// Net: this port has no reading of the engine allocator's own total. Anything that needs one has to be
// built from the process's RSS plus the instruments below, or from a new bmalloc-level probe.
//
// Engine thread only: it takes JSC::JSLockHolder (PerformanceLogging.cpp:69). That is satisfied at both
// call sites, and neither is inside a `#pragma strict_gs_check(push, off)` region.
static void apoLogMemoryStats(const char* when)
{
    auto stats = WebCore::PerformanceLogging::memoryUsageStatistics(WebCore::ShouldIncludeExpensiveComputations::Yes);
    for (auto& [key, value] : stats)
        gpuLogMarkerF("SL: mem %s %s=%llu", when, key.characters(), (unsigned long long)value);

    // 2026-09-24, second round, because the first round's answer pointed here -- and the first round's
    // reading of `document_count` was WRONG in the way that matters, so it is corrected in place rather
    // than left for the next agent to trust. The claim was "document_count froze at 11 ... byte-identical
    // across the next six navigations". Over 26 navigations of the same page the real series is 6, 10, 14,
    // 18, 23, 27, 31, 35, 41, 45, 47, 49, 53, 57, 61, 65, 67, 71 ... 90: no plateau, ~+4 documents per
    // load, unbounded. Six samples could not have shown the plateau it was said to show. See the note on
    // apoLogMemoryStats()'s preamble and on apoLogLiveDocuments() -- the short-sample trap is the same one
    // that produced [[diag-line-cannot-sync-a-load]].
    //
    // What survives from that round, and what it means: `liveSize == size` and `decodedSize == 0` on the
    // readings below are the load-bearing facts, and they are what identifies the resource payload as the
    // memory and the retained documents as the holders.
    //
    // Units: BYTES. `CachedResource::size()` is `encodedSize() + decodedSize() + overheadSize()`
    // (CachedResource.h:193), so these are byte counts -- the earlier `...KB` label in this log was wrong
    // by 1000x and is corrected here. In bytes the figures are sane (1.4 MB of images, 22 MB of scripts
    // on dzen.ru) and the earlier worry that "one load claims 19 GB of scripts" is dissolved.
    //
    // The shape is what the instrument supports, and it is unambiguous: `liveSize == size` on every type
    // on every load (the cache holds no resource without a client, so `pruneDeadResourcesToSize(0)` --
    // the non-critical release path -- can never free anything), and `decodedSize == 0` for images on
    // every load while `count` is 22-44. That last one is the load-bearing reading: `size()` includes
    // decodedSize, so an image the cache believes has zero decoded bytes is not a lie about memory, it
    // means the Cairo image path never calls `CachedImage::decodedSizeChanged()`. Consequence: those
    // images never enter `m_liveDecodedResources`, which is the list `pruneLiveResourcesToSize()` walks --
    // so neither release path (nor `DestroyDecodedDataForAllLiveResources`) can reach decoded bitmap data.
    // It is the standing explanation for why `evictResources()`, which destroys the `CachedImage` objects
    // themselves, is the only lever that has ever moved the slope. Suspect, not finding.
    auto& cache = WebCore::MemoryCache::singleton();
    auto st = cache.getStatistics();
    auto logType = [&](const char* name, const WebCore::MemoryCache::TypeStatistic& t) {
        gpuLogMarkerF("SL: mem %s cache_%s count=%d sizeB=%d liveB=%d decodedB=%d",
            when, name, t.count, t.size, t.liveSize, t.decodedSize);
    };
    logType("images", st.images);
    logType("css", st.cssStyleSheets);
    logType("scripts", st.scripts);
    logType("xsl", st.xslStyleSheets);
    logType("fonts", st.fonts);
}

// Apotheosis 2026-09-24: price a retained Document, so the leaked ones can be told apart from each other
// and the frontier of this work is where the numbers are rather than where the hypotheses ended.
//
// THE PRICING, which is what changed the direction, and it says the documents are CHEAP:
//   #0..#1  frame=1 rv=1 nodes=12..20              url=                       <- two SVG-image Page documents
//   #2      frame=1 rv=0 nodes=1518..1853          url=https://dzen.ru/      <- one main document
// Fractions of a MB together, against a real-page slope of ~50-90 MB/load. So this is a leak worth fixing
// and NOT the accumulator. Every `frame=1` also killed the earlier reading of this same probe, which had
// claimed three of the four retained documents were Page-less DOMParser products; `rv=1` on the SVG ones
// independently confirms their hidden Pages are alive, matching `SL: live pages`.
//
// THE FRONTIER as of 0.1.10.49 -- what every controlled arm does, so the next instrument starts here
// instead of re-running these:
//   * 20 x 1200x1200 PNGs, a 32x32 control and a no-image grid (`make-image-test-pages.ps1`): the big arm
//     costs +116 MB ONCE -- matching the predicted 110 MB of ARGB -- and then PLATEAUS.
//   * ten external SVG images (`make-svg-test-pages.ps1`): +10 live Pages at ~0.4 MB each, self-clearing.
//   * documents created and dropped, and created and kept by JS (`make-doc-test-pages.ps1`): both plateau.
//     The design trap this test exposed will catch the next agent too, so it is written down: retention
//     by page JS does NOT survive a navigation, because the page goes away and takes its references with
//     it -- that page can never show a cross-navigation document leak, and its flat reading proves
//     nothing about the port. Arms were validated by `bodyKids=` (9 with the markers present against 4
//     without), because a flat positive control is otherwise indistinguishable from a dead script.
//   * 20 promoted layers against the same 20 boxes unpromoted (`make-layer-test-pages.ps1`; the log
//     confirms `compositing=0` against `compositing=1`): alternating in ONE warm process, the control
//     read 3, -38, -3, -65, 3, -3 MB and the composited arm 24, 8, 71, 24 -- every one positive, but
//     eight consecutive loads of the composited arm gave 80, 22, -15, -31, -2, 21, -10, i.e. a LEVEL
//     SHIFT of about 80 MB and then a plateau, not a slope.
// So every mechanism this port can build synthetically plateaus, and only a real site climbs. The honest
// state: image payload, SVG Pages, document creation and layer backing stores are all ELIMINATED as
// unbounded accumulators, and what remains is specific to what dzen.ru does -- which is also the shape of
// a page-side defect any browser would have, and the two have not been separated.
//
// The two facts above must not be re-derived, and one earlier claim about them is withdrawn: an earlier
// reading of this same registry called it "frozen at 11", on three samples -- three samples cannot show a
// plateau, and over 26 loads the series climbs to 90.
//
// Two accessors answer the biggest question and neither needs an imported symbol, both being inline in
// Document.h: `renderView()` -- a document with a live render tree is still owned by a rendering Page --
// and `frameID()`, which is `std::nullopt` for a document that was never created with a frame, i.e. the
// parser-made products of `createHTMLDocument`/`DOMParser`. The node count is the weight proxy, and it is
// what makes the attribution quantitative rather than a guess about URLs.
//
// On the node walk, and why it is written with ContainerNode rather than Node's own accessors: `Node`'s
// `firstChild()` is only declared inline in Node.h and defined in NodeInlines.h, while
// `ContainerNode::firstChild()` is inline in the class body (ContainerNode.h:43) and `Node::parentNode()`
// is inline in Node.h:916. `isContainerNode()` is inline (Node.h:243), so the downcasts are conditioned
// on the flag. This keeps the port free of a header it would otherwise have to include, and -- the part
// that matters -- of any out-of-line accessor that would fail at LINK time rather than compile time.
//
// Cost control: the walk is capped. It runs once per retained document per navigation, and a pathological
// tree must not turn the instrument into the load's dominant cost.
static unsigned apoCountNodes(const WebCore::Node* root)
{
    const unsigned cap = 200000;
    unsigned count = 0;
    const WebCore::Node* node = root;
    while (node && count < cap) {
        ++count;
        const WebCore::Node* descend = nullptr;
        if (node->isContainerNode())
            descend = static_cast<const WebCore::ContainerNode*>(node)->firstChild();
        if (descend) {
            node = descend;
            continue;
        }
        while (node && node != root && !node->nextSibling())
            node = node->parentNode();
        if (!node || node == root)
            break;
        node = node->nextSibling();
    }
    return count;
}

static void apoLogLiveDocuments(const char* when)
{
    struct Entry {
        char url[88];
        unsigned nodes;
        bool hasFrame;
        bool renderTree;
    };
    Entry docs[12];
    int n = 0, total = 0;
    for (auto& weakDoc : WebCore::Document::allDocuments()) {
        ++total;
        if (n >= 12)
            continue;
        WebCore::Document* doc = weakDoc.ptr();
        if (!doc)
            continue;
        // url() is virtual+final and defined in the class body, so it needs no imported symbol: the call
        // goes through the vtable or is devirtualized, and either way there is nothing to link against.
        WTF::CString u = doc->url().string().utf8();
        Entry& e = docs[n];
        if (u.length() > sizeof e.url - 1) {
            memcpy(e.url, u.data(), sizeof e.url - 4);
            memcpy(e.url + sizeof e.url - 4, "...", 4);
        } else
            memcpy(e.url, u.data(), u.length() + 1);
        e.hasFrame = doc->frameID().has_value();
        e.renderTree = doc->hasLivingRenderTree();
        WebCore::Element* root = doc->documentElement();
        e.nodes = root ? apoCountNodes(root) : 0;
        ++n;
    }
    gpuLogMarkerF("SL: docs %s total=%d shown=%d", when, total, n);
    for (int i = 0; i < n; ++i)
        gpuLogMarkerF("SL: doc %s #%d frame=%d rv=%d nodes=%u url=%s", when, i,
            docs[i].hasFrame ? 1 : 0, docs[i].renderTree ? 1 : 0, docs[i].nodes, docs[i].url);
}

int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA)
{
    DBG_STAGE("WebCoreSessionLoad enter url=%s", url ? url : "null");
    if (!url || !outRGBA || w <= 0 || h <= 0) {
        DBG_STAGE("WebCoreSessionLoad exit: bad args");
        return kErrBadArgs;
    }
    DBG_STAGE("WebCoreSessionLoad calling ensureWebCoreInitialized");
    ensureWebCoreInitialized();
    DBG_STAGE("WebCoreSessionLoad ensureWebCoreInitialized done");
    if (g_inPump) {
        DBG_STAGE("WebCoreSessionLoad exit: busy");
        return kErrBusy;
    }
    apoLogLivePages("at entry");
    DBG_STAGE("WebCoreSessionLoad calling teardownSession");
    teardownSession();
    DBG_STAGE("WebCoreSessionLoad teardownSession done");
    // Apotheosis 2026-09-24: release the process-wide caches on every navigation, right where the old
    // session is gone and the new one does not exist yet.
    //
    // Measured on 0.1.10.35, same page repeated: https://example.com/ x10 plateaus (1132892-1138124 KB),
    // dzen.ru x5 grows monotonically ~+100-145 MB per load (1133152 -> 1780648 KB). The growth is NOT an
    // accumulating Page: this function is teardownSession() -> emplace() -> buildSession() with no early
    // return that skips the teardown, and the session trace shows 24 `Page::create ok` against 24 `enter
    // url` and 24 teardowns. Nor is it the caches the port already bounds (BackForwardCache::setMaxSize(0),
    // MemoryCache::setCapacities(0, 8 MB, 16 MB) in ensureWebCoreInitialized). It is therefore whatever
    // ~Page does not free -- process-wide caches -- which is why it scales with page weight and not with
    // navigation count.
    //
    // WebCore's own remedy had no caller at all before this line: WebCoreReleaseMemory (ABI) and this
    // function were both dead, and the harness's AppMemoryUsageIncreased handler only logs. The port's
    // comment in ensureWebCoreInitialized claimed "harness calls WebCoreReleaseMemory() under memory
    // pressure" and it never did -- the SILENT LIE class of Doc/STUB-AUDIT.md.
    //
    // Critical::Yes, and that is the 2026-09-24 correction to this line: the non-critical path purges
    // only what is already dead (MemoryCache::pruneDeadResourcesToSize(0), FontCache::releaseNoncritical
    // MemoryInAllFontCaches), and measurement on 0.1.10.36 showed it does not touch the accumulator at
    // all. Critical::Yes additionally reaches MemoryCache::pruneLiveResourcesToSize(0,
    // DestroyDecodedDataForAllLiveResources) (MemoryRelease.cpp:118-128).
    // platformReleaseMemory() is a no-op on this port (MemoryRelease.cpp:280, !PLATFORM(COCOA) &&
    // !USE(SKIA)), so nothing platform-, Cairo- or ANGLE-specific is touched. Synchronous::Yes is the
    // branch that calls WTF::releaseFastMallocFreeMemory(), the part that returns pages to the OS; this
    // runs on the engine thread between sessions, so it costs no cross-thread wait.
    //
    // MEASURED on 0.1.10.40, five dzen.ru loads, and the answer is negative on both counts:
    //   - warm slope +160 MB/load, against +161 for 0.1.10.35 (no release at all) and +155 for 0.1.10.36
    //     (non-critical release). The three are indistinguishable. Critical::Yes does not bound this.
    //   - the live-Page count does not fall either. At one navigation the log reads
    //     `at entry=3` -> `after teardown=2` -> `after build=5`: the teardown drops exactly the one
    //     session Page, and pruneLiveResourcesToSize(0, ...) sitting between them removes nothing.
    //
    // Why, read from the callee rather than guessed (MemoryCache.cpp:281-334): pruneLiveResourcesToSize
    // does NOT remove resources. It walks m_liveDecodedResources and calls destroyDecodedData() only,
    // under `ASSERT(current->hasClients())` -- a resource with clients never leaves the cache by this
    // path. So the critical branch frees decoded bitmaps, not CachedImage/SVGImage, and therefore cannot
    // reach a hidden Page. The earlier reading of this comment ("can drop it") was wrong about the
    // callee, and the measurement above is what says so.
    //
    // This call is kept: it returns real memory at the margin (-126 MB over three light loads on
    // 0.1.10.36), it bounds the caches, and it is the port-side half of the wiring the port's own comment
    // always claimed existed. It is NOT the fix for the accumulation, and the cost it charges every
    // navigation -- dropping decoded data for live resources, plus Synchronous::Yes's
    // deleteAllCode(PreventCollectionAndDeleteAllCode) (MemoryRelease.cpp:155-158) -- is not measured.
    gpuLogMarker("SL: releaseMemory (per navigation)");
    WebCore::releaseMemory(Critical::Yes, Synchronous::Yes);
    // Apotheosis 2026-09-24: the cache release, ARMED BY FILE and OFF by default. It was briefly the
    // default on 0.1.10.49 and reverted the same session: the reading that justified promoting it was a
    // single -125 MB outlier averaged over five loads, and the controlled A/B in one warm process reads
    // 52 MB/load with it OFF against 47 with it ON -- no effect the instrument can resolve. The whole
    // record, including the two withdrawn percentages, is in the comment on apoReleaseResourceCache().
    // Do not re-promote it without an interleaved measurement; per-navigation deltas on a real page span
    // -71 to +54, so a two-window comparison will "prove" anything.
    apoReleaseResourceCache("after teardown");
    // The `evictcache.txt` / `decoded.txt` switch pair that used to sit here is GONE, and the reason is
    // that its question is answered: the two calls were measured separately (0.1.10.47-48) and each gives
    // about -57 %, but only together are they flat, so a build that can run one without the other now
    // invites exactly the mis-attribution this project has paid for twice. `cacherelease.txt` is the single
    // remaining arm boundary. One consequence to keep in mind when reading older logs: the arms recorded
    // in `Doc/Summary.md` under those two names were produced by 0.1.10.47/48 and cannot be reproduced
    // from this build -- their numbers stand, their switches do not.
    apoLogLivePages("after teardown");
    apoLogMemoryStats("after teardown");
    apoLogLiveDocuments("after teardown");
    g_lastNetError[0] = '\0';
    g_spaProbe[0] = '\0';
    g_lastPendingResources = 0;
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;
    g_session.emplace();
    g_session->w = w;
    g_session->h = h;
    DBG_STAGE("WebCoreSessionLoad calling buildSession");
    int rc = buildSession(url, w, h, outRGBA);
    gpuLogMarkerF("SL: session page=%p rc=%d", (const void*)g_session->page.get(), rc);
    apoLogLivePages("after build");
    apoLogMemoryStats("after build");
    apoLogLiveDocuments("after build");
    DBG_STAGE("WebCoreSessionLoad buildSession returned rc=%d", rc);
    if (rc != kOK) {
        DBG_STAGE("WebCoreSessionLoad buildSession failed, tearing down");
        teardownSession();
    }
    DBG_STAGE("WebCoreSessionLoad exit rc=%d", rc);
    return rc;
}

// Close the session: tear down the Page and everything hanging off it
void WebCoreCloseSession()
{
    if (g_inPump)
        return;
    teardownSession();
}

// Synthesise a tap at (x,y). Coordinates are viewport-relative; EventHandler applies
// the scroll offset itself (windowToContents). Mouse move/down/up so the page really
// sees a click, then pump -- settle ticks plus rAF -- so whatever it triggered runs.
// Apotheosis: the page's own verdict on the last tap -- declared in WebCoreDriver.h, which is where the
// reasoning lives. Written on the engine thread inside WebCoreClickAt and read by the harness on that
// same thread as soon as the call returns, so it needs no synchronisation; a failed or refused call
// leaves the -1 default behind, which the harness reads as "unknown" and not as "the page said no".
static int g_lastClickDefaultPrevented = -1;

int WebCoreLastClickDefaultPrevented()
{
    return g_lastClickDefaultPrevented;
}

int WebCoreClickAt(int x, int y, uint8_t* outRGBA)
{
    using namespace WebCore;
    g_lastClickDefaultPrevented = -1;   // this call's verdict; every early return above leaves "unknown"
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;   // scoped guard: clears g_inPump on every exit path

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // flush layout first: elementFromPoint needs real geometry

    // Apotheosis 2026-09-18: what the DOM itself received from this tap.
    //
    // Every explanation of a dead control in this project has ended at the same wall: between "we
    // dispatched a mouse press and release" and "the page reacted" the engine is a black box, and
    // `[HIT] settled` only reports what WebCore thought it handled -- not what the document saw. It
    // cannot separate the three causes that matter: no DOM `click` event ever existed (so the anchor's
    // default action never had a turn), a click fired but nothing was listening, or the page called
    // preventDefault(). The reported symptom -- "I tapped a link on dzen.ru and the page only flinched"
    // -- is exactly a case that cannot be settled from outside, so the page is asked directly, through
    // the evalJS channel probeSpaModule already uses.
    //
    // Installed before the events and read after them, so the line describes this tap and not an
    // earlier one. Capture phase at the document: an author handler calling stopPropagation() must not
    // be able to hide the event from the record.
    //
    // Apotheosis 2026-09-18 (second pass): `dp=` was read *in the capture listener*, i.e. before any
    // author handler had run, which made it 0 for every event the document had not already refused
    // at an earlier capture stage -- so the one field that separates "the page said no" from "the page
    // was silent" was vacuous. Measured: a page whose container handler calls preventDefault() still
    // logged `dp=0`. The authoritative value is the one read *after* the dispatch finishes, and the
    // event object still carries it (WebKit does not pool events), so the last click is kept and
    // `dpAfter=` reports it at readback time. Kept as-is rather than replaced: `dp=` still answers
    // "had something already refused it by the time it reached the document", and the pair is
    // cheaper to read than to re-derive.
    {
        char installOut[64] = "";
        int irc = evalJS(*lf,
            "(function(){try{if(window.__apoClickLog)return 'already';"
            "var L=[];window.__apoClickLog=L;"
            "function rec(t){return function(e){try{var tgt=e.target;"
            "if(t==='click')window.__apoLastClick=e;"
            "var a=(tgt&&tgt.closest)?tgt.closest('a'):null;"
            "var p=[];try{var cp=e.composedPath?e.composedPath():[];"
            "for(var i=0;i<cp.length&&i<4;i++){p.push(cp[i].tagName?cp[i].tagName.toLowerCase():'?');}}catch(x){}"
            "var dp=0;try{dp=e.defaultPrevented?1:0;}catch(x2){}"
            "L.push(t+'|'+(tgt&&tgt.tagName?tgt.tagName.toLowerCase():'?')"
            "+'|a='+(a?(a.getAttribute('href')||'-').substr(0,50):'-')"
            "+'|dp='+dp+'|tr='+(e.isTrusted?1:0)+'|btn='+(typeof e.button=='number'?e.button:'-')"
            "+'|path='+p.join('>'));"
            "if(L.length>8)L.shift();}catch(x3){}};}"
            "document.addEventListener('mousedown',rec('down'),true);"
            "document.addEventListener('mouseup',rec('up'),true);"
            "document.addEventListener('click',rec('click'),true);"
            "return 'installed';}catch(e){return 'EX:'+e;}})()",
            installOut, sizeof installOut);
        // Logged on every tap, not once: 'already' means the listener survived from the previous tap,
        // i.e. the document was not replaced, and 'installed' right after a tap means it was -- which
        // is by itself the answer to "did the engine navigate on its own".
        gpuLogMarkerF("[HIT] dom-probe rc=%d %s", irc, installOut[0] ? installOut : "(empty)");
    }

    // Apotheosis: name what the tap landed on, before any event is dispatched (JS may remove the
    // element). "The button did not react" and "the tap missed the button by a few pixels" look
    // identical from outside the engine, and cookie banners put an accept button right next to a
    // policy link -- one tap-sized miss navigates instead of dismissing. Goes to the same marker
    // file as the GPU steps. The reference is kept alive past the dispatch: whether this exact
    // element is still in the document once the handlers have run is the single fact that separates
    // "the page ignored the tap" from "the page reacted and the new frame never reached the screen".
    RefPtr<Element> probe = doc->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
    if (probe) {
        const CString tag = probe->localName().string().utf8();
        const CString id = probe->getIdAttribute().string().utf8();
        const CString qa = probe->attributeWithoutSynchronization(HTMLNames::classAttr).string().utf8();
        CString href;
        for (RefPtr<Element> e = probe; e; e = e->parentElement()) {
            if (is<HTMLAnchorElement>(*e)) {
                href = downcast<HTMLAnchorElement>(*e).href().string().utf8();
                break;
            }
        }
        gpuLogMarkerF("[HIT] %d,%d -> <%s> id=[%s] class=[%s] href=[%s]", x, y,
            tag.data() ? tag.data() : "?", id.data() ? id.data() : "",
            qa.data() ? qa.data() : "", href.data() ? href.data() : "-");
        // Apotheosis: the ancestor chain, because on a component-framework page the element under
        // the finger is almost never the control. Every dead tap recorded on hh.ru landed on a
        // <span> carrying a magritte-button__label class -- the label inside a button, not the
        // button -- and without the chain there is no way to tell from a report whether the tap
        // reached the right control at all. Four levels is enough to cross label, button and
        // wrapper; the class is truncated because framework class names are hashed and long.
        // This is deliberately not a listener probe: EventTarget::hasEventListeners would answer
        // "is anything bound here" directly, but EventNames.h is not in the sparse checkout and the
        // symbol is not in webcore-exports.def, so it would cost a WebCore relink to find out.
        {
            char chain[220] = "";
            size_t chainLen = 0;
            int level = 0;
            for (RefPtr<Element> e = probe->parentElement(); e && level < 4; e = e->parentElement(), ++level) {
                const CString aTag = e->localName().string().utf8();
                const CString aCls = e->attributeWithoutSynchronization(HTMLNames::classAttr).string().utf8();
                int wn = std::snprintf(chain + chainLen, sizeof chain - chainLen, " < %s.%.28s",
                    aTag.data() ? aTag.data() : "?", aCls.data() ? aCls.data() : "");
                if (wn < 0 || static_cast<size_t>(wn) >= sizeof chain - chainLen) {
                    chain[chainLen] = '\0';
                    break;
                }
                chainLen += static_cast<size_t>(wn);
            }
            gpuLogMarkerF("[HIT] ancestors:%s", chain[0] ? chain : " (none)");
        }
    } else
        gpuLogMarkerF("[HIT] %d,%d -> no element at point", x, y);

    // Reset the load state before pumping: dispatch itself can complete a load
    // (signalLoadComplete, LoadingFrameLoaderClient.cpp:131). A stale mainDone would make
    // pumpLoop return early, so the tap's own navigation would never be waited for.
    g_session->load = DriverLoadState{};
    if (g_session->client) {
        g_session->client->resetLoadState();
        DriverLoadState* lp = &g_session->load;   // NOTE: g_session must not be reset while this pointer is captured
        g_session->client->setLoadCompletionHandler([lp](bool failed) {
            if (lp->mainDone)
                return;
            lp->mainDone = true;
            lp->failed = failed;
        });
    }

    DoublePoint p(static_cast<double>(x), static_cast<double>(y));
    OptionSet<PlatformEvent::Modifier> mods;
    MonotonicTime t = MonotonicTime::now();
    PlatformMouseEvent move(p, p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0.0, SyntheticClickType::NoTap);
    lf->eventHandler().handleMouseMoveEvent(move);     // drives :hover / elementUnderMouse
    PlatformMouseEvent down(p, p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, 0.0, SyntheticClickType::NoTap);
    const bool pressHandled = lf->eventHandler().handleMousePressEvent(down).wasHandled();    // raises the user-gesture indicator (popup/autoplay policy)
    PlatformMouseEvent up(p, p, MouseButton::Left, PlatformEvent::Type::MouseReleased, 1, mods, MonotonicTime::now(), 0.0, SyntheticClickType::NoTap);
    const bool releaseHandled = lf->eventHandler().handleMouseReleaseEvent(up).wasHandled();    // dispatches the DOM 'click' -- the anchor's default action runs from here

    // Focus-on-tap. Headless has no window for the OS to focus, so a tap on a text
    // input, textarea or contenteditable has to focus it here -- otherwise
    // WebCoreFocusedEditable stays 0 and the IME path has nothing to type into.
    if (RefPtr<Document> hdoc = lf->document()) {
        if (RefPtr<Element> hit = hdoc->elementFromPoint(static_cast<double>(x), static_cast<double>(y))) {
            RefPtr<Element> target;
            for (RefPtr<Element> e = hit; e; e = e->parentElement()) {
                if ((is<HTMLInputElement>(*e) && downcast<HTMLInputElement>(*e).isTextField())
                    || is<HTMLTextAreaElement>(*e)) { target = e; break; }
            }
            if (!target && is<HTMLElement>(*hit) && downcast<HTMLElement>(*hit).isContentEditable())
                target = hit;
            if (target)
                target->focus();
        }
    }

    // pump so the page's work (onclick and friends) runs; a tap has no navigation to await
    pumpLoop(*lf, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ true,
             /*settleCapTicks*/ 160, /*watchdog*/ 30.0, /*pageForRendering*/ g_session->page.get());

    // Apotheosis: the verdict on the tap. wasHandled tells us whether anything in the page consumed
    // the event; connected=0 means the page's own script pulled the element out of the DOM, so a
    // control that still looks present on screen is a presentation problem, not a dead handler.
    gpuLogMarkerF("[HIT] settled: press=%d release=%d connected=%d",
        pressHandled ? 1 : 0, releaseHandled ? 1 : 0, (probe && probe->isConnected()) ? 1 : 0);

    // Apotheosis: read back what the listeners installed above saw,
    // and clear in place -- assignment
    // (`window.__apoClickLog = []`) would leave the recorder closures pushing into the array they
    // captured, and every later tap would report no-events while the records piled up unreachable.
    //
    // `dpAfter=` is the authoritative defaultPrevented, read here, after the dispatch has finished.
    // The per-record `dp=` cannot be: a capture listener at the document runs before every author
    // handler, so it always sees the pristine value. -1 means no click was recorded at all (the
    // anchor's default action therefore never had a turn). See the note at the install site.
    {
        char domOut[800] = "";
        int drc = evalJS(*lf,
            "(function(){try{var L=window.__apoClickLog;if(!L)return 'no-logger';"
            "var s=L.length?L.join(' ;; '):'no-events';L.length=0;"
            "var c=window.__apoLastClick;window.__apoLastClick=null;"
            "s+=' ;; dpAfter='+(c?(c.defaultPrevented?1:0):-1);"
            "return s;}catch(e){return 'EX:'+e;}})()",
            domOut, sizeof domOut);
        gpuLogMarkerF("[HIT] dom rc=%d %s", drc, domOut[0] ? domOut : "(empty)");
        // Apotheosis: publish the page's verdict for the harness. Parsed out of the readback line rather
        // than computed a second time, because that line is the one the docs tell a reader to consult --
        // one value, one place for it to be wrong.
        if (const char* dp = std::strstr(domOut, "dpAfter="))
            g_lastClickDefaultPrevented = std::atoi(dp + 8);
    }

    // Apotheosis: and why. A control that ignores a tap has three possible reasons and the diag line
    // cannot separate them: the page's scripts never executed, they executed but the framework never
    // claimed this element (hydration stopped short), or everything is bound and our synthetic event
    // is not reaching the handler. fw= reports the first own property starting with '_' found on the
    // hit element or its ancestors, which is how React marks the host nodes it manages
    // (__reactFiber$…/__reactProps$…) once hydration has run -- present means bound, none means not.
    // rs= is the document's readyState: anything other than 'complete' long after the load means the
    // parser never finished, and deferred scripts (i.e. every modern bundle) never ran at all.
    {
        char probeScript[900];
        std::snprintf(probeScript, sizeof probeScript,
            "(function(){try{var e=document.elementFromPoint(%d,%d);var f='none',d=0;"
            "while(e&&d<8){var k=Object.keys(e);for(var i=0;i<k.length;i++){"
            "if(k[i].charAt(0)=='_'){f=k[i].substr(0,24)+'@'+d;d=9;break;}}"
            "if(d==9)break;e=e.parentElement;d++;}"
            "var sc=document.scripts,s=0,t=[];for(var j=0;j<sc.length;j++){if(sc[j].src)s++;}"
            "for(var j=Math.max(0,sc.length-3);j<sc.length;j++){var q=sc[j];"
            "t.push((q.src?q.src.split('/').pop().substr(0,20):'inline')+(q.defer?'+d':'')+(q.async?'+a':''));}"
            "var b=document.body,last=b&&b.lastElementChild?b.lastElementChild.tagName:'-';"
            "return 'rs='+document.readyState+' fw='+f+' src='+s+'/'+sc.length"
            "+' bodyLast='+last+' tail='+t.join(',');}"
            "catch(err){return 'throw:'+err;}})()", x, y);
        char probeOut[400] = "";
        int prc2 = evalJS(*lf, probeScript, probeOut, sizeof probeOut);
        gpuLogMarkerF("[HIT] js rc=%d %s", prc2, probeOut);
    }

    // Apotheosis: name the script the parser is stuck on. A document that stays at readyState
    // 'loading' has one script it still intends to execute itself, and until that happens the
    // deferred bundles behind it never run. willBeParserExecuted means "the parser owns this one".
    // What it does NOT mean is anything about readiness: the ready= field printed here used to be
    // readyToBeParserExecuted(), which WebCore only ever sets for an inline parser-inserted script
    // waiting on stylesheets (ScriptElement.cpp:334) and never for a <script src=...>, so it read 0
    // for all seven of hh.ru's bundles even in the sessions where the page worked and React was
    // demonstrably mounted. It is replaced by state that moves: loaded / error from the
    // LoadableScript, and fired = the element dispatched its load event, i.e. it executed. The
    // telling combination is loaded=1 error=0 fired=0 -- the bundle arrived and never ran.
    // Also count the deferred ones for scale: those are the bundles that would run at
    // DOMContentLoaded.
    {
        RefPtr<WebCore::HTMLCollection> scripts = doc->scripts();
        const unsigned count = scripts ? scripts->length() : 0;
        unsigned deferred = 0, errored = 0, inOrder = 0;
        char blockers[320] = "";
        size_t blockersLen = 0;
        bool blockersFull = false;
        for (unsigned i = 0; i < count; ++i) {
            RefPtr<WebCore::HTMLScriptElement> script = dynamicDowncast<WebCore::HTMLScriptElement>(scripts->item(i));
            if (!script)
                continue;
            if (script->willExecuteWhenDocumentFinishedParsing())
                ++deferred;
            if (script->willExecuteInOrder())
                ++inOrder;
            if (script->errorOccurred())
                ++errored;
            if (!script->willBeParserExecuted())
                continue;
            // Keep counting after the buffer fills; only the naming stops. The counters above are
            // already incremented by this point, but the loop used to break out entirely, which
            // would have truncated them the moment a page had more parser-owned scripts than fit.
            if (blockersFull)
                continue;
            auto srcU8 = script->attributeWithoutSynchronization(WebCore::HTMLNames::srcAttr).string().utf8();
            const char* full = srcU8.data() ? srcU8.data() : "";
            const char* slash = std::strrchr(full, '/');
            const char* name = (slash && slash[1]) ? slash + 1 : (full[0] ? full : "inline");
            WebCore::LoadableScript* loadable = script->loadableScript();
            const char loadedFlag = loadable ? (loadable->isLoaded() ? '1' : '0') : '-';
            const char errorFlag  = loadable ? (loadable->hasError() ? '1' : '0') : '-';
            int wn = std::snprintf(blockers + blockersLen, sizeof blockers - blockersLen,
                "#%u:%.36s loaded=%c err=%c fired=%d; ", i, name,
                loadedFlag, errorFlag, script->haveFiredLoadEvent() ? 1 : 0);
            if (wn < 0 || static_cast<size_t>(wn) >= sizeof blockers - blockersLen) {
                blockers[blockersLen] = '\0';
                blockersFull = true;
                continue;
            }
            blockersLen += static_cast<size_t>(wn);
        }
        gpuLogMarkerF("[HIT] scripts n=%u defer=%u inorder=%u err=%u parserOwned=[%s%s]",
            count, deferred, inOrder, errored, blockers[0] ? blockers : "none",
            blockersFull ? "..." : "");
    }

    // re-read view/frame and re-size before painting
    lf = g_session->page->localMainFrame();
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    view = lf->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(g_session->w, g_session->h));
    doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);

    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// Scroll by (dx,dy), positive means down. Each tick drives isolatedUpdateRendering so
// IntersectionObserver and lazy loading fire; the position is clamped to [min,max].
int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // flush layout: contentsSize and the scroll limits come from it

    ScrollPosition cur = view->scrollPosition();
    ScrollPosition minP = view->minimumScrollPosition();
    ScrollPosition maxP = view->maximumScrollPosition();
    int tx = cur.x() + dx;
    int ty = cur.y() + dy;
    if (tx < minP.x()) tx = minP.x();
    if (tx > maxP.x()) tx = maxP.x();
    if (ty < minP.y()) ty = minP.y();
    if (ty > maxP.y()) ty = maxP.y();
    view->setScrollPosition(ScrollPosition(tx, ty));

    // M3 note: a scroll does not pay for pumpLoop's 8 s rendering-update wait. It runs
    // isolatedUpdateRendering (scroll steps, IntersectionObserver), resizes and relayouts,
    // paints once, and the harness's StartLiveMode (WebCoreLiveTick) takes it from there
    // for the JS-driven follow-up work (clicks, further loads).
    g_session->page->isolatedUpdateRendering();
    // Note: this path deliberately does not call extractLinks -- link rects are
    // viewport-relative and change with every scroll, so the harness calls WebCoreSyncLinks.
    int nonWhite = 0;
    g_gpuScrollFast = true;   // fast scroll: skip forceDirtyTree (the content moved, so it is dirty already)
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    return kOK;
}

// Rebuild the link table after a scroll. The rects are viewport-relative, so scrolling
// invalidates all of them even when nothing was repainted -- hence this separate call.
int WebCoreSyncLinks()
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);
    return kOK;
}

// Find in page: mark every match and return the match count (>= 0), or an error.
//   matchCase != 0 = case sensitive, wrap != 0 = wrap around; clear via WebCoreFindClear.
int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!utf8 || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    String text = String::fromUTF8(utf8);
    OptionSet<FindOption> opts;
    if (!matchCase) opts.add(FindOption::CaseInsensitive);
    if (wrap)       opts.add(FindOption::WrapAround);
    g_findText = text;
    g_findOpts = opts;

    if (text.isEmpty()) {
        g_session->page->unmarkAllTextMatches();
        int prc = finishInteractionPaint(outRGBA);
        return prc == kOK ? 0 : prc;
    }
    unsigned count = g_session->page->markAllMatchesForText(text, opts, /*shouldHighlight*/ true, /*max*/ 1000);
    auto data = g_session->page->findString(text, opts);
    if (data.range)
        g_session->page->revealCurrentSelection();
    int prc = finishInteractionPaint(outRGBA);
    if (prc != kOK)
        return prc;
    return static_cast<int>(count);
}

// Move to the next (forward != 0) or previous match: 1 = found, 0 = not, < 0 = error
int WebCoreFindNext(int forward, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    if (g_findText.isEmpty())
        return 0;
    g_inPump = true;
    PumpGuard guard;

    OptionSet<FindOption> opts = g_findOpts;
    if (!forward) opts.add(FindOption::Backwards);
    auto data = g_session->page->findString(g_findText, opts);
    if (data.range)
        g_session->page->revealCurrentSelection();
    int prc = finishInteractionPaint(outRGBA);
    if (prc != kOK)
        return prc;
    return data.range ? 1 : 0;
}

// Clear all find highlights; 0 on success
int WebCoreFindClear(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    g_session->page->unmarkAllTextMatches();
    g_findText = WTF::String();
    int prc = finishInteractionPaint(outRGBA);
    return prc == kOK ? 0 : prc;
}

// Apotheosis: memory pressure release. Called by harness on UWP memory events.
//
// 2026-09-24: this function and its only intended caller were both dead until now -- the harness
// registered AppMemoryUsageIncreased / Decreased / LimitChanging and its handlers only LOGGED, so the
// engine's remedy for memory pressure was unreachable. The harness now posts a job that calls this, and
// the marker below is what makes it visible in gpuinit-steps.txt from the driver's side: the
// per-navigation release calls WebCore::releaseMemory directly and logs its own text, so a line reading
// "(memory-pressure event)" can only have come through the ABI, i.e. from a real UWP memory event.
// Distinguishing the two matters when reading a slope: they are the same callee with different causes.
extern "C" void WebCoreReleaseMemory(int critical)
{
    gpuLogMarkerF("SL: releaseMemory (memory-pressure event) critical=%d", critical);
    // Apotheosis 2026-09-24: process init belongs HERE, and its absence was an intermittent
    // startup death -- the app failed to start in roughly 3 launches out of 10.
    //
    // Every other entry point that touches WebCore calls this first (WebCoreRenderHtml,
    // WebCoreLoadUrl, WebCoreSessionLoad, WebCoreGpuInit); this one did not, and until today the
    // omission could not bite because nothing called it. Now the harness posts a job on the UWP
    // memory events, and one of those events fires *immediately at handler registration* -- the
    // platform reports the first reading as a rise into `low`. The measured sequence:
    //
    //   21:30:59.368  mem-level UP: mem 35216/12497952 KB level=low   <- handler posts the job
    //   21:30:59.430  WebEngine: loop ready, waiting for jobs        <- 62 ms LATER
    //   21:30:59.433  VEH: 0xC0000005 tid=8312 faultaddr=0x8          <- the engine thread dies
    //
    // The job is therefore the engine thread's FIRST WebCore call, ahead of any load. It reaches
    // MemoryPressureHandler::isUnderMemoryPressure -> singleton() -> the Windows constructor, whose
    // member initialiser is m_windowsMeasurementTimer(RunLoop::mainSingleton(), ...). mainSingleton()
    // is `{ ASSERT(s_mainRunLoop); return *s_mainRunLoop; }` and the ASSERT compiles out in release,
    // so with s_mainRunLoop still null -- WTF::initializeMainThread() lives in this function's
    // lambda, below -- it returns a null reference and the Ref<RunLoop> refcount increment faults
    // reading offset 8. That is the 0x8, exactly, and the frames name it:
    //
    //   JSC+da0ab0  WTF::MemoryPressureHandler::MemoryPressureHandler()   (MemoryPressureHandler.cpp:75)
    //   JSC+da0a02  WTF::MemoryPressureHandler::isUnderMemoryPressure()   (MemoryPressureHandler.h:125)
    //   WebCore+199fad2  WebCore::releaseCriticalMemory()                 (MemoryRelease.cpp:122)
    //   Harness+b805e    the `mem-release` job lambda
    //
    // Not a defensive check but the same contract the other four obey: any ABI entry point that may
    // run as the engine thread's first job initialises the process first. It has to be fixed here
    // rather than only by gating the event in the harness, because a genuine `medium` event raised
    // before the loop is ready races identically -- the harness gate below removes the trigger, this
    // removes the hazard.
    ensureWebCoreInitialized();
    gpuLogMarker("[SL] releaseMemory: WebCore initialised");
    WebCore::releaseMemory(critical ? Critical::Yes : Critical::No,
                           Synchronous::Yes);
}

// M4 pinch: set the page scale (clamped to [0.5, 6.0]) around the focal point, so the
// content under the fingers stays put. setPageScaleFactor also updates the compositor's
// backing-store contentsScale (pageScaleFactor * deviceScale). outRGBA may be null, and
// the return is 0; the harness sends a scale plus a focal point per gesture step.
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();

    if (scale < 0.5f) scale = 0.5f;
    if (scale > 6.0f) scale = 6.0f;

    float oldScale = g_session->page->pageScaleFactor();
    if (oldScale <= 0.0f) oldScale = 1.0f;
    ScrollPosition scroll = view->scrollPosition();
    // document-space focal point = (scroll + focal)/oldScale; new scroll = it * newScale - focal
    double cx = (static_cast<double>(scroll.x()) + focalX) / oldScale;
    double cy = (static_cast<double>(scroll.y()) + focalY) / oldScale;
    int nsx = static_cast<int>(cx * scale - focalX + 0.5);
    int nsy = static_cast<int>(cy * scale - focalY + 0.5);
    if (nsx < 0) nsx = 0;
    if (nsy < 0) nsy = 0;

    g_session->page->setPageScaleFactor(scale, IntPoint(nsx, nsy));
    g_session->page->isolatedUpdateRendering();
    doc->updateLayoutIgnorePendingStylesheets();

    int nonWhite = 0;
    // g_gpuScrollFast stays clear here: a scale change must not skip the dirty-tree pass
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// M4: the page scale as a per-mille integer (1000 = 1.0x, 2500 = 2.5x), for the harness
int WebCoreGetPageScale()
{
    if (!g_session || !g_session->page)
        return 1000;
    float s = g_session->page->pageScaleFactor();
    if (s <= 0.0f) s = 1.0f;
    return static_cast<int>(s * 1000.0f + 0.5f);
}

// Apotheosis 2026-09-19: the CSS page zoom -- how many device pixels one CSS pixel occupies.
//
// This is NOT WebCoreSetPageScale above, and the difference is the whole reason the function exists.
// Page scale MAGNIFIES the finished layer tree without re-running layout (the layout viewport is
// unchanged, so `documentElement.clientWidth` stays put). Zoom RE-LAYS OUT: the layout viewport
// becomes viewport/zoom CSS px, every layer's geometry is multiplied by zoom, and media queries and
// `device-width` follow. A zoomed page is not a magnified page.
//
// Why the harness needs it. In GPU mode the engine viewport is the panel's PHYSICAL size, because
// the ANGLE swapchain is created in physical pixels and XAML stretches whatever size it is given
// across the whole panel -- a DIP-sized surface therefore comes out magnified by the panel's
// CompositionScale (2.00 on the bench, and the button that made the page look zoomed-in). Handing
// the engine 2736 physical px as a viewport without a compensating zoom lays the page out at 2736
// CSS px: the desktop layout at half size. Measured 0.1.10.25 on the bench, same window, same page:
// `contents=2736x12368` in GPU mode against software mode's `contents=1368x12368`. With
// zoom = CompositionScale the layout is 1368 CSS px -- software's own answer -- while the layers are
// drawn at 2 device px per CSS px into the 2736-px surface. Because zoom multiplies the layer
// geometry, the TextureMapper tiles come out at the physical resolution instead of being upscaled,
// which is where the sharpness comes from.
//
// Software present has a DIP-sized viewport and must stay at zoom 1.0, so this is a GPU-path setting:
// the harness applies it when the GPU is armed and resets it to 1.0 on a hand-back to software.
//
// **A request, not a one-shot application.** The frame's factor does not survive a navigation: with
// the GPU presenting, 2.00 established at arming was found back at 1.00 at the next hand-back, twice
// (07:18:38 -> 07:20:03 and 07:21:27 -> 07:25:42). The harness re-applies the zoom from EnableGpu,
// which runs only after the load settles, so the frames between the commit and that call would be
// laid out at the un-zoomed width -- twice too wide against a physical-pixel viewport, i.e. the
// reported "stretched" page. The value is therefore remembered in g_pageZoom and re-asserted by
// apoReassertPageZoom from the present paths, which costs one float compare per frame and removes
// the window entirely. See Doc/GPU-LIVENESS.md §9.
//
// Threading: engine-thread-only, like every other mutating export here.
//
// It does NOT repaint, deliberately: both callers in the harness are a resize that repaints anyway
// (GPU arm -> ApplyViewportSize -> WebCoreGpuResize; hand-back -> ApplyViewportSize ->
// WebCoreSessionResize), and a full repaint of a real page costs ~1.2 s of engine thread here. A
// caller that sets the zoom and does not repaint leaves the screen showing the previous layout --
// there is no other caller, and none should be added without one.
int WebCoreSetPageZoom(float zoom)
{
    using namespace WebCore;
    if (!g_session || !g_session->page || !g_session->mainFrame)
        return kErrNoSession;
    if (!(zoom > 0.0f))
        return kErrBadArgs;
    if (zoom < 0.1f) zoom = 0.1f;
    if (zoom > 8.0f) zoom = 8.0f;

    // The request is remembered, not just applied: a navigation resets the frame's factor behind our
    // back, so the value has to outlive the call for apoReassertPageZoom to restore it. Recorded
    // before the no-op early-out below, so a request that is a no-op *now* still governs the next
    // present -- which is the whole point, since "already 1.00" is exactly what the reset looks like.
    g_pageZoom = zoom;

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;

    const float before = lf->pageZoomFactor();
    if (before == zoom && lf->textZoomFactor() == 1.0f) {
        // Say the no-op out loud. "The zoom call ran and nothing changed" and "the zoom call never
        // ran" are indistinguishable in a log that only records success, and this project has paid
        // for that twice (Doc/GPU-LIVENESS.md §8).
        gpuLogMarkerF("[GPU] page zoom already %.2f -- nothing to do", (double)zoom);
        return kOK;
    }

    lf->setPageZoomFactor(zoom);
    // The zoom changes every length in the tree, so nothing about the previous layout survives.
    // No view->setNeedsLayout() here: LocalFrameView has no such member in this revision, and
    // setPageZoomFactor already marks the frame's layout dirty -- the forced layout below is what
    // makes the change visible to the very next contentsSize() read.
    if (RefPtr<Document> doc = lf->document())
        doc->updateLayoutIgnorePendingStylesheets();
    g_session->page->isolatedUpdateRendering();

    const int dw = view->contentsSize().width();
    gpuLogMarkerF("[GPU] page zoom %.2f -> %.2f (layout %dx%d CSS-ish, viewport %dx%d)",
        (double)before, (double)zoom, dw, view->contentsSize().height(), g_session->w, g_session->h);
    return kOK;
}

// The mobile-vs-desktop UA switch exposed to the host UI
// UA: mobile=1 -> iPhone UA, 0 -> the Windows desktop UA. Set from the harness, not here.
void WebCoreSetUserAgentMobile(int mobile)
{
    g_apoUaMobile = (mobile != 0);
}

// Custom UA override for userAgent(); empty means fall back to the built-in mobile/desktop UA
void WebCoreSetUserAgentString(const char* ua)
{
    if (!ua || !*ua) { g_apoCustomUA[0] = '\0'; return; }
    size_t n = std::strlen(ua);
    if (n >= sizeof(g_apoCustomUA)) n = sizeof(g_apoCustomUA) - 1;
    std::memcpy(g_apoCustomUA, ua, n);
    g_apoCustomUA[n] = '\0';
}

// M1: is the GPU path armed? True when PortChromeClient has a root GraphicsLayer
// (i.e. compositing is possible for this session): 1 = armed, 0 = not.
int WebCoreEnableCompositing()
{
    if (!g_session || !g_session->chrome)
        return 0;
    return g_session->chrome->rootLayer() != nullptr ? 1 : 0;
}

#if USE(TEXTURE_MAPPER)
// M2: bring up the GPU -- GL context, TextureMapper, and the globals they need
int WebCoreGpuInit(void* nativeWindow, int w, int h)
{
    using namespace WebCore;
    gpuLogMarker("[GPU] enter");
    if (w <= 0 || h <= 0)
        return kErrBadArgs;
    // Apotheosis 2026-09-04: the early-out is correct but it was silent, and that silence was
    // itself a suspect (open question 2 of the 0.1.9.84 triage). The per-URL re-probe calls this
    // again on every navigation; the second call returns kOK having created nothing, so the
    // surface keeps the geometry of the FIRST call. If the requested size differs, the caller has
    // been told "GPU ready at w x h" about a surface that is not that size -- which is a real
    // defect, not a stale-context guess. Say so in the log instead of leaving the reader to
    // deduce it from a bare "[GPU] enter" with no following marker (that absence is what the
    // 21:04:00 re-probe looked like, and reading it took longer than it should have).
    if (g_gpuActive) {
        if (w != g_gpuW || h != g_gpuH)
            gpuLogMarkerF("[GPU] already active at %dx%d, request %dx%d IGNORED -- use WebCoreGpuResize",
                g_gpuW, g_gpuH, w, h);
        else
            gpuLogMarkerF("[GPU] already active at %dx%d -- reusing context", g_gpuW, g_gpuH);
        return kOK;
    }
    // Apotheosis: fine-grained GPU init logging to pinpoint the post-init crash
    // (process dies without any exception stream; gpuinit.txt never written).
    gpuLogMarker("[GPU] ensureWebCoreInitialized");
    ensureWebCoreInitialized();
    gpuLogMarker("[GPU] sharedDisplay");
    PlatformDisplay& display = PlatformDisplay::sharedDisplay();
    gpuLogMarker("[GPU] GLContext::create");
    std::unique_ptr<GLContext> ctx = nativeWindow
        ? GLContext::create(display, reinterpret_cast<GLNativeWindowType>(nativeWindow))
        : GLContext::createOffscreen(display);
    if (!ctx) {
        gpuLogMarker("[GPU] GLContext::create -> null");
        return -20;
    }
    gpuLogMarker("[GPU] makeContextCurrent");
    if (!ctx->makeContextCurrent()) {
        gpuLogMarker("[GPU] makeContextCurrent -> false");
        return -21;
    }
    gpuLogMarker("[GPU] TextureMapper::create");
    std::unique_ptr<TextureMapper> tm = TextureMapper::create();
    if (!tm) {
        gpuLogMarker("[GPU] TextureMapper::create -> null");
        return -22;
    }
    gpuLogMarker("[GPU] commit globals");
    g_glContext = ctx.release();
    g_textureMapper = tm.release();
    g_gpuW = w;
    g_gpuH = h;
    g_gpuPresentMode = (nativeWindow != nullptr);
    g_gpuActive = true;
    gpuLogMarker("[GPU] done");
    return kOK;
}

// Apotheosis: resize a live GPU session's render surface + viewport to w x h and repaint.
//
// WebCoreGpuInit created the ANGLE window surface ONCE at the harness-passed size (kW x kH,
// initially the 720x1080 phone portrait viewport) and the SwapChainPanel stretches it to fill
// ContentArea. GPU mode never resized it, so any aspect mismatch stretched the whole page
// (the "everything too wide" distortion). This recreates the surface at the new size.
//
// Threading: engine-thread-only, serialized by the job queue (like WebCoreGpuInit). ANGLE
// marshals surface create/destroy back to the panel dispatcher internally, so no UI-thread
// wait ever happens. `nativeWindow` is a FRESH IInspectable* of a PropertySet carrying
// EGLNativeWindowTypeProperty (same SwapChainPanel) + EGLRenderSurfaceSizeProperty = (w,h);
// the caller must keep it alive (the harness stores it in m_gpuProps on success).
//
// Tear-down order matters: the TextureMapper's GL texture ids belong to the old context, so
// it is freed while that context is still current, BEFORE the context/surface itself. A fresh
// TextureMapper is built on the new context, and g_gpuScrollFast is cleared so gpuPrepare
// force-regenerates every layer's backing store on the new context.
int WebCoreGpuResize(void* nativeWindow, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !nativeWindow || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;

    // Apotheosis: per-step timing markers. A resize of a live heavy page (hh.ru, a 1024x5176
    // SPA) was measured at 5.9 s once and then never returned at all, which stalled every
    // later job on the engine thread — the queue is FIFO and single-threaded, so a wedged
    // resize silently swallows navigations. From the outside the candidates were
    // indistinguishable (ANGLE surface teardown/create marshalling to the panel dispatcher vs
    // relayout + full backing-store regeneration at a new width), because all of them run
    // before writeDiag() and therefore leave the diag text frozen at the pre-resize geometry.
    // Each marker is flushed before the next step starts, so the last line in
    // LocalState\gpuinit-steps.txt names the step that hung.
    const MonotonicTime resizeT0 = MonotonicTime::now();
    MonotonicTime resizePrev = resizeT0;
    auto step = [&](const char* name) {
        const MonotonicTime now = MonotonicTime::now();
        gpuLogMarkerF("[GPU] resize %dx%d: %s (+%.0f ms, total %.0f ms)", w, h, name,
            (now - resizePrev).milliseconds(), (now - resizeT0).milliseconds());
        resizePrev = now;
    };
#if USE(TEXTURE_MAPPER)
    if (!g_gpuActive || !g_glContext || !g_textureMapper) {
        step("no live GPU context -> kErrNoView");
        return kErrNoView;
    }
    step("begin");

    delete g_textureMapper;
    g_textureMapper = nullptr;
    step("old TextureMapper deleted");
    delete g_glContext;
    g_glContext = nullptr;
    step("old GLContext deleted");

    PlatformDisplay& display = PlatformDisplay::sharedDisplay();
    std::unique_ptr<GLContext> newCtx = GLContext::create(display, reinterpret_cast<GLNativeWindowType>(nativeWindow));
    step("GLContext::create returned");
    if (!newCtx || !newCtx->makeContextCurrent()) {
        // Surface is gone; never present into a dead swapchain. Fall back to Cairo soft
        // rendering for the rest of the session (harness keeps its old kW/kH on rc!=0).
        g_gpuActive = false;
        g_gpuPresentMode = false;
        gpuLogMarker("[GPU] WebCoreGpuResize: GLContext::create failed");
        return -21;
    }
    step("new context current");
    std::unique_ptr<TextureMapper> newTm = TextureMapper::create();
    if (!newTm) {
        g_gpuActive = false;
        g_gpuPresentMode = false;
        gpuLogMarker("[GPU] WebCoreGpuResize: TextureMapper::create failed");
        return -22;
    }
    step("new TextureMapper created");
    g_glContext = newCtx.release();
    g_textureMapper = newTm.release();
    g_gpuW = w;
    g_gpuH = h;
    g_gpuScrollFast = false;   // regenerate all backing stores on the fresh context
#endif
    g_session->w = w;
    g_session->h = h;
    g_stageTrace = true;                       // fine-grained markers inside finishInteractionPaint
    const int rc = finishInteractionPaint(outRGBA);
    g_stageTrace = false;
    char tail[64];
    snprintf(tail, sizeof(tail), "finishInteractionPaint rc=%d", rc);
    step(tail);
    return rc;
}

int WebCoreComposite()
{
    using namespace WebCore;
    if (!g_gpuActive || !g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    GraphicsLayer* root = g_session->chrome->rootLayer();
    if (!view || !root) {
        // Apotheosis 2026-09-19: kErrNoView (-4) is the answer the harness's GPU probe reads as "this
        // page/build cannot present through the GPU", and WHICH of the three is missing -- the frame,
        // its view, or the compositing root layer -- decides where to look next, but the caller cannot
        // see it: the harness logs `Composite=-4` and nothing else. Measured 2026-09-18 23:58:
        // `EnableCompositing=1 Composite=-4` on a fully rendered dzen.ru, i.e. rootLayer() was non-null
        // in the call immediately before, which leaves the frame's view as the suspect -- and the
        // software path (`tail: paintToRGBA exit rc=0 nonWhite=200299`) proves a view exists somewhere.
        // `mainFrame` and `page->localMainFrame()` are two different handles and are printed side by
        // side here for exactly that reason. Same rule as WebCoreClickAt's [HIT] trace: name the branch,
        // or the next reader infers it wrongly.
        gpuLogMarkerF("[GPU] Composite -> kErrNoView: g_gpuActive=%d page=%p frame=%p view=%p "
                      "root=%p mainFrame=%p pageMainFrame=%p",
            g_gpuActive ? 1 : 0, (void*)g_session->page, (void*)lf.get(),
            (void*)view.get(), (void*)root, (void*)g_session->mainFrame.get(),
            (void*)g_session->page->localMainFrame());
        return kErrNoView;
    }
    return gpuPresent(*view, g_gpuW, g_gpuH, *root);
}

int WebCoreCompositeReadback(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_gpuActive || !g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    if (!outRGBA)
        return kErrBadArgs;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    GraphicsLayer* root = g_session->chrome->rootLayer();
    if (!view || !root) {
        // Apotheosis 2026-09-19: see the same branch in WebCoreComposite -- the readback path needs the
        // same attribution, because it is the one that prints `rb:` lines and its -4 has the identical
        // three candidates.
        gpuLogMarkerF("[GPU] CompositeReadback -> kErrNoView: frame=%p view=%p root=%p",
            (void*)lf.get(), (void*)view.get(), (void*)root);
        return kErrNoView;
    }
    int nonWhite = 0;
    return gpuCompositeReadback(*view, g_gpuW, g_gpuH, *root, outRGBA, nonWhite);
}

void WebCoreGpuSetFlip(int flipH, int flipV)
{
    g_gpuFlipH = (flipH != 0);
    g_gpuFlipV = (flipV != 0);
}

// Apotheosis 2026-08-29: the host tells the driver whether the GPU surface is on screen RIGHT NOW.
// Call with 1 immediately after switching to the swapchain panel and with 0 the moment the host
// reverts to the software bitmap (or hides the panel for any other reason). Getting this wrong in the
// 0 direction merely costs a readback; getting it wrong in the 1 direction paints nothing at all,
// which is why it defaults to 0 and why the host must set it explicitly.
void WebCoreSetDirectPresent(int on)
{
    g_gpuDirectPresent = (on != 0);
    gpuLogMarkerF("[GPU] direct present -> %d", g_gpuDirectPresent ? 1 : 0);
}
#else // !USE(TEXTURE_MAPPER): stubs when building without TextureMapper (e.g. x64-gpu with Cairo only)
int WebCoreGpuInit(void*, int, int) { return -20; }
int WebCoreComposite() { return kErrNoSession; }
int WebCoreCompositeReadback(uint8_t*) { return kErrNoSession; }
void WebCoreGpuSetFlip(int, int) { }
void WebCoreSetDirectPresent(int) { }
#endif // USE(TEXTURE_MAPPER)

// M2: dump the FrameView's scroll/contents/view/document-background numbers plus the
// layer tree (GraphicsLayer::layerTreeAsText) into out, for the GPU diagnostics.
int WebCoreGpuLayerInfo(char* out, int len)
{
    using namespace WebCore;
    if (!out || len <= 0)
        return kErrBadArgs;
    out[0] = 0;
    if (!g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    std::string s;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    IntPoint origScroll;
    bool didProbe = false;
    if (view) {
        IntPoint sp = view->scrollPosition();
        origScroll = sp;
        IntPoint minP = view->minimumScrollPosition();
        IntPoint maxP = view->maximumScrollPosition();
        IntSize cs = view->contentsSize();
        Color bg = view->documentBackgroundColor();
        auto [r, g, b, a] = (bg.isValid() ? bg : Color::white).toColorTypeLossy<SRGBA<float>>().resolved();
        bool usesComp = view->renderView() && view->renderView()->usesCompositing();
        char h[512];
        snprintf(h, sizeof h,
                 "docBg=#%02X%02X%02X%02X valid=%d lastContentPx=%d\n"
                 "scrollPos=%d,%d min=%d,%d max=%d,%d contents=%dx%d view=%dx%d usesCompositing=%d\n",
                 (int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f), (int)(a * 255 + 0.5f),
                 bg.isValid() ? 1 : 0, g_lastContentPx,
                 sp.x(), sp.y(), minP.x(), minP.y(), maxP.x(), maxP.y(),
                 cs.width(), cs.height(), g_session->w, g_session->h, usesComp ? 1 : 0);
        s += h;
        // Scroll round-trip probe: push the view to y=300, run frameViewDidScroll, read it back.
        // A read-back of 0 means maxScroll is 0; scrolled contents should show at (0,-300).
        view->setScrollPosition(ScrollPosition(0, 300));
        if (auto* rv = view->renderView())
            rv->compositor().frameViewDidScroll();
        IntPoint sp2 = view->scrollPosition();
        char h2[160];
        // Apotheosis 2026-09-18: the trailing Chinese annotation here was destroyed to '?' bytes by
        // the same encoding accident that hit the harness (Doc/DESTROYED-STRINGS.md); replaced, not
        // restored. The probe is a scroll round-trip: push the view to y=300, then read it back.
        snprintf(h2, sizeof h2, "-- after setScrollPosition(0,300)+frameViewDidScroll: scrollPos=%d,%d (expected 0,300) --\n",
                 sp2.x(), sp2.y());
        s += h2;
        didProbe = true;
    }
    if (GraphicsLayer* root = g_session->chrome->rootLayer()) {
        String tree = root->layerTreeAsText(AllLayerTreeAsTextOptions);   // the layer fields worth grepping: paintsIntoWindow/tileCache/drawsContent/backingStoreAttached
        CString u = tree.utf8();
        s.append(u.data(), u.length());
    } else {
        s += "(no root GraphicsLayer)\n";
    }
    if (didProbe && view) {   // restore the original scroll -- the probe must not move the page
        view->setScrollPosition(origScroll);
        if (auto* rv = view->renderView())
            rv->compositor().frameViewDidScroll();
    }
    int n = static_cast<int>(s.size());
    if (n > len - 1) n = len - 1;
    memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = 0;
    return kOK;
}

int WebCoreFocusedEditable()
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame || g_inPump)
        return 0;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    // Is an editable focused? A text input, a textarea or a contenteditable element
    // counts; canEdit() alone is unreliable headless, so the element is checked first.
    if (RefPtr<Document> doc = lf->document()) {
        if (RefPtr<Element> fe = doc->focusedElement()) {
            if (is<HTMLInputElement>(*fe))
                return downcast<HTMLInputElement>(*fe).isTextField() ? 1 : 0;
            if (is<HTMLTextAreaElement>(*fe))
                return 1;
            if (is<HTMLElement>(*fe) && downcast<HTMLElement>(*fe).isContentEditable())
                return 1;
        }
    }
    return lf->editor().canEdit() ? 1 : 0;
}

// Insert text into the focused editable, pump so its JS runs, then repaint
int WebCoreTypeText(const char* utf8, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!utf8 || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<Document> doc = lf->document();
    String text = String::fromUTF8(utf8);

    // Why write straight into the DOM's value: measured (see the imedebug line) a focused
    // input can report canEditAfter=1, inserted=1 and still end up with valueLen=0 -- headless
    // editor().canEdit()/FrameSelection cannot be trusted for input elements, and
    // insertTextWithoutSendingTextEvent does not update .value. So: set the value and dispatch
    // input (no change event, exactly like any programmatic set); contenteditable falls
    // through to the editor's own insertText below.
    int canEditBefore = lf->editor().canEdit() ? 1 : 0;
    const char* feTag = "none";
    const char* pathTag = "none";
    int inserted = 0;
    RefPtr<Element> fe = doc ? doc->focusedElement() : nullptr;
    if (fe && is<HTMLInputElement>(*fe) && downcast<HTMLInputElement>(*fe).isTextField()) {
        feTag = "input"; pathTag = "direct";
        auto& input = downcast<HTMLInputElement>(*fe);
        String cur = input.value();
        (void)input.setValue(makeString(cur, text), DispatchNoEvent); // append without firing an event; the input event is dispatched just below
        input.dispatchInputEvent();
        inserted = 1;
    } else if (fe && is<HTMLTextAreaElement>(*fe)) {
        feTag = "textarea"; pathTag = "direct";
        auto& ta = downcast<HTMLTextAreaElement>(*fe);
        String cur = ta.value();
        (void)ta.setValue(makeString(cur, text), DispatchNoEvent);
        ta.dispatchInputEvent();
        inserted = 1;
    } else {
        feTag = fe ? "other" : "none"; pathTag = "editor"; // contenteditable or no element at all: go through the editor
        if (lf->editor().canEdit()) {
            lf->editor().insertTextWithoutSendingTextEvent(text, false, nullptr);
            inserted = 1;
        } else {
            // last resort: a raw Char key event into the editor
            OptionSet<PlatformEvent::Modifier> mods;
            MonotonicTime t = MonotonicTime::now();
            PlatformKeyboardEvent raw(PlatformEvent::Type::RawKeyDown, ""_s, ""_s, ""_s, ""_s, ""_s, 0, false, false, false, mods, t);
            lf->eventHandler().keyEvent(raw);
            PlatformKeyboardEvent ch(PlatformEvent::Type::Char, text, text, ""_s, ""_s, ""_s, 0, false, false, false, mods, t);
            lf->eventHandler().keyEvent(ch);
            PlatformKeyboardEvent up(PlatformEvent::Type::KeyUp, ""_s, ""_s, ""_s, ""_s, ""_s, 0, false, false, false, mods, MonotonicTime::now());
            lf->eventHandler().keyEvent(up);
        }
    }
    int canEditAfter = lf->editor().canEdit() ? 1 : 0;
    // read the value back out of the DOM (not from our own bookkeeping) for the diag line
    unsigned feValLen = 0;
    if (doc) {
        if (RefPtr<Element> fe2 = doc->focusedElement()) {
            if (is<HTMLInputElement>(*fe2))
                feValLen = downcast<HTMLInputElement>(*fe2).value()->length();
            else if (is<HTMLTextAreaElement>(*fe2))
                feValLen = downcast<HTMLTextAreaElement>(*fe2).value()->length();
        }
    }
    g_imeDiag = std::string("canEditBefore=") + std::to_string(canEditBefore)
              + " fe=" + feTag + " path=" + pathTag + " canEditAfter=" + std::to_string(canEditAfter)
              + " inserted=" + std::to_string(inserted) + " valueLen=" + std::to_string(feValLen);
    pumpQuick(*lf, g_session->page.get());
    return finishInteractionPaint(outRGBA);
}

// Read back the last WebCoreTypeText diagnostic (the "IME debug" line in LocalState)
int WebCoreEditDebug(char* out, int cap)
{
    if (!out || cap <= 0)
        return kErrBadArgs;
    std::snprintf(out, static_cast<size_t>(cap), "%s", g_imeDiag.c_str());
    return kOK;
}

// Key action: 0 = backspace, 1 = Enter (submit for an input, newline for a textarea)
int WebCoreKeyAction(int action, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    RefPtr<LocalFrame> lf = g_session->mainFrame;

    // reset the load state so the pump can wait for whatever this key triggers
    g_session->load = DriverLoadState{};
    if (g_session->client) {
        g_session->client->resetLoadState();
        DriverLoadState* lp = &g_session->load;
        g_session->client->setLoadCompletionHandler([lp](bool failed) {
            if (lp->mainDone) return;
            lp->mainDone = true;
            lp->failed = failed;
        });
    }

    if (action == 0) {
        // As in WebCoreTypeText: for input/textarea trim the value directly (the harness sets it
        // that way), and use the editor's DeleteBackward only for contenteditable.
        RefPtr<Document> kdoc = lf->document();
        RefPtr<Element> kfe = kdoc ? kdoc->focusedElement() : nullptr;
        if (kfe && is<HTMLInputElement>(*kfe) && downcast<HTMLInputElement>(*kfe).isTextField()) {
            auto& input = downcast<HTMLInputElement>(*kfe);
            String cur = input.value();
            if (!cur.isEmpty()) {
                (void)input.setValue(cur.left(cur.length() - 1), DispatchNoEvent);
                input.dispatchInputEvent();
            }
        } else if (kfe && is<HTMLTextAreaElement>(*kfe)) {
            auto& ta = downcast<HTMLTextAreaElement>(*kfe);
            String cur = ta.value();
            if (!cur.isEmpty()) {
                (void)ta.setValue(cur.left(cur.length() - 1), DispatchNoEvent);
                ta.dispatchInputEvent();
            }
        } else {
            lf->editor().command("DeleteBackward"_s).execute();
        }
        // As in WebCoreTypeText: pump with its ~0.8 s quiet window before repainting
        pumpQuick(*lf, g_session->page.get());
        return finishInteractionPaint(outRGBA);
    } else if (action == 1) {
        OptionSet<PlatformEvent::Modifier> mods;
        MonotonicTime t = MonotonicTime::now();
        // RawKeyDown + Char (charCode 13, so keypress handlers see Enter) + KeyUp
        PlatformKeyboardEvent raw(PlatformEvent::Type::RawKeyDown, ""_s, ""_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, t);
        lf->eventHandler().keyEvent(raw);
        PlatformKeyboardEvent ch(PlatformEvent::Type::Char, "\r"_s, "\r"_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, t);
        lf->eventHandler().keyEvent(ch);
        PlatformKeyboardEvent up(PlatformEvent::Type::KeyUp, ""_s, ""_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, MonotonicTime::now());
        lf->eventHandler().keyEvent(up);
    } else {
        return kErrBadArgs;
    }
    pumpLoop(*lf, &g_session->load.mainDone, true, 160, 30.0, g_session->page.get());
    return finishInteractionPaint(outRGBA);
}

// Repaint the session as it stands (no input, no pump); 0 on success
int WebCoreSessionPaint(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// Apotheosis: resize the live session's viewport and repaint at the new size.
//
// Without this the viewport was whatever width the harness passed to WebCoreSessionLoad -- a
// hardcoded 720x1080 phone viewport -- so on a desktop-sized window the page laid out in a
// 720px column and the rest of the window stayed empty. Every paint entry point writes exactly
// g_session->w * g_session->h * 4 bytes, so the size is a property of the session, not of the
// call, and it can only be changed here: the caller must allocate its buffer for the size it
// passes in and must not resize concurrently with another engine call (all C ABI calls are
// already serialized onto the single engine thread).
//
// The heavy lifting is finishInteractionPaint(), the same path a click or a scroll takes: it
// re-resizes the view from g_session, relayouts, re-extracts the link hit-table (link rects are
// viewport-relative, so they are stale the moment the width changes) and repaints.
int WebCoreSessionResize(int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;
    // 8192 is an arbitrary but generous sanity ceiling: paintToRGBA has to create a cairo
    // surface of exactly this size, and a bogus value from the harness would be a huge
    // allocation failure deep inside cairo rather than a clean error here.
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;

    g_session->w = w;
    g_session->h = h;
#if USE(TEXTURE_MAPPER)
    // Apotheosis: keep the compositor's idea of the surface in step. The ANGLE surface itself is
    // resized by the harness on the panel dispatcher (never from here -- waiting on the UI thread
    // from the engine thread deadlocks), so this only updates the size we present at.
    if (g_gpuActive) {
        g_gpuW = w;
        g_gpuH = h;
    }
#endif
    return finishInteractionPaint(outRGBA);
}

// Live tick: RunLoop cycles -> rendering update -> layout -> paint without full pump.
// Updates diag information for the harness and returns the paint result.
// Harness compares g_lastFrameHash to decide whether a new blit is needed.
// Apotheosis: which step of WebCoreLiveTick is currently executing.
//
// Measured on the device 2026-08-22: the engine thread hangs inside the job labelled `live-tick`
// (heartbeat: `busy=1 pending=0 job=live-tick`), twice in a row. WebCoreLiveTick is a sequence of
// seven distinct operations and the trace stops among loader traffic, which runs inside the
// RunLoop::cycle() calls at the top -- so the step matters and guessing it does not.
//
// Deliberately a plain integer and NOT a marker file. Every other diagnostic here writes a flushed
// line per step, which is right for a path taken once per navigation and wrong for this one: the tick
// runs several times a second, and an fopen/fwrite/fclose per step on the device's slow flash would
// change the timing of the very thing being measured. One store to a volatile long costs nothing, and
// the value is read out by the harness's heartbeat -- from the UI thread, which is provably still
// alive when the engine thread is not.
//
// Single writer (the engine thread), single reader (the UI thread), and a torn read is impossible for
// an aligned long on both targets. volatile rather than std::atomic only to keep this usable from
// inside the strict_gs_check regions without pulling in more machinery.
static volatile long g_liveTickStep = 0;

enum LiveTickStep {
    kTickIdle              = 0,
    kTickRunLoopCycle1     = 1,   // dispatching queued callbacks -- curl responses land here
    kTickRunLoopCycle2     = 2,
    kTickRunLoopCycle3     = 3,
    kTickIsolatedRendering = 4,   // rAF / IntersectionObserver / JS
    kTickFrameLookup       = 5,
    kTickMicrotasks        = 6,   // promise callbacks, module evaluation
    kTickLayout            = 7,   // updateLayoutIgnorePendingStylesheets
    kTickCountResources    = 8,
    kTickPaint             = 9,   // paintToRGBA
    kTickReturned          = 10,
};

int WebCoreGetLiveTickStep(void)
{
    return (int)g_liveTickStep;
}

// Apotheosis 2026-09-19: the engine's liveness gauge, published for the harness's wedge watchdog.
// See `apoEngineTick` for why it exists and what it does not cover.
//
// Never negative, monotonic in every phase that matters, and deliberately NOT reset per job: the
// caller compares it against its own previous reading, so only a change has to be visible. The live
// tick's step index is added in rather than incremented through a setter, because that value already
// moves once per operation inside the tick (RunLoop cycles, rendering, microtasks, layout, paint) --
// which is exactly the granularity the watchdog needs from the one job that can run for seconds at a
// time on a heavy page. The two parts cannot cancel into a false "unmoved": a step index that falls
// while the counter rises by one is a harmless one-beat miss, and the next beat sees both.
long long WebCoreGetEngineActivity(void)
{
    return g_engineActivity.load(std::memory_order_relaxed) + (long long)g_liveTickStep;
}

int WebCoreLiveTick(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;

#if defined(WK_WINUWP)
    // Apotheosis: fresh 15 s script budget for this tick's isolatedUpdateRendering/microtasks
    // (see apoArmJsWatchdog above for why the deadline must be re-armed per window).
    apoArmJsWatchdog();
#endif

    // Cycle RunLoop to process timers and queued tasks before isolatedUpdateRendering.
    for (int i = 0; i < 3; ++i) {
        g_liveTickStep = kTickRunLoopCycle1 + i;
        RunLoop::cycle();
    }
    g_liveTickStep = kTickIsolatedRendering;
    g_session->page->isolatedUpdateRendering();   // rAF, intersection observers, and rendering updates
    g_liveTickStep = kTickFrameLookup;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    if (!lf) {
        g_liveTickStep = kTickReturned;
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view) {
        g_liveTickStep = kTickReturned;
        return kErrNoView;
    }
    RefPtr<Document> doc = lf->document();
    if (!doc) {
        g_liveTickStep = kTickReturned;
        return kErrNoDocument;
    }
    g_liveTickStep = kTickMicrotasks;
    doc->eventLoop().performMicrotaskCheckpoint();
    g_liveTickStep = kTickLayout;
    doc->updateLayoutIgnorePendingStylesheets();
    g_liveTickStep = kTickCountResources;
    g_lastPendingResources = countPendingResources(*doc);
    int nonWhite = 0;
    g_liveTickStep = kTickPaint;
    const int paintRc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    g_liveTickStep = kTickReturned;
    return paintRc;
}

int WebCoreGetPendingResourceCount()
{
    return g_lastPendingResources;
}

// Frame hash for change detection by the harness.
unsigned WebCoreGetFrameHash()
{
    return g_lastFrameHash;
}

// ---------------------------------------------------------------------------
// Minimal stub variant for bring-up: only does process init + Page creation,
// then fills the buffer with opaque solid red. Lets you validate the init +
// Page::create path (steps 1-3) and the C ABI/buffer plumbing before trusting
// the full layout+paint path. Build with -DWEBCOREDRIVER_STUB to substitute it
// for the real entry point.
// ---------------------------------------------------------------------------
#ifdef WEBCOREDRIVER_STUB
int WebCoreRenderHtmlStub(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    (void)utf8Html;
    if (!outRGBA || w <= 0 || h <= 0)
        return kErrBadArgs;
    ensureWebCoreInitialized();
    auto cfg = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    // Apotheosis 2026-09-18: real Web Storage, as in buildSession -- see Src/port/PortStorage.h.
    cfg.storageNamespaceProvider = WebCorePort::createPortStorageNamespaceProvider();
    Ref<Page> page = Page::create(WTF::move(cfg));
    if (!page->localMainFrame())
        return kErrNoMainFrame;
    for (int i = 0; i < w * h; ++i) {
        outRGBA[i * 4 + 0] = 0xFF; // R
        outRGBA[i * 4 + 1] = 0x00; // G
        outRGBA[i * 4 + 2] = 0x00; // B
        outRGBA[i * 4 + 3] = 0xFF; // A
    }
    return kOK;
}
#endif

} // extern "C"

