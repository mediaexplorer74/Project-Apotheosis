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
// M2 (post-0.2.3.73) moved the local namespace onto SQLite under
// LocalState\localstorage.db. The interface and quota accounting are unchanged;
// PortStorageArea now loads an origin's rows when it is created and commits each
// set/remove/clear before dispatching the storage event. Session and transient
// local storage stay in memory, because their lifetime is the session or the
// top-level page, not the installation. If the database cannot be opened, the
// local areas fall back to the old process-scoped map and say so in the trace.
// ============================================================================

#pragma once

#include <wtf/Ref.h>

namespace WebCore {
class StorageNamespaceProvider;
}

namespace WebCorePort {

// Install into PageConfiguration::storageNamespaceProvider before Page::create.
WTF::Ref<WebCore::StorageNamespaceProvider> createPortStorageNamespaceProvider();

// M2: open (or create) the SQLite database that backs localStorage. Call before
// the first Page is built, with a UTF-8 path in a writable directory such as
// LocalState. Returns false and leaves the in-memory backend in place on error.
bool setPortStorageDatabasePath(const char* utf8Path);

// Rows in ItemTable across every origin; startup evidence, not a page API.
long long portStorageDatabaseRowCount();

} // namespace WebCorePort
