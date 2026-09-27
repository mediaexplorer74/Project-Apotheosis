#include "config.h"
#include "FontCache.h"
#include "FontCascade.h"
#include "FontCascadeDescription.h"
#include "FontPlatformData.h"
#include "Font.h"

#if PLATFORM(WIN) && USE(CAIRO) && defined(WK_WINUWP)

#include <wtf/HashFunctions.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace WebCore {

void FontCache::platformInit()
{
}

void FontCache::platformInvalidate()
{
}

namespace {

// Apotheosis 2026-09-27, diagnostic only. WebCoreDriver.cpp's WebCorePortTrace is file-local
// (llvm-nm reports it as ?WebCorePortTrace@?A0xC8417F77@@..., i.e. an anonymous namespace), so
// it cannot be called from another translation unit -- declaring it produces LNK2019 whichever
// linkage is chosen. Every other port trace (portLoaderTraceBoth) is `static` for the same
// reason. Rather than re-plumb a trace API through the driver, this reproduces the driver's
// own destination rule: it derives the path from FONTCONFIG_FILE and appends to
// port-trace.txt beside it. Keeps this investigation to a single-TU rebuild.
void apotheosisFontTrace(const char* fmt, ...)
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

} // namespace

// Apotheosis 2026-09-27 — MEASURED, and it is NOT the cause of the missing text.
//
// This used to `return nullptr` unconditionally. Implementing it for real changes nothing on screen:
// example.com still paints zero glyphs. Instrumenting it (traces below) settles the question that
// two earlier diagnoses got wrong. On a real load of https://example.com/ this function is called
// and SUCCEEDS every single time:
//
//   FONT cfpd: enter family='system-ui' size=24.00 isNull=0 isEmpty=0
//   FONT cfpd: picked segoeui.ttf
//   FONT cfpd: OK segoeui.ttf at 24.00px
//   FONT cfpd: enter family=''        size=16.00 isNull=0 isEmpty=1
//   FONT cfpd: OK segoeui.ttf at 16.00px
//   FONT fallback: OK simhei at 24.00px
//
// So a face is built, a scaled font exists, and the cache is warm — and still no glyph reaches the
// screen. The failure is strictly downstream of font selection, in shaping or rasterisation.
// Two things worth keeping from that trace: the family arrives EMPTY (isEmpty=1) far more often
// than not, which is the same empty-family condition Doc/Archive/FONT-NULL-FAMILY-CRASH.md documents
// as a null-AtomString producer; and an empty family must still resolve to a face rather than
// nullptr, which is what the segoeui.ttf default below is for.
//
// The bundled-face machinery is real and works: apotheosisSetForcedBundledFontName() ->
// apotheosisBundledFontFaceForName() (FreeType, cairo-ft) -> apotheosisCreateScaledFont().
//
// Deliberately minimal: five bundled faces, regular style only, no foreign formats. Weight and italic
// are ignored because no bold/italic cut is bundled; a browser that renders bold text as regular is
// a cosmetic problem, whereas one that renders no text at all is fatal.

namespace {

// ASCII lower-casing, and folding of spaces/quotes, so that "Times New Roman", "times new roman"
// and "TIMESNEWROMAN" all land on the same entry without pulling ICU into this translation unit.
void apotheosisNormalizeFamily(const char* family, char* out, size_t outLen)
{
    size_t j = 0;
    if (family) {
        for (const char* p = family; *p && j + 1 < outLen; ++p) {
            char c = *p;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c == ' ' || c == '"' || c == '\'')
                continue;
            out[j++] = c;
        }
    }
    out[j] = 0;
}

// The five faces the port ships inside Assets\fonts.
const char* apotheosisPickBundledFontFile(const char* family)
{
    char name[128];
    apotheosisNormalizeFamily(family, name, sizeof(name));

    // A non-ASCII family name is a CJK/localised name (SimSun, Microsoft YaHei, PingFang, ...).
    // segoeui.ttf and the other Latin faces cannot cover it; simhei.ttf contains Latin + CJK, so
    // send every such request there rather than letting it fail. This matters for real sites:
    // the home page uses font-family:sans-serif, but zh/ja/ko pages ask by localised name.
    for (const char* p = name; *p; ++p) {
        if (static_cast<unsigned char>(*p) >= 0x80)
            return "simhei.ttf";
    }

    auto is = [name](const char* candidate) { return !strcmp(name, candidate); };

    // Monospace first, then serif: a browser resolves the generic families in a fixed order, and
    // the remaining two faces are Arial (a sans) and SimHei (the CJK catch-all).
    static const char* kMono[] = { "monospace", "courier", "couriernew", "consolas", "lucidaconsole", "monaco", "menlo", "dejavusansmono", "liberationmono" };
    for (const char* c : kMono) {
        if (is(c))
            return "cour.ttf";
    }

    static const char* kSerif[] = { "serif", "times", "timesnewroman", "georgia", "garamond", "cambria", "bookantiqua", "palatino", "constantia", "liberationserif" };
    for (const char* c : kSerif) {
        if (is(c))
            return "times.ttf";
    }

    static const char* kSans[] = { "sans-serif", "sansserif", "segoeui", "segoeuix", "arial", "helvetica", "verdana", "tahoma", "calibri", "roboto", "opensans", "lato", "system-ui", "systemui", "ui-sans-serif" };
    for (const char* c : kSans) {
        if (is(c))
            return "segoeui.ttf";
    }

    // Unknown family: browsers fall back to the user's default family rather than rendering
    // nothing, so an unrecognised name must not return nullptr either.
    return "segoeui.ttf";
}

} // namespace

// Defined in FontPlatformData.cpp; the declaration is repeated here so that this translation unit
// does not need that header (which would drag in the cairo/ft include soup).
extern void apotheosisSetForcedBundledFontName(const char*);

std::unique_ptr<FontPlatformData> FontCache::createFontPlatformData(const FontDescription& description, const AtomString& familyName, const FontCreationContext&, OptionSet<FontLookupOptions>)
{
    // Apotheosis 2026-09-18, kept verbatim: computedSize() is 0 until style resolution sets it, and
    // FontDataCacheKeyTraits::emptyValue() IS FontPlatformData(0.f, false, false). Handing the hash
    // table its own EMPTY sentinel makes HashTable::validateKey() fire a RELEASE assert ->
    // WTFCrashWithInfo -> abort(). Measured: scrolling dzen.ru aborted the process with exit code 3
    // from exactly this call. A release assert is not conditional on ASSERT_ENABLED. So refuse a
    // non-positive or NaN size; the caller handles nullptr.
    const float size = description.computedSize();
    apotheosisFontTrace("FONT cfpd: enter family='%s' size=%.2f isNull=%d isEmpty=%d",
        familyName.string().utf8().data(), double(size), int(familyName.isNull()), int(familyName.isEmpty()));
    if (!(size > 0.0f)) {
        apotheosisFontTrace("FONT cfpd: decline, size non-positive or NaN");
        return nullptr;
    }

    // The family is the cache's own lookup key and arrives already resolved. This WebKit's
    // FontDescription carries no family member at all -- it lives entirely in this argument --
    // so there is nothing to fall back to, and an empty name simply misses every table and
    // lands on segoeui.ttf.
    // AtomString is not a String subclass in this WebKit; .string() is its accessor (see
    // BitmapImage.h), and the .string().utf8() shape is the idiom used across WebCore.
    const char* file = apotheosisPickBundledFontFile(familyName.string().utf8().data());
    apotheosisFontTrace("FONT cfpd: picked %s", file);

    // Force the requested face, build the platform data, then release the override immediately.
    // The override is a file-local global in FontPlatformData.cpp, so it must never be left set.
    apotheosisSetForcedBundledFontName(file);
    auto platformData = std::make_unique<FontPlatformData>(size, false, false);
    apotheosisSetForcedBundledFontName(nullptr);

    // A face that failed to build leaves m_scaledFont null. Caching a font-less platform data
    // would poison every later lookup for this family, so decline instead.
    if (!platformData || !platformData->scaledFont()) {
        apotheosisFontTrace("FONT cfpd: face FAILED for %s at %.2fpx", file, double(size));
        return nullptr;
    }

    apotheosisFontTrace("FONT cfpd: OK %s at %.2fpx", file, double(size));
    return platformData;
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
    apotheosisFontTrace("FONT fallback: enter size=%.2f", double(size));
    if (!(size > 0.0f)) // also rejects NaN
        return nullptr;
    apotheosisSetForcedBundledFontName("simhei.ttf");
    auto platformData = FontPlatformData(size, false, false);
    apotheosisSetForcedBundledFontName(nullptr);
    // A face that could not be built leaves m_scaledFont null. That is not the empty sentinel (the
    // size is non-zero) so it would not assert, but caching a font-less platform data is still wrong.
    if (!platformData.scaledFont()) {
        apotheosisFontTrace("FONT fallback: simhei FAILED at %.2fpx", double(size));
        return nullptr;
    }
    apotheosisFontTrace("FONT fallback: OK simhei at %.2fpx", double(size));
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
