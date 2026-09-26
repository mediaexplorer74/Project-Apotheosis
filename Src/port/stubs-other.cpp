// stubs-other.cpp — platform symbol stubs ("other" class) for the WebCore→Win10M render DLL.
// Each definition copies the declaration from its WebCore/PAL header verbatim so the mangled

// name matches what WebCore.lib expects. Bodies are no-ops / safe defaults for the
// layout+paint-only render path. See undef-other.txt for the symbol list.

#include "config.h"

#include "DragImage.h"
#include "Cursor.h"
#include "Icon.h"
#include "SharedMemory.h"
#include "PlatformKeyboardEvent.h"
#include "HTMLSelectElement.h"
#include "GraphicsLayer.h"
#include "ImageAdapter.h"
#include "Image.h"
#include "IntSize.h"
#include "FloatSize.h"
#include "graphics/SystemFontDatabase.h"
// Apotheosis: platformSystemFontShorthandInfo needs a real family atom and a real weight.
#include <WebCore/FontSelectionAlgorithm.h>
#include <WebCore/WebKitFontFamilyNames.h>
#include "graphics/win/DisplayRefreshMonitorWin.h"
#include "FontCache.h"
#include "FontCascade.h"
#include "FontCustomPlatformData.h"
#include "FontPlatformData.h"
#include "Font.h"
#include "PlatformScreen.h"
#include "Widget.h"
#include "CSSFontFaceSrcValue.h"
#include "ComplexTextController.h"
#include "EventHandler.h"
#include "FloatRect.h"
#include "GlyphPage.h"
#include "HandleUserInputEventResult.h"
#include "MainThreadSharedTimer.h"
#include "MouseEventWithHitTestResults.h"
#include "RenderStyle.h"
#include "RenderTheme.h"
#include "ScrollbarTheme.h"
#include "Theme.h"
#include <wtf/Assertions.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CharacterProperties.h>
#include <wtf/unicode/CharacterNames.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unicode/utf16.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// ============================================================================
// Apotheosis: temporary glyph-path diagnostics. Appends to
// APOTHEOSIS_GLYPH_LOG (a full file path, injected by the harness); no-op when
// the env var is unset, so it costs nothing in normal runs.
// ============================================================================
static void apotheosisGlyphLog(const char* format, ...)
{
    const char* path = std::getenv("APOTHEOSIS_GLYPH_LOG");
    if (!path || !*path)
        return;
    FILE* f = std::fopen(path, "a");
    if (!f)
        return;
    va_list args;
    va_start(args, format);
    std::vfprintf(f, format, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}


// ============================================================================
// MSVC compiler intrinsic barrier. clang-cl on ARM may not provide it as an
// intrinsic; supply an empty extern "C" definition so the reference resolves.
// (Real semantics: a compiler-only reordering barrier — harmless here.)
// ============================================================================
#if !__has_builtin(_ReadWriteBarrier)
extern "C" void _ReadWriteBarrier() { }
#endif

namespace WebCore {

// ============================================================================
// DragImage (DragImage.h). On Win, DragImageRef == HBITMAP (struct HBITMAP__*).
// Render path never drags; return null/empty. risky: none (drag not exercised).
// ============================================================================
IntSize dragImageSize(DragImageRef)
{
    return IntSize();
}

DragImageRef scaleDragImage(DragImageRef, FloatSize)
{
    return nullptr;
}

DragImageRef createDragImageFromImage(Image*, ImageOrientation, GraphicsClient*, float)
{
    return nullptr;
}

void deleteDragImage(DragImageRef)
{
}

// ============================================================================
// Cursor (Cursor.h). ensurePlatformCursor is a private no-op: no platform
// cursor is materialized in the render-only path.
// ============================================================================
void Cursor::ensurePlatformCursor() const
{
}

// SharedCursor (Cursor.h, PLATFORM(WIN)). Destructor no-op (does not own the HCURSOR lifecycle here).
SharedCursor::~SharedCursor()
{
}

// ============================================================================
// Icon (graphics/Icon.h). Destructor + paint are no-ops; file-chooser icons
// are not rendered in this port.
// ============================================================================
Icon::~Icon()
{
}

void Icon::paint(GraphicsContext&, const FloatRect&)
{
}

// ============================================================================
// SharedMemory (SharedMemory.h). No real shared-memory backing in the render
// DLL — allocate/map/createHandle return null/nullopt. RISKY: any IPC/shared
// surface path that depends on these will get null and must handle it.
// ============================================================================
SharedMemory::~SharedMemory()
{
}

RefPtr<SharedMemory> SharedMemory::allocate(size_t)
{
    return nullptr;
}

RefPtr<SharedMemory> SharedMemory::map(Handle&&, Protection, CopyOnWrite)
{
    return nullptr;
}

std::optional<SharedMemory::Handle> SharedMemory::createHandle(Protection)
{
    return std::nullopt;
}

// ============================================================================
// SystemFontDatabase (graphics/SystemFontDatabase.h). singleton returns a
// static local; platformInvalidate is a no-op.
// ============================================================================
SystemFontDatabase& SystemFontDatabase::singleton()
{
    // SystemFontDatabase() is protected; expose it via a trivial derived type
    // so the static local single instance can be constructed.
    struct ConstructibleSystemFontDatabase : SystemFontDatabase {
        ConstructibleSystemFontDatabase() : SystemFontDatabase() { }
    };
    static NeverDestroyed<ConstructibleSystemFontDatabase> database;
    return database.get();
}

// Apotheosis: the returned family MUST be a real, non-null atom, and this stub used to return a
// null one (AtomString()) together with a size of 0. Both are fatal, for two different reasons.
//
// The family: StyleFontFamily.cpp turns a system-font shorthand into a single-value font family --
//
//     if (isSystemFontShorthand(valueID)) {
//         auto family = SystemFontDatabase::singleton().systemFontShorthandFamily(lowerFontShorthand(valueID));
//         ASSERT(!family.isEmpty());                                    // compiled out in release
//         return { WTF::move(family), FontFamilyKind::Generic };
//     }
//
// -- and nothing downstream checks it again. The null atom is installed as familyAt(0) of a
// FontCascadeDescription and is then hashed by CSSFontFaceSet::fontFace() ->
// HashMap::find() -> ASCIICaseInsensitiveHash::hash(const StringImpl*), whose only guard is an
// ASSERT that release builds also compile out, so it calls StringImpl::is8Bit() on nullptr. On the
// bench that is an AV reading 0x10, symbolized 2026-09-18 from a UEF stack taken while habr.com
// was loading: RenderText.cpp:425 -> FontCascadeFonts::glyphDataForVariant -> CSSFontSelector ->
// CSSFontFaceSet::fontFace. See Doc/FONT-NULL-FAMILY-CRASH.md.
//
// The size: FontPlatformData(0, ...) IS the hash table's emptyValue(), and HashTable::validateKey
// release-asserts on it, so a size of 0 is fatal on its own even with a good family name.
//
// So answer with something usable, exactly as SystemFontDatabaseGLib.cpp does -- it falls back to
// standardFamily when the platform has no system font name. This port renders every family through
// the bundled font anyway (see stubs-font-uwp.cpp), so the name only has to be a valid atom:
// -webkit-standard is the one WebCore's own fallback machinery uses for "no family requested".
SystemFontDatabase::SystemFontShorthandInfo SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand)
{
    return SystemFontShorthandInfo { WebKitFontFamilyNames::standardFamily, 16, normalWeightValue() };
}

void SystemFontDatabase::platformInvalidate()
{
}

// ============================================================================
// DisplayRefreshMonitorWin (graphics/win/DisplayRefreshMonitorWin.h). No display
// link in this port — return null. RISKY: animation/refresh driving that relies
// on a real monitor will get null.
// ============================================================================
RefPtr<DisplayRefreshMonitorWin> DisplayRefreshMonitorWin::create(PlatformDisplayID)
{
    return nullptr;
}

// ============================================================================
// PlatformKeyboardEvent (PlatformKeyboardEvent.h). Type/Modifier are aliases of
// PlatformEventType / PlatformEventModifier (hence the mangled enum names).
// No live keyboard state in the render path: empty modifier set, no-op disambiguate.
// ============================================================================
OptionSet<PlatformEvent::Modifier> PlatformKeyboardEvent::currentStateOfModifierKeys()
{
    return { };
}

void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type, bool)
{
}

// ============================================================================
// GraphicsLayer (graphics/GraphicsLayer.h). Type == GraphicsLayerType. The
// non-compositing render path must never construct a GraphicsLayer.
// GPU build (USE_TEXTURE_MAPPER) gets its real implementation from GraphicsLayerTextureMapper.cpp ->
// here we only provide a stub for the software build (no texmap), otherwise it collides with the real
// implementation as a duplicate symbol (the M2 link hits a duplicate).
// ============================================================================
#if !USE(TEXTURE_MAPPER)
Ref<GraphicsLayer> GraphicsLayer::create(GraphicsLayerFactory*, GraphicsLayerClient&, Type)
{
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

// ============================================================================
// ImageAdapter (graphics/ImageAdapter.h). loadPlatformResource returns the
// shared null Image (1x1-empty equivalent) so callers get a valid Ref instead
// of crashing; invalidate is a no-op.
// ============================================================================
Ref<Image> ImageAdapter::loadPlatformResource(const char*)
{
    return Ref { Image::nullImage() };
}

void ImageAdapter::invalidate()
{
}

// ============================================================================
// HTMLSelectElement (html/HTMLSelectElement.h). No platform-specific keydown
// handling: report "not handled".
// ============================================================================
bool HTMLSelectElement::platformHandleKeydownEvent(KeyboardEvent*)
{
    return false;
}

// ============================================================================
// FontCustomPlatformData (graphics/FontCustomPlatformData.h). No custom font
// loading in the UWP App Container (AddFontMemResourceEx is desktop-only).
// ============================================================================
FontCustomPlatformData::~FontCustomPlatformData() = default;

// Apotheosis: report every webfont format as unsupported. This used to claim
// truetype/opentype/woff/svg while create() below returns nullptr and
// fontPlatformData() is RELEASE_ASSERT_NOT_REACHED — i.e. the port advertised a
// capability it does not have. The cost was real: a page whose text is styled
// only with a downloadable face (ya.ru) rendered blank, because WebKit accepted
// the source, downloaded it, held the text in the font-display block period and
// then had no usable face to swap in. Saying "no" up front makes WebKit skip the
// source before the download and fall through to the next src entry and finally
// to the local last-resort font, so the text is drawn with a real face.
//
// Sources declared without a format() hint are not routed through here; those
// still download and then fail at create(), which is the ordinary failure path.
// Revisit when webfonts are wired to FreeType (FT_New_Memory_Face +
// cairo_ft_font_face_create_for_ft_face) — then this can start returning true
// for the formats actually implemented.
bool FontCustomPlatformData::supportsFormat(const String&)
{
    return false;
}

bool FontCustomPlatformData::supportsTechnology(const FontTechnology&)
{
    return false;
}

FontPlatformData FontCustomPlatformData::fontPlatformData(const FontDescription&, bool, bool, const FontCreationContext&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

// ============================================================================
// FontCache (graphics/FontCache.h). No GDI font enumeration in App Container;
// return empty/safe defaults. lastResortFallbackFont returns the system UI font
// at the requested size via a Cairo toy font face.
// ============================================================================
Vector<String> FontCache::systemFontFamilies()
{
    return { };
}

bool FontCache::isSystemFontForbiddenForEditing(const String&)
{
    return false;
}

Vector<FontSelectionCapabilities> FontCache::getFontSelectionCapabilitiesInFamily(const AtomString&, AllowUserInstalledFonts)
{
    return { };
}

Ref<Font> FontCache::lastResortFallbackFont(const FontDescription& fontDescription)
{
    // No GDI font enumeration in App Container. Return a size-only FontPlatformData;
    // Cairo will use its toy font face as fallback.
    FontPlatformData fpData(fontDescription.computedSize(), false, false);
    return Font::create(fpData);
}

// Screen functions (PlatformScreen.h) moved to webcore-driver-stubs.cpp
// to avoid duplicate symbol errors with lld-link.

// ============================================================================
// Font platform lifecycle (graphics/Font.h). Real metrics from the Cairo FT
// scaled font (no fontconfig): ascent/descent from cairo_font_extents, cap/x
// height approximated from "T"/"x" text extents, space width from " " extents.
// Falls back to zeros if the scaled font is missing or in error state.
// ============================================================================
void Font::platformInit()
{
    m_syntheticBoldOffset = m_platformData.syntheticBold() ? 1.0f : 0.f;
    m_fontMetrics.reset();
    m_avgCharWidth = 0;
    m_maxCharWidth = 0;

    if (!m_platformData.size())
        return;

    cairo_scaled_font_t* scaledFont = m_platformData.scaledFont();
    if (!scaledFont || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS)
        return;

    cairo_font_extents_t fontExtents { };
    cairo_scaled_font_extents(scaledFont, &fontExtents);

    float ascent = narrowPrecisionToFloat(fontExtents.ascent);
    float descent = narrowPrecisionToFloat(fontExtents.descent);
    float lineGap = narrowPrecisionToFloat(fontExtents.height - fontExtents.ascent - fontExtents.descent);

    // Approximate cap/x height from text extents (FreeType would be more precise).
    auto heightOfCharacter = [&](const char* character) -> float {
        cairo_text_extents_t textExtents { };
        cairo_scaled_font_text_extents(scaledFont, character, &textExtents);
        // narrowPrecisionToFloat is only specialized for double — keep the
        // intermediate double so the generic (undefined) template isn't picked.
        double height = platformData().orientation() == FontOrientation::Horizontal
            ? textExtents.height : textExtents.width;
        return narrowPrecisionToFloat(height);
    };
    float capHeight = heightOfCharacter("T");
    float xHeight = heightOfCharacter("x");

    m_fontMetrics.setAscent(ascent);
    m_fontMetrics.setDescent(descent);
    m_fontMetrics.setCapHeight(capHeight);
    m_fontMetrics.setLineSpacing(lroundf(ascent) + lroundf(descent) + lroundf(lineGap));
    m_fontMetrics.setLineGap(lineGap);
    m_fontMetrics.setXHeight(xHeight);

    cairo_text_extents_t spaceExtents { };
    cairo_scaled_font_text_extents(scaledFont, " ", &spaceExtents);
    m_spaceWidth = narrowPrecisionToFloat(platformData().orientation() == FontOrientation::Horizontal
        ? spaceExtents.x_advance : -spaceExtents.y_advance);

    apotheosisGlyphLog("platformInit: size=%.1f asc=%.1f desc=%.1f lineGap=%.1f cap=%.1f x=%.1f space=%.1f isFT=%d",
        m_platformData.size(), ascent, descent, lineGap, capHeight, xHeight, m_spaceWidth,
        cairo_ft_scaled_font_lock_face(scaledFont) ? 1 : 0);
    cairo_ft_scaled_font_unlock_face(scaledFont);
}

void Font::platformCharWidthInit()
{
    m_avgCharWidth = 0;
    m_maxCharWidth = 0;
    initCharWidths();
}

void Font::platformDestroy()
{
}

RefPtr<Font> Font::platformCreateScaledFont(const FontDescription&, float scaleFactor) const
{
    float scaledSize = scaleFactor * m_platformData.size();
    return Font::create(FontPlatformData::cloneWithSize(m_platformData, scaledSize));
}

RefPtr<Font> Font::platformCreateHalfWidthFont() const
{
    return nullptr;
}

// ============================================================================
// Theme (platform/Theme.h). singleton returns a default Theme via derived class
// (Theme has protected constructor, no pure virtuals).
// ============================================================================
struct StubTheme : Theme { };
Theme& Theme::singleton()
{
    static StubTheme instance;
    return instance;
}

// ============================================================================
// RenderTheme (rendering/RenderTheme.h). singleton returns a default RenderTheme.
// RenderTheme has protected constructor, no pure virtuals.
// ============================================================================
struct StubRenderTheme : RenderTheme { };
RenderTheme& RenderTheme::singleton()
{
    static StubRenderTheme instance;
    return instance;
}

// ============================================================================
// FontCustomPlatformData (graphics/FontCustomPlatformData.h). Static factory
// methods — no custom font loading in UWP App Container.
// ============================================================================
RefPtr<FontCustomPlatformData> FontCustomPlatformData::create(SharedBuffer&, const String&)
{
    return nullptr;
}

RefPtr<FontCustomPlatformData> FontCustomPlatformData::createMemorySafe(SharedBuffer&, const String&)
{
    return nullptr;
}

// ============================================================================
// EventHandler (page/EventHandler.h). No-op event handling — both methods are
// private but need definitions for the link. Return "not handled".
// ============================================================================
bool EventHandler::eventActivatedView(const PlatformMouseEvent&) const
{
    return false;
}

HandleUserInputEventResult EventHandler::passMouseMoveEventToSubframe(MouseEventWithHitTestResults&, LocalFrame&, HitTestResult*)
{
    return { false };
}

// Screen functions (screenDepth, screenAvailableRect, screenRect) moved to
// webcore-driver-stubs.cpp to avoid duplicate symbol errors with lld-link.

// ============================================================================
// MainThreadSharedTimer (platform/MainThreadSharedTimer.h).
//
// Apotheosis: a real timer. MainThreadSharedTimer is the one platform timer
// underneath ThreadTimers, so every WebCore::Timer on the main thread fires
// through it: ThreadTimers::setSharedTimer installs the fired function
// (platform/ThreadTimers.cpp:74) and ThreadTimers::updateSharedTimer arms it
// (platform/ThreadTimers.cpp:102) by calling setFireInterval below. While these
// three methods were empty no WebCore timer in this port ever fired, which
// silently disabled every deferred engine operation. The one that broke real
// sites is ScriptableDocumentParser::executeScriptsWaitingForStylesheetsSoon():
// its one-shot 0-second timer is the only thing that resumes an HTML parser
// parked on a script waiting for stylesheets, so a page with an inline <script>
// after a <link rel=stylesheet> in <head> stopped parsing at that script for
// good -- head parsed, no <body>, readyState stuck at Loading.
//
// PlatformWinUWP.cmake does not build platform/win/MainThreadSharedTimerWin.cpp:
// that implementation drives itself from a hidden HWND and WM_TIMER, and the
// engine thread has no window and no message pump. The generic upstream
// implementation (the !USE(CF) && !OS(WINDOWS) branch of
// platform/MainThreadSharedTimer.cpp) uses a RunLoop::Timer instead, which is
// what this port can support: the driver pumps RunLoop::mainSingleton() on the
// engine thread, and pumpLoop()'s own settle/watchdog RunLoop::Timers already
// tick there reliably every 50 ms.
//
// Two things differ from that upstream branch. The class declares no m_timer
// member under OS(WINDOWS), so the timer lives here as a file-local singleton;
// and fired() is public precisely so that a platform implementation can call it
// from a non-member context (see the FIXME on its declaration).
// ============================================================================
static RunLoop::Timer& mainThreadSharedRunLoopTimer()
{
    // Created on first use, i.e. on the engine thread the first time WebCore arms
    // a timer, and bound to the run loop the driver actually pumps -- verified
    // equal to RunLoop::currentSingleton() there, with isMainThread()=1. Never
    // destroyed, like the singleton whose fired() it drives.
    static NeverDestroyed<RunLoop::Timer> timer(Ref { RunLoop::mainSingleton() },
        "MainThreadSharedTimer"_s,
        WTF::Function<void()> { [] { MainThreadSharedTimer::singleton().fired(); } });
    return timer.get();
}

void MainThreadSharedTimer::setFireInterval(Seconds interval)
{
    // ThreadTimers passes an interval relative to now, already clamped to >= 0
    // there; RunLoop::TimerBase::startOneShot clamps again.
    ASSERT(m_firedFunction);
    mainThreadSharedRunLoopTimer().startOneShot(interval);
}

void MainThreadSharedTimer::stop()
{
    mainThreadSharedRunLoopTimer().stop();
}

void MainThreadSharedTimer::invalidate()
{
}

// ============================================================================
// ScrollbarTheme (platform/ScrollbarTheme.h). nativeTheme must return the
// correct theme subclass. No scrollbar interaction in the render DLL; return
// the base class (all virtuals have default bodies).
// ============================================================================
ScrollbarTheme& ScrollbarTheme::nativeTheme()
{
    static ScrollbarTheme instance;
    return instance;
}


// ============================================================================
// ComplexTextController (platform/graphics/ComplexTextController.h). No
// complex text shaping in the render path.
// ============================================================================
void ComplexTextController::collectComplexTextRunsForCharacters(std::span<const char16_t>, unsigned, const Font*)
{
}

// ============================================================================

// FontPlatformData::familyName - needed for linker (not compiled in x64 build)
String FontPlatformData::familyName() const
{
    return String();
}
// Font (platform/graphics/Font.h). Platform font queries — no system font
// information available in App Container.
// ============================================================================
// Pitch comes from the FT face flags directly. Upstream defers to
// FontPlatformData::isFixedPitch(), which lives in FontPlatformDataFreeType.cpp —
// not part of this port's source list, so querying FT_IS_FIXED_WIDTH here avoids
// an unresolved external.
void Font::determinePitch()
{
    m_treatAsFixedPitch = false;

    cairo_scaled_font_t* scaledFont = m_platformData.scaledFont();
    if (!scaledFont || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS)
        return;

    if (FT_Face face = cairo_ft_scaled_font_lock_face(scaledFont)) {
        m_treatAsFixedPitch = FT_IS_FIXED_WIDTH(face);
        cairo_ft_scaled_font_unlock_face(scaledFont);
    }
}

// Coverage query against the real FreeType face. Upstream uses FcFreeTypeCharIndex;
// FT_Get_Char_Index is the pure-FreeType equivalent (no fontconfig in this port).
bool Font::platformSupportsCodePoint(char32_t character, std::optional<char32_t> variation) const
{
    cairo_scaled_font_t* scaledFont = m_platformData.scaledFont();
    if (!scaledFont || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS)
        return false;

    FT_Face face = cairo_ft_scaled_font_lock_face(scaledFont);
    if (!face)
        return false;

    bool supported = variation
        ? !!FT_Face_GetCharVariantIndex(face, character, variation.value())
        : !!FT_Get_Char_Index(face, character);

    cairo_ft_scaled_font_unlock_face(scaledFont);
    return supported;
}

// ============================================================================
// GlyphPage (platform/graphics/GlyphPage.h). Maps characters to glyph indices
// via the FreeType face behind the Cairo scaled font. Upstream
// (GlyphPageTreeNodeFreeType.cpp) uses FcFreeTypeCharIndex, but fontconfig is
// deliberately not linked in this port — FT_Get_Char_Index is the equivalent
// pure-FreeType lookup. Returning false here (the previous stub) meant no
// character ever mapped to a glyph, so pages rendered background-only.
// ============================================================================
bool GlyphPage::fill(std::span<const char16_t> buffer)
{
    const Font& font = this->font();
    cairo_scaled_font_t* scaledFont = font.platformData().scaledFont();
    if (!scaledFont || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
        apotheosisGlyphLog("fill: no/invalid scaledFont");
        return false;
    }

    FT_Face face = cairo_ft_scaled_font_lock_face(scaledFont);
    if (!face) {
        apotheosisGlyphLog("fill: lock_face returned null (not an FT scaled font?)");
        return false;
    }

    std::optional<Glyph> zeroWidthSpaceGlyphValue;
    auto zeroWidthSpaceGlyph = [&] {
        if (!zeroWidthSpaceGlyphValue)
            zeroWidthSpaceGlyphValue = FT_Get_Char_Index(face, zeroWidthSpace);
        return *zeroWidthSpaceGlyphValue;
    };

    bool haveGlyphs = false;
    unsigned bufferOffset = 0;
    for (unsigned i = 0; i < GlyphPage::size; i++) {
        if (bufferOffset == buffer.size())
            break;
        char32_t character;
        U16_NEXT(buffer, bufferOffset, buffer.size(), character);

        Glyph glyph = FT_Get_Char_Index(face, FontCascade::treatAsSpace(character) ? space : character);
        // If the font lacks a Default_Ignorable character, substitute zero-width space.
        if (!glyph && (isDefaultIgnorableCodePoint(character) || isControlCharacter(character)))
            glyph = zeroWidthSpaceGlyph();

        if (!glyph)
            setGlyphForIndex(i, 0, ColorGlyphType::Outline);
        else {
            setGlyphForIndex(i, glyph, ColorGlyphType::Outline);
            haveGlyphs = true;
        }
        // Apotheosis: log only the misses. This used to trace every code point, which is
        // 256 fopen/fclose per glyph page -- tens of thousands of them on a real page, slow
        // enough inside the AppContainer to look like a hang. A miss (glyph 0 -> tofu) is the
        // only diagnostically interesting case anyway, so this keeps the data that matters
        // for the CJK tofu investigation and costs nothing on healthy text.
        if (!glyph)
            apotheosisGlyphLog("fill: MISS i=%u cp=U+%04X", i, character);
    }

    cairo_ft_scaled_font_unlock_face(scaledFont);
    return haveGlyphs;
}

// ============================================================================
// FontCache (platform/graphics/FontCache.h). No system font enumeration.
// platformAlternateFamilyName returns empty, platformInit no-op,
// createFontPlatformData returns null.
// ============================================================================
ASCIILiteral FontCache::platformAlternateFamilyName(const String&)
{
    return { };
}

// Apotheosis: stub for glyph display list check (no HarfBuzz)
bool FontCascade::canUseGlyphDisplayList(const RenderStyle&)
{
    return false;
}

} // namespace WebCore


