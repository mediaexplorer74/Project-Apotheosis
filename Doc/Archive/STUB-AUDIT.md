# The `stubs-*` audit: what is a stub, what is a lie, and what it costs

Written 2026-09-18, at the maintainer's request: *"Стабы — это заглушки, часто содержащие недоделки?
Впору перепроверить их все, чтобы скорректировать прогноз по времени приведения браузера Apotheosis в
чувство."* The premise is correct and the project has already paid for it once: the most expensive defect
so far (§10f, `DZEN-SCROLL-DEATH.md`) was a stub that answered *success* and stored nothing.

This document is the inventory. It is written from the sources, cross-checked against the build, and it
says plainly which claims are measured and which are inference.

## 1. The classification that matters

A stub is tolerable only when its caller is built to survive it. Three classes:

| Class | Behaviour | Cost |
|---|---|---|
| **HONEST FAILURE** | Returns an error the caller is designed to handle — a failed load, an absent feature the page can feature-detect. | The API is simply absent. Acceptable. |
| **SILENT LIE** | Returns something that looks like success and is empty, fake, or wrong. | A page loops, mis-syncs, or renders wrong, with **no trace anywhere**. This is the class that must be emptied. |
| **NEVER REACHED** | No caller in this configuration. | Nothing today; a landmine the day a page or an upstream change reaches it. |

The distinguishing question is never "is this function implemented" but **"if this function is reached,
does anybody find out?"** §10f was invisible because `EmptyStorageArea::setItem` had an empty body and
returned success.

## 2. Where these files are actually compiled

Two lists, and a file in neither is dead code. Measured, not assumed:

**Compiled into `WebCore.dll`** — `WebKit/Source/WebCore/PlatformWinUWP.cmake:84-95`:

```
stubs-ax.cpp        stubs-crypto.cpp    stubs-font-uwp.cpp  stubs-gdi-uwp.cpp
stubs-loader.cpp    stubs-network.cpp   stubs-other.cpp     stubs-pasteboard.cpp
stubs-screen-uwp.cpp  stubs-sqlite.cpp
```

**Compiled into the driver archive** (`WebCoreDriver-gpu-<arch>.lib`) —
`Src/port/link-driver-gpu-x64.ps1:21-26`:

```
WebCoreDriver  PortChromeClient  LoadingFrameLoaderClient  PortPlatformStrategies
PortNetworkStorageSession  PortStorage  webcore-driver-stubs
stubs-crypto  stubs-pasteboard  stubs-network  stubs-ax  stubs-other  stubs-loader
```

**Compiled by nobody** — `chromeclient-stubs.cpp`, `webcore-internal-stubs.cpp`, `stub-driver.cpp`. See
§4.1: one of them would be catastrophic if it ever entered a link line.

Six files are in **both** lists — `stubs-ax`, `stubs-crypto`, `stubs-loader`, `stubs-network`,
`stubs-other`, `stubs-pasteboard`. Each is compiled twice per architecture and both copies define the
same symbols; the linker keeps whichever archive it meets first. It is harmless today because the two
copies are the same source, but it is duplicated build time in a build where `ninja -j1` is mandatory, and
it is exactly the "умножение сущностей" `Doc/UNIFICATION.md` exists to remove.

## 3. Per-file verdict

| File | Verdict | What it actually is |
|---|---|---|
| `stubs-other.cpp` (653 ln) | **MISNAMED — it is the platform implementation file** | Fonts, glyphs, the shared timer, theme singletons. Four real implementations, plus the record of four silent lies that were found and repaired (below). |
| `stubs-font-uwp.cpp` (101 ln) | **The font backend, and the largest fidelity gap** | `createFontPlatformData` returns `nullptr` unconditionally: font selection is replaced by a forced-bundled-face override. |
| `stubs-network.cpp` (85 ln) | **Two honest-ish stubs, one with a security flavour** | `NetworkStateNotifier` always "online"; libpsl absent so the public-suffix list is empty. |
| `stubs-pasteboard.cpp` (220 ln) | **Correctly split: loud writes, silent reads** | Writes `RELEASE_ASSERT_NOT_REACHED()`; reads return benign empties. Clipboard does not work, by design and by assert. |
| `stubs-crypto.cpp` (348 ln) | **HONEST-AND-LOUD, plus a stale header** | Every WebCrypto operation asserts; the registry stays empty. `crypto.getRandomValues` is *not* affected — it is real (`BCryptGenRandom`). |
| `stubs-ax.cpp` (82 ln) | **NEVER REACHED for rendering; total loss for accessibility** | All no-ops, which is honest: there is no AX client to notify. |
| `stubs-screen-uwp.cpp` (32 ln) | **SILENT LIE, and duplicated** | Hardcoded 1080×1920 screen; the window is 1024×694. The same constants exist in `webcore-driver-stubs.cpp`. |
| `stubs-gdi-uwp.cpp` (141 ln) | **NEVER REACHED (if the FT backend is the only one compiled)** | Fake GDI: `GetTextMetricsW` returns invented metrics, `DeleteObject` returns success. |
| `stubs-sqlite.cpp` (299 ln) | **HONEST FAILURE** | Every open returns `SQLITE_CANTOPEN`; `SQLiteDatabase::open()` returns false and callers degrade. Not the §10f mechanism — the storage defect was in WebCore's client wiring, not here. |
| `stubs-loader.cpp` (18 ln) | **NOT A STUB** | `CurlSSLHandle::platformInitialize()` sets a real cipher list and EC curves. Misnamed file. |
| `webcore-driver-stubs.cpp` (263 ln) | **83 % commented-out residue, 17 % a duplicate** | Its only live code is the screen functions of §3.1. The rest is the original link-driven scratchpad, kept for its methodology note. |
| `chromeclient-stubs.cpp` (26 ln) | **NOT COMPILED — dead** | Barcode/face/text detectors. Something upstream already answers them. |
| `webcore-internal-stubs.cpp` (33 ln) | **NOT COMPILED — dead, and dangerous if revived** | `DocumentWriter::addData` is an empty body and `hasPlatformStrategies()` returns **false**. Both are load-bearing for the function of the engine; compiling this file would silently break loading. |
| `stub-driver.cpp` | **NOT COMPILED — dead** | Superseded by `webcore-driver-stubs.cpp`. |

## 4. Findings worth acting on, most expensive first

### 4.1 Fonts are not a stub, they are a substitute backend — and CSS `font-family` is ignored

This is the largest fidelity gap in the port and it is not filed under "unfinished".

* `FontCache::createFontPlatformData(...)` (`stubs-font-uwp.cpp:22`) returns `nullptr`
  **unconditionally**. That is the function through which WebCore turns a *family name* into a usable
  face.
* `FontCache::systemFallbackForCharacterCluster(...)` builds its fallback from a **filename**:
  `apotheosisSetForcedBundledFontName("simhei.ttf")`.
* The mechanism behind that name is a fork patch in
  `WebKit/Source/WebCore/platform/graphics/FontPlatformData.cpp:64-…`:
  `apotheosisBundledFontFaceForName()` reads the file from `APOTHEOSIS_FONTS_DIR`, loads it with
  FreeType, and hands back a `cairo_font_face_t`.

**Consequence, stated plainly:** the engine has a working text stack, but it selects faces by *file*, not
by family. A page that asks for `Roboto`, `Helvetica`, `Arial`, `serif` or a named system font gets the
bundled face (measured indirectly and consistently: `glyph.log`'s `platformInit:` lines carry real
non-zero metrics and `isFT=1`, and dzen.ru's Cyrillic text renders correctly — the stack works, it just
does not *choose*). Pages that depend on a family for layout or for meaning (icon fonts, metric-compatible
substitution) will therefore be wrong in ways that look like a CSS bug.

**Related, and separately fixable — webfonts are entirely absent.** `FontCustomPlatformData::supportsFormat`
and `supportsTechnology` return `false` for everything (`stubs-other.cpp:267-275`), and `create()` returns
`nullptr`. The comment there records that this was *deliberately* changed from a lie: the port used to
advertise truetype/opentype/woff/svg while `create()` returned nullptr, and the cost was that **ya.ru
rendered blank** — WebKit accepted the source, downloaded it, held the text in the `font-display` block
period and had no face to swap in. Returning `false` up front makes WebKit skip the source before the
download and fall through to the local last-resort font. **A page whose text is styled only with a
downloadable face still renders in the wrong face, but it renders.** `@font-face` pages (icon fonts
included) are therefore a known, bounded gap.

*Honest statement:* neither of these is a "silent lie" in the §10f sense — nothing loops. They are a
capability that is missing, discovered only by looking at the text. Effort: **L** for real font-family
matching (needs a family→file map, or fontconfig in the App Container), **M** for webfonts via
`FT_New_Memory_Face` + `cairo_ft_font_face_create_for_ft_face` — the comment in the source already names
both calls.

### 4.2 Dead code that would be actively harmful if revived

`webcore-internal-stubs.cpp` is compiled by nothing (verified: absent from
`PlatformWinUWP.cmake:84-95` and from every link script; the only references to it are in
`link-driver-x64.ps1` and `recompile-stubs-x64.ps1`, both of which belong to the retired
non-GPU line). It contains two bodies that would break the engine out loud:

```cpp
void DocumentWriter::addData(const SharedBuffer&) { }     // the port feeds bytes through this by hand
bool hasPlatformStrategies() { return false; }             // PortPlatformStrategies IS installed
```

Both are **false where the rest of the port says true**. `chromeclient-stubs.cpp` and `stub-driver.cpp`
are inert. Recommendation: delete all three, or move them under a clearly-named `archive/`, because the
failure mode of a revived copy is a silent one — `hasPlatformStrategies() == false` makes WebCore take
fallback paths that are indistinguishable from real ones at a glance.

### 4.3 The screen the engine believes in is not the screen it is drawing on

`screenRect` / `screenAvailableRect` return a hardcoded 1080×1920 (`stubs-other.cpp:59-68` *and*
`stubs-screen-uwp.cpp:14-23` — the same constants, twice), while the session's own diagnostic line reports
`contents=1024x694`. So `window.screen.*`, `@media (device-width)`, `@media (device-height)` and
`screen.colorDepth` answer for a phone that is not running the app.

This is a **SILENT LIE** by the classification above: a site that branches on screen size gets a confident
wrong answer, and nothing in the logs says so. It is also the reason `-Url`-driven tests can disagree with
the phone — the same engine reports different screen metrics per architecture only because the constant is
the same and the *window* is not.

Effort: **S** — plumb the real view size into these functions instead of a literal. The duplication should
go at the same time (one definition, in the driver, since `stubs-screen-uwp.cpp` exists only as the
WebCore-side copy).

### 4.4 Four silent lies of exactly the §10f shape were already found and repaired here

This is the strongest evidence for the maintainer's premise, and it should be read as a warning rather than
as history. All four are inside `stubs-other.cpp`, each with the measurement in its comment:

1. **`MainThreadSharedTimer` was three empty bodies.** `MainThreadSharedTimer` is the single platform timer
   underneath `ThreadTimers`, i.e. under every `WebCore::Timer` on the main thread. While it was empty,
   **no WebCore timer in this port ever fired**, which silently disabled every deferred engine operation.
   The one that broke real sites: `ScriptableDocumentParser::executeScriptsWaitingForStylesheetsSoon()`'s
   one-shot 0-second timer is the only thing that resumes an HTML parser parked on a script that waits for
   stylesheets — so a page with an inline `<script>` after a `<link rel=stylesheet>` in `<head>` stopped
   parsing at that script **for good**: head parsed, no `<body>`, `readyState` stuck at `Loading`. It is a
   real `RunLoop::Timer` now.
2. **`GlyphPage::fill` returned `false`.** Consequence: no character ever mapped to a glyph, so pages
   rendered background-only. It now maps through `FT_Get_Char_Index` against the real face.
3. **`FontCustomPlatformData::supportsFormat` claimed formats it did not have** — see §4.1; the cost was
   ya.ru rendering blank.
4. **`SystemFontDatabase::platformSystemFontShorthandInfo` returned a null family name and a size of 0.**
   Found 2026-09-18, and this one is a *hard AV*, not a wrong rendering: `CSSValueConversion<FontFamilies>`
   installs the returned atom as `familyAt(0)` and upstream only guards it with `ASSERT(!family.isEmpty())`,
   which release builds compile out — so the null reached `CSSFontFaceSet::fontFace` → `HashMap::find` →
   `ASCIICaseInsensitiveHash::hash(const StringImpl*)` (its only guard, an `ASSERT`, also compiled out) and
   dereferenced `nullptr` at offset 0x10. The zero size is fatal independently: `FontPlatformData(0, …)`
   *is* the hash table's `emptyValue()`. It now answers `{ standardFamily, 16, normalWeightValue() }`, the
   shape `SystemFontDatabaseGLib.cpp` uses. Full audit: `Doc/FONT-NULL-FAMILY-CRASH.md`.

The common shape is worth naming: **a function that looks like plumbing, returns a plausible value, and
switches off a whole subsystem.** None of the first three produced an error, a log line, or a crash. The
fourth did crash — but only two subsystems away, in font lookup, with nothing in the port naming the system
font shorthand as the source; it took a symbolized dump and six isolating test pages to walk back. That is
the cost of this class even when it is loud.

### 4.5 Input and clipboard: the ones a user notices next

| Stub | Class | What a user sees |
|---|---|---|
| `Pasteboard::read*` → empty, `write*` → `RELEASE_ASSERT_NOT_REACHED()` | read side SILENT LIE, write side HONEST | Copy and paste do not work. A page reading `navigator.clipboard.readText()` gets `""` and no error. The asserts mean a *paste* into a page aborts the process rather than doing nothing — verify before shipping a build where paste is reachable. |
| `PlatformKeyboardEvent::currentStateOfModifierKeys()` → `{ }` | SILENT LIE | The engine **never knows Shift/Ctrl/Alt state**. Shift-click to extend a selection, ctrl+A, modifier-aware text input — all silently degrade to the unmodified behaviour. |
| `Cursor::ensurePlatformCursor()` → no-op | minor SILENT LIE | CSS `cursor:` has no visible effect. |
| `HTMLSelectElement::platformHandleKeydownEvent` → `false` | minor | Keyboard navigation inside a focused `<select>` does nothing. (Opening one is `ChromeClient`'s job — whether the port answers that is a separate question and is not in this table.) |
| `Icon` / `DragImage` / `SharedMemory` | NEVER REACHED | Drag is off (`ENABLE(DRAG_SUPPORT)=OFF`), file-chooser icons are not painted. `SharedMemory::allocate` returning `nullptr` is only reached if an IPC/shared surface path exists — none does. |

### 4.6 Accessibility is a total, honest zero

`stubs-ax.cpp` is nine no-ops and two constants. There is no AX client, so nothing is lost *today* — the
class is NEVER REACHED for rendering. But it means the browser is unusable with a screen reader, which is a
product-level gap rather than a bug. Effort: **L**, and it needs a UIA provider in the C++/CX harness, not
a change in the port layer.

### 4.7 Two claims in these files are now stale documentation

* `stubs-crypto.cpp`'s header says it supplies `PAL::CryptoDigest` with an all-zero `computeHash()`, and
  points at `port/undef-crypto.txt`. **Neither is true**: the file defines no `CryptoDigest`, and
  `undef-crypto.txt` does not exist. The real digest is BCrypt-backed
  (`PAL/pal/crypto/win/CryptoDigestWin.cpp`), and `cryptographicallyRandomValues` ends in
  `BCryptGenRandom`, so `crypto.getRandomValues` is sound. Someone reading only the comment would conclude
  the port has no cryptography and could "fix" a working path.
* `stubs-font-uwp.cpp:68-81` explains its `#if USE(HARFBUZZ)` guard by an x64/ARM32 HarfBuzz asymmetry that
  `Doc/HARFBUZZ-ICU-DIVERGENCE.md` **closed on 2026-08-22** — both lines now have `USE_HARFBUZZ=1`. The
  guard is therefore always true and the comment's reasoning no longer applies. The definition is still
  needed (the HarfBuzzNG font backend is not compiled), but for a different reason than stated.

### 4.8 Not verified, and flagged as such

* **Which `Theme`/`RenderTheme`/`ScrollbarTheme` singleton wins.** `stubs-other.cpp:397-413,510-514`
  defines `Theme::singleton()`, `RenderTheme::singleton()` and `ScrollbarTheme::nativeTheme()` by hand,
  while `PlatformWinUWP.cmake:14` includes `platform/Adwaita.cmake`, which compiles
  `RenderThemeAdwaita` / `ScrollbarThemeAdwaita`. Two definitions of each symbol exist; the linker picks
  one by archive order. The consequence is page-visible — Adwaita means themed form controls and
  scrollbars, the hand-rolled one means the base classes (unthemed). **This has not been measured**, and
  the measurement is cheap: a local page with `<button>`, `<input type=checkbox>`, `<progress>` and a
  scrolling div. It belongs on the test list, not in a conclusion.
* **`ComplexTextController::collectComplexTextRunsForCharacters` is an empty body**
  (`stubs-other.cpp:521-523`). With `USE(HARFBUZZ)=1` on both lines, upstream would supply shaping from a
  HarfBuzz-backed translation unit — if that unit is not compiled, this empty body is what runs, and
  complex scripts (Arabic, Devanagari, combining marks) do not shape. Whether the HarfBuzz unit is compiled
  is **unchecked**; the risk is a duplicate symbol if it is.
* **`FontCache::systemFontFamilies()` and `getFontSelectionCapabilitiesInFamily()` return empty** — the
  honest consequence of §4.1, not a separate defect.

## 5. What this does to the forecast

The maintainer's question was whether `stubs-*` files hide unfinished work badly enough to move the
schedule. Answer, and the reasoning:

**The stub files themselves are not the problem — the substitute backends behind them are.** Read the
files and most of them are honest: they assert when a write would corrupt state, they return empty when a
probe legitimately expects empty, and the two largest silent lies in the project's history were found and
repaired inside `stubs-other.cpp` before this audit existed. What the audit found instead is that the
port's *missing capabilities* are filed under filenames that begin with `stubs`, which is why they were
never counted as work: **font selection, screen metrics, webfonts, clipboard, modifier keys and
accessibility**. None of those is a link stub; each is a decision about what the browser is.

So the schedule effect is not "the stubs will take longer than expected". It is that four capabilities
that a user would call *the browser working* were invisible in the plan because they lived in files named
"stub". They are now **Phase 2.5** of `PLAN.md`, ordered by what a user notices, with S/M/L on each.

One correction to the premise, recorded because it matters more than the audit itself: the class that
costs the most is not "not implemented", it is **"implemented as success"**. Nothing in this audit is as
expensive as `EmptyStorageArea::setItem` was, and that function had a body of `{ }`.

## 6. Method, so the next audit can be shorter

1. `grep -rn "stubs-" WebKit/Source/WebCore/PlatformWinUWP.cmake` and the `$Srcs` array in
   `Src/port/link-driver-gpu-*.ps1` give the only two lists that matter. A file in neither is dead code.
2. Read each file's header comment **and then the bodies**, because the headers are the least reliable
   part: `stubs-crypto.cpp` describes a `CryptoDigest` it does not define, `stubs-loader.cpp` is not a
   stub at all, and `webcore-driver-stubs.cpp` is 83 % commented-out.
3. For every body, ask: *if a page reaches this, does anything say so?* Empty body + success return is the
   whole defect class.
4. Mark reachability **unverified** unless it was measured. Three of this document's items are flagged that
   way on purpose; an audit that guesses reachability is the diagnostic that lies (§4.8, and CLAUDE.md's
   "a diagnostic that guesses which module an address belongs to will eventually lie").




