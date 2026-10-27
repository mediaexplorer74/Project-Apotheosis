// ============================================================================
// PortUserAgent.h - the one pair of user-agent strings the port sends.
//
// Both halves of a load have to agree about what kind of browser this is:
//
//   * WebCore's own requests take the UA from
//     LoadingFrameLoaderClient::userAgent(), which is what a page sees as
//     navigator.userAgent and what its subresource requests carry;
//   * the driver's top-level navigation is a libcurl fetch in WebCoreDriver.cpp,
//     so it sets CURLOPT_USERAGENT itself.
//
// Until 2026-10-25 the second half was always a Safari-on-Windows string, even
// when the first half was the iPhone one. dzen.ru reads that pair as
// "desktop Windows + a browser that is not Yandex/Edge" and covers the page with
// its browser-switch-trap. The x64 bench reproduced it; the same page loaded
// cleanly once both halves were desktop.
//
// A second dzen finding is why the desktop pair has two strings: with Edge the
// trap is gone, but Yandex Direct still serves a "download Yandex Browser"
// splashscreen to Edge/Firefox/Chrome. A Yandex Browser UA avoids it, but that
// string is the right identity only for Yandex properties -- not for every
// site. So desktop chooses by host: Yandex-owned sites get the Yandex string,
// everything else keeps Edge. The host list is explicit and suffix-matched, not
// a substring test, so `notdzen.ru` and `dzen.ru.example.com` stay on Edge.
// (microsoft.com blocks this bench's client with either UA; its block is not a
// usable one-bit test for this branch.)
//
// Keeping the strings here, rather than in each file, makes the pair impossible
// to drift apart again.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace WebCorePort {

inline constexpr char mobileUserAgent[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_4 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/16.4 Mobile/15E148 Safari/604.1";

// Desktop: a current Edge/Chromium UA. Legacy Safari-on-Windows strings are
// blocked by sites such as microsoft, and dzen.ru's browser-switch-trap treats
// a non-Edge Chromium UA as unsupported on Windows, so this is deliberately
// Edge rather than plain Chrome.
//
// Apotheosis 2026-10-27: bumped Edg/120 -> Edg/156 (Chromium 120 -> 138) because the real
// machine runs Edge 156.0.4314.42 and sites gate on the pair: nokiadesignarchive.aalto.fi's
// GDPR CMP logged "[CMP] Your browser (edge v120) is not supported. The CMP requires edge v124
// or higher and may not function correctly" -- a broken CMP/onboarding is suspected of leaving
// the site's rgba(0,0,0,0.8) welcome overlay half-drawn (maintainer's "graph went dark"). The
// mobile UA above is untouched (Apple-side gates do not read the Edge key).
inline constexpr char desktopUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36 Edg/156.0.0.0";

// Desktop, Yandex-owned hosts only. dzen.ru serves a Yandex Browser Direct
// splashscreen to Edge, and Yandex properties are tuned to YaBrowser; the
// version matches the compatibility baseline, not the newest Yandex release.
inline constexpr char desktopYandexUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 YaBrowser/24.1.0.0 Safari/537.36";

// Consumer Yandex domains. Regional domains are listed individually because a
// generic "any label named yandex" rule would also match attacker-controlled
// names such as yandex.example.com. Add a suffix here when a new regional
// domain is actually needed; do not add API/cloud hosts, which are better left
// on the ordinary desktop UA.
inline constexpr const char* yandexHostSuffixes[] = {
    "dzen.ru",
    "yandex.ru",
    "yandex.com",
    "yandex.net",
    "yandex.by",
    "yandex.kz",
    "yandex.ua",
    "yandex.com.tr",
    "ya.ru",
    "yastatic.net",
};

// The host part of an absolute URL, without scheme, userinfo, port or path.
// Returns an empty view for about:, data:, file: and other non-host URLs.
inline std::string_view urlHost(const char* url)
{
    if (!url || !*url)
        return {};

    std::string_view text(url);
    size_t start = 0;
    const size_t scheme = text.find("://");
    if (scheme != std::string_view::npos)
        start = scheme + 3;
    else if (text.size() >= 2 && text[0] == '/' && text[1] == '/')
        start = 2;
    else
        return {};

    const size_t authorityEnd = text.find_first_of("/?#", start);
    std::string_view authority = text.substr(start,
        authorityEnd == std::string_view::npos ? std::string_view::npos : authorityEnd - start);

    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos)
        authority.remove_prefix(at + 1);

    if (authority.empty())
        return {};

    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos)
            return {};
        return authority.substr(1, close - 1);
    }

    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos)
        authority = authority.substr(0, colon);
    return authority;
}

inline bool hostMatchesSuffix(std::string_view host, const char* suffix)
{
    const size_t suffixLength = std::strlen(suffix);
    if (host.size() < suffixLength)
        return false;

    const size_t offset = host.size() - suffixLength;
    for (size_t i = 0; i < suffixLength; ++i) {
        unsigned char a = static_cast<unsigned char>(host[offset + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return offset == 0 || host[offset - 1] == '.';
}

inline bool isYandexHost(const char* url)
{
    const std::string_view host = urlHost(url);
    if (host.empty())
        return false;
    for (const char* suffix : yandexHostSuffixes) {
        if (hostMatchesSuffix(host, suffix))
            return true;
    }
    return false;
}

} // namespace WebCorePort
