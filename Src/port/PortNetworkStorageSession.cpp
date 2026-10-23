// PortNetworkStorageSession.cpp — see .h. Activated from draft-PortNetworkStorageSession.cpp.
// Owns a process-default curl NetworkStorageSession (mimicking WebKitLegacy's
// NetworkStorageSessionMap::defaultStorageSession()) so cookies are actually read/written + persisted.
// On-disk path: NetworkStorageSessionCurl's defaultCookieJarPath() uses
// localUserSpecificStorageDirectory()+"cookie.jar.db" under PLATFORM(WIN), or the CURL_COOKIE_JAR_PATH
// env var. The former may be unwritable in the App Container, so at driver init we point
// CURL_COOKIE_JAR_PATH at LocalState.
#include "config.h"

#include "PortNetworkStorageSession.h"

#include <WebCore/NetworkStorageSession.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/LocalFrame.h>
// M1: the concrete jar, so the process-default session can stop being ephemeral.
#include <WebCore/CookieJarDB.h>
#include <pal/SessionID.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/MainThread.h>
#include <wtf/UniqueRef.h>
#include <wtf/text/WTFString.h>

#if PLATFORM(WIN)
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#endif

namespace WebCorePort {

using namespace WebCore;

namespace {

// M1: where the jar lives. Empty keeps the old ephemeral behaviour, and is the safe value until the
// managed host has a writable directory to offer: NetworkStorageSessionCurl's defaultCookieJarPath()
// lands in localUserSpecificStorageDirectory(), which the App Container refuses to write, and
// opening it was the 0.1.6.0 crash on any webpage.
WTF::String& portCookieJarPath()
{
    static NeverDestroyed<WTF::String> path;
    return path.get();
}

std::unique_ptr<WebCore::NetworkStorageSession>& portSessionSlot()
{
    static NeverDestroyed<std::unique_ptr<WebCore::NetworkStorageSession>> session;
    return session.get();
}

} // namespace

void setPortCookieJarPath(const char* utf8Path)
{
    portCookieJarPath() = utf8Path ? WTF::String::fromUTF8(utf8Path) : WTF::String();

    // The contract is "call before the first network use", and in the app that is exactly what
    // happens: SetupRuntimeEnv runs as the engine Loop's first job, before any page (and therefore
    // any session) exists. If a session does exist anyway, swap its database instead of accepting
    // the call and ignoring it -- silently keeping ":memory:" would make this export a lie that
    // only a restart could expose.
    if (auto& session = portSessionSlot())
        session->setCookieDatabase(makeUniqueRef<WebCore::CookieJarDB>(
            portCookieJarPath().isEmpty() ? ":memory:"_s : portCookieJarPath()));
}

// Single process-default curl session. Main thread (the WebKit main thread, pinned by the driver in
// ensureWebCoreInitialized).
WebCore::NetworkStorageSession& defaultPortStorageSession()
{
    ASSERT(isMainThread());
    auto& session = portSessionSlot();
    if (!session) {
        // Ephemeral session ID -> NetworkStorageSessionCurl.cpp builds a ":memory:" CookieJarDB by
        // default. That was deliberate while the port's own jar path was unwritable in the App
        // Container; it is replaced below the moment the host names a writable path, which is what
        // makes cookies survive a restart. Without a path the old, crash-free behaviour is kept.
        session = makeUnique<WebCore::NetworkStorageSession>(PAL::SessionID::generateEphemeralSessionID());
        if (!portCookieJarPath().isEmpty())
            session->setCookieDatabase(makeUniqueRef<WebCore::CookieJarDB>(portCookieJarPath()));
    }
    return *session;
}

// Apotheosis 2026-09-18: DEAD CODE, and this is worth a line because a plan was written on the
// assumption that it runs. This function is called from nowhere -- grep the tree: it appears only as
// its own declaration, this definition and one include comment. It therefore never calls
// setCookieAcceptPolicy, which has exactly one call site in the tree (the line below), so the policy
// actually in force is CookieJarDB's own default, `CookieAcceptPolicy::Always` (CookieJarDB.h:76) --
// the most permissive value there is, not the restrictive one the dzen.ru cookie note assumed.
// Changing the constant below changes nothing at runtime. The deeper reason is that the top-level
// document never reaches this jar at all: the driver fetches it itself, in ApoFetchChannel
// (WebCoreDriver.cpp), and feeds the bytes to DocumentWriter. Wiring this up is a separate decision,
// deliberately not taken here.
void ensureDefaultPortStorageSession()
{
    auto& s = defaultPortStorageSession();
    // By default only accept cookies from the main document domain (the usual port convention);
    // cookieDatabase() lazily opens the jar here.
    s.setCookieAcceptPolicy(CookieAcceptPolicy::OnlyFromMainDocumentDomain);
}

// DOM path: document.cookie reaches the real jar through this provider.
class PortStorageSessionProvider final : public WebCore::StorageSessionProvider {
public:
    static Ref<PortStorageSessionProvider> create() { return adoptRef(*new PortStorageSessionProvider); }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }
private:
    PortStorageSessionProvider() = default;
};

// HTTP path: ResourceHandle's curl bridge reaches the real jar via d->m_context->storageSession().
class PortFrameNetworkingContext final : public WebCore::FrameNetworkingContext {
public:
    static Ref<PortFrameNetworkingContext> create(WebCore::LocalFrame* frame)
    {
        return adoptRef(*new PortFrameNetworkingContext(frame));
    }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }

#if PLATFORM(WIN)
    WebCore::ResourceError blockedError(const WebCore::ResourceRequest&) const final { return { }; }
#endif

private:
    explicit PortFrameNetworkingContext(WebCore::LocalFrame* frame)
        : WebCore::FrameNetworkingContext(frame) { }
};

Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider()
{
    return PortStorageSessionProvider::create();
}

Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame* frame)
{
    return PortFrameNetworkingContext::create(frame);
}

} // namespace WebCorePort
