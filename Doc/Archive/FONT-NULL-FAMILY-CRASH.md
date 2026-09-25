# The null font-family crash (AV reading 0x10)

Status: **root cause identified, fix in, verified on the bench 2026-09-18** — `0.1.10.4` loaded
`https://habr.com/ru/` twice with the crash that motivated this document absent both times, and with
both guards compiled into the shipped `WebCore.dll`. The `ofd:` / `fset:` traces did not fire in either
load, so the installing call site of §4 is still unnamed. Measured on the bench; the defect itself is in
WebCore plus one port stub, so nothing here is x64-only.

This document is the record of a hard crash that the MVP bar ("a real modern page loads over the
network, paints, and responds to touch, **without crashing**") does not tolerate. It is written in
the order the evidence arrived, because two of the three steps were wrong for a while and the
corrections are more useful than the conclusion.

## 1. Symptom

Loading `https://habr.com/ru/` in `0.1.10.3` on the x64 bench killed the engine thread. The
unhandled-exception filter caught it (`LocalState\log-crash-habr.txt`; `crashverdict.txt` from the
next launch confirms the previous process died with a stuck heartbeat):

```
UEF: unhandled exception code=0xC0000005 at addr=00007FFE28735E21 tid=2568
UEF: AV reading address=0x10
UEF:   Harness.exe = 00007FF7E5DC0000
UEF:   WebCore.dll = 00007FFE27A00000
```

`tid=2568` is the engine thread (`WE-job: engine tid=2568`). The thread was inside a `live-tick`
job, so the crash is in the periodic `WebCoreLiveTick` repaint, not in the fetch or the harness:

```
WebEngine::loop()                          Src\harness\MainPage.xaml.cpp:1560
 └─ live-tick job lambda                   Src\harness\MainPage.xaml.cpp:4290
     └─ WebCoreLiveTick()                  Src\port\WebCoreDriver.cpp:4113   (doc->updateLayoutIgnorePendingStylesheets())
```

Note the address arithmetic, which is where the first attempt went wrong: the log prints a **load**
address, the PDB wants the **preferred** one, so WebCore frames must be symbolized at
`0x180000000 + (addr - 0x7FFE27A00000)`. Passing the bare load address gives `??:0:0` for every
frame. `Harness.exe`'s preferred base is `0x140000000` (read from its PE header).

## 2. The chain, symbolized

```
StringImpl::is8Bit                                  wtf/text/StringImpl.h:316      <-- AV reads 0x10
ASCIICaseInsensitiveHash::hash(StringImpl&)         wtf/text/StringHash.h:129
ASCIICaseInsensitiveHash::hash(const StringImpl*)   wtf/text/StringHash.h:136      <-- ASSERT(string), compiled out
ASCIICaseInsensitiveHash::hash(const String&)       wtf/text/StringHash.h:182
HashTable<...>::inlineLookup                        wtf/HashTable.h:675
HashMap<String, Vector<Ref<CSSFontFace>>>::find     wtf/HashMap.h:366
CSSFontFaceSet::fontFace                            css/CSSFontFaceSet.cpp:470     <-- m_facesLookupTable.find(family)
CSSFontSelector::opportunisticallyStartFontDataURLLoading   css/CSSFontSelector.cpp:294
opportunisticallyStartFontDataURLLoading            platform/graphics/FontCascadeFonts.cpp:422
FontCascadeFonts::glyphDataForVariant               platform/graphics/FontCascadeFonts.cpp:434
FontCascadeFonts::glyphDataForCharacter             platform/graphics/FontCascadeFonts.cpp:563
FontCascade::glyphDataForCharacter                  platform/graphics/FontCascade.cpp:453
FontCascade::canUseSimplifiedTextMeasuring          platform/graphics/FontCascade.cpp:477
RenderText::initiateFontLoadingByAccessingGlyphDataAndComputeCanUseSimplifiedTextMeasuring
```

Read it from the bottom: **layout asks for glyphs, and the first thing `glyphDataForVariant` does is
hash every family in the description.** Upstream's `StringHash.h` guards the `const StringImpl*`
overload with `ASSERT(string)` and nothing else, so a null family is a null dereference in any
release build. The AV address (0x10) is `StringImpl::is8Bit()` reading through `nullptr`.

So the question is not "why did the hash crash" — it is **"which family in that description was
null, and who put it there"**.

## 3. The producer

`FontCascadeDescription()`'s constructor is the source of null families *in general*:

```cpp
FontCascadeDescription::FontCascadeDescription()
    : m_families(RefCountedFixedVector<AtomString>::create(1))     // one DEFAULT-CONSTRUCTED AtomString
```

A default-constructed `AtomString` is **null**, not empty. Two upstream paths exist to replace it,
and both are deliberate:

- `StyleResolver::defaultStyleForElement` (`style/StyleResolver.cpp:643`) and
  `StyleResolveForDocument` (`style/StyleResolveForDocument.cpp:88`) call
  `setOneFamily(standardFamily)` — the real `-webkit-standard` atom, built by
  `WebKitFontFamilyNames::init()`, which `LocalFrame`'s constructor calls through
  `ProcessWarming::initializeNames()`.
- `BuilderCustom::applyInitialFontFamily` (`style/StyleBuilderCustom.h:558-566`) is a **no-op by
  design**: it builds a default `FontCascadeDescription`, then guards the install with
  `if (!initialDesc.firstFamily().isEmpty())`. For a null family `isEmpty()` is true, so nothing is
  installed and whatever the description already had survives. That is the intended behaviour — the
  platform's initial family is supposed to already be in place.

A third path produces a null family and does **not** repair it.
`CSSValueConversion<FontFamilies>::operator()` (`style/values/fonts/StyleFontFamily.cpp`) has four
returns:

| value | family installed | flag set? |
|---|---|---|
| `CSSPrimitiveValue` that is a font family | `AtomString{primitiveValue->stringValue()}` | n/a |
| `-webkit-body` | `AtomString{settings().standardFontFamily()}` | **no** |
| a generic family keyword | `*familyNamesData->at(index)` | n/a |
| a system-font keyword | `systemFontShorthandFamily(...)` | **no** |
| invalid at computed-value time (3 sites) | `nullAtom()` | **yes** |

The three `nullAtom()` sites are safe *because* they set the flag: `Builder::applyProperty`
(`style/StyleBuilder.cpp:430`) detects it afterwards and re-applies the property as `unset`, so the
null is overwritten by the inherited value. That is why test page `fontvar1.html`
(`body { font-family: var(--apo-missing); }`) renders fine — verified, not assumed.

The **system-font keyword** row is the one without a safety net, and it is precisely the row the port
broke. Upstream writes:

```cpp
if (isSystemFontShorthand(valueID)) {
    auto family = SystemFontDatabase::singleton().systemFontShorthandFamily(lowerFontShorthand(valueID));
    ASSERT(!family.isEmpty());                                   // compiled out in release
    return { WTF::move(family), FontFamilyKind::Generic };
}
```

The `ASSERT` is upstream's statement of the invariant *"a system font always has a family name"*. The
port's stub lied about exactly that (`Src\port\stubs-other.cpp`):

```cpp
SystemFontDatabase::SystemFontShorthandInfo SystemFontDatabase::platformSystemFontShorthandInfo(FontShorthand)
{
    return SystemFontShorthandInfo { AtomString(), 0, FontSelectionValue() };   // null family, zero size
}
```

A null `AtomString` with no flag set, installed as `familyAt(0)` by
`BuilderCustom::applyValueFontFamily`, which is unconditional. Every lookup of it is then a null
dereference. `SystemFontDatabaseGLib.cpp` shows the intended shape — it falls back to
`WebKitFontFamilyNames::standardFamily` when the platform has no system font name. The zero size is a
second, independent defect: `FontPlatformData(0, ...)` **is** the hash table's `emptyValue()`, and
`HashTable::validateKey` release-asserts on it (that is the dzen.ru killer of 2026-09-17, still in
memory). One stub, two documented invariants broken.

## 4. What does *not* reach it — measured, not reasoned

Six local pages were navigated one at a time through the harness path (`nav.txt`), each isolating one
construct that the analysis above nominates:

| page | construct | result |
|---|---|---|
| `fontvar1.html` | `body { font-family: var(--apo-missing) }` | renders (`nonwhite=949/1036944`), no crash |
| `fontvar2.html` | `body { font-family: caption }` | renders, no crash |
| `fontvar3.html` | `body { font: caption; font-size: 16px }` | renders, no crash |
| `fontvar4.html` | `body { font-family: -webkit-body }` | renders, no crash |

Each non-crash closes a branch, and the reasons are all structural:

- **`font-family: caption`** is parsed as a `<family-name>` — a *string* named "caption" — because
  `consumeGenericFamilyUnresolved` (`css/parser/CSSPropertyParserConsumer+Font.cpp:291`) accepts only
  `serif, sans-serif, cursive, fantasy, monospace, -webkit-body, -webkit-pictograph, system-ui, math`.
  No system-font keyword can arrive through a `font-family` declaration at all.
- **`font: caption`** (the shorthand) never reaches `applyValueFontFamily`. The shorthand's longhands
  are stored with `IsImplicit::Yes`, and the shorthand applier resolves the family through
  `resolveForUnresolvedFont` → `fontFamilyFromUnresolvedFontFamily`, which **filters nulls**
  (`compactMap`) and then bails out of the whole shorthand when the list comes back empty. The
  sentinel keyword therefore dies inside the resolver, and — as a side effect — so does the zero
  size that the same sentinel would otherwise have produced through
  `systemFontShorthandSize`.
- **`-webkit-body`** returns `settings().standardFontFamily()`. Since the page renders, that value is
  not null in this port. (It may well be *empty*; an empty atom has a real `StringImpl` and hashes
  harmlessly — which is the difference between this case and the one that crashed.)
- **invalid at computed-value time** is caught by the builder flag, as shown above.

The consequence is important and slightly uncomfortable: **the trigger is reachable only through the
system-font-shorthand row, and a plain stylesheet cannot supply it.** The remaining entry points are
the ones that synthesise longhand values internally rather than parsing them — implicit longhand
application through a cascade path that skips the shorthand resolver, `StyleProperties`-based inline
styles, shorthand serialization round-trips, and the editing path (`EditingStyle.cpp:329`). That the
crash happened once, on a heavy JS page, and not on repeat attempts is consistent: it needs whichever
of those paths habr.com's own script took, at a moment when the description had not yet been repaired.

**This is the honest state of the analysis: the mechanism is proven, the producer is named and
fixed, and the exact call site that first installs it is still open.** The instrumentation added in
§5 exists to close that last gap the next time it happens, instead of taking the process down.

## 5. The fix

Three edits, all small, none of them changing upstream semantics for a well-formed page.

1. **`Src\port\stubs-other.cpp` — the stub stops lying.** `platformSystemFontShorthandInfo` returns
   `{ WebKitFontFamilyNames::standardFamily, 16, normalWeightValue() }`: a real atom (reusing the
   atom WebCore's own fallback machinery already uses, rather than inventing a family name), a real
   size, a real weight. This is the producer-side fix and the one that matters: it removes the only
   place in this port where a null family can enter a `FontFamilies`.

2. **`WebKit\Source\WebCore\css\CSSFontFaceSet.cpp` — the choke point refuses a null key.** Guarded
   by `#if defined(WK_WINUWP)`, one early return before `m_facesLookupTable.find(family)`, plus an
   `apotheosisWebTrace` line. This is the *insurance*: `fontFace` is the single function where every
   path — `opportunisticallyStartFontDataURLLoading`, `fontRangesForFamily`, the standard-family
   fallback — hashes an incoming family, so one guard there covers all of them. Guarding only the
   first caller would merely have moved the AV one frame later, because `realizeFallbackRangesAt`
   reaches the same function with the same description immediately afterwards.

3. **`WebKit\Source\WebCore\platform\graphics\FontCascadeFonts.cpp` — name the event.** In
   `opportunisticallyStartFontDataURLLoading`, a null family is logged and skipped instead of being
   passed on. It is the only place that still knows *which* description produced the null (family
   count, `isSpecifiedFont`, computed size), so it is where the open question in §4 gets answered.
   The sink is `apotheosisWebTrace`, the WTF bridge the port already uses, and it is silent unless
   `APO_TRACE_TEXT=1` — so this costs nothing in a normal session.

A null that is refused is still a page rendering with the wrong font, not a page that works. The
guards are there to convert a hard crash into a loud, named degradation; the stub fix is what
actually removes the defect.

### 5a. Verified on `0.1.10.4`, 2026-09-18

Four independent facts, none of them an inference from the other:

| what | how it was measured | result |
|---|---|---|
| the crash is gone | `https://habr.com/ru/` loaded **twice** in one session on the fixed build | no UEF, no `crashverdict` from this run, `heartbeat` idle at `busy=0 pending=0 finished=471 tickstep=10`, process alive |
| the page actually rendered | the same diag lines | `contents=1368x23607`, `nonwhite=200299/1036944`, title `Публикации / Моя лента / Хабр` |
| the guards are in the binary that ran | `grep -a` the shipped `build-x64-gpu\bin\WebCore.dll` | both `REFUSED null family` and `ofd: NULL family` present |
| the trace can be armed — and stays silent when it is not | `texttrace.txt` created, then removed, with a relaunch between | armed: `texttrace: armed by …` banner plus **428 121 → 849 034** `wt:` lines; unarmed: **no banner, count unchanged at 849 034** across a fresh load |

The third row is the one that matters most and is the easiest to skip: a *missing* diagnostic message
proves nothing unless the diagnostic is known to be in the binary and known to be able to fire. Both
were checked separately, in both directions.

**What this does not establish.** The null family did not occur in either load — `ofd:` and `fset:`
count zero — so §4's open question is *unanswered*, not answered negatively. The crash of `0.1.10.3` was
one occurrence on one page; that habr.com now loads twice without it is strong evidence for the stub fix
(it is the only null source removed) but not proof that every path into `FontCascadeDescription` is
clean. The guard is what covers the rest, and it has not been exercised.

## 6. How to reproduce the diagnosis

**The trace is armed by a file, not by an environment variable** — and the first version of this
document said otherwise, which would have made the whole section unusable. `x64-cycle.ps1` launches
through `Start-Process "shell:AppsFolder\..."`, so the app is activated by the *shell* and inherits the
shell's environment, not the PowerShell session's; `$env:APO_TRACE_TEXT=1` never reaches the process.
The port therefore arms itself from the harness's own log path inside `WebCoreSetGpuInitLogFile`:

```powershell
# $LS = %LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState
New-Item -ItemType File -Force -Path "$LS\texttrace.txt"     # arm
# ...launch, it stays armed for that process only...
Remove-Item "$LS\texttrace.txt"                              # disarm (takes effect next launch)
```

`gpuinit-steps.txt` line 1 then reads `texttrace: armed by …\LocalState\texttrace.txt`. The switch is a
file for the same two reasons `jstack.txt` is one (`PortChromeClient.cpp:125`): it has to work against
an already-installed build, and Device Portal cannot write LocalState on the Lumia. It has to be armed
*before the first navigation*, because `apotheosisWebTrace` caches the flag in a function-local static
on its first call.

**Cost, measured:** with the trace on, `glyph.log` gained ~150 000 lines per minute (`gdv` / `gdc` /
`brk` are per-glyph and per-break-iterator, and `gdc->` doubles `gdc`) — 20 MB in three minutes of a
habr.com load. Arm it for one run, then remove the file. `ofd:` and `fset:` themselves are rare by
construction: they fire only when a null family actually appears.

Run the bench and read the two markers out of `LocalState\glyph.log`:

```powershell
pwsh -File Src\tools\x64-cycle.ps1 -Url https://habr.com/ru/
Select-String -Path "$LS\glyph.log" -Pattern '^wt: (ofd|fset):'
```

`ofd: NULL family …` names the description; `fset: REFUSED null family …` names the lookup. Both are
per-occurrence, not throttled, so a page that hits it once will say so once.

Symbolizing a crash log from the same session:

```powershell
# WebCore.dll: preferred base 0x180000000, load base from the UEF "modules:" block
printf '0x180D35E21\n' | llvm-symbolizer --obj=build-x64-gpu/bin/WebCore.dll --functions=linkage --demangle
```

## 7. Open items

- Which call site first installs the null family (§4). The instrumentation is in place and **verified
  able to fire** (§5a); the event simply did not recur in two full loads of the page that crashed, so
  this stays open until it does. Until then the guard is what keeps the page alive.
- `standardFontFamily` is not null in this port, but whether it is *empty* — and what
  `-webkit-body` therefore resolves to — has not been measured. An empty family silently degrades to
  the fallback chain rather than crashing, which is why it has never been noticed.
- The other stubs that fabricate a value where WebCore asserts on one deserve the same audit. This
  one was found by a crash; the class of defect was already catalogued in `Doc/STUB-AUDIT.md`, and this
  is now its fourth entry.
- **The bench cannot set an environment variable for the app it launches.** Discovered while writing
  §6 and worth generalising: `Start-Process "shell:AppsFolder\…"` activates through the shell, so
  anything a run needs to be told must arrive as a *file in LocalState* (the `jstack.txt` /
  `texttrace.txt` / `nav.txt` pattern) or be shipped inside the appx.
