// PortNetworkStorageSession.h — process-default curl NetworkStorageSession (cookie persistence).
// Implemented by PortNetworkStorageSession.cpp. Both consumers share one jar:
//   HTTP (Cookie/Set-Cookie headers) via FrameNetworkingContext::storageSession();
//   DOM (document.cookie) via PageConfiguration.cookieJar's StorageSessionProvider.
#pragma once
#include <wtf/Ref.h>

namespace WebCore {
class NetworkStorageSession;
class StorageSessionProvider;
class FrameNetworkingContext;
class LocalFrame;
}

namespace WebCorePort {

// M1: point the process-default session at a file-backed cookie jar. Call before the first network
// use (the managed host does it first thing in SetupRuntimeEnv); an empty path keeps the ephemeral
// ":memory:" jar. If the session already exists, its database is swapped, so the call is not a
// silent no-op -- but cookies already held in the old jar are not carried over.
void setPortCookieJarPath(const char* utf8Path);

WebCore::NetworkStorageSession& defaultPortStorageSession();
void ensureDefaultPortStorageSession();                                         // prewarm (open jar + set accept policy)
WTF::Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider();         // for CookieJar::create (DOM path)
WTF::Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame*);  // for createNetworkingContext (HTTP path)

} // namespace WebCorePort
