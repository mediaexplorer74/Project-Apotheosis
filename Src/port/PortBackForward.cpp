// ============================================================================
// PortBackForward.cpp -- a BackForwardClient that actually records history.
//
// Why it exists. pageConfigurationWithEmptyClients() (the only source of a
// PageConfiguration in this port) installs EmptyBackForwardClient, and every
// one of its methods is a silent no-op:
//
//     void addItem(Ref<HistoryItem>&&) final { }
//     RefPtr<HistoryItem> itemAtIndex(int, FrameIdentifier) final { return nullptr; }
//     unsigned backListCount() const final { return 0; }
//
// So the engine's back/forward list is always empty. BackForwardController::
// canGoBackOrForward(-1) is driven entirely by backListCount(), it therefore
// always answers false, and WebCoreGoBack can never move. Measured on the x64
// bench 2026-10-10: example.com loaded, a link was tapped and the engine
// followed it (frame hash 0xd8431c54 -> 0x52cad7f7, URL www.iana.org), and the
// very next probe reported `engine back=0 fwd=0` -- there was nothing to go
// back to, because nothing had been written down.
//
// This is the third member of one family and it should be read as such:
// EmptyStorageNamespaceProvider made localStorage a silent success (PortStorage),
// EmptySocketProvider returned a null channel and aborted the process
// (PortWebSocket), and EmptyBackForwardClient silently drops every navigation.
// In each case the empty client is a plausible-looking no-op rather than a
// working default, and in each case the port has to supply the real thing.
//
// What it does. Keeps the items in a Vector, keeps a cursor into it, and
// answers exactly the questions BackForwardController asks:
//   backListCount()    -- the cursor's distance from the front (the count behind us)
//   forwardListCount() -- the entries after the cursor
//   itemAtIndex(n)     -- the entry n steps from the cursor, nullptr when out of range
//   containsItem()     -- by itemID, which is what HistoryItem::operator== uses
//   goToItem()         -- the engine telling us the cursor moved; we adopt it
//   addItem()          -- a committed navigation: drop the forward tail, append, cursor to it
//
// The semantics mirror WebKit2's WebBackForwardListProxy, which is the real
// client this port is standing in for. Subframe items (setChildItem) are
// deliberately dropped: createFrame() returns nullptr from the port's clients,
// so this port never has a child frame and there is nothing to parent.
//
// Everything runs on the engine thread, like PortStorage and PortWebSocket; no
// locks by design.
// ============================================================================

#include "config.h"

#include "PortBackForward.h"

#include <WebCore/BackForwardClient.h>
#include <WebCore/FrameIdentifier.h>
#include <WebCore/HistoryItem.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace WebCorePort {

using namespace WebCore;

// Diagnostic. Writes to port-trace.txt beside FONTCONFIG_FILE, which is the destination rule every
// other trace writer in this port uses (see stubs-font-uwp.cpp's apotheosisFontTrace for why each
// translation unit re-derives the path instead of sharing a trace API: the driver's own writers are
// file-local, and declaring them across TUs gives LNK2019 whichever linkage is chosen).
void apoBackForwardTrace(const char* fmt, ...)
{
    const char* fcc = getenv("FONTCONFIG_FILE");
    if (!fcc)
        return;
    std::string path(fcc);
    auto pos = path.rfind('\\');
    if (pos == std::string::npos)
        return;
    path.resize(pos + 1);
    path += "port-trace.txt";

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (FILE* f = fopen(path.c_str(), "ab")) {
        fputs(buf, f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    }
}

class PortBackForwardClient final : public BackForwardClient {
public:
    static Ref<PortBackForwardClient> create() { return adoptRef(*new PortBackForwardClient); }

    void addItem(Ref<HistoryItem>&& item) final
    {
        // A committed navigation. Anything the cursor was holding ahead of it belongs to a branch
        // of history the user has now left, so it goes -- this is what makes a new navigation after
        // a Back discard the Forward tail, and it is the same rule the app's own _navStack applies.
        if (m_cursor >= 0 && static_cast<size_t>(m_cursor) < m_items.size())
            m_items.shrink(m_cursor + 1);
        else
            m_items.clear();

        m_items.append(WTF::move(item));
        m_cursor = static_cast<int>(m_items.size()) - 1;
        apoBackForwardTrace("PBF: addItem url=%s n=%u cursor=%d back=%u fwd=%u",
                            m_items[static_cast<size_t>(m_cursor)]->urlString().utf8().data(),
                            static_cast<unsigned>(m_items.size()), m_cursor,
                            backListCount(), forwardListCount());
    }

    void setChildItem(BackForwardFrameItemIdentifier, Ref<HistoryItem>&&) final
    {
        // No child frames exist on this port (see the note above), so there is no parent item to
        // attach to. Dropping this is correct here, not a stub.
    }

    void goToItem(HistoryItem& item) final
    {
        // Called by HistoryController::goToItem right before it navigates, so that a second
        // Back/Forward arrives with the cursor already in the right place. An item we do not hold
        // is not a reason to move the cursor -- the navigation still proceeds.
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].ptr() == &item || m_items[i]->itemID() == item.itemID()) {
                m_cursor = static_cast<int>(i);
                apoBackForwardTrace("PBF: goToItem url=%s cursor=%d", item.urlString().utf8().data(), m_cursor);
                return;
            }
        }
        apoBackForwardTrace("PBF: goToItem url=%s NOT HELD (cursor stays %d)",
                            item.urlString().utf8().data(), m_cursor);
    }

    Vector<Ref<HistoryItem>> allItems(FrameIdentifier) final { return m_items; }

    RefPtr<HistoryItem> itemAtIndex(int distance, FrameIdentifier) final
    {
        int target = m_cursor + distance;
        if (target < 0 || static_cast<size_t>(target) >= m_items.size())
            return nullptr;
        return m_items[target].ptr();
    }

    unsigned backListCount() const final
    {
        return m_cursor > 0 ? static_cast<unsigned>(m_cursor) : 0;
    }

    unsigned forwardListCount() const final
    {
        if (m_cursor < 0)
            return 0;
        size_t after = m_items.size() - (static_cast<size_t>(m_cursor) + 1);
        return static_cast<unsigned>(after);
    }

    bool containsItem(const HistoryItem& item) const final
    {
        for (auto& held : m_items) {
            if (held.ptr() == &item || held->itemID() == item.itemID())
                return true;
        }
        return false;
    }

    void close() final
    {
        m_items.clear();
        m_cursor = -1;
    }

private:
    PortBackForwardClient() = default;

    Vector<Ref<HistoryItem>> m_items;
    // -1 before the first item, and after close(). currentItem() is itemAtIndex(0), so this value
    // is what makes "no current item yet" answer nullptr instead of reading m_items[0].
    int m_cursor { -1 };
};

Ref<BackForwardClient> createPortBackForwardClient()
{
    apoBackForwardTrace("PBF: createPortBackForwardClient -> a recording list (EmptyBackForwardClient no longer in use)");
    return PortBackForwardClient::create();
}

} // namespace WebCorePort
