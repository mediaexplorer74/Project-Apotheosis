// ============================================================================
// WebCoreDriverInternal.h — shared state and helpers for core.cpp + browser.cpp
//
// This is NOT a public header. It exposes the internal globals and helper
// functions that both modules need. The public API is WebCoreDriver.h (C ABI).
// ============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <optional>
#include <algorithm>

#include <cairo.h>

#include <wtf/MainThread.h>
#include <wtf/RefPtr.h>
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>
#include <wtf/text/CString.h>
#include <wtf/URL.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/Function.h>
#include <wtf/UniqueRef.h>
#include <wtf/Vector.h>
#include <wtf/MonotonicTime.h>
#include <wtf/OptionSet.h>

#include <JavaScriptCore/InitializeThreading.h>

#include <WebCore/CommonAtomStrings.h>
#include <WebCore/WebCoreJITOperations.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/Page.h>
#include <WebCore/Settings.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/DocumentView.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentWriter.h>
#include <WebCore/Document.h>
#include <WebCore/EventLoop.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/IntRect.h>
#include <WebCore/IntSize.h>
#include <WebCore/FloatRect.h>
#include <WebCore/HTMLCollection.h>
#include <WebCore/HTMLAnchorElement.h>
#include <WebCore/ElementInlines.h>
#include <WebCore/Color.h>
#include <WebCore/GraphicsContextCairo.h>
#include <WebCore/RefPtrCairo.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/SubstituteData.h>
#include <WebCore/EventHandler.h>
#include <WebCore/HandleUserInputEventResult.h>
#include <WebCore/FocusController.h>
#include <WebCore/Editor.h>
#include <WebCore/ScrollView.h>
#include <WebCore/PlatformMouseEvent.h>
#include <WebCore/MouseEventTypes.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlSSLHandle.h>
#include <WebCore/CertificateInfo.h>
#include <WebCore/BackForwardCache.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/CookieJar.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/CachedResourceLoader.h>
#include <WebCore/CachedResource.h>
#include <WebCore/HTMLInputElement.h>
#include <WebCore/HTMLTextAreaElement.h>
#include <WebCore/HTMLElement.h>
#include <WebCore/ScriptController.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/FindOptions.h>
#include <WebCore/SimpleRange.h>
#include <WebCore/DoublePoint.h>
#include <WebCore/GraphicsLayer.h>
#include <WebCore/RenderView.h>
#include <WebCore/RenderLayerCompositor.h>

#include <WebCore/PlatformDisplay.h>
#include <WebCore/GLContext.h>
#include "texmap/TextureMapper.h"
#include "texmap/TextureMapperLayer.h"
#include "texmap/GraphicsLayerTextureMapper.h"
#include "texmap/BitmapTexture.h"

#include "PortChromeClient.h"
#include "LoadingFrameLoaderClient.h"
#include "PortNetworkStorageSession.h"

// ============================================================================
// Error codes
// ============================================================================
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
    kErrBadUrl       = -9,
    kErrLoadFailed   = -10,
    kErrLoadTimeout  = -11,
    kErrNoSession    = -12,
    kErrBusy         = -13,
    kErrFrameGone    = -14,
};

// ============================================================================
// Shared globals (defined in core.cpp, used by browser.cpp)
// ============================================================================

// Diagnostics
extern char g_lastNetError[512];
extern char g_lastDiag[4096];
extern char g_lastTitle[512];
extern char g_lastUrl[1024];
extern uint32_t g_lastFrameHash;
extern int g_lastPendingResources;

// User agent
extern "C" bool g_apoUaMobile;
extern "C" char g_apoCustomUA[2048];

// SPA probe
extern char g_spaProbe[512];

// CA cert blob (for standalone WebCoreDownload curl handle)
extern std::vector<uint8_t> g_caBytes;

// Sub-resource load counters
extern int g_loadStarted, g_loadResponse, g_loadComplete, g_loadFail;
extern "C" void WebCorePortBumpLoad(int kind);

// Link hit-table
struct LinkRect { int x, y, w, h; std::string url; };
extern std::vector<LinkRect> g_links;

// IME debug
extern std::string g_imeDiag;

// Find-in-page state
extern WTF::String g_findText;
extern WebCore::FindOptions g_findOpts;

// GPU state
extern bool g_gpuActive;
extern WebCore::GLContext* g_glContext;
extern WebCore::TextureMapper* g_textureMapper;
extern int g_gpuW, g_gpuH;
extern bool g_gpuFlipH, g_gpuFlipV;
extern int g_lastContentPx;
extern bool g_gpuScrollFast;
extern bool g_gpuPresentMode;

// ============================================================================
// Session (defined in core.cpp, used by browser.cpp)
// ============================================================================
struct DriverLoadState {
    bool mainDone = false;
    bool failed   = false;
};

struct Session {
    RefPtr<WebCore::Page> page;
    RefPtr<WebCore::LocalFrame> mainFrame;
    WebCorePort::LoadingFrameLoaderClient* client = nullptr;
    WebCorePort::PortChromeClient* chrome = nullptr;
    int w = 0, h = 0;
    DriverLoadState load;
};
extern std::optional<Session> g_session;
extern bool g_inPump;

struct PumpGuard { ~PumpGuard() { g_inPump = false; } };

// ============================================================================
// Helper functions (defined in core.cpp, used by browser.cpp)
// ============================================================================

// Process init (JSC/MainThread/AtomStrings/PlatformStrategies)
bool ensureWebCoreInitialized();

// RunLoop pump until idle or watchdog
void pumpLoop(WebCore::LocalFrame& frame, const bool* mainDone, bool allowEarlyStopWithoutNav,
              int settleCapTicks, double watchdogSeconds, WebCore::Page* pageForRendering);

// Extract <a href> hit-table from current document
void extractLinks(WebCore::Document* document, int renderH);

// Cairo software paint: render view to RGBA8888 buffer
int paintToRGBA(WebCore::LocalFrameView& view, int w, int h, uint8_t* outRGBA, int& nonWhiteOut);

// Write diagnostic info after render
void writeDiag(WebCore::Document& document, WebCore::LocalFrameView& view, int w, int h, int nonWhite);

// Build session from URL
int buildSession(const char* url, int w, int h, uint8_t* outRGBA);

// Teardown session (safe ordering)
void teardownSession();

// GPU helpers
void forceDirtyTree(WebCore::GraphicsLayer& l);
void gpuPrepare(WebCore::LocalFrameView& view, WebCore::GraphicsLayerTextureMapper& glRoot);

// Diagnostic stage marker: writes to stage.txt in LocalState
void WebCoreStage(const char* stage);
