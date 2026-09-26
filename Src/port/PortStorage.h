// ============================================================================
// PortStorage.h -- Web Storage that actually stores.
//
// Apotheosis 2026-09-18. Until this file existed the port had no
// StorageNamespaceProvider of its own, so `Page` received
// EmptyStorageNamespaceProvider from pageConfigurationWithEmptyClients()
// (WebCore/loader/EmptyClients.cpp:560). Its EmptyStorageArea accepts every
// setItem and drops it: length() is always 0, item() is always null, nothing
// throws. `localStorage` therefore LOOKS present -- which is why the fork enabled
// it (WebCoreDriver.cpp, the DOM Storage comment) -- and is a silent no-op.
//
// dzen.ru's settings-sync module is a read-after-write loop:
//
//     p = () => { const e = o.p.getItem(i); return e ? d(e) : (f({settings:l,isFirstSet:!0}), l) }
//     u = e => { o.p.setItem(i, JSON.stringify(e)); ...document.dispatchEvent(s) }
//
// Its convergence condition is that the write be visible to the next read. With
// the write discarded the condition can never hold, so it re-enters itself
// synchronously forever across the C++ boundary. That is the freeze in
// Doc/DZEN-SCROLL-DEATH.md section 10f -- not a page bug, and not the watchdog
// failing to terminate: there is no released browser in which this loop fails to
// converge.
//
// The areas below are in-memory and process-scoped: values survive navigation
// within a session, not an app restart. That is a deliberate, documented limit.
// This port's sqlite3 is a stub (Src/port/stubs-sqlite.cpp -- every open returns
// SQLITE_CANTOPEN) because libsqlite3 is not available under
// WINAPI_FAMILY_APP, so there is no database to persist into. A storage API that
// answers honestly about what it holds is worth more than one that reports
// success and forgets, and a page cannot tell the difference until the app
// restarts.
// ============================================================================

#pragma once

#include <wtf/Ref.h>

namespace WebCore {
class StorageNamespaceProvider;
}

namespace WebCorePort {

// Install into PageConfiguration::storageNamespaceProvider before Page::create.
WTF::Ref<WebCore::StorageNamespaceProvider> createPortStorageNamespaceProvider();

} // namespace WebCorePort
