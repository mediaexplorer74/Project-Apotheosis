#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Apotheosis (2026-09-18): classify an engine diag string as "the load produced nothing".
//
// Why this exists as its own header: the harness must not show a empty white window and call it a
// success. A load can return rc=0 and still paint nothing -- dzen.ru redirects to an SSO page whose
// body is empty, which a user reads as "the browser crashed". The signature is in the diag line the
// driver already writes, and keeping the parser here (rather than inside the 6k-line MainPage) makes
// it testable against real log samples.
//
// Fields it reads, as produced by WebCoreDriver's writeDiag():
//   nonwhite=<painted>/<total>   pixels differing from the document background
//   body=<0|1>                   a body element exists
//   bodyKids=<n|-1>              body childElementCount, or -1 when there is no body
//
// "Empty" = zero non-white pixels AND no body content. A legitimately white page still has a body,
// so it is not misreported as empty.
namespace ApoDiagEmpty {

// Returns the bodyKids value found (0 when absent) and whether the string looks empty.
inline bool LooksEmpty(const char* diag, int& bodyKidsOut)
{
    bodyKidsOut = 0;
    if (!diag || !*diag)
        return false;

    bool sawNonWhite = false, nonWhiteIsZero = false;
    // Scan every occurrence: the diag string embeds resource lists and script states that may also
    // contain the token, and the authoritative one is the last field written.
    for (const char* p = diag; (p = strstr(p, "nonwhite=")) != nullptr; p += 9) {
        sawNonWhite = true;
        nonWhiteIsZero = (strtol(p + 9, nullptr, 10) == 0);
    }

    bool sawBodyKids = false;
    if (const char* p = strstr(diag, "bodyKids=")) {
        bodyKidsOut = static_cast<int>(strtol(p + 9, nullptr, 10));
        sawBodyKids = true;
    }

    return sawNonWhite && nonWhiteIsZero && sawBodyKids && bodyKidsOut <= 0;
}

} // namespace ApoDiagEmpty