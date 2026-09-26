// ============================================================================
// PortStorage.cpp -- the in-memory Web Storage backend. See PortStorage.h for
// why it exists and what it deliberately does not do.
//
// Three classes, mirroring upstream's shape but with no database underneath:
//
//   PortStorageArea           one origin's key/value map (+ quota accounting)
//   PortStorageAreaMap        origin -> area, shared by the namespaces that must
//                             share storage (a page and the pages it opens)
//   PortStorageNamespace      one namespace (local / transient / session)
//   PortStorageNamespaceProvider  hands them out; what Page asks for
//
// Everything runs on the engine thread. WebCore reaches these only from script
// execution and from Document teardown, both of which are already serialised
// there, so there is no lock anywhere by design -- adding one would be the thing
// that needed proving, not its absence.
// ============================================================================

#include "config.h"

#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>    // inline LocalFrame::protectedDocument()
#include <WebCore/Page.h>                 // Page::group(), Page::sessionID()
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/SecurityOriginHash.h>   // DefaultHash<SecurityOriginData>
#include <WebCore/Storage.h>              // Storage::frame(), for the source-window test
#include <WebCore/StorageArea.h>
#include <WebCore/StorageEventDispatcher.h>
#include <WebCore/StorageNamespace.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/StorageType.h>
#include <pal/SessionID.h>
#include <wtf/HashMap.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

#include <cstdarg>
#include <cstdio>
#include <limits>

using namespace WebCore;

namespace WebCorePort {

// Defined in WebCoreDriver.cpp. The one log channel that is flushed per line, so
// it survives the hang it is used to explain.
void portDiagLog(const char* utf8);

// Apotheosis: storage gets a voice. This whole defect hid for months *because*
// storage said nothing -- every call reported success and left no trace, so a
// page looping on it looked like a page looping on nothing. The per-area cap in
// traceWrite() is not cosmetic: a looping page writes without bound, and
// gpuinit-steps.txt is append-only on a device with little free space.
//
// The format attribute is not decoration: it is what makes a wrong %s in one of
// the call sites below a compile error rather than a line of garbage in the one
// log that is supposed to explain a freeze.
#if defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static void portStorageLog(const char* fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    portDiagLog(line);
}

// ---------------------------------------------------------------------------
// PortStorageArea -- one origin's key/value map.
//
// The StorageArea contract WebCore's LocalDOMWindow expects: writes are visible
// to the very next read, key(index) walks a stable order, and quota overflow is
// reported through the out parameter (Storage::setItem turns it into the
// QuotaExceededError the page sees) rather than by throwing from here.
// ---------------------------------------------------------------------------
class PortStorageArea final : public StorageArea {
public:
    PortStorageArea(StorageType type, unsigned quota, const SecurityOrigin& origin)
        : m_type(type)
        , m_quota(quota)
        , m_origin(origin.isolatedCopy())
        , m_originLabel(origin.data().toString().utf8())
    {
    }

    unsigned length() final { return m_keys.size(); }

    String key(unsigned index) final
    {
        if (index >= m_keys.size())
            return String();
        return m_keys[index];
    }

    String item(const String& key) final
    {
        auto it = m_values.find(key);
        if (it == m_values.end())
            return String();
        return it->value;
    }

    void setItem(LocalFrame& sourceFrame, const String& key, const String& value, bool& quotaException) final
    {
        auto it = m_values.find(key);
        if (it != m_values.end() && it->value == value) {
            // A write that changes nothing is not a change: no event, no log line.
            // dzen.ru's loop rewrites the same settings object on every turn, so
            // this early return also keeps the log readable.
            return;
        }

        unsigned oldBytes = it == m_values.end() ? 0 : entryBytes(key, it->value);
        unsigned newBytes = entryBytes(key, value);
        if (isQuotaEnforced() && m_bytes - oldBytes + newBytes > m_quota) {
            quotaException = true;
            portStorageLog("storage: QUOTA origin=%s key=%.60s used=%u want=%u quota=%u",
                m_originLabel.data(), key.utf8().data(), m_bytes, m_bytes - oldBytes + newBytes, m_quota);
            return;
        }

        String oldValue = it == m_values.end() ? String() : it->value;
        if (it == m_values.end()) {
            m_keys.append(key);
            m_values.add(key, value);
        } else
            it->value = value;
        m_bytes = m_bytes - oldBytes + newBytes;

        traceWrite(key, value);
        dispatchStorageEvent(key, oldValue, value, sourceFrame);
    }

    void removeItem(LocalFrame& sourceFrame, const String& key) final
    {
        auto it = m_values.find(key);
        if (it == m_values.end())
            return;
        String oldValue = it->value;
        m_bytes -= entryBytes(key, oldValue);
        m_values.remove(it);
        m_keys.removeFirst(key);
        traceRemove(key, oldValue);
        dispatchStorageEvent(key, oldValue, String(), sourceFrame);
    }

    void clear(LocalFrame& sourceFrame) final
    {
        if (m_keys.isEmpty())
            return;
        m_keys.clear();
        m_values.clear();
        m_bytes = 0;
        portStorageLog("storage: clear origin=%s writes=%u", m_originLabel.data(), m_writes);
        // Per spec the event for clear() carries a null key and null values.
        dispatchStorageEvent(String(), String(), String(), sourceFrame);
    }

    bool contains(const String& key) final { return m_values.contains(key); }
    StorageType storageType() const final { return m_type; }
    size_t memoryBytesUsedByCache() final { return 0; }

private:
    // UTF-16 units at two bytes each. An approximation of what a real browser
    // counts, and deliberately an over-estimate: refusing a write we could have
    // held is recoverable, letting the map grow has no ceiling at all.
    static unsigned entryBytes(const String& key, const String& value)
    {
        return 2u * (key.length() + value.length());
    }

    // StorageMap::noQuota is UINT_MAX; 0 reaches us when a namespace was built
    // before Page::setSessionStorageQuota ran. Either way: no ceiling.
    bool isQuotaEnforced() const
    {
        return m_quota && m_quota != std::numeric_limits<unsigned>::max();
    }

    // First writes of an area are all interesting; after that a heartbeat.
    void traceWrite(const String& key, const String& value)
    {
        if (!shouldTrace())
            return;
        portStorageLog("storage: set origin=%s key=%.40s len=%u writes=%u",
            m_originLabel.data(), key.utf8().data(), value.length(), m_writes);
    }

    void traceRemove(const String& key, const String& oldValue)
    {
        if (!shouldTrace())
            return;
        portStorageLog("storage: del origin=%s key=%.40s oldlen=%u removes=%u",
            m_originLabel.data(), key.utf8().data(), oldValue.length(), ++m_removes);
    }

    bool shouldTrace()
    {
        ++m_writes;
        return m_writes <= 8 || !(m_writes % 500);
    }

    void dispatchStorageEvent(const String& key, const String& oldValue, const String& newValue, LocalFrame& sourceFrame)
    {
        // The source window is excluded by identity, so a page never hears its own
        // write -- which is also why a `storage` self-loop cannot be the freeze
        // mechanism (measured: LocalState\storageloop.html, n=0).
        auto isSourceStorage = [&sourceFrame](Storage& storage) {
            return storage.frame() == &sourceFrame;
        };
        auto url = sourceFrame.protectedDocument()->url().string();
        if (isLocalStorage(m_type)) {
            // A null PageGroup is the honest argument here, not a shortcut: WebCore reads it
            // as "no grouping", notifies every page, and filters by origin and by the
            // source-window test above -- which for localStorage is exactly the spec, since
            // the area is shared by all same-origin pages. This port installs no PageGroup
            // and has one Page, so there is no group to name.
            StorageEventDispatcher::dispatchLocalStorageEvents(key, oldValue, newValue, nullptr, m_origin.get(), url, WTF::move(isSourceStorage));
            return;
        }
        // Session storage does need the Page: its scope is the tab, and the dispatcher
        // walks that page's windows. Frame::page() is NOT exported from WebCore.dll (the
        // port links the DLL's import library, so only WEBCORE_EXPORTed symbols exist),
        // hence the walk; Page::forEachPage is exported, and with one Page it yields the
        // source page. See Doc/UNKNOWN-EXPORTS.md's list of accessors that look available
        // and are not.
        if (Page* page = firstPage())
            StorageEventDispatcher::dispatchSessionStorageEvents(key, oldValue, newValue, *page, m_origin.get(), url, WTF::move(isSourceStorage));
    }

    // Page::forEachPage hands out references, so nothing is held between calls and there
    // is no pointer to go stale when a Page is destroyed.
    static void collectFirstPage(Page& page)
    {
        if (!s_walkPage)
            s_walkPage = &page;
    }

    static Page* firstPage()
    {
        s_walkPage = nullptr;
        Page::forEachPage(collectFirstPage);
        Page* found = s_walkPage;
        s_walkPage = nullptr;
        return found;
    }

    // File-scope state for the walk above; see its definition below the class.
    static Page* s_walkPage;

    StorageType m_type;
    unsigned m_quota { 0 };
    unsigned m_bytes { 0 };
    unsigned m_writes { 0 };
    unsigned m_removes { 0 };
    Ref<SecurityOrigin> m_origin;
    CString m_originLabel;
    HashMap<String, String> m_values;
    Vector<String> m_keys;   // insertion order, for key(index)
};

// The only state the Page walk above needs, and it is reset before and after every walk:
// a Page that is destroyed between two dispatches can never leave a dangling reader.
Page* PortStorageArea::s_walkPage = nullptr;

// ---------------------------------------------------------------------------
// PortStorageAreaMap -- origin -> area, refcounted so that two namespaces which
// must share storage can share this map instead of duplicating it.
//
// One area per origin is the whole contract: a value written by one document of
// an origin must be visible to the next read by any other document of the same
// origin. Per-namespace areas would break exactly that.
// ---------------------------------------------------------------------------
class PortStorageAreaMap final : public RefCounted<PortStorageAreaMap> {
public:
    static Ref<PortStorageAreaMap> create(StorageType type, unsigned quota)
    {
        return adoptRef(*new PortStorageAreaMap(type, quota));
    }

    Ref<StorageArea> areaFor(const SecurityOrigin& origin)
    {
        auto& slot = m_areas.add(origin.data(), nullptr).iterator->value;
        if (!slot)
            slot = adoptRef(new PortStorageArea(m_type, m_quota, origin));
        return slot.releaseNonNull();
    }

private:
    PortStorageAreaMap(StorageType type, unsigned quota)
        : m_type(type)
        , m_quota(quota)
    {
    }

    StorageType m_type;
    unsigned m_quota { 0 };
    HashMap<SecurityOriginData, RefPtr<PortStorageArea>> m_areas;
};

// ---------------------------------------------------------------------------
// PortStorageNamespace -- one namespace: local, transient-local, or session.
// ---------------------------------------------------------------------------
class PortStorageNamespace final : public StorageNamespace {
public:
    static Ref<PortStorageNamespace> create(PAL::SessionID sessionID, StorageType type,
        unsigned quota, const SecurityOrigin* topLevelOrigin, Ref<PortStorageAreaMap>&& areas)
    {
        return adoptRef(*new PortStorageNamespace(sessionID, type, quota, topLevelOrigin, WTF::move(areas)));
    }

    static Ref<PortStorageNamespace> create(PAL::SessionID sessionID, StorageType type,
        unsigned quota, const SecurityOrigin* topLevelOrigin)
    {
        return create(sessionID, type, quota, topLevelOrigin, PortStorageAreaMap::create(type, quota));
    }

    Ref<StorageArea> storageArea(const SecurityOrigin& origin) final
    {
        return m_areas->areaFor(origin);
    }

    const SecurityOrigin* topLevelOrigin() const final { return m_topLevelOrigin.get(); }

    // A page opened from this one (window.open, target=_blank) must see the *same*
    // session storage, so the copy shares the area map rather than duplicating it.
    Ref<StorageNamespace> copy(Page&) final
    {
        return create(m_sessionID, m_type, m_quota, m_topLevelOrigin.get(), m_areas.copyRef());
    }

    PAL::SessionID sessionID() const final { return m_sessionID; }
    void setSessionIDForTesting(PAL::SessionID sessionID) final { m_sessionID = sessionID; }

private:
    PortStorageNamespace(PAL::SessionID sessionID, StorageType type, unsigned quota,
        const SecurityOrigin* topLevelOrigin, Ref<PortStorageAreaMap>&& areas)
        : m_areas(WTF::move(areas))
        , m_sessionID(sessionID)
        , m_type(type)
        , m_quota(quota)
        , m_topLevelOrigin(topLevelOrigin ? RefPtr<SecurityOrigin> { topLevelOrigin->isolatedCopy() } : nullptr)
    {
    }

    Ref<PortStorageAreaMap> m_areas;
    PAL::SessionID m_sessionID;
    StorageType m_type;
    unsigned m_quota { 0 };
    RefPtr<SecurityOrigin> m_topLevelOrigin;
};

// ---------------------------------------------------------------------------
// PortStorageNamespaceProvider -- what Page asks for.
//
// The base class caches the local namespace itself (m_localStorageNamespace) and
// keys transient ones by origin, so this only has to mint namespaces; the routing
// from a Document to "local or transient, and for which origin" is upstream's.
// ---------------------------------------------------------------------------
class PortStorageNamespaceProvider final : public StorageNamespaceProvider {
public:
    static Ref<PortStorageNamespaceProvider> create()
    {
        portStorageLog("storage: provider installed (in-memory, process-scoped)");
        return adoptRef(*new PortStorageNamespaceProvider);
    }

    Ref<StorageNamespace> createLocalStorageNamespace(unsigned quota, PAL::SessionID sessionID) final
    {
        portStorageLog("storage: namespace local quota=%u session=%llu", quota,
            static_cast<unsigned long long>(sessionID.toUInt64()));
        return PortStorageNamespace::create(sessionID, StorageType::Local, quota, nullptr);
    }

    Ref<StorageNamespace> createTransientLocalStorageNamespace(SecurityOrigin& origin, unsigned quota, PAL::SessionID sessionID) final
    {
        portStorageLog("storage: namespace third-party origin=%s quota=%u",
            origin.data().toString().utf8().data(), quota);
        return PortStorageNamespace::create(sessionID, StorageType::TransientLocal, quota, &origin);
    }

    RefPtr<StorageNamespace> sessionStorageNamespace(const SecurityOrigin& topLevelOrigin, Page& page, ShouldCreateNamespace shouldCreate) final
    {
        auto it = m_sessionStorageNamespaces.find(topLevelOrigin.data());
        if (it == m_sessionStorageNamespaces.end()) {
            if (shouldCreate == ShouldCreateNamespace::No)
                return nullptr;
            portStorageLog("storage: namespace session origin=%s quota=%u",
                topLevelOrigin.data().toString().utf8().data(), sessionStorageQuota());
            it = m_sessionStorageNamespaces.add(topLevelOrigin.data(),
                PortStorageNamespace::create(page.sessionID(), StorageType::Session,
                    sessionStorageQuota(), &topLevelOrigin)).iterator;
        }
        return it->value;
    }

    // The base class default RELEASE_ASSERTs. This port has one Page and never opens a
    // second one, so the honest override is "nothing to move" rather than a crash if
    // some future path asks. Upstream's WebKitLegacy version moves the namespace; there
    // is nowhere here for it to move to.
    void cloneSessionStorageNamespaceForPage(Page&, Page&) final { }

private:
    // Keyed by top-level origin rather than by Page as upstream does: with a single
    // Page the two keys cannot disagree, and the origin is the thing that actually
    // scopes session storage.
    HashMap<SecurityOriginData, RefPtr<StorageNamespace>> m_sessionStorageNamespaces;
};

Ref<StorageNamespaceProvider> createPortStorageNamespaceProvider()
{
    return PortStorageNamespaceProvider::create();
}

} // namespace WebCorePort
