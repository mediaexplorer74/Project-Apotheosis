// ============================================================================
// PortWebSocket.cpp -- a SocketProvider that fails WebSocket connections
// cleanly, so a page that opens one cannot abort the process.
//
// Why it exists. pageConfigurationWithEmptyClients() (the only source of a
// PageConfiguration in this port) installs EmptySocketProvider, whose
// createWebSocketChannel() returns nullptr. WebCore::WebSocket::connect()
// RELEASE_ASSERTs on a null channel -- "Every ScriptExecutionContext should
// have a SocketProvider", Modules/websockets/WebSocket.cpp:288. dzen.ru opens
// a WebSocket (Yandex Metrika is in the trace immediately before the death),
// so on the device the assert fired and abort() killed the browser. The
// 0.2.3.28 stack reader named it: file WebSocket.cpp, function
// ExceptionOr<void> WebCore::WebSocket::connect(const String &, const
// Vector<String> &), line 288, counter 317. See Doc/BUILD-ARM32.md.
//
// What it does instead. The port has no WebSocket networking, so the channel
// here reports ConnectStatus::KO from connect(). WebSocket::connect() turns
// that into failAsynchronously() -- the page gets an ordinary error event and
// a closed socket, which is what a browser without WebSocket support should
// look like. Nothing is silently successful: the connection really does fail,
// it just fails observably.
//
// Everything runs on the engine thread, like PortStorage; no locks by design.
// ============================================================================

#include "config.h"

#include "PortWebSocket.h"

#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SocketProvider.h>
#include <WebCore/ThreadableWebSocketChannel.h>
#include <WebCore/WebTransportSession.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

#if USE(LIBRICE)
namespace WebCore {
class RiceBackend;
class RiceBackendClient;
}
#endif

namespace WebCorePort {

using namespace WebCore;

// A channel that never connects. Every mutator is a no-op; connect() is the only method whose answer
// matters, and KO is the one WebSocket::connect() handles by failing the socket asynchronously. The
// channel is refcounted like WorkerThreadableWebSocketChannel: ThreadableWebSocketChannel inherits
// AbstractRefCounted, whose ref()/deref() a concrete subclass must supply, and RefCounted<T> supplies
// them -- so the base list is RefCounted<T> first, then the interface, exactly as the worker does it.
class PortWebSocketChannel final : public RefCounted<PortWebSocketChannel>, public ThreadableWebSocketChannel {
public:
    static Ref<PortWebSocketChannel> create() { return adoptRef(*new PortWebSocketChannel); }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    PortWebSocketChannel() = default;

    ConnectStatus connect(const URL&, const String&) final { return ConnectStatus::KO; }
    String subprotocol() final { return String(); }
    String extensions() final { return String(); }
    void send(CString&&) final { }
    void send(const JSC::ArrayBuffer&, unsigned, unsigned) final { }
    void send(Blob&) final { }
    void close(int, const String&) final { }
    void fail(String&&) final { }
    void disconnect() final { }
    void suspend() final { }
    void resume() final { }
    WebSocketChannelIdentifier progressIdentifier() const final { return WebSocketChannelIdentifier::generate(); }
    bool hasCreatedHandshake() const final { return false; }
    bool isConnected() const final { return false; }
    ResourceRequest clientHandshakeRequest(const CookieGetter&) const final { return ResourceRequest(); }
    const ResourceResponse& serverHandshakeResponse() const final
    {
        static NeverDestroyed<ResourceResponse> empty;
        return empty;
    }
};

class PortSocketProvider final : public SocketProvider {
public:
    static Ref<PortSocketProvider> create() { return adoptRef(*new PortSocketProvider); }

    RefPtr<ThreadableWebSocketChannel> createWebSocketChannel(Document&, WebSocketChannelClient&) final
    {
        return PortWebSocketChannel::create();
    }

    std::pair<RefPtr<WebTransportSession>, Ref<WebTransportSessionPromise>> initializeWebTransportSession(ScriptExecutionContext&, WebTransportSessionClient&, const URL&, const WebTransportOptions&) final
    {
        return { nullptr, WebTransportSessionPromise::createAndReject() };
    }

#if USE(LIBRICE)
    RefPtr<WebCore::RiceBackend> createRiceBackend(WebCore::RiceBackendClient&) final { return nullptr; }
#endif

private:
    PortSocketProvider() = default;
};

Ref<SocketProvider> createPortSocketProvider()
{
    return PortSocketProvider::create();
}

} // namespace WebCorePort
