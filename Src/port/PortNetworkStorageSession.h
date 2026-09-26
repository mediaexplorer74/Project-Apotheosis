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

WebCore::NetworkStorageSession& defaultPortStorageSession();
void ensureDefaultPortStorageSession();                                         // prewarm (open jar + set accept policy)
WTF::Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider();         // for CookieJar::create (DOM path)
WTF::Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame*);  // for createNetworkingContext (HTTP path)

} // namespace WebCorePort
