// WebCoreDriverProbe.cpp — foundation check: can we compile a .cpp that includes WebCore's internal
// headers using WebCore's own clang-cl flags? It does not render; it only proves "WebCore's public
// headers are usable from outside + they compile". Success = the Phase 1b driver approach is viable.
#include "config.h"            // WebCore's first include in every TU (under -IE:\...\WebKit\Source\WebCore)

#include <WebCore/Page.h>
#include <WebCore/Frame.h>
#include <WebCore/Document.h>

#include <cstdint>

// Exported C interface (the outer C++/CX app calls through it; ABI-neutral).
extern "C" int WebCoreProbe()
{
    // Only take static sizes, ensuring these types are fully visible and the headers compile.
    // Does not instantiate anything (avoids link-time symbols).
    return static_cast<int>(sizeof(WebCore::Page) + sizeof(WebCore::Document));
}
