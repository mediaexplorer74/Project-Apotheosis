// ============================================================================
// core.cpp — Engine core: init, session, rendering, GPU
//
// This file owns:
//   - One-time WebKit process initialization
//   - Session struct + build/teardown lifecycle
//   - Cairo software rendering (paintToRGBA)
//   - GPU compositing pipeline (ANGLE + TextureMapper)
//   - RunLoop pump + link extraction + diagnostics
//
// All functions run on the single WebKit main thread.
// Public C ABI exports are in browser.cpp (interaction, navigation, find, etc.).
// ============================================================================

#include "WebCoreDriverInternal.h"

extern void installPortPlatformStrategies();

namespace {

using namespace WebCore;

// ============================================================================
// Globals — defined here, extern'd in WebCoreDriverInternal.h
// ============================================================================

// Diagnostics
char g_lastNetError[512] = "";
char g_lastDiag[4096] = "";
char g_lastTitle[512] = "";
char g_lastUrl[1024] = "";
uint32_t g_lastFrameHash = 0;
int g_lastPendingResources = 0;

// User agent
extern "C" bool g_apoUaMobile = true;
extern "C" char g_apoCustomUA[2048] = {0};

// SPA probe
char g_spaProbe[512] = "";

// CA cert blob
std::vector<uint8_t> g_caBytes;

// Sub-resource load counters
int g_loadStarted = 0, g_loadResponse = 0, g_loadComplete = 0, g_loadFail = 0;

extern "C" void WebCorePortBumpLoad(int kind)
{
    switch (kind) {
    case 0: ++g_loadStarted; break;
    case 1: ++g_loadResponse; break;
    case 2: ++g_loadComplete; break;
    case 3: ++g_loadFail; break;
    }
}

// Link hit-table
std::vector<LinkRect> g_links;
std::string g_imeDiag;

// Find-in-page state
WTF::String g_findText;
WebCore::FindOptions g_findOpts;

// GPU state
bool g_gpuActive = false;
WebCore::GLContext* g_glContext = nullptr;
WebCore::TextureMapper* g_textureMapper = nullptr;
int g_gpuW = 0, g_gpuH = 0;
bool g_gpuFlipH = false;
bool g_gpuFlipV = false;
int g_lastContentPx = 0;
bool g_gpuScrollFast = false;
bool g_gpuPresentMode = false;

// Session
std::optional<Session> g_session;
bool g_inPump = false;

// ============================================================================
// Process init
// ============================================================================

bool ensureWebCoreInitialized()
{
    static bool initialized = [] {
        JSC::initialize();
        WTF::initializeMainThread();
        WebCore::initializeCommonAtomStrings();
        installPortPlatformStrategies();
        WebCore::populateJITOperations();

        // OOM prevention on 32-bit: disable back-forward cache, tighten resource caps
        WebCore::BackForwardCache::singleton().setMaxSize(0);
        WebCore::MemoryCache::singleton().setCapacities(0, 8u * 1024 * 1024, 16u * 1024 * 1024);
        return true;
    }();
    return initialized;
}

// ============================================================================
// RunLoop pump
// ============================================================================

void pumpLoop(LocalFrame& frame, const bool* mainDone, bool allowEarlyStopWithoutNav,
              int settleCapTicks, double watchdogSeconds, Page* pageForRendering)
{
    bool stopped = false;
    auto stopLoop = [&stopped] {
        if (stopped) return;
        stopped = true;
        RunLoop::currentSingleton().stop();
    };

    int settleTicks = 0;
    int quietTicks = 0;
    RefPtr<LocalFrame> frameRef = &frame;

    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "pump.settle"_s,
        WTF::Function<void()> { [&stopLoop, &settleTicks, &quietTicks, frameRef, mainDone,
                                  allowEarlyStopWithoutNav, settleCapTicks, pageForRendering] {
            if (pageForRendering) {
                pageForRendering->isolatedUpdateRendering();
                RefPtr<LocalFrame> mf = pageForRendering->localMainFrame();
                if (mf.get() != frameRef.get()) { stopLoop(); return; }
            }
            if (RefPtr<Document> doc = frameRef->document())
                doc->eventLoop().performMicrotaskCheckpoint();

            RefPtr<DocumentLoader> dl = frameRef->loader().activeDocumentLoader();
            bool loading = dl && dl->isLoadingInAPISense();
            bool navDone = mainDone && *mainDone;
            bool ready = navDone || allowEarlyStopWithoutNav;

            if (loading) quietTicks = 0; else ++quietTicks;
            if (navDone) ++settleTicks;
            if (ready && quietTicks >= 16) { stopLoop(); return; }
            if (navDone && settleTicks > settleCapTicks) stopLoop();
        } });
    settle.startRepeating(0.05_s);

    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "pump.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop(); } });
    watchdog.startOneShot(WTF::Seconds(watchdogSeconds));

    RunLoop::run();
    settle.stop();
    watchdog.stop();
}

// ============================================================================
// Link extraction
// ============================================================================

void extractLinks(Document* document, int renderH)
{
    g_links.clear();
    if (!document) return;

    Ref<HTMLCollection> links = document->links();
    unsigned n = links->length();
    for (unsigned i = 0; i < n && g_links.size() < 4000; ++i) {
        Element* el = links->item(i);
        if (!el || !is<HTMLAnchorElement>(*el)) continue;
        auto href = downcast<HTMLAnchorElement>(*el).href();
        if (href.isEmpty() || !href.isValid() || !href.protocolIsInHTTPFamily()) continue;

        FloatRect r = el->boundingClientRect();
        if (r.width() <= 0 || r.height() <= 0) continue;
        if (r.maxY() < 0 || r.y() > static_cast<float>(renderH)) continue;

        LinkRect lr;
        lr.x = static_cast<int>(r.x());
        lr.y = static_cast<int>(r.y());
        lr.w = static_cast<int>(r.width());
        lr.h = static_cast<int>(r.height());
        lr.url = href.string().utf8().data();
        g_links.push_back(std::move(lr));
    }
}

// ============================================================================
// Cairo software rendering
// ============================================================================

int paintToRGBA(LocalFrameView& view, int w, int h, uint8_t* outRGBA, int& nonWhiteOut)
{
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

    {
        GraphicsContextCairo context(adoptRef(cr));
        view.setPaintBehavior(PaintBehavior::FlattenCompositingLayers | PaintBehavior::Snapshotting);
        view.paint(context, IntRect(IntPoint(), IntSize(w, h)));
    }

    cairo_surface_flush(surface);

    // BGRA→RGBA swizzle + un-premultiply + FNV hash + non-white count
    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    uint32_t hash = 2166136261u; // FNV-1a seed
    int nonWhite = 0;

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
        }
        // FNV hash: sample every 4th pixel for speed
        for (int x = 0; x < w; x += 4) {
            hash ^= static_cast<uint32_t>(drow[x * 4 + 0]) * 31u + drow[x * 4 + 1];
            hash *= 0x01000193u;
        }
    }

    cairo_surface_destroy(surface);
    g_lastFrameHash = hash;
    nonWhiteOut = nonWhite;
    return kOK;
}

// ============================================================================
// Diagnostics
// ============================================================================

void writeDiag(Document& document, LocalFrameView& view, int w, int h, int nonWhite)
{
    URL finalURL = document.url();
    String title = document.title();
    IntSize contentsSize = view.contentsSize();

    int pending = 0;
    for (auto& kv : document.cachedResourceLoader().allCachedResources()) {
        CachedResource* res = kv.value.get();
        if (!res) continue;
        auto status = res->status();
        if (status == CachedResource::Unknown || status == CachedResource::Pending)
            ++pending;
    }
    g_lastPendingResources = pending;

    snprintf(g_lastTitle, sizeof(g_lastTitle), "%s", title.utf8().data());
    snprintf(g_lastUrl, sizeof(g_lastUrl), "%s", finalURL.string().utf8().data());

    snprintf(g_lastDiag, sizeof(g_lastDiag),
        "url=%s\ntitle=%s\ncontents=%dx%d\nnonWhite=%d\npending=%d\n"
        "loadStarted=%d loadResponse=%d loadComplete=%d loadFail=%d\nspa=%s",
        finalURL.string().utf8().data(),
        title.utf8().data(),
        contentsSize.width(), contentsSize.height(),
        nonWhite, pending,
        g_loadStarted, g_loadResponse, g_loadComplete, g_loadFail,
        g_spaProbe);
}

// ============================================================================
// GPU compositing
// ============================================================================

void forceDirtyTree(GraphicsLayer& l)
{
    l.setNeedsDisplay();
    for (const auto& c : l.children())
        forceDirtyTree(c.get());
    if (auto* r = l.replicaLayer())
        forceDirtyTree(*r);
    if (auto* m = l.maskLayer())
        m->setNeedsDisplay();
}

void gpuPrepare(LocalFrameView& view, GraphicsLayerTextureMapper& glRoot)
{
    view.updateLayoutAndStyleIfNeededRecursive();

    Color docBg = view.documentBackgroundColor();
    glRoot.setBackgroundColor(docBg.isValid() ? docBg : Color::white);

    view.flushCompositingStateIncludingSubframes();

    if (auto* renderView = view.renderView())
        renderView->compositor().frameViewDidScroll();

    if (!g_gpuScrollFast)
        forceDirtyTree(glRoot);
    g_gpuScrollFast = false;

    glRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);
    glRoot.layer().applyAnimationsRecursively(MonotonicTime::now());
}

static int gpuCompositeReadback(LocalFrameView& view, int w, int h,
                                GraphicsLayer& root, uint8_t* outRGBA, int& nonWhiteOut)
{
    if (!g_glContext || !g_textureMapper) return kErrNoView;
    g_glContext->makeContextCurrent();
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    gpuPrepare(view, glRoot);

    Ref<BitmapTexture> texture = BitmapTexture::create(IntSize(w, h),
        { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
    Color docBg = view.documentBackgroundColor();
    if (!docBg.isValid()) docBg = Color::white;

    g_textureMapper->beginPainting(TextureMapper::FlipY::No, texture.ptr());
    glClearColor(docBg.red(), docBg.green(), docBg.blue(), docBg.alpha());
    glClear(GL_COLOR_BUFFER_BIT);
    glRoot.layer().paint(*g_textureMapper);
    g_textureMapper->endPainting();

    glFinish();

    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());

    // Flip H/V + swizzle + hash
    uint32_t hash = 2166136261u;
    int nonWhite = 0;
    for (int y = 0; y < h; ++y) {
        int sy = g_gpuFlipV ? (h - 1 - y) : y;
        const uint8_t* srow = tmp.data() + static_cast<size_t>(sy) * w * 4;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            int sx = g_gpuFlipH ? (w - 1 - x) : x;
            drow[x * 4 + 0] = srow[sx * 4 + 0];
            drow[x * 4 + 1] = srow[sx * 4 + 1];
            drow[x * 4 + 2] = srow[sx * 4 + 2];
            drow[x * 4 + 3] = srow[sx * 4 + 3];
            if (drow[x * 4 + 0] != 255 || drow[x * 4 + 1] != 255 || drow[x * 4 + 2] != 255)
                ++nonWhite;
        }
        for (int x = 0; x < w; x += 4) {
            hash ^= static_cast<uint32_t>(drow[x * 4 + 0]) * 31u + drow[x * 4 + 1];
            hash *= 0x01000193u;
        }
    }
    g_lastFrameHash = hash;
    nonWhiteOut = nonWhite;
    return kOK;
}

static int gpuPresent(LocalFrameView& view, int w, int h, GraphicsLayer& root)
{
    if (!g_glContext || !g_textureMapper) return kErrNoView;
    g_glContext->makeContextCurrent();
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    gpuPrepare(view, glRoot);

    glViewport(0, 0, w, h);
    Color docBg = view.documentBackgroundColor();
    if (!docBg.isValid()) docBg = Color::white;

    g_textureMapper->beginPainting(TextureMapper::FlipY::No, nullptr);
    glClearColor(docBg.red(), docBg.green(), docBg.blue(), docBg.alpha());
    glClear(GL_COLOR_BUFFER_BIT);
    glRoot.layer().paint(*g_textureMapper);
    g_textureMapper->endPainting();

    g_glContext->swapBuffers();
    return kOK;
}

// ============================================================================
// Session build / teardown
// ============================================================================

int buildSession(const char* url, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;

    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid()) return kErrBadUrl;

    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    auto& session = *g_session;
    session.load.mainDone = false;
    session.load.failed = false;

    auto onLoadDone = [&session](bool failed) {
        if (session.load.mainDone) return;
        session.load.mainDone = true;
        session.load.failed = failed;
    };

    // Wire our LoadingFrameLoaderClient into the page config
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [&session, onLoadDone](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
                client->setLoadCompletionHandler([onLoadDone](bool failed) { onLoadDone(failed); });
                session.client = client.ptr();
                return client;
            } };
    }

    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    page->settings().setScriptEnabled(true);
    page->settings().setLoadsImagesAutomatically(true);
    page->settings().setShouldAllowUserInstalledFonts(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    page->setIsVisible(true);

    // GPU path: attach PortChromeClient if GPU active
    if (g_gpuActive) {
        auto chrome = makeUniqueRefWithoutRefCountedCheck<WebCorePort::PortChromeClient>(*page);
        session.chrome = chrome.ptr();
        page->settings().setAcceleratedCompositingEnabled(true);
        page->settings().setForceCompositingMode(true);
        pageConfiguration.chromeClient = WTFMove(chrome);
        page = Page::create(WTF::move(pageConfiguration));
        page->settings().setScriptEnabled(true);
        page->settings().setLoadsImagesAutomatically(true);
        page->settings().setShouldAllowUserInstalledFonts(false);
        page->settings().setAcceleratedCompositingEnabled(true);
        page->settings().setForceCompositingMode(true);
        page->setIsVisible(true);
    } else {
        page->settings().setAcceleratedCompositingEnabled(false);
    }

    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame) return kErrNoMainFrame;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view) return kErrNoView;

    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);
    view->resize(IntSize(w, h));

    // Issue network load
    ResourceRequest request { WTF::move(parsedURL) };
    FrameLoadRequest frameLoadRequest { *localMainFrame, WTF::move(request), SubstituteData { } };
    Ref<FrameLoader> loader = localMainFrame->loader();
    loader->load(WTF::move(frameLoadRequest));

    // Pump until idle
    pumpLoop(*localMainFrame, &session.load.mainDone, false, 160, 30.0, page.get());

    if (session.load.failed) return kErrLoadFailed;

    // Final layout
    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document) return kErrNoDocument;

    view = localMainFrame->view();
    if (!view) return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));

    document->updateLayoutIgnorePendingStylesheets();
    extractLinks(document.get(), h);

    // Render
    int nonWhite = 0;
    if (g_gpuActive && session.chrome && session.chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, w, h, *session.chrome->rootLayer());
        else
            gpuCompositeReadback(*view, w, h, *session.chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    writeDiag(*document, *view, w, h, nonWhite);

    session.page = page;
    session.mainFrame = localMainFrame;
    session.w = w;
    session.h = h;

    return kOK;
}

void teardownSession()
{
    if (!g_session) return;
    g_session->client = nullptr;
    g_session->chrome = nullptr;
    if (g_session->mainFrame) {
        if (auto* loader = g_session->mainFrame->loader().documentLoader()) {
            if (auto* writer = loader->writer())
                writer->end();
        }
    }
    if (g_session->page) {
        if (auto* mf = g_session->page->localMainFrame()) {
            if (auto* dl = mf->loader().documentLoader())
                mf->loader().stopAllLoaders();
        }
    }
    g_session->mainFrame = nullptr;
    g_session->page = nullptr;
    g_session.reset();
    // Drain deferred cleanup
    for (int i = 0; i < 4; ++i)
        RunLoop::cycle();
}

} // anonymous namespace
