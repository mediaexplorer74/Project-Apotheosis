/*
 * PortBackForward.h -- the port's BackForwardClient: a real, recording history list.
 *
 * pageConfigurationWithEmptyClients() installs EmptyBackForwardClient, whose every method is a
 * silent no-op -- addItem() does nothing and backListCount() returns 0. Nothing is ever recorded,
 * so canGoBackOrForward(-1) is permanently false and the engine believes it has no history at all,
 * however many links the user has followed. See PortBackForward.cpp for the measurement.
 */

#pragma once

#include <WebCore/BackForwardClient.h>
#include <wtf/Ref.h>

namespace WebCorePort {

// A BackForwardClient that keeps the items it is given, keeps a cursor into them, and answers the
// cursor questions the engine asks. Everything runs on the engine thread; no locks by design,
// exactly like PortStorage and PortWebSocket.
WTF::Ref<WebCore::BackForwardClient> createPortBackForwardClient();

// Diagnostic only, and deliberately exported so the OTHER translation unit that has to be checked --
// LoadingFrameLoaderClient.cpp's createHistoryItemTree, which is the only producer of entries --
// can write into the same file. Appends one line to port-trace.txt beside FONTCONFIG_FILE, the
// destination rule every other trace writer in this port already uses. Cheap enough to leave
// installed: it is a handful of lines per navigation, not per frame.
void apoBackForwardTrace(const char* fmt, ...);

} // namespace WebCorePort
