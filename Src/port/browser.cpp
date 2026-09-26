// ============================================================================
// browser.cpp — C ABI exports: interaction, navigation, find, GPU, downloads
//
// This file implements all 30 public functions declared in WebCoreDriver.h.
// It depends on core.cpp for engine init, session, rendering, and GPU state.
//
// All functions run on the single WebKit main thread (serialized).
// ============================================================================

#include "WebCoreDriverInternal.h"
#include "WebCoreDriver.h"
#include <cstring>
#include <cstdio>
#include <curl/curl.h>

extern void installPortPlatformStrategies();

// ============================================================================
// TLS / CA certificates
// ============================================================================

extern "C" void WebCoreSetCACertBlob(const uint8_t* data, int len)
{
    if (!data || len <= 0) return;
    Vector<uint8_t> bytes(static_cast<size_t>(len));
    std::memcpy(bytes.mutableSpan().data(), data, static_cast<size_t>(len));
    WebCore::CurlContext::singleton().sslHandle().setCACertData(WTF::move(bytes));
    g_caBytes.assign(data, data + len);
}

// ============================================================================
// One-shot rendering
// ============================================================================

extern "C" int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!utf8Html || !outRGBA || w <= 0 || h <= 0) return kErrBadArgs;

    ensureWebCoreInitialized();

    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    page->settings().setScriptEnabled(false);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif

    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame) return kErrNoMainFrame;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view) return kErrNoView;

    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(false);

    Ref<FrameLoader> loader = localMainFrame->loader();
    RefPtr<DocumentLoader> activeLoader = loader->activeDocumentLoader();
    if (!activeLoader) return kErrNoLoader;

    DocumentWriter& writer = activeLoader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin(URL());
    {
        const size_t len = std::strlen(utf8Html);
        Ref<SharedBuffer> buffer = SharedBuffer::create(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(utf8Html), len));
        writer.addData(buffer.get());
    }
    writer.end();

    const IntSize size(w, h);
    view->resize(size);

    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document) return kErrNoDocument;

    document->updateLayoutIgnorePendingStylesheets();
    extractLinks(document.get(), h);

    int nonWhite = 0;
    int rc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
    if (rc != kOK) return rc;
    writeDiag(*document, *view, w, h, nonWhite);
    return kOK;
}

// ============================================================================
// Persistent session lifecycle
// ============================================================================

extern "C" int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA)
{
    if (!url || !outRGBA || w <= 0 || h <= 0) return kErrBadArgs;
    ensureWebCoreInitialized();
    if (g_inPump) return kErrBusy;

    teardownSession();
    g_lastNetError[0] = '\0';
    g_spaProbe[0] = '\0';
    g_lastPendingResources = 0;
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;
    g_session.emplace();
    g_session->w = w;
    g_session->h = h;

    int rc = buildSession(url, w, h, outRGBA);
    if (rc != kOK) teardownSession();
    return rc;
}

extern "C" void WebCoreCloseSession()
{
    teardownSession();
}

// ============================================================================
// Interaction: click, scroll, type, keys
// ============================================================================

extern "C" int WebCoreClickAt(int x, int y, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;
    if (g_inPump) return kErrBusy;
    PumpGuard guard;
    g_inPump = true;

    auto* lf = g_session->mainFrame.get();
    if (!lf || !lf->document()) return kErrFrameGone;

    IntPoint p(x, y);
    auto timestamp = MonotonicTime::now();
    OptionSet<PlatformEvent::Modifier> mods;

    // Mouse move → hover
    PlatformMouseEvent move(p, p, MouseButton::None, MouseMovedEvent, 0, mods, timestamp);
    lf->eventHandler().handleMouseMoveEvent(move);

    // Mouse down
    PlatformMouseEvent down(p, p, MouseButton::Left, MousePressedEvent, 1, mods, timestamp);
    lf->eventHandler().handleMousePressEvent(down);

    // Mouse up → triggers DOM click + default action
    PlatformMouseEvent up(p, p, MouseButton::Left, MouseReleasedEvent, 1, mods, timestamp);
    lf->eventHandler().handleMouseReleaseEvent(up);

    // Focus editable element if hit
    if (auto* doc = lf->document()) {
        if (auto* el = doc->elementFromPoint(p)) {
            if (is<HTMLInputElement>(*el) || is<HTMLTextAreaElement>(*el)
                || downcast<HTMLElement>(*el).isContentEditable()) {
                el->focus();
                if (auto* input = dynamicDowncast<HTMLInputElement>(*el))
                    input->select();
            }
        }
    }

    // Settle + render
    RefPtr<LocalFrameView> view = lf->view();
    if (!view) return kErrNoView;

    RefPtr<Document> doc = lf->protectedDocument();
    if (!doc) return kErrNoDocument;

    bool loadDone = false;
    auto onDone = [&loadDone](bool) { loadDone = true; };
    // Note: completion handler was set at session build; we just pump
    pumpLoop(*lf, nullptr, true, 8, 10.0, g_session->page.get());

    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);

    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    // Update title/URL
    snprintf(g_lastTitle, sizeof(g_lastTitle), "%s", doc->title().utf8().data());
    snprintf(g_lastUrl, sizeof(g_lastUrl), "%s", doc->url().string().utf8().data());

    return kOK;
}

extern "C" int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;
    if (g_inPump) return kErrBusy;

    auto* lf = g_session->mainFrame.get();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    if (!view) return kErrNoView;

    ScrollPosition cur = view->scrollPosition();
    ScrollPosition max = view->maximumScrollPosition();
    int nx = std::max(0, std::min(cur.x() + dx, max.x()));
    int ny = std::max(0, std::min(cur.y() + dy, max.y()));
    view->setScrollPosition(ScrollPosition(nx, ny));

    if (g_gpuActive) g_gpuScrollFast = true;

    // Drive IntersectionObserver / lazy-load
    if (g_session->page)
        g_session->page->isolatedUpdateRendering();

    // Render
    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    return kOK;
}

extern "C" int WebCoreSyncLinks()
{
    if (!g_session || !g_session->mainFrame) return kErrNoSession;
    auto* lf = g_session->mainFrame.get();
    RefPtr<Document> doc = lf ? lf->protectedDocument() : nullptr;
    if (!doc) return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);
    return kOK;
}

extern "C" int WebCoreTypeText(const char* utf8, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;
    if (g_inPump) return kErrBusy;
    if (!utf8 || !*utf8) return kErrBadArgs;

    auto* lf = g_session->mainFrame.get();
    auto* doc = lf ? lf->document() : nullptr;
    if (!doc) return kErrNoDocument;

    // Try Editor::insertTextWithoutSendingTextEvent (works for focused inputs)
    bool inserted = false;
    if (auto* focused = doc->focusedElement()) {
        if (auto* input = dynamicDowncast<HTMLInputElement>(*focused)) {
            input->select(); // ensure canEdit
            inserted = true;
        } else if (auto* textarea = dynamicDowncast<HTMLTextAreaElement>(*focused)) {
            inserted = true;
        } else if (downcast<HTMLElement>(*focused).isContentEditable()) {
            inserted = true;
        }
    }

    WTF::String text = WTF::String::fromUTF8(utf8);
    if (inserted && lf->editor().canEdit()) {
        lf->editor().insertTextWithoutSendingTextEvent(text);
    } else {
        // Fallback: synthetic keyboard events
        PlatformKeyboardEvent::Type types[] = {
            PlatformKeyboardEvent::RawKeyDown,
            PlatformKeyboardEvent::Char,
            PlatformKeyboardEvent::KeyUp
        };
        for (auto type : types) {
            PlatformKeyboardEvent evt(type, text, "", OptionSet<PlatformEvent::Modifier>(),
                                      timeIntervalSinceProcessStartup());
            lf->eventHandler().keyEvent(evt);
        }
    }

    // Render updated state
    doc->updateLayoutIgnorePendingStylesheets();
    RefPtr<LocalFrameView> view = lf->view();
    if (!view) return kErrNoView;

    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    return kOK;
}

extern "C" int WebCoreKeyAction(int action, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;
    if (g_inPump) return kErrBusy;

    auto* lf = g_session->mainFrame.get();
    auto* doc = lf ? lf->document() : nullptr;
    if (!doc) return kErrNoDocument;

    if (action == 0) {
        // Backspace
        if (lf->editor().canEdit())
            lf->editor().command("DeleteBackward").execute();
    } else if (action == 1) {
        // Enter — real keyboard event for form submit
        WTF::String cr("\r");
        PlatformKeyboardEvent::Type types[] = {
            PlatformKeyboardEvent::RawKeyDown,
            PlatformKeyboardEvent::Char,
            PlatformKeyboardEvent::KeyUp
        };
        for (auto type : types) {
            PlatformKeyboardEvent evt(type, cr, "", OptionSet<PlatformEvent::Modifier>(),
                                      timeIntervalSinceProcessStartup());
            lf->eventHandler().keyEvent(evt);
        }
    }

    // Settle + render
    PumpGuard guard;
    g_inPump = true;
    pumpLoop(*lf, nullptr, true, 4, 5.0, g_session->page.get());

    doc->updateLayoutIgnorePendingStylesheets();
    RefPtr<LocalFrameView> view = lf->view();
    if (!view) return kErrNoView;

    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    snprintf(g_lastTitle, sizeof(g_lastTitle), "%s", doc->title().utf8().data());
    snprintf(g_lastUrl, sizeof(g_lastUrl), "%s", doc->url().string().utf8().data());

    return kOK;
}

extern "C" int WebCoreFocusedEditable()
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return 0;
    auto* doc = g_session->mainFrame->document();
    if (!doc) return 0;
    auto* focused = doc->focusedElement();
    if (!focused) return 0;
    if (is<HTMLInputElement>(*focused) || is<HTMLTextAreaElement>(*focused))
        return 1;
    if (downcast<HTMLElement>(*focused).isContentEditable())
        return 1;
    if (g_session->mainFrame->editor().canEdit())
        return 1;
    return 0;
}

// ============================================================================
// Page scale (pinch zoom)
// ============================================================================

extern "C" int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;

    auto* lf = g_session->mainFrame.get();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    if (!view) return kErrNoView;

    // Clamp
    scale = std::max(0.5f, std::min(6.0f, scale));

    // Compute new scroll to keep focal point stable
    ScrollPosition cur = view->scrollPosition();
    float ratio = scale / g_session->page->pageScaleFactor();
    int nx = static_cast<int>(cur.x() * ratio + focalX * (1.0f - ratio));
    int ny = static_cast<int>(cur.y() * ratio + focalY * (1.0f - ratio));

    g_session->page->setPageScaleFactor(scale, IntPoint(nx, ny));

    // Settle + render
    PumpGuard guard;
    g_inPump = true;
    pumpLoop(*lf, nullptr, true, 4, 5.0, g_session->page.get());

    RefPtr<Document> doc = lf->protectedDocument();
    if (doc) doc->updateLayoutIgnorePendingStylesheets();

    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    return kOK;
}

// ============================================================================
// Live tick (animation frame advance)
// ============================================================================

extern "C" int WebCoreLiveTick(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) return kErrNoSession;

    auto* lf = g_session->mainFrame.get();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    if (!view) return kErrNoView;

    // Advance animations + microtasks
    if (g_session->page) {
        g_session->page->isolatedUpdateRendering();
        if (auto* doc = lf->document())
            doc->eventLoop().performMicrotaskCheckpoint();
    }

    // Check if frame is still alive
    RefPtr<Document> doc = lf->document();
    if (!doc) return kErrFrameGone;

    doc->updateLayoutIgnorePendingStylesheets();

    // Hash for idle detection
    g_lastPendingResources = 0;
    if (doc) {
        for (auto& kv : doc->cachedResourceLoader().allCachedResources()) {
            CachedResource* res = kv.value.get();
            if (!res) continue;
            auto status = res->status();
            if (status == CachedResource::Unknown || status == CachedResource::Pending)
                ++g_lastPendingResources;
        }
    }

    int nonWhite = 0;
    if (g_gpuActive && g_session->chrome && g_session->chrome->rootLayer()) {
        if (g_gpuPresentMode)
            gpuPresent(*view, g_session->w, g_session->h, *g_session->chrome->rootLayer());
        else
            gpuCompositeReadback(*view, g_session->w, g_session->h,
                                 *g_session->chrome->rootLayer(), outRGBA, nonWhite);
    } else {
        int rc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
        if (rc != kOK) return rc;
    }

    return kOK;
}

// ============================================================================
// Metadata / diagnostics
// ============================================================================

extern "C" int WebCoreGetTitle(char* buf, int len)
{
    if (!buf || len <= 0) return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastTitle);
    return n < 0 ? 0 : n;
}

extern "C" int WebCoreGetUrl(char* buf, int len)
{
    if (!buf || len <= 0) return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastUrl);
    return n < 0 ? 0 : n;
}

extern "C" int WebCoreGetLinkCount()
{
    return static_cast<int>(g_links.size());
}

extern "C" int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len)
{
    if (i < 0 || i >= static_cast<int>(g_links.size())) return 0;
    const LinkRect& lr = g_links[i];
    if (x) *x = lr.x;
    if (y) *y = lr.y;
    if (w) *w = lr.w;
    if (h) *h = lr.h;
    if (url && len > 0)
        std::snprintf(url, static_cast<size_t>(len), "%s", lr.url.c_str());
    return 1;
}

extern "C" uint32_t WebCoreGetFrameHash()
{
    return g_lastFrameHash;
}

extern "C" int WebCoreGetPendingResourceCount()
{
    return g_lastPendingResources;
}

extern "C" int WebCoreGetLastError(char* buf, int len)
{
    if (!buf || len <= 0) return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastNetError);
    return n < 0 ? 0 : n;
}

extern "C" int WebCoreGetDiag(char* buf, int len)
{
    if (!buf || len <= 0) return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastDiag);
    return n < 0 ? 0 : n;
}

extern "C" int WebCoreEditDebug(char* out, int cap)
{
    if (!out || cap <= 0) return 0;
    int n = std::snprintf(out, static_cast<size_t>(cap), "%s", g_imeDiag.c_str());
    return n < 0 ? 0 : n;
}

// ============================================================================
// Find in page
// ============================================================================

extern "C" int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outBuf)
{
    using namespace WebCore;
    if (!g_session || !g_session->page) return kErrNoSession;
    if (!utf8 || !*utf8) {
        // Empty → clear highlights
        g_session->page->unmarkAllTextMatches();
        return 0;
    }

    g_findText = String::fromUTF8(utf8);
    g_findOpts = { FindOption::CaseInsensitive };
    if (matchCase) g_findOpts.remove(FindOption::CaseInsensitive);
    if (wrap) g_findOpts.add(FindOption::WrapAround);

    g_session->page->markAllMatchesForText(g_findText, g_findOpts, true, 1000);
    auto data = g_session->page->findString(g_findText, g_findOpts);
    if (data.range)
        g_session->page->revealCurrentSelection();

    // Render with highlights
    if (outBuf && g_session->mainFrame) {
        auto* view = g_session->mainFrame->view();
        if (view) {
            int nonWhite = 0;
            paintToRGBA(*view, g_session->w, g_session->h, outBuf, nonWhite);
        }
    }

    return data.count > 0 ? data.count : 0;
}

extern "C" int WebCoreFindNext(int forward, uint8_t* outBuf)
{
    using namespace WebCore;
    if (!g_session || !g_session->page) return kErrNoSession;
    if (g_findText.isEmpty()) return 0;

    auto opts = g_findOpts;
    if (!forward) opts.add(FindOption::Backwards);

    auto data = g_session->page->findString(g_findText, opts);
    if (data.range)
        g_session->page->revealCurrentSelection();

    if (outBuf && g_session->mainFrame) {
        auto* view = g_session->mainFrame->view();
        if (view) {
            int nonWhite = 0;
            paintToRGBA(*view, g_session->w, g_session->h, outBuf, nonWhite);
        }
    }

    return data.count > 0 ? 1 : 0;
}

extern "C" int WebCoreFindClear(uint8_t* outBuf)
{
    using namespace WebCore;
    if (!g_session || !g_session->page) return kErrNoSession;
    g_session->page->unmarkAllTextMatches();

    if (outBuf && g_session->mainFrame) {
        auto* view = g_session->mainFrame->view();
        if (view) {
            int nonWhite = 0;
            paintToRGBA(*view, g_session->w, g_session->h, outBuf, nonWhite);
        }
    }
    return kOK;
}

// ============================================================================
// Downloads (standalone curl, no render)
// ============================================================================

static size_t downloadWrite(void* ptr, size_t size, size_t nmemb, void* stream)
{
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
}

extern "C" int WebCoreDownload(const char* url, const char* outPath)
{
    if (!url || !*url || !outPath || !*outPath) return -1;

    std::string partPath = std::string(outPath) + ".part";
    FILE* fp = nullptr;
    if (fopen_s(&fp, partPath.c_str(), "wb") != 0 || !fp) return -2;

    CURL* h = curl_easy_init();
    if (!h) { std::fclose(fp); std::remove(partPath.c_str()); return -3; }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, downloadWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(h, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/16.4 Safari/605.1.15");

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

    if (rc != CURLE_OK) { std::remove(partPath.c_str()); return -100 - static_cast<int>(rc); }
    if (code < 200 || code >= 400) { std::remove(partPath.c_str()); return static_cast<int>(code); }

    std::remove(outPath);
    if (std::rename(partPath.c_str(), outPath) != 0) { std::remove(partPath.c_str()); return -4; }
    return static_cast<int>(code);
}

// ============================================================================
// User agent
// ============================================================================

extern "C" void WebCoreSetUserAgentMobile(int mobile)
{
    g_apoUaMobile = (mobile != 0);
}

extern "C" void WebCoreSetUserAgentString(const char* ua)
{
    if (ua) {
        std::strncpy(g_apoCustomUA, ua, sizeof(g_apoCustomUA) - 1);
        g_apoCustomUA[sizeof(g_apoCustomUA) - 1] = '\0';
    } else {
        g_apoCustomUA[0] = '\0';
    }
}

// ============================================================================
// GPU compositing
// ============================================================================

extern "C" int WebCoreEnableCompositing()
{
    if (!g_session || !g_session->chrome) return 0;
    return g_session->chrome->rootLayer() ? 1 : 0;
}

extern "C" int WebCoreGpuInit(void* nativeWindow, int w, int h)
{
    using namespace WebCore;
    if (g_gpuActive) return 0; // already initialized

    // Create GL context via ANGLE
    auto& display = PlatformDisplay::sharedDisplay();
    if (nativeWindow)
        g_glContext = GLContext::create(display, static_cast<EGLNativeWindowType>(nativeWindow));
    else
        g_glContext = GLContext::createOffscreen(display);

    if (!g_glContext) return -1;
    g_glContext->makeContextCurrent();

    // Create TextureMapper
    g_textureMapper = TextureMapper::create().ptr();
    if (!g_textureMapper) return -2;

    g_gpuW = w;
    g_gpuH = h;
    g_gpuActive = true;
    g_gpuPresentMode = (nativeWindow != nullptr);

    return kOK;
}

extern "C" int WebCoreGpuLayerInfo(char* outBuf, int len)
{
    if (!outBuf || len <= 0) return 0;
    if (!g_session || !g_session->mainFrame) {
        std::snprintf(outBuf, static_cast<size_t>(len), "no session");
        return kOK;
    }
    auto* lf = g_session->mainFrame.get();
    auto* view = lf->view();
    auto* doc = lf->document();
    if (!view || !doc) {
        std::snprintf(outBuf, static_cast<size_t>(len), "no view/doc");
        return kOK;
    }

    ScrollPosition sp = view->scrollPosition();
    IntSize cs = view->contentsSize();
    float scale = g_session->page ? g_session->page->pageScaleFactor() : 1.0f;
    Color bg = view->documentBackgroundColor();

    int n = std::snprintf(outBuf, static_cast<size_t>(len),
        "scrollPos=%d,%d contents=%dx%d scale=%.2f bg=#%06x compositing=%d gpu=%d",
        sp.x(), sp.y(), cs.width(), cs.height(), scale,
        bg.isValid() ? bg.rgb() : 0, WebCoreEnableCompositing(), g_gpuActive ? 1 : 0);

    return n > 0 ? n : 0;
}
