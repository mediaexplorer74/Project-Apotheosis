# Project Apotheosis — Development Plan

## 0a. Current status — Sep 18, 2026 (morning)

> **Supersedes the "NEXT PHASE" section below and the Aug 25 plan text for current state; older
> sections kept as historical record — see [STATUS-2026-09-18.md](STATUS-2026-09-18.md).**

- **x64 `0.1.9.97`** built Release, installed **in-place** (no `Remove-AppxPackage`; LocalState
  backed up). Built from .96 sources + PE32/PE32+ parser fix in `ApoReadModuleName` (watchdog
  diagnostic). Harness.exe SHA256 matches build output.
- **Runtime verification (2026-09-18 05:50–06:30):** navseq completed all 8 URLs without
  crash. example.com ×2, Hacker News, layertest.html, ya.ru all rendered correctly.
  **Ya.ru rendered a full page with images** (78 scripts, nonwhite=234723). dzen.ru redirected
  to SSO login (`sso.dzen.ru/install`) — site requires login, not a browser bug.
- **CSS crash `0xc0000005 @ WebCore.dll+0x204901f` did NOT reproduce** on .97 for any tested
  site. The .96 guard (stale `CalculationValue` handle check) remains a candidate fix;
  mechanism not proven.
- **GPU readback:** fail-closed guard verified (78 `rb: no live GL context` messages,
  0 readPixels). Cairo fallback renders correctly. Compositing visible on layertest.
- **Process stability:** PID 10648 survived ~40 minutes of active navigation. No crash events.
- **Memory/disk:** 4 GB RAM; ~38 GB free disk.
- **Toolchain current:** ICU 78 via vcpkg; SDK 19041 harness / 26100 x64 engine; MSVC 14.44.
  No Debug harness; no clean rebuild on 2026-09-18.
- ⚠️ **`x64-cycle.ps1` currently REMOVES the installed package** despite its header — avoid it for
  a data-preserving update.

## 0b. Current status — Sep 18, 2026 (evening, later)

> **The dzen.ru killer is named and fixed.** Full measurement chain, timeline and negative results:
> [DZEN-SCROLL-DEATH.md](DZEN-SCROLL-DEATH.md). Read that before touching any of this.
>
> **The crash is closed; the freeze is now the front.** §10c names it — a page JS runaway inside a
> timer callback, the live tick never returning, ~2.2 MB/s of allocation, and a JSC watchdog that fires
> on time and does not stop it. Next steps 0a/0b below are the two narrow questions left.
>
> **Updated the same evening on `0.1.9.109`: §10e.** The JS-stack probe names the runaway, and it
> refuted two of the assumptions above — the watchdog **does** terminate a single-frame infinite loop
> (so "fires and does not stop it" is specific to what dzen.ru's loop crosses), and dzen.ru **does not
> freeze without a gesture** (two runs, rendered and idle for five minutes). 0a is reframed, 0b has one
> candidate eliminated, and both now wait on 0c: the maintainer's gesture on the live `.109` build.
>
> **Later still, `0.1.10.x`: the front moved twice.** `0e`/`0g` closed the tap and the watchdog
> coupling in `0.1.10.0`–`0.1.10.2`; then a **new hard crash** appeared on habr.com — an AV reading
> `0x10` in font lookup, root-caused to a null family produced by a port stub. See item **0h** and
> [FONT-NULL-FAMILY-CRASH.md](FONT-NULL-FAMILY-CRASH.md).
>
> **And the morning's "the CSS crash did not reproduce" was one measurement too early.** The
> `CalculationValue`-handle AV returned deterministically on the engine thread, through the
> **second** `nonNanCalculatedValue` overload the 2026-09-17 guard did not cover. Root-caused from
> the VEH stack, contained, and written up in item **0i** /
> [CALC-HANDLE-DANGLING.md](CALC-HANDLE-DANGLING.md) — including the symbolisation recipe, because
> neither `llvm-symbolizer` nor `cdb` can read this PDB.

**The killer.** `FontCache::systemFallbackForCharacterCluster()` — the fork stub in
`Src\port\stubs-font-uwp.cpp` — built `FontPlatformData(description.computedSize(), false, false)`
with no zero check. `FontDescription::m_computedSize` starts at `0` (`FontDescription.h:166`) and
`FontDataCacheKeyTraits::emptyValue()` **is** `FontPlatformData(0.f, false, false)`
(`FontCache.cpp:96-100`). A zero size therefore produced an object identical to the hash table's own
EMPTY sentinel, and `HashTable::validateKey()` (`HashTable.h:534`) refuses exactly that with a
**release** assert → `WTFCrashWithInfo` → `abort()`. Scrolling dzen.ru eventually measured text with a
zero-sized `FontDescription`, and the process aborted.

**How it was measured** (each link independent):

- `cdb` attached to the live pid: `Last event: 24c8.1058: Exit process 0:24c8, **code 3**` — an
  `abort()`, not `0xC0000409` fail-fast, not `0xC00000FD`, not a platform kill.
- `DEATHNET: _set_abort_behavior -> prev=0x2`: `_CALL_REPORTFAULT` **was** on, so before 0.1.9.105
  every `abort()` became a Watson fast-fail that runs no user handler. **That is the whole
  explanation of the silence**, and it is why no earlier build ever logged anything.
- `0.1.9.105`'s `signal(SIGABRT, …)` produced a 40-frame stack whose frame #3 disassembles to
  `WTFCrashWithInfo … call [__imp_abort]`, called from `FontCache::fontForPlatformData+0x345`.
- `ub` backwards from the call site shows the guard: `isEmptyValue(key)` → `je <normal path>`, and the
  fall-through leads to `lea rdx,[file]` / `lea r8,[assert]` / `mov ecx,216h` (line 534) /
  `call WTFCrashWithInfo`. `da` on `r8` names `…FontDataCacheKeyTraits>::validateKey(const ValueType &)`;
  on `rdx`, `build-x64-gpu\WTF\Headers\wtf/HashTable.h`.
- `HashTraits.h:302-304` + `FontPlatformData.h:429-440`: the checker's second template argument is
  `hasIsEmptyValueFunction` (false here), so "empty" means `value == emptyValue()`, and **`m_size` is
  the only field that can differ** — `isEmptyValue()` is true iff `m_size == 0`.

**Exonerated, and why the earlier reading of each was wrong:**

- **The `wtfcash` tracer was instrumented on the unused twin.** It hangs off `WTFCrash()`; every
  assert in this tree goes through `WTFCrashWithInfo`, which on x64 is
  `Assertions.h:982-988` — literally `{ CRASH(); }`, compiling to `call abort`. Empty `wtfcash` in
  every session was never evidence.
- `WTFCrash` itself (`Assertions.cpp:408` → `__builtin_trap()` → `ud2` → `0xC000001D`) — the earlier
  reasoning that a `UEF:` line would have appeared is *correct for `WTFCrash`* and irrelevant, because
  `WTFCrashWithInfo` never reaches it.
- The JSC watchdog — recorded here as **1479** arms and **0** fires, on the theory that
  `Watchdog::startTimer` refreshes the CPU deadline on every re-arm and so can never reach its
  callback. **That reading was wrong.** It was corrected on 2026-09-18 evening: the watchdog *does*
  fire and its callback *does* run (DZEN-SCROLL-DEATH.md §10c item 5, with the timestamp arithmetic).
  The exoneration stands — it was never the cause of *this* crash — but the mechanism attributed to it
  does not, and the real reason no fire had been observed is that nothing had ever run JS long enough.
- Stack overflow (the 14:22 wedgedump put `SP` ≈17 KB below a 16 MB thread-stack top, 15 frames deep).
- The nested-flexbox frame cycle in the abort stack is **ordinary** intrinsic-width recursion, one
  iteration per DOM level — not a runaway. The capture stops at 40 frames.

**Fixed in `Src\port\stubs-font-uwp.cpp`:** decline (`return nullptr`) on a non-positive/NaN computed
size, and on a platform data whose `scaledFont()` is null. `nullptr` is what the same file's
`createFontPlatformData` already returns unconditionally, and the caller handles it. **This is a
class**: any fork code building a `FontPlatformData` from a CSS-derived size needs the same guard,
because a release assert is not conditional on `ASSERT_ENABLED`.

**Still open, and *not* the crash:** the ~30 s black window the maintainer saw before the death.
Nothing dates it — `heartbeat.txt` is one rewritten line with no history — and there is no
`beat-stuck` anywhere after 14:44:21, so the engine was **not** stalled at the end. What is recorded:
`[GPU] readback failed -- falling back to Cairo` 550× in 8 minutes (`gpuLogMarker`,
`WebCoreDriver.cpp:869`), and **no `[GPU] present failed` line at all**, i.e. no direct present was
ever attempted. What this says about the visible SwapChainPanel is not yet measured.

**Verified 2026-09-18 on `0.1.9.106`.** The same gesture was repeated on dzen.ru: **no `SIGABRT`, no
exit code 3, no assert string in the session**, and `glyph.log`'s `platformInit:` lines carry real
non-zero sizes, so the guard did not disable normal font creation. Two *other* things surfaced, both
written up in [DZEN-SCROLL-DEATH.md](DZEN-SCROLL-DEATH.md) §10:

- **A first-chance access violation inside the wedge dumper fakes a hang when `cdb` is attached.**
  `CopyStackWords`' `memcpy` reads 16 KB of stack through `__try`/`__except`; when the window hits a
  guard page the AV is *handled* and the dump just says `stack copy failed` — measured in **four of
  the nine dumps in LocalState, with the process surviving every one**. Under a debugger the same
  benign fault stops every thread, which is what produced a "the browser is completely frozen, I
  cannot even activate the address bar" report that had nothing to do with the engine. Suppress it
  with `sxd c0000005`, or run without a debugger. Related: never end a `cdb` session with
  `Stop-Process` — that kills the debuggee with it; use `.detach`.
- **The post-fix stall is a page-JS runaway whose termination request does not take effect — and the
  JIT A/B refuted the interpreter explanation.** A genuine freeze was first sampled on the
  *interpreter*: `finished=682` frozen for 60+ s, `log.txt` silent, UI thread healthy (the address bar
  worked), the engine thread in `slow_path_enter` (`CommonSlowPaths.cpp:926` — trivial and
  non-blocking, so it was *executing*) ← `llint_entry`, with **all 28 other threads parked and no lock
  held by anyone**. Every bench session until then ran with `JSC_useJIT=false`, forced by the packaged
  `Src\harness\Assets\navseq.txt` (`jit=0`, `MainPage.xaml.cpp:3760-3776`) on a `.77`-era belief that
  the x64 bench cannot JIT while the Lumia can. `navseq.txt` now ships `enabled=0` with the `jit=0` line
  removed, i.e. **JIT on, autoplay silent**, and `0.1.9.107` re-ran the gesture with it on:
  **the engine wedged identically** (`finished` frozen at `376`, WEDGE, then `WATCHDOG: engineBusy=1
  finished=376` every 40 s). The interpreter was not the cause. What the freeze is, with the stack,
  `tickstep`, the allocation rate and the watchdog fire, is DZEN-SCROLL-DEATH.md §10c: **dzen.ru JS
  enters a synchronous `dispatchEvent`/microtask cycle inside a timer callback, the live tick never
  returns (`heartbeat.txt` holds `job=live-tick tickstep=1` for 12 minutes straight), memory grows at
  ~2.5 MB/s, and the JSC watchdog — which *fires*, 15.05 s after arming, to the tenth of a second —
  logs `runaway script terminated` without the script terminating.** Which C++ clearance point swallows
  the pending `TerminationException` is the open question.
- **Both lines run the same JIT tiers, so no x64 rendering experiment is misleading on that axis.**
  `build-x64-gpu\CMakeCache.txt` and `build-arm32-gpu\CMakeCache.txt` agree: `ENABLE_JIT=ON`,
  `ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`, `ENABLE_C_LOOP=OFF` — **LLInt + Baseline JIT, no DFG, no
  FTL**, on the bench and on the phone alike. `CLAUDE.md`'s "incl. FTL" for the x64 line was wrong and
  has been corrected; the divergence `Doc/HARFBUZZ-ICU-DIVERGENCE.md` listed as the most likely to
  mislead does not exist. The LLInt is present on ARM32 as offlineasm-generated ARMv7 Thumb-2 code
  (DZEN-SCROLL-DEATH.md §10d).


## 0c. Current status - Sep 24, 2026 (the memory front opens)

> **The front has moved from crashes to memory.** Both crashes that dominated 0a-0i are contained
> (`nav-load` AV and the stale `CalculationValue` handle), the tap dispatcher is fixed, and the
> remaining defect that can kill the app on the *target device* is unbounded RSS growth across
> navigations. Sections and items below are kept as history; the live items are **0j** (memory), **0k**
> (the deferred architectural fix) and **0l** (subframes do not exist) in Next steps.

- **The measurement that defines the problem.** Same page repeated, x64 bench, `0.1.10.35`:
  `example.com` x10 plateaus in a 1132892-1138124 KB band; `dzen.ru` x5 grows monotonically
  1133152 -> 1780648 KB, **+100-145 MB per load**. Growth tracks *page weight*, not navigation count.
- **A leak on the bench is a reap on the phone.** The Lumia 950 has 3 GB total and an App Container
  budget far below the 12.5 GB the bench reports, and its failure mode for an over-budget process is a
  silent log truncation with no dump (`device-diagnostics-that-survive`).
- **Not an accumulating `Page`, and not the port's caches.** Sessions are strictly 1:1 (24
  `Page::create ok` / 24 teardowns, no early return), and the port already sets
  `BackForwardCache::setMaxSize(0)` and `MemoryCache::setCapacities(0, 8 MB, 16 MB)`.
- **`WebCore::releaseMemory()` had no caller at all.** `WebCoreReleaseMemory` was declared in both ABI
  headers, defined, and never called; the harness's `AppMemoryUsageIncreased` handler only logs, while
  the port's comment claimed the harness called it - the SILENT LIE class of `STUB-AUDIT.md`. A
  per-navigation release now exists (`WebCoreSessionLoad`), it demonstrably returns memory at the
  margin (-126 MB over three light loads), and it is **not** the fix: +155 MB/load with the
  non-critical path, +160 with the critical one, against +161 with no release at all.
- **Three hypotheses have been falsified by measurement**, not by argument: the font cache, the loader
  (`analyze-loader-trace.ps1`: `inflight`/`hosts` drain to 0, `STILL OPEN: 0`), and the mechanism of
  `releaseMemory(Critical::Yes)`. The last one is worth naming because it was reasoned from a header:
  `MemoryCache::pruneLiveResourcesToSize` (`MemoryCache.cpp:281-334`) **does not remove resources** -
  it walks `m_liveDecodedResources` and only calls `destroyDecodedData()`, under
  `ASSERT(current->hasClients())`. A resource with clients cannot leave the cache by that path, so the
  critical branch frees decoded bitmaps, not `CachedImage`/`SVGImage`, and cannot reach a hidden Page.
- **What is measured now, and it is new.** A port-side instrument (`apoLogLivePages`, no WebCore edit,
  ARM32-portable - `Page::forEachPage`, `Page::mainFrameURL()` and `Page::nonUtilityPageCount()` are all
  `WEBCORE_EXPORT`ed, checked with `llvm-nm --defined-only`; `isUtilityPage()` is inline in `Page.h`) logs
  the count, the utility flag and the URL of every live `Page` at three points per navigation. Result on
  `0.1.10.41`, five `dzen.ru` loads: **the session Page always carries a URL, and every Page that
  survives teardown is anonymous** (`mainFrameURL()` empty), 2-3 added per load, and none removed by the
  `releaseMemory(Critical::Yes)` call that sits between teardown and build.
- **The SVG mechanism is CONFIRMED, quantitatively and by flag.** Controlled pages in `LocalState`
  (`Src\tools\make-svg-test-pages.ps1` + `svg-page-test.ps1`, loaded over `file:///` via `nav.txt`):
  ten EXTERNAL svg images produced exactly ten extra live Pages (`8 -> 18`), and returning to the
  zero-SVG page brought the count straight back down (`18 -> 7`). The flag closes it: those Pages are
  **utility** (`U`), `nonUtility` stays at 1, and `isUtilityPage()` is true exactly for a Page whose
  ChromeClient is an `SVGImageChromeClient` (`Page.cpp:276-279`), which is what `SVGImage.cpp:489`
  installs. So a hidden whole Page per svg image (`SVGImage.cpp:497`) is no longer a hypothesis.
- **~~`MemoryCache::evictResources()` is the first thing that has ever moved the slope: +156 -> +65 MB per
  load~~ — WITHDRAWN 2026-09-24 (`.49`/`.50`).** The `0.1.10.42` reading (`five dzen.ru loads each way`,
  evict armed by `LocalState\evictcache.txt`) compared **one process in two sequential windows**, the
  disarmed window first and therefore cold: the `-58 %` is the process warming up. In one warm process,
  alternating the switch, the difference is not there (`52 MB/load OFF against 47 ON`; interleaved
  `OFF 5, 4, 44 / ON 54, -71, 48`). Per-navigation deltas on a real page span `-71 ... +54`, so the spread
  within one arm exceeds the difference between arms. **All "-57 %" / "-58 %" / "44-58 MB floor" figures
  in this document are withdrawn** — the authoritative record is the comment on
  `apoReleaseResourceCache()` in `Src\port\WebCoreDriver.cpp`. What the code does is still worth knowing
  (it is `setDisabled(true); setDisabled(false);`, and `setDisabled(true)` at `MemoryCache.cpp:752-765`
  calls `remove()` on every resource of every session *ignoring clients* — the only exported entry point
  that can drop a `CachedImage` that still has one), which is why the call survives as a default-off
  escape hatch rather than as a fix.
- **But it does not free a single hidden Page.** The live-Page count still climbs (`8 -> 17` across the
  evicting run) and no `U` Page is ever removed by either `releaseMemory(Critical::Yes)` or
  `evictResources()`. The slope fell because something else in the cache was freed.
- **Three explanations for the retention were tested and killed**, each by a page that differs in one
  respect (all in `make-svg-test-pages.ps1`): page JS holding the `HTMLImageElement`s (`tjs` behaves
  exactly like `tjs2`), the images reached from CSS rather than `<img>` (`tcss`), and svg nesting
  (`tnest`). All three release their Pages on the next teardown (`27 -> 16`). Unclosed loader requests
  were the fourth: 880 `+` against 883 `-` in `port-trace.txt`, so nothing is stuck mid-flight.
- **And the Page is not the expensive part, which is the uncomfortable part.** Ten local SVG Pages cost
  about +4 MB (`1034 -> 1038`), i.e. ~0.4 MB each - three orders of magnitude below the ~40 MB/Page the
  dzen.ru arithmetic would need for the Pages to account for its growth. So dzen.ru's slope is **not**
  explained by the Pages alone even though they are real and do accumulate, and the honest state is:
  one confirmed mechanism, one working half-measure, and an unexplained remainder.
- **Later the same evening (builds `.43`/`.44`/`.45`), two more instruments turned the remainder into a
  set of eliminations.** `SL: mem` (`PerformanceLogging::memoryUsageStatistics` + `MemoryCache::getStatistics()`)
  and `SL: docs` (`Document::allDocuments()` with urls) run at `after teardown` and `after build` on every
  navigation, from `Src\port\WebCoreDriver.cpp`, into `gpuinit-steps.txt`.
  - **~~The JSC heap SATURATES~~ — WITHDRAWN.** `javascript_gc_heap_size_mb` / `javascript_gc_object_count`
    grew 65 -> 96 -> 136 and appeared to freeze at **113 MB / 1 722 272 objects**, byte-identical across six
    further navigations. That was an artifact of three to six samples: over **26** navigations the same
    series climbs 6, 10, 14 ... 90 with no plateau. What survives is the reading method - it is taken after
    a full `garbageCollectNow()` every time, so the objects counted are live ones.
  - **~~Retained Documents saturate too~~ — WITHDRAWN; they grow ~+4/navigation without bound.** The urls
    and node counts below are still the useful half: an SVG image's document has an **empty** url -
    `t10.html` added exactly +10 empty-url documents and +10 live Pages, all freed at the next teardown -
    so the dzen-url documents are page-created ones (DOMParser -> `about:blank`, XHR-as-document -> the
    response url), not svg images.
  - **The port has no subframes at all**, so every `<iframe>` is an empty painted box (0 new Documents,
    0 requests, no child url in `port-trace.txt`). Two consequences, both in item **0l**: a capability
    gap of its own, and the invalidation of the iframe retention experiment, whose negative meant nothing.
  - **The cache holds nothing dead** - `liveSize == size` on every type on every load, which is the proof
    that the non-critical release path (`pruneDeadResourcesToSize(0)`) can never free anything - and
    images report **`decodedSize = 0`** on every load while the cache holds 22-44 of them. Either the
    Cairo image path never calls `setDecodedSize()`, or it decodes without telling the cache; in the first
    case decoded surfaces are resident and invisible to every instrument here, which would explain why
    `evictResources()` (which destroys the `CachedImage` objects and their surfaces) is the only lever
    that worked. **Suspect, not finding.**
  - **The harness is exonerated** for the per-navigation part: five consecutive `file://` loads moved RSS
    733 -> 687 -> 674 and 670 -> 674 -> 678, flat to slightly down.
  - **The slope on these three builds reads 150 / 156 / 135 MB/load**, overlapping the `0.1.10.35`
    baseline (161) and every release arm, so all nine arms stay one set: **three release arms are
    indistinguishable from no release.**
  - **Two questions owed before any absolute figure is quoted**: the unit ambiguity in the cache
    statistics (read as KB, one dzen load claims 19 GB of scripts) and an engine-side total independent
    of MemoryCache accounting, e.g. `WTF::fastMallocStatistics()` committed bytes per navigation.
    **Both are now answered, both negative, same evening — see item 0j:** the unit is **bytes** (the log
    label was wrong by 1000x), and no readable engine-allocator total exists on this port
    (`fastMallocStatistics()` is the process's peak working set; `memoryFootprint()` returns 0 in the App
    Container). The `decodedSize = 0` reading was thereby sharpened into the mechanism: `size()` includes
    decodedSize, so it means the Cairo image path never calls `decodedSizeChanged()`, those images never
    enter `m_liveDecodedResources`, and **no release path can reach decoded bitmap data.**
- **~~And the accumulator is now named, priced and reached (builds .47/.48 - the entry that closes the
  front).~~ WITHDRAWN in `.49`/`.50` - what is left is a FRONTIER, not an accumulator.** The pricing half
  stands: `Src\tools\make-image-test-pages.ps1` builds a grid that fits the viewport on both lines, so
  every image is painted and decoded; twenty 1200x1200 PNGs drawn at 200x100 cost **+116 MB on the first
  paint against a predicted 110 MB** of ARGB pixels (the first figure here to match a prediction), and the
  controls - the same grid with no images, and with twenty 32x32 images in the same cells - are flat at
  +1 MB/load. On the `file://` page `after teardown cache_images count=0`, so the memory is freed and
  reused; on dzen.ru the same reading is `count=17 sizeB=1008233 liveB=1008233`, surviving every teardown,
  with `decodedB = 0` throughout - which is the code-level fact that survives: **no upstream release path
  can reach decoded bitmap data here**, because the Cairo image path never calls `decodedSizeChanged()`, so
  the image never enters `m_liveDecodedResources`. But the conclusion drawn from it - "the decoded lever
  takes dzen.ru from 136 to 44 MB/load, so decoded image data is the accumulator" - rested on **one
  outlier** (an armed window whose first delta was `-125 MB`, averaged with `+37, +51, +55` down to "flat")
  and does not survive a warm-process A/B (`52 OFF` vs `47 ON`). The **44-58 MB/load floor does not
  exist.** See item 0j step 2c for the withdrawal in full; the authoritative record is the comment on
  `apoReleaseResourceCache()`.
- **What replaces it: every controlled arm PLATEAUS, and only dzen.ru climbs.** 20 viewport-fitting
  1200x1200 PNGs (+116 MB once, then flat); ten external SVG images (~0.4 MB/Page, self-clearing);
  documents created and dropped or kept by page JS (flat - and `window.__keep` dies with the page at
  navigation, so that arm cannot see a cross-navigation leak at all); 20 layers promoted by
  `will-change: transform` against 20 identical unpromoted boxes (all-positive deltas `24, 8, 71, 24`
  alternating against a flat control, but 8 consecutive loads read `80, 22, -15, -31, -2, 21, -10` - an
  **~80 MB level shift, not a slope**, with `compositing=1` vs `0` verified in the log); the two cache
  levers (inert). The retained Documents are priced at **fractions of a MB** - one dzen document
  (`nodes=1518..1853`, `rv=0`) plus two 12-20-node empty-url SVG documents (`rv=1`). Against that,
  `https://dzen.ru/` climbs monotonically in every run (fresh process -> ~1 GB in ten loads; a second run
  `+549 MB` over ten; a default-configuration `.50` run `130 -> 535 MB` in three), so what differs is
  *something about that page*, not navigation as such. **Whether that is a port defect or a page-side
  defect any browser would have is NOT yet separated**, and that separation - not another release lever -
  is what the next round has to produce.
- **Next steps** are item **0j**, whose remaining step is now *attribution*, not a default decision. Do
  not build a fix on the assumption that the retained Pages, the caches or the decoded images are the
  leak: every one of those was believed and each was withdrawn. **Six explanations that merely fit have
  been wrong here; the seventh will not be distinguishable from them without a control.**

### 0c-addendum. What the same evening added, 2026-09-24 (build `.51`, no rebuild — measurement only)

Two new items came out of measuring rather than reasoning, and both belong to the MVP's own acceptance
criterion rather than to the memory front:

- **Item 0m — the acceptance criterion is now measurable, and it costs 8.8 s of silence per tap.** The
  scripted-tap driver `Src\tools\tap-test.ps1` exists, three real verdicts were produced on three real
  sites, and the engine was seen following an anchor by itself. The latency is the finding.
- **Item 0n — `habr.com/ru/feed/` intermittently never builds a body** and shows the user the harness's
  empty-page panel. `analyze-loader-trace.ps1` calls it a `HARD STALL`; the candidate mechanism is
  `pumpLoop` exiting with the loader still holding work, which is a port-side lifetime question and a
  cheaper fix than anything on the memory front.
- **And one correction to how every number above must be read:** this bench's `settings.ini` carries
  **`gpudefault=1`**, so the GPU probe has been armed for the recent runs even though the build notes
  describe the seeded default as `0`. The GPU engaging doubles the layout
  (`contents=1024x26118` -> `2048x52530`) and that single step measured **+1.2 GB** (876956 ->
  2075540 KB) — which is also the largest single contributor found so far to the RSS figures in item 0j.
  Attribute from the log's `EnableGpu:` lines, never from an assumption about the configuration.

## Next steps (agreed 2026-09-18)

0. ~~**Verify the fix on the bench.**~~ **Done** — `0.1.9.106` scrolled dzen.ru top-to-bottom with no
   `SIGABRT`, no exit code 3, no assert string, and `glyph.log`'s `platformInit:` lines carry real
   non-zero sizes (9.3 … 32.0, `isFT=1`), so the guard did not disable normal font creation. Still
   worth doing: port the same guard to any other CSS-size → `FontPlatformData` construction in the
   fork (`createFontPlatformData` and the fixed stub are the only two found in `Src\port`).

0a. **Find what swallows the termination request — DOWNGRADED, not needed for 0b's answer.**
   `DZEN-SCROLL-DEATH.md` §10e, measured on `0.1.9.109`: **the watchdog's termination works.** A
   `while (true)` inside a `setTimeout` (the same timer entry path §10c recorded) is stopped within
   seconds of the fire, the page renders afterwards, memory stays flat, and the new JS-stack probe
   named the loop (`runaway.html:12:22 :: neverReturns`, `:19:17 :: runawayTimer`) with a live
   `topCallFrame`. So the question is not "termination never takes effect" but "what does the dzen.ru
   loop cross that a single-frame loop does not" — an event/dispatch or microtask boundary that
   observes and clears the pending exception. Suspects are unchanged and now worth testing against
   **the probe**, not a debugger: `JSEventListener::handleEvent`, `JSExecState::didLeaveScriptContext`
   / `~JSExecState`, `reportException` in `EventTarget::innerInvokeEventListeners`, and
   `Microtasks.cpp:105`. The probe writes to `gpuinit-steps.txt`, so a fire during a real freeze now
   yields the script name and position outright.
0b. **Decide whether the runaway is the page's or the port's — ANSWERED: the port's. It was Web
   Storage.** §10f: the port installed no `StorageNamespaceProvider`, so `Page` kept
   `EmptyStorageNamespaceProvider` from `pageConfigurationWithEmptyClients()`
   (`WebCore/loader/EmptyClients.cpp`), whose `StorageArea::setItem` is an empty body — `localStorage`
   existed, threw nothing, and stored nothing. dzen.ru's settings-sync module converges only when a
   write is visible to the next read, so it re-entered itself synchronously, forever. Confirmed by
   measurement in item 0c. The original question, kept for the record: the cycle is page-initiated
   (`dispatchEventForBindings` — the port never calls `dispatchEvent` from C++, verified by grep), but
   *something* has to be feeding it. dzen.ru does not freeze like this in a released browser, so either
   a feature we answer wrongly keeps a page-side loop from converging, or an event we generate is
   re-entering WebCore. The instrument that answers it is the existing one: `gpuLogMarkerF` into
   `gpuinit-steps.txt` (the channel that has survived every crash and hang here) carrying the event
   type and the dispatch nesting depth. Do **not** add a debugger to that loop again — §10a.
   **One candidate is now eliminated, measured:** a listener that queues a microtask which dispatches
   the same event again does *not* freeze (`LocalState\dispatchcycle.html`, `0.1.9.109`) — the
   checkpoint does not re-drain page-queued microtasks as unbounded synchronous nesting in this build.
   §10e measurement 2. So the cheapest reconstruction of §10c's cycle is wrong, and the real one either
   runs through WebCore's own `runInternalMicrotask` → `processSpeculationRules` path or is not
   synchronous at all.
0c. **Reproduce the freeze properly — DONE 2026-09-18 on `0.1.9.110`, and it did not freeze.**
   The maintainer performed the same three touchscreen swipes down the dzen.ru feed that froze `.109`,
   at 17:57:56–17:58:00. Recorded outcome, all read from `LocalState` afterwards and not from feel:

   | Signal | `.109`, after the gesture | `0.1.9.110`, after the gesture |
   |---|---|---|
   | `heartbeat.txt` | frozen: `tickstep=1`, `busy=1`, no `finished` advance | idle: `busy=0 pending=0 tickstep=10`, `finished` advanced 134 → 954 |
   | `js watchdog fired` in `gpuinit-steps.txt` | present, probe named the page loop | **absent** for the whole session |
   | `storage: set origin=https://dzen.ru` | absent — the write was a no-op | present, 14 distinct keys incl. `ludca` (128 B), `beerka`, `rb_sync_id`, `tracer-device-id` |
   | crash / `SIGABRT` / wedgedump from the gesture | one recorded | none |

   The engine *did* stall twice during the gesture (`beat-stuck #1–#2`, `job=live-tick`, `finished`
   stuck at 556 and 392 for ~0.7 s each) — but in both cases `finished` **resumed advancing**, which is
   the known lazy-bytecode-compile scroll stall (§10b; memory entry
   `dzen-scroll-stall-is-lazy-bytecode-compile`), not the runaway. The one `WEDGE` dump in the session is
   at 17:56:45, during the *navigation*, six beats with `job=nav-load`, and it cleared itself in two
   seconds (`[STAGE] after-load rc=0 compositing=1` at 17:56:47); older sessions show the same two `WEDGE`
   lines per run, so it is pre-existing and not introduced here.

   **What this does and does not establish.** It is one run, and the gesture is not mechanically
   repeatable — treat it as strong evidence that the storage fix removed the freeze trigger, not as
   proof that no freeze remains. `jstack.txt` is still in LocalState, so the probe stays armed for the
   next attempt. The historical text of this item follows, because it is still the recipe:

   §10e measurement 3: two runs that loaded
   dzen.ru over `nav.txt` and were left alone, one in a freshly restarted process, rendered
   (`nonwhite` ≈ 301 000/710 656, `bodyKids=64`, `scripts=49`), advanced `finished` to 611 and then sat
   idle for five minutes with `tickstep=10` and flat memory. No watchdog fire. **The runaway is armed
   by interaction** — the scroll, and in one recorded case a window resize immediately before the tick
   that never returned. A scripted `NavigateTo` cannot reach it, which is why every scripted attempt
   has missed it. Ask the maintainer for the gesture while `.109` is installed; `jstack.txt` is
   already in LocalState, so the console self-test is armed too. *(Historical: the gesture was
   performed on `.110` on 2026-09-18 — see the table above.)*
0d. **Audit the `stubs-*` files as a class, and re-derive the long-term estimates from that.**
   Requested by the maintainer on 2026-09-18: *"Стабы — это заглушки, часто содержащие недоделки?
   Впору перепроверить их все, чтобы скорректировать прогноз по времени приведения браузера
   Apotheosis в чувство."* The premise is right, and §10f is the proof: the most expensive defect in
   this project so far was a **stub that lied quietly**. A stub is only tolerable when the caller is
   built to survive it. So the audit classifies every stubbed symbol three ways:

   * **HONEST FAILURE** — returns an error the caller is designed to handle (a failed load, a missing
     feature the page can feature-detect). Cost: the API is simply absent.
   * **SILENT LIE** — returns something that looks like success and is empty or fake. Cost: a page
     loops, mis-syncs, or renders wrong, with no trace anywhere. **This is the class that has to be
     emptied**, and it is why every area now logs.
   * **NEVER REACHED** — no caller in this configuration. Cost: nothing today; a landmine the day a
     page or an upstream change reaches it.

   Progress so far:

   | Stub file | Class | Note |
   |---|---|---|
   | `stubs-sqlite.cpp` | **HONEST FAILURE** | every open → `SQLITE_CANTOPEN`; `SQLiteDatabase::open()` returns false and the caller degrades. **Not** the §10f mechanism — the storage defect was in WebCore's client wiring (`EmptyStorageNamespaceProvider`), not here. It is still the reason `PortStorage.cpp` is in-memory rather than database-backed. |
   | `stubs-crypto.cpp` | HONEST-AND-LOUD, plus a **stale header comment** | all WebCrypto operations `RELEASE_ASSERT_NOT_REACHED()`, so the registry stays empty and `window.crypto.subtle` is simply absent. The header claims it provides a `PAL::CryptoDigest` with an all-zero `computeHash()` — it does not, and the real one is BCrypt-backed (`PAL/pal/crypto/win/CryptoDigestWin.cpp`); `cryptographicallyRandomValues` ends in `BCryptGenRandom`, so `crypto.getRandomValues` is sound. |
   | `stubs-loader.cpp` | **NOT A STUB** | `CurlSSLHandle::platformInitialize()` sets a real cipher list and EC curves. Misnamed file. |
   | `stubs-network`, `stubs-ax`, `stubs-other`, `stubs-pasteboard`, `webcore-driver-stubs` | *to classify* | — |

   **DONE 2026-09-18.** The full audit is `Doc/STUB-AUDIT.md`: per-file verdicts, the two build lists
   (which files are compiled into `WebCore.dll` and which into the driver archive — and which are compiled
   by *nothing*), the six findings ranked by user-visible cost, and the three claims that had to be marked
   *unverified* rather than asserted. The forecast effect is **Phase 2.5** below, added from that table
   rather than from impression. The headline: the port has no "tofu" problem, it has a **face-selection**
   problem — `FontCache::createFontPlatformData` returns `nullptr` and faces are chosen by filename — so
   the largest remaining fidelity gap is not in the stubs at all but in the bundled-font substitute
   backend behind them.

0e. **Two failed taps on dzen.ru after the freeze fix — DIAGNOSED 2026-09-18, one half fixed.**
   Reported by the maintainer immediately after the gesture that confirmed 0c: *"я на dzen.ru кликнул на
   какую-то ссылку — страница лишь дрогнула, но навигации не произошло. Второй раз попробовал на другой
   ссылке — на экране жуть какая-то появилась с error curlcode=28."* Both are in
   `Doc/TOPLEVEL-FETCH-BUDGET.md` with the raw log; the short version:

   * **Tap 1 (the flinch) — the harness swallowed it, and the reason is not observable.** The driver's
     `[HIT]` probe confirms the tap landed on a real `<a href>` and that the page's release handler ran
     (`press=0 release=1 connected=1`), yet neither side navigated. `ForwardClickToEngine`'s dispatcher
     (`MainPage.xaml.cpp:3433-3447`) navigates only when `navW.empty() && !changedCopy && !linkHit.empty()`;
     on dzen.ru — a React SPA — the tap makes the page repaint, so `changedCopy` is true and the code takes
     its "the page handled it in-page" branch. **Which branch actually ran is a hypothesis**: the dispatcher
     logs none of its inputs, and that is the first thing to fix. Its frame-hash test ("the frame changed,
     therefore the page handled the click") is false for any framework that repaints on hover.
     **CLOSED 2026-09-18 in `0.1.10.7`** — and the gate turned out to be wrong in *both* directions, which
     only a controlled measurement could show: three local pages differing solely in their `click` handler
     produced `branch=nav-link` for the page that called `preventDefault()` with no repaint (the harness
     navigated **against the page's explicit refusal**) and a correct `in-page` for the page that
     preventDefaulted *and* repainted — correct by luck, saved by the repaint rather than the refusal. The
     dispatcher now adds a navigation only when `rc==0 && navW->empty() && !linkHit->empty()` **and the page
     did not call `preventDefault()`** — a new ABI export `WebCoreLastClickDefaultPrevented()`, read from the
     existing `[HIT] dom` readback (`dpAfter=`) that was already being captured after the settle pump.
     `wasHandled()` was measured first and rejected: it is `true` in both the navigating and the refusing
     case, and `HandleUserInputEventResult` in 2.52.4 has no `isDefaultPrevented()`. The frame hash is still
     printed but is no longer a gate. `Doc/TAP-DISPATCH.md`.
   * **Tap 2 (the "жуть") — a correct page shown as an error.** Navigation started, `WebCoreSessionLoad`
     waited its hardcoded **2 500 ms**, gave up, showed the error page (`rc=-16`) — and seconds later the
     same transfer finished **`size=1016600 http=200 crc=0`** and was discarded. libcurl never timed out.
     The `curlcode=28` in the banner is a constant the consumer presets itself, reported as though libcurl
     produced it, which is how it was read — twice, including in the first write-up here.
   * **Fixed in `0.1.10.0` — diagnostics *and* behaviour.** The failure is filed as
     `phase=toplevel-fetch type=PortBudget curlcode=0 domain=port-budget`, and the worker writes an
     explicit `SL: DISCARDED -- … no second request was made. If this line is present, the error page was
     wrong.` The budget itself then stopped being a fixed deadline: it is a **progress-aware stall
     detector** — 2 500 ms, re-armed while the byte counter is still advancing, with a 9 500 ms ceiling
     that sits just above the worker's own 8 s so the worker's curl error is preferred to the consumer's
     guess. The protection against a dead radio survives by delegation, and the worker turned out to
     have had the correct detector all along (`CURLOPT_LOW_SPEED_LIMIT/TIME` plus its `XFERINFO`
     callback). Full reasoning and the trade-off: `Doc/TOPLEVEL-FETCH-BUDGET.md` §3/§5.
   * **Measured on the bench the same evening, `0.1.10.0`.** The article that used to be discarded now
     loads (1 010 647 B, `http=200`, `crc=0`, `nav: OK rc=0`) — but it fit inside the budget, so that
     pass proves nothing about the re-arm. Forcing it with a 3 MB transfer that cannot fit in 2 500 ms:
     `SL: curl bg: still moving (dl=1383670, 2512 ms in), re-arming`, then the transfer completed
     4 457 ms after the navigation was issued. **That transfer is the old failure case exactly** — the
     old build abandoned it at 2 500 ms and reported `curlcode=28` while the bytes were on the wire.
   * **`0.1.10.0`** — the version after this milestone, at the maintainer's direction: after `0.1.9.110`
     the project moves to `0.1.10.0`. `Package.appxmanifest` is bumped; `0.2.0.0` remains the target for
     the end of the whole phase.

0g. **The wedge watchdog had been calibrated against the budget `0e` replaced — now it asks instead.**
   `MainPage.xaml.cpp` dumps a wedged engine after six beats (~4.2 s) of an unchanged `finished` counter,
   and its comment said why: *"6 x 700 ms = 4.2 s, comfortably past the 2.5 s fetch ceiling"* — a number
   from a different file, invalidated silently by the re-arm. The 3 MB run produced the predicted false
   positive one second before the page finished loading (`WEDGE: no progress for 6 beats
   (finished=549 busy=1 loading=1)`, then `after-load … rc=0`), costing a full stack walk of every thread
   for a page that was loading correctly; on the device it would fire on **every page slower than ~4.3 s**.
   Fixed the same way the budget was: stop estimating. New export in **both** ABI headers,
   `long long WebCoreGetFetchProgress(void)` — bytes so far, or -1 when no top-level fetch is in flight —
   with `ApoFetchChannel::inFlight` behind it; advancing bytes now count as progress while `finished`
   stands still, and `beat-stuck` prints `fetchdl=`. Six beats still means "nothing moved for ~4.2 s",
   and that is now literally true. `Doc/TOPLEVEL-FETCH-BUDGET.md` §7b. **The lesson is the transferable
   part:** two files coupled by a magic number, with nothing at the change site to say so.
   **Verified the same evening (`0.1.10.1`)** on an endlessly streaming public source that blocks the
   engine thread for ~8 s while ~44 KB/s keeps arriving: three consecutive `still moving … re-arming`
   lines, **no `WEDGE` line anywhere in those eight seconds**, and the failure attributed to curl in
   curl's own words (`type=Curl curlcode=28 desc=Operation timed out after 8036 milliseconds with
   366132 bytes received`). The same stimulus under `0.1.10.0` gives an error page at 2.5 s plus a
   `WEDGE` at ~4.2 s. One residual found by that run and fixed: the counter was advanced only from
   `XFERINFO`, whose cadence (~1 Hz) is slower than the 700 ms heartbeat, so isolated `beat-stuck #1`
   lines still appeared; it is now advanced from the write callback, on every body chunk. Re-measured on
   `0.1.10.2` with a positive control this time — `fetchdl=0` on the second beat proves the counter is
   published and read while the engine is blocked, then nine further beats of the same 8 s block produced
   **no `beat-stuck` and no `WEDGE` at all**. §7d.

0f. **Seven user-visible strings were destroyed (not mis-encoded) and are now rewritten.**
   `Doc/DESTROYED-STRINGS.md`. The bytes are literal `?` (`0x3F`) in the file, so nothing was recoverable;
   the same files are otherwise genuine UTF-8 with working Cyrillic, which bounds this to nine literals —
   six in the built-in **home page** and the **error page** HTML, one tab-switcher title, one driver
   diagnostic. The home and error pages are still **unlocalized** (static HTML built with no access to
   `m_uiLang`); that gap is recorded, not closed.

0h. **habr.com killed the engine thread with an AV reading `0x10` — the null font family; producer fixed
   and verified on the bench 2026-09-18.** `Doc/FONT-NULL-FAMILY-CRASH.md`. A null
   `AtomString` family inside a `FontCascadeDescription` is hashed by `CSSFontFaceSet::fontFace`
   (`m_facesLookupTable.find(family)` → `ASCIICaseInsensitiveHash::hash(const StringImpl*)`, whose only
   guard is an `ASSERT` that release builds compile out) and dereferences `nullptr`. The producer in
   *this port* is `stubs-other.cpp`'s `SystemFontDatabase::platformSystemFontShorthandInfo`, which
   returned `{ AtomString(), 0, FontSelectionValue() }` — breaking two invariants upstream documents
   (a non-empty family; a non-zero size, the latter being `FontPlatformData::emptyValue()`).
   Three edits: the stub now answers `{ standardFamily, 16, normalWeightValue() }`; `CSSFontFaceSet::fontFace`
   refuses a null key (insurance — it is the single choke point every path hashes through); and
   `FontCascadeFonts::opportunisticallyStartFontDataURLLoading` names the event through
   `apotheosisWebTrace` (silent unless `APO_TRACE_TEXT=1`). Four isolating test pages proved a plain
   stylesheet **cannot** supply the null (the generic-family whitelist, the `unset` re-application of the
   invalid-at-computed-value-time flag, and the shorthand resolver's null filter each close one door), so
   the exact installing call site is still open — the instrumentation answers it on the next occurrence.
   Class: the `SILENT LIE` of `Doc/STUB-AUDIT.md` §4.4, now four entries long.
   **Verified on `0.1.10.4`, 2026-09-18:** habr.com loaded twice, no UEF and no crash verdict, the
   engine idle at `finished=471`, `nonwhite=200299/1036944`; both guard strings confirmed compiled into
   the shipped `WebCore.dll`; the `ofd:`/`fset:` trace confirmed able to fire and confirmed silent when
   unarmed. The null did not recur, so §4 stays open — a *missing* diagnostic only means something once
   the diagnostic is known to be in the binary and known to be able to fire, and both were checked
   separately, in both directions. One durable toolchain fact came out of it: **the bench cannot pass
   an environment variable to the app it launches** (`Start-Process "shell:AppsFolder\…"` activates via
   the shell), so `APO_TRACE_TEXT` is armed by `LocalState\texttrace.txt` instead — the `jstack.txt`
   pattern, and now the third switch of that shape.

0i. **The `CalculationValue`-handle crash was real and came back through the *other* overload —
   root-caused from the VEH stack and contained, 2026-09-18.** `Doc/CALC-HANDLE-DANGLING.md`.
   Item 0a of the morning (`.97`, "`0xc0000005 @ WebCore.dll+0x204901f` did NOT reproduce …
   mechanism not proven") closed the question one measurement too early: the guard written on
   2026-09-17 went into `LengthWrapperData::nonNanCalculatedValue(float, const ZoomFactor&)` only,
   while the deterministic engine-thread crash arrives through
   `nonNanCalculatedValue(float, const ZoomNeeded&)` — `StyleLengthWrapperData.cpp:113`. What the
   VEH record plus an offline symbolisation established:

   * The fault is `movzbl 0x8(%rdx), %eax` inside the fork's `WTF::visitOneVariant`
     (`wtf/StdLibExtras.h:548`), i.e. `v.index()` on a **31-alternative** variant — the only such
     variant on this path is `Style::Calculation::Node`. `%rdx` held **7** or **8**: a *reference
     that was never an address*, which is why the faulting data address is `0xF`/`0x10` rather than
     a plausible wild pointer. The chain below it is
     `Style::Calculation::Value::evaluate` ← `nonNanCalculatedValue` ←
     `StyleTranslateTransformFunction.cpp:92` ← `StyleTransformResolver` ←
     `RenderLayerCompositor::updateCompositingLayers` — the **compositing** path, not layout.
   * Mechanism: `LengthWrapperData` holds an unsigned handle into the process-wide
     `Calculation::ValueMap`; `ValueMap::get()` / `deref()` guard only with `ASSERT`, which release
     compiles out, so an overderef'd handle makes `find()` return `end()` and the code proceeds with
     a `Calculation::Value&` read out of the hash table's own storage.
   * **Fix:** one containment point, `LengthWrapperData::hasLiveCalculationValue()`
     (`ValueMap::contains`), used by **both** overloads and by `isCalculatedEqual()`, which reached
     `calculationValue()` unguarded as well. A stale handle returns 0, exactly as a NaN result
     would. Guarded `#if defined(WK_WINUWP)`, which both toolchains set.
   * **Two traps documented there for the next reader:** (a) `WTFLogAlways` on this port sinks to
     `OutputDebugStringA`, **not a file** — the 2026-09-17 comment claiming the guard's hits are
     recorded is wrong, and `grep "[APO calc]"` finding nothing proves nothing; (b) the `VEH` lines
     I was reading spanned **two builds** whose every frame differs by exactly `0xD0` in `.text`,
     and RVAs from different builds must never be mixed. The offline symbolisation recipe
     (`.pdata` for the function, line tables for the file — section-relative, so subtract the
     `.text` RVA) is in §3 of that document, because `llvm-symbolizer` and `cdb` both answer
     `??:0:0` on this clang-cl PDB.
   * **Still open:** *who* overderefs the handle. Containment stops the crash; it does not explain
     the stale handle. The first step is a file sink for the guard's hit — it is invisible today.
0j. **Bounded memory — LIVE item, and its remaining step is ATTRIBUTION, not a default decision.**
   Chosen by the maintainer on 2026-09-24 ("bound the memory") over the
   `ENGINE-INITIATED-NAV-CANCEL.md` architectural work, which stays untouched in item 0k. Section
   **0c** above carries the measurements; this item carries the steps and their order. Steps 1, 2b and 2c
   are done — **and 2c's answer came back negative, which is what the remaining work has to be rebuilt
   on top of.** Nothing in this item has been built for ARM32 or measured on the device.

   * Done: the per-navigation `releaseMemory` (kept — it returns memory at the margin and bounds the
     caches, and it is the port-side half of wiring the port always claimed), the harness memory-level
     handlers (they post a release job; **no device test**), the live-Page instrument, the memory
     instruments (`SL: mem` / `SL: docs` / the priced `SL: doc` lines) — and **six** falsified hypotheses
     (the font cache, the loader, `Critical::Yes`-reaches-the-accumulator, the JS heap, retained
     Documents, and decoded image data as the accumulator). Two working levers were claimed and both
     were **withdrawn**; 2c explains why.
   * **Step 1 — DONE 2026-09-24. The SVG hypothesis is CONFIRMED, and three explanations of the
     retention were killed.** Ten external svg images produce exactly ten hidden Pages; the Pages are
     `utility` (the flag `SVGImage` itself installs); JS holding the image elements, CSS-reached images
     and svg nesting all release normally. Details in section **0c**. The instrument is
     `Src\tools\make-svg-test-pages.ps1` + `Src\tools\svg-page-test.ps1`, and its results are read out of
     `SL: live pages` lines in `gpuinit-steps.txt`. Rule for the next test built on it: a page that
     merely *fits* an explanation has been wrong four times here — make the page differ in exactly one
     respect from its control, as `tjs`/`tjs2` do.
   * **Step 2 — CLOSED NEGATIVE 2026-09-24 (.49/.50), and the method is the finding.** The question was
     what evicting the resource cache costs. The answer is that the arm never had an effect to price:
     `evictResources()`'s "+156 -> +65 MB/load" compared **one process in two sequential windows**, the
     disarmed one first and therefore cold. In one warm process, alternating the switch and holding
     everything else constant, the two arms are indistinguishable, and the per-navigation spread *within*
     one arm (+/- ~60 MB on a real page) exceeds any difference *between* arms. **The rule this bought, and
     it applies to every arm in this item: two windows of a run are not two arms. Alternate inside one
     process, and never compare a cold window against a warm one.**
   * **Step 2b — DONE 2026-09-24 (builds .43-.48). The accumulator is NAMED: decoded image data.**
     Instruments added, all in `Src\port\WebCoreDriver.cpp` and all logged into `gpuinit-steps.txt` at
     both `after teardown` and `after build`:
     `apoLogMemoryStats()` (`PerformanceLogging::memoryUsageStatistics` for the JSC heap, plus
     `MemoryCache::getStatistics()` for the cache) and `apoLogLiveDocuments()` (`Document::allDocuments()`
     with each document's url).
     * **~~The JSC heap saturates and is not the accumulator.~~ WITHDRAWN.** `javascript_gc_heap_size_mb`
       and `javascript_gc_object_count` grew 65 -> 96 -> 136 and appeared to freeze at **113 MB /
       1 722 272 objects**; that was three to six samples. Over **26** navigations the series climbs
       6, 10, 14 ... 90 with no plateau. The reading method survives (a full `garbageCollectNow()` before
       every sample, so the objects counted are live).
     * **~~Retained Documents saturate too.~~ WITHDRAWN — `document_count` grows ~+4/navigation without
       bound.** What survives is their identity: an SVG image's document has an **empty** url (proved on
       the controlled pages — `t10.html` added exactly +10 empty-url documents and +10 Pages, all freed at
       the next teardown), so the dzen-url documents are page-created ones (DOMParser -> `about:blank`,
       XHR-as-document -> the response url) and not svg images.
     * **Iframes are not a candidate and cannot be: the port has no subframes at all.** See item **0l** —
       that experiment was *invalidated*, not answered, which is why the item is written up separately.
     * **The resource cache holds nothing dead** (`liveSize == size` for every type on every load), which
       is the proof that the non-critical release path can never free anything — and the unit is **bytes**,
       not KB (`CachedResource::size()` = `encodedSize + decodedSize + overheadSize`; the log label was
       wrong by 1000x and the field names are now `sizeB`/`liveB`/`decodedB`).
     * **No readable engine-allocator total exists on this port.** `WTF::fastMallocStatistics()` is
       exported from `JavaScriptCore.lib` but on Windows is `GetProcessMemoryInfo().PeakWorkingSetSize`
       with the bmalloc fields hardcoded to 0 — it was wired in, tracked RSS to the megabyte
       (137 -> 880 MB), and that lockstep *was* the tell; removed, with the reason recorded in the function
       comment. `WTF::memoryFootprint()` is guarded out of the App Container and returns 0 on both
       architectures. Attribution therefore stays with the process RSS plus WebCore's accounting.
     * **The harness is exonerated** for the per-navigation part: five consecutive `file://` loads moved
       RSS 733 -> 687 -> 674 and 670 -> 674 -> 678, flat to slightly down.
   * **Step 2c — DONE 2026-09-24 (.47-.50): the decoded-image store was priced, reached — and the answer
     came back NEGATIVE.** The pricing stands; the lever does not.
     * **Priced on controlled pages** (`Src\tools\make-image-test-pages.ps1`, a grid that fits inside the
       viewport on both lines so every image is painted and decoded): twenty 1200x1200 PNGs drawn at
       200x100 cost **+116 MB on the first paint** against a predicted **110 MB** of ARGB pixels — the
       first figure in this project that has matched a prediction — and then flat across four more loads,
       because on a `file://` page `after teardown cache_images count=0`. The controls (the same grid with
       no images, and with twenty 32x32 images drawn into the same cells) are flat at +1 MB/load.
     * **On dzen.ru the same reading is the opposite**: `after teardown cache_images count=17
       sizeB=1008233 liveB=1008233` — images, scripts and CSS **survive every teardown**, always live.
     * **And `decodedB = 0` for images on every load including those 110 MB of known-decoded pixels.** Since
       `size()` *includes* decodedSize, that is not under-reporting: the Cairo image path never calls
       `CachedImage::decodedSizeChanged()`, so an image never enters `m_liveDecodedResources`, the list
       `pruneLiveResourcesToSize()` walks, and `destroyDecodedData()` is never called on it. **This is a
       real code-level gap and it is the reason both calls are kept available** — but it is a gap in the
       release paths, *not* evidence of what the accumulator is, which is the distinction the next bullet
       turned on.
     * **The lever was promoted to the default on `.49` and reverted on `.50`, because the reading that
       justified the promotion was one outlier.** The withdrawn table read:

       | arm | deltas | slope |
       |---|---|---|
       | disarmed, 6 loads | 242, 157, 168, 50, 62 | **136 MB/load** |
       | armed, 6 loads | 22, 89, 43, 53, 83 | 58 MB/load |
       | armed, 4 more loads | 4, 79, -20 | 21 MB/load |

       **-57 %** was published from it and is **withdrawn**. The armed window's *first* delta was -125 MB;
       averaged over five loads it produced the "flat", and the same run's remaining deltas were +37, +51,
       +55. A process does not shrink 125 MB because a cache was evicted. Both windows came from **one
       process, disarmed first and therefore cold** — the same method error as `evictResources()` in step 2,
       and the `-58 %` is withdrawn with it.
     * **The controlled re-measurement (`.49`, one WARM process, one build, alternating the switch):**
       `arm OFF 52 MB/load (23, 64, 69, 51)` against `arm ON 47 MB/load (26, 57, 57, 48)`; then interleaved,
       2 loads per arm x 3 cycles: `OFF 5, 4, 44 / ON 54, -71, 48`. **The lever is inert at this sample
       count** — the spread within one arm exceeds the difference between arms. Re-measured in a *fresh*
       process the same arm reads 97 MB/load, which is the warm-up again.
     * **Therefore the "44-58 MB/load floor both levers leave behind" does not exist**, and neither does the
       argument built on top of it ("two levers landing in the same place proves decoded image data is the
       bulk of the accumulator"). There was no floor and no landing place. What the code now does:
       `MemoryCache::destroyDecodedDataForAllImages()` and `MemoryCache::evictResources()` are both called,
       **armed by `LocalState\cacherelease.txt` and OFF by default**, and the state is printed on every
       navigation (`SL: cache release after teardown: DISARMED (cacherelease.txt absent)`) so no run can be
       misread afterwards. The full withdrawal is in the function comment on `apoReleaseResourceCache()`
       in `Src\port\WebCoreDriver.cpp`; that comment, not this item, is the authoritative version.
   * **Step 2d — DONE 2026-09-24 (.49/.50), and it turned the item round.** The step was written as "price
     the lever, then choose the default". Both halves are settled, and neither went the way the step
     expected: there is no lever to price and therefore no default to choose.
     (a) **The lever's cost is MOOT, because the lever has no effect to price.** The `+645 ms, about +7 %`
     reading below is **withdrawn**: it compared 4 disarmed loads against 6 armed loads taken after them,
     and a second run of the same arm produced a *lower* median (8096 ms) than the disarmed arm (8636 ms).
     Within-arm spread exceeds between-arm difference here too, so `load-time.ps1` — an otherwise correct
     instrument — was asked a question its sample count cannot answer. The script itself stays, because it
     reads the engine's own `[STAGE] before-load` / `[STAGE] after-load` markers out of `log.txt` instead
     of polling, and that is what makes it usable at all.

     <details><summary>the withdrawn table, kept as the record</summary>

     | arm | load durations | median |
     |---|---|---|
     | disarmed | 1571, 7591, 8636, 8892 ms | 8636 ms |
     | armed | 8471, 9283, 9003, 9191, 9281, 9471 ms | 9281 ms |

     </details>
     ⚠️ **The first version of that script polled for the next `diag:` line and was invalid**, in a way
     worth keeping: the harness writes `diag:` after every load *and after every resize*, so "the next
     diag" is not "the load finished". Five samples read 1941, 8373, 214, 11732, 11481 ms for one page and
     one configuration, and the 214 ms sample is a resize diag. The fix was to stop polling and read the
     load's own span from the log. Do not re-introduce a `diag:`-based synchronisation here — the same
     trap is described in `rss-loop.ps1`, where the sync is only used to pace the loop, not to time it.
     (b) **The floor is withdrawn with the lever** — there is no 44-58 MB/load floor; it was the residual
     of a cold-window-against-warm-window comparison. Nothing about it needs an instrument.
     (c) **No default is chosen, because there is nothing to default to.** The two calls stay in the code
     as a **default-off escape hatch** (`cacherelease.txt`), kept for a reason independent of every
     withdrawn number: the upstream release paths provably cannot reach decoded bitmap data on this port
     (the `decodedB = 0` bullet in 2c), so a page that *does* accumulate decoded surfaces has no other
     lever. Leaving them off is not a delay in the decision — it is the correct reading of the measurement.
     (d) **What replaces 2d is attribution, not release.** Every controlled arm built so far plateaus:
     20 viewport-fitting 1200x1200 PNGs (+116 MB once, then flat), ten external SVG images (~0.4 MB/Page,
     self-clearing), documents created and dropped or kept by page JS (flat — and the "kept" arm is
     structurally unable to see a cross-navigation leak, since `window.__keep` dies with the page at
     navigation), 20 layers promoted by `will-change: transform` (all-positive deltas `24, 8, 71, 24`
     against a flat control, but 8 consecutive loads read `80, 22, -15, -31, -2, 21, -10` — an ~**80 MB
     level shift, not a slope**, with `compositing=1` vs `0` verified in the log), and both cache levers
     (inert). Heavy real pages do not plateau: **dzen.ru x12 from a FRESH process, default configuration,
     read 136 -> 1142 MB with every delta positive (`264, 155, 84, 60, 86, 49, 55, 86, 42, 43, 82`,
     ~91 MB/load, no plateau)** — and habr.com, in the same warm process, is a sawtooth that averages
     +53 MB/load (`237, -92, 22, 205, -70, 63, 9`).
     **But the same dzen.ru, warm at ~1.8 GB, read +7 MB/load and the process SHRANK by 429 MB over eight
     loads.** So the slope is a fresh-process phenomenon and the releases only engage near a high
     watermark; on a 3 GB Lumia that watermark is reached far sooner, which is exactly why the curve below
     it still matters. **The next round has to separate "a port defect" from "a page-side defect any
     browser would have"** — and the process RSS plus WebCore's own accounting have now been exhausted as
     sources for that. Two candidates no measurement has excluded: the memory a real page allocates
     through paths this port stubs out (`Doc\STUB-AUDIT.md` is the inventory), and compositing, the only
     structural difference verified between the flat pages and the growing one.
     (e) **The harness memory-pressure handler has never fired — in any log.** Every log in `LocalState`
     says `memory-level handlers registered, mem 35200/12497952 KB level=low`, and there is **no
     `mem-level UP` line in any of them**: the bench sits at `level=low` with a 12.5 GB limit and never
     approaches a threshold. So the handler wired on 2026-09-24 is untestable on the bench by
     construction, the releases in (d) are not it, and the device is the only place it can be exercised —
     which is also the only place it matters. **It must not be reported as verified.**
     ⚠️ **Bench state as of 2026-09-24, build `.50`: DEFAULT configuration.** `decoded.txt` and
     `evictcache.txt` were deleted and are read by no code path; the only switch left is
     `cacherelease.txt`, absent, and the build **prints its own state on every navigation**
     (`SL: cache release after teardown: DISARMED (cacherelease.txt absent)`) so that no run can be
     misread afterwards — silence about configuration is what the previous two rounds were lost to. Check
     the file before reading a run, exactly as with `-Gpu`.
     **ARM32 has none of this wired into a built artefact and no device measurement of any of it.**
   * **Step 3 — measure what the current release costs us.** `releaseMemory(Critical::Yes,
     Synchronous::Yes)` now runs on every navigation and calls
     `GarbageCollectionController::deleteAllCode(PreventCollectionAndDeleteAllCode)`
     (`MemoryRelease.cpp:155-158`), i.e. it throws away all JIT code each time — a plausible reason for
     the lazy-bytecode-compile scroll stall (`dzen-scroll-stall-is-lazy-bytecode-compile`). Unmeasured;
     a navigation that re-JITs everything is not free on a Lumia. **This is now the only per-navigation
     release in the default configuration** — the cache pair runs beside it only when `cacherelease.txt`
     is present, so the two are separated by default and a cost reading no longer has to disentangle them.
     Measuring it is the one item here that does not depend on attribution: it is a real cost paid on the
     phone for a benefit (`-126 MB over three light loads`) that was itself never re-measured in a
     controlled arm and should be treated with the same suspicion as the two withdrawn levers.
   * **ARM32:** every change here is port-side, no ABI change, no new archive. The ARM line was **not**
     built and **not** device-tested for any of it — "portable in principle" is not "verified".
0k. **The `ENGINE-INITIATED-NAV-CANCEL.md` architectural fix — deferred, not dropped.** The maintainer
   called this the cooler option; it was not chosen for this cycle. Also still open from that document:
   it references a section 7 that does not exist, and `Package.appxmanifest` still carries the
   destroyed `??` strings.
0l. **The port has no subframes at all — a capability gap, found 2026-09-24, and a methodology trap.**
   `LoadingFrameLoaderClient::createFrame()` returns `nullptr`
   (`Src\port\LoadingFrameLoaderClient.cpp:265`), inherited verbatim from `EmptyFrameLoaderClient` and
   sitting under the banner comment that says so; `createPlugin()` next to it does the same. The port
   therefore creates **exactly one frame**, and every `<iframe>` on every page is a no-op that is parsed,
   laid out and painted as an empty box. Measured on the controlled pages
   (`Src\tools\make-iframe-test-pages.ps1`, JS-free on purpose): `i4.html` (four iframes) ->
   `bodyKids=5`, `nonwhite` 2363 -> 9798, but **0 new Documents, 0 new Pages and no request for any child
   url anywhere in `port-trace.txt`**.
   * **Capability.** Adverts, embeds, widgets, comment systems, SSO and payment flows are iframes. This
     belongs beside the other gaps classified in [STUB-AUDIT.md](STUB-AUDIT.md) — it is the same class as
     the `StorageNamespaceProvider` no-op that cost weeks, except this one is visible to the eye. It is
     not a small fix: `createFrame` has to build a `LocalFrame` with its own
     `LoadingFrameLoaderClient`, and the subframe's navigation then has to be served by the port exactly
     as the main frame is (policy, loader, `DocumentWriter` feed), plus a decision about who owns
     subframe memory. Deferred: the MVP acceptance is "responds to touch without crashing", and this
     changes the memory picture of item **0j** rather than only adding a feature.
   * **Method.** The iframe experiment existed to test whether retained subframe documents explain
     dzen.ru's growth, and it produced a *negative that meant nothing*: "no documents are retained" was
     true only because no documents were ever created. A control page can be invalid for a reason
     unrelated to the hypothesis — the same trap `make-svg-test-pages.ps1` warns about for inline
     `<svg>`. Confirm the control exercises the mechanism before reading its result.
0m. **The acceptance criterion is now measurable, and its cost is 8.8 seconds of silence — measured
   2026-09-24 on `0.1.10.51`, no build needed.** `Src\tools\tap-test.ps1` (new) drives
   `LocalState\nav.txt`'s `taplinkstr:<text>` / `tap:<x>,<y>` into `TapFromScript` -> `HandleTapAt` ->
   `ForwardClickToEngine` — the identical function a finger enters — then reads the harness's own
   verdict and proves the app still answers with one more navigation. Three branches, three real sites,
   all successful: an HN logo self-link (`branch=nav-link`, `navEmpty=1`), an HN story where **the engine
   followed the anchor itself** (`branch=in-page`, `navEmpty=0`, landed on `item?id=…`), and a raw
   `tap:500,400` on `habr.com/ru/feed/` that navigated to the article and loaded it `rc=0`.
   * **`branch=in-page` with `navEmpty=0` is the GOOD path.** The gate makes the harness navigate only
     when the *engine* left the document URL alone; `in-page` there means the engine moved it itself. A
     report that reads the label as "the tap was swallowed" reads it as its opposite.
   * **The finding is the latency, and it is the user-visible half:** `simtap:` at `20:54:25.189`,
     `TapDone:` at `20:54:33.968` — **8.8 s** on a page with 113 resources pending, because `TapDone` is
     written when the tap's own `pumpLoop` (30 s watchdog, 160-tick cap, 0.8 s quiet threshold) finally
     ends, and the harness shows **nothing** during those seconds. On the Lumia it will be longer. This
     is what "responds to touch" has to be judged against, and it is a harness-side fix (feedback before
     the pump, not after), independent of item 0n.
   * **Still not demonstrated: a finger.** `nav.txt` bypasses XAML gesture recognition, and synthetic
     input does not reach the manipulation stack on this bench, so a defect inside that stack remains
     out of every script's reach. Do not report this item as "touch works".
   * Two traps for the next reader of a tap log: `[HIT]` markers live in `gpuinit-steps.txt` while
     `simtap:`/`TapDone:` live in `log.txt` (read one and you will conclude the click never reached the
     engine), and a verdict that has not arrived yet is not a verdict that failed — the first version of
     the tool waited 8 s and printed exactly that.
0n. **`habr.com/ru/feed/` intermittently never builds a body — a real, popular page that shows the user
   nothing. Mechanism named 2026-09-24; no fix applied yet.** Not slow, not blank-painting: `body=0`,
   `bodyKids=-1`, `rs=L/p1/ig0/pr0/st0/le0`, `sheets=0`, `scripts=3` of the 14 a successful load
   reports, `nonwhite=0/710656` — and the harness says so itself (`empty page: notice shown url=…
   requested=… bodyKids=-1`).
   * **The chain, from the preserved stall** (`LocalState\stall-arms\gpu1-c2\`): `loader: serve
     started=208 inflight=6 pending=65 hosts=1` — six transfers in flight and sixty-five queued, all on
     **one** host (`assets.habr.com`), which is exactly `PortPlatformStrategies.cpp`'s
     `maxRequestsInFlightPerHost = 6`. The port advances that queue in one place and one place only,
     `ResourceLoader::remove()`. The render-blocking `light-v2.css` sits at the head of the queue and is
     never dispatched: `analyze-loader-trace.ps1` reports `NEVER STARTED: 1 — light-v2.css`, and the
     diag's `res:` list carries it as `light-v2.css(s1)` — `s1` is `CachedResource::Status::Pending`,
     not a stage number — while every other sheet is `(s2)`, i.e. loaded. The diag also names what is
     waiting on that sheet: `scr=defer=0 err=0 blk=1[#2:inline/l-e-f0 ]`, an **inline parser-blocking
     script** that loaded nothing, failed nothing and *never fired*. WebCore confirms the sheet is
     outstanding in its own bookkeeping: `sheets=0` is `haveStylesheetsLoaded()` returning false, and
     that function is exactly `!styleScope().hasPendingSheets() || m_ignorePendingStylesheets`
     (`Document.cpp:10036`) with `ig=0` beside it — so a pending sheet is not an inference here, it is
     what the engine says. (Do not read `sheets=0` as a stall tell on its own: a healthy habr load
     reports it too, because sheets are pending normally while a page is loading.) So: six stalled
     transfers saturate the host → the render-blocking sheet queues behind them → the parser blocks on
     the sheet → no body. 71 of the 166 requests the engine asked for were never dispatched at all.
   * **It is not a slow fetch.** The harness wrote `[STAGE] after-load` **3.25 s** after `before-load`
     (`21:01:04.390` → `21:01:07.640`): the document arrived on time, and the white page is the *parser*
     stopping afterwards.
   * **The previous candidate mechanism was falsified, and the falsification is the reusable part.**
     This item used to blame `pumpLoop` exiting with work outstanding. Counting loader activity after
     every `loader: MARK pumpLoop done` in `port-trace.txt`: in *every* session the loader keeps starting
     and retiring afterwards — the stall session shows **344 starts and 347 retirements after the pump
     exited**, because the engine then proceeds into `pump: timers armed, entering RunLoop::run()`. The
     pump is not the gate. An earlier reading of `gen=2 started=56 inflight=7 pending=56` as "the stall's
     pending set" was the same error in miniature: **`port-trace.txt` accumulates across runs**, so a
     count taken over the whole file mixes residue from force-killed sessions; the session boundary is
     the last drop in `started=`. The tool that reported "35 loads started and never retired" reads the
     whole file and needs session scoping before any of its counts are quoted again.
   * **The GPU correlation is dead, and how it died is worth keeping.** Four runs with the probe
     disarmed, four with it armed, arms *not* interleaved. `Src\tools\stall-arms.ps1` alternates them
     run-to-run and reads the arm back **from the log** (`gpu-presented` / `compositing-on-no-present` /
     `software`), never from what it wrote to `settings.ini`. Result: `gpu0-c1` process died at startup,
     `gpu1-c1` rendered (`gpu-presented`), `gpu0-c2` rendered (`compositing-on-no-present`), `gpu1-c2`
     **STALL** — read back as `software`. The stall landed in the software arm while the GPU arm
     rendered. **`gpudefault=` is a seeding default that the runtime probe overrides per document URL,
     not a switch** (see `Doc/GPU-LIVENESS.md` and the build notes in `CLAUDE.md`), so the old tally
     compared labels a run may never have engaged.
   * **It is not a 60-second white page.** The chain above implies a bounded wait: upstream's curl sets
     `CURLOPT_LOW_SPEED_LIMIT 1` / `CURLOPT_LOW_SPEED_TIME <defaultTimeoutInterval>`, default 60 s
     (`CurlContext.h:155`), so six stalled transfers should die at 60 s and free the queue. Six
     `Src\tools\stall-watch.ps1 -Seconds 110` attempts: four rendered, **two stalled and outlived the
     entire 110 s window**. The 35 s window the original verdict was taken in sits *inside* that
     timeout — which is why `stall-watch.ps1` reports a time series instead of a boolean.
   * **The double-queue hypothesis is closed for this host; the real question is open.** The port caps
     at 6 per host and curl's own `CurlDefaultMaxHostConnections` is also 6 (`CurlContext.h:55`, applied
     at `CurlRequestScheduler.cpp:218-220`), so the six the port hands over *are* the six curl runs —
     none is parked inside curl's scheduler where no libcurl timeout would apply. **Why six transfers
     carrying a 60 s low-speed timeout do not die at 60 s is not answered.** The direct reading is now
     built into `stall-watch.ps1`: at the end of its window it prints the last `loader: serve` line and
     the last retirement from the trace tail, so a stalled run states whether `started` ever advanced or
     `inflight` ever dropped. **A frozen counter past 60 s means curl is not the thing that is stuck**,
     and then the fix must be port-side regardless of the cause.
   * **Tools written for this, all in `Src\tools\`:** `stall-arms.ps1` (alternate the GPU arm run-to-run
     on one URL, read the arm back from the log, preserve evidence per run), `stall-watch.ps1` (sample
     the diag every `-StepSec`, report the age of the reading the verdict rests on, preserve log +
     trace tail on a stall), and `tap-test.ps1` (item 0m).
0o. **CLOSED 2026-09-24 (`.55`): the intermittent engine-thread AV at startup. Mechanism symbolised,
   reproduced on demand, fixed, and verified over 9 armed launches.** This item previously read "once in
   ~60 preserved logs, not symbolised"; it was in fact **once in roughly three launches out of ten**, and
   it was a regression we introduced the same day.

   **The chain.** `0.1.10.51` wired the UWP memory events to the engine, which made `WebCoreReleaseMemory`
   reachable for the first time (`Summary.md` 0b: it had no caller at all while a port comment claimed the
   harness called it). `AppMemoryUsageIncreased` fires **at handler registration** on some launches — the
   platform reports a fresh process's first reading as an upward crossing into `low`, the *lowest* level —
   and the handler posted a `mem-release` job. That job then became the engine thread's **first WebCore
   call of the process**, ahead of any navigation and ahead of process init:

   ```
   21:30:59.368  mem-level UP: mem 35216/12497952 KB level=low   <- handler posts; UI thread 11076
   21:30:59.430  WebEngine: loop ready, waiting for jobs          <- 62 ms LATER
   21:30:59.433  VEH: code=0xC0000005 tid=8312 faultaddr=0x8      <- the ENGINE thread dies
   ```

   `tid=8312` ≠ `11076`, so the fault is inside the job, not the handler. The next process's verdict line
   states it independently: `CRASHED … finished=1 job=mem-release tickstep=0 stage=(none)` — exactly one
   job completed and the engine was inside `mem-release`. Symbolised frames:
   `JSC+da0ab0` = `WTF::MemoryPressureHandler::MemoryPressureHandler()` (`MemoryPressureHandler.cpp:75`),
   `JSC+da0a02` = `isUnderMemoryPressure()` (`MemoryPressureHandler.h:125`),
   `WebCore+199fad2` = `WebCore::releaseCriticalMemory()` (`MemoryRelease.cpp:122`),
   `Harness+b805e` = the job lambda. The Windows constructor's member initialiser is
   `m_windowsMeasurementTimer(RunLoop::mainSingleton(), …)`, and `RunLoop::mainSingleton()` is
   `{ ASSERT(s_mainRunLoop); return *s_mainRunLoop; }` with the **ASSERT compiled out in release** — so a
   still-null `s_mainRunLoop` yields a null reference and the `Ref<RunLoop>` refcount increment faults at
   offset 8, the logged `faultaddr`. `s_mainRunLoop` is set by `WTF::initializeMainThread()` inside
   `ensureWebCoreInitialized()`.
   * **One-line statement:** `WebCoreReleaseMemory` was the only ABI entry point touching WebCore without
     first initialising the process; the other four (`WebCoreRenderHtml`, `WebCoreLoadUrl`,
     `WebCoreSessionLoad`, `WebCoreGpuInit`) all open with `ensureWebCoreInitialized()`.
   * **The fix is two halves and neither substitutes for the other.** (a) `Src\port\WebCoreDriver.cpp`:
     `WebCoreReleaseMemory` calls `ensureWebCoreInitialized()` first, with a `[SL] releaseMemory: WebCore
     initialised` marker — this removes the hazard for *any* release job that precedes init, including a
     genuine `medium` event raised before the loop is ready. (b) `Src\harness\MainPage.xaml.cpp`: the
     handler releases only at `Medium` and above (`MemoryLevelIsPressure()`) and logs **both** decisions
     (`mem-release: posting` / `mem-release: skipped -- level below medium …`) — the correct semantics,
     since `low` is the platform's resting state and releasing there buys re-decodes the Lumia cannot
     spare, the same argument the `LimitChanging` handler already made for ignoring a ceiling *rise*.
   * **Verification, and its ceiling.** The platform fires the startup event only sometimes — 3 launches
     in ~10 in the dying series, then **20 consecutive launches with no event at all** — so a green sweep
     proves nothing by itself. `LocalState\mem-release-race.txt` therefore makes the harness post the job
     at handler registration, **45 ms before `loop ready`**, the exact position the dying launches had,
     and `Src\tools\startup-soak.ps1` reports per launch whether that arm fired, refusing to let a sweep
     that never armed the trigger be read as evidence. **9 armed launches: 9/9 alive, zero `0xC0000005`**,
     with the driver's markers showing the job first and the init inside it
     (`1 SL: releaseMemory (memory-pressure event)` → `2 PS: install has=0 didInstall=1` →
     `4 [SL] releaseMemory: WebCore initialised` → `5 [GPU] enter`). 20 unarmed launches also survived,
     which says nothing about the race. **Not proved:** the platform's own event has not been observed
     firing after the fix — only the hand-armed equivalent, same job through the same queue.
   * **The two other unhandled UEFs** in the preserved corpus remain unsymbolised, both on `0.1.9.x` at
     different addresses, and are **not** this defect. If one recurs, the recipe is unchanged:
     `llvm-symbolizer --obj=<the DLL>` (pass the **DLL**, not the PDB — the PDB alone is rejected as "not
     a valid object file"), falling back to `.pdata` from `llvm-readobj --unwind` plus `llvm-pdbutil dump
     -l` for file:line, minding that line-table ranges are section-relative while `--unwind`'s are not
     ([CALC-HANDLE-DANGLING.md](CALC-HANDLE-DANGLING.md) §3).
   * **Method warning, paid for three times in one new tool.** `startup-soak.ps1` produced three wrong
     counts, all the same shape — a counter that does not measure what its label says: it counted
     `mem-release`, which matches its own probe's line `mem-release-race: checked, armed=0`, so it
     reported a release on every launch of a build where the event never fired; then the arming field was
     not copied into the per-run object, so the summary read `$null` while the per-run column read `1`;
     then the summary kept the old field name and read `0/6` beside a column reading `1/1`. What caught
     all three was that **two displays of the same run disagreed** — worth building in deliberately.
0p. **The fix for item 0n, designed from the code rather than from the cause. Implemented, compiled and
   linked into `.52`+ (`PortPlatformStrategies.cpp`, clean compile for x64), NOT yet measured.** The whole
   defect reduces to one property the scheduler lacks: **the port cannot make progress on its own.**
   `serveHost` stops the moment a host is at its cap
   (`PortPlatformStrategies.cpp:486`), the cap frees only in `LoaderStrategy::remove()`, and `remove()`
   runs only when a transfer reaches a terminal state. If six transfers never reach one, the host
   serves nothing again — not the render-blocking stylesheet, not anything — and no amount of priority
   helps, because **priority is only a queue order and an order is worthless when nothing is dequeued**.
   That is why this fix does not need the unanswered question in 0n to be answered first; it removes the
   dependency instead.
   * **Layer 1 — a critical resource may exceed the cap, by a bounded margin.** Measured in
     `DefaultResourceLoadPriority.cpp:30`: `MainResource` is `VeryHigh`, `CSSStyleSheet` / `JSON` /
     `Script` are `High`, fonts and media are `Medium`, and `ImageResource` is `Low`. On habr the six
     stuck transfers are images and the queued sixty-five contain the render-blocking sheet — so the
     congestion is `Low` and the thing being starved is `High`. `HostState::atLimit()` therefore lets a
     `High`/`VeryHigh` request start while `m_loading.size() < maxRequestsInFlightPerHost + 2`; the
     `VeryLow` idle-only rule and the cap for everything below `High` are untouched, so the
     anti-congestion property that motivated the cap in the first place (the 724-image page) still holds
     for the bulk.
   * **Layer 2 — a saturated host cannot stall forever, whatever the cause.** A per-loader start time
     (`MonotonicTime`) and a per-host *last retirement* time are kept; if a host has been at its cap with
     pending work and **not one** retirement for `kSaturatedHostCeiling` (20 s), the longest-in-flight
     request is cancelled — `ResourceLoader::cancel()` — which reaches `finishNetworkLoad()` →
     `loaderStrategy()->remove(this)` and frees the slot. This is the architectural half: it makes the
     port's progress independent of any transfer reaching a terminal state, and it mirrors the
     progress-aware stall detector that already works in `WebCoreSessionLoad`
     ([TOPLEVEL-FETCH-BUDGET.md](TOPLEVEL-FETCH-BUDGET.md)). It fires only on that exact triple, and it
     logs `loader: STALL-CEILING …` — **a ceiling that fires must never be silent**, the same rule the
     GPU probe's skipped-probe line follows, because silence is what hid that defect for a day.
   * **The wake-up — MEASURED WORKING 2026-09-24 on `.55`.** A stalled page schedules nothing by itself,
     so the check cannot live only in `serve()`: `RunLoop::mainSingleton().dispatchAfter()` re-arms it
     while any host is saturated, and stops re-arming once none is. Cancellation happens at the **top** of
     `serve()`, before `serveHost()` walks anything, so the re-entrant `remove()` → `scheduleServe()` it
     triggers cannot mutate a container being iterated.
     **The open question was whether such a timer fires at all here.** `RunLoop::mainSingleton()` is
     whatever thread first reached `WTF::initializeMainThread()` — here the engine thread, whose run loop
     is pumped from *inside a load job* (`WebCoreDriver.cpp:685`, `RunLoop::run()` in `pumpLoop`); the
     existing working precedent in the same file (`scheduleServe()`'s `mainSingleton().dispatch(...)`)
     works for that reason. The stall is precisely the case where the load job has **ended**
     (`[STAGE] after-load` arrives 3.25 s in while the queue is still stuck), so a timer armed on that
     loop could plausibly never fire. It was therefore instrumented rather than argued, and the
     instrument answered: on a real `dzen.ru` load (26 peaks in flight, 15 queued, 9 hosts) the marker
     channel shows **`loader: STALL-WATCH armed in 5s` → `loader: STALL-WATCH fired`**, twice, and **no
     `loader: STALL-CEILING`** — the timer fires, and it does not fire spuriously on a healthy heavy page.
     Both lines go through `portLoaderTraceBoth` (`gpuinit-steps.txt`), per the rule that a new loader
     observation goes to the channel that has survived every crash here.
     **What is still unproven is the rescue, not the wake-up:** no run has yet produced the triple
     (host at cap + pending work + 20 s with no retirement), because the page that does it —
     `habr.com/ru/feed/` — became **unreachable from the bench** during this session (`curl`
     `time-connect` never completes; the port reports `type=Curl curlcode=28 desc=Connection timed out
     after 3006 milliseconds`, which is the port's *own* deliberate 3 s `CURLOPT_CONNECTTIMEOUT`, not a
     new defect — do not raise that budget, per the standing rule). `example.com` (0.12 s) and
     `news.ycombinator.com` (0.25 s) answered normally at the same moment, so this is the host, not the
     network. Until it returns, the reproduer must be another page that queues >6 same-host subresources
     behind a render-blocking sheet; `dzen.ru`'s second load reaches 15 queued and is the closest seen.
   * **Layer 1 costs something Layer 2 does not, and it is not yet measured away.** Letting two `High`
     requests past the cap takes the host to 8 concurrent transfers while curl's own
     `CurlDefaultMaxHostConnections` is **6** — i.e. two of them queue *inside* curl, where the port's
     accounting cannot see them. That trades one unbounded queue for a smaller opaque one, so Layer 1 is
     the half to be suspicious of on measurement; Layer 2 is the half that makes the port independent of
     curl's behaviour. Both are implemented together on purpose, to be separated by measurement rather
     than by argument.
   * **Symbol availability was checked, not assumed.** `ResourceLoader::cancel()` is `WEBCORE_EXPORT`ed
     in `WebCore.lib`, and **WTF's `RunLoop::dispatchAfter`, `RunLoop::mainSingleton` and
     `MonotonicTime::now` are in `JavaScriptCore.lib`, not `WebCore.lib`** — the port links both
     (`link-driver-gpu-x64.ps1:48-49`), which is why the first lookup found nothing. Nothing here needs
     an upstream edit, so **no WebCore rebuild** is implied — the constraint that a header edit costs
     362/362 unified sources.
   * **What it does not fix.** Six transfers holding slots for 20 s each is still six transfers doing
     nothing for 20 s; on the Lumia this ceiling will be felt as a long stall that then *ends* instead
     of a blank window that does not. The cause of the non-retirement is still open (item 0n) and
     `stall-watch.ps1`'s loader line is the instrument that will name it.
0q. **The error page had no way out of it. FIXED in `.56` and verified end to end; the architectural
   half (a dead bitmap over a live-but-unrelated session) is still open.** Found 2026-09-24 by asking
   what the user can *do* with the page the port shows when a site is unreachable — a question the
   earlier pass over the same code did not ask, and a premise it got wrong.
   * **First, a correction to a claim made the same evening.** "A failed navigation leaves the previous
     page on screen and no error page appears" was **wrong**. It was inferred from two absences
     (`title=Hacker News` on the `nav: FAIL` line, and no `empty page:` notice) and both are explained
     otherwise: the title is read and printed **before** the error page is rendered, and the notice is
     about a different condition (`rc=0` **and** nothing painted, which `emptyPage` computed from the
     *previous* document correctly reported as false). `WebCoreRenderHtml(MakeErrorHtml(...))` returns
     **0**, so `ok = (rc == 0)` is true and the error page is blitted like any successful load.
     Measured on `.56`: `error page: rendered url=https://habr.com/ru/feed/ rc=0 blit=yes`. The
     misleading step was reading a *field* (`title=`) instead of the *outcome* (`WE-job:post-try rc=0`).
   * **The real defect: it was completely inert.** On the failure path the harness sets
     `sessionActive = false`, so `m_sessionActive` goes false; `HandleTapAt` then takes its
     link-table branch instead of `ForwardClickToEngine`, and every scroll/pinch handler
     early-returns. And `MakeErrorHtml` contained no `<a href>` at all — while `extractLinks`
     publishes exactly those, and only those whose protocol is in the HTTP family — so the table was
     **empty**. Net effect on the device: a full-screen "Could not load the page" that answers no tap
     and no swipe, with the address bar as the only exit. On a phone that is indistinguishable from
     "the browser died", which is the MVP criterion failing.
   * **The fix.** A "Try again" anchor pointing at the **failed** url (not the current document's — the
     engine is still on the previous page, so retrying "where we are" would reload what was already on
     screen), block-shaped with real padding so `boundingClientRect()` has a non-zero box and
     `extractLinks` does not skip it; and `HtmlEscape` on both interpolated strings — `err` embeds the
     requested url, and a query string with two parameters was being parsed as an entity reference, so
     the page misrendered its own explanation. Plus one log line, because the render result had never
     been recorded and `WE-job:post-try rc=0` is written by the success path too.
   * **Verified on `.56`.** `simtap: taplinkstr -> link #0 rect=(24,308 158x65)
     dip=(103.0,340.5) url=https://habr.com/ru/feed/` → `OnPageTapped: … session=0 links=1 sim=1` →
     `NavigateTo: url=https://habr.com/ru/feed/`, i.e. the tap works through the same dispatcher a
     finger uses. The escaping round-trips: with `?a=1&b=2&c=3` the extracted href is the literal
     original url. Process stayed alive over four cycles, no `VEH:`, no `WEDGE`.
   * **What is still open — the architectural half.** The error page is drawn through
     `WebCoreRenderHtml`, which builds a **throwaway `Page`** (scripting and compositing disabled) and
     returns pixels; the resident session keeps the previous document. So `m_sessionActive` is false and
     `diag:` / `GetTitle` / `GetFrameHash` keep describing a page that is **not on screen**, and the
     only interaction the error page can offer is an anchor, because that is the only affordance the
     non-session tap path knows. The same design carries `about:home`, which works, so this is
     consistent rather than new — but a proper fix is one document and one session:
     feed the error HTML into the **resident** session (a `WebCoreSessionRenderHtml`-shaped export,
     meaning the C ABI changes in **both** headers) so the page is live, scrollable and tappable like
     any other. Not done here: it is an ABI addition, and `.56` already removes the dead end.
0r. **REFUTED on 2026-09-24 by the `.60` instrument: `lenta.ru` *does* reach `readyState = Complete`.**
   The claim below ("ends with `readyState = I` forever while the loader is fully drained") was an
   artifact of reading a **cached diagnostic string** five times and calling the result "frozen, not
   stale" — see the measurement bullet at the end of this item, and the new item **0s** for the
   diagnostic defect that produced it. What survives is a **slowness** finding, not a liveness one.
   The item is kept in full because the eliminations are still valid and still enumerated what the
   load event does *not* wait on. The original framing follows.
   This is the honest replacement for 0n/0p's missing reproduer: `habr.com`
   stayed unreachable from the bench all evening, so the reproduer was **found by measurement instead of
   by reputation** — and the result is better than habr ever was, because this page answers.
   * **The instrument that found it: `Src\tools\queue-shape.ps1`.** It ranks a list of URLs by how
     closely their loader queue matches the 0n shape, reading the port's own `loader: serve
     started=… inflight=… pending=… hosts=…` lines out of `gpuinit-steps.txt` (the channel that has
     survived every crash here), scoped per navigation by file offset. Measured on a live session:
     `lenta.ru` 67 lines with **one** host at its cap and work queued behind it, its peak queue **82**;
     `rbc.ru` **1** such line; `dzen.ru` **0**.
   * **And it caught a mistake in its own first version, which is the part worth keeping.** The first
     cut reported `peakInflight >= 6` and `minHosts` as separate columns and told the reader to combine
     them — i.e. it took one number from one `serve` line and another from a *different* line. That is
     the "two numbers from two observations" trap this project keeps paying for, so the tool now counts
     the triple **on a single line**: `hosts=1 && inflight>=6 && pending>0`. The gap it exposes is the
     size of the error: on `rbc.ru` the loose reading fires on **74** lines and the honest one on **1**.
   * **`lenta.ru` does not reproduce 0n** — `body=1` from the first sample, `nonwhite=1428157/2842624`,
     the page is laid out and visible. What it reproduces is the *other* half, and it is live: five
     post-load samples 5 s apart (`latediag[1/5]`…`[5/5]`, 22:15:34→22:15:54) all read
     `pending=19 rs=I/p0/ig0/pr0/st0/le0` — **frozen**, not a stale snapshot — while
     `stall-watch.ps1` over 120 s shows the loader at `inflight=0 pending=0 hosts=0`. The port has
     retired everything; WebCore has not noticed.
   * **`analyze-loader-trace.ps1` names the layer:** `starts / finishes : 121 / 126`, `STILL OPEN: 0`,
     `every Pending resource was started at least once`, and its verdict is *"the scheduler is
     exonerated — every request started and retired, and nothing WebCore waits for was skipped. The loss
     is inside the notification between a finished `CachedResource` and the `PendingScript` / parser
     that waits on it."* So this is **not** the loader, **not** the per-host cap, and **not** a stalled
     transfer: the transfer finished and the completion did not reach the waiter.
   * **Why it matters for the MVP:** `readyState` never reaching Complete means **`load` never fires**,
     so every `window.onload` handler on the page never runs — the same class of user-visible failure
     as 0k, arrived at from the other side. The parser is not blocked (`blk=4` all `f1`, `sheets=1`
     means `haveStylesheetsLoaded()` is true), and the deferred bundles did execute (`l1e0f1`).
   * **A hypothesis to test, not a finding.** In the visible (truncated) `res:` list the `Pending`
     entries are a *family* — `owl_article_280_*`, `owl_photo_280_*` — while their 250-wide siblings
     are all `(s2)`. That is consistent with one shared client/request per family whose completion is
     dropped, and it is also consistent with those simply being the last ones in flight. **Not
     distinguished.** The `res:` list is truncated by the 4096-byte diag buffer, so the full set cannot
     be read from `log.txt` as it stands — a first step is to make `WebCoreGetDiag` report the count of
     `Pending` entries and the first N, or to raise the harness buffer.
   * **That first step is done, in `.57`, and it answered the question — by refuting the family
     hypothesis.** Two silent cuts were removed. The port's `res:` loop used to `break` when its buffer
     filled and then **close the bracket anyway**, so a cut list was indistinguishable from a complete
     one (the same class as every other diagnostic lie here); it now builds the whole list first and, if
     anything did not fit, writes `+N more]` *inside* the brackets. `g_lastDiag` went 4096 → 8192, and
     on the harness side `LogWriteF`'s stack buffer went 2048 → 8192 (it, not the port, was the binding
     cut on `latediag`) along with the two 4096 diag buffers. Measured effect: the `latediag` line is now
     **5472 bytes** against a 2048 ceiling, **112 entries** visible, and **no `+N more]`** — the whole
     list fits.
   * **The sharpened answer, and why the family story is wrong.** `pending=22`, `rs=I/p0/ig0/pr0/st0/le0`,
     `s2: 85 / s1: 22 / s3: 5`. **All 22 `Pending` entries are images** — 13 `owl_article_280_*`,
     2 `owl_photo_280_*`, 7 `preview_*.jpg`. Not one script, not one stylesheet. And at the **family**
     level `owl_article_` is s1=13 / s2=39 while `preview_` is s1=7 / s2=13, so this is not "one family
     whose completion is dropped": the same *kind* of resource completes normally 85 times and hangs 22
     times. (Per-hash, not per-family: an individual URL appearing on both sides is not what the data
     shows.) The count also **varies between runs** (19, then 22), which says timing, not identity.
   * **Consequence for the waiter.** No script and no sheet is `Pending`, and `sheets=1` /
     `blk=4[…]` all `f1` already said the parser is not blocked — so the thing that never completes is
     the **`load` event, waiting on those images** (`le0`). `analyze-loader-trace.ps1`'s verdict sentence
     names `PendingScript` generically; for this page the waiter is `CachedImage` / the load event, and
     its wording should not be read as having found a script.
   * **`.58` exonerates the network layer, and `.59` exonerates the engine's receipt of it.** Both were
     bought with the same shared channel (`ApoLoaderTrace.h`, below). `.58` added `RH:` markers to
     `platform/network/ResourceHandle.cpp`: all 32 then all 21 `Pending` leaves traced the complete
     `RH: start` / `RH: resp` / `RH: done`, with **zero** `RH: CANCEL` and **zero**
     `RH: DETACH-BEFORE-TERMINAL` — so the terminal callback really was delivered to the
     `ResourceHandleClient`. `.59` added `SL:` markers to `loader/SubresourceLoader.cpp`: all 21 traced
     `RH:start > RH:resp > SL:resp > RH:done > SL:finish`, with **zero** `SL: DROPPED`, **zero**
     `SL: willCancel` and **zero** `SL: http-status-error`. `SL: finish` is written immediately after
     `resource->finish()`, which does `if (!errorOccurred()) setStatus(Cached)`, so the completion
     reached the `CachedResource` and the status was set — and the diag still read `(s1)`.
   * **Two holes in that reading, both stated here because both were mine.** (1) `SL: finish` is written
     *before* `notifyDone()`, and `notifyDone()` is where the `RequestCountTracker` is released
     (`m_requestCountTracker = std::nullopt`) — so everything traced above can be green while the
     counter that actually gates the load event is still held. (2) `rh-trace.log` accumulates across the
     whole session while `latediag` belongs to one document, so pairing a session-wide trace against one
     document's resource map was not airtight. Neither is a reason to distrust the layer exonerations;
     both are reasons not to have concluded from them alone.
   * **`.60` moves the boundary out of the loader entirely, and this is the current state.** Reading
     `FrameLoader::checkCompleted()` (`loader/FrameLoader.cpp:993`) rather than bisecting it: it is the
     **only** place that executes `document->setReadyState(Document::ReadyState::Complete)` (line 1035),
     and five early returns sit above it. The third is
     `if (document->cachedResourceLoader().requestCount()) return;` — and **`requestCount()` is not the
     harness diag's `pending=`.** `countPendingResources` (`WebCoreDriver.cpp:486`) counts
     `CachedResource::Status::Unknown|Pending` over `allCachedResources()`; `requestCount()` is
     `CachedResourceLoader::m_requestCount`, an `int` incremented by `SubresourceLoader`'s
     `RequestCountTracker` constructor and decremented by its destructor. **Two different counters, and
     every measurement of this defect before `.60` read the first while the `load` event waits on the
     second** — so a page can read `pending=0` and still be blocked there forever, and the two numbers
     were never evidence about each other. `.60` logs all five gates together on every blocked attempt
     (`FL: BLOCKED parse= req= delay= sheets= kids=`), the `FL: PASSED` line that means "readyState is
     about to become Complete", and `RC: +N`/`RC: -N` on both counter functions so the tracker that does
     not come back is named rather than counted.
   * **`.60` measured, and the measurement refutes the premise of this whole item.** The trace (913
     lines, cleared immediately before the navigation, so it holds exactly one document):
     - `FL:` walk: index **0** `FL: PASSED` (a spurious `checkCompleted` on the still-resident home
       page right after the delete), then blocked attempts climbing to `req=230` and descending
       **monotonically** to `req=2`, ending index **907** `FL: PASSED -- readyState -> Complete`.
     - `RC:` balance: **113 distinct leaves, +113 / −113, 0 unbalanced, final `RC: -0`.** No leak, no
       double-decrement, never negative. **All three candidate mechanisms in the bullet below are dead.**
     - Every one of the **38** resources the diag reads as `(s1)` has a complete history
       `RC:+ → RH:start → RH:resp → SL:resp → RH:done → SL:finish → RC:-` (e.g.
       `owl_article_280_75ef… RC:+117[97] RH:start[877] RH:resp[894] SL:resp[895] RH:done[904] SL:finish[905] RC:-0[906]`).
       **They were in flight, not lost** — and note *when* they started: 573 trace lines after their
       loaders were created, which is the real finding (below).
     - `RH: fail x9` is **not** a port defect: the nine are exactly the nine
       `NAV: FAIL … code=6 domain=CurlErrorDomain` lines, and `6` is `CURLE_COULDNT_RESOLVE_HOST` —
       `ssp.rambler.ru`, `mc.webvisor.org`, `top-fwz1.mail.ru`, `st.top100.ru`, `counter.yadro.ru`,
       `www.tns-counter.ru`, `visor.sberbank.ru`. Ad/tracker hosts that do not resolve from this bench.
     - Confirmed independently of the trace, by the port's own **live** JS readback on the next tap:
       `[HIT] js rc=0 rs=complete …` — the engine reporting `document.readyState === 'complete'`.
     - **The `latediag[1..5]` "frozen" reading was the artifact.** All five samples (23:22:37 →
       23:22:57) carry a **byte-identical** embedded diag (`s1=38 s2=69 s3=5`, `rs=I`,
       `nonwhite=1429147`, `contents=2560x15748`) while their `mem=` prefix is **live** and different in
       every sample. Five identical samples were **one observation read five times**. `g_lastDiag` is a
       cached 8192-byte string written only by `writeDiag` (`WebCoreDriver.cpp:1209`), and every
       `writeDiag` call site is a *software* path — `buildSession`, `finishInteractionPaint`,
       `WebCoreLoadUrl`, `WebCoreClickAt`, `WebCoreSetPageScale`, `WebCoreSessionPaint`. `gpuPresent`
       (`:887`) does not call it, so once the harness went to **direct present** (`[GPU] direct present
       -> 1`) nothing refreshed the string for the rest of the session. CLAUDE.md documents the same
       mechanism for `nonwhite`; it applies to the *whole* diag line, which is why `rs=I` was still
       being reported 2 minutes after `readyState` became Complete. → new item **0s**.
   * **What is actually wrong: the front page needs ~125 s from navigation to the load event.** The
     top-level fetch itself is fine (`before-load 23:22:28.211` → `after-load 23:22:31.949`, ~3.7 s);
     the trace's last write is **23:24:33**. In between, 113 subresources are created **up front**
     (`RC` peaks at **230**, so loaders are not deferred at creation) and the port serves them ~6 at a
     time, while the engine thread is also saturated by paint jobs (`beat-stuck … job=live-tick`). The
     user-visible consequence is the same as the false 0r: a page that renders immediately but whose
     handlers do not run for two minutes. **This belongs with 0m (tap latency ~8.8 s), not with the
     AV/loader family** — same cause shape (engine-thread serialisation), three orders of magnitude
     apart in what it costs.
   * **The three candidate mechanisms, kept because all three are now refuted by measurement** — one
     entry per column of the `.60` `RC:` balance, so the next reader need not re-derive them: a
     `notifyDone()` that returns early (`reachedTerminalState()`, or `documentLoader()` null — which
     writes only a `RELEASE_LOG` that sinks to `OutputDebugStringA` and is **invisible** on this port),
     a double-decrement that leaves `m_requestCount` negative (the call site tests non-zero, so negative
     would block just as hard — and `ASSERT(m_requestCount > -1)` is compiled out in this release
     configuration), or a `CachedResource` whose `load()` ran twice so two trackers exist for one
     completion. `.60` shows 113 `+` against 113 `−` with a final value of exactly 0 and **zero**
     `NEGATIVE` markers, which is the signature of *none* of the three. One caution about reading those
     lines: the sign in `RC: +N` / `RC: -N` is a **literal in the format string** naming the direction,
     not the value's sign — `RC: -228` means "decrement, counter is now 228", and a `min=-228` computed
     by parsing that field as an integer is a bug in the parser, not a negative counter. `loader: CALL
     type=N main=M` prints `CachedResource::Type`, where on this tree
     **`ImageResource` is 1**, not 2 (`MainResource` 0, `JSON` 2, `CSSStyleSheet` 3, `Script` 4,
     `FontResource` 5, `SVGFontResource` 6, `MediaResource` 7, then `EnvironmentMapResource` and
     `ModelResource` because `ENABLE(MODEL_ELEMENT)` is on, so `RawResource` is 10).
   * **The channel these three builds bought, and it is reusable: `ApoLoaderTrace.h`**
     (`WebKit\Source\WebCore\platform\network\`). The loader's interesting half lives in **WebCore.dll**,
     and neither existing sink can be called from there — `WebCorePort::portDiagLog` is defined in
     `WebCoreDriver.cpp` and linked into **Harness.exe** (an unresolved external, not a diagnostic), and
     `WTFLogAlways` sinks to `OutputDebugStringA` (`WTF/wtf/Assertions.cpp`) and never reaches a file.
     The header is a shared inline sink, armed by a **file** (`rh-trace.txt` beside
     `APOTHEOSIS_GLYPH_LOG`) writing `rh-trace.log`, because the bench activates the app through
     `shell:AppsFolder\…` and it never sees the PowerShell environment — the same rule as `nav.txt`,
     `jstack.txt` and `texttrace.txt`. It computes the leaf name exactly as the port's `loader: +` line
     does, so the two traces pair by string. Nothing else includes it, so it dirties only the unified
     sources that use it — unlike a widely-included WebCore header, measured on 2026-09-18 to cost all
     362 of them. One trap: `rh-trace.log` is **not** in `update-local-x64.ps1`'s move-to-`prelaunch-*`
     list, so it accumulates across navigations — delete it before a run.
   * **Reproducer, restated:** `https://lenta.ru/`, on demand, on a page that is otherwise fine. It
     reproduces **a ~125 s load**, not a hang — `readyState` does reach `Complete`. `habr.com` is no
     longer required for this half of the work — and note that `habr.com`'s own
     `curlcode=28 desc=Connection timed out after 3006 milliseconds` is the port's deliberate 3 s
     `CURLOPT_CONNECTTIMEOUT`, not a defect, and **must not be raised**.
0s. **The `diag:` string is a cached snapshot and the GPU present path never refreshes it, so on the
   device the engine reports a state from minutes ago. FOUND 2026-09-24, design decided, not yet
   implemented.** This is the diagnostic defect that produced the false 0r — and it is worse on the
   target than on the bench, because the device runs `gpudefault=1`, i.e. exactly the configuration
   in which nothing refreshes the string.
   * **The mechanism.** `g_lastDiag` (`Src\port\WebCoreDriver.cpp:356`, 8192 B) is written **only** by
     `writeDiag` (`:1209`) and read **only** by `WebCoreGetDiag` (`:2979`), which just copies it. Every
     `writeDiag` call site is on a *software* path: `buildSession` (`:2241`), `finishInteractionPaint`
     (`:2892`), `WebCoreLoadUrl` (`:3458`), `WebCoreClickAt` (`:4288`), `WebCoreSetPageScale` (`:4540`),
     `WebCoreSessionPaint` (`:5157`). `gpuPresent` (`:887`) does not call it. So under
     `[GPU] direct present -> 1` the diag freezes at the last non-paint write — measured on `lenta.ru`
     at the post-resize sample, i.e. ~4 s after the load began, still reading `rs=I` **2 minutes after
     `readyState` became Complete**. CLAUDE.md already documents this for `nonwhite`; it applies to the
     whole line, and the line is the only view the device gives.
   * **Why the obvious fix is a trap.** `writeDiag(document, view, w, h, nonWhite)` takes `nonWhite`,
     and on the present path there is **no source for it**: direct present writes no RGBA buffer, so
     there is nothing to count. It is computed locally by every caller (the software paint at `:1088`,
     the readback at `:860`) and stored nowhere. Passing a fabricated `0` would insert a new lie into
     the one field the harness parses — `DiagLooksEmpty` (`MainPage.xaml.cpp`) reads
     `body=`/`bodyKids=`/`nonwhite=` — and that is the `SILENT LIE` class this project has paid for
     three times (`Doc/STUB-AUDIT.md`). **A fresh value must never be fabricated to make a diagnostic
     look live.**
   * **The design, as implemented in `.61`, in three parts.** (a) **Carry it**: `static int g_lastNonWhite
     = -1` (−1 = never measured), set **inside `writeDiag` itself** — the one place that ever receives a
     measured count — so no future caller has to remember to update it. If it is still −1 the refresh is
     **skipped and logged once** (`[GPU] diag refresh skipped: …`) rather than passed a fabricated zero:
     unreachable in practice, since the load path writes a diag before the GPU can present, and reported
     anyway because "the refresh quietly did nothing" is the failure mode this item removes. (b) **Label
     the carrier**: append ` gpu=1` when the refresh comes from the present path, so a *carried*
     `nonwhite` is distinguishable by the field's **presence** from a live one, without changing that
     field's grammar — `DiagLooksEmpty` keeps working. (c) **Throttle**: the present path runs per frame
     and `writeDiag` walks every cached resource (113 leaves on `lenta.ru`), so refresh at most every
     ~200 ms; the first present always refreshes, because the timer starts unset. A diag that is 200 ms
     old is honest; one that is two minutes old is not.
   * **Do not "fix" this by making `WebCoreGetDiag` format on demand.** It is called on the **UI
     thread**, and it may not touch the engine (CLAUDE.md's iron rule; the cached string exists for that
     reason). The refresh has to happen on the engine thread, at the moment the engine is already
     painting. A related pre-existing race stays out of scope: `WebCoreGetDiag` `snprintf`s from a buffer
     the engine may be writing, so a torn read is possible — worth a lock only if it is ever observed to
     matter, and it is *not* what produced the false 0r.
1. **Continue crash verification** — run navseq repeatedly; if `0x204901f` reproduces, capture
   dump with cdb-attach (`sxi bpe` + `sxe av` + `.dump /ma`). If it stays silent, mark the
   .96 guard as candidate fix and move on.
   → **ANSWERED, and the earlier reading was too generous: the crash *did* reproduce.** See item
   **0i** and [CALC-HANDLE-DANGLING.md](CALC-HANDLE-DANGLING.md). The guard was real but covered
   only one of the two `nonNanCalculatedValue` overloads.
2. **ARM32 build line** — once x64 is stable, rebuild ARM32 engine for Lumia 950
   (needs 20-25 GB disk; we have ~38 GB).
3. **Desktop UA test** — `ua=1` in settings.ini may fix dzen.ru white margins (mobile layout).
4. **Independent rendering/JS tests** — separate GPU rendering verification from JS execution.
5. **Git optional** — per owner's decision; no Git work required.

---

## dzen.ru blank white page (SSO redirect loop) — 2026-09-18

**Symptom (measured):** `dzen.ru` loads `rc=0` but lands on
`sso.dzen.ru/install?uuid=...` — a page with `body=0`, `bodyKids=-1`, one script, `rs=I`
(interactive, never Complete) and `nonwhite=0/710656`. To a user this is indistinguishable
from a crash: a permanently blank white window. Four attempts (mobile UA, desktop UA,
navseq, repeated nav.txt) all landed on the same SSO page; yesterday's run instead showed
`dzen.ru/?sso_failed=blocked` followed by real content, so the site's behaviour changed
between sessions.

**Grounded hypothesis (NOT PROVEN — read from source, not yet tested):**
`Src/port/PortNetworkStorageSession.cpp` uses a **process-level ephemeral in-memory** curl
cookie jar (`SessionID::generateEphemeralSessionID()`, `":memory:"` CookieJarDB) and sets
`CookieAcceptPolicy::OnlyFromMainDocumentDomain`. Two consequences worth testing:
- the accept policy may reject cookies set by the cross-host SSO page (`sso.dzen.ru` while
  the main document is `dzen.ru`), so the SSO handshake never completes and the server
  re-redirects to the install page indefinitely;
- the jar is in-memory only, so nothing survives an app restart (logins cannot persist).

### Short term (cheap, independently useful)

#### ST-1. Never show a bare white void (harness overlay) — ✅ DONE 2026-09-18, measured on `0.1.10.5`

Implemented and verified on the bench. `DiagLooksEmpty` (`MainPage.xaml.cpp:413`) classifies the load
from the diag string, `ShowEmptyPageNotice` / `HideEmptyPageNotice` build the panel in code (no XAML
edit, so `MainPage.g.hpp` and `verify-xaml-connect.ps1` stay out of it), and the acceptance criterion
was met in one session:

```
empty page: notice shown url=https://sso.dzen.ru/install?uuid=… requested=https://dzen.ru/ bodyKids=-1 ua=desktop
empty page: notice hidden                                      <- after navigating to habr.com
diag=url=https://habr.com/ru/ title=Хабр body=1 nonwhite=292060/1036944
```

**One defect was found by testing it rather than by reading it, and fixed in `0.1.10.5`.** The notice
promised "the URL the engine actually ended up on" and the code passed the URL that was *requested* —
on the one case the panel exists for, the two differ and the difference *is* the diagnosis. Worse, both
buttons navigated to `s->m_currentUrl`, so "Reload" re-fetched `sso.dzen.ru/install` and could only ever
produce the same blank window. Now the document URL is read on the load's own thread (never the UI
thread — the engine serializes every C ABI call), the panel names both addresses when they differ, and
both buttons retry the address the user asked for. The original plan text is kept below, with its
numbered steps, as the record of what the implementation was supposed to be.

<details><summary>Original ST-1 specification</summary>

Goal: a load that produced no content must not look like a crash. Show an in-window notice
instead of an empty frame. Independent of the dzen root cause; helps every blank-page case.

Steps:
1. Find the load-completion path in `Src/harness/MainPage.xaml.cpp` (the `[STAGE] after-load`
   site) and the existing diag reader (`WebCoreGetDiag`, already used by `ApplyViewportSize`).
2. After a load completes with `rc == 0`, parse the diag string fields `body=`, `bodyKids=`,
   `nonwhite=` (format: `nonwhite=<n>/<total>`, `bodyKids=-1` when there is no body element).
3. Treat as "empty page" when all hold: `rc == 0`, `nonwhite == 0`, and `bodyKids <= 0`
   (or `body == 0`). Require the values to persist for two consecutive diagnostic samples so a
   slow first paint is not misreported.
4. Build the notice **in code** (do not add named XAML elements): create a `Border` +
   `TextBlock` + `Button` and append it to the existing root panel's `Children`, so
   `MainPage.g.hpp` and `verify-xaml-connect.ps1` stay untouched.
5. Content: the URL, a short line ("The server returned an empty page"), and two buttons:
   **Reload** (re-navigate to the same URL) and **Desktop UA** (flip `ua` in `settings.ini`
   and reload), plus the current UA mode in the text.
6. Hide the overlay on any successful paint (`nonwhite > 0`) or on navigation start.
7. Log one line per occurrence (`empty page: url=... body=0 nonwhite=0/710656 ua=mobile`) so
   the frequency of the condition is measurable across sites.
8. Rebuild harness only (`Src\tools\_build-appx-x64.bat`), bump the manifest, install in-place
   with a LocalState backup, and verify the overlay on `dzen.ru` (expected: notice visible
   instead of a blank window) and on `ya.ru`/`example.com` (expected: no notice).

Acceptance: on dzen.ru the user sees an explanatory notice with working Reload; on rendering
sites the notice never appears. No engine rebuild, no C ABI change.

</details>

#### ST-2. Cookie policy experiment — CLOSED 2026-09-18, premise was wrong, better experiment done

**As written, ST-2 was a no-op.** The plan named a function (`ensureDefaultPortSession`) that does
not exist; the real one is `ensureDefaultPortStorageSession()`, and **it is called from nowhere** —
it appears only as its own declaration, its definition and one include comment. `setCookieAcceptPolicy`
has exactly one call site, inside it (`PortNetworkStorageSession.cpp:56`), so the file's Chinese
comment claiming a restrictive default was describing dead code. `CookieJarDB`'s real default is
`CookieAcceptPolicy::Always` (`CookieJarDB.h:76`) — the *most* permissive value, not the restrictive
one the hypothesis needed changed. Editing that line changes nothing at runtime. A DEAD CODE note was
added above the function so the next reader does not rebuild this trap.

**Why the policy was never the mechanism.** The top-level document does not travel through WebCore's
`ResourceHandle` at all. `ApoFetchChannel::run()` (`WebCoreDriver.cpp`) fetches it on its own curl
handle and feeds the bytes to `DocumentWriter`. That handle had `CURLOPT_FOLLOWLOCATION` but **no
cookie engine**, so every `Set-Cookie` the SSO chain returned was discarded and no `Cookie` was ever
sent — with any accept policy, on any jar.

**The experiment actually run** (`WebCoreDriver.cpp:1405`):

    curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");   // empty string = in-memory cookie engine, no file

plus one diagnostic line per fetch (`:1470-1487`) reporting effective URL, redirect count, jar size,
body size and HTTP code.

**Result on x64** (`gpuinit-steps.txt`, appx 0.1.9.99):

    SL: fetch eff=https://sso.passport.yandex.ru/push?uuid=06b98647-...&retpath=...dzen.ru%2F%3Fis_autologin_ya%3Dtrue
        redirects=1 cookies=9 size=2886 http=200 crc=0

9 cookies stored, and dzen.ru took its autologin branch — a branch it never reached before. Every
other URL in the same session shows `cookies=0`. So the mechanism is confirmed: **the cookie engine
was genuinely missing and is now working.**

**dzen.ru still does not render.** The final document is still
`sso.dzen.ru/install?uuid=06b98647-...`, `body=0 nonwhite=0/710656`, with
`res:[log?uuid=...(s3) install?uuid=...(s2)]` — two subresources at stage 3 and 2, never loaded. The
process stayed healthy throughout (152 MB, responding) and the .96/.97 CSS crash did not reproduce.

**The finding that replaces the hypothesis — two disjoint cookie stores.** The engine enabled here
belongs to `ApoFetchChannel`'s curl handle. WebCore's own jar is a separate `CookieJarDB` reached
through `NetworkStorageSession`, and it is still never fed, because the top-level response does not
pass through `ResourceHandle`. Whatever performs the remaining handshake — page JS issuing
`fetch`/`XHR`, or a WebCore-initiated navigation — reads the **second, empty** jar. Cookies are now
in the process but in the wrong one. This also means the LT-1 note below ("replace main-document-only
filtering") was aimed at the wrong object too.

**Next candidate, not yet attempted:** replay the top-level response's `Set-Cookie` headers into
WebCore's session with `NetworkStorageSession::setCookiesFromHTTPResponse`, collecting them via
`CURLOPT_HEADERFUNCTION` on the fetch handle. That is the join between the two stores. Cheaper
diagnostic first if preferred: log whether the two `sso.dzen.ru` subresources are even requested by
the engine thread or by page script.

**Unrelated but measured in the same build:** the documented `install-local-x64.ps1` /
`x64-cycle.ps1` route calls `Remove-AppxPackage`, which wipes LocalState entirely — logs, settings,
dumps. `Src/tools/update-local-x64.ps1` was added to install in place (bump the manifest, then
`Add-AppxPackage`) and move the previous run's logs aside instead of deleting them.

#### ST-3. Log the redirect chain for one load — ✅ DONE 2026-09-18

A `CURLOPT_HEADERFUNCTION` on `ApoFetchChannel`'s handle records every response hop (status line,
`Set-Cookie` count, `Location`). The chain is now a log line instead of an inference:

    SL: fetch eff=https://sso.passport.yandex.ru/push?uuid=… redirects=1 cookies=9 size=2883 http=200 crc=0
    SL: hop[0] status=302 setcookie=3 loc=https://sso.passport.yandex.ru/push?uuid=…&re…
    SL: hop[1] status=200 setcookie=6 loc=-

#### ST-4. One cookie jar for both network paths — ✅ DONE 2026-09-18, did NOT fix dzen

**The port fetches through two independent paths**, and that is the structural oddity behind this
whole thread: `ApoFetchChannel` (own thread, own curl handle) performs top-level document transfers,
while `PortLoaderStrategy -> ResourceHandle -> NetworkStorageSession -> CookieJarDB` serves
everything the engine asks for. ST-2 proved the first had no cookie engine; ST-4 closes the other
half by filing each hop's `Set-Cookie` into the jar the *second* path reads, with
`NetworkStorageSession::setCookiesFromHTTPResponse(firstParty, hopUrl, value)`, on the engine thread
(the session asserts `isMainThread()`), hop URLs reconstructed by following `Location` because
`sso.passport.yandex.ru`'s cookie is not `sso.dzen.ru`'s.

Measured: `SL: cookies applied=9 hops=2`, applied before the HTML reaches `DocumentWriter` — i.e.
before the page's own JS navigated. **`dzen.ru` still renders nothing**, with a diag byte-for-byte
identical to the pre-fix run. So the jar split was real and is now closed, and it was **not** the
cause of the blank page. Only the top-level → engine direction was implemented: the dzen chain gets
its cookies from top-level responses, so the reverse edge is not needed for this defect, and adding
it speculatively would mean two writers on one header. Measure before adding it.

#### ST-5. Instrument the engine's own network path — ✅ DONE 2026-09-18 (0.1.9.101 / 0.1.9.102)

**Item 1 — the engine's loader is now visible, and the blind spot was never the loader.** The tracer
was left writing only to `port-trace.txt`, the one file that was producing nothing; it now writes the
low-volume events to `portDiagLog` as well, which is the marker channel that has survived every crash
and hang this project has had.

The 0.1.9.101 probe settled it in one build, and the answer is not the one the plan expected:

| Marker | Meaning |
|---|---|
| `PS: install has=0 didInstall=1 fcc=C:\…\LocalState\…` | `FONTCONFIG_FILE` **is** visible from `PortPlatformStrategies.cpp` — the environment was never the problem |
| `PS: createLoaderStrategy` | WebCore asks *our* factory for the loader strategy |
| `loader: CALL https://news.ycombinator.com/news.css?…` ×5 | `CachedResource::load` reaches our `loadResource` |
| `loader: serve started=5 inflight=0 pending=0 hosts=0` | the scheduler serves them and drains |

So the call sites, the environment and the bridge were all fine, and the fault was inside
`portLoaderTrace`'s own file sink — `fopen_s(&file, path, "a")` never produced a file while the
driver's `fopen(path, "ab")` on the identical path always did. 0.1.9.102 rewrites the sink to mirror
the proven function and adds a **one-shot self-report** (`PT: sink ok path=…` / `PT: SINK FAILED path=…
errno=…`), so this failure mode can never again be silent. Measured on 0.1.9.102:
`PT: sink ok path=…\LocalState\port-trace.txt`, and `port-trace.txt` went from **0 to 53** `loader:`
lines in one session.

**Honest limit:** the rewrite changed three things at once (`fopen_s` → `fopen`, `"a"` → `"ab"`, and
adding `fflush`), so which one actually mattered is **not isolated**. The sink works and now reports
its own state; the mechanism is not proven. Recorded as such rather than claimed.

**Item 3 — the analyzer guard — DONE.** `analyze-loader-trace.ps1` now stops instead of concluding
when a trace has no loader traffic, exits 2, and reads **both** `port-trace.txt` and
`gpuinit-steps.txt`. Verified against the real empty `.100` trace, where it previously printed
`verdict: the scheduler is exonerated`. A second, subtler guard was needed and added: `MARK` lines can
now be present while the `+`/`-` pairs are not (they are the bulk channel and stay file-only), which
would print `starts / finishes : 0 / 0` again — the verdict now additionally requires that pairs exist.

**Item 2 — unifying the two tracers into a shared `PortTrace.h` — deliberately NOT done.** The file's
own comment asks for it "the next time that file has to be rebuilt anyway", i.e. `WebCoreDriver.cpp`.
That is not this change, and folding a refactor into a build whose only job is to produce a clean
diagnostic would make a confusing result unattributable. It stays open.

**What the new evidence says about the blank page — the loader is exonerated, on data this time.**
For the dzen generation (`g5`):

    loader: CALL https://sso.dzen.ru/install?uuid=…      loader: +…049CE0 g5 → -…049CE0 g5 h=1
    loader: CALL https://sso.dzen.ru/log?uuid=…          loader: +…03F8F0 g5 → -…03F8F0 g5 h=1
    loader: serve started=7 inflight=0 pending=0 hosts=0

Every request started, every one retired, no `DECLINED`, no `h=0` host miss, nothing left open,
nothing crossed teardown — matching the diag's `pending=0`. `install?uuid=…` is `s2` (**Cached: it
loaded**), `log?uuid=…` is `s3` (`LoadError`). The page is not waiting on anything.

The structural finding is one line up from there: the **session's only top-level fetch** is
`SL: curl download url=https://dzen.ru` → 302 → `sso.passport.yandex.ru/push?uuid=…&retpath=…` → 200,
2883 bytes → `DocumentWriter feed`. The document then *reports* the URL
`https://sso.dzen.ru/install?uuid=…` — a URL that was **never fetched as a top-level document**. The
2883-byte response is a JS redirect page; its navigation created a new `DocumentLoader`, and `body=0`,
`rootKids=-1`, `bodyKids=-1`, `rs=I` is what a document looks like when **its bytes never arrived**.
`ApoFetchChannel` only serves navigations the *harness* asks for, so a page-initiated one has to be
served by the engine's own `FrameLoader` → `DocumentLoader` → `ResourceHandle` path — which is exactly
the path no diagnostic has ever covered. **That is ST-6, and it is where the blank page now lives.**

#### ST-6. Instrument engine-initiated top-level navigations (in progress, 0.1.9.103)

**Premise corrected before any code was written.** This item was drafted as "the one path still
unobserved … it does not go through `PortLoaderStrategy` (subresources only, now proven)". That
parenthesis is **false**, and it was false when it was written. `CachedResource::load()`
(`loader/cache/CachedResource.cpp:271`) is the only asynchronous caller of
`LoaderStrategy::loadResource()`, and `DocumentLoader::loadMainResource()`
(`loader/DocumentLoader.cpp:2318`) reaches it through
`m_cachedResourceLoader->requestMainResource()` → `CachedResourceLoader::requestMainResource()` →
`requestResource(CachedResource::Type::MainResource, …)`. Upstream's `WebResourceLoadScheduler`
answers that same hook with `SubresourceLoader::create` for every type, main resource included
(`Source/WebKitLegacy/WebCoreSupport/WebResourceLoadScheduler.cpp:95`), and this port copies that
shape. **So a page-initiated navigation does go through `PortLoaderStrategy`.** The trace already
contained it — `loader: CALL https://sso.dzen.ru/install?uuid=…` in generation `g5` — and nothing in
the line said so.

That is what the item really is: not a missing path, a **missing label**. The trace prints a URL and
a pointer, so a navigation and an `<img>` on the same page are the same line; and the fail
dispatches dropped the one field that separates a network failure from a refusal.

**What answers the question, and where it is written.** All of it is in the port layer — no upstream
WebCore edit, no WebCore rebuild, which is what keeps it portable to the ARM32 tree as-is.

| Signal | Where | What it settles |
|---|---|---|
| `NAV: policy navigation url=` | `LoadingFrameLoaderClient::dispatchDecidePolicyForNavigationAction` | the page set `location` and the policy was asked — ahead of any `DocumentLoader` |
| `NAV: docloader create url= substitute=` | same, `createDocumentLoader` | a `DocumentLoader` exists for that navigation (every one after the first is engine-initiated: the harness path feeds `DocumentWriter` by hand) |
| `NAV: provisional started` / `start provisional` / `main request url=` / `provisional redirect` | same | the provisional load reached WebCore's own machinery, and its main-resource request went out |
| `NAV: policy response status= mime=` | same, `dispatchDecidePolicyForResponse` | a response *was received* — it can still be refused here, and that must not be read as "no request" |
| `NAV: COMMIT` / `committedLoad bytes=` | same | the decisive line: the main resource arrived and became the document |
| `NAV: FAIL phase= type= code= domain= url=` | same, the three fail dispatches | which dispatch fired, and the `ResourceError`'s **type** |
| `loader: CALL type=N main=M` | `PortPlatformStrategies.cpp` | which arrivals are navigations (`type=0` is `MainResource`) |

**The `lasterr=` ambiguity is closed at the source.** `lasterr=[curlcode=0 domain= desc= url=]` was
read as "no error, so no request was made". It is neither: a *failure dispatch fired*, carrying a
`ResourceError` whose type is `Type::Null` — empty because this port's own `LoaderStrategy` error
factories (`cancelledError`, `blockedError`, `cannotShowURLError`, `interruptedForPolicyChangeError`,
…) all `return { }` — and `Null` is exactly what `ResourceErrorBase::isNull()` tests for
(`platform/network/ResourceErrorBase.h:70`), i.e. the value other WebCore code reads as "no error".
`WebCorePortRecordNetError` now takes and prints `phase` and `type`, so an empty-looking error names
itself instead of being interpreted.

**Open, deliberately not fixed in this build:** those factories returning a null `ResourceError` for
a real refusal is an upstream-semantics violation in its own right, and one that can make WebCore
treat a refused load as a successful one. It is a behaviour change, not an instrument, so it is
measured first and fixed with its own before/after. What is needed to answer ST-6's question is the
type, and the type is now recorded.

### Long term

#### LT-1. Real cookie and storage support (unblocks logins and most SPAs)

- Persist the cookie jar to disk: point `CURL_COOKIE_JAR_PATH` at LocalState (the port header
  itself lists this as the follow-up after the ephemeral jar was adopted to fix the 0.1.6.0
  container crash).
- Replace main-document-only filtering with registrable-domain matching plus explicit
  same-site/third-party policy, so `dzen.ru` and `sso.dzen.ru` can complete a handshake.
- Add tests: cookie set/read across navigations, across a restart, and across two hosts of the
  same registrable domain.

#### LT-2. "Why is this page blank?" diagnostics

One user-visible report per load: empty body, pending resources, rejected cookies, redirect
history, final readyState. This turns future blank-page reports into evidence instead of
guesswork.

#### LT-3. Site-compat levers in the UI

Keep the mobile/desktop UA switch; consider per-site override memory so a site that needs
desktop mode stays in desktop mode.

### Ordering

ST-1 first (user-visible, low risk, no engine rebuild) → ST-2 ✅ **done 2026-09-18** → ST-3 ✅ **done
2026-09-18** → ST-4 ✅ **done 2026-09-18, negative result** → ST-5 ✅ **done 2026-09-18** (items 1 and 3;
item 2 deferred with a reason) → **ST-6 ← in progress 2026-09-18, instrumented in 0.1.9.103; its
premise was corrected before any code was written** → LT-1 → LT-2/LT-3.

ST-2 and ST-4 together closed the cookie question from both ends and **neither explained the blank
page**. ST-5 then closed the loader question and **exonerated the loader on evidence** — every dzen
request starts, retires, and nothing is left pending. Neither the cookie family nor the scheduler
explains the blank page, and both are now ruled out by measurement rather than by argument. What ST-5
did produce is the shape of what is left: the document reports a URL that was never fetched as a
top-level document, which moves the entire question from the network to engine-initiated navigation.
LT-1's framing stays as corrected: its subject is not the accept policy (dead code, see ST-2) and not
jar persistence — it is that two independent fetch paths exist at all, and that one of them is
invisible. ST-6 makes the second one visible.


---

> Updated: Aug 25, 2026

## NEXT PHASE: full x64 build line rebuild (before returning to ARM32)

The x64 line has found its fans -- owners of older UMPC tablets with Windows 10 x64. The dev
machine ("стенд") must also stay in working order as a powerful secondary debugging tool for the
custom browser. A full from-scratch x64 rebuild is planned before any further ARM32 work:

- Rebuild `build-x64-gpu` from scratch with all accumulated fixes (.62-.77): persistent curl,
  background fetch, fail-fast timeouts, async image decode, useJIT=false, Watchdog, background
  settings thread.
- Verify ya.ru and news.ycombinator.com survive on the Surface Pro 5.
- This validates every fix on a second platform before locking them in.

---

## 0. Current state and the next three things

### What v1.0 is, stated by the maintainer on 2026-08-21

> A light, minimal browser that somehow opens a dozen not-useless sites — from the simplest test pages
> like `example.com` to news sites like Hacker News and dzen.ru. Elaborate authentication in Google
> services and watching YouTube are **out of scope**: if they happen to work, that is a happy bonus; if
> not, so be it.

This is the definition to hold the plan against, and it is deliberately narrow. "Somehow opens" tolerates
slow; it does not tolerate blank. Two consequences worth stating because they *reduce* work:

- **In scope for v1.0**: a session that can visit ten sites one after another, pages that finish loading,
  content that is actually painted, scrolling that moves. That is it.
- **Out of scope for v1.0**, however tempting: device-pixel-ratio rendering (sharper text, not more sites
  opened), `crypto.subtle` (login flows, explicitly excluded), video and MSE, the performance profile
  work. All of these are recorded in `Doc/REVENANT-COMPARISON.md` behind the v1.0 line.

Measured against that definition, only two of the open defects actually block v1.0: the second load in a
session (task #14) and deferred scripts that never execute (task #1). Everything else on the list is
quality or reach, not the MVP.

The ARM32 line is no longer a build target — it is a **running browser on a Lumia 950**. That moves the
plan: the question is no longer "can we build for the device" but "which defect keeps it from being
usable".

**Done since Aug 19, each with its own document:**

- The launch crash: `SecurityOrigin::protocol()` / `::host()` returned `String` by reference to a
  temporary, harmless on x64 and fatal on ARM32 — `Doc/ARM32-DANGLING-SECURITYORIGIN.md`.
- The app was re-navigating to the current URL after GPU init, and the replayed load killed the
  process. Removed; a composited frame is now requested instead of refetching the page.
- The white screen: switching to the GPU surface is conditional on a frame actually coming out of it,
  because a page without compositing layers has nothing to present.
- The ARM32 toolchain, dependencies and appx pipeline, including the four undocumented prerequisites —
  `Doc/ARM32-BUILD-GUIDE.md` §11.
- Both harness architectures on one SDK (19041) and one XAML compiler, with a build-time check that
  fails if `MainPage.g.hpp` drifts from the markup.

**The three things that matter next, in order:**

1. **The second load in a session.** The first page renders, later ones do not: the same `example.com`
   gives `nonwhite=177840/177840` first and `0/177840` third, with `rs=C`, `pending=0` and correct
   layout each time. One root cause with two faces — it used to crash, now it paints nothing. Suspect
   the session-reuse branch of `WebCoreSessionLoad` leaving the paint target pointing at the previous
   document. Reproduce on x64, where an iteration costs minutes rather than a 20-minute deploy.
2. **Give x64 the same text stack as ARM32.** x64 has `USE_HARFBUZZ` off and ICU 75.1; ARM32 has
   HarfBuzz and ICU 78.3. Every rendering comparison made on x64 until now used a different shaping
   engine than the device — `Doc/HARFBUZZ-ICU-DIVERGENCE.md` has the six-link causal chain and a
   two-stage fix.
3. **Deferred scripts that never execute**, which is what keeps hh.ru from finishing —
   `Doc/DEFERRED-SCRIPTS.md`.


### Update Aug 25, 2026: instant silent death bisected and closed

Eleven builds (.62-.77) of investigation, seven diagnostic instruments, three root causes found
and fixed. ya.ru now loads fast and survives indefinitely. The bisection proved:

- **JS execution is the killer**: without JS, all sites survive; with JS, death correlates with
  script volume (example.com=0 scripts survives, dzen.ru SPA dies instantly).
- **JIT execution on W10M AppContainer** terminates the process silently. useJIT=false is now
  unconditional on both architectures until a proper W^X page-provisioning mechanism exists.
- **The main-document fetch must never block the engine thread.** It runs on a dedicated thread;
  the engine waits max 2.5 s then shows an error page.

Lightweight mode (JS off) is viable for the v1.0 definition: "a light, minimal browser that
somehow opens a dozen not-useless sites". Static HTML+CSS renders correctly, links work,
navigation is stable. Full JS support requires fixing whatever inside JSC kills W10M processes
-- that is the next major phase after x64 build line consolidation.

See `Doc/PUMPLOOP-SILENT-DEATH.md` for the full investigation log.

Everything below this section predates Aug 19. Where it disagrees with this section, this section wins.

---

## 1. Repository Cleanup & Path Migration ✅

### Status: Completed

All `E:\Apotheosis\` hardcoded paths have been replaced with `$env:APOTHEOSIS_*` environment variables throughout `.ps1`, `.bat`, `.cmake` files. The `Harness.vcxproj` uses relative `$(ProjectDir)` syntax.

| Task | Status |
|------|--------|
| Remove `repro_*`, `mangle-repro*`, `_*.bat`, `*.obj`, `*.log` from `port/` | ✅ ~60 files deleted |
| Create `Src/setenv.ps1` with `APOTHEOSIS_ROOT` + sub-vars | ✅ |
| Fix 87 path occurrences in scripts | ✅ |
| Fix 83 quoting issues (single→double quotes) | ✅ |
| Fix `Harness.vcxproj` (vcxproj uses MSBuild properties) | ✅ |
| Fix `harness-cmd.bat` (bat uses `%VAR%` syntax) | ✅ |
| Verify zero `E:\Apotheosis\` in `.ps1`/`.bat`/`.cmake` | ✅ Clean |

### Key files created

| File | Purpose |
|------|---------|
| `Src/setenv.ps1` | Sets `$env:APOTHEOSIS_ROOT`, `PORT`, `HARNESS`, `TOOLS`, `ANGLE`, `CRASH` |

---

## 2. x64 Build Infrastructure ✅

### Status: ALL deps installed, CMake configured, WTF/bmalloc compiled, PAL header gen done, WebCore compiling

| Component | Status | Notes |
|-----------|--------|-------|
| LLVM/clang-cl 22.1.8 | ✅ Installed | `C:\Program Files\LLVM\` |
| Ninja | ✅ Via VS2022 | `...\CMake\Ninja\ninja.exe` |
| WebKit source (webkitgtk-2.52.4) | ✅ Cloned | commit e4ab5336 |
| ANGLE x64 binaries | ✅ Done | NuGet → `Src\angle\x64\` |
| vcpkg x64-uwp deps (16 pkgs) | ✅ ALL INSTALLED | Community triplet (no VS_PATH/TOOLSET bug) |
| vcpkg extra pkgs | ✅ freetype, harfbuzz[core], libxml2, libpng, zlib, brotli, pixman | For Cairo image backend |
| ICU x64-uwp | ✅ Built | `C:\icu-x64-uwp\` |
| SQLite3 x64-uwp | ✅ Manually built | Amalgamation, `SQLITE_OS_WINRT=1` |
| **Cairo 1.18.4** | ✅ **Manually built** | 49 .obj → 1 MB static lib (image backend only) |
| **libpsl** | ✅ **Stub** | `libpsl.h` header stub |
| **curl** | ✅ **`x64-windows` fallback** | `x64-uwp` triplet failed |
| CMake configure | ✅ FIRST SUCCESS (June 29, 2026) | `build-x64-gpu` generated |
| **OptionsWinUWP.cmake** | ✅ **Complete** | Cairo, ANGLE, CURL, LibPSL stubs configured |
| **PlatformWinUWP.cmake** | ✅ **Complete** | Includes Cairo + Curl + Win platform dirs |
| WTF compiled | ✅ | 12+ WK_WINUWP patches across 8 WTF files (RunLoopWin, FileSystemWin, WindowsExtras, DbgHelperWin, OSAllocatorWin, MemoryPressureHandlerWin, SignalsWin, PlatformWin.cmake) |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId via `add_definitions` |
| **PAL build** | ✅ **Header gen complete** | Object library — .obj files compiled as part of WebCore |
| **JavaScriptCore** (physical file set) | ✅ **COMPILED** | FTL_JIT enabled; `JavaScriptCore.dll` 18.8 MB; both AT&T-asm files via GNU driver |
| **WebCore** (includes PAL .objs) | 🔄 **All 156 source files compiled** | ✅ All 156 unified sources compile cleanly. Linker blocked: 20 unresolved externals in `stubs-other.cpp`. |

### GNU driver for AT&T assembly files

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly that clang-cl cannot parse. Solution: two custom ninja rules using **clang++ (GNU driver)**. The GNU rules use `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in` (not the MSVC `/Fo`/`/showIncludes` flags). `FLAGS` must use `-D`/`-I` syntax; `-imsvc` in `INCLUDES` must be replaced with `-isystem` since clang++ doesn't understand `-imsvc`.

Status: LowLevelInterpreter.cpp ✅; MacroAssemblerX86_64.cpp 🔴 (`-imsvc` not yet replaced in its INCLUDES).

### Key findings from PAL build analysis

| Discovery | Detail |
|-----------|--------|
| **PAL is an OBJECT library** | No `PAL.lib` target exists. PAL's 19 `.cpp` files compile to `.obj` files that are directly linked into WebCore.dll |
| **2462 total ninja edges** | Full build graph for PAL target includes all header generation (1203 steps) + compilation steps |
| **`ninja PAL` only generates headers** | The `PAL` phony target depends on `PAL_CopyHeaders` only, not on PAL object compilation |
| **Actual compilation targets** | `ninja JavaScriptCore` → `bin/JavaScriptCore.dll`, `ninja WebCore` → `bin/WebCore.dll` |
| **C++23 confirmed** | `build.ninja` uses `-clang:-std=c++23`, WTF/bmalloc compile cleanly |
| **Build launch workaround** | Use `[System.Diagnostics.Process]::Start()` for long builds — tool's bash kills subprocesses after timeout |
| **Stale process hazard** | Two zombie clang-cl processes stalled for 1+ hour compiling `LowLevelInterpreter.cpp`. Always `taskkill /F /PID` stale PIDs + delete `.ninja_lock` |
| **CMake 4.0 missing rules** | Only ~10 of 34+ compiler/linker rules emitted in `rules.ninja`. Workaround: `patch-build-ninja-gnu.ps1` scans `build.ninja` for all rule names, auto-generates missing rules in `rules.ninja`. Also adds utility rules (`CLEAN`, `HELP`, `RERUN_CMAKE`) omitted by CMake 4.0 |

### GNU driver status (July 1)

Both LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp now compile cleanly with the clang++ GNU driver. The `-imsvc`→`-isystem` conversion is handled by `patch-build-ninja-gnu.ps1`. GNU rules use `deps = gcc`, `-MD -MF $out.d`, `-o $out -c -- $in` syntax.

**Important:** `build.ninja` corruption discovered — if the patch script is interrupted (timeout) during `WriteAllText`, the 38MB `build.ninja` is silently truncated. Always verify file integrity after patching, or re-run CMake with deleted `build.ninja` + `rules.ninja` to regenerate fresh files before re-patching.

### What was fixed in scripts

| Script | Fix |
|--------|-----|
| `configure-gpu-x64.ps1` | Added `$cmake` and `$ninja` path definitions (were undefined) |
| `link-driver-gpu-x64.ps1` | ANGLE path `$Root\angle\x64` → `$Root\Src\angle\x64`; added compile-each-source loop |
| `compile-driver-gpu-x64.ps1` | Full compile script with correct target triple, includes, UWP defines |

---

## 3. Multilingual UI (en/ru/zh) ✅

### Status: Completed

| Feature | Status |
|---------|--------|
| `.resw` files for en-US, zh-Hans, ru-RU | ✅ Created |
| `GetStr()` dual-source loading (`.resw` + fallback table) | ✅ Implemented |
| All hardcoded Chinese toasts replaced | ✅ 20+ locations |
| Language persistence (`settings.ini` → `lang=0\|1\|2`) | ✅ |
| `ComboBox` language selector in Settings | ✅ |

### Future improvements

1. Migrate XAML static labels to `x:Uid` binding
2. Auto-detect system language on first launch
3. Localize `kHomeHtml` (browser home page HTML)

---

## 4. WebKit Upgrade: 2.52.4 → 2.53.4

See `Doc/WEBKIT-UPGRADE.md` for full analysis.

**Status:** Research complete. **Effort:** Low-Medium (~2-3 days). Mostly Skia-focussed changes — low impact on our TextureMapper path.

---

## 5. Upstream Sync (gpu-path1, June 28)

### Status: Patches applied

| Commit | Change | Our sync |
|--------|--------|----------|
| `22c9721` | Anti-OOM + BackForwardCache disable | ✅ Patched `WebCoreDriver.cpp` |
| `1601b87` | MEDIA-PLAN.md | ✅ Copied to `Doc/` |
| `a5a1a20` | CryptoDigest → real OpenSSL SHA | ✅ Patched `stubs-crypto.cpp` |
| `ca387e7` | OOBE language selection | ⚠️ Our 3-lang i18n is more advanced |
| `c18e0fd` | MinVersion lowered to 14393 | ✅ Patched `Package.appxmanifest` |
| `5ff1a18` | License cleanup | ✅ Kept our LICENSE |
| `00d5427` | HTTP→HTTPS redirect fix | 🔄 Needs WebKit source patch |

### Files patched

| File | Change |
|------|--------|
| `Src/port/stubs-crypto.cpp` | CryptoDigest → real OpenSSL SHA |
| `Src/harness/Package.appxmanifest` | Version 0.1.8.5, MinVersion 14393 |
| `Src/port/WebCoreDriver.cpp` | BackForwardCache::setMaxSize(0), MemoryCache caps, WebCoreReleaseMemory() |
| `Src/port/WebCoreDriver.h` | WebCoreReleaseMemory() declaration |

---

## 6. Critical Technical Debt

### 6.1. Path system ✅ RESOLVED
- ✅ `E:\Apotheosis\` removed from all scripts
- ✅ `$env:APOTHEOSIS_ROOT` + sub-variables in place
- ✅ `$(ProjectDir)` relative paths in vcxproj

### 6.2. Remaining
| Issue | Status | Notes |
|-------|--------|-------|
| Signing cert password in clear text | ⚠️ Known | `CN=EdgeHTMLReborn` hardcoded |
| ARM32 vcvars missing in VS2022+ | ⚠️ Known | `arm32-uwp-env.ps1` workaround |
| SDK 26100 removed ARM32 libs | ⚠️ Known | Locked to SDK 19041 |
| ICU path hardcoded | ⚠️ Known | `C:\icu-arm-uwp\` not configurable |
| Stale ninja processes | ⚠️ Operational | Must kill before each build |

### 6.3. vcpkg VS UWP Detection ✅ RESOLVED

**Problem:** `vcpkg install <pkg> --triplet x64-uwp` fails with "Unable to find a valid Visual Studio instance" when using overlay triplet with explicit `VCPKG_VISUAL_STUDIO_PATH`.

**Root cause:** vcpkg's VS detection validates UWP instances by checking registry keys that fail when `VCPKG_VISUAL_STUDIO_PATH` is explicitly set.

**Resolution:** Use **community triplets** (`C:\vcpkg\triplets\community\x64-uwp.cmake`) instead of overlay triplets. No `VCPKG_VISUAL_STUDIO_PATH` — lets vcpkg auto-detect VS.

```cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME WindowsStore)
set(VCPKG_CMAKE_SYSTEM_VERSION 10.0)
```

---

## 7. Architecture Overview

### 7.1. Graphics Pipeline (CPU vs GPU)

```
WebKit (HTML/CSS → rendering tree)
        │
        ├── CPU path (default, always available)
        │   WebCore → Cairo → RGBA bitmap → WriteableBitmap (XAML)
        │
        └── GPU path (activated after WebCoreGpuInit() succeeds)
            WebCore → TextureMapper → OpenGL ES 2.0 → ANGLE → D3D11 → SwapChainPanel (XAML)
```

### 7.2. Cross-Platform: ARM32 vs x64

| Layer | ARM32 (Lumia 950) | x64 (PC debug) |
|-------|-------------------|----------------|
| **Toolchain** | clang-cl `thumbv7-unknown-windows-msvc` | clang-cl `x86_64-unknown-windows-msvc` |
| **System** | UWP App Container | UWP App Container (same sandbox) |
| **JIT** | JSC JIT (ARM) | JSC JIT + **FTL_JIT** (x64) |
| **Graphics** | ANGLE → D3D11 **FL 9.3** → Adreno 418 | ANGLE → D3D11 **FL 11** (desktop); GPU path live since Aug 16 — `WebCoreGpuResize` keeps the surface at ContentArea size |
| **Dependencies** | `C:\vcpkg\installed\arm-uwp\` | `C:\vcpkg\installed\x64-uwp\` |
| **ICU** | `C:\icu-arm-uwp\` | `C:\icu-x64-uwp\` |
| **Build dir** | `build-arm32-{webcore,jit,gpu}` → being renamed **`build-arm32-*`** | `build-x64-gpu` (= `build-<arch>-<variant>`) |
| **Driver objs** | `.arm.obj` / `.gpu.obj` / `.jit.obj` → unifying to **`.arm32.obj`** | `.x64.obj` |
| **Linking** | static WebKit libs (PAL/WTF/WebCore/JavaScriptCore) | DLL import libs + `WebCoreFull.lib` for the harness |
| **Float caveat** | hard-float NEON; `#if __ARM_PCS_VFP || __thumb__` clamp in Cairo image interp (saturation overflow) | x87/SSE — no such issue |
| **Appx version** | **SAME** — shared `Package.appxmanifest` (ProcessorArchitecture overridden per-platform by MSBuild packaging) | **SAME** |

---

## 8. Roadmap

> **Plan policy (Aug 16 2026)**:
> - **GitHub-related tasks are OPTIONAL / non-blocking** — CI, GitHub Actions, auto-releases, PR
>   pipelines, GitHub-hosted runners are all deferred "nice-to-have" items; never schedule them as
>   required milestones. Local builds and local verification come first.
> - **All ARM32-related dev/build tasks are FULLY LOCAL** — they run on the dev machine (this repo,
>   `build-arm32-*`, `build-x64-gpu`; local clang-cl / lld-link / MSVC v143 / `arm32-uwp-env.ps1` /
>   `configure-gpu-*.ps1` / `link-*.ps1`). No cloud CI / GitHub runners for ARM32; real-device work is
>   manual via `Deploy-Robust.ps1` + WDP.
> - Ordering: local x64 verification → local ARM32 build artifacts → manual real-device validation →
>   (optional) any CI/GitHub automation.

### Phase A: Build Environment ✅ COMPLETE (June 29-30, 2026)

| # | Component | Status | Notes |
|---|-----------|--------|-------|
| 1 | LLVM/clang-cl | ✅ | 22.1.8 |
| 2 | Ninja | ✅ | WinGet + VS2022 bundled |
| 3 | WebKit source | ✅ | webkitgtk-2.52.4 |
| 4 | ANGLE x64 binaries | ✅ | NuGet → `Src\angle\x64\` |
| 5 | vcpkg x64-uwp deps (16 pkgs) | ✅ | Community triplet |
| 6 | ICU x64-uwp | ✅ | `C:\icu-x64-uwp\` |
| 7 | SQLite3 x64-uwp | ✅ | `SQLITE_OS_WINRT=1` |
| 8 | CMake configure | ✅ | `build-x64-gpu` |
| 9 | WTF compiled | ✅ | 12+ WK_WINUWP patches across 8 WTF files |
| 10 | bmalloc compiled | ✅ | getpid fix |
| 11 | PAL header generation | ✅ | 1203 header steps completed |

### Phase B: First x64 WebKit Build ✅ COMPLETE (July 1-18, 2026)

| Step | Component | Status | Notes |
|------|-----------|--------|-------|
| 1-9 | vcpkg/ICU/SQLite/Configure/WTF/bmalloc/PAL/JSC/WebCore | ✅ All compiled | |
| 10 | Link port driver → `WebCoreDriver-gpu.lib` | ✅ | 947 .objs → static lib (clang-cl) |
| 11 | **Harness.exe** | ✅ **46 MB, zero linker errors** | UWP C++/CX app (MSVC v145) |

**Three-part link solution** (Jul 18 2026 — superseded by pure-stub approach Jul 20):

| Technique | Was needed for | Current approach |
|-----------|---------------|-----------------|
| `WebCoreFull.lib` (947 `/MD` .objs) | ~15 WebCore internal symbols | **Removed** — 7 stubs in `webcore-internal-stubs.cpp` provide local definitions for the 7 symbols the port layer directly references. The other 8 symbols were already available via WebCore.dll exports. |
| `cairo-complete-x64-uwp.lib` + `/NODEFAULTLIB:libcmt.lib` | All Cairo C API + internal symbols | **Removed** — Cairo enters only via WebCore.dll exports. No static Cairo lib needed. CRT mismatch eliminated. |
| `wtf-string-stubs.cpp` (4 local data globals) | `s_emptyAtomString`, `emptyStringData`, `nullStringData`, `nullAtomData` | **Retained** — these 4 WTF data stubs are still needed (WTF.dll does not export them; they're referenced by inline functions in WTF headers). |

**Build environment:** Surface Pro 5 (i5-7300U, 4GB RAM); VS18 Insiders (MSVC 14.51.36231, PlatformToolset v145).

### Phase B2: x64 Appx Build — VS18 XAML Fix + Link Resolution (Jul 18-20)

| Step | Task | Status |
|------|------|--------|
| 1 | Build Harness.exe (MSVC v145, x64, Release) | ✅ 46 MB (Phase B2-1) → 1 MB (Phase B2-6) |
| 2 | Build UWP Appx package for x64 | ✅ 70.5 MB — deploys to AppContainer |
| 3 | Fix DLL dependencies in Appx | ✅ CRT DLLs added, icudt75.dll stub created, XBF inclusion fixed |
| 4 | **Fix VS18 XAML compiler compatibility** | ✅ `.g.h` (not `.g.hpp`) includes + `XamlGimpl.cpp` includes `.g.hpp` implementations |
| 5 | Verify AppContainer launch | ❌ **Superseded** — 46 MB EXE's silent exit was CRT mismatch; 1 MB EXE not yet tested |
| 6 | **Resolve all LNK2019 — final approach** | ✅ **7 stubs in `webcore-internal-stubs.cpp`** (3 funcs + 4 data globals). `U_BUILDING_WebCore` did NOT work — port layer's `-DBUILDING_WebCore` generates DIRECT references, not `__imp_` thunks. Only local definitions can resolve. |
| 7 | Rebuild WebCore.dll cleanly | ✅ **Manual `lld-link` via `link-webcore-dll.ps1`** (47 MB, ~151 s). Bypasses ninja's stale-`.ninja_deps` + missing `WebCoreBindings` marker (timed-out previous build never flushed). |
| 8 | Rebuild Harness.exe (clean link) | ✅ **1,057,792 bytes — zero LNK2019** |
| 9 | Port x64-gpu infra back to ARM32 | ⏳ Next milestone |
| 10 | Build Harness for ARM32 → Lumia 950 | ⏳ After x64 launch verification |

### Phase B3: x64 AppContainer Launch — GS Crash & XAML Crash (Jul 20)

| Step | Task | Status |
|------|------|--------|
| 1 | Install v0.1.8.17 appx on Win11 (29617) | ✅ Installs & launches (XAML OK) |
| 2 | Identify GS crash in `frame->init()` → ucrtbase.dll (0xc0000409) | ✅ Confirmed — stack overflow theory |
| 3 | Change thread stack: std::thread(1 MB) → CreateThread(8 MB) | ✅ Applied |
| 4 | Compile real port lib (all 12 files) | ✅ 15.8 MB, all 12 .cpp compiled after reboot |
| 5 | Build appx v0.1.8.18 with real lib | ✅ 53.8 MB |
| 6 | **Discover XAML crash in ALL new builds** | 🔴 XAML activation (0xc000027b) @ 0x916eff in Windows.UI.Xaml.dll |
| 7 | Determine root cause of XAML crash | ⏳ Unknown — environmental (VS18 build differs from VS2022) |
| 8 | Workaround: manual appx repack (original XBF + new EXE) | ❌ New EXE still crashes XAML regardless |
| 9 | Build Harness.exe with 8 MB stack + port lib → test | ⏳ Blocked by XAML crash |
| 10 | Deploy to ARM32 Lumia 950 | ⏳ After x64 launch |

### AppContainer Launch Findings (Jul 20 2026)

**Two independent crashes identified:**

**Crash #1 — GS (`0xc0000409`) in `ucrtbase.dll`**:
- Original v0.1.8.17 EXE: XAML activation OK, crashes in `frame->init()` → WebCore call chain overflows thread stack
- Root cause (theory): WebEngine thread gets only **1 MB stack** (`std::thread` default), PE header reserves 16 MB for main thread only. In AppContainer, guard pages are stricter → stack overflow corrupts `/GS` canary → false positive `STATUS_STACK_BUFFER_OVERRUN`
- **Fix**: Replace `std::thread([this]{loop();}).detach()` with `CreateThread(nullptr, 8*1024*1024, ...)` in `MainPage.xaml.cpp:368`
- Also added `#pragma strict_gs_check(push, off)` at dense call sites (buildSession, WebCoreLoadUrl, WebCoreNavigateTo) in `WebCoreDriver.cpp`

**Crash #2 — XAML activation (`0xc000027b`) in `Windows.UI.Xaml.dll` @ 0x916eff**:
- ALL EXEs built in current VS18 Insiders environment exhibit this — including:
  - Build with `/GS-` (old) and default `/GS` (new)
  - Build with stub lib and real lib
  - Build with `AppxPackage=true` and `AppxPackage=false` → manual repack
  - Build with 1 MB and 8 MB thread stack
- Original v0.1.8.17 EXE (pre-built, unknown build toolchain) does NOT have this crash
- **Root cause**: Not related to code changes. Possibly MSVC CRT version mismatch (14.51 vs 14.44+), XAML compiler version difference (VS18 vs VS2022), or SDK/Windows metadata binding change.
- **No known fix yet.**

**Memory crisis (4 GB RAM)**:
- Surface Pro 5 (i5-7300U): ~440 MB free after reboot, 18.3 GB free disk
- Compilation of `WebCoreDriver.cpp` (7.5 MB C++ with WebCore header chain) requires reboot first — otherwise `fatal error C1060` (heap exhausted)
- All 12 port files compiled successfully after reboot → `WebCoreDriver-gpu.lib` = 15.8 MB
- Always use `ninja -j1`; never parallel WebKit builds

**Key discovery (Jul 20): Export approach abandoned for stub approach**
- The 15 symbols identified Jul 19 were correct, but the port layer is compiled with `-DBUILDING_WebCore` (dllexport mode) — this makes ALL references DIRECT, bypassing `__declspec(dllimport)` totally. Even symbols exported from WebCore.dll with `__imp_` thunks in `WebCore.lib` are NEVER used by the port layer.
- `-U_BUILDING_WebCore` was tried but did NOT produce `__imp_` references as expected — the resulting LNK2019 set was identical.
- **Only solution**: provide local stub definitions for every symbol the port layer directly references.
- Final count: **7 unique symbols** (3 non-exported functions + 4 exported data globals), not 15 — the other 8 symbols resolved via WebCore.dll's existing exports or were already in the import lib.

**The 7 resolved symbols (Jul 20):**

| # | Symbol | Kind | Resolution |
|---|--------|------|-----------|
| 1 | `DocumentLoader::addData(...)` | Non-exported func | Stub (no-op) |
| 2 | `Editor::insertTextWithoutSendingTextEvent(...)` | Non-exported func | Stub (no-op) |
| 3 | `PlatformStrategies::hasPlatformStrategies()` | Non-exported func | Stub (returns `false`) |
| 4 | `HTMLNames::aTag` | Exported data | `LazyNeverDestroyed<HTMLQualifiedName>` — **unconstructed** (no default ctor for `QualifiedName`) |
| 5 | `HTMLNames::inputTag` | Exported data | Same — unconstructed |
| 6 | `HTMLNames::textareaTag` | Exported data | Same — unconstructed |
| 7 | `ResourceRequestBase::s_defaultTimeoutInterval` | Exported data | `= 10.0;` (plain `double`) |

**Risk**: #4-6 are `LazyNeverDestroyed<HTMLQualifiedName>` with `.construct()` NOT called (no default ctor). If code calls `.get()` on them (e.g., `extractLinks`), it will crash.

**Ninja stale-deps root cause (Jul 20):**
- Previous `ninja WebCore -j1` build timed out at the link step (lld-link → out of memory on 4 GB RAM machine)
- The timed-out build NEVER flushed `.ninja_deps` and `.ninja_log` — both remained stale
- Additionally, the `Source/WebCore/CMakeFiles/WebCoreBindings` marker file was missing (never created by the interrupted build)
- Next ninja run detected `stored deps info out of date` for every .obj and missing marker → triggered full 932-step rebuild (# WebCore 7346 actions)
- **Fix**: manually invoke `lld-link` with all 931 .obj files directly (script: `link-webcore-dll.ps1`), skipping ninja entirely

**Files created/changed (Jul 20):**
- `Src/port/webcore-internal-stubs.cpp`: NEW — all 7 stubs
- `Src/port/webcore-exports.def`: RTTI entry removed (`??_R4ChromeClient@WebCore@@6B@` — not generated with `/GR-`)
- `Src/port/link-webcore-dll.ps1`: NEW — manual lld-link for WebCore.dll
- `Src/port/recompile-stubs-x64.ps1`: NEW (originally `recompile-stubs-arm32.ps1`, renamed Aug 16 — it is an x64 script) — single-file recompile of port stubs
- `Src/harness/Harness.vcxproj`: WebCoreFull.lib, cairo-complete-x64-uwp.lib, `/FORCE:MULTIPLE` all removed
- `Src/port/webcore-exports.def`: Stripped of RTTI entries

### Phase C: Upgrade & Refactor
1. Upgrade WebKit to 2.53.4
2. Consolidate stubs, reduce code duplication
3. Add CI

---

## Current Goal (Aug 11, 2026): ✅ ACHIEVED — "about:home renders text on x64, zero exceptions"

**Previous overriding goal**: `about:home` (and any page) must render with **zero exceptions** — приложение не вылетает при старте.

**Status: met on the x64 dev line.** The app launches clean, `WebCoreRenderHtml` returns `rc=0`, and the frame contains real antialiased text (596 distinct colors; was 1). ARM32 inherits the same font path and should be re-validated on device.

### Where we are (state machine)

| Step | State | Evidence |
|------|-------|----------|
| 1. Font-patched `FontPlatformData` ctor (bundled TTF via FreeType) | ✅ done | `WebKit/.../FontPlatformData.cpp` `#if WK_WINUWP`; `apotheosisBundledFontFace()` |
| 2. `WebCore.dll` rebuilt + linked with psl.lib | ✅ done | 47.45 MB (11.08 11:05) |
| 3. `WebCoreFull.lib` regenerated; harness links psl.lib | ✅ done | link order now `WebCore.lib` **before** `WebCoreFull.lib` — see below |
| 4. Full appx v0.1.8.25 built + installed on Win11 | ✅ done | engine init clean (fonts ×5, CACert, loop ready) |
| 5. Tier 3 crash (0xc0000005 in `cairo_scaled_font_create`) | ✅ **fixed Aug 10** | `Doc/2026-08-10-crash-fix.md` |
| 6. Font stubs implemented (`Font::platformInit`, `GlyphPage::fill`) | ✅ done | pure Cairo+FreeType, no fontconfig; verified at runtime via `APOTHEOSIS_GLYPH_LOG` |
| 7. **Text never painted** (`drawGlyphs` = 0 calls, flat-color frame) | ✅ **fixed Aug 11** | root cause: whole page forced onto the unimplemented complex text path — see `Summary.md` §10 |
| 8. `about:home` renders text | ✅ **done** | `drawGlyphs` ×3 (n=15 @64px, n=33 @28px, n=8 @20px); glyphs legible & correctly colored |

### What fixed it

`FontCascade::codePath()` forced every run longer than one character to `CodePath::Complex` because the build defines neither `USE_FONT_VARIANT_VIA_FEATURES` nor `USE_FREETYPE`. The complex path lands in `ComplexTextController::collectComplexTextRunsForCharacters`, a **no-op stub** — zero runs, zero glyphs, nothing drawn. Both that guard (`FontCascade.cpp:676`) and the sibling in `canHandleRunAsSimpleText` (`:633`) now carry `&& !defined(WK_WINUWP)`, putting this Cairo+FreeType port on the FreeType branch it semantically belongs to. `USE_FREETYPE` itself cannot be defined — upstream ties it to fontconfig, which changes `FontPlatformData`'s layout and pulls headers this port does not ship.

### Build-graph gotchas worth remembering

- Unified sources mean an upstream `.cpp` has **no per-file `.obj`** — grep `build-x64-gpu/WebCore/DerivedSources/unified-sources/` (674 hash-named bundles) to find its home.
- `lib/WebCore.lib` is an **import** library; `llvm-nm` on it lists only `webcore-exports.def` entries. Query the objects instead.
- Harness link order: `WebCore.lib` **before** `WebCoreFull.lib`, else `LNK4006` flood → `LNK1102` (linker OOM).
- Ninja can't drive these rebuilds (stale deps log, post-move paths). Use `build-x64-gpu/mkrsp.py <obj-basename>` → `clang-cl @relink.rsp` → `Src/port/link-webcore-dll.ps1`.
- Python here is the **`py`** launcher (3.12.10); bare `python`/`python3` hit the Store alias stub.

### Next objective

1. 🔴 **Remove temporary instrumentation**: the `apotheosisTextLog` trace in `FontCairo.cpp` (`FontCascade::drawGlyphs`) and the `apotheosisGlyphLog` calls in `Src/port/stubs-other.cpp`; rebuild + relink.
2. 🔴 **Render a real page** (not just the inline about:home) on x64 and verify text, images, and layout.
3. 🔴 **Wire the real complex text path** — upstream ships `platform/graphics/harfbuzz/ComplexTextControllerHarfBuzz.cpp` and HarfBuzz is already linked. Needed for RTL/Arabic/Indic and ligature-heavy content, which currently renders blank.
4. 🔴 **Re-validate on ARM32 / Lumia 950** — the same `WK_WINUWP` guards apply to the device build.
5. 🟡 Consider exporting the 4 GPU/TextureMapper symbols via `webcore-exports.def` so the harness can drop `WebCoreFull.lib` entirely (reclaims 3.4 GB and matches the ARM32 link shape).

### Key artifacts for this goal

- Frame dump: `%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_edmb40rfkwsbg\LocalState\shot_ui.bmp`
- Frame verifiers: `build-x64-gpu/bmpstat.py` (color histogram), `build-x64-gpu/bmp2png.py` (visual check, stdlib only)
- Rsp generator: `build-x64-gpu/mkrsp.py`
- Font patch: `WebKit\Source\WebCore\platform\graphics\FontPlatformData.cpp`
- Code path fix: `WebKit\Source\WebCore\platform\graphics\FontCascade.cpp` (lines 631-642, 674-686)
- Font stubs: `Src\port\stubs-other.cpp` (`Font::platformInit`, `GlyphPage::fill`)
- LocalState diag: `...\LocalState\log.txt`, `...\LocalState\glyph.log`

### Phase D: Features
1. WebGL support (already partially working via ANGLE)
2. Service Worker / PWA support
3. Video/audio playback (see `Doc/MEDIA-PLAN.md`)
4. Upstream WK_WINUWP patches

---

## 8.5. Current Goal (Aug 16, 2026): ✅ CJK done + ✅ GPU whole-content distortion FIXED

### Done (Aug 16)
- **CJK hang root-caused & fixed**: `FontDataCacheKeyTraits::emptyValue()` hash empty-sentinel
  infinite loop (size-0 `FontPlatformData` got a live `m_scaledFont` → memset-0 slot never compared
  equal → linear probe spun forever). Fixed with the `if (size)` guard in `FontPlatformData.cpp:182`
  so the size-0 sentinel keeps `m_scaledFont` null. CJK resolves through a forced simhei bundled
  face (`stubs-font-uwp.cpp` `systemFallbackForCharacterCluster` + `apotheosisSetForcedBundledFontName`).
- Full 8-test trace suite passes (single/mixed/wan-meta/kana-n/br-kana/mixed-e/br-euro/cjk-b, all
  ALIVE + afterLoad=True; `gdc-> c=U+4E00 g=1078 ok=1`, `adv w=48.00`).
- appx v0.1.8.51 on Win11; `https://example.com` loads (`rc=0`, `title=Example Domain`).
- **GPU whole-content distortion FIXED** — implemented `WebCoreGpuResize(nativeWindow, w, h, outBuf)`
  in `WebCoreDriver.cpp` (declared in BOTH C ABI headers): the harness no longer bails on
  `m_gpuPresent` in `ApplyViewportSize` — it posts an engine job that recreates the GLContext/surface
  from a fresh `PropertySet` (`EGLRenderSurfaceSizeProperty = (w,h)`) + a fresh TextureMapper, then
  `finishInteractionPaint`. Tear-down order: old TextureMapper freed while old ctx current → old
  ctx/surface destroyed → new ctx created + current → new TextureMapper built. `g_gpuScrollFast=false`
  forces `forceDirtyTree` so backing stores regenerate. On recreate failure → Cairo SW fallback.
  Verified on x64: `ApplyViewportSize: engine rc=0` across many live resize cycles
  (1024→768→1081→1179→593→885→634×694), post-resize diag `contents=1024x694`, no crash, fixed
  200×200 test boxes stay square. Real-device (ARM32, D3D11 FL9_3) validation still pending.

### Next goal — ARM32↔x64 sync, then ARM32 rebuild
1. ✅ **Maximal ARM32↔x64 sync** (DONE Aug 16): ARM32 build dirs renamed `build-clang-*` →
   `build-arm32-*` (matches `build-x64-gpu` = `build-<arch>-<variant>`) across all port scripts
   (configure/compile/link/build-*) + the two x64 adapters (`compile-driver-x64.ps1`,
   `link-driver-x64.ps1`) + `harness-cmd.bat` + both vcxproj — zero-risk (old ARM32 tree was gone).
   Obj suffixes unified: GPU `.arm32.obj` / JIT `.arm32-jit.obj` / soft `.arm32-soft.obj`
   (x64 stays `.x64.obj`). `recompile-stubs-arm32.ps1` renamed → `recompile-stubs-x64.ps1`
   (it was an x64 script misnamed). Same appx version for both arches: already true —
   one shared `Package.appxmanifest`.
2. **Local ARM32 rebuild**: `configure-gpu-arm32.ps1` (`build-arm32-gpu`) → multi-hour
   `ninja -C build-arm32-gpu WebCore -j1` (4 GB box) → `link-driver-gpu-arm32.ps1` →
   msbuild harness `/p:Platform=ARM /p:AppxPackage=true`. No new porting work — the shared
   WK_WINUWP source carries the CJK + `WebCoreGpuResize` fixes automatically.
3. **Manual real-device validation**: deploy ARM32 appx to the Lumia 950 via `Deploy-Robust.ps1` +
   WDP; validate the site ladder + GPU; confirm the 200×200 squares stay square. Screen-width
   differences (L950 1440×2560 vs Lumia 640 720×1280) are a non-issue after the resize fix.

---

## 8.6. Current Goal (Aug 18, 2026): ARM32 WebCore.dll link - three root causes fixed, link relaunched

### Root causes found & fixed in source (full detail + transform table:
`Doc/2026-08-18-arm32-webcore-link-fixes.md`)
1. **x64-uwp lib paths leaked into the ARM link** - `WebCore/PlatformWinUWP.cmake:66-73`
   hardcoded `C:/vcpkg/installed/x64-uwp/lib/` for the 6 Cairo/vcpkg libs (jpeg, libpng16,
   libwebp, libwebpdemux, pixman-1, freetype). Fixed with a `CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM"`
   gate -> arm-uwp. (CMakeCache was NOT the source - verified clean.)
2. **webcore-exports.def carries x64 MSVC mangling** (x64 embeds an `E` marker: QEAA/AEBV/PEAV;
   ARM32 thiscall drops it: QAA/ABV/PAV). New `Src/port/webcore-exports-arm32.def` (31 entries);
   x64 DEF untouched; `PlatformWinUWP.cmake` picks the DEF per arch.
3. **FontCascade::fontForCombiningCharacterSequence undefined** - `USE_HARFBUZZ=ON` on ARM
   (OptionsWinUWP.cmake forces `HarfBuzz_FOUND TRUE` so `LocaleICU.cpp`'s `hb-icu.h` compiles)
   compiles out the generic impl (`FontCascade.cpp:1761-1773`, guard
   `!PLATFORM(COCOA) && !USE(HARFBUZZ)`); the USE(CAIRO) impl (`FontCairoHarfbuzzNG.cpp` via
   `platform/FreeType.cmake`) is not part of the UWP port. Stub added to `Src/port/stubs-font-uwp.cpp`
   (glyphDataForCharacter fallback, WK_WINUWP guard). x64 unaffected (HarfBuzz ICU NOTFOUND ->
   USE_HARFBUZZ=OFF -> generic branch active).

### State
- **ARM line fully built (Aug 18, 22:44)**: WebCore.dll linked (21:56, 32.8 MB, zero errors) ->
  WebCoreDriver-gpu.lib archived (12 objs, driver DLL step removed) -> **Harness_0.1.9.3_ARM.appx
  packaged (46 MB)** with fresh ARM binaries (WebCore.dll/JavaScriptCore.dll/ANGLE/vcpkg set
  verified inside). Full story + the five root causes: `Doc/2026-08-18-arm32-webcore-link-fixes.md`.
- The five root causes fixed in source: (1) x64-uwp lib paths in PlatformWinUWP.cmake (arch
  gate); (2) x64 MSVC mangling in webcore-exports.def (new webcore-exports-arm32.def); (3)
  USE_HARFBUZZ compiles out fontForCombiningCharacterSequence (stub in stubs-font-uwp.cpp); (4)
  obsolete driver DLL step referencing non-exported RenderTheme protected virtuals (archive-only
  now); (5) harness-cmd.bat carries -DBUILDING_WebCore making port data refs direct instead of
  __imp_ (strip added to compile-driver-gpu-arm32.ps1 - x64 script already had it).
- **Deployment pending (manual)**: Lumia 950 WDP at 192.168.3.51:443 was OFFLINE at session end.
  Deploy via `Src\tools\Deploy-Robust.ps1`/`Wdp-Deploy.ps1`, then check LocalState diagnostics.
  ICU resolved Aug 18: ARM is INTENTIONALLY on vcpkg ICU78 (engine imports icuuc78 -> icudt78
  with real data; Harness.vcxproj:159-164); the harness's icuuc75 LoadPackagedLibrary block is
  `if (hIcu)`-guarded and skips on ARM. x64 stays on ICU75 (stub icudt75.dll + icudt75l.dat
  injection) - both lines are self-consistent, no changes needed.
- Build recipes: WebCore via `run-webcore.ps1`/ninja -j1; driver via `link-driver-gpu-arm32.ps1`
  (self-derives env now); harness via `build-harness.ps1` with `APOTHEOSIS_ARCH=arm` +
  `APOTHEOSIS_BUILD_GPU=...\build-arm32-gpu\lib`. x64 line (appx 0.1.9.3) untouched.

---

## 9. Current Goal (Aug 11, 2026, later): "a real page loads over the network on x64"

### Blocker — root-caused, fix in the oven

Every load through `WebCoreSessionLoad` died with a deterministic `0xC0000005` on the engine
thread. `about:home` was immune only because `WebCoreRenderHtml` never enters `pumpLoop`. A
trivial local `file://` page crashed identically, so it was never content, network, or TLS.

Cause: `std::partial_ordering` has a user-provided constructor → Microsoft ABI returns it via a
hidden sret pointer, shifting both operands of
`WTF::operator<=>(const TimeWithDynamicClockType&, const TimeWithDynamicClockType&)` to RDX/R8.
clang-cl did not apply that consistently across WTF TUs inside the *same* `JavaScriptCore.dll`:
the out-of-line definition emitted the sret form (`0x8(%rdx)` / `0x8(%r8)`), while
`Condition::waitUntilUnchecked` emitted the register-return form (`RCX=&a`, `RDX=&b`, result in
`AL`, **R8 never set**). Mangled names are identical → the linker bound the mismatched pair
silently. The callee then dereferenced the raw `QueryPerformanceCounter` tick that the adjacent
`nowWithSameClock()` had left in R8. It needs `Condition::waitUntil` with a *finite* timeout,
i.e. `RunLoop::run()` in Drain mode with a scheduled timer — only `pumpLoop`, which had never
completed once in this app. Full detail in `Summary.md`.

Fix: `WK_WINUWP`-guarded **inline** definition in `wtf/TimeWithDynamicClockType.h`, out-of-line
copy in the `.cpp` guarded out. Call-site codegen changes and `WebCore.dll` *imports* the symbol
(2 refs — leaving it stale means the DLL won't load), so this costs
`ninja -C build-x64-gpu JavaScriptCore WebCore -j1` ≈ 1155 objects, ~5 obj/min, ≈ 3.5–4 h.

### Verification recipe (run in this order once ninja finishes)

1. `pwsh -File Src/port/link-driver-gpu-x64.ps1` — relink `WebCoreDriver-gpu.lib`.
2. Bump `Src/harness/Package.appxmanifest` (`Add-AppxPackage` refuses a same-version reinstall).
3. `msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true` — **no `MinimalTest`**.
4. `Remove-AppxPackage` then `Add-AppxPackage`. Removal **wipes LocalState**, so rewrite
   `settings.ini` and `test.html` *after* install.
5. Expect `twdct probe: compare ok` in `LocalState/port-trace.txt` — that line is the direct
   assertion that the ABI split is gone.
6. Ladder: local `file://test.html` → `http://example.com` → `https://example.com` →
   progressively HackerNews.

### Corrections to the documented build commands

| Documented | Reality on the x64 box |
|---|---|
| `link-driver-gpu-arm32.ps1`, `compile-driver-gpu-arm32.ps1` | ARM32-only — they hard-fail via `arm32-uwp-env.ps1`. Use the `*-x64.ps1` variants. |
| `/p:MinimalTest=true` (in CLAUDE.md) | Crash-isolation stub: compiles `MainPage.minimal.cpp`, excludes `MainPage.xaml.cpp`, links **no engine libs**. Real builds must leave it unset. |
| `DBG_STAGE` for driver tracing | Only reaches `OutputDebugStringA`, invisible inside the AppContainer. Use `WebCorePortTrace` → `LocalState/port-trace.txt` (append + flush per line, survives a hard AV). |

### UI state (the other half of the MVP ask)

`XAML: LoadComponent OK — real UI tree connected` is in `LocalState/log.txt` as of the 16:03 run,
so the code-only fallback is no longer in play — the real `MainPage.xaml` chrome (status bar
`TitleText`, URL capsule, tab button, ⋯ menu) is live. Two genuine gaps remain against the MVP
list: **Back/Forward and About are only reachable inside the ⋯ action sheet**, not on the bottom
bar. Sequence that after the page-load verification so an untested UI change can't confound it.

⚠️ `MainPage.xaml.cpp` carries a **hand-pasted copy** of the generated `InitializeComponent` /
`Connect` (lines ~3035-3573) — pasted so `LoadComponent` could be wrapped in try/catch, which a
generated file can't hold. Editing `MainPage.xaml` renumbers the XBF connection IDs, so the paste
must be re-synced or `x:Name` fields silently bind to the wrong elements. Verify sync with:

```bash
diff <(grep -oE 'case [0-9]+:|this->[A-Za-z]+ = safe_cast|MainPage::On[A-Za-z]+' "Src/harness/Generated Files/MainPage.g.hpp") \
     <(sed -n '3070,3573p' Src/harness/MainPage.xaml.cpp | grep -oE 'case [0-9]+:|this->[A-Za-z]+ = safe_cast|MainPage::On[A-Za-z]+')
```

Currently byte-identical, i.e. in sync.

### Disk

`build-x64-gpu/lib/WebCoreFull.lib` (3.21 GB) deleted — referenced nowhere in `build.ninja` (it
came from a one-off `WebCoreFull.rsp`) and nowhere in the harness link, since
`webcore-exports.def` now exports the four GPU/TextureMapper symbols that were its only reason to
exist. C: free 7.8 GB → 11 GB. Item 5 of the previous "Next objective" list is therefore ✅.
Keep `WebCore.pdb` / `JavaScriptCore.pdb`: live build outputs and required for
`llvm-symbolizer` forensics.

---

## 13. Aug 14, 2026 — Toolchain recovery done; objective = x64 first, ARM second

The dev box OS was reinstalled (Win11 x64, 4 GB RAM). Everything needed for the **x64-uwp debug
line** was rebuilt from scratch in one session; the full inventory, the OS-side RAM work, the
ARM32-MSVC-discovery and the failure log are in `Summary.md` §2026-08-14. Operational summary:

| Tool | Where it lives now (no `C:\` root litter) |
|------|---------------------------------------------|
| LLVM/CMake/Ninja | `C:\Program Files\LLVM`, `C:\Program Files\CMake`, ninja ×2 paths |
| Python 3.12.10 | `%LOCALAPPDATA%\Programs\Python\Python312` + junction `C:\Python312` |
| vcpkg x64-uwp (13 ports) | `C:\vcpkg` (icu excluded on purpose) |
| ICU75 + sqlite3.h | `C:\icu-x64-uwp` (import libs re-created from saved DLLs) |
| harfbuzz | `C:\vcpkg\installed\x64-uwp` (from preserved `deps-build`) |
| Ruby 3.4.10 + MSYS2 | `C:\tools\ruby34`, `C:\tools\msys64` (ucrt64 gcc) |
| pwsh / perl / ccache | vcpkg `downloads\tools` + `C:\Strawberry\c\bin\ccache.exe` (build.ninja legacy path) |

**Immediate objective (this sprint):** on the **x64 dev machine** ("emulator") get a fresh
`Harness.appx` that launches and renders **real sites** (not only `about:home`). The engine is
rebuilding now (`ninja -C build-x64-gpu WebCore -j1`, scheduled task `ApotheosisNinja`). Once the
DLLs are fresh the ladder is: link driver → bump appx version → `msbuild` harness → install →
`file://` → `http://` → `https://` → HN/dzen. 

**ARM32 (Lumia 950) is the second half of the objective** and is now unblocked at the toolchain
level: `Microsoft.VisualStudio.Component.VC.14.38.17.8.ARM` is installable from the VS Installer
catalog (no SDK 22621, no third-party MSVC); `arm32-uwp-env.ps1` already points at SDK 19041; Ruby
is in place for the JSC bytecode generator. Sequence: finish the x64 validation → install the ARM
toolset → port the stub-based link approach to `build-arm32-gpu`.

---

*Progress is measured in working DLLs. Milestone: `bin\WebCore.dll` (x64) ✅ → `Harness.exe` ✅ → Appx registered on Win 11 ✅ → AppContainer launch ✅ → `about:home` renders real text ✅ (Aug 11) → `partial_ordering` ABI split root-caused ✅ (Aug 11) → toolchain recovered after OS reinstall ✅ (Aug 14) → **engine rebuilding for real-site validation (in flight)** → ARM32 Lumia 950.*

---

## 10. Aug 11, 2026 (evening): the "8.5 hour rebuild" was not the header edit — the build tree had been moved

Fixing the `partial_ordering` ABI split touched one WTF header, and `ninja -C build-x64-gpu
JavaScriptCore WebCore` responded by scheduling **1528 steps** (~8.5 h at `-j1` on 2 cores). That
number is not a consequence of the edit. `ninja -d explain -n <one object>` — the single most
useful instrument here — showed the objects were dirty for reasons that had nothing to do with it,
and peeling them off one at a time exposed a **moved build tree**:

```
ninja explain: C:/Users/Admin/source/repos/!Vibe/Apotheosis/.../wtf/Platform.h is dirty
```

The tree was configured at `C:\Users\Admin\source\repos\!Vibe\Apotheosis` and now lives at
`C:\Users\media\source\repos\Vibe\Apotheosis`. `build.ninja` uses build-relative paths so
*compiling* still works, but `.ninja_deps` stores **absolute** header paths. Every one of them now
resolves to nothing, a missing dep counts as dirty, and so every object built before the move is
permanently dirty. Any `ninja` invocation in this tree therefore proposes a full rebuild forever.

### The dirty-reason ladder (each fix exposed the next)

| # | `-d explain` reason | Cause | Fix |
|---|---|---|---|
| 1 | old-path headers "is dirty" | `.ninja_deps` holds pre-move absolute paths | `New-Item -ItemType Junction 'C:\Users\Admin\source\repos\!Vibe' -Target 'C:\Users\media\source\repos\Vibe'` — old paths resolve again |
| 2 | `build-x64-gpu/WTF/Headers/wtf/TimeWithDynamicClockType.h` newer than object | CMake **copies** WTF headers into the build tree; that copy is a separate input | copy the edited header over it by hand, then `touch -r` a neighbour copy to back-date it |
| 3 | `cmake_pch.cxx.pch` newer than object | the PCH was regenerated, and it is an input to all ~1000 objects | none available — see below |
| 4 | *(self-inflicted)* "stored deps info out of date" for **all 1520** objects | I blanket-`touch`ed the objects to beat the PCH; ninja marks an output dirty when its mtime **exceeds** its recorded deps mtime | do not touch objects in a `deps = msvc` build |

Rung 3 has no mtime solution: back-dating the PCH below the objects makes the PCH itself older than
the July headers it includes, so ninja rebuilds it and it is new again. Deleting `.ninja_deps`
doesn't help either — a *missing* deps record is also "dirty", and the step count stayed at 1528.

### Conclusion: bypass ninja for provably-narrow work

Ninja's dirty set is an mtime approximation. When the affected object set has been established by
other means (here: a binary scan for the mangled symbol — see §9), drive the compiler directly.

- `ninja -t commands -s <target>` prints the **exact single command** for one edge, fully expanded.
  Strip `/showIncludes` (it exists only to feed ninja's dep scanner) and run it from the build root.
- Link edges use a response file that ninja writes at build time and deletes afterwards, so it must
  be reconstructed. `CMakeFiles/rules.ninja` gives the template — `rspfile_content = $in_newline
  $LINK_PATH $LINK_LIBRARIES` — so the content is the edge's explicit inputs (the `build ...:` line
  up to the `|` separator, one per line) followed by the edge's `LINK_LIBRARIES`. Unescape ninja's
  `$$` → `$`, `$ ` → space, `$:` → `:`; the paths here **do** contain both. `RSP_FILE` on the edge
  gives the path to write (`CMakeFiles/JavaScriptCore.rsp`, `CMakeFiles/WebCore.rsp`, build-root
  relative). Then run the link command from `ninja -t commands -s bin/WebCore.dll` unchanged.
- Validate the reconstructed rsp by checking every `.obj`/`.lib` it names exists **before** linking.
  That check is what caught the next two problems.

Result: **9 targeted objects + 9 stubs + 2 DLL links ≈ 35 min** instead of 8.5 h.

### Two more consequences of the move, both found by the rsp existence check

1. **Out-of-tree source objects were orphaned.** CMake mangles absolute source paths into the object
   tree as `WebCore.dir/C_/Users/<user>/...`, so after the move the WebCore link referenced
   `C_/Users/media/.../Src/port/stubs-*.cpp.obj` while the only objects on disk were under
   `C_/Users/Admin/...`. All 9 `stubs-*.cpp` had to be compiled. (`stubs-other.cpp` needed it anyway
   — the per-code-point glyph trace was removed there.)
2. **Some objects predate the current CMake configuration.** `PublicSuffixStoreCurl.cpp.obj` still
   referenced `psl_builtin` / `psl_is_public_suffix2` / `psl_registrable_domain`; libpsl appears
   nowhere in `build.ninja` or in `C:\vcpkg\installed\x64-uwp\lib`, so that object was built when
   `USE(LIBPSL)` was on. Recompiling it drops the references. Expect to iterate: relink, read the
   `lld-link: error: undefined symbol` list, recompile the objects it names, relink.

### Gotchas to remember

- `ccache` **is** in the compile line (`C:\Strawberry\c\bin\ccache.exe`), which is why the 9 objects
  finished far faster than the observed ~2 obj/min ninja rate.
- `taskkill` needs the **WINPID** from `ps -W` (4th column), not the Cygwin PID (1st).
- `TimeWithDynamicClockType.h`'s mtime is deliberately back-dated to `MonotonicTime.h`'s. If that
  header is edited again, remember both the source **and** the `build-x64-gpu/WTF/Headers` copy
  must be updated.
- The junction at `C:\Users\Admin\source\repos\!Vibe` is now load-bearing for `.ninja_deps` and for
  reading pre-move PDB/paths. Do not delete it. The real cure is a from-scratch reconfigure, which
  needs disk we don't have.

## 11. Aug 11, 2026 (evening, 2): pumpLoop completes — and the next crash, one frame later

The `ALWAYS_INLINE` `operator<=>` fix landed (9 objects + 9 stubs + 2 DLL relinks, ~35 min) and
the invariant verified: the mangled name appears in **0 of 1540** objects and in neither DLL's
import/export table. Result, appx 0.1.8.32, local `file:///test.html`:

```
twdct probe: compare ok, less=0
pump: timers armed, entering RunLoop::run()
settle: tick enter/isolatedUpdateRendering ok/microtaskCheckpoint ok/tick exit  x16
pump: RunLoop::run() returned          <-- first time ever
```

Then a **new** `0xC0000005 reading address 0x5` on the engine thread. Symbolized via
`llvm-pdbutil dump --section-contribs` (all three engine frames in `WebCoreDriver.x64.obj`) plus
`llvm-objdump` at the RVA. Faulting instruction:

```
movabsq $-0x1fffffffffffe, %rax   # 0xFFFE000000000002 == JSC::JSValue::NotCellMask
testq   %rax, %r14 ; jne <not-cell>
cmpb    $0x2, 0x5(%r14)           # 2 == JSType::StringType  -> AV on 0x5
```

**Root cause.** `evalJS` called `result.toWTFString(globalObject)` on the value returned by
`ScriptController::executeScriptInWorldIgnoringException`, which returns an **empty** `JSValue`
whenever `canExecuteScripts()` refuses or the script threw. `JSValue::isCell()` is
`!(bits & NotCellMask)` and the empty value is all-zero bits, so it answers *true*, `asCell()` is
null, and reading `JSCell::m_type` at offset 5 faults. `probeSpaModule` runs on every load, so
this fired the instant `pumpLoop` started returning. Fixed with `if (!result) return kErrNoDocument;`
plus `spa: probe rc=… kick='…'` tracing; `probeSpaModule` now also skips its 4 s import pump when
the probe never ran, instead of stalling every navigation for nothing.

**Rule of thumb this establishes:** an AV on a *tiny* address (`0x5`, `0x8`, `0x10`) is a real
member offset off a null base, not a wild pointer — and for JSC specifically it means an
unguarded empty `JSValue`.

Full triage playbook for the next session (or a smaller model): **`Doc/NEXT-STEPS-CRASH-TRIAGE.md`**
— the URL ladder, the diag-string field guide, the PDB symbolization recipe (offsets are
**decimal**), ranked hypotheses for real sites (TLS blob, `internetClient` capability, settle
heuristic, hit-test), and the do-not-break rules (ninja is dead here; a cold
`WebCoreDriver.cpp` compile is ~25 min on this 2-core/3.9 GB box).

**Verified 2026-08-12:** appx 0.1.8.33 logs `spa: probe rc=-6 kick=''` with **no `UEF:` block** and
the process alive — the `evalJS` guard holds. The local probe page painted
(`nonwhite=39657/777600`, `bodyKids=7`, Cyrillic included) and was confirmed on screen by the user.

---

## 12. The first real sites: they load, and they still show an empty window

`http://example.com` → `rc=0`, `title=Example Domain`, fully painted, **confirmed on screen**. The
first network request in this project's history. Then, with no further code change,
`https://news.ycombinator.com` → `rc=0`, `title=Hacker News`, `contents=720x2012`,
`res:[s.gif(s0) y18.svg(s0) news.css?…(s1)]`. So **TLS, curl, DNS, sockets, the AppContainer's
outbound capability and the packaged `cacert.pem` blob all work.** The entire §5.1/§5.2 hypothesis
block of the triage playbook — ranked first — was wrong, and is now marked RESOLVED there.

But HN rendered as a **blank window with two large up/down arrows** at the bottom right. Those
arrows are `ScrollFab` (`MainPage.xaml.cpp`); they appear when `sessionActive` is true, so they were
never a scrollbar or an artifact — **they were proof the navigation succeeded.**

### The trap in the diagnostics

`nonwhite` counts pixels whose RGB ≠ (255,255,255), and a cairo surface is created **zero-filled =
transparent black**, whose RGB is (0,0,0). So the metric read exactly backwards from the obvious
interpretation:

| site | diag | what it actually meant |
|---|---|---|
| dzen.ru | `nonwhite=0/777600` | paint ran and filled a **white** background; no content |
| HN | `nonwhite=777600/777600` | paint drew **nothing at all**; the surface was untouched |

"100 % non-white" looked like a triumph and was the opposite. Costly hour. A second red herring:
`glyph.log` was full of `fill: MISS i=… cp=U+6C30…`, which looked like a broken font backend. It is
not — the codepoints arrive in aligned runs of **16** because a WebKit `GlyphPage` is 16 glyphs wide
and the stub logs the whole page; these are ordinary fallback probes into CJK/symbol blocks that a
Latin font legitimately lacks, and **only misses are logged**, so healthy text produces no lines.

### Three independent root causes, one compile

All in `Src/port/WebCoreDriver.cpp`:

1. **JavaScript was never enabled on the session path.** `Settings::scriptEnabled` defaults to
   `false` in WebCore — WebKitGTK's `WebKitWebView` turns it on for its clients, but we assemble the
   `Page` by hand, and only the one-shot `WebCoreLoadUrl` called `setScriptEnabled(true)`.
   `buildSession`, the path the harness actually uses, never did. Diag showed `js=0/0`, and the
   second zero misled us for weeks: `canExecuteScripts()` reports false *as a consequence of*
   `scriptEnabled` being false, which is why the earlier `effectiveSandboxFlags = { }` fix — aimed
   squarely at that second zero — did not move it. A pure SPA like the configured home page
   `dzen.ru` therefore built a DOM and painted a blank white page, exactly as observed.
2. **We stopped pumping while WebCore was still refusing to paint.** `Document` carries an
   `OptionSet<VisualUpdatesPreventedReason>` (`Client`, `ReadyState`, `Suspension`,
   `RenderBlocking`), and while it is non-empty `RenderLayer::shouldSuppressPaintingLayer()` bails
   out of **the entire layer tree** — not a partial paint, *no* paint, not even a base background:

   ```cpp
   // Source/WebCore/rendering/RenderLayer.cpp
   if (!layer->renderer().document().visualUpdatesAllowed())
       return true;
   ```

   A render-blocking stylesheet in flight is precisely that state. On HN the main resource's
   `isLoadingInAPISense()` had already gone false while `news.css` was still pending, so
   `pumpLoop`'s quiet counter reached 16 (~0.8 s) and stopped roughly 3 s before WebCore would have
   drawn a single pixel. The reason clears when the sheet lands *or* when `Document`'s suppression
   timer fires at `settings().incrementalRenderingSuppressionTimeoutInSeconds()` — whose upstream
   default of **5 s is longer than our whole pump budget.**
   Fix: the settle tick now counts `!visualUpdatesAllowed()` as not-quiet (bounded by the existing
   `settleCapTicks`/watchdog), and `buildSession` lowers that timeout to 2 s, so a stalled
   stylesheet costs the *styling* and never the page. Deliberately **not** gated on `pending == 0`:
   HN shows images parked at status 0 that may never start, so that would stall every page to the
   watchdog. Images do not block painting; only render-blocking stylesheets do.
3. **The surface was handed over transparent.** With painting suppressed, WebCore never laid down a
   base background, so the harness blitted a fully transparent bitmap and the white `ContentArea`
   showed through — the "empty window". `paintToRGBA` now pre-fills opaque white, which is a
   browser's base canvas anyway and a no-op on any page that paints its own background.

Diag gained `vua=<0|1>` (`Document::visualUpdatesAllowed()`) and `sheets=<0|1>`
(`haveStylesheetsLoaded()`), because a blank window has three explanations that look identical on
screen — *nothing loaded*, *nothing executed*, *nothing painted* — and until now the diag string
could only distinguish the first. `vua` is the definitive tell; check it **before** suspecting the
renderer, Cairo, fonts or the GPU. Note this also changes `nonwhite=0`: it now means "white page"
*or* "nothing drawn", so `vua` is what separates them.

**Not yet verified:** the driver was mid-compile (~25 min, cold) when the session ended. §8 of
`Doc/NEXT-STEPS-CRASH-TRIAGE.md` is the exact checklist to confirm it: expect `js=1/1`, `vua=1`,
and a *middling* `nonwhite` on HN. If HN now crashes instead of painting, that is **progress, not
regression** — it would be the first time real page JavaScript has ever executed in this port, and
§3.1 (unguarded empty `JSValue`) is the known shape to check first.


---

### AI agents contributing

- **opencode** - https://opencode.ai · model deepseek-v4-flash-free (opencode/deepseek-v4-flash-free) · Aug 2026
- **Claude Opus 5** - (to be added)

---

## 14. After the MVP — the extended plan (written 2026-08-22)

**What counts as the MVP here:** a real modern page loads over the network on the Lumia 950, paints,
and responds to touch, without crashing. As of today the first three are demonstrated (ya.ru paints,
taps and popups work) and the fourth is not: `Doc/PUMPLOOP-SILENT-DEATH.md` is open.

Two platforms exist in this project and only two — **x64-UWP** and **ARM32-UWP**. Nothing here implies a
Linux or GTK target; the source tarball is called `webkitgtk` only because that is the release the whole
WebKit tree was cut for. `Doc/WIKI_EN.md` §11 explains that properly.

### Phase 0 — Prove the tree still builds. Do this before anything else.

This is first on purpose. Every other phase edits something, and today proved that an edit can cost a
day when the build directories have quietly drifted from their own sources.

1. **From scratch, both platforms, all three engine targets** — `WTF`, `JavaScriptCore`, `WebCore` — in
   a fresh directory, not the existing one. Not to get artefacts: to find out what no longer compiles.
   Today's evidence that this is not theoretical: `wtf/win/MemoryFootprintWin.cpp` had been in WTF's
   source list since 2026-08-18 and had **never once been compiled for ARM32**, because only WebCore was
   ever rebuilt. It cannot compile in an App Container at all.
2. **Make `build-arm32-gpu` reconfigurable.** Right now regenerating `build.ninja` produces a
   configuration that links shared Cairo, pulls in `user32/gdi32/msimg32` against the port's own stubs,
   and does not compile. Until this is fixed, no CMake file can be edited safely — which is a permanent
   tax on every future change. See CLAUDE.md and the memory entry
   `arm-build-dir-cannot-be-reconfigured`.
3. **One command that does both and reports.** `Src\tools\arm-bootstrap.ps1` is the model: every step in
   its own process, stop at the first failure, leave a verdict file naming the step and the last 40
   lines. An unattended window must end in progress or a diagnosis, never in silence.
4. **Then delete the one-offs that Phase 0 proves unnecessary** — starting with `C:\icu-x64-uwp`, which
   is still named by the CMake Cairo fallback branch and, until 2026-08-22, was the only place on the
   machine holding `sqlite3.h`.

### Phase 1 — Close the open defects, in this order

1. **The live-tick hang — CLOSED 2026-09-18.** It was not `CurlRequestScheduler`. It was Web
   Storage: the port installed no `StorageNamespaceProvider`, so `localStorage` was a silent no-op and
   dzen.ru's settings-sync module could never converge (`DZEN-SCROLL-DEATH.md` §10f). Confirmed by the
   real-gesture test on `0.1.9.110` — see items 0b/0c above. The engine still stalls briefly under a
   scroll (`finished` advances slowly, the known lazy-bytecode-compile stall), but it no longer wedges.
2. **Loading stalls — narrowed.** hh.ru's never-finishing load is root-caused and documented
   (`Doc/DEFERRED-SCRIPTS.md`); dzen.ru's blank page was measured **not** to be a lost load at all — every
   request starts and retires, and the failure is in engine-initiated navigation, not the network.
3. **Interaction quality** — one-finger scroll is far slower than pinch (pinch is a visual
   `ScaleTransform`, scroll goes through the engine), and zoom does not commit after a pinch.
4. **The load-completion criterion.** A load is declared finished while nine to twelve of its requests
   are still in flight — measured in every crash run. Whatever the hang turns out to be, that is a
   defect in its own right: the page is called ready before it stops loading.

### Phase 2 — Make the two build lines honestly comparable

The x64 line exists to make ARM32 debuggable in minutes instead of twenty. That argument only holds
where the lines agree, and every divergence has at some point made a measurement fail to transfer.
Closed on 2026-08-22: text shaping and ICU version (both now HarfBuzz + ICU 78.3 from vcpkg), the
`gpudefault=0` default on the bench (now `-Gpu`), and the input path (the bench never generated a
gesture — the dev machine has a touchscreen, so it can).

Still open, in descending order of how badly they mislead:

1. **JIT — this item was wrong and is closed.** Measured 2026-09-18: `build-x64-gpu\CMakeCache.txt` and
   `build-arm32-gpu\CMakeCache.txt` agree — `ENABLE_JIT=ON`, `ENABLE_DFG_JIT=OFF`, `ENABLE_FTL_JIT=OFF`,
   `ENABLE_C_LOOP=OFF`. Both lines run **LLInt + baseline JIT and nothing above them**, so the bench is
   not "interpreting where the device compiles". The difference that was observed once came from a
   runtime switch in the packaged `navseq.txt`, not from the build. No divergence remains here.
2. **Two Windows SDKs.** The harness is pinned to 19041 (the only SDK with ARM32 libraries); the x64
   engine line is still configured against 26100. Retiring 26100 is a Phase 0 task in disguise, because
   it means reconfiguring.
3. **`/Zi` in the x64 port objects only** — 41× larger objects, which is why an x64 relink is slow and
   why symbolising an x64 stack works at all. Harmless once understood.

### Phase 2.5 — The stub-driven capability gaps (added 2026-09-18 from `Doc/STUB-AUDIT.md`)

The audit the maintainer asked for found the gaps below. They are ordered by **what a user notices**, not
by how hard they are, and each carries an S/M/L. The point of the ordering is that the first two are the
difference between "renders pages" and "renders pages the way the page meant", and the last three are
invisible.

| # | Gap | Class | Effort |
|---|---|---|---|
| 1 | **CSS `font-family` is ignored.** `FontCache::createFontPlatformData` returns `nullptr` unconditionally; faces are selected by *filename* through a bundled-font override (`APOTHEOSIS_FONTS_DIR`). Text renders, but never in the requested family. | missing capability | **L** |
| 2 | **No webfonts and no `@font-face`.** `FontCustomPlatformData::supportsFormat` is `false` for every format and `create()` returns `nullptr`. Icon fonts — which many sites use for every glyph of their UI — render as nothing. | missing capability | **M** |
| 3 | **Screen metrics are a hardcoded lie:** `screenRect` says 1080×1920 while the window is 1024×694, so `window.screen.*` and `@media (device-width/height)` answer for a phone that is not running. The constants also exist **twice** (`stubs-screen-uwp.cpp` and `webcore-driver-stubs.cpp`). | **silent lie** | **S** |
| 4 | **Modifier keys are never known.** `PlatformKeyboardEvent::currentStateOfModifierKeys()` returns `{ }`, so Shift/Ctrl/Alt are always "not held" — shift-click, ctrl+A and modifier-aware input silently degrade. | **silent lie** | **S** |
| 5 | **Clipboard does not exist.** Reads return empty; writes `RELEASE_ASSERT_NOT_REACHED()`. Verify the write path is unreachable before shipping a build where a paste could abort. | split: silent reads / loud writes | **M** |
| 6 | Accessibility is a total zero — nine no-ops. Not a bug; a product gap that needs a UIA provider in the harness, not a change in the port. | never reached | **L** |

Three further items belong on the **test** list rather than in this table, because they are unverified and
the measurement is cheap (§4.8 of the audit): which `RenderTheme`/`ScrollbarTheme` singleton actually wins
against Adwaita (themed vs unthemed form controls), whether `ComplexTextController`'s empty body is what
runs (complex scripts), and whether the double compilation of six stub files into both `WebCore.dll` and
the driver archive is worth collapsing (it is duplicated build time in a `ninja -j1` build).

Phase 0 of this plan already made the general point and this audit sharpens it: **a stub is where
unfinished work hides, and the ones that cost the most are the ones that answer "success".** Three such
answers were found and repaired inside `stubs-other.cpp` alone before this audit existed — an empty
`MainThreadSharedTimer` that stopped every WebCore timer in the port, a `GlyphPage::fill` that returned
false so no character ever mapped to a glyph, and a `FontCustomPlatformData` that advertised font formats
it did not have.

### Phase 3 — Retire the accumulated one-offs

`Doc/UNIFICATION.md` carries the measurements behind each of these; read it before re-deriving any.

1. The dead 3.49 GB `WebCoreFull.lib`.
2. The duplicated static Cairo.
3. `/FORCE:MULTIPLE` — every use of it hides a real duplicate-symbol question.
4. The remaining hardcoded absolute paths outside `$env:APOTHEOSIS_ROOT`.

### Phase 4 — Sharing the work

`Doc/SHARING-AND-OPEN-SOURCE.md` has the plan. Gated on Phase 1: publishing a browser that hangs on
the second navigation would waste the goodwill of the few people who care about this device. Note the
existing plan policy in §8 — GitHub automation is explicitly optional and must never become a blocking
milestone.

### Phase 5 — Harvest from Revenant

`Doc/REVENANT-COMPARISON.md` lists what to take and what to skip. Gated on stability for the same
reason as Phase 4.

### What this plan deliberately does not contain

- **No new platforms.** Not Linux, not GTK, not ARM64. Two targets, both UWP.
- **No cloud CI.** Per §8: local builds and local verification first, forever.
- **No feature work while a hang is open.** A browser that renders more sites and still dies on the
  second navigation is further from an MVP, not closer.

### Phase 0 progress, 2026-09-03 — the x64 half is done

**Item 1, x64 side: COMPLETE.** The x64 line was rebuilt end to end after 37 ARM-only builds
(.41 – .77): engine 376/376 clean, driver relinked, appx built, installed and run. This is exactly the
audit Phase 0 asks for, and it paid immediately — three latent breakages surfaced, all of them the same
class: *code that had never been compiled for the architecture it claimed to support.* Details in
`Doc/HARFBUZZ-ICU-DIVERGENCE.md`, postscript; the durable rules went into CLAUDE.md.

**Item 1, ARM side: still open.** ARM was last built on 2026-08-25 and has not been re-verified since
the fixes below. The changes are architecture-neutral by construction, but that claim is untested on
ARM and must not be reported as verified.

**Item 2 (make `build-arm32-gpu` reconfigurable): untouched, and now the main structural debt.** Every
x64 fix below could be made without touching a CMake file; the moment one is needed, the ARM tree still
breaks.

**What was fixed while doing this** — all of it applies to both architectures:

| Defect | Consequence before the fix |
|---|---|
| Wedge-dump unwinder and register dump used ARM `CONTEXT` members with no arch split | the harness did not compile for x64 at all |
| `VerifyXamlConnect` hooked to `MarkupCompilePass1` but reads a Pass2 output | after a Clean, the guard blocked the build that would have restored the file |
| Driver took the direct-present branch whenever a native window had once been passed | frames presented into a *collapsed* panel; a fully loaded page in a white window, no error |
| `gpuCompositeReadback` returned `kOK` even for an empty composite | white buffer reported as a delivered frame, suppressing the Cairo fallback |
| Chrome client installed only `if (g_gpuActive)` | sessions built before `WebCoreGpuInit` could never composite; the probe read -12 and latched to software for the whole process |
| Image-list walk capped at `0x80000000`, 64 MB attribution window, addresses printed `(unsigned)` | JSC frames attributed to WebCore and symbolised against the wrong DLL — a full wrong diagnosis, retracted |

**Verified, with the measurement:** a page forcing a layer (`will-change: transform` + `translateZ(0)`)
gives `EnableCompositing=1 Composite=0`, `nonwhite=710656/710656`, no readback-failure marker; the
per-URL GPU re-probe fires twice in one process; and the first dump from the repaired tooling resolves
cleanly to `WebEngine::loop` → the navigation job → `condition_variable::wait_until`, which is the 2.5 s
background-fetch wait, not a hang.

**Still open on x64, in priority order:**

1. **`dzen.ru` ends the process.** The one wedge dump taken after these fixes was on `dzen.ru`, and the
   process was gone afterwards. `dzen.ru` was already on the open list as a loading stall
   (`rs=I pending=31`), so this is very likely that defect and not a new one — but it is the next thing
   to reproduce deliberately, with the repaired dumper, on a page that is *not* already known-broken.
2. **Nothing has been confirmed by eye.** Every paint result here is a counter in a log. A counter
   proves the frame reached the buffer, not that it reached the screen — a distinction this
   investigation has already been caught by once.
3. **`Composite=-4` on ordinary pages is expected, not a defect.** `ya.ru` at 1024×694 genuinely has no
   compositing layer, so the GPU path correctly declines. Do not read -4 as a failure.
