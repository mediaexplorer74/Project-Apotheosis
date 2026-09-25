# Project Apotheosis — Engineering Research Summary

> Compiled by an AI code agent during deep-dive sessions (June 28 - July 20, 2026).
> Original repo: [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
> Reddit thread: [r/windowsphone — Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)

## 0a. Current status — Sep 18, 2026 (morning)

> Current evidence: [STATUS-2026-09-18.md](STATUS-2026-09-18.md) (renamed from
> STATUS-2026-09-17.md). All older status sections below are historical snapshots, not
> validation of the current package.

- **x64: `0.1.9.97` built Release and installed in-place** (`Add-AppxPackage` update, no
  `Remove-AppxPackage`; LocalState backed up). Built from .96 sources + PE32/PE32+ parser
  fix in `ApoReadModuleName` (watchdog diagnostic). Harness.exe SHA256 matches build output.
- **Runtime verification (2026-09-18 05:50–06:30):** navseq completed all 8 URLs without
  crash. example.com ×2, Hacker News, layertest.html, ya.ru all rendered correctly
  (`rc=0`, nonwhite >0, `rs=C`). **ya.ru rendered a full page with images** (78 scripts,
  nonwhite=234723). dzen.ru redirected to SSO login (`sso.dzen.ru/install`) — site requires
  login, not a browser bug.
- **CSS crash `0xc0000005 @ WebCore.dll+0x204901f` did NOT reproduce** on .97 for any tested
  site. The .96 guard (stale `CalculationValue` handle check) remains a candidate fix;
  mechanism not proven.
- **GPU readback:** fail-closed guard verified (78 `rb: no live GL context` messages,
  0 readPixels). Cairo fallback renders correctly. Compositing visible on layertest.
- **Process stability:** PID 10648 survived ~40 minutes of active navigation (5+ page loads,
  window resize). No crash events in Event Log.
- **Memory/disk:** laptop has 4 GB RAM (not 12 as assumed); memory pressure real. Disk freed
  ~10.7 GB (old AppX cleanup + temp caches + hibernation off). Free: ~38 GB.
- ⚠️ **Current `x64-cycle.ps1` REMOVES the installed package** despite its header — do not use it
  for a data-preserving update; use manifest bump + `Add-AppxPackage` in-place, as done today.

---

## Status update — Sep 24, 2026 22:00 — the startup death: mechanism proved, fixed, and reproduced on demand

> Written after build `.55`, i.e. five builds after `.50`. **This supersedes 0b's framing for the MVP:**
> 0b calls the active defect "unbounded RSS growth across navigations", and that is still true as a
> measurement — but between `.51` and `.55` the app **failed to start at all in roughly three launches
> in ten**, which is the acceptance criterion ("responds to touch without crashing") failing outright.
> Fixed in `.52`/`.55`; the RSS work is untouched and still open below.

### The defect

`.51` wired the UWP memory events to the engine, making `releaseMemory()` reachable for the first time
(section 0b records that it had **no caller at all** while a port comment claimed the harness called it —
the SILENT LIE class). The wiring introduced a startup crash, and **it was introduced by us, today**.

### The mechanism, in four independent instruments

`AppMemoryUsageIncreased` fires **at handler registration** on some launches — the platform reports its
first reading of a fresh process as an upward crossing into `low`, the *lowest* level. The handler posted
a `mem-release` job, and that job was then the engine thread's **first WebCore call of the process**,
ahead of any navigation and ahead of process init:

```
21:30:59.368  mem-level UP: mem 35216/12497952 KB level=low   <- handler posts; UI thread
21:30:59.430  WebEngine: loop ready, waiting for jobs          <- 62 ms LATER
21:30:59.433  VEH: code=0xC0000005 tid=8312 faultaddr=0x8      <- the ENGINE thread dies
```

`tid=8312` is not the logging thread (`11076`), so the fault is inside the job, not the handler. The
wedge dumper agrees from outside, in the *next* process's verdict line: `CRASHED … finished=1
job=mem-release tickstep=0 stage=(none)` — the engine had completed exactly one job and was inside
`mem-release`. `stage=(none)` because `WebCoreReleaseMemory` writes no stage marker.

The faulting frames name the rest:

| frame | symbol |
|---|---|
| `JSC+da0ab0` | `WTF::MemoryPressureHandler::MemoryPressureHandler()` — `MemoryPressureHandler.cpp:75` |
| `JSC+da0a02` | `WTF::MemoryPressureHandler::isUnderMemoryPressure()` — `MemoryPressureHandler.h:125` |
| `WebCore+199fad2` | `WebCore::releaseCriticalMemory()` — `MemoryRelease.cpp:122` |
| `Harness+b805e` | the `mem-release` job lambda |

The Windows constructor's member initialiser is `m_windowsMeasurementTimer(RunLoop::mainSingleton(), …)`.
`RunLoop::mainSingleton()` is `{ ASSERT(s_mainRunLoop); return *s_mainRunLoop; }` and **the ASSERT
compiles out in release**, so with `s_mainRunLoop` still null it returns a null reference and the
`Ref<RunLoop>` refcount increment faults reading offset 8 — the logged `faultaddr=0x8`, exactly.
`s_mainRunLoop` is set by `WTF::initializeMainThread()`, which lives in `ensureWebCoreInitialized()`.

**The one-line statement of the bug:** `WebCoreReleaseMemory` was the only ABI entry point that touched
WebCore without first initialising the process. The other four (`WebCoreRenderHtml`, `WebCoreLoadUrl`,
`WebCoreSessionLoad`, `WebCoreGpuInit`) all open with `ensureWebCoreInitialized()`. It could not bite
before `.51` because nothing called it.

### The fix — both halves, and why neither is the other's substitute

| where | change |
|---|---|
| `Src/port/WebCoreDriver.cpp` | `WebCoreReleaseMemory` now calls `ensureWebCoreInitialized()` before `releaseMemory`, with a `[SL] releaseMemory: WebCore initialised` marker. This is the **architectural** half: it removes the hazard for *any* release job that arrives before init, including a genuine `medium` event raised before the loop is ready |
| `Src/harness/MainPage.xaml.cpp` | the `AppMemoryUsageIncreased` handler releases only at `Medium` and above (`MemoryLevelIsPressure()`), and logs **both** decisions — `mem-release: posting` or `mem-release: skipped -- level below medium …`. This is the **correct-semantics** half: `low` is the platform's resting state, and releasing there costs re-decodes the Lumia cannot spare — the same argument the `LimitChanging` handler already made for ignoring a ceiling *rise* |

### The verification, and its honest ceiling

The platform fires its startup event only *sometimes* (3 launches in ~10 in the dying series, then **20
consecutive launches with no event at all**), so a green sweep proves nothing on its own. The race is
therefore **reproduced on demand**: `LocalState\mem-release-race.txt` makes the harness post the release
job at handler registration — 45 ms before `loop ready`, the exact position the dying launches had, and
`startup-soak.ps1` reports per launch whether that arm actually fired, refusing to let a sweep that never
armed the trigger be read as evidence.

- **9 armed launches** (job posted before `loop ready`, as the process's first job): 9/9 alive, zero
  `0xC0000005` in either channel. The driver's own markers put the job first and the init inside it:
  `1 SL: releaseMemory (memory-pressure event) critical=1` → `2 PS: install has=0 didInstall=1` →
  `4 [SL] releaseMemory: WebCore initialised` → `5 [GPU] enter`.
- **20 unarmed launches**: 20/20 alive (the trigger did not fire, so these say nothing about the race).
- **What is not proved:** the platform's own event has not been observed firing *after* the fix — only
  the hand-armed equivalent of it. The arm posts the same job at the same point through the same queue,
  so it exercises the same path; but it is a reproduction, not the original trigger firing.

### A method warning, paid for three times in one tool

`startup-soak.ps1` gave three wrong counts before it was right, all of the same shape — **a counter that
does not measure what its label says**: it counted `mem-release`, which matches its own arming probe's
line `mem-release-race: checked, armed=0` and so reported a release on every launch of a build where the
event never fired; then the arming field was not copied into the per-run object, so the summary read
`$null` while the per-run column said `1`; then the summary kept the old field name and read 0/6 with the
column reading 1/1. What caught all three was that **two displays of the same run disagreed**. That is the
cheapest invariant available and it is worth building in deliberately.

### Open, and next

- **The stall** (0b's RSS work is separate): `habr.com/ru/feed/` intermittently never builds a body —
  one host at its 6-in-flight cap with 65 queued starves the render-blocking `light-v2.css`. The fix is
  implemented and compiles (PLAN 0p) but is **not yet measured**, and its periodic wake-up is suspect —
  see PLAN 0p: the timer is armed on `RunLoop::mainSingleton()`, which is the *engine* thread's run loop
  and is pumped from inside a load job, while the stall is precisely the case where that job has ended.
- **ARM32 has none of today's work**: neither the memory handler, nor the instrument set, nor this fix
  has been built for the device line or measured on the Lumia.

---


## Status update — Sep 24, 2026 22:15 — the error page: the port showed it, and the user could do nothing with it (`.56`)

Same evening as the startup fix, and it came out of asking a question the earlier passes over the same
code never asked: **what can the user actually DO with the page we show when a site is unreachable?**
The answer was nothing, and one premise in the process turned out to be wrong.

- **A claim made hours earlier in this session was falsified.** "A failed navigation leaves the previous
  page on screen and no error page appears" was inferred from `title=Hacker News` on the `nav: FAIL`
  line plus the absence of an `empty page:` notice. Both are explained otherwise: the title is printed
  **before** `MakeErrorHtml` is rendered (so it names the *previous* document by construction), and the
  notice is guarded by `emptyPage`, a different condition computed from the old document's diag.
  `WebCoreRenderHtml` returns **0** on the failure path, so `ok = (rc == 0)` is true and the error page
  is blitted exactly like a successful load. Measured on `.56`:
  `error page: rendered url=https://habr.com/ru/feed/ rc=0 blit=yes`.
  **The lesson is the same shape as `WEDGE` and `body=?`:** read the *outcome* field
  (`WE-job:post-try rc=0`), never a descriptive one, before concluding that a branch did not run.
- **The real defect: the error page was fully inert.** On failure the harness sets
  `sessionActive = false`, so `m_sessionActive` goes false — `HandleTapAt` then uses its extracted
  link table instead of `ForwardClickToEngine`, and every scroll/pinch handler early-returns. And
  `MakeErrorHtml` emitted **no** `<a href>` at all, while `extractLinks` publishes only those (and only
  HTTP-family ones). So the table was empty: a full-screen "Could not load the page" that answered no
  tap and no swipe, address bar the only exit. On the phone that reads as "the browser died" — the MVP
  criterion failing visibly.
- **Fixed in `.56`:** a "Try again" anchor to the **failed** url (not the current document's — the
  engine is still on the previous page, so retrying "where we are" reloads what was already there),
  sized so `boundingClientRect()` is non-zero and `extractLinks` keeps it; `HtmlEscape` on both
  interpolated strings (`err` embeds the requested url, and `?a=1&b=2` was being parsed as an entity
  reference); and a log line, because the render result had never been recorded.
- **Verified:** `simtap: taplinkstr -> link #0 rect=(24,308 158x65) dip=(103.0,340.5)
  url=https://habr.com/ru/feed/` → `OnPageTapped: … session=0 links=1 sim=1` → `NavigateTo:` — through
  the same dispatcher a finger uses. The `&` round-trips (extracted href is the literal original url).
  Four failure cycles, process alive, no `VEH:`, no `WEDGE`.
- **Still open, and it is the architectural half:** the error page is drawn through
  `WebCoreRenderHtml`, which builds a **throwaway `Page`** and returns pixels, so the resident session
  keeps the previous document and `diag:` / `GetTitle` / `GetFrameHash` describe a page that is not on
  screen. One document, one session — feeding the error HTML into the *resident* session — means an ABI
  addition in **both** `WebCoreDriver.h` copies. See PLAN item **0q**.

## 0b. Current status — Sep 24, 2026 (the memory front)

> The active defect is now **unbounded RSS growth across navigations**, not a crash. Live plan and the
> ordered next steps: [PLAN.md](PLAN.md) sections **0c**, **0j** (memory) and **0l** (subframes). This
> section records what was measured; it deliberately records the falsified explanations too, because six
> of them looked right. **Last updated after build `.50`.** `.49` promoted a fix on a single outlier and
> `.50` reverted it, which is why the "**Withdrawn**" material below is the part to read first — and why
> the bench is, for the first time since 2026-09-24 19:45, back in its **default** configuration.
>
> **Priority note (added 2026-09-24 22:00):** between `.51` and `.55` the app also failed to *start* in
> roughly three launches in ten, which outranks the RSS work for the MVP — that is the newer section
> above, now proved and fixed. Nothing in this section is retracted or superseded by it; the growth
> measurements below are untouched by the startup fix.

**WITHDRAWN — read this before the narrative below, which still contains two of the claims.** Everything
in this block was published as a finding and does not survive a controlled re-measurement. It is kept
because a withdrawn claim that is not written down is a claim the next agent re-makes.

| withdrawn claim | what actually happened |
|---|---|
| "the accumulator is **named**: decoded image data" (`.47`/`.48`) | the naming rested on **one outlier**: an armed window whose *first* delta was −125 MB, averaged with +37, +51, +55 down to "flat". A process does not shrink 125 MB because a cache was evicted. Re-measured in a fresh process the same arm reads 97 MB/load |
| the decoded lever's **−57 %**, eviction's **−58 %** | both came from the same two-window method with the disarmed window taken on a **fresh** process (deltas 242, 157, 168 = warm-up) and the armed window on a warm one. The warm-up alone produces the whole effect |
| "the **44–58 MB/load floor** both levers leave behind" | the residual of that comparison. There is no floor |
| the decoded lever's **+7 % load-time cost** | a second run of the same arm gave a *lower* median (8096 ms) than the disarmed arm (8636 ms). Within-arm spread exceeds between-arm difference |
| "the **JSC heap saturates**" at 113 MB / 1 722 272 objects | an artifact of three to six samples: over **26** navigations the series climbs 6, 10, 14 … 90 with no plateau |
| "retained **Documents saturate** too" at 3 → 5 → 11 | the same short-sample error. They grow ~+4/navigation without bound |

**What the controlled re-measurement actually says** (`.49`, ONE warm process, same page, same build,
alternating the switch, everything else held constant): the decoded lever reads **52 MB/load OFF against
47 ON**; interleaved 2 loads × 3 cycles reads **OFF 5, 4, 44 / ON 54, −71, 48**. Per-navigation deltas on a
real page span −71 … +54, so the spread *within* one arm exceeds the difference *between* arms — **this
instrument cannot resolve a lever of this size at this sample count.** The lesson is about method, not
about caches: two windows of a run are not two arms, and a plateau claim needs enough samples to see one.

**The problem, in one table.** Same page repeated on the x64 bench, `0.1.10.35`:

| loop | RSS |
|---|---|
| `https://example.com/` ×10 | plateaus, 1132892–1138124 KB |
| `https://dzen.ru/` ×5 | 1133152 → 1780648 KB, **+100–145 MB per load** |

A leak on the bench is a reap on the phone: the Lumia has 3 GB and its failure mode for an over-budget
process is a silent log truncation with no dump.

**What is solid.** `WebCoreSessionLoad` is `teardownSession()` → `emplace()` → `buildSession()` with no
early return, and the session trace is strictly 1:1 (24 `Page::create ok` / 24 teardowns), so a retained
session `Page` is not the accumulator. `releaseMemory()` had **no caller at all** — declared in both ABI
headers, defined, never invoked — while a port comment claimed the harness called it under memory
pressure; that claim is the SILENT LIE class of `STUB-AUDIT.md`. A per-navigation release now exists and
returns real memory at the margin (−126 MB over three light loads) but does **not** bound the growth:
+161 MB/load with no release, +155 with `Critical::No`, +160 with `Critical::Yes` — indistinguishable.

**The confirmed mechanism.** Each external SVG image creates a hidden whole `Page`
(`SVGImage.cpp:497`, with upstream's own FIXME that the cache cannot see the cycle). Confirmed twice
over on controlled pages: ten external svg images produced exactly ten extra live Pages (`8 → 18`) and
returning to the zero-SVG page brought the count back (`18 → 7`); and the Pages carry the `utility`
flag, which `isUtilityPage()` sets exactly for a Page whose ChromeClient is an `SVGImageChromeClient`
(`Page.cpp:276-279`), i.e. the one `SVGImage.cpp:489` installs. Instrument:
`Src\tools\make-svg-test-pages.ps1` + `svg-page-test.ps1`, read out of `SL: live pages` in
`gpuinit-steps.txt`.

**~~The one lever that moved the slope.~~ WITHDRAWN — see the block at the top.** `MemoryCache::evictResources()`
— `setDisabled(true/false)`, whose `setDisabled(true)` calls `remove()` on every resource of every session
**ignoring clients** — appeared to take dzen.ru from +156 to +65 MB per load across five loads each way,
both arms from one build armed by `LocalState\evictcache.txt`. Both arms came from *one process in two
sequential windows*, the disarmed window first and therefore cold: the −58 % is the warm-up. What survives
from this row is the instrument, not the lever — the live-Page count keeps climbing (`8 → 17`) and no
hidden Page is freed by either call.

**Killed by measurement, one page per explanation** (each differs from its control in exactly one
respect): page JS holding the `HTMLImageElement`s (`tjs` ≡ `tjs2`), images reached from CSS (`tcss`), svg
nesting (`tnest`) — all release on the next teardown (`27 → 16`); and unclosed loader requests (880 `+`
against 883 `−` in `port-trace.txt`). Also killed earlier: the font cache, and the loader as retainer.

**The uncomfortable part, and the reason nothing has been declared fixed.** Ten local SVG Pages cost
~+4 MB total, i.e. **~0.4 MB each** — far too little for the retained Pages to account for dzen.ru's
growth on their own. So one mechanism is confirmed, one half-measure works, and the remainder is
unattributed.

**What the three instrumented builds (`.43`/`.44`/`.45`, Sep 24 evening) settled — mostly eliminations.**
Two new traces now run at `after teardown` and `after build` on every navigation, both written from
`Src\port\WebCoreDriver.cpp` into `gpuinit-steps.txt`: `SL: mem` (`PerformanceLogging::
memoryUsageStatistics` for the JSC heap, plus `MemoryCache::getStatistics()`) and `SL: docs`
(`Document::allDocuments()` with each document's url).

- **~~The JSC heap saturates and is not the accumulator.~~ WITHDRAWN.** `javascript_gc_heap_size_mb` /
  `javascript_gc_object_count` grew 65 → 96 → 136 and appeared to freeze at **113 MB / 1 722 272 objects**
  — but that was three to six samples. Over **26** navigations the same series climbs 6, 10, 14 … 90 with
  no plateau. What survives: it is read after a full `garbageCollectNow()` each time, so the objects it
  counts are live.
- **~~Retained Documents saturate too.~~ WITHDRAWN — they grow ~+4/navigation without bound.** Their urls
  and their node counts are still the useful half of this row: an SVG image's document has an **empty**
  url (`t10.html` added exactly +10 empty-url documents and +10 Pages, all freed at the next teardown), so
  the dzen-url documents are page-created ones (DOMParser → `about:blank`, XHR-as-document → the response
  url), not svg images.
- **The port has no subframes at all.** `createFrame()` returns `nullptr`, so every `<iframe>` is an empty
  painted box: 0 new Documents, 0 requests. This is a capability gap in its own right (adverts, embeds,
  SSO, payments) and it *invalidated* the iframe retention experiment — the negative it produced meant
  nothing. New plan item **0l**.
- **The cache holds nothing dead**: `liveSize == size` for every type on every load, which is the proof
  that the non-critical release path (`pruneDeadResourcesToSize(0)`) can never free anything. And images
  report **`decodedSize = 0`** on every load while the cache holds 22–44 of them — the leading suspect for
  invisible resident memory, and a plausible reason why eviction is the only lever that works: it destroys
  the `CachedImage` objects and their surfaces rather than freeing tracked bytes.
- **The harness is exonerated** for the per-navigation part: five consecutive `file://` loads moved RSS
  733 → 687 → 674 and 670 → 674 → 678 — flat to slightly down.

The slope on the three instrumented builds reads 150 / 156 / 135 MB per load, overlapping the
`0.1.10.35` baseline (161) and every release arm, so all nine arms stay tabulated as one set: **three
release arms are indistinguishable from no release.**

**Two questions that were owed are now answered, both negative, both the same evening.** (1) The unit
ambiguity in the cache statistics is settled: **bytes**, and the log label was wrong by 1000× —
`CachedResource::size()` is `encodedSize() + decodedSize() + overheadSize()`, so a dzen.ru load caches
1.4 MB of images and 22 MB of scripts and the "19 GB of scripts" worry is dissolved. (2) An engine-side
total independent of MemoryCache accounting **does not exist here**: `WTF::fastMallocStatistics()` is
exported but on Windows is `GetProcessMemoryInfo().PeakWorkingSetSize` with the bmalloc fields hardcoded
to 0, so it is a tautology beside the harness's own RSS — it was wired in, it tracked RSS to the
megabyte (137 → 880 MB) in perfect lockstep, and that lockstep was the tell; it is removed. And
`WTF::memoryFootprint()` is guarded out of the App Container and returns 0 on both architectures.
Instrumentation therefore has to stay with the process RSS plus WebCore's own accounting.

The `decodedSize = 0` reading was sharpened by the same round: `size()` *includes* decodedSize, so the
cache is not under-reporting a number it has — the Cairo image path never calls
`CachedImage::decodedSizeChanged()`, an image with no decoded size never enters
`m_liveDecodedResources`, and that is the list `pruneLiveResourcesToSize()` walks. **No release path can
reach decoded bitmap data.**

**~~And the accumulator is now NAMED, PRICED and REACHED.~~ WITHDRAWN in `.49`/`.50` — what is left is a
frontier, not an accumulator.** The pricing half of that claim is still good and is kept below; the
conclusion drawn from it is not. The code that carried the conclusion was reverted to a file-armed,
default-off call (`cacherelease.txt`), and the withdrawal is recorded verbatim in the function comment on
`apoReleaseResourceCache()` in `Src\port\WebCoreDriver.cpp` — that comment, not this section, is the
authoritative version.

**What genuinely survives — the priced images.** The pricing run
(`Src\tools\make-image-test-pages.ps1`) builds a grid that fits inside the viewport on both lines so every
image is painted and therefore decoded: twenty 1200×1200 PNGs drawn at 200×100 cost **+116 MB on the first
paint against a predicted 110 MB of ARGB pixels** — the first figure in this project that has matched a
prediction — then flat, because on a `file://` page `after teardown cache_images count=0` and that memory
is freed and reused. The controls (the same grid with no images, and with twenty 32×32 images in the same
cells) are flat at +1 MB/load. And on dzen.ru the same reading is the opposite — `count=17 sizeB=1008233
liveB=1008233`, **surviving every teardown** — with `decodedB = 0` throughout, which is the code-level
fact that stands: **no upstream release path can reach decoded bitmap data here**, because the Cairo image
path never calls `CachedImage::decodedSizeChanged()`, so an image never enters `m_liveDecodedResources`,
the only list `pruneLiveResourcesToSize()` walks. That is why the two calls remain *available*
(`destroyDecodedDataForAllImages()` walks every resource live and dead, ignoring clients) — they are kept
as an escape hatch for that defect, not as a measured fix.

**What is left is a frontier.** Every arm built to expose an accumulator plateaus or shows a level shift,
never a slope:

| arm | reading |
|---|---|
| 20 × 1200×1200 PNGs in a viewport-fitting grid | +116 MB on the first paint against a predicted 110 — then flat |
| ten external SVG images | ~0.4 MB/Page, self-clearing (`8 → 18`, back to `7` at the next teardown) |
| documents created and dropped, and created and kept, by page JS | flat — and `window.__keep` dies with the page at navigation, so this arm **cannot see a cross-navigation leak at all** (it measures the wrong thing, which is recorded in `make-doc-test-pages.ps1`) |
| 20 layers promoted by `will-change: transform` against 20 identical unpromoted boxes | all-positive deltas (24, 8, 71, 24) alternating against a flat control — but 8 consecutive loads of the promoted arm read **80, 22, −15, −31, −2, 21, −10**: an ~**80 MB level shift, not a slope**, with `compositing=1` vs `0` verified in the log |
| the two cache levers, controlled and interleaved | inert (top of this section) |
| the retained Documents, priced | ~1 dzen document (`nodes=1518..1853`, `rv=0`) plus two 12–20-node empty-url SVG documents (`rv=1`) — **fractions of a MB**, against 50–190 MB/load |

**The one thing that still climbs is `https://dzen.ru/` itself** — and, in the same warm process,
`https://habr.com/ru/feed/` climbs too, so it is not that one site. Three readings, all on the default
configuration (`.50`, `cacherelease.txt` absent), all from `Src\tools\rss-loop.ps1`:

| run | process state | series (MB) | reading |
|---|---|---|---|
| dzen.ru ×12 | **fresh** | 136 → 400 → 555 → 639 → 699 → 785 → 834 → 889 → 975 → 1017 → 1060 → **1142** | **every delta positive** (264, 155, 84, 60, 86, 49, 55, 86, 42, 43, 82), **91 MB/load, no plateau in twelve loads** |
| habr.com ×8 | warm (~1.4 GB) | 1408 → 1768 | mean +53 MB/load but **sawtooth**: 237, **−92**, 22, 205, **−70**, 63, 9 |
| dzen.ru ×8 | warm (~1.8 GB) | 1842 → **1413** | mean **+7 MB/load** and the process *shrank* by 429 MB over eight loads |

**The slope is a fresh-process phenomenon, and the releases only appear near ~1.8 GB.** Once the process is
large, the same page that climbed 91 MB/load from a fresh start reads +7 MB/load and hands memory back
(−174 MB in one load). The controlled pages are flat at 0…+1 MB/load throughout, so what differs is
*something about heavy real pages*, and what remains unbounded is the curve **below** whatever watermark
engages at ~1.8 GB on the bench. That watermark is the part that matters for the target: the Lumia has
3 GB in total and an App Container budget far below the bench's 12.5 GB, so a curve that needs 1.8 GB to
turn over is a curve that runs to the wall on the phone first.

**And the harness's memory-pressure handler has never fired — not once, in any preserved log.** Every log
in `LocalState` records `MainPage: memory-level handlers registered, mem 35200/12497952 KB level=low`, and
**no `mem-level UP` line exists in any of them**, because the bench sits at `level=low` with a 12.5 GB
limit and is never near a threshold. So the handler wired on 2026-09-24 is untested by construction on the
bench, the releases above are *not* it, and the only place it can be tested is the device — where it is the
half that matters. **Whether it is a port defect or a page-side defect any browser would have is still not
separated**, which is what the next round has to produce; the process RSS and WebCore's own accounting have
both been exhausted as sources for it.

⚠️ **Bench state as of 2026-09-24, build `.50`: DEFAULT configuration.** `decoded.txt` and `evictcache.txt`
were deleted and are read by no code path; the only switch left is **`cacherelease.txt`**, absent, and the
build says so on every navigation (`SL: cache release after teardown: DISARMED (cacherelease.txt absent)`).
A switch that reports its own state is the point — silence is what let the old arms be misread. Check the
file before reading any run, exactly as with `-Gpu`.

---

## 0. Where the project actually stands (Aug 25, 2026)

> **Supersedes the Aug 21 section below.** That section is kept for its reasoning; where it
> contradicts this one, this one wins.

This document is a research archive and parts of it have been overtaken. Read this section first; where
it contradicts anything below, this section is correct and the older text is kept for its reasoning.

**The browser runs on the phone.** The ARM32 appx deploys to a Lumia 950 (Win10M 15254.603) over Device
Portal and opens real HTTPS sites. `example.com` paints its whole viewport, `news.ycombinator.com`
renders fully with its script loaded, `ya.ru` reaches `readyState=Complete`. Touch reaches the engine.
ICU 78 initialises and `ubrk_open` works.

**The dominant defect has been bisected to JavaScript execution.** A controlled experiment (build .76,
`setScriptEnabled(false)`) proved that removing JS eliminates the instant silent death across all tested
sites: ya.ru and dzen.ru survive (blank, as expected for SPAs), while example.com and HN render fully
with or without JS. With JS on: ya.ru dies ~+2–3 s of content, dzen.ru dies instantly. The heavier the
script bundle, the faster the kill. The root cause inside JSC execution is not yet identified — it could
be GC, stack depth, promise microtask queue, or an interaction between JSC allocation and the W10M
AppContainer memory manager. This is the next major investigation.

**Lightweight mode is viable.** Without JS, real sites load, parse, paint their static HTML/CSS, and
the browser survives indefinitely. For a "retro browser on old hardware" use case this is a legitimate
shipping configuration, not just a diagnostic.

**Fixed along the way (each verified on device):**

| Fix | Build | Mechanism |
|-----|-------|-----------|
| Font probe per-char file I/O disabled | .62 | apotheosisWebTrace fired fopen/fclose per glyph during layout |
| JSC Watchdog created + armed | .61/.66 | ensureWatchdog() exported from JSC.dll; setTimeLimit(15s) re-armed per window |
| Persistent curl handle | .70 | CA store (121 certs) parsed once per process instead of once per navigation |
| Background main-document fetch | .71 | Engine thread waits max 2.5 s; transfer runs on dedicated thread |
| Fail-fast curl timeouts + progress abort | .66/.67 | CONNECTTIMEOUT 3s, LOW_SPEED 1B/s×2s, progress-abort after 2.5s stall |
| Async image decoding | .63 | Large/animated images decode off-thread |

**Diagnostic instruments built (all remain armed):**
wtimer, rllock, rlsleep, wedgedump v6 (.pdata unwind + sibling threads + per-PID naming),
UEF raw-stack dump, wtfcash hook, heartbeat 700 ms. See `Doc/PUMPLOOP-SILENT-DEATH.md`.

**Corrections to claims made below, all measured rather than argued:**

- SDK **10.0.19041.0 does have the XAML compiler** — `bin\10.0.19041.0\XamlCompiler\` holds a
  full-size `Microsoft.Windows.UI.Xaml.Build.Tasks.dll`, plus the UAP platform, `Windows.winmd` and 88
  contract references. The old note that 26100 was needed "because 19041 is missing the XAML compiler
  DLL" described an incomplete SDK install, not an SDK defect. Both architectures now build against
  19041, which is also the only SDK with ARM32 libraries at all.
- `MainPage.g.hpp` is **included, never frozen**. A hand-pasted copy of it with hardcoded connection
  ids is what killed startup in 0.1.8.64 through 0.1.8.67, silently binding `x:Name` fields to the
  wrong controls. `Src/tools/verify-xaml-connect.ps1` now fails the build if the generated file drifts
  from the markup.
- `XamlMarkupCompileEnabled` is **true**. Markup compilation is live; the workarounds described below
  belong to a period when it was not.

**New documents worth reading before touching the relevant area:**

| Document | Subject |
|---|---|
| `Doc/ARM32-DANGLING-SECURITYORIGIN.md` | the launch crash: `String` returned by reference to a temporary, and two false trails |
| `Doc/HARFBUZZ-ICU-DIVERGENCE.md` | x64 shapes text without HarfBuzz and on ICU 75 while ARM32 uses HarfBuzz and ICU 78 |
| `Doc/DEFERRED-SCRIPTS.md` | why hh.ru stalls, and what the script-state fields actually mean |
| `Doc/UNIFICATION.md` | collapsing the two build lines: the link, the SDK, the generated XAML code |

---

## 1. What Is This Project?

**Apotheosis** (codename "EdgeHTML Reborn") ports the modern **WebKit/WebCore** rendering engine (webkitgtk-2.52.4) to **Windows 10 Mobile on ARM32 (UWP, App Container)**, targeting the **Lumia 950** family. It brings JIT-accelerated JavaScript and GPU-composited rendering to a platform abandoned by Microsoft years ago.

**Status (real device, Lumia 950, Win10M 15254):**

| Feature | Status | Notes |
|---------|--------|-------|
| WTF + JSC CLoop | ✅ | Phase 0 — engine core runs on device |
| WebCore + Cairo SW render | ✅ | Bing, GitHub, Apple, MS sites render correctly |
| Live interactive session | ✅ | Mouse events, form input, scroll, keyboard |
| JSC JIT | ✅ | ~5-50× speedup over CLoop |
| GPU compositing (ANGLE + TextureMapper) | ✅ | Direct present to SwapChainPanel |
| Smooth scroll / pinch-zoom | ✅ | GPU-backed, real-time |
| Browser shell (tabs, URL bar, settings) | ✅ | Version 0.1.8+ |
| Multi-language UI (en/ru/cn) | ✅ | `.resw` + fallback table |
| **x64-uwp build (PC debug)** | ✅ **Renders text end-to-end** | Appx v0.1.8.51 installed & launched on Win11 x64; engine init clean; real text incl. **CJK** paints (CJK hang fixed Aug 16); `https://example.com` loads (`rc=0`); full 8-test trace suite passes; **GPU whole-content distortion FIXED Aug 16** (`WebCoreGpuResize` — surface follows ContentArea, `contents=1024x694`). Real-device (ARM32) validation pending. See §10.

> **x64 AppContainer launch progression (Aug 2026)**: crashes broke into tiers; T1–T7 (XAML, BCrypt, font, Navigate, XBF, PLM, MinimalTest) resolved. Tier 3 (`cairo_scaled_font_create` AV) was fixed Aug 10 — see `Doc/2026-08-10-crash-fix.md`. The follow-on "dark blue background, no text" symptom was fixed Aug 11 (§10). Full CRT/ICU75/harfbuzz/fontconfig recovery after the Windows reinstall is documented in `AGENTS.md`.

> **Aug 16 2026 — CJK hang root-caused & fixed; GPU whole-content distortion FIXED**:
> - **CJK "strange font height / tofu" = an infinite loop**, not a crash: `FontDataCacheKeyTraits::emptyValue()`
>   (`FontCache.cpp:100-104`) builds the hash empty-sentinel as `FontPlatformData(0.f,false,false)`; with
>   `emptyValueIsZero=true` WTF memset-0s slots and tests `slot == emptyValue()` via `platformIsEqual`
>   (an `m_scaledFont` pointer compare). The WK_WINUWP ctor gave the size-0 sentinel a **live** scaled font,
>   so a zeroed slot's null pointer never matched → linear probe never terminated → watchdog hang at 40s.
>   **Fix**: `if (size)` guard in `FontPlatformData.cpp:182` keeps `m_scaledFont` null for size 0.
>   CJK now resolves via forced simhei bundled face (`stubs-font-uwp.cpp` + `apotheosisSetForcedBundledFontName`).
>   Verified: full 8-test trace suite passes (all ALIVE), `gdc-> c=U+4E00 g=1078 ok=1`, `adv w=48.00`.
> - **GPU whole-content distortion — FIXED** (not just planned): `WebCoreGpuInit` created the ANGLE swapchain
>   once at 720×1080 and GPU mode never resized it; `SwapChainPanel` stretched it to ContentArea → every
>   aspect mismatch made all content look too wide ("font weight / whole site too wide"). **Fix (Aug 16)**:
>   `WebCoreGpuResize(nativeWindow, w, h, outBuf)` in `WebCoreDriver.cpp` (declared in both C ABI headers).
>   The harness (`ApplyViewportSize`) no longer bails on `m_gpuPresent` — it posts an engine job that
>   recreates the GLContext/surface from a fresh `PropertySet` (`EGLRenderSurfaceSizeProperty`) + a fresh
>   TextureMapper, then `finishInteractionPaint`; `g_gpuScrollFast=false` forces `forceDirtyTree`. On
>   recreate failure → Cairo SW fallback. **Verified on x64**: `engine rc=0` across many live resize cycles
>   (1024→768→1081→1179→593→885→634×694), post-resize diag `contents=1024x694`, no crash, 200×200 test
>   boxes stay square. Fonts were always correct — the frame was stretched. Real-device (ARM32, FL9_3)
>   validation still pending. Details: `AGENTS.md` ("Rendering distortion") + `Doc/PLAN.md` §8.5.
> - **ARM32↔x64 sync DONE (Aug 16)**: ARM32 dirs renamed `build-clang-*` → `build-arm32-*`
>   (matches `build-x64-gpu`), obj suffixes unified (GPU `.arm32.obj` / JIT `.arm32-jit.obj` / soft
>   `.arm32-soft.obj`), `recompile-stubs-arm32.ps1` renamed → `recompile-stubs-x64.ps1` (it was an x64
>   script misnamed arm32), shared appx version (already true — one `Package.appxmanifest`); then full ARM32
>   rebuild (reconfigure + multi-hour `ninja -j1`, old tree wiped by the OS reinstall) → Lumia validation.
> - **Plan policy (Aug 16)**: GitHub/CI tasks are OPTIONAL/non-blocking; all ARM32 dev/build is fully local
>   on the dev machine; real-device work is manual (WDP). Order: local x64 → local ARM32 artifacts → real device.

---

## 2. Source & Origin

- **GitHub:** [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
- **Reddit:** [r/windowsphone](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)
- **Author:** Jimmy Xiao (GitHub: `Jimmyxiao2009`)
- **License:** MIT (port layer); LGPL-2.1/BSD (upstream WebKit + dependencies)

The upstream WebKit source is **webkitgtk-2.52.4** (released June 2, 2026), GitHub tag: `webkitgtk-2.52.4` (commit `7acdf5e`).

---

## 3. Architecture

```
Harness — UWP App (C++/CX, MSVC v143)
   · MainPage: toolbar, gestures → engine
   · GpuPanel (SwapChainPanel) ← GPU | RenderImage ← SW fallback
        │  C ABI (WebCoreDriver.h)
WebCoreDriver (clang-cl → WebCoreDriver-gpu.dll)
   · Resident Page/Frame session, event dispatch
   · Cairo paintToRGBA | TextureMapper GPU composite
   · PortChromeClient / FrameLoaderClient / Strategies
        │
WebKit / WebCore / JSC / WTF (clang-cl, WK_WINUWP patches)
```

**Key decisions:**
- Three layers decoupled by a stable **C ABI** (`WebCoreDriver.h`)
- Engine thread: all calls serialized on single background thread
- UI thread: never synchronously waits on engine (deadlock prevention)
- Two paint paths: Cairo SW (fallback) | TextureMapper GPU (runtime switch)
- ANGLE (D3D11 FL9_3) as OpenGL ES 2.0 wrapper for UWP App Container

---

## 4. Build System

### 4.1 Three Toolchains

| Layer | Compiler | Target | Output |
|-------|----------|--------|--------|
| WTF/JSC/WebCore | **clang-cl** (LLVM 22.1.7) | `thumbv7-unknown-windows-msvc` | Static `.lib` |
| Port driver | **clang-cl** + **lld-link** | ARM32 UWP | `WebCoreDriver-gpu.dll` |
| Harness (UWP app) | **MSVC v143** (14.44.35207) | ARM | `Harness.appx` |

### 4.2 Build Configurations

| Dir | JIT | GPU (ANGLE) | Purpose |
|-----|-----|-------------|---------|
| `build-arm32-webcore` | ❌ (CLoop) | ❌ (Cairo) | Phase 1b baseline |
| `build-arm32-jit` | ✅ | ❌ (Cairo) | JIT line |
| `build-arm32-gpu` | ✅ | ✅ | **Active dev line (gpu-path1, ARM32)** |
| `build-x64-gpu` | ✅ (incl. **FTL_JIT**) | ❌ (Cairo-only) | **Active: PC debug (x64-uwp)** |

### 4.3 Manual Dependency Builds

| Dependency | Build Method | Notes |
|------------|-------------|-------|
| ICU 75 | MSBuild, UWP config | `C:\icu-x64-uwp\` |
| **Cairo 1.18.4** | **Manual** — 49 .obj, **1 MB static lib** | Image backend only; built with clang-cl x64 |
| SQLite3 | Amalgamation | `SQLITE_OS_WINRT=1` |
| ANGLE x64 | NuGet | `Src\angle\x64\` |
| libpsl | Stub header | `libpsl.h` — no actual lib needed for port |
| curl | vcpkg `x64-windows` | Fallback — `x64-uwp` triplet failed |

### 4.4 Ninja Build Target Structure

Key discovery: **PAL is an OBJECT library** — no `PAL.lib` produced. PAL `.obj` files are compiled and linked directly into `WebCore.dll`. The top-level ninja targets:

| Ninja Target | Produces | Status |
|---|---|---|
| `ninja bmalloc` | Object lib objects | ✅ Compiled |
| `ninja WTF` | Object lib objects | ✅ Compiled |
| `ninja PAL` | Headers only | ✅ Header gen complete (1203 steps) |
| `ninja JavaScriptCore` | `bin/JavaScriptCore.dll` | ✅ **FTL_JIT enabled**, 18.8 MB DLL (x64) |
| `ninja WebCore` | `bin/WebCore.dll` (includes PAL objs) | ✅ **All 156 source files compiled + linked** into `WebCoreFull.lib` (947 .objs, 3.4 GB). WebCore.dll exports ~15 core WebCore internal symbols — need WEBKIT_EXPORT / .def file |
| `ninja all` | Everything | ❌ |

---

## 5. Key Engineering Discoveries

### 5.1 Critical Patches (WK_WINUWP)

| Area | Changes |
|------|---------|
| Memory allocation | `VirtualAlloc` → `VirtualAllocFromApp` |
| File I/O | `CreateFileW` → `CreateFile2` |
| Crypto | `CryptGenRandom` → `BCryptGenRandom` |
| Threading | Remove SEH `__try` (clang ARM can't lower `cleanupret`) |
| Networking | Stub `DNSResolveQueuePlatform` |
| Graphics | Cairo via port flushes: `stubs-font-uwp.cpp` (createFontPlatformData→nullptr) + Cairo `ft-stub`/`twin` on x64; NO DWrite/GDI compiled. Fix in flight = bundled-TTF backend |
| C++ exceptions | `_HAS_EXCEPTIONS=0` + `/EHs-c-` |
| mpark::variant | Replaced with `std::variant` |
| Window APIs | `SHGetValueW`/`GetWindowLongPtr`/`SetWindowLongPtr` guarded in `WindowsExtras.h` |
| Debug Help | `#include <dbghelp.h>` + `SymFromAddress` guarded in `DbgHelperWin.h/.cpp`; UWP stub returns `false` |
| Filesystem (UWP) | `SHGetFolderPathW` → `GetEnvironmentVariableW`; `CreateFileW` → `CreateFile2` in `FileSystemWin.cpp` |
| Memory unlock | `VirtualUnlock` guarded in `OSAllocatorWin.cpp` |
| Memory pressure | `CreateMemoryResourceNotification`/`QueryMemoryResourceNotification` guarded in `MemoryPressureHandlerWin.cpp` |
| Signals | `AddVectoredExceptionHandler` guarded in `SignalsWin.cpp` |
| Event loop | RunLoopWin.cpp rewritten for UWP: HWND messaging → generic condition-variable loop (`USE(GENERIC_EVENT_LOOP)`) |
| Timing | `timeBeginPeriod`/`timeEndPeriod` guarded in `CurrentTime.cpp` |
| Stack traces | `SYMBOL_INFO` usage guarded in `StackTrace.h` |
| Thread ID | Win32 thread ID (replaces pthread) in `MainThreadGeneric.cpp` |
| Build system | New `OptionsWinUWP.cmake` custom port; `PlatformWinUWP.cmake` files for WTF/JSC/bmalloc/Source/Tools; `WebKitCommon.cmake` — `WinUWP` added to `ALL_PORTS` |
| Third-party | `unifdef` stub for ThirdParty builds |
| **Font rendering** | `Font.h`/`FontPlatformData.h`: GDI API guards (`CreateFontIndirect`, `GetDeviceCaps`, `GetObject`, `SelectObject`, `DeleteObject`, `TextOutW`); `SharedGDIObject.h` stub; `FontMemoryResource.h` disabled under WK_WINUWP |
| **CSSValueAggregates.h** | `SpaceSeparatedArray` template constructor fix — unblocks CSS value compilation |

### 5.2 The "Pack Expansion" Compiler Wall

clang's `thumbv7-windows-msvc` backend cannot mangle variadic pack expansions in function template signatures when using `mpark::variant`. Fix: replace `WTF::Variant` (= `mpark::variant`) with `std::variant` under `WK_WINUWP` guard. This unblocked ~80% of WebCore compilation.

### 5.3 GNU Driver for Assembly Files

LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp contain AT&T-syntax inline assembly (`asm("...%rsi...")`) that clang-cl cannot parse. Solution: compile these two files with **clang++ (GNU driver)** while everything else uses clang-cl. Two custom ninja rules (`_gnu_Release`) added to `rules.ninja`:

```
rule _gnu_Release
  command = clang++.exe --target=x86_64-unknown-windows-msvc -x c++ $DEFINES $INCLUDES $FLAGS -MD -MF $out.d -o $out -c -- $in
  deps = gcc
  depfile = $out.d
```

Key differences from clang-cl rules: `deps = gcc` (not msvc), `-MD -MF $out.d` (not `/showIncludes`), `-o $out -c -- $in` (not `/Fo$out /c $in`), no `/Fd` for PDB. The `FLAGS` must use `-D` defines (not `/D`), `-I` includes (not `/I`). The `-imsvc` paths in `INCLUDES` must be replaced with `-isystem` for the GNU rule, since clang++ doesn't understand `-imsvc`.

**Current status (x64):** Both LowLevelInterpreter.cpp and MacroAssemblerX86_64.cpp compile cleanly with the GNU driver. The `-imsvc`→`-isystem` conversion is handled by `patch-build-ninja-gnu.ps1` for both files' `INCLUDES`. The only warnings are the harmless `[[no_unique_address]]` attribute ignored by clang-cl.

### 5.4 CMake 4.0 Missing Rules Workaround

CMake 4.0's Ninja generator has a quirk: **not all compiler/linker rules are emitted into `rules.ninja`**. Fresh builds produce only ~10 rules (bmalloc, unifdef, WTF, LLInt*), leaving ~24+ rules missing — including `JavaScriptCore`, `WebCore`, `WebKit`, `jsc`, `ANGLE`, `PAL`, and all utility rules (`CLEAN`, `HELP`, `RERUN_CMAKE`).

**Solution:** `patch-build-ninja-gnu.ps1` now scans `build.ninja` for all unique rule names used in `build` statements, cross-references against `rules.ninja`, and auto-generates any missing rules with correct command templates (C/CXX compiler, executable/shared-library/static-library linker, utility rules). This is a fully generalized workaround — it handles any future CMake re-generation without hardcoded target lists.

### 5.5 x64 Build Progress

| Component | ARM32 | x64-uwp |
|-----------|-------|---------|
| vcpkg deps (16 pkgs) | ✅ | ✅ ALL INSTALLED |
| vcpkg x64-uwp extra pkgs | N/A | ✅ freetype, harfbuzz[core], libxml2, libpng, zlib, brotli, pixman |
| ICU 75 | ✅ Custom cross-build | ✅ Built at `C:\icu-x64-uwp\` |
| SQLite3 UWP | ❌ Not bundled | ✅ Manually built (amalgamation) |
| **Cairo 1.18.4** | ✅ | ✅ **Manually built — 49 .obj, 1 MB static lib (image backend)** |
| ANGLE | ✅ Pre-built ARM | ✅ x64 from NuGet at `Src\angle\x64\` |
| WebKit source | ✅ Same source | ✅ Same source |
| WebCore config | ✅ `build-arm32-gpu` | ✅ `build-x64-gpu` — **CMake WinUWP fully configured** |
| OptionsWinUWP.cmake | N/A | ✅ Complete port: Cairo, ANGLE, CURL, LibPSL stubs |
| PlatformWinUWP.cmake | N/A | ✅ Includes Cairo + Curl + Win platform dirs |
| libpsl | N/A | ✅ Stub `libpsl.h` created |
| curl | N/A | ✅ `x64-windows` fallback (x64-uwp failed) |
| WTF build | ✅ | ✅ COMPILED (x64, clang-cl, Release; 12+ WK_WINUWP patches) |
| bmalloc build | ✅ | ✅ COMPILED |
| LLIntOffsetsExtractor | ✅ | ✅ LINKED |
| PAL build | ✅ | ✅ Header gen complete (object lib — .objs in WebCore) |
| JSC LowLevelInterpreter.cpp | ✅ | ✅ GNU driver (AT&T assembly, `deps = gcc`, `-MD -MF`) |
| JSC MacroAssemblerX86_64.cpp | ✅ | ✅ GNU driver (`-imsvc`→`-isystem` via patch script) |
| **JavaScriptCore** (overall) | ✅ (ARM) | ✅ **FTL_JIT ON**, `JavaScriptCore.dll` 18.8 MB — all unified sources compiled |
| **WebCore** | ✅ | ✅ **All 156 source files compiled + linked** into WebCoreFull.lib (947 .objs) |
| **CMake 4.0 rules workaround** | N/A | ✅ `patch-build-ninja-gnu.ps1` auto-scans for missing rules |
| Port driver | ✅ | ✅ WebCoreDriver-gpu.lib (15.8 MB — all 12 port files real code) |
| Harness appx | ✅ | ✅ EXE = 1 MB, zero LNK2019, real lib linked |
| AppContainer launch (x64) | 🔴 **T3 live (Aug 2026)** | **Crash progression**: T1 (XAML 0xc000027b) FIXED ✅ → T2 (JSC/BCryptGenRandom 0xc0000409) FIXED ✅ → T3 (Cairo font backend 0xc0000005) 🔴 LIVE → T4+ (Navigate/XBF/PLM/MinimalTest) FIXED ✅ |
| 8 MB thread stack fix | ✅ | Applied — CreateThread replaces std::thread for 8 MB WebEngine stack |

**Build environment (Surface Pro 5, i5-7300U, 4GB RAM):** Build dir ~0.4 GB; C: drive free ~15.9 GB.

---

## 6. Multi-Language UI

**Status:** Complete. Three languages:

| Language | Code | ID |
|----------|------|----|
| 中文 (Chinese) | `zh-Hans` | 0 |
| English | `en-US` | 1 |
| Русский (Russian) | `ru-RU` | 2 |

**Dual-source loading:** `GetStr()` tries `.resw` first, falls back to `kStr[lang][id]` table.

---

## 7. x64 Build Infrastructure ✅

All dependencies installed and verified during June 28 - July 1 sessions:

| Dependency | Status | Notes |
|------------|--------|-------|
| Toolchain | ✅ `Toolchain-x64-UWP-clang.cmake` | clang-cl `x86_64-unknown-windows-msvc` |
| vcpkg x64-uwp (16 pkgs) | ✅ ALL INSTALLED | Community triplet workaround |
| vcpkg extra pkgs | ✅ freetype, harfbuzz[core], libxml2, libpng, zlib, brotli, pixman, expat, bzip2 | For Cairo image backend |
| ICU x64-uwp | ✅ `C:\icu-x64-uwp\` | libs + DLLs |
| SQLite3 UWP | ✅ Manually built | `SQLITE_OS_WINRT=1` |
| **Cairo 1.18.4** | ✅ **Manually built** | 49 .obj → 1 MB static lib (image backend only) |
| ANGLE x64 | ✅ NuGet | `Src\angle\x64\` |
| **libpsl** | ✅ **Stub** | `libpsl.h` header stub — no actual library needed |
| **curl** | ✅ **`x64-windows` fallback** | `x64-uwp` triplet failed; dynamic linking OK for x64 |
| CMake configure | ✅ FIRST SUCCESS (June 29) | `build-x64-gpu` generated |
| **OptionsWinUWP.cmake** | ✅ **Complete** | Cairo, ANGLE, CURL, LibPSL stubs configured |
| **PlatformWinUWP.cmake** | ✅ **Complete** | Includes Cairo + Curl + Win platform dirs |
| WTF compiled | ✅ | 12+ WK_WINUWP patches applied |
| bmalloc compiled | ✅ | getpid→GetCurrentProcessId |
| PAL headers | ✅ | 1203 ninja steps completed |
| **JavaScriptCore** | ✅ **FTL_JIT ON** | `JavaScriptCore.dll` 18.8 MB; all unified sources + two GNU-driver AT&T-asm files |
| **WebCore** | ✅ **Re-linked, 47 MB** | `WebCore.dll` rebuilt via manual `lld-link` (bypasses ninja stale-deps). Export `.def` cleaned up (RTTI entry removed — not generated with `/GR-`). No `WebCoreFull.lib` needed. |
| **Port driver** | ✅ **WebCoreDriver-gpu.lib (15.8 MB)** | All 12 port .cpp files compiled into real code lib. 7 stubs in `webcore-internal-stubs.cpp` still needed for `-DBUILDING_WebCore` direct references. |
| **Harness appx** | ✅ **1 MB EXE, zero LNK2019** | Real lib builds successfully. Appx installs to AppContainer. |
| **AppContainer launch** | 🔴 **T3 live (Aug 2026)** | T1 (XAML 0xc000027b, SDK fix) ✅. T2 (JSC/BCryptGenRandom 0xc0000409, NTSTATUS inversion) ✅. T3 (Cairo font backend 0xc0000005) 🔴 — real path: stubs-font-uwp + Cairo ft-stub/twin, NOT DWrite. |

**Build environment quirks:**
- C++23 confirmed: `build.ninja` emits `-clang:-std=c++23`
- Perl must be on PATH for Python codegen scripts
- `ninja` locks: always delete `.ninja_lock` + `.ninja_log` after interrupted builds
- Long builds must use `[System.Diagnostics.Process]::Start()` — tool's bash wrapper kills subprocesses
- Appx packaging: XBF files not auto-included — need `<DisableEmbeddedXbf>true</DisableEmbeddedXbf>` + `InjectXbfForPackaging` vcxproj target
- Desktop CRT: WebCore/JSC compiled against desktop CRT — must bundle `MSVCP140.dll`, `MSVCP140_2.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` in Appx
- ICU data: `icudt75.dll` missing from ICU UWP build — wrapper DLL created reading `icudt75l.dat` at load time; source at `C:\Temp\icudt75.cpp`, binary at `C:\icu-x64-uwp\bin\`
- VS18 XAML compiler generates `.g.h` (not `.g.hpp`) for XAML type declarations; implementations in `.g.hpp` files are NOT compiled automatically. Fix: `#include "App.g.h"` in source files + `XamlGimpl.cpp` includes `App.g.hpp` and `MainPage.g.hpp`
- **4 GB RAM crisis**: clang-cl compilation of large files (`WebCoreDriver.cpp` = 5.2 MB .obj) requires ~950 MB RAM. Without fresh reboot (~440 MB free), `fatal error C1060` (heap exhausted). Always compile port files first thing after reboot.
- **XAML crash (0xc000027b) — FIXED** by switching SDK from 10.0.19041.0 to 10.0.26100.0 (missing XAML compiler DLL). Set `XamlMarkupCompileEnabled=false` + `DisableEmbeddedXbf=true`. MSBuild VS18 still overwrites `MainPage.g.hpp` even with disable — use hybrid appx instead.
- **BCryptGenRandom NTSTATUS/bool inversion — FIXED**: `BCryptGenRandom` returns 0 = `STATUS_SUCCESS`. Code had `if (!BCryptGenRandom(...)) CRASH()` → `!0` = true → CRASH on success. One-line fix in `RandomDevice.cpp:115`.
- **Hybrid appx procedure** (for after JSC/WebCore DLL rebuilds — avoid rebuilding Harness.exe):
  1. `makeappx unpack /p Harness_0.1.8.18_x64.appx /d appx18/`
  2. Replace `JavaScriptCore.dll` (and/or `WebCore.dll`) with new builds
  3. Delete `AppxSignature.p7x` + `AppxBlockMap.xml`
  4. `makeappx pack /d appx18/ /p hybrid.appx`
  5. `signtool sign /fd SHA256 /a /f cert.pfx /p apotheosis hybrid.appx`
  6. `Add-AppxPackage hybrid.appx`

---

## 8. Repository Layout

```
Apotheosis\
├── WebKit\               ← webkitgtk-2.52.4 (gitignored)
├── build-x64-gpu\        ← x64 build output (gitignored)
├── Src\
│   ├── port\             ← Port layer + build scripts (tracked)
│   │   ├── WebCoreDriver.{cpp,h}
│   │   ├── PortChromeClient.{h,cpp}
│   │   ├── LoadingFrameLoaderClient.{h,cpp}
│   │   ├── stubs-*.cpp
│   │   ├── Toolchain-*.cmake (ARM32 + x64)
│   │   ├── configure-gpu*.ps1 / link-driver-gpu*.ps1
│   │   ├── compile-driver-gpu*.ps1
│   │   └── build-harness.ps1
│   ├── harness\           ← UWP app (C++/CX, XAML)
│   │   └── Resources/{en-US,zh-Hans,ru-RU}/Resources.resw
│   ├── tools\             ← Deploy/diagnostic scripts
│   ├── angle\include\     ← ANGLE headers (tracked)
│   └── setenv.ps1         ← Environment setup
├── Doc\                   ← Documentation (tracked)
│   ├── PLAN.md            ← Development plan
│   ├── Summary.md         ← This file
│   ├── WIKI_EN.md         ← English wiki
│   ├── WIKI_RU.md         ← Russian wiki
│   ├── WIKI_CN.md         ← Chinese wiki
│   ├── HANDOFF.md         ← Phase 0 handoff
│   ├── M2-HANDOFF.md      ← GPU rendering details
│   ├── MEDIA-PLAN.md      ← Video/audio roadmap
│   ├── WEBKIT-UPGRADE.md  ← 2.52.4→2.53.4 analysis
│   └── MORNING-STATUS*, NIGHT-LOG*
├── AGENTS.md              ← Codex guidance
├── README.md / _CN.md / _RU.md
```

---

## 9. Recommended Next Steps

### Short-term — Fix x64 Appx Silent Exit & Deploy
1. ✅ **Export approach explored then abandoned** — `webcore-exports.def` created, `build.ninja` patched, 4 ChromeClient overrides added, `WebCoreFull.lib`/`cairo-complete`/`/FORCE:MULTIPLE` stripped from vcxproj.
2. ✅ **Stub approach succeeded instead** — port layer compiled with `-DBUILDING_WebCore` generates DIRECT symbol references; only local stub definitions in `webcore-internal-stubs.cpp` can resolve them. Exports not possible for these symbols.
3. ✅ **WebCore.dll rebuilt** via `lld-link` — manual script `link-webcore-dll.ps1` bypasses ninja's stale-deps + missing `WebCoreBindings` marker. 47 MB, 151 s.
4. ✅ **7 LNK2019 resolved**: 3 stub functions (`addData`, `insertTextWithoutSendingTextEvent`, `hasPlatformStrategies`) + 4 stub data globals (`aTag`, `inputTag`, `textareaTag`, `s_defaultTimeoutInterval`).
5. ✅ **Harness.exe at 1 MB** — zero linker errors, zero `/FORCE:MULTIPLE`, zero CRT mismatch.
6. ✅ **Real port lib compiled** — all 12 port .cpp files (15.8 MB `WebCoreDriver-gpu.lib`) with all real code
7. ✅ **Appx installed on Win11** — appx v0.1.8.17 + v0.1.8.18 both install to AppContainer
8. ✅ **XAML crash (0xc000027b) fixed** — SDK 10.0.26100.0 + `XamlMarkupCompileEnabled=false`
9. ✅ **BCryptGenRandom NTSTATUS inversion fixed** — `!BCryptGenRandom()`→`BCryptGenRandom()` in `RandomDevice.cpp:115`
10. 🔴 **Cairo font backend (0xc0000005)** — current blocker, Aug 2026; see tier 3 below. The historical DWrite/GDI stack is **not** in the current build — real path is `stubs-font-uwp.cpp` + Cairo `ft-stub`/`twin`.
11. ⏳ **ARM32 retrofit**: Apply stub-based approach to ARM32 port-layer build

### x64 AppContainer Launch — Crash Progression (Jul–Aug 2026)

**Tiered crash progression** — each fix reveals the next deeper issue:

| Tier | Crash | Symptom | Status |
|------|-------|---------|--------|
| 1 | XAML activation (0xc000027b) | Window never appears | ✅ **FIXED** — SDK 10.0.26100.0 + `XamlMarkupCompileEnabled=false` |
| 2 | BCryptGenRandom (0xc0000409) | Window appears → JSC init → `__fastfail` | ✅ **FIXED** — NTSTATUS/bool inversion in `RandomDevice.cpp:115` |
| 3 | Cairo font NULL (0xc0000005) | Window + JSC OK → font resolution AV | 🔴 **LIVE BLOCKER (Aug 2026)** — see below; NOTE: the DWrite/GDI stack described in older text is **not** compiled in this build |
| 4–7 | XAML Navigate / XBF / PLM / MinimalTest | bootstrap crashes | ✅ **ALL FIXED** (see AGENTS.md) |

#### Crash Tier 1 — XAML Activation (0xc000027b) — FIXED

```
Faulting module name: Windows.UI.Xaml.dll
Exception code: 0xc000027b (E_BOUNDS)
Faulting module offset: 0x0000000000916eff
```

**Root cause**: SDK 10.0.19041.0 is missing the XAML compiler DLL (`Microsoft.Windows.UI.Xaml.Build.Tasks.dll`). VS18 MSBuild hits MSB4181 when trying to run CompileXaml.

**Fix**: Switch `WindowsTargetPlatformVersion` to **10.0.26100.0** (Win11 SDK that ships with the compiler). Set `XamlMarkupCompileEnabled=false` + `DisableEmbeddedXbf=true`. Use pre-generated `.g.hpp` files (from VS2022 era) to bypass VS18 XAML compiler.

**Problem**: MSBuild VS18 overwrites `MainPage.g.hpp` even with `XamlMarkupCompileEnabled=false`. Solution: restore from `.g.hpp.backup` before each build, or use **hybrid appx** approach (unpack working appx → replace DLLs → repack → sign).

**Hybrid appx workflow** (preserves working `Harness.exe` + `WebCore.dll`):
1. `makeappx unpack /p Harness_0.1.8.18_x64.appx /d appx18/`
2. Replace `JavaScriptCore.dll` (and/or `WebCore.dll`) with freshly built version
3. Delete `AppxSignature.p7x` + `AppxBlockMap.xml`
4. `makeappx pack /d appx18/ /p hybrid.appx`
5. `signtool sign /fd SHA256 /a /f cert.pfx /p apotheosis hybrid.appx`
6. `Add-AppxPackage hybrid.appx`

#### Crash Tier 2 — BCryptGenRandom NTSTATUS Inversion (0xc0000409) — FIXED

```
Exception code: 0xc0000409 (FAST_FAIL_FATAL_APP_EXIT)
Crash module: ucrtbase!abort (called via WebCore!FrameLoader::init → ... → JSC)
```

**Root cause (crash dump analysis)**: `WTF::RandomDevice::cryptographicallyRandomValues` in `JavaScriptCore.dll` calls `BCryptGenRandom` which returns **NTSTATUS** (0 = STATUS_SUCCESS, non-zero = failure). But the code uses the `!` operator — `if (!BCryptGenRandom(...)) CRASH()`. Since `BCryptGenRandom` SUCCEEDS and returns 0, `!0` = true → `CRASH()` on success.

**Disassembly confirmed**:
```
call BCryptGenRandom
test eax, eax
je   abort  ← jumps to abort when eax == 0 (STATUS_SUCCESS!)
```

**Fix** (one line in `WebKit/Source/WTF/wtf/RandomDevice.cpp:115`):
```cpp
// Before (WRONG — NTSTATUS treated as BOOL):
// if (!BCryptGenRandom(nullptr, buffer.data(), buffer.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
//     CRASH();
// After (CORRECT — NTSTATUS 0 = success):
if (BCryptGenRandom(nullptr, buffer.data(), buffer.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
    CRASH();
```

**Rebuild**: `ninja -C build-x64-gpu JavaScriptCore -j1` (46 steps, clean). `JavaScriptCore.dll` = 18,789,376 bytes.

**Verification**: Hybrid appx (old working `Harness.exe` + new `JavaScriptCore.dll`) survives past JSC init — no more 0xc0000409.

#### Crash Tier 3 — Cairo Font NULL Dereference (0xc0000005) — LIVE BLOCKER

```
Exception code: 0xc0000005 (ACCESS_VIOLATION)
Faulting module: WebCore.dll
Fault address: cairo_scaled_font_create+0x89c (mov eax, dword ptr [rcx+8])
```

**Call stack** (from a captured minidump; Aug 2026):
```
Style::resolve → FontCascade::realizeFallbackRangesAt → FontCache::lastResortFallbackFont
  → Font::create → cairo_scaled_font_create(fontFace=NULL/bogus)  ← AV
```

**Root cause (VERIFIED vs actual build, Aug 2026)**: The historical DWrite/GDI diagnosis is **stale**. The x64 build (`WK_WINUWP=1`) does **not** compile the upstream win/graphics font files (`FontPlatformDataWinCairo.cpp`, `FontPlatformDataWin.cpp`, `FontCacheWin.cpp` are absent from `WebCoreFull.lib`; `createCairoDWriteFontFace` symbol missing). The real font path is:
- **`Src\port\stubs-font-uwp.cpp`** — `FontCache::createFontPlatformData()` → `nullptr`, `systemFallbackForCharacterCluster()` → `nullptr`. No system-font loading.
- Cairo linked as `cairo-complete-x64-uwp.lib` with **`cairo-ft-stub`** (FreeType backend stubbed) + **`cairo-font-face-twin`** (embedded "twin" vector font). No DWrite/GDI backend.

So `lastResortFallbackFont` yields a null/unusable `cairo_scaled_font_t*`. **Fix**: implement `CoreFont::createFontPlatformData()` in `stubs-font-uwp.cpp` to return a valid cairo face from a bundled TTF (injected into the appx, read via LocalState/resource), with the built-in twin font as ultimate last-resort. Do not reintroduce DWrite.

#### Crash Tier 5 — localMainFrame->init() (0xC0000005) — RESOLVED (superseded by T3 font blocker)

**DebugView confirmed** (Jul 23 2026):
- All init steps pass (JSC, WTF, AtomStrings, PlatformStrategies, BackForwardCache, MemoryCache)
- `Page::create` succeeds
- `writer.end()` now succeeds (after FontPlatformData Cairo toy font fix)

**Resolution**: Earlier `Frame::init()` / post-init aborts were the **same** Cairo-font cascade: `lastResortFallbackFont` produced a null/bad face. Framework now reaches the first text/style pass; the reproducible AV is in `cairo_scaled_font_create` — the single live blocker (Tier 3). No remaining `Frame::init()` null-vptr.

**Next step**: implement a real UWP font backend (bundled TTF cairo face in `stubs-font-uwp.cpp`), relink `WebCoreDriver-gpu.lib`, rebuild appx, re-capture a `-mm` dump.

### Memory Constraints (4 GB RAM — Surface Pro 5)

The build machine (i5-7300U, 4 GB RAM) significantly impacts development velocity:

| Issue | Impact |
|-------|--------|
| Reboot required before compiling large files | Without reboot: ~440 MB free → `fatal error C1060` (heap exhausted in clang-cl) |
| `WebCoreDriver.cpp` (7.5 MB) | 5.2 MB .obj after compilation — requires ~950 MB+ RAM for the compiler |
| All 12 port files successful after reboot | `WebCoreDriver-gpu.lib` = 15.8 MB |
| Ninja parallel builds impossible | Always `ninja -j1` |
| lld-link needs exclusive memory | ~2 GB RSS for linking 931 .obj files into WebCore.dll |

**Empirical rule**: After boot, only ~440 MB free. After opening VS Code + browser, ~200 MB. Large compilations must be done first thing after reboot.

### Port Layer Compilation (Jul 20)

All 12 port .cpp files compiled with clang-cl (`x86_64-unknown-windows-msvc`, Release, `-DBUILDING_WebCore`):

| File | .obj Size |
|------|-----------|
| `WebCoreDriver.cpp` | 5,199,206 bytes |
| `PortChromeClient.cpp` | 500,331 bytes |
| `LoadingFrameLoaderClient.cpp` | 684,614 bytes |
| `PortPlatformStrategies.cpp` | 1,791,104 bytes |
| `PortNetworkStorageSession.cpp` | 530,287 bytes |
| `webcore-driver-stubs.cpp` | 386,491 bytes |
| `stubs-crypto.cpp` | 1,693,960 bytes |
| `stubs-pasteboard.cpp` | 711,898 bytes |
| `stubs-network.cpp` | 365,553 bytes |
| `stubs-ax.cpp` | 1,596,496 bytes |
| `stubs-other.cpp` | 1,970,158 bytes |
| `stubs-loader.cpp` | 261,053 bytes |

**Total lib**: 15,859,018 bytes (15.8 MB). Duplicate symbol warnings (LNK4006) for 7 screen-query stubs defined in both `webcore-driver-stubs` and `stubs-other` — harmless (first definition wins).

**Build command used** (automated, from `package-driver-x64.ps1`):

### Near-term — Fix the Cairo Font Backend (Tier 3) — ✅ RESOLVED Aug 11 2026

**Status update (Aug 11 2026):** Tier 3 (the `cairo_scaled_font_create` AV, fixed Aug 10) is done and **text now renders on x64**. What remained after the crash fix was a *silent* defect: the frame painted only the body background (`#1a1a2e`) with no glyphs. Full story in §10 below. Historical record of the Tier 3 investigation (kept for reference):

- ✅ **Patched `FontPlatformData` ctor** (`WebKit/Source/WebCore/platform/graphics/FontPlatformData.cpp`, `#if WK_WINUWP`): `apotheosisBundledFontFace()` loads a bundled TTF (`APOTHEOSIS_FONTS_DIR` env, set by harness) via FreeType (`FT_New_Memory_Face`) and builds a Cairo FT face (`cairo_ft_font_face_create_for_ft_face`).
- ✅ **WebCore.dll rebuilt & linked** with the patch: 45.25 MB (07.08 21:12).
- ✅ **WebCoreFull.lib regenerated** (3331.8 MB, 21:17) from all 979 WebCore .objs; harness x64 link line gained `psl.lib` (fresh `PublicSuffixStoreCurl.cpp.obj` needs `psl_builtin/public_suffix…`).
- ✅ **Full appx v0.1.8.25 (68 MB)** built & installed on the Win11 x64 dev machine (cert → Root + Trusted People; old package removed, new one registered). Engine init log is CLEAN: all 5 bundled fonts found, `fonts.conf` written, CACert 189462 B, loop ready.
- 🔴 **Crash — reproducer**: 0xc0000005, "Attempt to execute non-executable address at `WebCore+0x0`" (module base). Full cdb stack: `WebCoreRenderHtml → FrameLoader::init → DocumentLoader::startLoadingMainResource → maybeLoadEmpty → finishedLoading → DocumentWriter::end → HTMLDocumentParser::finish → prepareToStopParsing → Document::finishedParsing → updateStyleIfNeeded → resolveStyle → Style::TreeResolver::resolve → Style::Scope::resolver → createDocumentResolver → Style::Resolver::initialize → FontCascade::primaryFont → FontCascadeFonts::primaryFont → realizeFallbackRangesAt → FontCache::lastResortFallbackFont → **FontPlatformData::FontPlatformData+0x834 (patched ctor)** → cairo_scaled_font_create+0x1f8 → cairo_scaled_font_create_in_error+0x20c → **RIP = WebCore+0x0 (MZ header, non-executable)**.
- **Interpretation**: the bundled-FT face (from `cairo_ft_font_face_create_for_ft_face`) goes through `cairo_scaled_font_create`; on this Cairo build (`cairo-complete-x64-uwp.lib`, rebuilt 07.08 04:21 with real `cairo-ft-font.x64.obj`; `freetype.dll` shipped + imported) the scaled-font creation hits the error path (`_in_error`) whose function-pointer dispatch lands on a NULL/garbage slot → jump to module base. FT face status/`load_flags`/ppem are the next suspects (see AGENTS.md tier 3 for the ordered checklist).

Remaining steps:
1. ✅ Fix `FontPlatformData` ctor FT path (done Aug 10 — see `Doc/2026-08-10-crash-fix.md`).
2. ✅ Rebuild `WebCore.dll` → harness appx.
3. ✅ Re-capture on Win11 — style resolution no longer faults.

---

## 10. Text Rendering on x64 — Root Cause & Fix (Aug 11 2026) ✅

**Symptom.** After the Tier 3 crash fix, `about:home` launched cleanly (`WebCoreRenderHtml: rc=0`, no dump) but displayed **nothing but a dark blue background** — no text at all. `shot_ui.bmp` was 720×1080 with **exactly 1 distinct color**, `ff1a1a2e`, i.e. precisely the `background:#1a1a2e` of the test page.

That single-color frame was the key data point: it proved the pipeline was *healthy*. Parsing, style resolution, layout, Cairo painting of the body background, the RGBA copy, the WriteableBitmap handoff and XAML display all worked. Only glyphs were missing.

### Stage 1 — Font stubs (implemented, verified working)

`Src/port/stubs-other.cpp` had zeroed/false font stubs. Two were implemented against pure Cairo+FreeType (no fontconfig — this port ships none):

- **`Font::platformInit()`** — real metrics from `cairo_scaled_font_extents`, with cap/x-height approximated via `cairo_scaled_font_text_extents`.
- **`GlyphPage::fill()`** — char→glyph via `FT_Get_Char_Index` on the face from `cairo_ft_scaled_font_lock_face`, replacing upstream's fontconfig-dependent `FcFreeTypeCharIndex`.

Temporary instrumentation (`APOTHEOSIS_GLYPH_LOG`) proved both correct at runtime: `platformInit` fired 4× with sane metrics (`asc=18.0 desc=5.0 cap=12.0 x=8.0 isFT=1`) at exactly the sizes the page requests, and `fill` mapped characters correctly (`U+0020→0x3`, `U+0030→0x13`, `U+200B→0xB08`, CJK `U+6C30→0x0` correctly unmapped in a Latin face).

**Yet the frame was still one flat color, and a trace in `FontCascade::drawGlyphs` fired ZERO times.** The glyph layer was perfect; text simply never reached a draw call.

### Stage 2 — Root cause: the whole page was routed to the unimplemented complex text path

`FontCascade::codePath()` (`platform/graphics/FontCascade.cpp:676`) contains:

```cpp
#if !USE(FONT_VARIANT_VIA_FEATURES) && !USE(FREETYPE)
    if (run.length() > 1 && (enableKerning() || requiresShaping()))
        return CodePath::Complex;
#endif
```

All three conditions held in this build:

| Condition | Value | Why |
|---|---|---|
| `USE_FONT_VARIANT_VIA_FEATURES` | not defined | never set for this port |
| `USE_FREETYPE` | **not defined** | deliberate — see below |
| `enableKerning()` | true | default for normal text |

So **every run longer than one character was forced onto `CodePath::Complex`** → `ComplexTextController` → `collectComplexTextRunsForCharacters`, which is a **no-op stub** in `Src/port/stubs-other.cpp`. It yields zero runs, so no glyph ever reaches `FontCascade::drawGlyphs`. Exactly the observed symptom.

The same `!USE(FREETYPE)` premise also gates `canHandleRunAsSimpleText` (line 633); that one only rejects *partial* runs, so it wasn't the active blocker, but it would have broken selection and highlighting later.

**Why `USE_FREETYPE` cannot simply be defined:** upstream ties that macro to fontconfig. Defining it adds `RefPtr<FcPattern> m_pattern` to `FontPlatformData` (an ABI/layout change) and pulls in fontconfig headers this port deliberately does not ship. A previous defect where port scripts force-defined it for port objects only — while WebCore was built without it — produced exactly that layout mismatch.

**Fix** (both guards, `WK_WINUWP`-scoped per project convention):

```cpp
#if !PLATFORM(GTK) && !PLATFORM(WPE) && !USE(FREETYPE) && !defined(WK_WINUWP)   // line 633
#if !USE(FONT_VARIANT_VIA_FEATURES) && !USE(FREETYPE) && !defined(WK_WINUWP)    // line 676
```

This port rasterizes through Cairo+FreeType, so it belongs on the FreeType branch semantically; it just can't say so via `USE_FREETYPE`. Text now takes the **simple path** (`WidthIterator` + `GlyphPage`), which is implemented and verified.

### Result

| Metric | Before | After |
|---|---|---|
| `drawGlyphs` calls | 0 | **3** (n=15 @64px, n=33 @28px, n=8 @20px) |
| Distinct colors in frame | 1 | **596** |
| Palette | `ff1a1a2e` only | `ffffffff` (h1), `ffa0c4ff` (`#a0c4ff` p), `ff888888` (`#888` p) + AA |

Visual confirmation: `about:home` renders "EdgeHTML Reborn" / "WebKit 2.52.4 · Windows 10 Mobile" / "Loading…" as properly antialiased, correctly positioned text. Non-ASCII (`·` U+00B7, `…` U+2026) maps correctly too.

### Build-graph lessons (cost more time than the fix)

- **Unified sources hide file membership.** WebCore compiles via CMake unified bundles, so an individual upstream `.cpp` produces **no matching `.obj`**. Absence of a per-file object is *not* evidence a file is excluded. Generated bundles live in `build-x64-gpu/WebCore/DerivedSources/unified-sources/` (674 files, hash-named `UnifiedSource-<hash>-N.cpp`) — **not** the empty `CMakeFiles/WebCore.dir/.../unified-sources` mirror. Grep the bundles to find a file's home.
- **`lib/WebCore.lib` is an import library.** Querying it with `llvm-nm` lists only symbols exported through `webcore-exports.def`. Internal classes such as `TextBoxPainter` show 0 defined symbols there while being fully present in the objects. Query the `.obj` files, not the import lib. (This produced a false lead mid-session.)
- **Harness link order is decisive.** With `WebCore.lib` (import lib) placed *before* the 3.4 GB `WebCoreFull.lib`, the DLL satisfies almost everything and the archive contributes only the 4 GPU/TextureMapper symbols missing from the `.def`. The reverse order floods `LNK4006` and ends in `LNK1102` (linker OOM).
- **Ninja is not usable** for these rebuilds (its deps log expects the pre-move source root and wants to rebuild ~1100 objects). Use `build-x64-gpu/mkrsp.py <object-basename>` to lift the exact `DEFINES`/`FLAGS`/`INCLUDES` out of `build.ninja` and drive `clang-cl @relink.rsp` directly, then relink via `Src/port/link-webcore-dll.ps1`.
- **Backslash-heavy Windows paths must not go through bash heredocs or `awk`** — escapes get mangled. Do path substitution in Python with marker matching. Ninja also escapes `:` as `$:` inside paths; unescape before handing to the compiler.
- **`link-webcore-dll.ps1` prints "WebCore.dll created: <size>" even when the link failed** (the old DLL is still on disk). Verify by timestamp.
- **Python on this machine is the `py` launcher** (3.12.10); bare `python`/`python3` hit the Microsoft Store alias stub.
- **`MinimalTest=true` links no engine** (uses `MainPage.minimal.cpp`) — useless for verifying rendering.

### Still stubbed (not blocking about:home, may matter for real sites)

`ComplexTextController::collectComplexTextRunsForCharacters` (no-op — everything now routes around it, but any content that still forces the complex path will render blank), `Font::platformSupportsCodePoint()` → false, `FontPlatformData::familyName()` → empty, `FontCache::platformAlternateFamilyName()` → empty, `Font::determinePitch()` no-op.

Upstream ships `platform/graphics/harfbuzz/ComplexTextControllerHarfBuzz.cpp` and HarfBuzz is already linked, so wiring the real complex path is the natural follow-up for RTL/Arabic/Indic and ligature-heavy content.

### Medium-term
1. Build Harness for ARM32 → Lumia 950 with all fixes
2. Fix C# harness (Apotheosis.csproj) — convert to old-format UWP C# project
3. Upgrade to webkitgtk-2.53.4
4. Refactor port layer — consolidate stubs, reduce duplication

### Long-term
1. Upstream WK_WINUWP patches to WebKit
2. WebGL, Service Worker, PWA support
3. Video/audio playback
4. CI/CD

---

## 2026-08-11 — every real page load crashed: a `std::partial_ordering` sret-ABI split

`about:home` painted, but **every** load through `WebCoreSessionLoad` died with a deterministic
`0xC0000005` on the engine thread. A trivial local HTML file crashed identically, so it was
never content-driven: `about:home` survives only because `WebCoreRenderHtml` never enters
`pumpLoop`, and `pumpLoop` had literally never completed once in this app.

**Root cause.** `std::partial_ordering` has a user-provided constructor, so under the Microsoft
ABI it is returned via a hidden sret pointer, shifting both operands of
`WTF::operator<=>(const TimeWithDynamicClockType&, const TimeWithDynamicClockType&)` to RDX/R8.
clang-cl did not apply that rule consistently across WTF translation units — both inside the
same `JavaScriptCore.dll`:

```
; Condition::waitUntilUnchecked (Condition.h) -- register-return form
18023d1e2: callq nowWithSameClock   ; clobbers R8 (QueryPerformanceCounter inside)
18023d1e7: movq  %rbx, %rcx         ; RCX = &timeout   (param 1, no sret slot)
18023d1ea: movq  %r14, %rdx         ; RDX = &nowTemp   (param 2)
18023d1ed: callq operator<=>        ; R8 = STALE QPC TICKS
18023d1f2: cmpb  $-0x1, %al         ; result read from AL

; TimeWithDynamicClockType.cpp definition -- sret form
180e17d84: movl  0x8(%rdx), %eax    ; a.m_type
180e17d87: cmpl  0x8(%r8), %eax     ; b.m_type   *** FAULTS ***
```

Mangled names are identical, so the linker bound the mismatched pair silently, with no warning
anywhere. The tell: faulting addresses climbed *monotonically over the session*
(`0x4170474A13` → `0x5141BD18F9`); at the Windows QPC rate of 10 MHz those decode to 7.8 h →
9.7 h of uptime. Garbage pointers do not track wall-clock time — that is what identified a
counter value sitting in a register. **When an AV address trends upward across runs, suspect a
clock value in a register, not a wild pointer.**

**Fix.** `#if defined(WK_WINUWP)` in `WebKit/Source/WTF/wtf/TimeWithDynamicClockType.h` defines
the `operator<=>` friend **inline** (body byte-identical to upstream); the out-of-line
definition in `TimeWithDynamicClockType.cpp` is `#if !defined(WK_WINUWP)`-guarded out. Inline
means each TU emits its own copy under whichever convention it also uses at the call site, so
the two cannot disagree. Because call-site codegen changes and `WebCore.dll` *imports* the
symbol (2 refs — it cannot be left stale or the DLL fails to load), this needs
`ninja -C build-x64-gpu JavaScriptCore WebCore` ≈ 1155 objects, ~3 h at `-j1`.

Suspect the same bug on ARM32 (`build-arm32-gpu`): same compiler, same headers, AAPCS has the
equivalent rule.

**Also fixed.** `buildSession` gated on `CURLINFO_RESPONSE_CODE`, which is 0 for non-HTTP
schemes — so every `file:`/`data:` URL was rejected `rc=-100` despite curl having fetched the
bytes. Now only enforced when the transfer actually produced a status. This is what made a
fully local, content-controlled route into `pumpLoop` possible, and it is what proved the crash
was not content-driven.

**Instruments worth keeping.** `llvm-objdump -d --start-address/--stop-address` on the DLL to
read the real calling convention at a call site — the only way to see an ABI split, since
symbol names and headers both look correct. `GetModuleHandleW(nullptr)` logged at startup so
ASLR'd EXE frames can be symbolized. `WebCorePortTrace` → `port-trace.txt` (append + flush per
line) next to `FONTCONFIG_FILE`, because `DBG_STAGE` only reaches `OutputDebugStringA`, which
nothing inside the AppContainer can observe.

**Build-command corrections.** `link-driver-gpu-arm32.ps1` and `compile-driver-gpu-arm32.ps1` are the ARM32
scripts (they hard-fail on the x64 box via `arm32-uwp-env.ps1`); the x64 line uses
`link-driver-gpu-x64.ps1` / `compile-driver-gpu-x64.ps1`. And `MinimalTest=true` — which
CLAUDE.md still shows in the documented harness build — is a crash-isolation stub that compiles
`MainPage.minimal.cpp`, excludes `MainPage.xaml.cpp`, and links **no engine libs** (0 imports).
Real builds must leave it unset.

**Link hygiene.** `WebCoreFull.lib` (3.21 GB) was in the harness link only to satisfy four
GPU-compositing symbols lacking upstream `WEBCORE_EXPORT`. Exporting them via
`webcore-exports.def` (`BitmapTexture::BitmapTexture`, `GLContext::createOffscreen`,
`GraphicsLayerTextureMapper::setBackgroundColor`, `TextureMapper::clearColor`) removed the
archive from the link entirely, collapsing the `/FORCE:MULTIPLE`-masked LNK4006 wall to one
benign CRT/stub collision and eliminating the duplicate-function-local-static hazard class.
It did **not** fix this crash — WTF is single-copy (WebCore.dll defines 0 RunLoop symbols and
imports 859 `@WTF@@` symbols from JSC) — but it is necessary hygiene and reclaimable disk.

## 2026-08-11 (evening) — the "3-hour rebuild" was the *moved build tree*, not the header edit

The section above estimated ~1155 objects / ~3 h to propagate the `operator<=>` fix. That was
wrong, and the reason is worth writing down because it will bite every future engine edit.

**`inline` was not enough — it had to be `ALWAYS_INLINE`.** A merely-inline definition is still
emitted as a COMDAT under the same mangled name, so a caller that declined to inline emits a
call the linker may bind to another TU's COMDAT — the identical mismatch, harder to see. Forcing
expansion emits no call and no COMDAT, which turns the fix into a *checkable invariant*:

```
??__MWTF@@YA?AUpartial_ordering@std@@AEBVTimeWithDynamicClockType@0@0@Z
```

must appear in **no** object file and in **no** DLL import/export table. Verified: 0 of 1540
objects; `--coff-imports`/`--coff-exports` clean on `WebCore.dll` and `JavaScriptCore.dll`.

**Why ninja wanted 1528 steps.** The tree was configured at `C:\Users\Admin\source\repos\!Vibe\…`
and now lives at `C:\Users\media\source\repos\Vibe\…`. `build.ninja` uses build-relative paths
(so compiling still works) but `.ninja_deps` stores **absolute** header paths. Those resolve to
nothing, a missing dep counts as dirty, and *every* pre-move object is permanently dirty.
`ninja -d explain -n <object>` is the only way to see this. Each fix exposed the next rung:

| # | ninja's reason | fix |
|---|---|---|
| 1 | old-path headers "is dirty" | directory junction `!Vibe` → `Vibe` (load-bearing, keep it) |
| 2 | CMake's header **copy** under `build-x64-gpu/WTF/Headers/wtf/` is a separate input | `cp` from source, then `touch -r` a sibling |
| 3 | `cmake_pch.cxx.pch` newer than the objects | **no mtime fix exists** — back-dating the PCH makes it older than the headers it includes, so ninja rebuilds it and it is new again |
| 4 | "stored deps info out of date" for all 1520 objects | self-inflicted by a blanket `touch`; ninja marks an output dirty when its mtime exceeds its recorded deps-log mtime |

So: **never `touch` objects in a `deps = msvc` build**, and deleting `.ninja_deps` does not help
either (a missing record is itself "dirty"). Ninja incremental builds in this tree are
permanently broken; the real cure is a from-scratch reconfigure, which needs disk we do not have.

**Bypassing ninja for narrow work.** `ninja -t commands -s <target>` prints the one
fully-expanded command for a single edge — strip `/showIncludes`, run from the build root. Link
edges use a response file ninja writes then deletes, but `CMakeFiles/rules.ninja` gives
`rspfile_content = $in_newline $LINK_PATH $LINK_LIBRARIES`, so it can be rebuilt from the
`build …:` edge line (explicit inputs up to the `|` separator, one per line) plus that edge's
`LINK_LIBRARIES`, unescaping `$$`→`$`, `$ `→space, `$:`→`:` (all three occur here). **Validate
the reconstructed rsp** — every non-flag `.obj`/`.lib` token must exist — before linking.

**Result: 9 objects + 9 stubs + 2 links ≈ 35 min instead of 3 h.** The 9 were found by binary
`grep -lF` for the mangled name across all 1529 objects, which is sound *only* because the old
declaration had no visible body, so every calling TU necessarily emitted an undefined external.

Two more consequences of the move: CMake mangles out-of-tree source paths into
`WebCore.dir/C_/Users/<user>/…`, which orphaned all 9 `Src/port/stubs-*.cpp` objects; and some
objects predate the current CMake configuration, so expect to relink, read
`lld-link: error: undefined symbol`, fix, relink. Here that surfaced three libpsl symbols whose
`lib\psl.lib` (a hand-written stub in `build-x64-gpu/libs/psl/`) appears nowhere in
`build.ninja` and had to be appended to the reconstructed `CMakeFiles/WebCore.rsp` by hand.

Minor traps: `ccache` sits in front of `clang-cl` in the compile line (so per-object times vary
wildly); `taskkill` needs the **WINPID** (4th column of `ps -W`), not the Cygwin PID; and
`_link.bat`'s `if errorlevel 1` guard did **not** catch an lld failure — confirm a link by the
output DLL's mtime and size, never by a trailing "OK" marker.

---

## 2026-08-14 — Toolchain recovery after the Windows reinstall (status: x64 rebuild in flight)

The OS was reinstalled on the dev box (Surface Pro 5, i5-7300U, 4 GB RAM); `C:\vcpkg`,
`C:\icu-x64-uwp`, MSVC/LLVM tooling, Ruby/Perl and the harness AppPackages were wiped. The repo
and `build-x64-gpu` (engine DLLs, `WebCoreDriver-gpu.lib`, ANGLE, ICU75 DLLs, harfbuzz) survived.
Full line restored, no `!Vibe`/`Admin` paths, no disk wasted:

| Component | Restored as | Notes |
|-----------|-------------|-------|
| LLVM 22.1.8, CMake 4.0.1, Ninja 1.12.1 | Direct download (no winget) | Ninja copied to both `chocolatey\bin` and `C:\Program Files\CMake\bin` |
| Python 3.12.10 | `%LOCALAPPDATA%\Programs\Python\Python312` | Plus **junction `C:\Python312`** → real dir (build.ninja calls `C:\Python312\python.exe`) |
| vcpkg deps (x64-uwp) | `C:\vcpkg` + 13 ports, 1.2 h, `VCPKG_MAX_CONCURRENCY=2` | brotli, bzip2, curl 8.21.0, expat, freetype 2.14.3, libiconv, libjpeg-turbo 3.2.0, libpng, libwebp, libxml2, openssl 3.6.3, pixman, pthreads, zlib. **ICU deliberately NOT via vcpkg** (ICU78 would collide with ICU75) |
| ICU 75 | `C:\icu-x64-uwp` rebuilt | import libs via `dumpbin`→`.def`→`lib.exe`; `bin` = `icuuc75.dll`,`icuin75.dll`,`icudt75.dll`,`icudt75l.dat`; `include` = 201 headers + **`sqlite3.h` 3.49.1** (amalgamation; `FindSQLite3` reads version from `C:/icu-x64-uwp/include/sqlite3.h`) |
| harfbuzz | `deps-build\harfbuzz-x64\src\{harfbuzz.dll,harfbuzz.lib}` → `C:\vcpkg\installed\x64-uwp` | vcpkg can't build harfbuzz for UWP (meson); import lib verified UWP-compatible |
| pwsh 7.6.3 | `C:\vcpkg\downloads\tools\powershell-core-7.6.3-windows` (auto-downloaded by vcpkg) | in User PATH for `Src\port` scripts |
| Perl 5.42.2.1 | `C:\vcpkg\downloads\tools\perl\5.42.2.1\perl\bin` | in PATH — required by Python codegen (`generate-inspector-protocol-bindings.py` calls `perl`) |
| Ruby 3.4.10 | **`C:\tools\ruby34`** (real dir, moved from `C:\Ruby34-x64`) | NSIS `/S /D=` silently failed → installed via `7zr` extract of official `.7z`. JSC bytecode generator calls `C:\tools\ruby34\bin\ruby.exe` |
| MSYS2 | `C:\tools\msys64` (base + system + **ucrt64** toolchain) | junction `C:\tools\ruby34\msys64` → `C:\tools\msys64` for `ridk`. `ridk install 3` refused to install (saw existing MSYS2) → installed base-devel + `mingw-w64-ucrt-x86_64-toolchain` via `pacman` directly |
| ccache 4.13.6 | `C:\Strawberry\c\bin\ccache.exe` | build.ninja compile lines still point at the old Strawberry path — single-file ccache satisfies them |

**x64-uwp dependency naming survived** — vcpkg produced exactly the DLLs `Harness.vcxproj` expects
(`z.dll`, `jpeg62.dll`, `libcurl.dll`, `libssl-3-x64.dll`, `libcrypto-3-x64.dll`, `libpng16.dll`,
`libwebp*.dll`, `libxml2.dll`, `brotli*`, `pixman-1-0.dll`, `freetype.dll`, `libexpat.dll`,
`iconv-2.dll`, `harfbuzz.dll`) — verified 18/18 present.

**OS-side RAM optimization** (4 GB box): `SysMain`, `WSearch`, `Spooler`, `PcaSvc`, `TrkWks`,
`whesvc`, `InventorySvc`, `UsoSvc`, `wuauserv` → Disabled; Edge/Yandex autostart removed; Defender
real-time protection off + exclusions for `C:\vcpkg`, `build-*`, `WebKit`, `C:\icu-x64-uwp`.
WinDefend service itself is ELAM-protected (registry `Start` write denied even as admin) — retry
after the next reboot now that Tamper Protection is off in the UI.

**ARM32 blocker resolved by discovery, not install:** the VS Installer catalog still ships the
ARM32 MSVC toolset as an individual component — `Microsoft.VisualStudio.Component.VC.14.38.17.8.ARM`
(and 14.40). So the Lumia line needs no SDK 22621 and no third-party MSVC: install the 14.38 ARM
component, point `arm32-uwp-env.ps1` at it (already SDK 19041), add Ruby (done) — then build.

**Manifest version reality-check:** `Package.appxmanifest` is already at **0.1.8.50** (not .25).
The .30–.33 series (Aug 12, driven by a previous agent session) already loaded **real pages** over
the network ladder: `file:///test.html`, `http://example.com` → `rc=0`, `https://news.ycombinator.com`
→ title resolved, while `dzen.ru` (pure SPA) painted blank-white — a JS-execution gap, not a loader
failure (`WebCoreDriver.cpp:1024`). Next harness build must bump the manifest **above .50**.

**x64 rebuild in flight** (started via scheduled task `ApotheosisNinja`, survives session death):
`ninja -C build-x64-gpu WebCore -j1`. Ninja's dirty set is huge — `.ninja_deps` stores pre-move
absolute paths, so everything pre-reinstall is permanently dirty (the "moved build tree" problem,
see §11). Expect a multi-hour single-core rebuild; generated sources now complete (JSCBuiltins,
Bytecodes, inspector bindings — fixed in order by: sqlite3.h, `C:\Python312` junction, Ruby 7z,
perl PATH, ccache). Cleanup note: the AppPackages prune kept only `unpack_patched` (a keep-list
prefix mismatch also removed `Harness_0.1.8.50_x64_Test` — the appx is rebuildable from source;
the ICU75 DLLs had already been copied out).

---

*"Reviving the Windows Phone web for the people who never let it die."*

---

### AI agents contributing

- **opencode** - https://opencode.ai · model deepseek-v4-flash-free (opencode/deepseek-v4-flash-free) · Aug 2026
- **Claude Opus 5** - (to be added)
