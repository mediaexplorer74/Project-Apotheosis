// ============================================================================
// stubs-network.cpp — link-time stubs for the "network" class of undefined
// platform symbols (EdgeHTML Reborn — ARM32 thumbv7 Win10-Mobile App Container).
//
// STATUS (Phase 1b, 2026-06-15 — curl backend turned ON):
//   This build is now USE(CURL)=1 / USE(OPENSSL)=1. The curl backend TUs under
//   platform/network/curl ARE compiled, so they now own the symbols that this
//   file used to stub. ALL of the former stubs here would LNK2005-collide with:
//     * ResourceHandle::*            -> revived in ResourceHandle.cpp
//                                       (#if USE(CURL) && WK_WINUWP curl bridge)
//     * SynchronousLoaderClient::*   -> SynchronousLoaderClient.cpp curl tail
//     * NetworkStorageSession cookies-> NetworkStorageSessionCurl.cpp
//     * ResourceError platform bits  -> ResourceErrorCurl.cpp
//     * ResourceResponse::platformSuggestedFilename -> ResourceResponseCurl.cpp
//     * CertificateInfo summary/isolatedCopy        -> CertificateInfoCurl.cpp
//   They have ALL been removed from this file.
//
// WHAT REMAINS:
//   Only the two NetworkStateNotifier platform hooks. The agnostic
//   NetworkStateNotifier.cpp IS compiled and owns singleton()/onLine()/
//   addListener()/updateState(); only these two platform members are missing
//   (normally NetworkStateNotifier{Win,GLib,Mac}.cpp, none of which is built for
//   this App-Container port). No real adapter polling -> pretend "online".
//   Pinning m_isOnLine prevents onLine()/updateState() from re-querying forever.
//
// PlatformScreen / DNSResolveQueue are owned by other stub files — untouched.
// ============================================================================

#include "config.h"

#include "NetworkStateNotifier.h"
#include "PublicSuffixStore.h"

namespace WebCore {

void NetworkStateNotifier::updateStateWithoutNotifying()
{
    m_isOnLine = true;
}

void NetworkStateNotifier::startObserving()
{
}

// Apotheosis: PublicSuffixStoreCurl.cpp (libpsl) is excluded from the WK_WINUWP
// build — libpsl is not installable for the UWP App Container (meson cross-build
// fails, same as harfbuzz/fontconfig). Without PSL data nothing is treated as a
// public suffix; the registrable domain falls back to the last two segments
// (mirrors the Soup fallback for SOUP_TLD_ERROR_NO_BASE_DOMAIN).

bool PublicSuffixStore::platformIsPublicSuffix(StringView domain) const
{
    return false;
}

static String permissiveTopPrivateDomain(StringView domain)
{
    auto position = domain.length();
    bool foundDot = false;

    while (position-- > 0) {
        if (domain[position] == '.') {
            if (foundDot)
                return domain.substring(position + 1).toString();
            foundDot = true;
        }
    }

    return foundDot ? domain.toString() : String();
}

String PublicSuffixStore::platformTopPrivatelyControlledDomain(StringView domain) const
{
    unsigned position = 0;
    while (position < domain.length() && domain[position] == '.')
        position++;

    auto tldView = domain.substring(position);
    if (tldView.isEmpty())
        return String();

    return permissiveTopPrivateDomain(tldView);
}

} // namespace WebCore
