// ============================================================================
// LoadingFrameLoaderClient.cpp
//
// Implementation of LoadingFrameLoaderClient: a verbatim copy of WebCore's
// EmptyFrameLoaderClient (Source/WebCore/loader/EmptyClients.cpp) with only the
// handful of behavioral changes required to let a real network navigation
// proceed (see LoadingFrameLoaderClient.h header comment for the diff).
//
// Build with the same clang-cl flags as a WebCore TU (config.h first); validate
// with port/compile-driver.ps1.
// ============================================================================

#include "config.h"
#include "LoadingFrameLoaderClient.h"

// Mirror the EmptyClients.cpp include set needed by the FrameLoaderClient bodies.
#include <WebCore/DocumentLoader.h>
#include <WebCore/FrameLoaderClient.h>            // FramePolicyFunction, PolicyAction
#include <WebCore/FrameLoaderTypes.h>             // PolicyAction enum
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/HistoryItem.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/NetworkStorageSession.h>
#include "PortNetworkStorageSession.h"   // cookie persistence: a real storageSession
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Unexpected.h>
#include <wtf/text/CString.h>

#include <cstdarg>
#include <cstdio>

// Apotheosis: diagnostic channel, defined in WebCoreDriver.cpp. Passes the failed ResourceError details
// (curl error code + domain + description + failed URL) to the driver, for on-device network-failure triage.
extern "C" void WebCorePortRecordNetError(const char* phase, const char* type, int code, const char* domain, const char* desc, const char* url);

// Apotheosis 2026-09-18 (ST-6): the navigation lifecycle channel, and why it is these callbacks
// rather than a WebCore patch.
//
// The question this build exists to answer: when the 2883-byte JS redirect page sets `location`,
// does a DocumentLoader get a response at all? Nothing recorded it. The loader trace shows requests
// but cannot say which of them are navigations -- PortLoaderStrategy::loadResource is the single
// hook for *every* CachedResource, the main one included, so a navigation and an <img> look alike
// there -- and the failure dispatches below were reporting a ResourceError with its most
// informative field dropped.
//
// The error's *type* is that field. `lasterr=[curlcode=0 domain= desc= url=]` was read as "no error,
// so no request was made"; it is in fact a ResourceError of type Null: non-null enough to be
// dispatched as a failure, and empty because the port's own LoaderStrategy error factories return
// `{ }`. "curl failed" and "the load was refused before it ever left" are different faults that
// produce the same three empty strings, and only the type separates them.
//
// These callbacks are the right hook because they are the engine's own, so a navigation the *page*
// starts is covered without an upstream WebCore edit and without a WebCore rebuild -- which matters,
// because the ARM32 tree takes hours to rebuild and this diagnostic must be portable to it as-is.
//
// Volume is safe: the port creates exactly one frame (createFrame() below returns nullptr), so there
// are no subframe navigations to multiply these lines, and the will-send hook is filtered to
// main-resource requests, which is what keeps it from echoing every image on the page. Writes go
// through portDiagLog -> gpuinit-steps.txt, one open/write/flush/close per line: the channel that has
// survived every crash and hang here. Declared the same way PortPlatformStrategies.cpp declares it,
// rather than by including PortChromeClient.h for the sake of one function.
namespace WebCorePort { void portDiagLog(const char* utf8); }

namespace {

void navTrace(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    WebCorePort::portDiagLog(buf);
}

// ResourceErrorBaseType as a word. Null is the one that matters here: it is what this port's error
// factories return, and it is why an empty lasterr= must not be read as "nothing went wrong".
const char* errorTypeName(WebCore::ResourceError::Type type)
{
    switch (type) {
    case WebCore::ResourceError::Type::Null: return "Null";
    case WebCore::ResourceError::Type::General: return "General";
    case WebCore::ResourceError::Type::AccessControl: return "AccessControl";
    case WebCore::ResourceError::Type::Cancellation: return "Cancellation";
    case WebCore::ResourceError::Type::Timeout: return "Timeout";
    }
    return "?";
}

} // anonymous namespace

namespace WebCorePort {

using namespace WebCore;

// Forward a failed load's ResourceError into the driver's diagnostic channel, and into the marker
// channel with the phase and the type spelled out.
//
// `phase` is not decoration. g_lastNetError holds only the *last* failure of the session and
// dispatchDidFailLoading fires for subresources too, so a failed favicon can overwrite the
// navigation error under investigation -- and nothing in the old string said which had happened.
static void recordNetError(const char* phase, const ResourceError& error)
{
    auto domain = error.domain().utf8();
    auto desc = error.localizedDescription().utf8();
    auto url = error.failingURL().string().utf8();
    const char* type = errorTypeName(error.type());
    navTrace("NAV: FAIL phase=%s type=%s code=%d domain=%.32s url=%.140s",
        phase, type, error.errorCode(),
        domain.data() ? domain.data() : "", url.data() ? url.data() : "");
    WebCorePortRecordNetError(phase, type, error.errorCode(), domain.data(), desc.data(), url.data());
}

// ---------------------------------------------------------------------------
// Networking context. EmptyClients.cpp defines an EmptyFrameNetworkingContext
// whose storageSession() returns nullptr; we reuse the same shape. (cookie /
// credential support is a later milestone — see report "STILL MISSING".)
// ---------------------------------------------------------------------------
namespace {

class LoadingFrameNetworkingContext : public FrameNetworkingContext {
public:
    static Ref<LoadingFrameNetworkingContext> create() { return adoptRef(*new LoadingFrameNetworkingContext); }

private:
    LoadingFrameNetworkingContext()
        : FrameNetworkingContext { nullptr }
    { }

    bool shouldClearReferrerOnHTTPSToHTTPRedirect() const final { return true; }
    NetworkStorageSession* storageSession() const final { return nullptr; }

    // Pure-virtual on PLATFORM(WIN): a generic blocked-load error is fine for the
    // headless render path (we never actually block requests here).
    ResourceError blockedError(const ResourceRequest& request) const final
    {
        return ResourceError("WebKitInternal"_s, 0, request.url(), "Request blocked"_s, ResourceError::Type::Cancellation);
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// BEHAVIORAL CHANGES (vs EmptyFrameLoaderClient)
// ---------------------------------------------------------------------------

// Approve the navigation so the provisional load actually starts.
void LoadingFrameLoaderClient::dispatchDecidePolicyForNavigationAction(const NavigationAction&, const ResourceRequest& request, const ResourceResponse&, FormState*, const String&, std::optional<NavigationIdentifier>, std::optional<HitTestResult>&&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode, FramePolicyFunction&& policyFunction)
{
    // Apotheosis 2026-09-18 (ST-6): the first hook a page-initiated navigation reaches, ahead of any
    // DocumentLoader. It is what separates "location was set and the policy was asked" from "location
    // was set and nothing happened at all" -- two states that both end in a blank page and were
    // previously indistinguishable. This port always answers Use, so the line is pure observation.
    CString navUrl = request.url().string().utf8();
    navTrace("NAV: policy navigation url=%.140s", navUrl.data() ? navUrl.data() : "(null)");
    policyFunction(PolicyAction::Use);
}

// Approve the response so the body is committed to the DocumentLoader.
void LoadingFrameLoaderClient::dispatchDecidePolicyForResponse(const ResourceResponse& response, const ResourceRequest& request, const String&, FramePolicyFunction&& policyFunction)
{
    // Apotheosis 2026-09-18 (ST-6): a main-resource load can be ended here -- turned into a download,
    // refused as unshowable -- and a navigation that dies at this gate has *received a response*, so
    // it must not be confused with one whose request never went out. Low volume: this fires once per
    // response that reaches a policy check on a frame this port has (there is only one frame). The
    // status code and MIME type are recorded because "200 text/html" and "200 application/octet-stream"
    // lead to opposite decisions and look identical in every other artifact.
    CString mimeType = response.mimeType().utf8();
    CString policyUrl = request.url().string().utf8();
    navTrace("NAV: policy response status=%d mime=%.48s url=%.100s",
        response.httpStatusCode(), mimeType.data() ? mimeType.data() : "",
        policyUrl.data() ? policyUrl.data() : "");
    policyFunction(PolicyAction::Use);
}

// We have no UI to open new windows in a headless render; ignore the request
// (but still complete the policy check so the loader is not left hanging).
void LoadingFrameLoaderClient::dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest&, FormState*, const String&, std::optional<HitTestResult>&&, FramePolicyFunction&& policyFunction)
{
    policyFunction(PolicyAction::Ignore);
}

bool LoadingFrameLoaderClient::canHandleRequest(const ResourceRequest&) const
{
    return true;
}

bool LoadingFrameLoaderClient::canShowMIMEType(const String&) const
{
    return true;
}

bool LoadingFrameLoaderClient::canShowMIMETypeAsHTML(const String&) const
{
    return true;
}

// Same as Empty: just construct the DocumentLoader.
Ref<DocumentLoader> LoadingFrameLoaderClient::createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData)
{
    // Apotheosis 2026-09-18 (ST-6): the earliest point at which a navigation the *page* started is
    // observable. A page-initiated navigation and a harness-initiated one both land here, which is
    // the point: the harness path feeds bytes through DocumentWriter by hand and never creates a
    // DocumentLoader for its own navigation, so every "NAV: docloader create" after the first is a
    // navigation the engine started on its own. Read before the move -- this is the navigation's URL.
    CString urlForNav = request.url().string().utf8();
    navTrace("NAV: docloader create url=%.140s substitute=%d",
        urlForNav.data() ? urlForNav.data() : "(null)", substituteData.isValid() ? 1 : 0);
    return DocumentLoader::create(WTF::move(request), WTF::move(substituteData));
}

// Load-completion: set the poll flags AND fire the optional completion handler
// (exactly once) so the driver can either poll or run/stop the RunLoop.
void LoadingFrameLoaderClient::signalLoadComplete(bool failed)
{
    if (m_loadFinished)
        return;
    m_loadFinished = true;
    m_loadFailed = failed;
    if (m_loadCompletionHandler) {
        // Move out before invoking so a re-entrant terminal dispatch is a no-op.
        auto handler = WTF::move(m_loadCompletionHandler);
        handler(failed);
    }
}

void LoadingFrameLoaderClient::dispatchDidFinishLoad()
{
    navTrace("NAV: finish load");
    signalLoadComplete(false);
}

void LoadingFrameLoaderClient::dispatchDidFailLoad(const ResourceError& error)
{
    recordNetError("didFailLoad", error);
    signalLoadComplete(true);
}

void LoadingFrameLoaderClient::dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading willContinue, WillInternallyHandleFailure)
{
    recordNetError("didFailProvisionalLoad", error);
    // Apotheosis: a server redirect (especially http->https 301, cross-scheme = origin change) fails the
    //   current provisional load as a "cancellation" (Type::Cancellation), but immediately starts a new
    //   provisional load (WillContinueLoading::Yes). We used to always signalLoadComplete(true), which
    //   misread this "will continue" intermediate cancel as a load failure, so gov.cn-style 301->https
    //   sites falsely reported -10. So: never terminate on will-continue / cancel; wait for the real
    //   didFinishLoad or a real network error (curl non-cancel errors still terminate immediately to show
    //   the error page); a genuine hang is caught by pumpLoop's watchdog.
    if (willContinue == WillContinueLoading::Yes || error.isCancellation())
        return;
    signalLoadComplete(true);
}

// This is not an Empty client.
bool LoadingFrameLoaderClient::isEmptyFrameLoaderClient() const
{
    return false;
}

// ---------------------------------------------------------------------------
// Everything below is the verbatim EmptyFrameLoaderClient no-op behavior.
// ---------------------------------------------------------------------------

RefPtr<LocalFrame> LoadingFrameLoaderClient::createFrame(const AtomString&, HTMLFrameOwnerElement&)
{
    return nullptr;
}

RefPtr<Widget> LoadingFrameLoaderClient::createPlugin(HTMLPlugInElement&, const URL&, const Vector<AtomString>&, const Vector<AtomString>&, const String&, bool)
{
    return nullptr;
}

bool LoadingFrameLoaderClient::hasWebView() const
{
    return true; // mainly for assertions
}

void LoadingFrameLoaderClient::makeRepresentation(DocumentLoader*)
{
}

#if PLATFORM(IOS_FAMILY)

bool LoadingFrameLoaderClient::forceLayoutOnRestoreFromBackForwardCache()
{
    return false;
}

#endif

void LoadingFrameLoaderClient::forceLayoutForNonHTML()
{
}

void LoadingFrameLoaderClient::setCopiesOnScroll()
{
}

void LoadingFrameLoaderClient::detachedFromParent2()
{
}

void LoadingFrameLoaderClient::detachedFromParent3()
{
}

void LoadingFrameLoaderClient::convertMainResourceLoadToDownload(DocumentLoader*, const ResourceRequest&, const ResourceResponse&)
{
}

void LoadingFrameLoaderClient::assignIdentifierToInitialRequest(ResourceLoaderIdentifier, DocumentLoader*, const ResourceRequest&)
{
}

bool LoadingFrameLoaderClient::shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier)
{
    return false;
}

void LoadingFrameLoaderClient::dispatchWillSendRequest(DocumentLoader*, ResourceLoaderIdentifier, ResourceRequest& request, const ResourceResponse& redirectResponse)
{
    // Apotheosis 2026-09-18 (ST-6): filtered to main-resource requests on purpose. This hook is
    // reached for every request the frame issues, and a 724-resource page would bury the marker
    // channel in it. ResourceRequestRequester is set to Main in DocumentLoader::startLoadingMainResource
    // before loadMainResource() runs, so the filter is exactly "this is a navigation".
    if (request.requester() != ResourceRequestRequester::Main)
        return;
    CString u = request.url().string().utf8();
    navTrace("NAV: main request url=%.140s redirect=%d",
        u.data() ? u.data() : "(null)", redirectResponse.isNull() ? 0 : 1);
}

void LoadingFrameLoaderClient::dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge&)
{
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)

bool LoadingFrameLoaderClient::canAuthenticateAgainstProtectionSpace(DocumentLoader*, ResourceLoaderIdentifier, const ProtectionSpace&)
{
    return false;
}

#endif

#if PLATFORM(IOS_FAMILY)

RetainPtr<CFDictionaryRef> LoadingFrameLoaderClient::connectionProperties(DocumentLoader*, ResourceLoaderIdentifier)
{
    return nullptr;
}

#endif

void LoadingFrameLoaderClient::dispatchDidReceiveResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse&)
{
}

void LoadingFrameLoaderClient::dispatchDidReceiveContentLength(DocumentLoader*, ResourceLoaderIdentifier, int)
{
}

void LoadingFrameLoaderClient::dispatchDidFinishLoading(DocumentLoader*, ResourceLoaderIdentifier)
{
}

#if ENABLE(DATA_DETECTION)

void LoadingFrameLoaderClient::dispatchDidFinishDataDetection(NSArray *)
{
}

#endif

void LoadingFrameLoaderClient::dispatchDidFailLoading(DocumentLoader*, ResourceLoaderIdentifier, const ResourceError& error)
{
    recordNetError("didFailLoading", error);
}

bool LoadingFrameLoaderClient::dispatchDidLoadResourceFromMemoryCache(DocumentLoader*, const ResourceRequest&, const ResourceResponse&, int)
{
    return false;
}

void LoadingFrameLoaderClient::dispatchDidDispatchOnloadEvents()
{
}

void LoadingFrameLoaderClient::dispatchDidReceiveServerRedirectForProvisionalLoad()
{
    navTrace("NAV: provisional redirect");
}

void LoadingFrameLoaderClient::dispatchDidCancelClientRedirect()
{
}

void LoadingFrameLoaderClient::dispatchWillPerformClientRedirect(const URL&, double, WallTime, LockBackForwardList)
{
}

void LoadingFrameLoaderClient::dispatchDidChangeLocationWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidPushStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidReplaceStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidPopStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchWillClose()
{
}

void LoadingFrameLoaderClient::dispatchDidStartProvisionalLoad()
{
    navTrace("NAV: start provisional");
}

void LoadingFrameLoaderClient::dispatchDidReceiveTitle(const StringWithDirection&)
{
}

void LoadingFrameLoaderClient::dispatchDidCommitLoad(std::optional<HasInsecureContent>, std::optional<UsedLegacyTLS>, std::optional<WasPrivateRelayed>)
{
    // Apotheosis 2026-09-18 (ST-6): the decisive line of this whole instrument. A commit means the
    // main resource arrived, was accepted by the policy check and became the document. Reachable
    // outcomes for a page-initiated navigation, in order of how bad they are:
    //   create -> ... -> commit            the navigation worked
    //   create -> start -> (nothing)       the load was requested and never came back
    //   no create line at all              the navigation never started
    navTrace("NAV: COMMIT");
}

void LoadingFrameLoaderClient::dispatchDidFinishDocumentLoad()
{
    navTrace("NAV: finish document load");
}

void LoadingFrameLoaderClient::dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone>)
{
}

void LoadingFrameLoaderClient::dispatchDidReachVisuallyNonEmptyState()
{
}

LocalFrame* LoadingFrameLoaderClient::dispatchCreatePage(const NavigationAction&, NewFrameOpenerPolicy)
{
    return nullptr;
}

void LoadingFrameLoaderClient::dispatchShow()
{
}

void LoadingFrameLoaderClient::updateSandboxFlags(SandboxFlags)
{
}

void LoadingFrameLoaderClient::updateOpener(std::optional<FrameIdentifier>)
{
}

void LoadingFrameLoaderClient::setPrinting(bool, FloatSize, FloatSize, float, AdjustViewSize)
{
}

void LoadingFrameLoaderClient::cancelPolicyCheck()
{
    // Apotheosis 2026-09-19: WebCore calls this when a policy check that was already in flight is
    // ABANDONED -- FrameLoader::stopAllLoaders, or a new navigation replacing the pending one. It is
    // silent in EmptyFrameLoaderClient, and its silence is why a navigation that died between
    // `policy navigation` and `policy response` looked identical to one that was never asked about.
    // Pairs with the `teardown PROVISIONAL-IN-FLIGHT` mark in the driver: this line says the decision
    // was dropped, that one says who dropped it. See Doc/ENGINE-INITIATED-NAV-CANCEL.md.
    navTrace("NAV: cancelPolicyCheck -- a policy decision in flight was abandoned");
}

void LoadingFrameLoaderClient::dispatchUnableToImplementPolicy(const ResourceError&)
{
}

void LoadingFrameLoaderClient::dispatchWillSendSubmitEvent(Ref<FormState>&&)
{
}

void LoadingFrameLoaderClient::dispatchWillSubmitForm(FormState&, URL&&, String&&, CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

void LoadingFrameLoaderClient::revertToProvisionalState(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::setMainDocumentError(DocumentLoader*, const ResourceError&)
{
}

void LoadingFrameLoaderClient::setMainFrameDocumentReady(bool)
{
}

void LoadingFrameLoaderClient::startDownload(const ResourceRequest&, const String&, FromDownloadAttribute)
{
}

void LoadingFrameLoaderClient::willChangeTitle(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::didChangeTitle(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::willReplaceMultipartContent()
{
}

void LoadingFrameLoaderClient::didReplaceMultipartContent()
{
}

void LoadingFrameLoaderClient::committedLoad(DocumentLoader* loader, const SharedBuffer& data)
{
    // Apotheosis 2026-09-18 (ST-6): the byte count is the point of this line. A commit with 0 bytes
    // and a commit with 2883 bytes produce the same empty-looking document in the diag, and telling
    // those apart is half of what ST-6 is for.
    navTrace("NAV: committedLoad bytes=%zu", data.size());
    // Apotheosis: the Empty variant is a no-op -> the network response data is discarded and the document
    // stays blank (white screen). Committing the received data to the DocumentLoader, which drives the
    // DocumentWriter/parser, is what gives the document content to render.
    if (loader)
        loader->commitData(data);
}

void LoadingFrameLoaderClient::finishedLoading(DocumentLoader*)
{
    navTrace("NAV: finishedLoading");
}

bool LoadingFrameLoaderClient::shouldFallBack(const ResourceError&) const
{
    return false;
}

void LoadingFrameLoaderClient::loadStorageAccessQuirksIfNeeded()
{
}

bool LoadingFrameLoaderClient::representationExistsForURLScheme(StringView) const
{
    return false;
}

String LoadingFrameLoaderClient::generatedMIMETypeForURLScheme(StringView) const
{
    return emptyString();
}

void LoadingFrameLoaderClient::frameLoadCompleted()
{
}

void LoadingFrameLoaderClient::restoreViewState()
{
}

void LoadingFrameLoaderClient::provisionalLoadStarted()
{
    navTrace("NAV: provisional started");
}

void LoadingFrameLoaderClient::didFinishLoad()
{
}

void LoadingFrameLoaderClient::prepareForDataSourceReplacement()
{
}

void LoadingFrameLoaderClient::updateCachedDocumentLoader(DocumentLoader&)
{
}

void LoadingFrameLoaderClient::setTitle(const StringWithDirection&, const URL&)
{
}

extern "C" bool g_apoUaMobile;        // defined in WebCoreDriver.cpp; UI can toggle mobile/desktop, takes effect on reload
extern "C" char g_apoCustomUA[2048];  // defined in WebCoreDriver.cpp; if non-empty it overrides mobile/desktop (set via WebCoreSetUserAgentString)

String LoadingFrameLoaderClient::userAgent(const URL&) const
{
    // Apotheosis: custom UA wins (entered by the user in settings, to bypass UA-blocking sites such as microsoft).
    if (g_apoCustomUA[0])
        return String::fromUTF8(g_apoCustomUA);
    // Default mobile iPhone Safari UA -> sites serve the mobile layout (narrower/lighter, tuned for a ~390px viewport).
    if (g_apoUaMobile)
        return "Mozilla/5.0 (iPhone; CPU iPhone OS 16_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Mobile/15E148 Safari/604.1"_s;
    // Desktop: a current Edge/Chromium UA (legacy Safari-on-Windows strings get blocked by sites like microsoft).
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0"_s;
}

void LoadingFrameLoaderClient::savePlatformDataToCachedFrame(CachedFrame*)
{
}

void LoadingFrameLoaderClient::transitionToCommittedFromCachedFrame(CachedFrame*)
{
}

#if PLATFORM(IOS_FAMILY)

void LoadingFrameLoaderClient::didRestoreFrameHierarchyForCachedFrame()
{
}

#endif

void LoadingFrameLoaderClient::transitionToCommittedForNewPage(InitializingIframe)
{
}

void LoadingFrameLoaderClient::didRestoreFromBackForwardCache()
{
}

void LoadingFrameLoaderClient::updateGlobalHistory()
{
}

void LoadingFrameLoaderClient::updateGlobalHistoryRedirectLinks()
{
}

ShouldGoToHistoryItem LoadingFrameLoaderClient::shouldGoToHistoryItem(HistoryItem&, IsSameDocumentNavigation, ProcessSwapDisposition) const
{
    return ShouldGoToHistoryItem::No;
}

bool LoadingFrameLoaderClient::supportsAsyncShouldGoToHistoryItem() const
{
    return false;
}

void LoadingFrameLoaderClient::shouldGoToHistoryItemAsync(HistoryItem&, CompletionHandler<void(ShouldGoToHistoryItem)>&&) const
{
    RELEASE_ASSERT_NOT_REACHED();
}

void LoadingFrameLoaderClient::saveViewStateToItem(HistoryItem&)
{
}

bool LoadingFrameLoaderClient::canCachePage() const
{
    return false;
}

ObjectContentType LoadingFrameLoaderClient::objectContentType(const URL&, const String&)
{
    return ObjectContentType::None;
}

AtomString LoadingFrameLoaderClient::overrideMediaType() const
{
    return nullAtom();
}

void LoadingFrameLoaderClient::redirectDataToPlugin(Widget&)
{
}

void LoadingFrameLoaderClient::dispatchDidClearWindowObjectInWorld(DOMWrapperWorld&)
{
}

#if PLATFORM(COCOA)

RemoteAXObjectRef LoadingFrameLoaderClient::accessibilityRemoteObject()
{
    return nullptr;
}

IntPoint LoadingFrameLoaderClient::accessibilityRemoteFrameOffset()
{
    return { };
}

#if ENABLE(ACCESSIBILITY_ISOLATED_TREE)
void LoadingFrameLoaderClient::setIsolatedTree(Ref<WebCore::AXIsolatedTree>&&)
{
}

RefPtr<WebCore::AXIsolatedTree> LoadingFrameLoaderClient::isolatedTree() const
{
    return nullptr;
}
#endif

void LoadingFrameLoaderClient::willCacheResponse(DocumentLoader*, ResourceLoaderIdentifier, NSCachedURLResponse *response, CompletionHandler<void(NSCachedURLResponse *)>&& completionHandler) const
{
    completionHandler(response);
}

#endif

void LoadingFrameLoaderClient::prefetchDNS(const String&)
{
}

RefPtr<HistoryItem> LoadingFrameLoaderClient::createHistoryItemTree(bool, BackForwardItemIdentifier) const
{
    return nullptr;
}

#if USE(QUICK_LOOK)

RefPtr<LegacyPreviewLoaderClient> LoadingFrameLoaderClient::createPreviewLoaderClient(const String&, const String&)
{
    return nullptr;
}

#endif

bool LoadingFrameLoaderClient::hasFrameSpecificStorageAccess()
{
    return false;
}

void LoadingFrameLoaderClient::revokeFrameSpecificStorageAccess()
{
}

void LoadingFrameLoaderClient::dispatchLoadEventToOwnerElementInAnotherProcess()
{
}

Ref<FrameNetworkingContext> LoadingFrameLoaderClient::createNetworkingContext()
{
    // cookie persistence: return a networking context with a real storageSession (HTTP Cookie/Set-Cookie
    // headers reach the jar through it).
    // frame is passed as nullptr (same as the original LoadingFrameNetworkingContext; the networking
    // context only uses storageSession).
    return WebCorePort::makeFrameNetworkingContext(nullptr);
}

void LoadingFrameLoaderClient::sendH2Ping(const URL& url, CompletionHandler<void(Expected<Seconds, ResourceError>&&)>&& completionHandler)
{
    ASSERT_NOT_REACHED();
    completionHandler(makeUnexpected(internalError(url)));
}

} // namespace WebCorePort
