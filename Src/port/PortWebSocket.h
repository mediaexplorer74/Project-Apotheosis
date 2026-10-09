/*
 * PortWebSocket.h -- the port's SocketProvider.
 *
 * pageConfigurationWithEmptyClients() installs EmptySocketProvider, whose
 * createWebSocketChannel() returns nullptr, and WebCore::WebSocket::connect()
 * RELEASE_ASSERTs on a null channel. See PortWebSocket.cpp for the device run
 * that named the assert.
 */

#pragma once

#include <WebCore/SocketProvider.h>
#include <wtf/Ref.h>

namespace WebCorePort {

// A provider whose channels never connect: connect() reports ConnectStatus::KO, which is the one
// answer WebSocket::connect() turns into a clean asynchronous failure instead of a process abort.
WTF::Ref<WebCore::SocketProvider> createPortSocketProvider();

} // namespace WebCorePort
