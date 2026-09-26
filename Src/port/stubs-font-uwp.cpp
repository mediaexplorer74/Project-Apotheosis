#include "config.h"
#include "FontCache.h"
#include "FontCascade.h"
#include "FontCascadeDescription.h"
#include "FontPlatformData.h"
#include "Font.h"

#if PLATFORM(WIN) && USE(CAIRO) && defined(WK_WINUWP)

#include <wtf/HashFunctions.h>

namespace WebCore {

void FontCache::platformInit()
{
}

void FontCache::platformInvalidate()
{
}

std::unique_ptr<FontPlatformData> FontCache::createFontPlatformData(const FontDescription&, const AtomString&, const FontCreationContext&, OptionSet<FontLookupOptions>)
{
    return nullptr;
}

RefPtr<Font> FontCache::systemFallbackForCharacterCluster(const FontDescription& description, const Font&, IsForPlatformFont, PreferColoredFont, StringView)
{
    // Apotheosis: the primary bundled face (segoeui.ttf) is Latin-only, and there is no
    // OS font backend in the App Container. Route any cluster the primary font cannot
    // cover (CJK, kana, symbols) to the bundled SimHei face, which contains Latin + CJK
    // glyphs. fontForPlatformData caches the result, so this is cheap after the first hit.
    // The forced-face override lives in FontPlatformData.cpp (file-local) to avoid a
    // FontPlatformData.h change (which would invalidate ~300 WebCore TUs).
    extern void apotheosisSetForcedBundledFontName(const char*);
    // Apotheosis 2026-09-18: computedSize() is 0 until style resolution sets it
    // (FontDescription.h:166), and FontDataCacheKeyTraits::emptyValue() IS
    // FontPlatformData(0.f, false, false) (FontCache.cpp:96-100). Building the platform data from a
    // zero size therefore hands fontForPlatformData() the hash table's own EMPTY sentinel, which
    // HashTable::validateKey() refuses with a RELEASE assert (HashTable.h:534) -> WTFCrashWithInfo ->
    // abort(). Measured: scrolling dzen.ru aborted the process with exit code 3 from exactly this
    // call -- SIGABRT stack, WTFCrashWithInfo called from FontCache::fontForPlatformData+0x345.
    // A release assert is not conditional on ASSERT_ENABLED, so this is a crash and not a debug trip.
    // With no usable size there is no fallback face to offer, so decline; the caller handles nullptr.
    const float size = description.computedSize();
    if (!(size > 0.0f)) // also rejects NaN
        return nullptr;
    apotheosisSetForcedBundledFontName("simhei.ttf");
    auto platformData = FontPlatformData(size, false, false);
    apotheosisSetForcedBundledFontName(nullptr);
    // A face that could not be built leaves m_scaledFont null. That is not the empty sentinel (the
    // size is non-zero) so it would not assert, but caching a font-less platform data is still wrong.
    if (!platformData.scaledFont())
        return nullptr;
    return fontForPlatformData(platformData);
}

unsigned FontPlatformData::hash() const
{
    return PtrHash<cairo_scaled_font_t*>::hash(m_scaledFont.get());
}

bool FontPlatformData::platformIsEqual(const FontPlatformData& other) const
{
    return m_scaledFont == other.m_scaledFont;
}

// Apotheosis: this definition is compiled ONLY when USE(HARFBUZZ) is on, which is the precondition the
// comment below has always stated. Upstream provides the same function from exactly one of two places:
// FontCascade.cpp, under `#if !PLATFORM(COCOA) && !USE(HARFBUZZ)`, or FontCairoHarfbuzzNG.cpp when
// HarfBuzz is on. With HarfBuzz on, both are absent from our build -- FontCascade.cpp's branch is
// disabled and we do not compile the HarfBuzzNG font backend -- so this fills the gap.
// With HarfBuzz OFF, FontCascade.cpp's branch IS compiled and this collides with it:
//   lld-link: error: duplicate symbol: WebCore::FontCascade::fontForCombiningCharacterSequence(...)
//   >>> defined at Src\port\stubs-font-uwp.cpp:53
//   >>> defined at ...UnifiedSource-3c72abbe-27.cpp.obj
// which is exactly what the x64 line hit on 2026-08-20, because x64 has USE_HARFBUZZ off while ARM32
// has it on. That asymmetry is not intentional and is documented in Doc/HARFBUZZ-ICU-DIVERGENCE.md: an
// inverted architecture gate in OptionsWinUWP.cmake asks x64 for a HarfBuzz ICU component it cannot
// have, so HarfBuzz_FOUND is false there. When that is fixed and both lines have HarfBuzz, this guard
// becomes always-true and can go away.
#if USE(HARFBUZZ)
RefPtr<const Font> FontCascade::fontForCombiningCharacterSequence(StringView stringView) const
{
    // Apotheosis: upstream provides this either from FontCascade.cpp (USE(HARFBUZZ) off)
    // or FontCairoHarfbuzzNG.cpp (USE(HARFBUZZ) on, shipped by FreeType.cmake). This port
    // has USE_HARFBUZZ=ON (LocaleICU.cpp needs hb-icu.h) but does not compile the
    // HarfBuzzNG font backend, so provide the plain glyphDataForCharacter fallback here.
    ASSERT(stringView.length() > 0);
    char32_t baseCharacter = *stringView.codePoints().begin();
    GlyphData baseCharacterGlyphData = glyphDataForCharacter(baseCharacter, false, NormalVariant);

    if (!baseCharacterGlyphData.isValid())
        return nullptr;
    return baseCharacterGlyphData.font.get();
}
#endif // USE(HARFBUZZ)

}

#endif
