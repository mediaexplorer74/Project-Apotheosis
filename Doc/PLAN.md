# Project Apotheosis — Development Plan

> **This is the current-state view.** It holds the live status, the v1.0 definition, and the open
> work. Every closed item, dated snapshot, investigation narrative and July-Aug phase log was moved
> verbatim to [Archive/PLAN-HISTORY.md](Archive/PLAN-HISTORY.md) — nothing was deleted. Old cross-references of the
> form `PLAN.md §...` resolve through the index below; each names where the section now lives.
> Last reorganised 2026-09-25.

---

## Section index — where old `PLAN.md §...` references went

Other documents cite this file by section. This table maps every such reference to its new home, so
no inbound link goes dead.

| Old reference | Status | Where it is now |
|---|---|---|
| §0 (v1.0 definition) | **live** | kept below — "What v1.0 is" |
| §0a (termination swallowed) | closed/downdegraded | PLAN-HISTORY.md |
| §0b (runaway: whose?) | answered | PLAN-HISTORY.md — Sep 18 evening snapshot |
| §0c (current status, memory front) | **live** | kept below |
| §0c-addendum (0m, 0n) | **live** | kept below |
| §0d (stubs audit) | done | PLAN-HISTORY.md — verdict also in Doc/STUB-AUDIT.md |
| §0e (two failed taps) | diagnosed | PLAN-HISTORY.md |
| §0f (destroyed strings) | rewritten | PLAN-HISTORY.md — also Doc/DESTROYED-STRINGS.md |
| §0g (watchdog vs budget) | fixed | PLAN-HISTORY.md |
| §0h (null font family) | fixed | PLAN-HISTORY.md — also Doc/FONT-NULL-FAMILY-CRASH.md |
| §0i (CalculationValue handle) | contained | PLAN-HISTORY.md — also Doc/CALC-HANDLE-DANGLING.md |
| §0j (bounded memory) | **LIVE** | kept below |
| §0k (nav-cancel arch fix) | deferred | kept below |
| §0l (no subframes) | open | kept below |
| §0m (tap latency) | **live** | kept below |
| §0n (habr body stall) | open | kept below |
| §0o (startup AV) | closed (.55) | PLAN-HISTORY.md |
| §0p (0n fix, unmeasured) | pending measure | kept below |
| §0q (error page inert) | fixed (.56) | PLAN-HISTORY.md — also Doc/error-page notes in 0q |
| §0r (lenta readyState) | refuted | PLAN-HISTORY.md |
| §0s (stale diag string) | **live** | kept below |
| dzen blank-white-page (SSO loop) | superseded by scroll-death work | PLAN-HISTORY.md — also Doc/DZEN-SCROLL-DEATH.md |
| NEXT PHASE (x64 rebuild) | note | PLAN-HISTORY.md |
| §8.5, §10, §12, §13, §14 / Phase 0-5 | historical | PLAN-HISTORY.md |
| numbered next-steps 1-5 (2026-09-18) | see below | kept below |

Cross-references *out* of this file were not edited; they still say `PLAN.md §...` and this index
is how the next reader follows them.

---

## 0. Current state and what v1.0 is

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

---

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


---

## Next steps — open items only

Closed items (0a-0i, 0o, 0q, 0r) keep their full narrative in PLAN-HISTORY.md; their verdicts are
summarised in the index above. The items below are the ones still carrying open work.

**0t. (NEW, 2026-09-25) Move the x64 *engine* to SDK 19041, then drop every 26100 mention.** The
harness is already on 19041 for both architectures and builds clean (`Harness.vcxproj` pins
`10.0.19041.0`; verified by a full Release/x64 appx build on 2026-09-25). The x64 **engine**
(`build-x64-gpu`) is still configured against 10.0.26100.0 in `Src/setenv.ps1:23`,
`Src/port/x64-uwp-env.ps1:11`, `Src/port/vcpkg-triplets/x64-uwp-toolchain.cmake:10`
(`CMAKE_MT`) and `x64-uwp.cmake`. The maintainer wants 26100 uninstalled entirely, so the SDK
split must die for real rather than being documented around. This is a *reconfigure + full
`ninja -j1` rebuild* (hours), and it is the gate for removing the ~51 `26100` mentions from the
docs without leaving a false claim behind. Blocked on nothing, but it is NOT urgent: the MVP pain
(0u) is fixed independently and must reach the device first. Do not start this while the maintainer
is reproducing on the device. Measurements: `Doc/UNIFICATION.md` Track B / B5.

**0u. (NEW, 2026-09-25) MVP UI: address-bar unblocked via runtime-only guards — DONE, verify on
device.** The v1 MVP is the address bar, Back, and the page. Two defects made the shipped UI
unusable and were fixed WITHOUT touching XAML or the generated `MainPage.g.hpp` (whose `Connect()`
id contract cannot be regenerated — see 0v), so the build stays green:
* `FindBar` and `SuggestPanel` are the only two overlay blocks whose XAML default is `Visible`
  (they carry no `Visibility="Collapsed"`), so they covered the content and the URL area until a
  fragile code path collapsed them. The constructor now forces both to `Collapsed`.
* The four full-screen overlays (action sheet, drawer = History/Favorites/Downloads, Settings,
  tab switcher) each open only to a half-working feature and their scrims swallowed stray taps;
  their `Show*` functions now return immediately. The ⋯ / tabs buttons remain in the XAML but do
  nothing until the XAML pass (0v) deletes them.
Result: Release/x64 `0.1.10.61` builds clean, `xaml/Connect in sync: 61/88`. **Next action is the
maintainer's**: install in-place (manifest bump + `Add-AppxPackage`, no `Remove-AppxPackage` — see
CLAUDE.md) and confirm on the device that the URL bar takes input, Back works, and no overlay
pops over the page.

**0v. (NEW, 2026-09-25) The clean UI pass is blocked on XAML-glue regeneration — get a minimal
repro first.** Deleting the 45 dead buttons from `MainPage.xaml` (the "ascetic UI") requires the
XAML compiler to regenerate `Generated Files\MainPage.g.hpp`: it renumbers every `Connect()` id
when any `x:Name` or handler is added/removed, and a stale id binds a field to the wrong control or
leaves it null — a null `^` in C++/CX is a raw dereference `catch (...)` cannot catch (it killed
startup 0.1.8.64–0.1.8.67). The project treats `g.hpp` as a build input and `verify-xaml-connect.ps1`
blocks any drift; empirically it does not regenerate (even an untouched XAML fails once `g.hpp` is
deleted — `WMC9999` / `MSB4181`). This is a build-toolchain problem, NOT a platform law, and it
has never been isolated in a minimal test. **Do this after 0u ships.** Method: build a throwaway
C++/CX UWP page (one Button + one Click handler) against SDK 19041, change the XAML, and see
whether `g.hpp` regenerates; if it does, the blocker is this project's configuration, not the
compiler. Reference: `Doc/UNIFICATION.md` §C, `verify-xaml-connect.ps1` header, CLAUDE.md
"Never freeze XAML-generated code".

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
     belongs beside the other gaps classified in [Archive/STUB-AUDIT.md](Archive/STUB-AUDIT.md) — it is the same class as
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
     ([Archive/TOPLEVEL-FETCH-BUDGET.md](Archive/TOPLEVEL-FETCH-BUDGET.md)). It fires only on that exact triple, and it
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

### Numbered next steps agreed 2026-09-18 (still open)

2. **ARM32 build line** — once x64 is stable, rebuild ARM32 engine for Lumia 950
   (needs 20-25 GB disk; we have ~38 GB).
3. **Desktop UA test** — `ua=1` in settings.ini may fix dzen.ru white margins (mobile layout).
4. **Independent rendering/JS tests** — separate GPU rendering verification from JS execution.
5. **Git optional** — per owner's decision; no Git work required.

Item 1 (continue crash verification) is answered by §0i/CalculationValue work — see Archive/PLAN-HISTORY.md.

## C# harness probes — managed DLL boundary (2026-09-25)

### Decision

The C# harness is an experiment, not the shipping browser. It must not be wired to WebKit static
archives by assumption. The measured and supported boundary is:

```text
C# UWP app → WebCoreDriver.dll → WebCore.dll / JavaScriptCore.dll
```

The C++/CX harness uses `WebCoreDriver-gpu-{arch}.lib`, `WebCore.lib`, `JavaScriptCore.lib` and other
native static/import libraries because it is a native executable. That static-link arrangement is not a
managed-code build pattern. `WebCoreFull.lib` is a special native workaround archive for unresolved
WebCore internals and is not a C# reference. C# must call the stable C ABI through P/Invoke and receive
`WebCoreDriver.dll` plus the WebKit runtime DLLs as package content.

### Probe trees

Use separate trees and build them sequentially, never as concurrent full WebKit builds on this 4 GB
machine:

```text
build-x64-19041-probe   active: SDK 19041 CMake/Ninja compatibility check
build-csharp-x64-probe  later: C# x64 UWP + DLL boundary/package check
build-csharp-arm32-probe later: C# ARM32 UWP/.NET Native question
```

The C# probes must not modify `build-x64-gpu`, `build-arm32-gpu`, or the production `Harness.vcxproj`.
A probe is allowed to create a small temporary copy or a dedicated output directory. Do not add
`WebCore.lib`, `JavaScriptCore.lib`, or `WebCoreFull.lib` to `Apotheosis.csproj`.

### Order of work

1. Let the active x64 SDK 19041 CMake/Ninja build finish and record its result.
2. Confirm the expected x64 outputs: `WebCore.dll`, `JavaScriptCore.dll`, and the static libraries used by
   the native harness.
3. Build the C# x64 UWP package with SDK 19041 and the existing `DllImport` declarations. If a driver DLL
   is not available, use a small dummy C ABI DLL for the packaging/PInvoke probe; do not fake a successful
   WebKit result.
4. Verify package identity, architecture, DLL placement, and a harmless native round-trip call.
5. Only then build the C# ARM32 probe. The first ARM32 question is whether VS 2022/.NET Native can
   produce and install the UWP package at all; this is not yet measured.
6. Port engine-thread plumbing, job queue, heartbeat, diagnostics, and navigation state only after the
   managed DLL boundary is proven. A C# UI alone is not a browser MVP.

### Stop conditions

Stop the C# probe and record the exact failure if:

- C# UWP cannot target SDK 19041 with the installed VS 2022 components;
- ARM32 `.NET Native` cannot produce a package;
- AppContainer rejects `DllImport` or native DLL deployment;
- a driver DLL cannot be made available with the correct architecture and signing/package identity.

Do not respond by copying native `.lib` files into the C# project, by making a desktop process masquerade
as UWP, or by claiming a runtime success from a compile-only probe.

### Honest status

`Src/Apotheosis/README.md` remains the source of truth for the C# experiment. The shipping path remains
`Src/harness/Harness.vcxproj`; the C# work is optional exploration and must not block the x64/ARM32 MVP.
