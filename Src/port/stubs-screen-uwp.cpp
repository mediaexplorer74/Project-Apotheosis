// ============================================================================
// stubs-screen-uwp.cpp — Screen function stubs compiled INTO WebCore.dll
// (needed because WebCore's own Media Queries / Screen.cpp reference them)
// ============================================================================

#include "config.h"

#include "FloatRect.h"
#include "Widget.h"
#include "DestinationColorSpace.h"

namespace WebCore {

static constexpr float kScreenW = 1080;
static constexpr float kScreenH = 1920;

int  screenDepth(Widget*)             { return 24; }
int  screenDepthPerComponent(Widget*) { return 8; }
bool screenIsMonochrome(Widget*)      { return false; }
bool screenHasInvertedColors()        { return false; }

FloatRect screenRect(Widget*)          { return FloatRect(0, 0, kScreenW, kScreenH); }
FloatRect screenAvailableRect(Widget*) { return FloatRect(0, 0, kScreenW, kScreenH); }

double screenDPI(PlatformDisplayID) { return 96.0; }

DestinationColorSpace screenColorSpace(Widget*) { return DestinationColorSpace::SRGB(); }

OptionSet<ContentsFormat> screenContentsFormats(Widget*) { return { ContentsFormat::RGBA8 }; }
bool screenSupportsExtendedColor(Widget*)               { return false; }

} // namespace WebCore
