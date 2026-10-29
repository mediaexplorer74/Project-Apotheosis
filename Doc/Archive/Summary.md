# Project Apotheosis — Engineering Research Summary

> Compiled by an AI code agent during deep-dive sessions (June 28 - July 20, 2026).
> Original repo: [Jimmyxiao2009/Project-Apotheosis](https://github.com/Jimmyxiao2009/Project-Apotheosis)
> Reddit thread: [r/windowsphone — Porting WebKitGTK 2.52.4 to Windows 10 Mobile](https://www.reddit.com/r/windowsphone/comments/1ugn2kn/porting_webkitgtk_2524_to_windows_10_mobile/)

## 0. Requirement 5 closes on the x64 bench — Back is the engine's list (`0.2.3.36`, Oct 14, 2026)

The last MVP requirement with no measurement at all now has one, and reaching it named a fourth
member of the `pageConfigurationWithEmptyClients()` family.

- **The engine's back/forward list was empty, and no error said so.** `EmptyBackForwardClient` is a
  silent no-op — `addItem` is `{ }`, `backListCount()` returns 0 — so `canGoBackOrForward(-1)` was
  permanently false however many links were followed. Replaced by `PortBackForward.cpp`: a recording
  client (a `Vector` of `Ref<HistoryItem>` plus a cursor) that answers the five questions
  `BackForwardController` asks. Same shape as `PortStorage.cpp` and `PortWebSocket.cpp`.
- **Two more stubs had to go with it, or Back stays dead.** The port's `LoadingFrameLoaderClient`,
  itself a copy of `EmptyFrameLoaderClient`, returned `nullptr` from `createHistoryItemTree` (so
  `updateBackForwardListClippedAtTarget` bailed before `addItem`) and `No` from
  `shouldGoToHistoryItem` (a policy veto that makes `HistoryController::goToItem` return on line one).
  `createHistoryItemTree` now calls the WEBCORE_EXPORTed
  `HistoryController::createItemTree(LocalFrame&, bool, BackForwardItemIdentifier)` — the real
  frame-tree walk — and `shouldGoToHistoryItem` answers `Yes`.
- **The port's own loads were never recorded, and the control case proved it.** A tap synthesises real
  mouse events, so the engine follows the anchor through `FrameLoader::load()` and that one *was*
  recorded: the trace held exactly `PBF: createHistoryItemTree clip=1 item=https://www.iana.org/...`
  then `PBF: addItem ... n=1 cursor=0`. `n=1` — `example.com`, the document the port had painted, was
  not in the list. The port fetches with curl and feeds `DocumentWriter` directly, so
  `FrameLoader::transitionToCommitted()` never runs and neither does its
  `case FrameLoadType::Standard: history().updateForStandardLoad()`.
- **`apoRecordSessionNavigation` closes both halves.** It writes the URL into the loader
  `FrameLoader::init()` built from an *empty* `ResourceRequest` (that is what
  `DocumentLoader::urlForHistory()` reads, and upstream's own guard drops an empty one), then
  reproduces `updateBackForwardListClippedAtTarget`'s three lines from the exported pieces. Global
  history and visited links, the rest of `updateForStandardLoad`, are deliberately not reproduced —
  this port has neither.
- **Measured, one process, driven only through `LocalState\autocmd.txt`** (`0.2.3.36`):
  `hist engine back=1 fwd=0` → `back` `rc=0 moved=1 url=https://example.com/` → `forward`
  `rc=0 moved=1 url=https://www.iana.org/help/example-domains`. `rc=0` is the engine answering; the
  host-stack fallback logs `rc=-99`.
- **The host stack's cursor was reconciled by `IndexOf`, and a repeated URL broke it.** With
  `[A, B, A]` the lookup answers the oldest `A` in both directions; measured, one Back moved the
  cursor from 3 to 1 and jumped over the entry it had landed on. `ApplyEngineFrame` now takes the
  delta rather than a flag and searches outward from the cursor in the direction of travel.

---

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

## Status update — Oct 3, 2026 — the C# line's MVP: Nokia Archive opens, the language repaints, thirteen sites pass

> Written against `ME.Apotheosis_0.2.1.4_x64`. The live plan at the time was [Plan_MVP.md](Plan_MVP.md) §0a and the
> acceptance table is [MVP.md](MVP.md). This section is the summary of what the session found;
> the same material is in more detail in those two files and in [LESSONS.md](../LESSONS.md).

### What the maintainer asked for, and what was demonstrated

Two asks: **Nokia Design Archive opens properly**, and **the settings switch works, including the
interface language**. Both are now demonstrated on x64, and both were broken by the same class of
assumption — that the UWP-standard route is the right route for this app.

| Ask | Before | Now | Evidence |
|---|---|---|---|
| Nokia Design Archive | untried | opens and paints | `after-load url=https://nokiadesignarchive.aalto.fi/ rc=0 compositing=0`; `diag: contents=720x4431 body=1 nonwhite=766352/777600` |
| Settings switch | labels blank | all captions present | screenshot of the Russian settings page: every control captioned |
| Interface language | did not repaint | repaints live, no restart | `lang 1` → settings page, both pivot headers and the home page repaint |

### The three defects, and why each looked like something else

1. **Cyrillic reached the engine as literal `?` bytes.** Every string-taking `DllImport` was declared
   `CharSet.Ansi`; UTF-8 handed to an ANSI marshaller does not survive. It looked like a font or an
   encoding problem, and it was neither: the same text files carried working Cyrillic, and the home
   page rendered `?????? ? ???????? ??????` only for strings that had crossed the P/Invoke boundary.
   Fixed by marshalling by hand — a private `…Raw(byte[])` `DllImport` and a public wrapper that
   encodes UTF-8 into a NUL-terminated `byte[]`. `[MarshalAs(UnmanagedType.LPUTF8Str)]` was avoided on
   purpose: its behaviour under .NET Native is unverified, and a pinned blittable array needs no
   verification.

2. **Every settings label rendered empty.** `x:Uid="SettingsTitle"` looks up `SettingsTitle.Text`;
   the fork's `.resw` files define bare names, so every lookup missed and rendered nothing. There is
   no error and no log line for a missed resource — the page simply shipped with uncaptioned controls.
   `x:Uid` is gone in favour of `x:Name` and one `RetranslateUi()` pass.

3. **"The language does not repaint the UI" is a property of the platform.**
   `ResourceLoader.GetForCurrentView()` is bound to the *view* language, fixed at launch, so it
   answered English for every key regardless of the picker — and answered for most keys, because the
   `.resw` names match the table's. A `.resw` is the right tool for an app whose language is decided
   before it starts and the wrong one for an app with a language picker. The table in `Str.cs` is
   authoritative now; the three `.resw` files stay in the package as the fork's translation corpus.
   Retiring them is a separate packaging decision, recorded as such in the manifest.

### A fourth defect, in the test channel rather than the product

`goto` refused to run when `_sessionActive` was false — but a visit to Home calls
`WebCoreCloseSession` and clears that flag, and the only thing that can restore a session is a
navigation, which the check refused. The channel could not start itself. A twelve-site run produced
**fifteen batches of `goto ignored: no session` and not one HTTP request**, and every site "failed"
for a reason that had nothing to do with any site. The guard is gone: `NavigateTo` is what creates or
replaces the session, so it is exactly what must stay reachable.

### Two ways the acceptance instrument was lying

Neither could be caught by reading the code, because both produced plausible numbers.

- **The verdict read `nonwhite` from a screen capture**, and the app's dark chrome fills the window:
  all twelve sites scored an identical **99.68 %**. That number was a question about the window asked
  of a measurement about the page. The verdict now uses the engine's own `diag: nonwhite=A/B`, and the
  spread is what makes it readable — example.com 100 % (its body background is `#f0f0f2`), nokia
  archive 99.64 %, wikipedia-m 81.68 %, reddit 75.91 %, w3.org 43.23 %, ya.ru 34.18 %, rbc.ru 7.85 %,
  habr 4.04 %, info.cern.ch 1.86 %, **dzen.ru 0.00 %**. dzen returned `rc=0` and painted nothing,
  which is exactly the failure `rc`-only checking cannot see — and the reason MVP.md says a test that
  checks only `rc=0` is incomplete.
- **A fixed sleep eventually read the wrong page.** On a heavy site the load outlived the sleep, so
  the script read the *previous* site's `after-load` line and `diag`, and the record shifted by one:
  reddit came back with an empty `rc`, and dzen and bad-domain both reported the identical `31.59`.
  Two sites cannot paint the same page. The loop now waits for the site's own URL to appear in an
  `after-load` line — the loader's own statement that it finished.

### The result

`13/13 PASS`, one process, no restart, `alive=True` on every row, with a screenshot and a `diag:` line
per site. Per-site record in `%USERPROFILE%\Documents\apotheosis-smoke\smoke.csv`. The list now
includes **Nokia Design Archive as site 13**, so the acceptance run covers the page the maintainer
actually asked about. Requirements 3, 4, 6, 8 and 10 close on x64 in [MVP.md](MVP.md); 5 (Back) and
7 (history/suggestions) are still unverified because the list does not drive them.

### Open, recorded rather than fixed

- **The engine viewport is a fixed `const int kW = 720, kH = 1080`** shared by the session, the GPU
  surface and the `WriteableBitmap`, so on a large window the page occupies the left part and the rest
  is white. This is a real feature — per-size viewport creation — not a stray constant, and changing
  it touches the present path that works on ARM. Not MVP work.
- **On the failure path the stage `diag` describes the previous document**, because the loader writes
  `rc` and `diag` before it renders the error HTML, and a failed load has not replaced the engine's
  document. The failed row reports `n/a` instead of a number belonging to another site.
- **An in-place appx update can fail on the PRI stage while rolling back cleanly.**
  `0x80073CF9`/`0x80004005`, `Failure in the SystemRegisterRemove state handler`, then `failed to
  register the windows.MRT extension: The data is invalid`. Recovery: back up `LocalState`,
  `Remove-AppxPackage`, `Add-AppxPackage`, restore — the backup is not optional, because
  `Remove-AppxPackage` deletes `LocalState`, which is where `settings.ini`, `history.tsv` and every
  log live. The attempted update rolled back to the previous version intact, twice.

### ARM32

Not started for the C# line. It remains the phase exit criterion: the same build on a real Lumia 950
under W10M, where the App Container, the ABI, the JIT and the font stack each differ — and where every
previous line of this project has already produced a separate bug. `Package.appxmanifest` is still
`ProcessorArchitecture="arm"`; the x64 bundle is produced by an msbuild property override, so the
manifest has not been switched and does not need to be switched back.

---

## Status update — Oct 3, 2026 (evening) — the page zoom did not survive a navigation

> Written against `ME.Apotheosis_0.2.3.9_x64`. The ask was the scaling one: stretch the *mobile* build
> of a site on a wide bench without it becoming a slab, and have *desktop* fit the content area exactly.

### The defect: the shell remembered a zoom the engine had thrown away

`WebCoreSessionLoad` calls `teardownSession()` unconditionally (`WebCoreDriver.cpp:4432`), and
`teardownSession` ends with `g_pageZoom = 1.0f` (`:1832`). Every navigation therefore builds a session
whose frame factor is 1.0 — while the shell's `_pageZoom` still holds the mobile value. `ApplyViewport`'s
no-op test then compares two equal numbers and answers "nothing to do", so the page was presented
unzoomed for the rest of the session. `apoReassertPageZoom`, the port's own guard against exactly this,
was disarmed by the same line: it fires when the frame's factor differs from `g_pageZoom`, and both were
1.0.

What it looked like from the outside, in order:

```
11:41:43.707  fitwindow: 1355x751 zoom=3,29                  <- ua mobile, on the desktop-UA page
11:41:43.783  apply: nothing to do, 1355x751 z=3,29 (current 3,29)
11:41:46.705  goto https://example.com                       <- the zoom is gone from here on
```

and two screenshots — desktop UA and mobile UA, same fitted window, same URL, `2736x1728` each — with
**no sampled pixel differing by more than 12/255**. A re-navigation that changes the user agent cannot
produce a byte-identical repaint; that number is what said "the frame was never re-zoomed", not "the
zoom is invisible".

The fix (0.2.3.9) is in the navigation's completion, the only place the cache can become wrong: if the
*wanted* zoom is not the engine's default, forget it and mark the viewport dirty, so the load's own
pickup re-applies it.

```csharp
if (_vpW >= 16 && Math.Abs(ComputeViewportZoom(_vpW) - 1f) > 0.001f)
{
    _pageZoom = -1f;   // unknown to the shell; the engine has 1.0
    _vpDirty = true;   // SetLoading(false) -> AdoptViewport re-derives _vpZoom too
}
```

Three things are deliberate. The test is on the *wanted* zoom, not the cached one, because the cached
one is the suspect. `_vpDirty` rather than a direct call, so `AdoptViewport` recomputes `_vpZoom` — which
also covers a user agent that changed while the load was in flight. And the whole block is skipped on
desktop, where the wanted zoom *is* the engine default: a navigation there writes no `fitwindow:` line
and spends no repaint, which was verified rather than assumed.

Verified after the fix, same bench, same window: a second `fitwindow: resize rc=0 zoom rc=0 1355x751
z=3,29` 1.6 s after the `goto`, a second `[GPU] page zoom 1.00 -> 3.29`, and a frame showing the phone
layout — the site's own `@media (max-width:700px)` fires, the column fills the area, the text is at
~3.3×. The desktop check afterwards logged `page zoom 3.29 -> 1.00 (layout 1355x751)` and then **no**
`fitwindow:` line after its `goto`.

### `contentsSize()` is in frame-view units — read the marker that way

The marker that finally settled this prints `view->contentsSize()`:
`[GPU] page zoom 1.00 -> 3.29 (layout 1355x2216 CSS-ish, viewport 1355x751)`. `1355` beside a
`1355`-wide viewport reads like "the zoom did not narrow the layout", and that reading was wrong:
`contentsSize()` is in frame-view coordinates, which already carry the zoom. `1355 / 3.29 = 412` and
`2216 / 3.29 = 673` — a 412 CSS px phone layout 673 CSS px tall, which is precisely what the zoom is
for. A second, independent confirmation came free from the Nokia page, whose document box equals its
viewport: the same zoom logs `layout 1355x751` there, i.e. `412 × 3.29` by `751`.

The consequence for the software paint is worth stating so nobody "fixes" it: `paintToRGBA` passes
`view.paint(context, IntRect(IntPoint(), size))` with no scale of its own, and that is correct —
`FrameView::paint` takes frame-view coordinates and applies the factor itself. The narrower layout and
the magnification both come out of the engine.

### Nokia Design Archive: it renders, and the thing over it is this bench's DNS

Both halves of the maintainer's suspicion now have answers, and neither is a rendering failure.

- **It renders, in both modes.** Desktop shows the Network visualisation and the whole filter panel —
  the graph with its node labels ("The Morph Concept", "Y2K Aesthetics", "The Design Process at Nokia",
  "Designing Lifestyles", "Cars and Design Inspiration"), Visualization Type, Keyword Search, Filter by
  Topic, Filter by Date with its histogram. Mobile gives the site's phone layout. **No white fields on
  either side in 0.2.3.9.**
- **What covered the "invention display" is the usercentrics consent modal**, and its backend cannot be
  reached *from this bench*: the bench's resolver (DNS.COMSS.ONE) answers `uct.service.usercentrics.eu`
  with `0.0.0.0`, and the engine logs `phase=didFailLoading … curlcode=6 … Could not resolve hostname`.
  The modal is local markup, so it paints and its buttons are there; it simply cannot complete its own
  handshake. That is a network-policy artefact of the stand, not a port defect, and it is the kind of
  answer that must be verified with `nslookup` before anyone reads it as a bug in this fork.
- **The "huge white fields left and right" are not reproducible here.** They match the blit-scratch
  artefact fixed in 0.2.3.3 (a frame larger than the scratch buffer) and verified then.
- **Gap recorded rather than papered over:** the C# line has no page-tap verb. The C++ harness had
  `tap:`/`taplink:` in `nav.txt` reaching the same `HandleTapAt` a finger uses, so dismissing a modal
  could be scripted there; here it cannot, and the consent dialog therefore cannot be dismissed by
  script on this stand.

### Also landed in this stretch

| Change | Why |
|---|---|
| `touchfab=` setting | the touch-emulation FAB sits in the content area and the bench notebook and the Lumias have real touchscreens |
| `ua mobile\|desktop` verb, `run-smoke.ps1 -Mode` | the two modes were never tested separately |
| `WebCoreSetGpuInitLogFile` called from C# | `g_gpuInitLog` was an empty `static char[1024]`; every port marker went to `OutputDebugString` and no file, so the whole engine-side diagnostic channel was invisible from the managed line — this is the call that produced the marker quoted above |
| `wait` releases the engine thread | it used `Task.Delay(ms).Wait()` *on* the engine thread, so a queued navigation could only run after the batch ended and `goto X; wait N; diag` could never observe its own navigation |
| `min-width:0` on the home tiles | a `1fr` grid track's automatic minimum is its min-content width, and the titles are `nowrap`, so one long title made the track as wide as the sentence and clipped the second column |

---

## Status update — Oct 3, 2026 (night) — the address-box dropdown: three causes for one "гигантское окно" (`.17`–`.19`)

The report: typing a URL and pressing Enter on the notebook raised a panel of visited sites that
covered the whole content area, titles wrapped onto two lines each. It had **three independent
causes**, and the fourth finding came out of trying to photograph the fix.

1. **Enter did not close the list.** `OnUrlKeyDown` navigated without touching it, so the page loaded
   underneath a panel that stayed. The Go button (`OnUrlAction`) always hid it — the two ways into a
   navigation disagreed about the dropdown. Fixed: Enter (and `CommitUrlBox` generally) calls
   `HideSuggestions()` first.
2. **A *dropped* navigation left it hanging.** `NavigateTo` hid the dropdown only *after* its
   `if (_loading) { AutoLog("nav DROPPED, shell busy"); return; }` early-return, so a navigation
   refused by a busy shell left the panel with nothing later to close it. That is the state in the
   user's own screenshot, where the address bar shows the ✕ loading glyph. Fixed: the hide moved above
   the early-return.
3. **`MaxLines = 1` without `TextWrapping = NoWrap` wraps anyway.** Measured: a long page title
   rendered as TWO full lines with no ellipsis, so eight history entries filled the window. Fixed in
   `MakeRow` — explicit `NoWrap` plus `CharacterEllipsis`, tighter padding, and the secondary line now
   runs through `DisplayUrl`, because eight rows of `https://` is eight wasted columns on a 360-DIP
   phone.
4. **The dropdown came back *after* Enter** — and this is what makes the panel appear "по нажатию
   Enter" rather than "пока набираешь". The commit writes the normalized URL into the box; that
   write's `TextChanged` is delivered **after** `_urlSyncing` has been cleared, so `OnUrlChanged` saw
   `_urlFocused == true`, matched the URL against the history the commit had just appended, and
   reopened the panel — one row reading `dzen - поиск / cn.bing.com/search?q=dzen` over a page that
   was already loading. Fixed by giving the shell one way to write the box, `SetUrlBoxText`, which
   remembers the string it wrote; `OnUrlChanged` now treats "the text equals what the shell last
   wrote" as a shell write and hides. The transient flag alone could never be enough.

**A measurement trap, worth its own line.** The `suggest` verb faked the box *text* but not the box's
*focus*, so `OnUrlChanged` — seeing `_urlFocused == false` — hid the panel immediately after the verb
showed it. The log then read `ui suggest 'dzen' rows=5 vis=1` while the screen was empty, twice (16:33
and 16:40), and the honest reading of that was "the panel does not render". It does; the verb was
measuring a state no finger can reach. Fixed by setting `_urlFocused` in the verb (a script cannot
focus a control — `synthetic-input-cannot-reach-uwp`) and letting the shipping chain build the panel,
plus a deferred probe: `ui suggest+700 vis=1 h=251 boxfoc=1`. A panel that is shown and then hidden
looks exactly like one that was never drawn if you only look once; 700 ms later is what tells them
apart.

**Height is now a fraction of the window, not a constant.** `SuggestScroll.MaxHeight` is clamped in
`ShowSuggestions` to `max(120, min(250, contentArea × 0.34))`. 250 DIP is a minority of a 912-DIP
notebook window and most of a 640-DIP phone window, and the phone is the shipping target. Measured on
the bench: `h=251`, five visible rows, one clipped line each.

**Verified on `0.2.3.19`:** `suggest dzen` → `rows=6 vis=1`, `+700 vis=1 h=251 boxfoc=1`, panel
photographed over `about:home` with single-line rows and scheme-less URLs; `enter` →
`ui enter 'https://cn.bing.com/search?q=dzen' vis=0`, and 14 s later the page was clean with the
panel gone and the committed URL in the bar.

**Build note (cost 20 minutes):** the C# line installs as an **appxbundle**. Building it with
`-p:AppxBundle=Never` produces an `.appx` that `Add-AppxPackage` refuses on an in-place update of a
package that has resource packs — `0x80073CF3`, "dependency or conflict validation". Build with
`-p:AppxBundle=Always` (or no property at all) when the install is meant to replace `.17`.

---

## Status update — Oct 3, 2026 (late night) — the ARM line: docket steps 1–2, and the Debug trap

The maintainer put the Lumia on charge and asked for the ARM line to start. Steps 1–2 of the docket in
[BUILD-ARM32.md](../BUILD-ARM32.md) need no device, and both are now done.

**Step 1 — the export gap is closed, and it was the one that would have killed the first run.** The
staged ARM payload exported **37** symbols against **39** on x64, missing `WebCoreSessionResize` and
`WebCoreSetPageZoom` — the viewport pair added to `WebCoreDriver.exports.def` after that payload was
linked. Since a function absent from the `.def` fails as `EntryPointNotFoundException` at *runtime*,
not at link time, the app would have thrown on the first viewport apply, after `about:home` had drawn:
a failure that looks like an engine bug and is a stale artifact. Relinked from current sources
(`Compiled 14/14`, `unresolved=0`, `duplicate=142` on `/FORCE:MULTIPLE`), `Exports: 39 functions`.

A side finding: `stubs-font-uwp.cpp` (edited 27 Sep, newer than the old payload) is **orphaned** — no
build script and no vcxproj compiles it; it survives only as comments in `WebCoreDriver.cpp`,
`stubs-other.cpp` and itself. So the payload's staleness was entirely `WebCoreDriver.cpp`/`.h`.

**Step 2 — the ARM appx builds, first try, both configurations.** `Apotheosis_0.2.3.19_arm.appxbundle`
(Release, 46.9 MB) and `..._arm_Debug.appxbundle` (47.9 MB). What was checked on the artifacts rather
than assumed:

- `ProcessorArchitecture="arm"`; all 22 engine DLLs plus `Apotheosis.dll` at the package **root**, so
  the `Link=%(Filename)%(Extension)` flattening works (inside the AppContainer there is no
  `SetDllDirectory`);
- **every** binary `0x01C4` ARMNT (25 in Debug, 24 in Release) — no x64 leak;
- every non-API-set import of `WebCoreDriver.dll` and `libcurl.dll` resolves to a packaged file, and
  the `-arm` suffix in libcurl's `libcrypto-3-arm` / `libssl-3-arm` imports matches the shipped
  filenames exactly;
- the **packaged** `WebCoreDriver.dll` carries all 39 exports, viewport pair included;
- `resources.pri` present with 94 `Language-EN-US`, `isDefault="true"` strings — a missing PRI is what
  once blanked every label, and this one is not missing; the bundle additionally ships
  `language-ru.appx` / `language-zh-hans.appx` with `<ResourcePackage>true</ResourcePackage>`;
- **no log or trace file in either package.** The "49 MB `port-trace.txt` inside the shipping appx"
  item is withdrawn — it is a runtime `LocalState` artifact and was never packaged.

**The one real obstacle is not in the engine — it is Debug vs Release.** A Debug UWP package's manifest
depends on `Microsoft.NET.Native.Framework.Debug.1.7` and `Microsoft.VCLibs.140.00.Debug`
(≥ 14.0.33519.0) and ships `ucrtbased.dll`. A retail Lumia has none of these, so `Add-AppxPackage`
fails on the unmet dependency before any of our code runs. Release depends only on the retail
frameworks. **Deploy Release|ARM.** This also answers the long-open CRT question: `MSVCP140_APP` /
`MSVCP140_2_APP` / `VCRUNTIME140_APP` are not payload files by design — `Microsoft.VCLibs.140.00`
provisions them, which the manifest requires at `MinVersion 14.0.33519.0`. That minimum is itself the
gate: check the phone's installed VCLibs over WDP **first**, because too old means a refused install,
not a loader error.

Remaining on the ARM line: step 3 (a managed WDP deploy path, backing up `LocalState` first) needs the
device address; step 4 is the first run; step 5 lists the named suspects (App Container, ABI/Thumb bit,
.NET Native, the font stack, the never-executed image, CRT provisioning).

---

## Status update — Oct 3, 2026 (after late night) — the `.24` pull answered "gesture or close", and the answer is neither: the engine dies idle, and it dies for a reason

> `0.2.3.25` built, audited and deployed to the Lumia (`ME.Apotheosis_0.2.3.25_arm__cj4nrm0qt9s1y`),
> **unlaunched**. The fix below is a diagnosis carried to a fix, not a verified cure: only the
> maintainer's next run can say whether the site is gone.

**What the maintainer reported.** Four tests: (1) instant crash on the first navigation to dzen.ru;
(2) a white background; (3) and (4) dzen.ru opened, then died quietly after a couple of seconds,
blocking their two-finger pinch and their scroll.

**What the trace says: no gesture was in flight at any death.** The whole 5,838-line file contains
`gesture:` bursts for `close` and the idle `live` tick only — **no `scroll`, `click`, `zoom` or
`resize` at all**, in a run whose point was that the maintainer *did* pinch and scroll. The two fatal
sessions (B and C, tests 3 and 4 — twins) read, in order: `gesture: close end n=1 dt=0ms idle=8495ms`
/ `idle=8981ms`, five `gesture: live` ticks, `teardown: enter why=load`, `teardown: exit why=load
dt=17ms` / `dt=8ms` (**completed**), ~1,400 lines of load / MIME / timer work, then the AV. So the
deaths arrived **idle, mid-load, after a completed teardown**, and the frozen pinch and scroll are a
consequence of a dead process rather than its cause. Session A (test 1) is a different class
altogether: it ends in write-AVs to address 0 in `SharedLibrary.dll+0x363B4D` and
`CoreUIComponents.dll+0xD4347` — platform and runtime code, with no `teardown:` bracket and no engine
fault, i.e. the same pair that ended `.23` §1 and not something the port can inspect.

**The instrument has a measured false positive, and it should be written down.** `gesture: … end` is
written by a *later* mark (the idle sweep, which the 1 Hz `live` tick also runs), not by the call
returning. So an open burst means "died during this gesture **or** before the next mark" — session A
is exactly that: `gesture: close enter` with no `end`, in a session that died in code the port never
sees. Read an open burst together with the fault class, never instead of it.

**The root cause of B and C, named to the instruction.** `Document::notificationClient()`
(`WebCore/dom/Document.cpp`) ends in `return &NotificationController::from(page.get())->client();` with
**no null check**, and `NotificationController::from(Page*)` is
`downcast<NotificationController>(Supplement<Page>::from(page, "NotificationController"_s))`. The
supplement is installed by the **embedder** — the only in-tree caller of `provideNotification()` is
`WebKitLegacy/mac/WebView/WebView.mm` — and this port builds its Pages with
`pageConfigurationWithEmptyClients()`. So the lookup answers *absent* and the unguarded dereference
reads the value pointer of a null hash-table entry: address **+8**, exactly the `faultaddr=0x8` the
death net recorded. The disassembly agrees at the instruction — `1037d160: 6885 ldr r5, [r0, #0x8]`
with `r0 == 0`, reached from a `bl` into an ICF-folded `WTF::HashTable::lookup` whose key literal on
the stack is `??_C@_0BH@CBCLEOFL@NotificationController?$AA@`. **This supersedes the `.23` reading**
that `faultaddr=0x8` meant "a dead `Document` whose frame/page chain is gone": that chain is `cbz`-
guarded at every step, and the missing supplement explains the same address without it.

**The fix is the client the embedder owes.** `PortNotificationClient` implements `NotificationClient`'s
six pure virtuals (`show` → `false`, every permission → `Denied`) and one immortal instance is
installed into every Page the driver creates, at all four `Page::create` sites (`buildSession`,
`WebCoreRenderHtml`, `WebCoreLoadUrl`, the w×h probe). Immortal is not a style choice:
`NotificationController` holds a **reference**, not an owner. `provideNotification` is already exported
by `WebCoreFull-*` on both lines, so this is a **relink, not a rebuild** — which matters, because a
WebCore header edit would have dirtied 362 unified sources.

**How `.25` was built and checked.** x64 first (compile + link, `unresolved=0`, 40 exports, staged with
a matching SHA256) because that line is the cheap place to catch a signature mistake — and it did catch
one: `WTF::Ref` has no `operator*` in this tree, so the four call sites take `*page.ptr()`. Then the ARM
relink (`Compiled 14/14`, `unresolved=0`, 40 exports, staged) and the bundle
(`Apotheosis_0.2.3.25_arm.appxbundle`, 46.1 MB, Release): `AUDIT PASSED`, 30 binaries all arm,
`WebCoreDriver.dll` 30.6 MB at the package root, 40/40 exports, `resources.pri` present. The map was
regenerated by the relink (2026-10-03 22:29), so the `.24` addresses are dead: every WebCore symbol
moved **+0xB0**, and the driver's own code moved with the inserted block.

**A build-script defect found on the way, and it was hiding a whole step.** `llvm-lib` cannot infer the
target machine from a `.def` alone (MS `lib.exe` reads it off the `.obj` files, and here there are
none), so `llvm-lib /OUT:… /DEF:…` fails with `/def option requires /machine to be specified`. Under
PowerShell 5.1 that stderr *terminated* the script before its export count and before its payload
staging — a clean link then leaves the previous DLL in `native\<arch>`, which is the "my fix did
nothing" failure the staging comment in the script warns about, and the ARM env script does the same
thing on its own `cl` banner. Under PowerShell 7 the identical call fails **silently** and the import
library is never regenerated at all. Fixed with `/MACHINE:$Machine` and an explicit exit-code check;
the ARM line is now run under `pwsh` (7), not `powershell.exe` (5.1).

**Two smaller corrections to standing notes.** `mem.txt` in this pull carries
`PRESSURE level=low used=9,8MB` 16 ms after a process start, so the memory-pressure handler **has**
fired — the claim that it never has is retired (the quota stays excluded: peak `used=192,2MB pct=12`).
And the pull has **three `started` lines for four tests**, so one launch never reached engine init —
which is also why "four tests" produced only three `DEATHNET: installed` blocks. Finally, `.24` has
**no abort deaths at all**: the three `ABRT` substring matches in the file are inside
`signal(SIGABRT)`, and the `.23` teardown-abort class (`Inspector::AgentRegistry::append`) did not
recur.

---

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

### 2026-10-03 (evening) — the Lumia dzen.ru death: the crash site is named, and the ARM line gets a death net

**The address recovery that unblocked everything.** `port-trace.txt` names every object by raw
pointer, and no log on the device records a module base, so those pointers were unreadable offline.
The base is recoverable from `Src/port/WebCoreDriver-arm.map` after all: a 64 KB-aligned base pins the
low 16 bits of every RVA, so only map symbols with the same low bits can be candidates — and exactly
**one** of the map's 5327 `??_7` vtables ends in `8B38`. That one is `??_7Timer@WebCore@@6B@` at RVA
`0x1858B38`, giving base **`0x5F9D0000`**; the two other vtables from the same window land on real
`??_7` entries at that base too, so the triple intersection settles it.

With the base, the map (188 111 symbols) resolves the whole crash window:

| Recorded | Resolves to |
|---|---|
| `vtbl=61228B38` (dying timer) | `??_7Timer@WebCore@@6B@` — a plain `WebCore::Timer` |
| `vtbl=61233F8C` (#129–130) | `??_7EventLoopTimer@WebCore@@6B@` |
| `vtbl=6126D994` (#125–128) | `??_7DeferrableOneShotTimer@WebCore@@6B@` |
| `caller=60226687` | `ParsedContentType::setContentTypeParameter` +0xA7 |
| `caller=602265B7` | `WebCore::collectHTTPQuotedString` +0x22F |

So the death is **inside the callback of a one-shot `WebCore::Timer`**, on the main thread, in a burst
fired right after four tracker loads, with MIME content-type parsing (`String::lower("charset")`) the
last thing that completed — and it left no SEH line, no dump and no WER record. `0x633BBF0D` sits
above `base+SizeOfImage` (`0x618C9000`), so at least one second module is involved in that marker; it
is not identified and nothing depends on it.

**Why an engine-side probe was not available.** `ninja -n -C build-arm32-gpu WebCore` reports **7400
pending edges**, and the single unified object carrying `ThreadTimers.cpp` still needs **6462**: the
generated `DerivedSources` are stale, so the PCH regenerates and every unified source goes dirty. The
ARM WebCore tree cannot be rebuilt incrementally at all — that is the half-day rebuild this line has
always avoided. The tree is fine to *link* against (1516 objects present; no cmake input newer than
`build.ninja`, so ninja will not reconfigure).

**The fix for the diagnosis, not the bug: a port-side death net** in `Src/port/WebCoreDriver.cpp`.
On ARM32 the port's SEH wrappers compile to nothing — `APO_SEH_SUPPORTED 0`, because clang-cl will not
emit `__try` for `thumbv7a`, which the source's own comment admitted as *"there is no ARM guard
yet"*. A Vectored Exception Handler is a runtime call rather than a compiler feature, so it does reach
that target: installed with `First=1`, it runs ahead of every SEH frame, observes only, and always
returns `EXCEPTION_CONTINUE_SEARCH`. It logs the codes that can end a process — access violation,
illegal instruction (`0xC000001D` = WebKit `CRASH()`), stack overflow, fast fail, heap corruption,
breakpoint — each with a `module+0xRVA` stack; plus `signal(SIGABRT)` **and**
`_set_abort_behavior(0, _CALL_REPORTFAULT)`, the flag whose default turns `abort()` into the Watson
fast-fail behind every silent death in this family.

`RtlAddVectoredExceptionHandler` is resolved at run time from the already-mapped ntdll. The C++
harness links `ntdll.lib` via `#pragma comment` and that works on x64, but linking it here would add
ntdll to the driver's import table on both architectures — a change to the bench line for a
device-only diagnostic. The two-call lookup leaves the x64 link untouched and reports a refusal as a
logged line rather than a build error.

**`0.2.3.23`** is built, `AUDIT PASSED` (30 binaries all arm, driver at the package root, `WebCore.dll`
absent, 40/40 declared symbols exported) and deployed to the Lumia. The next device run must scroll
with a real finger — synthetic input does not reach the UWP input stack, so a scripted gesture cannot
produce this death.

### 2026-10-03 (late) — the death net's first device run: it fired, and the death is not one death

`0.2.3.23` on the Lumia, six launch attempts by the maintainer, four of which reached the engine and
left a trace. Three answers, and one of them closes a question the net was built to ask.

**The App Container permits a Vectored Exception Handler.** `DEATHNET: RtlAddVectoredExceptionHandler
-> 091EE738` — a real handle, so the port's own net installs on the device exactly as it does on the
bench. And `_set_abort_behavior prev=0x2` is measured confirmation of the mechanism behind every
earlier silent death: `_CALL_REPORTFAULT` **was** armed, so `abort()` was a Watson fast-fail that ran
no handler. Clearing it is why `ABRT:` reached us.

**Three signatures in one file.** §2 (the long run) dies mid-flight, inside a one-shot
`WebCore::Timer` callback, after `String::lower("charset")` inlined into
`ParsedContentType::setContentTypeParameter` — an **access violation reading address `0x8`** inside
`Document::notificationClient()` (`WebCoreDriver.dll+0x37CB05`). §3 and §4 die with **identical**
stacks: `abort()` from `JavaScriptCore.dll+0x1EA01A` =
`WTF::Vector<WTF::UniqueRef<Inspector::InspectorAgentBase>>::expandCapacity<FailureAction::Crash>`, i.e.
`Inspector::AgentRegistry::append` (`m_agents.append` — the only caller of that exact instantiation) —
and both **at teardown**, after a clean `quiet=15` settle and a 15 s idle drain, with `nonWhite=0`: the
page had never painted. §1 ends on write-AVs to 0 in `SharedLibrary.dll` and `CoreUIComponents.dll`.

Read together the shape is one thing — **the engine touching something already destroyed**: a dead
`Document` reachable from a timer callback, and an agent registry appended to while the session tears
down. That is the family the board already carries: the dead `ScheduledTask` aborts of 2026-09-24, the
hidden `Page`s that outlive teardown, and the throwaway-`Page` design still open.

**The memory hypothesis is dead, measured.** `mem.txt` peaks at **179.5 MB against a 1536 MB quota**,
`pct=11`, `level=low` on every sample. Nothing in this session came near a limit, so no theory may
reach for the quota again without explaining that file.

**Two gaps the next build must close.** The port trace has **no input vocabulary at all** — its
prefixes are `rl`, `APOTRACE`, `settle`, `wtimer`, `loader`, `rlsleep`, `tail`, `pump`, `twdct`,
`DEATHNET`, `VEH`, `rllock`, `PS`, `spa` and the font markers, and not one of them is a gesture. So
this file cannot tie a death to the pinch or the scroll that the maintainer saw, and the teardown
aborts look at least as much like "the app was closed" as "the gesture killed it". A marker on the
engine entry points a gesture can reach is the cheapest thing that fixes that. Second: the stack capture
is partial — `RtlCaptureStackBackTrace` returned **0 frames** in the VEH and **3** in the ABRT handler,
so the caller above `expandCapacity` is not in the log, and the frame count is printed for exactly this
reason.

Minor, flagged and not concluded: one timer (`#90`) reported `vtbl=6126EC44`, which resolves into a
`.Lswitch.table`, not any vtable — and it entered and exited cleanly, so it is not the killer.

---

### 2026-10-03 (night) — the port learns to name the gesture, and `.24` is on the phone

The `.23` run named three death sites and could not name a single gesture. That is not an
impression: the 4019 lines of `port-trace.txt` were counted for `swipe`, `pinch`, `zoom`, `tap`,
`gesture`, `scroll`, `input` and `touch`, and every one of them was **zero**. So "the maintainer
scrolled and it died" and "the maintainer left the app and a teardown died" were the same file — and
the two teardown aborts in it, sitting after a clean `quiet=15` settle and a 15 s drain, looked
exactly like a closed app. `0.2.3.24` closes that gap with a port-side instrument and no engine
rebuild: **one relink**.

**The grammar.** Every engine entry point a finger can reach now writes a burst:

```
gesture: scroll enter
gesture: scroll cont n=12 dt=612ms
gesture: scroll end n=37 dt=1902ms idle=1600ms
gesture: live n=142
```

| Convention | Why |
|---|---|
| one `enter` per burst, not one line per call | a drag drives `WebCoreScrollBy` from `PumpScroll`, which re-posts only after the previous call has returned, so a drag is many sequential calls — a line per call would bury the file it exists to explain |
| `cont` at most every 500 ms, carrying the running count | a long drag keeps a cadence, but a bounded one |
| `end` after 1.5 s with no call | written by a sweep that *every* mark runs, including the 1 Hz live tick; without it a finished drag and a drag that was still in flight when the process died would both end the trace with a `cont` line |
| **a `cont` with no `end` after it** | **is the statement "the engine died during this gesture"** — the same enter/exit balance that made the timer death readable (100 `wtimer` enters against 99 exits) |
| `gesture: live n=` | a liveness cadence, not a gesture: `WebCoreLiveTick` is a 200 ms timer whenever the app is idle and foreground, so it is rate-limited to one line per 2 s |

**The teardown is bracketed too, and that is the sharper half.** `teardownSession()` has five
callers and the `.23` file could not tell them apart. It now takes a `why=` and writes
`teardown: enter why=…` / `teardown: exit why=… dt=…ms`:

| `why=` | Caller |
|---|---|
| `load` | `WebCoreSessionLoadImpl` — **every navigation tears the old session down first** |
| `load-failed` | `buildSession` returned non-zero |
| `close` | `WebCoreCloseSessionImpl` — the app closing |
| `paint:frameGone` | `finishInteractionPaint` — a gesture's own paint found the frame gone |
| `click:frameGone` | `WebCoreClickAtImpl`, same |

An `enter` with no `exit` puts the death *inside* the teardown and names the caller; an `exit`
followed by the abort puts it *after*, in the caller. Two of the five callers are gesture routes, so
the earlier reading — "the teardown aborts look like the app was closed" — was a guess between
three possibilities, not two.

Marked at the boundary as well: `WebCoreCloseSession` (before the impl's `g_inPump` refusal, so "the
app asked to close and the port said no" is not lost), `WebCoreClickAt`, `WebCoreScrollBy`,
`WebCoreSetPageZoom` and `WebCoreSessionResize` (both marked *before* their early returns, so a
refused request still leaves the finger's trace). Deliberately **not** marked: `WebCoreSetPageScale`
— page scale left the managed line in 0.2.3.14, so a verb for it would be one nothing can trigger.
And `WebCoreSetPageZoom`'s entry cannot fire once per frame, because the present path's re-assert
does not come through the export at all (`apoReassertPageZoom` sets the factor on the frame
directly).

**The build.** x64 first, because the port source is shared: 13/13 sources `EXIT=0 errors=0`, and
the only warnings in `WebCoreDriver.cpp` are the three that were already there. Then the ARM relink
off the existing `WebCoreFull-arm32.lib`: `unresolved=0`, `duplicate=142` (JSC and WTF twice, as
designed), **40 exports via `.def`**. The audit returns `AUDIT PASSED` — 30 binaries all arm, the
30.6 MB `WebCoreDriver.dll` at the package root, `resources.pri` present, **40/40 declared symbols
exported** — and its imports are the engine's own dependencies and nothing new. Version `0.2.3.24`,
deployed to `mc-00111d42b575` as `ME.Apotheosis_0.2.3.24_arm__cj4nrm0qt9s1y`, deliberately
**unlaunched**: the crash needs a real finger (`synthetic-input-cannot-reach-uwp`, in the memory
set), so the run is the maintainer's.

**A finding on the way, on the x64 line.** The shipped `native\x64\WebCoreDriver.dll`
(2026-10-03 09:11) carried **39** exports against a `.def` that declares **40** — missing exactly
`WebCoreReleaseMemory`, the symbol added after the 2026-10-03 scroll measurement. Its only managed
caller wraps it in `catch (Exception ex) { MemLog("release FAILED…") }`, so a stale payload does not
crash the bench: it silently stops releasing memory and leaves one `release FAILED:
EntryPointNotFoundException` line. Rebuilt (`-Arch x64`, `unresolved=0`, 40 exports), so both
payloads now match the `.def`. The lesson is the audit's own, from the other side: a symbol missing
from the `.def` fails at *link* time, but a symbol present in the `.def` and absent from the
*artifact* fails at run time.

**Reading the next run — and one trap.** The relink regenerated `Src/port/WebCoreDriver-arm.map`
(2026-10-03 21:40, 42,000,503 bytes), so **the `.24` run's base address is not `0x5F9D0000`** and
every `.23` address is dead. Re-derive the base from the map of the DLL that produced the log and
prove it with *exact* symbol hits — the technique is written up under
`symbolize-from-logs-no-debugger` in the memory set, the number is not. Then grep the pull for
`gesture:` and `teardown:` **first**: those lines decide what the rest of the file means.

**Open at the end of this entry.** The instrument is deployed and unproven — no device run has
produced a `gesture:` line yet, so the grammar is designed, compiled and shipped, not measured. The
three `.23` signatures stand exactly as recorded; which of them a finger causes is what the next run
must answer, and the two questions it was built for are: does a `cont` with no `end` precede the
first `VEH:` line, and is the teardown abort preceded by `teardown: enter why=close` (an app that
was closing) or by a gesture verb (an app that was being used).

---

## Oct 3, 2026 (latest) — the `.25` device run: the notificationClient AV is gone, and the death is now a release assert

The maintainer made three attempts on the deployed `0.2.3.25`: *immediate crash, white window, content
for a second or two and then a crash*. The pull is `Src/tools/device-logs/20261003-224203/`
(`port-trace.txt` 4,366 lines, `mem.txt`, `stage.txt`); it holds **two** engine processes for three
launches, so the immediate crash never reached engine init and left no trace at all.

**The white-window attempt is not an engine death.** It loaded `sso.dzen.ru/install`, painted nothing
(`nonWhite=0`) and ended on the platform pair `SharedLibrary.dll+0x363B4D op=1 faultaddr=0x0` and
`CoreUIComponents.dll+0xD4347 op=1 faultaddr=0x0` — null *writes* in the .NET Native runtime and in
XAML. Those two lines are the **only** `VEH:` entries in the whole pull.

**The fix worked.** The access violation reading `0x8` inside `Document::notificationClient()` — the
fault that killed both `.24` fatal sessions — **did not recur** in either process. The
`PortNotificationClient` supplement is confirmed against the site it was built for.

**The content attempt dies differently.** It painted real dzen.ru (`nonWhite=265380/777600`), the
stall-watch fired, and then, ~16 s in, a one-shot `WebCore::Timer` entered `fired()` and never
returned: `ABRT: abort() reached the CRT signal handler`. The three captured frames resolve to
`ApoPortAbortHandler+0x45` ← `ucrtbase.dll+0x39DDF` ← **`WebCoreDriver.dll+0xECEB`**, and RVA `0xECD0`
in the `.25` map is `?WTFCrashWithInfo@@YAXHPBD0H@Z` — the four-argument release-crash helper from
`WTF/wtf/Assertions.h`, emitted into the driver's own object, whose body is `blx` through the IAT slot
`0x1184B85C` that the import table proves is `abort`. **So the engine hit a release assert on a real
page**, not a fault. The last engine work before it is MIME content-type parsing —
`ParsedContentType::setContentTypeParameter` calling `String::lower("charset")`, the same last-completed
work as the `.23` and `.24` deaths.

**Reading `wtimer` correctly matters.** It is emitted in `ThreadTimers.cpp::sharedTimerFiredInternal`
around `item->timer().fired()`: `obj` is the `TimerBase` address, `vtbl` is read before the call, and
`left = m_timerHeap.size() - 1` is **the batch depth, not a countdown and not milliseconds**. The object
that dies (`0A5D87D0`) appears 21 times in the session, twenty of them clean — one long-lived timer or a
recycled slot; this trace cannot tell which.

**Open.** Which assert it is is *not* known: `WTFCrashWithInfo(line, file, function, counter)` spills all
four arguments into its own frame (`str r3,[sp,#0xc]` … `str r0,[sp]`) and they are still on the stack
while the CRT handler runs, so a handler that scans upward for a return address in
`[&WTFCrashWithInfo, +0x40)` and reads `sp+0 … sp+0xc` names it — port-only, no WebKit rebuild. That is
the `.26` instrument. `mem.txt` peaks at 215.5 MB of 1536 MB, so OOM stays excluded.

Two corrections recorded the same night: the `WebCore::provideNotification` RVA in the `.25` reference
table is **`0x86a208`**, not `0x869208` (the map's VA column is `1086a208`; the wrong value came from
reading the section:offset column as an RVA), and the `ABRT stack (3 frames)` limit is the known
`RtlCaptureStackBackTrace` behaviour, already documented from the `.23` pull.

## Oct 9, 2026 — the `.26` instrument: the assert's own arguments become readable

`0.2.3.26` is built, audited and **deployed to the Lumia**, installed over `0.2.3.25` (the device was
reachable over WDP at `192.168.1.35` and the package list confirmed
`ME.Apotheosis_0.2.3.25_arm__cj4nrm0qt9s1y` before the push). Nothing has been run on the device yet:
this build exists only so that the next dzen.ru death names itself.

**What the instrument is.** `ApoPortScanForAssert()` in `Src/port/WebCoreDriver.cpp`. The `.25` pull left
one question — *which* release assert kills the engine — and the answer is physically on the stack:
`WTFCrashWithInfo(line, file, function, counter)` spills its four arguments into its own frame before it
calls `abort()`, and nothing unwinds that frame before the CRT signal handler runs. So the scan walks up
from the handler's own frame looking for the argument **triple by shape**: a word that
`GetModuleHandleExW(FROM_ADDRESS)` attributes to an image and that reads as printable NUL-terminated ASCII
**ending in a source suffix** (`.cpp`, `.hpp`, `.mm`, `.cc`, `.cxx`, `.h`, `.c`), the next word as
`function`, and `line`/`counter` bracketing the pair.

Three deliberate choices, each answering a way this could lie:

- **Shape, not a symbol range.** There is no symbol to take: `WTFCrashWithInfo` is `inline` in
  `WTF/wtf/Assertions.h`, is emitted into `WebCoreDriver.arm32.obj` itself (`llvm-nm` shows it `T` there),
  and this TU includes no WTF header. A `[&WTFCrashWithInfo, +0x40)` range would need a declaration the
  port cannot get.
- **Bounded reads.** Every string read is clamped by that image's own `SizeOfImage` (`e_lfanew` at `+0x3C`,
  the optional header at `+24`, `SizeOfImage` at `+56` of it), so a string in the last page of an image is
  never read past its end — and a pointer that is not in any image is never dereferenced at all.
- **Every hit, not the first.** The scan logs up to four hits and dumps the words around each, because a
  scan that stops at its first match lets a single false positive hide the truth.

It is called from **both** witnesses: `ApoPortAbortHandler` (the `abort()` route) and the `0xC000001D`
slot of `ApoPortDeathNet` (the trapping `RELEASE_ASSERT` route), so both shapes of that family are named
by the same code.

**Build facts (both lines in step).** ARM: `Compiled 14/14`, `[link] EXIT=0 unresolved=0`,
`Exports: 40 functions`, `SHA256 7A0D5B62…`; x64: `Compiled 13/13`, `unresolved=0 duplicate=0`,
`Exports: 40`, `SHA256 03FD0951…`. Bundle `Apotheosis_0.2.3.26_arm.appxbundle` (48.4 MB, Release),
`Test-AppxPayload.ps1` → `AUDIT PASSED`: 30 binaries all arm, driver at the package root,
`resources.pri` present, 40/40 declared exports. The freshly linked, staged and packaged
`WebCoreDriver.dll` all hash to `7a0d5b62c203513d…`, so the audit ran against this build's own artifact
and not a previous one.

**Unverified then, and wrong in hindsight.** No device run had read the instrument when this section was
written. `0.2.3.26` and `0.2.3.27` have since run on the device and named nothing — the reader was dead
code in both, and the reason is in the next section. The `.25` claim that the `notificationClient` fix
removes the device death still stands as **confirmed only in part**: the AV reading `0x8` did not recur,
and a release assert replaced it.

## Oct 9, 2026 — the assert reader was dead code, and the disassembly is what said so

`0.2.3.26` was deployed (`ME.Apotheosis_0.2.3.26_arm__cj4nrm0qt9s1y`, HTTP 202 in 20 s) and `0.2.3.27`
after it (HTTP 202 in 184 s). The maintainer reproduced on both — a pinch and a scroll on each. Pull
`Src/tools/device-logs/20261009-101459/` (`port-trace.txt` 1,053,149 bytes / 27,374 lines; the file
accumulates across installs, so its five processes are `.25`-era, `.26`-era and the two `.27` runs).

**The assert is deterministic.** Every abort in the file is the same event: `ABRT #2 WebCoreDriver.dll`
at `+0xECEB` (`.25`), `+0xEEC5` (`.26`), `+0xEECD` (`.27`) — the same **`+0x1B`** inside
`?WTFCrashWithInfo@@YAXHPBD0H@Z`, in every build. The class of death has never changed.

**The reader never worked.** `.26` wrote `no argument triple in 512 words`; `.27` wrote
`no argument triple in 1024 words from 09CBE888` and then a 384-line wide window in which **every line
held the same word**. That is not a stack. It is one value printed 384 times.

**The cause is the reader, not the window size** — which is what the first write-up of this run said,
from the `.26` log alone, and it was wrong. The walk was written as

```cpp
void* anchor = nullptr;
void** p = (void**)&anchor;      // p[i] for i > 0 reads past the object: undefined behaviour
```

`llvm-objdump` on `WebCoreDriver.arm32.obj` shows the 384-iteration loop loading **always `[sp,#0x20]`**
— the one slot holding `anchor`:

```
162: movs  r5, #0x60          ; i = 96
16e: ldr   r0, [sp, #0x20]    ; the same word, every iteration
184: adds  r5, #1
186: cmp.w r5, #0x1e0         ; 480
```

An out-of-bounds subscript on a promoted scalar local is undefined, so clang treated every access as
the same object and hoisted the load out of the loop. **Both nets were dead code on every build** — the
shape net did not "miss", it never read real memory. The lesson is written down separately, because it
generalises: a stack reader needs a base the optimizer cannot promote and volatile reads, and every
printed line must carry its own address so a repeat is visible in the log rather than silent.

**`.28` makes it a stack reader.** The base is the caller's captured-frame array — a pointer parameter
— every word goes through `ApoReadStackWord()`, a `void* volatile const*` read, and each line prints
its own slot address. The frame net (the `#2` return address, whose four lower neighbours *are* the
spilled arguments, `a = i - 4`, validated by shape against the copies in our own `frames[]`) and the
wide window both stay, now over `0 … 479`. A port-only change; still no WebKit rebuild.

**Gesture correlation, both runs.** The maintainer's two `.27` reproductions are the file's fourth and
fifth processes, and each ends inside a one-shot `WebCore::Timer` callback — the vtable is
`??_7Timer@WebCore@@6B@` (VA `0x1185A038`), the plain one-shot, not a `DeferrableOneShotTimer` — so the
death site is the same one `.23`–`.26` found.

* **Pinch.** `gesture: zoom enter` → `resize enter` → `zoom end n=1 dt=0ms idle=3592ms` →
  `resize end n=1 dt=0ms idle=3410ms` → `gesture: live n=50` → death. The pinch completed; the process
  died ~3.4 s later, idle.
* **Scroll.** `gesture: scroll enter` → `gesture: live n=31` → **no `end`** → death — inside the scroll,
  which is what the maintainer reported.

Both die with the same last-completed engine work: `String::lower` over MIME types and response **header
names** (`image/webp`, `image/gif`, `Cache-Control`, `Content-Type`, `Date`, `Pragma`,
`Transfer-Encoding`, `X-Content-Type-Options`, `X-XSS-Protection`, `X-Requestid`) — the same
`ParsedContentType` subsystem as the `.23`/`.24`/`.25` deaths, so the pointer to the subsystem stands.

`0.2.3.28` carries the fix on both lines (ARM `Compiled 14/14`, `duplicate=142`, `Exports: 40`,
`SHA256 BF549277…`; x64 `Compiled 13/13`, `duplicate=0`, `Exports: 40`, `SHA256 77B3CA74…`);
`Test-AppxPayload.ps1` reports `AUDIT PASSED` (30 binaries all arm, 40/40 exports), and the freshly
linked, staged **and packaged** `WebCoreDriver.dll` all hash to `bf54927727c36400…`, so the audit ran
against this build's own artifact. Deployed `ME.Apotheosis_0.2.3.28_arm__cj4nrm0qt9s1y`, `HTTP 202 in
509 s`, `Success`. **The reader is still unverified on the device** — `.28` is the first build whose walk
provably reads memory, so the maintainer's next reproduction is the proof: it must log either
`ABRT assert(frame#N): file=… function=… (line=… counter=… slot=…)` / `ABRT assert(shape): …`, or a wide
window whose `slot` addresses **differ line to line**.
The assert itself is still unnamed; `.28` is the build that can name it.

**Superseded the same day by the `.29` reading below: the assert IS named, and the reader could not have
named it.** The `.28` pull answered both questions without the reader's help.

---

## `0.2.3.29`: dzen.ru's device death is a null WebSocket channel, and the reader had two faults

Source: the maintainer's `.28` reproduction, pulled as
`Src/tools/device-logs/20261009-105721/port-trace.txt` (1,225,857 bytes / 32,140 lines). The window is
the first one that can be read by eye — the volatile/opaque-base rewrite worked, and every line carries a
distinct `slot` — yet **both nets were still silent** (`no argument triple in 1024 words`). That
contradiction is what the session resolved, and it had two independent halves.

### Half one — the instrument: `ApoImageString()` computed a length and never copied the string

The function scanned an image string to decide "printable NUL-terminated ASCII, at least `minLen`",
returned true, and filled in only the terminator (`out[n] = '\0'`) — the copy of the scanned characters
was simply absent. `file[]` and `fn[]` therefore stayed uninitialised stack buffers, and
`ApoFileSuffixOk(file, flen)` inspected whatever the previous frame had left there. The shape net could
never match a real `file` pointer, and the frame net, which validates its candidate triple through the
same shape check, could not match either. **Both nets had been dead since they shipped** — `.26`'s
512-word scan, `.27`'s 1024-word scan and `.28`'s repaired walk all failed for this reason, not because
of the window size.

It was found by disbelieving the source: a host simulation of the same logic *did* match the `.28`
window, so the compiled code had to differ from the text — and `llvm-objdump` showed only
`strb r3,[r7,r2]` (the terminator) in the loop, followed by the `buf[n-4..n-1]` suffix read. Fix: one
line, `out[n] = c;`.

### Half two — the answer: `RELEASE_ASSERT(m_channel)` at `WebCore/Modules/websockets/WebSocket.cpp:288`

The `.28` window is legible by hand. The `WTFCrashWithInfo` argument spill sits at window slots
**105–108** — `line = 0x120 = 288`, a `file` pointer, a `function` pointer, `counter = 0x13D = 317` — four
words **above** slot 104, which holds the `frames[2]` return address `5BE3F2CD`. (So the frame net's
`a = i - 4` was the wrong offset; `.29` tries both, `i + 1` first.) Resolving the two pointers through the
local packaged DLL's PE — base `0x5BE30000`, RVA `0x1AF0C2C` for the file string, `0x1AF0BD6` for the
function:

```
file      WebKit/Source/WebCore/Modules/websockets/WebSocket.cpp
function  ExceptionOr<void> WebCore::WebSocket::connect(const String &, const Vector<String> &)
line      288        counter   317
```

```
if (RefPtr provider = context->socketProvider())
    m_channel = ThreadableWebSocketChannel::create(context.get(), *this, *provider);
// Every ScriptExecutionContext should have a SocketProvider.
RELEASE_ASSERT(m_channel);                        // WebSocket.cpp:288
```

**Why the channel was null.** `ThreadableWebSocketChannel::create()` forwards to
`provider.createWebSocketChannel(...)`, and the only `PageConfiguration` this port ever has comes from
`pageConfigurationWithEmptyClients()`, which installs `EmptySocketProvider` — whose
`createWebSocketChannel()` returns `nullptr`. dzen.ru opens a WebSocket: Yandex Metrika
(`mc.yandex.ru/metrika/tag_phono.js`) is the last `loader: CALL` before the abort. So the assert fired and
`abort()` killed the browser. This is the **same empty-client trap** as `.25`'s missing
`NotificationController` supplement ([PLAN.md](../PLAN.md) §0a) — a second `pageConfigurationWithEmptyClients`
supplement that WebCore assumes the embedder provides.

### The fix, and the `.29` build

`Src/port/PortWebSocket.{h,cpp}` adds a port-side `SocketProvider` whose `createWebSocketChannel()`
returns a stub `ThreadableWebSocketChannel` whose `connect()` answers `ConnectStatus::KO`.
`WebSocket::connect()` turns KO into `failAsynchronously()` (line ~331), so the page gets an ordinary
WebSocket error event and a closed socket — a browser without WebSocket support, which is what this port
is. Nothing is silently successful: the connection really does fail, observably, on the page's own terms.
Wired at **every live `Page::create` site** (`buildSession`, `WebCoreRenderHtml`, `WebCoreLoadUrl`, the
`-DWEBCOREDRIVER_STUB` stub, plus the stale `core.cpp`), because the provider lives in the configuration,
not the `Page` (`Page::m_socketProvider` is a `const Ref` and cannot be replaced afterwards).

Three implementation facts cost a compile each and are worth recording: `WorkerThreadableWebSocketChannel`
is `RefCounted<T>` **first**, then the interface, and `RefCounted::ref()`/`deref()` resolve only there
(`ThreadSafeRefCounted<T>` derives from `ThreadSafeRefCountedBase`, not from `RefCounted<T>`); the
interface's `ref()`/`deref()` are pure virtuals inherited from `AbstractRefCounted` and a concrete class
must supply them; and `initializeWebTransportSession()` needs `<WebCore/WebTransportSession.h>` actually
included, because `SocketProvider.h` only forward-declares the class and `RefPtr<WebTransportSession>`'s
destructor then fails on an incomplete type.

It is a relink on both lines, no WebKit rebuild — the port links WebCore **statically**
(`WebCoreFull-arm32.lib` / `WebCoreFull-x64-19041.lib`), so `ThreadableWebSocketChannel`'s constructor and
the promise machinery resolve out of the archive. ARM: `Compiled 15/15`, `[link] EXIT=0 unresolved=0
duplicate=142`, `Exports: 40`, `SHA256 14E73E2A…`. x64: `Compiled 14/14`, `unresolved=0 duplicate=0`,
`Exports: 40`, `SHA256 B212351D…`. Bundle `Apotheosis_0.2.3.29_arm.appxbundle` (48,356,817 bytes,
Release): `AUDIT PASSED`, 30 binaries all arm, driver at the package root (30.6 MB), `resources.pri`
present, 40/40 declared exports, and linked == staged == packaged = `14e73e2aa6a84516…`. Deployed
`ME.Apotheosis_0.2.3.29_arm__cj4nrm0qt9s1y`, `HTTP 202 in 88 s`, `Success`.

**Unverified on the device.** The expectation for the next reproduction is *no death at all* on dzen.ru;
if something else kills it, the repaired reader should now name it. A silent death with no reader line
would mean looking at the instrument again, not the engine.

---

### The `.29` device report, read from what the pull could reach (2026-10-09)

The maintainer's first run on `.29` died on the first dzen.ru navigation before any gesture; the second
run survived a white first paint (the SSO-install case), a Reload, a pinch, a partial scroll stall and a
navigation to ya.ru, where the page came up at an enormous scale and the pinch did nothing at all. The
browser was left open, so nothing was pulled from `port-trace.txt` beyond what WDP would serve.

**What the readable files establish.** `setup-trace.txt` gives two launches 30 s apart (12:41:17 and
12:41:47) and the first is the death; the second process is alive in
`/api/resourcemanager/processes` (pid 2772, 273.5 MB) with a flat `mem.txt`. `autocmd-log.txt` carries
every pinch and every `[GPU] page zoom …` line `gpu-steps.txt` carries the tail of. The symptom is
explained end to end by the **`fitwindow=0`** branch, and no new engine defect is needed:

- `settings.ini` holds `zoom=50`, written by an earlier pinch. `SetFitWindow(false)` documents what off
  means — "the fixed 720x1080 viewport at zoom 1" — but the zoom still gets in, because the load path at
  `MainPage.xaml.cs:1155` re-applies the persisted value after every navigation
  (`if (!_fitWindow && _sessionActive && _defaultZoom != 100) CommitPageZoom(_defaultZoom / 100.0f, "reload")`),
  which is the `[GPU] page zoom 1.00 -> 0.50` that follows each load in `gpu-steps.txt`.
- 50 % is documented as "a desktop layout on a small screen" (`ViewportZoom()`), and that is what the
  diag shows for ya.ru: `contents=724x1080` with the page's own resources named `cb=desktop` and
  `srv=morda_ru_desktop` — a desktop document in a phone-sized surface.
- The pinch commits through `CommitPageZoom`, whose clamp ends at `kZoomMin = 0.5f` — the value the
  setting already holds — so the commit is a no-op and the engine logs `page zoom already 0.50 -- nothing
  to do`. The two pinches that did move (0.52, 0.67) were inside the same range and were undone by the
  next load's re-apply.
- A pinch whose commit lands while `_interacting` is set is deferred with `_vpDirty = true` and the
  comment that "the pinch is not lost, it is only late" — but the only thing that drains `_vpDirty` is
  `ApplyViewport`, and that returns at once when `_fitWindow` is off. **In the ARM configuration a
  deferred pinch is dropped outright**, which is the literal reading of "питч не отрабатывает вообще".
  The two `pinch: deferred (L0I1)` lines at 12:42:14 and 12:42:15 are that path.

**The pull could not reach `port-trace.txt` while the app ran, and that turned out to be the blocker.**
The file is 1,457,910 bytes and answered HTTP 404 to every read for as long as the process lived —
twenty consecutive attempts, stable, and the file was not growing (the engine was idle), so this is not
a size limit and not a write race: a 3.9 MB JPEG from the device's CameraRoll reads fine over the same
endpoint, and `mem.txt`, `stage.txt`, `gpu-steps.txt` and `update.json` read fine from the same folder.
**Closing the browser unblocked it**: the same file then pulled whole, 1,458,151 bytes, 38,263 lines,
first try. The two earlier successful pulls (1.2 MB at 10:57, 2.5 MB in August) were also taken with the
owning process dead. So the rule is not "the trace is unreadable" but "the trace is unreadable *while its
owner lives*" — which means the maintainer's most valuable runs, the ones where they leave the browser
open to show a live symptom, are exactly the runs whose evidence is locked until the process ends. The
durable cure is to stop producing one unbounded append-only file: rotate the trace at a fixed size so the
readable window always contains the tail, and a dead session's trace is closed and pullable.

### The `.29` trace, read after the browser closed (2026-10-09)

**The first `.29` session, and what it was doing.** `DEATHNET: installed` at line 32149, `SetFontsDir …
ME.Apotheosis_0.2.3.29_arm…` at 32146. Its first navigation is not dzen.ru's front page: the loader's
first call is `https://sso.dzen.ru/install?uuid=0f5c11a2-…`, then `https://sso.dzen.ru/log?uuid=…`, both
served, `inflight=0 hosts=0`, then `spa: probe rc=0 kick='no-mod'`, `tail: paintToRGBA exit rc=0
nonWhite=0` — **the SSO install page paints nothing, which is the maintainer's "белое окно"** — and then
`teardown: enter why=load` … `teardown: exit why=load dt=16ms`, and 353 lines into the session the
process aborts. The second session (line 33023) runs to the end of the file with gestures and no death,
matching the report.

**The abort, symbolised.** The two JSC return addresses resolve against `build-arm32-gpu/jsc.map`
(ImageBase 0x10000000), not against the PDB:

- `ABRT #2 JavaScriptCore.dll+0x1EA01B` → VA `0x101EA00A` + 0x11 =
  `CanMakeCheckedPtrBase<std::atomic<unsigned>, unsigned, std::atomic<bool>, 0>::
  crashDueToCheckedPtrToDeadObject` — the **atomic** instantiation (the single-threaded one is at RVA
  0x177AA0; the atomic one belongs to `CanMakeThreadSafeCheckedPtr`, and `RunLoop::TimerBase::
  ScheduledTask` is `ThreadSafeRefCounted<ScheduledTask>` + `RedBlackTree<…>::ThreadSafeNode`,
  `RunLoopGeneric.cpp:278`).
- `+0x850C27` → VA `0x10850A60` + 0x1C7 = `RedBlackTree<RunLoop::TimerBase::ScheduledTask,
  MonotonicTime>::insert` (the function spans 0x850A60–0x850C3E, and `m_schedules.insert(&task)` is at
  `RunLoopGeneric.cpp:632`). `insert()` opens with `x->reset()`, whose `CheckedPtr` assignment
  **decrements the old target** — and MSVC parks the cold `crashDueToCheckedPtrToDeadObject` call at the
  end of the function, which is why the crash site sits near the end of `insert` while the source site is
  its first statement.
- The frames in between are our own handler (`#0 WebCoreDriver.dll+0xDF15`, inside
  `?ApoPortAbortHandler@@YAXH@Z` at RVA 0xDED0) and the CRT dispatcher; `tid=4232` is the main thread, as
  the session's own `pump: tid=4232 cur=0DBF7000 main=0DBF7000 isMainThread=1 isMain=1` says.

**So the death is the RunLoop `CheckedPtr`-to-dead-object abort — the class found on the bench on
2026-09-18 and cured in `Source/WTF/wtf/RedBlackTree.h` on 2026-09-19 — and the cure is not on the
phone.** `remove()` in that file ends with `z->reset()` under `#if defined(WK_WINUWP)`, and its probes
(`apoRBCheck`, `TREE-BAD-INS-COUNT`, `TREE-BAD-AFTER-SCHED`, `TREE-BAD-REM-COUNT`) are in
`RunLoopGeneric.cpp`. Measured:

| binary | mtime | `TREE-BAD` string present |
|---|---|---|
| `build-arm32-gpu/bin/JavaScriptCore.dll` (the one on the Lumia) | **2026-08-23** | **0** |
| `build-x64-gpu/bin/JavaScriptCore.dll` (bench) | 2026-09-27 | **5** |

`apoRBCheck` is in neither `jsc.map` nor `WebCoreDriver-arm.map`, and `TREE-BAD` appears zero times in
the 38,263-line trace. The driver map also shows `RunLoopGeneric.cpp.obj` count = 0 with
`?mainSingleton@RunLoop@WTF@@SA…` resolving to `JavaScriptCore:JavaScriptCore.dll`: the driver **imports**
the one RunLoop rather than carrying a copy, so the tree that aborted is JSC's. The ARM line is a
**relink**, and a WTF header change cannot travel through a relink — `JavaScriptCore.dll` for ARM has to
be rebuilt, which is the first defect the relink model cannot carry.

**Why the repaired reader still named nothing.** `CheckedRef.h:341` is
`static NO_RETURN_DUE_TO_CRASH NEVER_INLINE void crashDueToCheckedPtrToDeadObject() { CRASH(); }` — plain
`CRASH()`/`WTFCrash()`, not `WTFCrashWithInfo(line, file, function, counter)`. There is no argument
triple anywhere on that stack, so `assert-scan: no argument triple in 1024 words` was the correct answer
to a question with no answer. This is not a second reader bug: `.28`'s `RELEASE_ASSERT(m_channel)` is a
`WTFCrashWithInfo` and did have a spill to find. And the `.29` fix did not fail — the WebSocket assert
never fired.

**A symbolizer caution, paid for again.** `llvm-symbolizer` against `JavaScriptCore.pdb` answered
`WTF::Vector<WTF::UniqueRef<Inspector::InspectorAgentBase>, …>::expandCapacity` for 0x1EA01B — a
plausible wrong answer. The linker map answered truthfully. Symbolise ARM JSC addresses from `jsc.map`.

### The ARM `JavaScriptCore.dll` rebuild — the `RedBlackTree` cure reaches the phone (2026-10-09)

The docket's step 6 was the one item that a *relink* could not deliver: `RedBlackTree.h`'s fix was a
header change, the ARM DLL predated it (2026-08-23 against the 2026-09-19 fix), and a header-only cure
cannot travel forward through a link. WTF headers sit in the PCH, so no target smaller than the whole
DLL carries it.

**Built.** `ninja -C build-arm32-gpu -j4 bin/JavaScriptCore.dll`, in the background, `NINJA_EXIT=0`,
21:20:55 → 21:55:35 — **35 minutes, not the 4-5 h the swap-size estimate suggested**, because a first
(aborted) run had already compiled ~2000 of the 2650 edges and ninja found them current. Verified on
the binary, not on the console: mtime `2026-10-09 21:55:31`, 10.42 MB, still `COFF-ARM` /
`IMAGE_FILE_MACHINE_ARMNT (0x1C4)` / ImageBase `0x10000000`, and **all six `TREE-BAD-*` markers
PRESENT** where the old DLL had none — the whole `apoRBCheck`/`apoRLLife` family, not one lucky byte.

**Two failures on the way, both worth keeping.**

*The first run died at step 1996* with `lld-link: error: could not open 'ucrt.lib'`. The environment
is **two** scripts: `setenv.ps1` (project paths) and `arm32-uwp-env.ps1` (`LIB`/`INCLUDE` for the UWP
ARM SDK). With only the first, every include path is baked into `build.ninja` as `-I`, so compilation
is fine for ~2000 steps and the *first link* — a build-time host generator,
`bin/LLIntSettingsExtractor.exe` — dies because the CRT is resolved through `LIB`. A missing `LIB` is
invisible until something links.

*The second was mine.* Fixing the six-week-old x64 break earlier the same day, I replaced the
`GetModuleHandleExW` guard `_LIBLOADERAPI_H_` (a macro no SDK defines) with `_APISETLIBLOADER_` (the
one `libloaderapi.h` actually uses) — which fixed x64 and **broke ARM**, where the macro is defined but
the function is not declared. Measured with `#pragma message` on both real command lines: on **both**
lines `_APISETLIBLOADER_` is defined and `WINAPI_FAMILY_PARTITION(DESKTOP|SYSTEM|GAMES)` is 0 (both
compile `WINAPI_FAMILY_APP`), so neither guard answers "is it declared?". The lines differ in the
**SDK header version**: ARM pins `10.0.19041.0`, whose `libloaderapi.h` hides the function behind the
desktop/system/games partitions; x64's chain reaches `10.0.26100.0`, which declares it for `APP` too —
proved by the `#line` marker in the preprocessed x64 TU. The declaration is now unconditional and
byte-exact to the SDK prototype (`__declspec(dllimport) BOOL __stdcall`), so a duplicate is a legal
redeclaration rather than the `conflicting types` the earlier `HMODULE`-returning version produced.
Both lines compile clean.

**Shipped to the package.** `WebCoreDriver.dll` was relinked against the current port sources
**40/40 exports**, and staged — the staged copy had been from 11:27 while `WebCoreDriver.cpp` was
edited at 15:02, so the trace rotation and the zoom log were absent from it. `Set-AppVersion.ps1`
bumped the identity `0.2.3.29 → 0.2.3.30` (the OS refuses a same-version-different-content install:
`0x80073CFB`). `MSBuild … -p:Configuration=Release -p:Platform=ARM -p:AppxBundle=Always` produced
`Apotheosis_0.2.3.30_arm.appxbundle`; `Test-AppxPayload.ps1` reported **AUDIT PASSED** (30 binaries all
arm, driver at the package root, `resources.pri` present, 40/40 exports), and the two DLLs **extracted
from the .appx inside the bundle** hash-match the fresh builds
(`JavaScriptCore.dll 1DA348EA…`, `WebCoreDriver.dll 3FB0B441…`), with `TREE-BAD-*` present in the
packaged copy too.

**Not yet done: the device run.** Nothing has been deployed; whether the `.29` dzen.ru death
(`crashDueToCheckedPtrToDeadObject` from `RedBlackTree::insert`) actually disappears is still open, and
the trace rotation and the pinch drain are equally unverified on the phone.

### The cheap-fixes pass while the JSC rebuild waits (2026-10-09)

The maintainer deferred the 2650-step ARM `JavaScriptCore.dll` rebuild to the next session and asked
for the port-side and managed-side fixes that do not need it. Five things landed; two of them are
findings, not code.

**1. The trace now rotates, because it cannot be read while it is open.** `WebCorePortTrace`
(`Src/port/WebCoreDriver.cpp`) and `portLoaderTraceV` (`Src/port/PortPlatformStrategies.cpp`) each
gained a `portTraceRotateIfNeeded` / `portLoaderTraceRotateIfNeeded` helper: at 4 MB the append
continues in a fresh `port-trace.txt` while the completed chunk becomes `port-trace.1.txt` (the driver's)
or `port-trace.2.txt` (the loader's), and a `trace: rotated …` seam is written into both so one rotated
session cannot be mistaken for two sessions. The byte count is kept in a static and re-probed against
the real file every 512 lines — **and re-probed again immediately before the destructive step**, because
two translation units write the same file: a stale count plus one shared `previous` name is how
`remove()` deletes the chunk the other writer just moved aside. That is also why the two writers rotate
to different names. This does not lift the read-side 404 — that is ownership by a live process, proved
by closing the browser — it bounds the damage: the newest session always fits in one closed file.

**2. The zoom log now names the space each number is in.** The old line said `layout 720x1080` at a
factor of 0.50 while the device's `diag` read `contents=724x1080`, and there was no way to tell which
number had moved or in which coordinate space. The line is now
`page zoom 0.50 -> 1.00 (contents 724x1080 css | viewport 720x1080 dip | w/zoom 720 css)`.

**3. The x64 driver had not been compiling since 2026-09-26 — and nobody could have noticed.**
`WebCoreDriver.cpp` declared `GetModuleHandleExW` behind `#if !defined(_LIBLOADERAPI_H_)`, an ARM32
workaround for an include chain that does not reach `libloaderapi.h`. No SDK on this machine guards
that header with `_LIBLOADERAPI_H_` — 19041 and 26100 both use `_APISETLIBLOADER_` — so the fallback
declaration was emitted everywhere, and on x64, where the header *is* included, it collided with the
real prototype: `conflicting types for 'GetModuleHandleExW'`, 9 warnings and 1 error. The reason six
weeks passed is the interesting part: `WebCoreDriver.gpu.x64.obj` was dated **2026-08-16**, before the
declaration existed, so the x64 line simply had not been rebuilt. A line that is not being built
verifies nothing — the mirror image of the stale ARM `JavaScriptCore.dll`, where the cure could not
travel *forward* through a relink.

**4. The Settings sheet got an off switch for the address-bar dropdown.** `_suggestOn` (default on,
persisted as `suggest=` in `settings.ini`) is read in `ShowSuggestions()` — the single function that
can put the panel on screen — and `ShowSuggestions` calls `HideSuggestions()` rather than returning,
so a box already open from a keystroke sent before the switch was turned off is closed rather than
left. The toggle is `SetSuggestSwitch` in the General stack, captioned through `LocSwitch` like its
neighbours, with `AddressBarSuggestions` appended to the `Str.Id` enum and all three language columns.

**5. `_vpDirty` had no drain on the fixed-viewport line.** The flag means "the engine was busy, pick
this up when it is free" and is written by `CommitPageZoom`, `ApplyViewport`, `AdoptViewport` and
`OnContentAreaSizeChanged`. Its only reader was `AdoptViewport()`, which returns at once when
`fitwindow` is off — so in the ARM configuration (fit off, fixed 720x1080) a zoom committed while the
engine was busy was dropped with no pickup, which is the device report "питч не отрабатывает вообще".
`DrainPendingZoom()` is the new single pickup: it re-checks busy, adopts the viewport when fitted and
re-runs the deferred commit when not, clearing `_vpDirty` first so `CommitPageZoom`'s already-at test
can still fire. It is called from `SetLoading(false)`, from `OnLiveTick`'s safety net and from
`OnImageManipCompleted` right after the pinch's commit.

---

## `0.2.3.30`: the rebuilt `JavaScriptCore.dll` is on the phone, the death is a different one, and the "blurry" report is measured (2026-10-09)

The first `.30` device run. All numbers below come from
`Src/tools/device-logs/20261009-224047/port-trace.txt` (3,288,252 B / 78,287 lines, pulled
2026-10-09 22:40) and `Src/tools/device-logs/20261009-224656/settings.ini`.

### What the run proved, for free

- **The rebuilt engine is installed and executing.** The `.30` session is lines **38268–77770**
  (session boundaries are the `SetFontsDir(from conf): …ME.Apotheosis_<ver>_arm…` lines; the file is
  append-only and holds `.26`–`.30`). It carries **1531** `rlife:` lines, all `tid=2548`; the `.29`
  session carries **zero**. `rlife:` is written by the rebuilt WTF itself, so this is the engine
  naming its own identity — no version bump and no extra instrument needed (see "How the device
  proves which `JavaScriptCore.dll` it is running" in `Doc/BUILD-ARM32.md`).
- **The new `RedBlackTree` detectors never fired.** `TREE-BAD-AT-LOOP-DESTROY`,
  `TREE-BAD-AFTER-SCHED`, `TREE-BAD-AFTER-UNSCHED`, `TREE-BAD-INS-COUNT`, `TREE-BAD-REM-COUNT`,
  `TREE-ZERO-LINK`, `TREE-WALK-TRUNCATED` = **0 hits in the whole file**. The defect class the `.29`
  death came out of did not recur.
- **The RunLoop tree is healthy at the end.** The `rlife:` `sched`/`unsched` pairs are balanced right
  up to the last line before the abort — `sched … count=1` / `unsched … count=0` alternating, no
  `count=0` while still in the tree.

### What died, and where the evidence stops

The process still aborted, ~20 s in, during a **burst of image loads**: the last lines before the
abort are `loader: CALL type=1 main=1` for `avatars.mds.yandex.net/get-direct/…` and
`get-yabs_performance/…` (ad banners and avatars), immediately after a run of the font path's
`APOTRACE String::lower`. The maintainer's report — "крашнулся секунд через двадцать при скролле
вниз" — is consistent: scrolling is what triggers those lazy image loads.

The signature **changed**. The abort header is

```
ABRT: abort() reached the CRT signal handler tid=2548
ABRT stack (3 frames):
  ABRT #0 WebCoreDriver.dll+0xE1AD
  ABRT #1 ucrtbase.dll+0x39DDF
  ABRT #2 WebCoreDriver.dll+0xF7E5
ABRT assert-scan: no argument triple in 1024 words from 0A992B64
```

- `#2` is now in **`WebCoreDriver.dll`**; on `.29` it was `JavaScriptCore.dll+0x1EA01B`.
- The scan found **no argument triple**, so this is a plain `CRASH()` / `WTFCrash()`, not a
  `RELEASE_ASSERT` — the assert-reader path is not involved, and the abort is not a
  `WTFCrashWithInfo` caller.
- Symbolization (the map's VA column is what the tracer's offsets compare against; it is **not** an
  absolute address — it equals `0x10000000 + RVA + 0x1000`, i.e. `RVA = offset − 0x1000`):
  - `#0 0xE1AD` → RVA `0xD168` `?ApoPortAbortHandler@…` at `+0x45` — the handler, as expected, the
    same `+0x45` seen on `.29`.
  - `#2 0xF7E5` → nearest-below is `??1PolicyContainer@WebCore@@QAA@XZ` at RVA `0xE7E4`, **one byte**
    before the frame. Its neighbours are `??1HTTPHeaderMap@WebCore@@QAA@XZ` (0xE686),
    `??1AdditionalNetworkLoadMetricsForWebInspector@WebCore@@QAA@XZ` (0xE728) and
    `?WTFCrashWithInfo@@YAXHPBD0H@Z` (0xE7CA, `+0x1B`). Empty destructors spaced ~0xA0 apart is the
    ICF-folded-block signature, so "nearest below" names an arbitrary twin.
  - **`#2` is therefore recorded as unresolved, not named.** What the frames do support: the abort
    came from driver-side code, not from JavaScriptCore.
- Instrument trap, recorded because it cost three passes: a lookup keyed on the map's **RVA field**
  lands in a completely different ICF cluster (`0xF6DC`–`0xF70C`, the `??_G`/`??_E` fold set) and
  returns a plausible wrong answer. Same class as `llvm-symbolizer`'s nearest-symbol trap.

### The "blurry content" report is a measurement

The device settings (`settings.ini`, pulled 2026-10-09 22:46) are:

```
zoom=50        fitwindow=0        gpudefault=0
```

With `fitwindow=0` the viewport is the fixed constant `kW×kH = 720×1080` **DIP**, and `zoom=50` makes
the layout width = viewport / 0.5 = **1440 CSS px**. The engine then rasterizes that 1440-px-wide
layout into a 720-px-wide buffer — **one buffer pixel per two CSS pixels**. The buffer is presented
through `RenderImage` (`Stretch="None"`, one bitmap pixel per DIP) and the compositor scales DIPs to
the panel's physical pixels (720 DIP = 1440 px on this Lumia), i.e. **another 2×**.

The port states the rule itself, at `WebCoreSetPageZoom` (`Src/port/WebCoreDriver.cpp:5947-5960`): GPU
present uses the panel's *physical* size as the viewport and `zoom = CompositionScale`, "which is
where the sharpness comes from"; and **"software present has a DIP-sized viewport and must stay at
zoom 1.0"**. So `zoom=50` on the software path is exactly the combination the code says not to use:
it halves the raster resolution on top of the unavoidable DIP→physical upscale. The app never asks
for the display scale at all — there is no `DisplayInformation` / `RawPixelsPerViewportPixel` /
`ResolutionScale` anywhere in `Src/Apotheosis`.

The sharp path is the GPU one, and on this device the engine has never composited:
`WebCoreEnableCompositing()` returns `g_session->chrome->rootLayer() != nullptr ? 1 : 0`, and the
harness calls it on every load — `compositing=0` in **all ten** pulled `stage.txt` files (2026-10-03
and 2026-10-09 alike). `gpudefault=0` in the settings file above. (An earlier note here argued from
`gpuinit.txt` being absent from every pull; that proves nothing, because `gpuinit.txt` is not in
`pull-device-logs.ps1`'s default list and was never requested. The `compositing=` field is the
evidence that was actually pulled.)

Two consequences worth keeping:

- **The one-step test is to set the zoom back to 100%.** That returns the raster to one buffer pixel
  per CSS pixel and should halve the perceived blur. The residual 2× DIP→physical upscale stays and
  is structural to the software present path.
- **The pinch is not the cause, and cannot be.** With `fitwindow=0` the only drain for a deferred
  commit (`AdoptViewport`/`ApplyViewport`) returns at once, so a pinch on this configuration never
  reaches the engine — its "survived well" in this run is a survived *gesture*, not a survived zoom.

Not measured: the trace rotation (3.29 MB < the 4 MB ceiling, so `.1`/`.2` were absent) and the
rebuilt-engine pinch path both remain unverified on the device.

### The second `.30` run: the zoom lever is reachable, the death reproduces exactly, and the trace rotation is root-caused (2026-10-09)

Pulled from `Src/tools/device-logs/20261009-225645/` (port-trace 6,223,695 B / 140,805 lines) and
`Src/tools/device-logs/20261009-224656/settings.ini`.

**The blur experiment.** `settings.ini` after the change reads `zoom=100` (was `zoom=50`);
`fitwindow=0` and `gpudefault=0` unchanged. The maintainer's report — "не идеально, но вроде бы
повеселее" — is exactly the split predicted above: the raster returns to one buffer pixel per CSS
pixel, and the DIP→physical upscale of the software present path remains.

- The zoom lever is confirmed **reachable**: the new session (lines 78292–140231) carries two
  `gesture: zoom enter` markers (82507, 87775), each followed within two lines by `gesture: resize
  enter` — the `PostZoomAndRepaint` zoom-then-resize pair — and `ApoGestureMark(kGestZoom, …)` is the
  first statement of `WebCoreSetPageZoom`.
- It is **not** confirmed *effective* by measurement, and the reason is worth keeping. `stage.txt`
  still read `contents=1024x1986`, byte-identical to the `zoom=50` run, because the diag is written
  once per completed load (`WriteStage`, `File.WriteAllText`) and the last completed load preceded the
  settings change — the diag cannot be synced to a zoom change (`diag-line-cannot-sync-a-load`, hit
  again). So only the visual report and the gesture markers distinguish the two settings; no number did
  — **at the time of that run.**
- **Corrected 2026-10-09 23:08: the port's zoom log was never 404; it was asked for under the wrong
  name.** The app sets `WebCoreSetGpuInitLogFile` to **`gpu-steps.txt`** (`MainPage.xaml.cs:219`) while
  the C++ harness passes `gpuinit-steps.txt`; `pull-device-logs.ps1`'s default list carried only the
  harness name, so every `.30` pull recorded a 404 for a file that existed and was being written, and
  the conclusion "that marker channel is dead in this configuration" was an artifact of the filename.
  The channel is not merely alive, it is **readable while the app runs**: `gpuLogMarker`
  (`WebCoreDriver.cpp:2582`) does `fopen(…, "a")` / `fprintf` / `fclose` **per line**, so no handle is
  held — unlike `port-trace.txt`. The script's default list now names both. Two `.30`-era pulls
  (12:50, 13:25) had in fact already fetched `gpu-steps.txt` by hand.
- **With the right file the number exists, and it says the 100 % setting was live.** The pull at 23:08
  (`Src/tools/device-logs/20261009-230812/gpu-steps.txt`, 52,218 B / 950 lines, taken against the
  running app) shows a file truncated once at process start (zero old-format `CSS-ish` lines — it is
  the current session in its entirety, two `WebCoreSessionLoad` of `dzen.ru`) whose first zoom marker
  reads `[GPU] page zoom 1.00 -> 0.59 (contents 720x1654 css | viewport 720x1080 dip | w/zoom 1220
  css)`. `before = 1.00` on the session's first zoom call is the missing measurement: with `zoom=100`
  the `_defaultZoom != 100` guard at `MainPage.xaml.cs:1166` skips the commit, `teardownSession` leaves
  the factor at 1.0, and the session therefore ran at **one buffer pixel per CSS pixel** — exactly the
  half-resolution term removed and the DIP→physical upscale left standing. That session then took two
  pinches that **both applied** (1.00→0.59, 0.59→0.80); the deferral trap is a race that loses one
  pinch that lands *during* an interaction, not a permanent drop of all of them.

**The death reproduces exactly.** The second session aborted at line 140231 with a stack identical to
the first run's — `#0 WebCoreDriver.dll+0xE1AD`, `#1 ucrtbase.dll+0x39DDF`, `#2
WebCoreDriver.dll+0xF7E5`, `no argument triple in 1024 words` — from a different thread (tid 4072 vs
2548), after prolonged scrolling. Two independent sessions, the same offsets: the `.30` death is
deterministic at its site, which `.29`'s was not.

**The trace rotation has never fired, and the cause is a silent sharing violation.**
`port-trace.txt` reached 6.2 MB against a 4 MB ceiling with no `.1` and no `.2`, while still carrying
the `.26`–`.29` sessions — so it has never rotated once in this install's life. The code is deployed
(the `rotated out at the size ceiling` string byte-searches clean in the staged ARM driver, staged
22:11:24 and packed at 22:14:47) and `ApoGestureMark` writes through `WebCorePortTrace` dozens of
times per session, so the path is executed constantly. `apoTraceAppend`
(`WebKit/Source/WTF/wtf/generic/RunLoopGeneric.cpp:57`) keeps its `FILE*` in a function-static and
**never closes it**, and MSVC's `fopen` omits `FILE_SHARE_DELETE` — so `rename(path, previous)` fails
with `ERROR_SHARING_VIOLATION` for the life of that handle, and the code's handler for that case is a
bare `return;` ("still over the ceiling; the next probe tries again"). It retries forever and reports
nothing. The engine's writer lives in a different module, so the rotation site cannot see it.
The ceiling constant and the two-name scheme are both correct; only the rename is impossible. A fix
must either have the engine writer open/append/close per line (as `WebCorePortTrace` does) or have the
rotation copy the chunk aside and truncate in place. Until then, pull `port-trace.txt` whole and read
`.1`/`.2` as always-absent.

### The full install snapshot, and the `.30` death named at last: `wtf/HashTable.h:534` (2026-10-09 23:12)

`Src/tools/device-logs/20261009-231207/port-trace.txt` — 6,223,695 B / 140,748 lines, pulled with the
app **closed** (a live process 404s this file) — carries the whole install, `.25` through `.30`, and
**eight aborts, one in every version**. Session boundaries are the `SetFontsDir(from conf): …ME.Apotheosis_<ver>_arm…`
lines:

| line | version | `#0` | `#2` |
|---|---|---|---|
| 4362 | `.25` | +0xDDB9 | +0xECEB |
| 13143 | `.26` | +0xDDD9 | +0xEEC5 |
| 20311 | `.27` | +0xDDDD | +0xEECD |
| 26985 | `.27` | +0xDDDD | +0xEECD |
| 31628 | `.28` | +0xDDDD | +0xF2CD |
| 32502 | `.29` | +0xDF15 | **JavaScriptCore.dll+0x1EA01B** |
| 77770 | `.30` | +0xE1AD | +0xF7E5 |
| 140231 | `.30` | +0xE1AD | +0xF7E5 |

`.25`–`.28` share the `+0xDDxx`/`+0xEExx` family and **every one of them reports
`assert-scan: no argument triple`** — they are the `RELEASE_ASSERT(m_channel)` WebSocket death, whose
`#2` names nothing because of the [[assert-reader-image-string-never-copied]] defect. `.29` is the only
one whose `#2` is a *different module* (JavaScriptCore — the `RedBlackTree` cure that a relink cannot
carry). `.30` reproduces its own signature twice, tid 2548 then 4072.

**The `.30` abort is now fully named — from the stack window, not from the scan.** The last abort's
`ABRT window[…]` dump holds `WTFCrashWithInfo`'s three arguments adjacent, in order:

```
window[104] 09F12C14 5BE3F7E5  WebCoreDriver.dll+0xF7E5   <- the tracer's "#2"
window[105] 09F12C18 00000216                              line = 0x216 = 534
window[106] 09F12C1C 5D683735  __FILE__       = …\build-arm32-gpu\WTF\Headers\wtf/HashTable.h
window[107] 09F12C20 5D69F061  __PRETTY_FUNCTION__ = void WTF::HashTable<WebCore::FontPlatformData,
                               WTF::KeyValuePair<WebCore::FontPlatformData, WTF::Ref<WebCore::Font>>, …,
                               WebCore::FontDataCacheKeyTraits>::validateKey(const ValueType &) [Key = …]
```

So it is a **`RELEASE_ASSERT` at `wtf/HashTable.h:534`**, in the FontCache font map
(`FontDataCacheKeyTraits`), reached with a key equal to that map's `emptyValue()` —
`FontPlatformData(0, false, false)`. `HashTable.h:534` is the exact line the port's own comment predicts
(`Src/port/stubs-font-uwp.cpp:291-299`, which guards that fallback path with a `size > 0` test and then a
`scaledFont()` test). The first reading of that coincidence was "the empty key arrives by a route the
guard does not cover"; the disassembly in the next section falsifies it — **the deployed ARM driver has
no guard at all.**
The surrounding window is a coherent **text-shaping** chain — `WidthIterator::applyExtraSpacingAfterShaping`
→ `commitCurrentFontRange` → `FontCascade::glyphDataForCharacter` → `FontCascadeFonts::glyphDataForVariant`
→ `glyphDataForSystemFallback` → `SystemFallbackFontCache::systemFallbackForCharacterCluster` →
`FontCache::systemFallbackForCharacterCluster` (the port stub) → `FontCache::fontForPlatformData` → the
assert — so **the process dies while shaping text that needed a system font fallback**. Same family as
`null-font-family-crash`.

**The method finding is the one to keep.** The net printed
`ABRT assert-scan: no argument triple in 1024 words from 09F12A74` and named nothing, while the raw
window dump three lines below contained the whole argument triple. Both read the same stack; only the
window found it. **When the scan reports nothing, read the window by hand before believing it.** (And
`#2 = +0xF7E5` symbolizes to `??1PolicyContainer@WebCore` at `+0xf7e4` only because `validateKey` is a
header template whose crash site was ICF-folded into a cluster of identical "call and never return"
bodies — the map names an arbitrary twin; the window names the truth.)

Arithmetic for the next reader: symbolise against the map's **VA column** with target
`= 0x10000000 + reportedOffset`; the driver's base in these dumps is `0x5BE30000` and the image is
≈ 30.6 MB (32,120,320 B), so offsets up to ≈ `0x1EA0000` are in-image and the `+0x1853735` /
`+0x186F061` string pointers above are legitimate literals, not mis-attribution.

### The `.30` death is closed, and the cause is a stale object inside `WebCoreFull-arm32.lib` (2026-10-09, later)

The assert named above has a producer, and the producer is not the current source. The deployed
`Src/Apotheosis/native/ARM/WebCoreDriver.dll` (2026-10-09 22:11:24, byte-identical to
`Src/port/WebCoreDriver-arm.dll`, the same link that wrote `WebCoreDriver-arm.map`) carries, at
`WebCoreDriver.dll+0xBE8950`, the map's `?systemFallbackForCharacterCluster@FontCache@WebCore` from member
`WebCoreFull-arm32:stubs-font-uwp.cpp.obj`. Disassembled, that body is:

```
10be8966  apotheosisSetForcedBundledFontName("simhei.ttf")   ; r0 = 0x1184f021
10be8972  vldr s0, [r6, #104]                       ; size = description.computedSize()
10be8978  FontPlatformData(size, false, false)      ; ctor, sret at sp+0x14
10be898c  apotheosisSetForcedBundledFontName(nullptr)
10be8998  fontForPlatformData(tmp) -> sret
```

`0x1184f021` is the string, by arithmetic: `.rdata` VMA `0x1184b000`, raw `0x184B600`, `simhei.ttf` at
file offset 25,485,857 → VA `0x1184f021`. And in the whole 158-byte body (`0x10be8950`–`0x10be89ee`,
bounded by the map's next symbol) **there is no float compare and no conditional branch** — the only
`bne` is the `__security_cookie` check. Neither the `size > 0.0f` guard nor the `scaledFont()` check
exists in the deployed code. So when `description.computedSize()` is 0, the ctor builds exactly
`FontDataCacheKeyTraits::emptyValue()` = `FontPlatformData(0.f, false, false)` and hands it to
`fontForPlatformData`, whose `HashMap::ensure` release-asserts on an empty key. **That is the `.30`
abort, by construction, with nothing left to guess.**

The function is byte-for-byte the object
`build-arm32-gpu/Source/WebCore/CMakeFiles/WebCore.dir/C_/…/Src/port/stubs-font-uwp.cpp.obj`,
**mtime 2026-08-22 09:50:35** — the same instructions in the same order, only the linker-resolved
immediates differ. The x64 twin of that same path is 2026-09-27 04:14:03 and *does* contain the guard,
which is why one source behaves on one line and not on the other: the shipped ARM image (22:11) and the
freshly relinked x64 image (22:24) are the two halves of that comparison.

**Why it never shipped.** `stubs-font-uwp.cpp` is compiled *inside* WebCore (that object's path is a
WebCore target), so it lands in the prebuilt 3.33 GB `build-arm32-gpu/lib/WebCoreFull-arm32.lib`. The ARM
line only rebuilds the port files named in `Src/port/build-driver-dll-arm32.ps1` (`$Sources`, lines 83–98:
WebCoreDriver, PortChromeClient, LoadingFrameLoaderClient, PortPlatformStrategies,
PortNetworkStorageSession, PortStorage, PortWebSocket, webcore-driver-stubs, stubs-crypto,
stubs-pasteboard, stubs-network, stubs-ax, stubs-other, stubs-loader, plus `..\harness\gdi-stubs`).
**`stubs-font-uwp` is not in that list.** All 939 objects under
`build-arm32-gpu/Source/WebCore/CMakeFiles/WebCore.dir` are older than 2026-09-01, so *every* port source
that WebCore compiles is frozen at 22–23.08 — `stubs-font-uwp.cpp`, `stubs-screen-uwp.cpp`,
`stubs-sqlite.cpp`, `NetworkStorageSessionCurl.cpp`, and the fork's own additions inside
`WebKit/Source/WebCore/platform/graphics/FontPlatformData.cpp` (`apotheosisBundledFontFaceForName`,
`apotheosisSetForcedBundledFontName`).

Two measurements that stop a wrong inference here. (1) The archive holds **two** stub objects: a newer one
whose literal pool is contiguous (`segoeui.ttf\0simhei.ttf\0APOTHEOSIS_FONTS_DIR\0`, archive offset
1,256,175,856) and the Aug-22 one near the end (3,330,162,876) — and the link took the old one. (2) The
`segoeui.ttf` and `APOTHEOSIS_FONTS_DIR` literals that *are* in the shipped DLL come from the freshly
compiled `WebCoreDriver.arm32.obj` (`Src/port/WebCoreDriver.cpp:1138,1158,4270`), not from the stub — so
"the DLL contains `segoeui.ttf`" says nothing about which stub object was linked.

**The fix is one line:** add `'stubs-font-uwp'` to `$Sources` in `Src/port/build-driver-dll-arm32.ps1`. The
loop then produces `stubs-font-uwp.arm32.obj` (`compile-driver-gpu-arm32.ps1` reads the flags out of
`build-arm32-gpu/build.ninja`), and the link appends the objects *before* `$WebKitLibs`, so the fresh
definitions win over the archive member; `/FORCE:MULTIPLE` is already applied on ARM (line 398), so even a
duplicate definition would be a warning this line already tolerates. **Verify offline, before any device
run:** the rebuilt driver must contain strings the deployed one lacks — `grep -a "FONT fallback"` on the
new DLL must succeed (today it finds nothing), and a disassembly at `+0xBE8950` must show a `vcmp` before
the forced-name call.

This is the same *class* as the JavaScriptCore `RedBlackTree` cure and a different *mechanism*: that one
was a header-only fix baked into a prebuilt DLL, this one is a stale object inside a prebuilt archive.
Both are answered by one rule for this line: **only the files in `$Sources` are live.**

**Applied and verified offline, same evening.** `'stubs-font-uwp'` is now in `$Sources`; the rebuild
recompiled that one translation unit (`stubs-font-uwp.arm32.obj`, 34,440 B — the old object was 649,427 B
because it carried debug info) and linked with `unresolved=0 duplicate=142`, the duplicates being exactly
the `/FORCE:MULTIPLE` case this line already tolerated. The fresh payload is
`Src/Apotheosis/native/ARM/WebCoreDriver.dll`, 32,123,392 B (was 32,120,320). Three checks, none needing the
device: (1) the image now contains `FONT fallback` / `FONT cfpd` / `FONT probe`, which the old one lacked;
(2) the map attributes `systemFallbackForCharacterCluster` to `stubs-font-uwp.arm32.obj` where it used to
name the archive member `WebCoreFull-arm32:stubs-font-uwp.cpp.obj`; (3) the function moved from `+0xBE8950`
to `+0x1FB20`, and its body there reads `vcmp.f32 s16, #0` / `vmrs APSR_nzcv, fpscr` / `ble …` immediately
after the trace call — the `if (!(size > 0.0f)) return nullptr` that the shipped code never had.

**Confirmed on the device, 2026-10-09 late.** `0.2.3.31` (bundle audited, packaged driver hash-identical
to the build) deployed `HTTP 202 in 36s` and ran dzen.ru to a hand close. The process **outlived `.30` by
two orders of magnitude** — four-plus minutes against twenty seconds — and no `crash-report.txt` was
written. The abort triple is absent: `HashTable.h:534`, `RELEASE_ASSERT`, `validateKey` and
`fontForPlatformData` are zero hits in the whole 83,462-line trace. The decisive detail is not the
silence but the new stub's own lines: `FONT cfpd:` 511, `FONT fallback:` 29, and
`FONT cfpd: decline, size non-positive or NaN` **26 times** — every one preceded by
`FONT cfpd: enter family='…' size=0.00`. That is precisely the input that used to reach
`fontForPlatformData` and release-assert at `HashTable.h:534`. The guard is now firing on hardware, so
the fix is measured, not inferred from a quiet log.

---

## `0.2.3.37`: requirement 5 is measured by the standing list, and the check lied four times first (2026-10-14)

Back already had a measurement — a hand-driven three-command probe on `0.2.3.36` (`example.com` → tap a
link → `www.iana.org`, `engine back=1`, `rc=0 moved=1` both ways). What it did not have was a place in
the standing acceptance list. Item 5 of the list says *record the back result per site*, and the list was
asserting it rather than measuring it.

### The result

`0.2.3.37`, one process, thirteen sites, driven entirely through `LocalState\autocmd.txt` by
`Src/tools/run-smoke.ps1`:

```
site             mode    rc eng_nonwhite screen_nonwhite swipe_ms verdict back_path    back_case back_ok back_restored
01-example-com   mobile   0       100,00           99,86 981      PASS    host         prev         True          True
02-info-cern-ch  mobile   0         1,42           19,82 928      PASS    host         prev         True          True
03-ycombinator   mobile   0        83,16           85,69 1034     PASS    host         prev         True          True
04-ya-ru         mobile   0         5,95           25,79 905      PASS    host         prev         True          True
05-wikipedia-m   mobile   0        63,85           37,58 1023     PASS    host         prev         True          True
06-w3-org        mobile   0        36,85           45,56 867      PASS    host         prev         True          True
07-rbc-ru        mobile   0        10,61           21,65 836      PASS    host         prev         True          True
08-lenta-ru      mobile   0        52,08           41,98 986      PASS    host         prev         True          True
09-habr-com      mobile   0         1,76           20,79 779      PASS    host         prev         True          True
10-reddit-com    mobile   0        66,05           63,12 808      PASS    host         prev         True          True
11-dzen-ru       mobile   0         0,00           18,33 907      FAIL    host         prev         True          True
12-bad-domain    mobile -16                        31,95          PASS    host-skipped prev         True          True
13-nokia-archive mobile   0        99,99           99,75 686      PASS    host         prev         True          True

passed 12 / 13 in mobile mode;  back 13 / 13;  forward returned 13 / 13
back breakdown: prev 13, same-url 0, other 0
```

**Back 13/13, forward returned 13/13** and the host cursor walks the stack exactly one step and back
on every row — `hist_before` `idx=N/N`, after the Back `idx=N-1/N`, after the Forward `idx=N/N`, with
the URL changing to the row above and back in each case. The one non-PASS is `11-dzen-ru`, and it is
not a site defect: see *The one site that never passed was never the site* below. Requirement 5 is
clean on all thirteen.

### `back_path` — the engine's list is not what answered, and that is correct

Every row reads `back via host rc=-99`. `rc=-99` is the host-stack fallback, and the reason is
structural rather than a defect: the list navigates with `goto`, and **the port builds a new engine
session per top-level navigation**, so the engine's back/forward list holds exactly one item when the
Back arrives. The trace says so before every load — `PBF: createPortBackForwardClient -> a recording
list`. The engine's list is exercised by the *tap* path, where the document follows its own anchor
inside one session, and was measured that way on `.36`.

### The host path was silent, and that was an app defect the harness found

`12-bad-domain` fails to load (`rc=-16`, `kErrCurlDownload`), so no session is live when its Back
arrives; `NavigateHistory` takes its `!_sessionActive` early return and answers from the host stack.
Until `0.2.3.37` that path logged **nothing**, so a Back that was skipped and a Back that never
arrived were indistinguishable — two rows in five runs had an empty `back_rc` for exactly that reason.
The early returns now name their state, and the row carries the line:

```
11:09:22.208 history back skipped reason=no-session
```

**This is the shape of the finding: the harness asked a question it could not interpret, and the
answer was an app change.** The rule the fix follows is the same one the navigation path already
follows — every way out of the method writes a line, including the ones that do nothing.

### Four wrong measurements, three of them the check's own fault

Each produced a specific, confident, false number, and all four are now recorded in
[LESSONS.md](../LESSONS.md):

1. **`forward did not return` ×13.** The Forward poll's completion marker was "any `hist` line", and
   the Back it was measuring had just written one. Both samples were byte-identical in the same
   millisecond. The marker must be the verb's *own* answer — count the lines of that shape before the
   verb and require the count to grow.
2. **`moved_to=''` on every row.** The log is append-only and the reader ran against a live writer, so
   a read caught a line flushed only as far as `... ur`. The prefix regex accepted it and its `url=`
   field read empty. The pattern now ends in `url=\S*$`, so a partial line simply does not match yet.
3. **`from idx=-1` on every row.** A PowerShell function's return value travels the pipeline, which
   *unrolls a one-element array*: the helper handed back a bare string whenever the log held one hist
   line, and a string's `[-1]` is its last character (`m` for `...about:home`, `/` for the Nokia URL).
   Fixed with `@()` around every read.
4. **A `hist`-only batch went unanswered in one row,** leaving an empty `hist_before` for a site whose
   Back was in fact correct. Two readings in one batch cannot be separated that way; the pre-Back
   sample is now in the same batch as the verb.

There is also a fifth, in the app: **site 13 filed as a MISMATCH with the engine having served the
Back**. The port trace explains it — `addItem url=<nokia> n=1 cursor=0` for the port's own recorded
load, then `addItem url=<nokia> n=2 cursor=1` from the document's own history push. The list holds the
same URL twice, so an engine Back moves between two identical entries and `WebCoreGetUrl` returns the
URL already displayed; the app correctly leaves the host cursor alone. **Three outcomes, not two**, so
the row now carries `back_case` (`prev` / `same-url` / `other`) and the summary prints the breakdown —
a bare `13 / 13` would have hidden which of the two successes each row was.

### The desktop axis, and why the list is run twice

The `-Mode desktop` axis had never been used. Run on the same build, in its own process, into
`apotheosis-smoke-desktop`:

```
site             mode     rc eng_nonwhite screen_nonwhite swipe_ms verdict back_path    back_case back_ok back_restored
01-example-com   desktop -16                        31,58          FAIL    host-skipped prev         True          True
02-info-cern-ch  desktop   0 1,42                   19,82 911      PASS    host         prev         True          True
03-ycombinator   desktop   0 83,16                  85,69 993      PASS    host         prev         True          True
04-ya-ru         desktop   0 5,66                   41,97 911      PASS    host         prev         True          True
05-wikipedia-m   desktop   0 63,85                  39,18 971      PASS    host         prev         True          True
06-w3-org        desktop   0 0,00                   18,33 862      FAIL    host         prev         True          True
07-rbc-ru        desktop   0 10,52                  22,51 825      PASS    host         prev         True          True
08-lenta-ru      desktop   0 52,31                  43,05 843      PASS    host         prev         True          True
09-habr-com      desktop   0 1,76                   30,31 736      PASS    host         prev         True          True
10-reddit-com    desktop   0 59,40                  54,74 737      PASS    host         prev         True          True
11-dzen-ru       desktop   0 0,00                   18,33 848      FAIL    host         prev         True          True
12-bad-domain    desktop -16                        31,97          PASS    host-skipped prev         True          True
13-nokia-archive desktop   0 99,83                  99,85 777      PASS    engine       same-url     True          True

passed 10 / 13 in desktop mode;  back 13 / 13;  forward returned 13 / 13
back breakdown: prev 12, same-url 1, other 0
```

Three things came out of it, and only the third survived a repeat:

1. **`01-example-com` returned `rc=-16` on a fresh process** — the first-navigation cost the startup
   handshake exists to absorb, and this time the handshake did not absorb it. The Back that followed
   is the `host-skipped` path, and the row carries its own explanation.
2. **`06-w3-org` painted nothing in desktop mode** (`0.00 %` against `36.85 %` in the mobile run
   minutes earlier).
3. **The `same-url` outcome is real and it was measured here** — site 13, `back via engine rc=0`,
   `fwd_attempts=3`, `=> ok (the page pushed its own URL)`. Until this run the third outcome was a
   prediction from one trace; now it has a row.

So the list was run again, same build, same mode, a new process, into `apotheosis-smoke-desktop-2`:

```
passed 12 / 13 in desktop mode;  back 13 / 13;  forward returned 13 / 13
back breakdown: prev 13, same-url 0, other 0
```

`01` returned `rc=0` and `06` painted `36.85 %`. **Both of the first run's anomalies were one-offs:
the fresh-process first-navigation flake and the intermittent parser stall, not site verdicts.** The
only miss common to all three runs is `11-dzen-ru` (`0.00 %` engine `nonwhite`) — and chasing that one
down is the section below. It is not a site verdict either.

**The stable finding is the one that did not move.** Across three runs, two modes and three processes,
**Back 13/13 and forward returned 13/13, every row**, the cursor walking `idx=N/N` → `N-1/N` → `N/N`.
Which carrier answered did move — `13-nokia-archive` was `host` in two runs and `engine` in the third
— and every answer was correct, because the fallback is a fallback and not a failure.

**A run that is not repeated is not a measurement.** Two of the three desktop findings dissolved on
the second run, and the check's own `passed` column moved 10 → 12 without a single line of code
changing in between. The harness also had to be taught one thing by this: started with an `-OutDir`
whose `$env:` had been eaten by the calling shell, it ran ten minutes and produced no screenshots
instead of refusing. It now verifies the directory exists before the first site and exits with a
message if it does not.

### The one site that never passed was never the site (2026-10-14, later)

`11-dzen-ru` is the row that has failed in every acceptance run since 2026-10-03: `rc=0`, engine
`nonwhite=0.00`, screenshot 18.33 % (the app's chrome on a white page). It was recorded as a known
site defect. It is not.

**The engine was never given dzen.ru as a document on a cold profile.** From the run's own
`port-trace.txt`, in order, for dzen's `goto`:

```
loader: CALL type=0 main=1 https://sso.dzen.ru/install?uuid=660fcbf6-c828-4b8d-9e78-745634df7e15
PBF: createHistoryItemTree clip=1 item=https://sso.dzen.ru/install?uuid=660fcbf6-…-745634df7e15
PBF: addItem url=https://sso.dzen.ru/install?uuid=660fcbf6-…-745634df7e15 n=1 cursor=0 back=0 fwd=0
tail: paintToRGBA exit rc=0 nonWhite=0
```

dzen answered with a redirect, **the engine followed it as its own navigation**, and the document it
loaded is `sso.dzen.ru/install` — every pixel white. The control sits a few hundred lines further
down the same file: the *second* load of the same URL in the same process runs
`PBF: addItem url=https://dzen.ru/ n=1 cursor=0` and paints `nonWhite=450125` (44 %).

**Measured both ways, deliberately.** A fresh process, first `goto https://dzen.ru` →
`diag: url=https://sso.dzen.ru/install?uuid=50c1f679-…` and `nonWhite=0`. The very next `goto` to the
same URL in the same process → `diag: url=https://dzen.ru/` and `nonWhite=450125`. Eight probes on
warm processes all painted (35–47 %); four acceptance runs on cold ones all blanked. Both readings
were consistent, and that is what made the wrong one look like a property of the site — the variable
was the process's cookie state, not the engine and not the page.

**The record could not have seen it, because every URL in it is the requested one.** `after-load url=`
is written from the port's own `surl`; the app's `_currentUrl` is written by harness-initiated
navigation; the host stack stores that same string. All three say `https://dzen.ru` while the engine's
document is `sso.dzen.ru/install`. The engine's own `diag: url=` is the only field that names the
loaded document.

So the harness now lifts it into a column:

- `final_url` — the document the engine actually loaded, parsed from the diag that was already being
  read for `nonwhite`;
- `redirected` — whether it differs from the requested URL (and `false` for a failed load, where the
  diag describes the *previous* page and would be a measurement that lies);
- a `(redirected: the document is …, not the requested …)` note under the row, and a summary line
  naming every redirected row.

**What is still open is much smaller and much better posed.** `sso.dzen.ru/install` painting
`nonWhite=0` says this engine rendered nothing for it. Whether that page has visible content a desktop
browser would show — a login form, a QR code — is a question this measurement does not answer. The
previous question was "why is dzen blank"; the question now has a URL attached.

### And one thing the list must do to itself

A Back leaves the host cursor one entry up, and the next site's `goto` pushes with `pushHistory=true`,
which truncates the forward tail. Without a Forward after every Back the stack would never have grown
past `[Home, current]` and the whole run would have measured a two-entry stack. **A probe that leaves
the cursor where it found it is the only kind that can measure a sequence.**

## `0.2.3.40`–`.43`: the acceptance list runs on the Lumia itself (2026-10-20)

The device could never be driven from the bench — Device Portal reads `LocalState` and every write
returns HTTP 500 — so the list now travels inside the package. `Assets\device-run.txt` is executed by
`MainPage.TryStartPackagedBatch()` once per package version, with a `device-run.done` marker written
before the first command so a dying run cannot replay itself. `Src/tools/build-app-accept.ps1` stages
a list and builds; `Src/tools/analyze-device-run.ps1` turns `autocmd-log.txt` into the same row-per-site
table the x64 smoke produces.

**`.40` — the full thirteen-site list, one process, 161 commands, 13/13.** No death, no fault marker,
no crash report; 956.7 MB of 1536 at the end. Every row names the document it loaded. Swipes ran at
752–1041 ms, Back/Forward are served by the host-stack fallback (`rc=-99`) with the index advancing on
every row, and dzen confirmed the x64 first-visit mechanism on the device: `sso.dzen.ru/install`,
`nonwhite=0`, `body=0` first, then **29.01 %** on the same process's restored visit.

**The instrument had three defects, all of them its own.** The error row named the previous site,
because `WebCoreRenderHtmlImpl` — the synchronous render behind the shell's error page — never wrote a
diag; fixed, and `.41` reads `(error page)`, `body=1`, **12.28 %**. The analyzer's `url=` was
unanchored and picked the `lasterr=[… url=…]` request instead of the empty document URL; anchored, with
`(error page)` as the named empty case. And the analyzer read the whole appended log, so a new run's
report opened with the previous install's rows; it now slices from the last `---- packaged batch`.

**`.41` — requirement 6, measured rather than inferred.** With the frame hash taken after the swipe as
well as before, a settled 720×1549 ycombinator page changed `0x9bc468b7 → 0xa2a82d90` inside an 807 ms
swipe and held that hash 1.2 s later. The gesture moves the page.

**`.42` opened a new question instead of closing one.** Twelve rows complete and readable, then the
process disappeared during the last site, 39 s after the navigation, with the engine idle and its
RunLoop still ticking in the trace, the memory sampler writing to the end, and no fault of any kind —
no `VEH` with a real code (all 51 are `OutputDebugString`), no assert, no crash report, 61 % of the
device limit. Nothing in the evidence calls that a crash. `.43` added `suspending`/`resuming`/`EXC`
markers to `boot.txt`, re-ran a short list without reproducing it (nokia itself failed on
`curlcode=28`, a DNS timeout, so the page was not exercised) and then idled for fifteen minutes after
its batch without being terminated — so there is no fixed-time reaper on an idle app.

**`.44` closed the loop by repeating the list under the `.42` conditions: 13/13, shell check, one
process.** nokia 99.53 %, the error page 12.28 %, dzen's SSO blank then 24.89 % restored, swipes
622–887 ms; the app was alive 16 minutes later, `boot.txt` had no lifecycle or exception marker, and
the 6.0 MB trace held 61 `VEH` lines, all `0x40010006`. Three full device runs, two clean, and the
intermittent `.42` termination has not recurred: it stays an open question with a standing instrument,
not a known crash. That evening's DNS was unreliable (a deploy timed out, `.43`'s nokia failed to
resolve), which explains `.44`'s low first readings on a few sites — their restored values show the
pages arriving.

**`.45` left only the controls `goto` bypasses, and closed them.** The acceptance list navigates
through the shell's own load call, so requirements 2 (address bar, navigate on Enter) and 7
(suggestions) had never been exercised on the Lumia. `probe-address.txt` uses `suggest`, which puts
the text in the real `UrlBox` and opens the real dropdown, and `enter`, which calls the same
`CommitUrlBox()` the key handler calls. Measured: `suggest example.com` → `rows=1 vis=1`, then
`h=62 boxfoc=1`, `enter` → `vis=0`, and the engine loads `https://example.com/` (`body=1`, fully
painted); `suggest dzen` → `rows=2 vis=1 h=111`, `enter` → `'https://cn.bing.com/search?q=dzen'`, and
the engine loads exactly that URL (97.26 % painted, 21 scripts). The panel does not reopen on the
shell's own URL write, the heights are a fraction of the window, and the shell still answers after
both commits. All ten requirements now have a device measurement.

**`.46`–`.48` found why an unattended run ends, and fixed it.** `.46` started the list at 20:14:01,
reached the wikipedia navigation at 20:16:42 and was suspended 11 s later, mid-`wait 40000` — `boot.txt`
wrote `suspending` and the process was gone by the next pull, with no fault of any kind. That is
suspend-then-reap, not a crash: the display timed out under an app making no touch input, and UWP
suspension is how such a run ends when nobody is watching. It also gave the open `.42` termination a
plausible mechanism (that run predates the lifecycle marker). `.47` makes the packaged batch itself hold
a `DisplayRequest` for its duration and **ran 13/13 unattended in 11 min 05 s with no suspension** —
dzen's SSO blank then 28.94 % restored, nokia 80.96 % with the engine's own Back, the error page
12.28 %. The fix had one defect the instrument caught: the request was created on the UI thread but
released on the engine thread, so `RequestRelease()` threw `InvalidCastException` and the display stayed
held; `.48` marshals the release through `Dispatcher.RunAsync`, and its 23-command probe batch measured
`display: held awake` (20:44:14.803) → `display: released` (20:45:32.044). The same probe is the
control: 73 s after the request was released, with nothing else happening, `boot.txt` wrote
`suspending` — an unattended app lets the display time out, which is exactly what the batch request
prevents.

**`.49`–`.51`: the scroll report is closed, and the "desktop mode" report is explained, not fixed
(2026-10-20).** The maintainer could not scroll `nokiadesignarchive.aalto.fi` on the Lumia and the
site stayed mobile even with "Start in Desktop mode" on. The page's own files answered both: it
scrolls inside `.mobile-view{height:100vh; overflow-y:auto}` while the frame's range is exactly
`0,0..0,0`, and its breakpoint (`nn(e)` at 870 px) is the same one for the CSS and the d3 graph.
`WebCoreScrollByAt` (`.51`) walks `elementFromPoint` upward for a scrollable overflow ancestor and
falls back to the frame, with both the point and the delta divided by `g_pageZoom` because
`scrollTop`/`ScrollPosition` are CSS px. Measured on the device in one process, no fingers on the
glass (`probe-scroll.txt`, 25 commands): at zoom 100 `[SCT] x=360 y=540 z=1.00 d=0,48 rc=0
out=1|DIV|mobile-view|720x1080` and the frame range `0,0..0,0` — the element, not the frame, is the
scroller, which is why the frame-only path could never move this page. At zoom 80 the layout width
is 900 CSS px (`out=0|||900x1350`), yet the page *loaded* at that width still logs
`Simulation paused on mobile`, and no scrollable element was found under the centre. The bundle's
width is `Mu() = Math.min(window.innerWidth, document.documentElement.clientWidth,
window.screen.width)`; `screen.width` is the phone's physical CSS width and page zoom cannot change
it. Hence the maintainer's "mixed Design+Mobile": the CSS half follows the layout width, the JS half
is clamped by the screen. The UA switch was never the lever; a port-level desktop geometry would be
(`LocalFrame::setOverrideScreenSize` exists and is in the ARM archive), and is left as a design
decision.

**Desktop mode now carries a screen override, and the x64 bench measured it (2026-10-20).** The
direct remedy, `LocalFrame::setOverrideScreenSize`, is not WEBCORE_EXPORTed, and the x64 line links
`WebCore.dll`'s import library, so the port reaches the same state from the page side: in Desktop
mode (`g_apoUaMobile == false`) a document-start user script redefines
`Screen.prototype.width/availWidth` to `window.innerWidth` and `height/availHeight` to
`window.innerHeight`. The virtual screen is therefore the LAYOUT width, not a fixed 980: at zoom
100 that is still 720 (mobile, unchanged) and at zoom 80 it is 900, so `mu()` and the CSS media
queries agree at every zoom instead of re-splitting at another one. All the symbols used
(`UserContentController`, `UserScript`, `mainThreadNormalWorldSingleton`,
`setUserContentProviderForWebKitLegacy`) are exported on x64 and present in the ARM archive; the
switch flips the provider on the live session, so the override applies to the NEXT load like the UA
header itself. The bundle's own code confirms the mechanism: `Mu()` min-clamps
`innerWidth`/`clientWidth`/`screen.width`, `Ss` re-runs it on every `window resize` and on `load`,
and `de()` logs `Simulation paused on mobile` exactly when `width <= mobileBreakpoint`. Measured on
the x64 bench: a mobile load injects nothing; a desktop load at the fitted 1355x751 geometry logs
`[JS] console/log user-script:2:1:433 [DG] screen 1355x751 inner 1355x751` and — for the first
time — no `Simulation paused on mobile`, with `d3.v7.min.js` loaded and the site's simulation
started (`[object Object]`, `Start tour`, `contents=1355x751`, nonwhite 1017599/1017605). A load
that starts before the window has been fitted still reads 720 at document start; the override
follows `innerWidth` live, so the site's `resize` handler is what closes the gate in that case.
The device probe `probe-screensize.txt` measures the same three states on the Lumia.

The same bench session surfaced a staging-side defect unrelated to this change: with
`fitwindow=1`, a home page loaded when `kW/kH` still held the 720x1080 default re-enters
`AdoptViewport -> NavigateTo("about:home")` roughly ten times a second (setup-trace:
`adopt: no session 1355x751 z=0,80 same=False url=about:home`), because each one-shot home render
leaves `_sessionActive == false`. It is visible in traces from the morning of 2026-10-20, i.e.
before this change; the ARM line (`fitwindow=0`) returns from `AdoptViewport` at once and does not
take the branch.

## 0.2.3.56 on the Lumia: the Nokia gate opens, and the graph is on the phone (2026-10-21)

The .55 pairing shipped to the ARM line as 0.2.3.56 (driver relink first — the 00:12 payload predated
the .def line and had no `WebCoreSetNextSessionZoom`; `llvm-readobj` caught it, the relink restored
46/46 exports, `Test-AppxPayload.ps1` re-verified them inside the bundle). The probe
`probe-screensize.txt` travelled in the appx (`build-app-accept.ps1 -Arch arm -Batch ...`, one batch
per version) and its rows A/B ran to script: A laid out `layout width 720 css in 720x1080 dip` and
the site returned its own verdict `Simulation paused on mobile`.

Row C's `goto` was then dropped by the shell-busy gate — page A was still holding `pending=1` on a
single hung subresource (`web.cmp.usercentrics.eu/ui/loader.js`, `curlcode=28` Timeout after the
60 s wait) — and the graph the maintainer saw on the phone came from his own Desktop-mode reload at
the zoom the batch had already armed (`reload: resize rc=0 zoom rc=0 ... z=0,80`). That path is the
same pairing, one step later, and the markers are unambiguous:

```
[GPU] next session zoom armed 0.80
[GPU] session built at zoom 0.80 (frame was 1.00): layout width 899 css in 720x1080 dip
[JS] console/log user-script:4:1:448 [DG] doc screen 900x1350 inner 900x1350 client 900x1350
```

and no `Simulation paused on mobile` line after that point. The pinches that followed stayed honest
under the override — `[DG] resize screen 1384x2076` at z=0.52, `1440x2160` at z=0.50 — i.e. the
virtual screen kept tracking the layout width across the maintainer's two-finger zooms on the
device, which is the semantics the .51 section above argued for.

Two findings left behind, both real and neither blocking the result:

* **A hung third-party resource blocks the next navigation for as long as it hangs.** The shell-busy
  gate drops a `goto` outright (`nav DROPPED, shell busy` in autocmd-log) while a load with one
  pending subresource is still in flight; here that was >60 s of Usercentrics loader timeout. The
  drop is silent from the user's side.
* **Landscape input looks dead on the phone** (maintainer report, corroborated by the log): at
  18:32 a `pinch:` line applies `rc=0` but z does not move (`z=0,50 -> z=0,50`), and scroll does not
  take either. Portrait pinches worked minutes earlier, so this is the rotation-sensitive input
  path, not the zoom lever itself — the known `fitwindow=0` pinch branch is the first suspect.

---

### 0.2.3.57 on the Lumia: rotation adopt verified, and the fixed frame overflows the real window (2026-10-21)

The landscape investigation the maintainer asked to close today is closed, in three parts.

**1. Rotation adopt works on the device — landscape input dead is CLOSED.** The first .57 probe
run (19:36–19:38) saw no rotation at all: the phone screen was locked and the batch's ROTATE window
passed over a suspended app (the maintainer confirmed they had stepped away). Relaunched by hand
over WDP at 19:54 with the screen unlocked, the maintainer rotated, and autocmd-log shows the full
chain:

```
19:54:52  rotate: resize rc=0 zoom rc=0 1080x720 z=1,00
19:54:53  drain:  resize rc=0 zoom rc=0 1080x720 z=0,80
19:55:02  pinch:  resize rc=0 zoom rc=0 1080x720 z=0,50
19:55:18  pinch:  resize rc=0 zoom rc=0 1080x720 z=0,50
19:55:28  dtapfit: resize rc=0 zoom rc=0 1080x720 z=1,00
```

The UserScript saw `[DG] resize screen 1350x900 … 1080x720` across the same window, i.e. the
desktop gate (1350 CSS > 870) stays open through the rotation. The maintainer then pinched to the
Usercentrics close-X **in landscape** and dismissed the overlay by finger — end-to-end proof that
landscape scroll and pinch are alive. What the old "dead" signature actually was: the rotation
no-op (fixed in .57) plus the `kZoomMin=0.50` floor reading as "zoom only partially works" (by
design).

**2. The address box still lies after an in-page link tap** — maintainer: tapped the Nokia link on
the start page, navigation happened, the box kept the old text. That is S3 confirmed by a finger,
not just by reading `m_currentUrl`'s writers.

**3. NEW FINDING — the fixed frame is bigger than the phone's real window.** The .57 batch's tap
row measures it directly: `tap dip=900,300 area=360x494 session=1` — ContentArea is **360x494
DIP** (window 360x640), i.e. the phone renders the app at a 360-DIP width, NOT the 576 the code
comment assumes for the 950. The engine frame is fixed 720x1080 and RenderImage is Stretch=None,
Top/Left, with no Viewbox anywhere in the chain: WriteableBitmap(720,1080) at 96 DPI displays as
720x1080 DIP — **twice the window each way**. The phone therefore shows the TOP-LEFT QUARTER of
the engine frame; the right and bottom of the window are white. Consequences, several of them
previously unexplained:

* the CMP overlay looked "barely visible" even zoomed out — its centre sits outside the visible
  quarter at zoom 1.0;
* the maintainer's "мыльность" report: 720 engine pixels laid over a 1440-px-wide panel is ~4
  physical px per engine pixel (or a panel upscale, if the display-resolution setting is not
  native) — the softness is real, not imagination;
* the frame must NOT be shrunk to the window: 360 / kZoomMin(0.50) = 720 CSS < 870 — the desktop
  gate could never open. The large virtual frame is load-bearing; the cure is to SCALE THE BLIT
  to the window and divide input coordinates by the same factor (ROADMAP S7).

The cause of the 360-DIP window is one of: display scale 4.0 (1440/360) or a lowered display
resolution at scale 2.0 (720/360); the question is with the maintainer (Settings → System →
Display) and the fix does not depend on the answer.

---

### 0.2.3.59 on the Lumia: the navigation queue is verified end-to-end (2026-10-21)

S2 (a hung subresource must not swallow the next navigation) is DONE and device-verified. The
code landed in 0.2.3.58; the verification needed a second build because the first probe's own
design hid the gate, and the reason is worth recording on its own (below).

**1. The marker chain, 0.2.3.59 batch, back-to-back gotos.** `probe-s2-queue.txt` v2 issues
`goto nokia` and `goto example.com` 16 ms apart — no `wait` between them (see part 2) — and
autocmd-log shows every stage of the queue:

```
21:28:14.668  goto https://nokiadesignarchive.aalto.fi/
21:28:14.684  goto https://example.com/
21:28:14.715  ui goto nokia done
21:28:14.730  nav QUEUED, shell busy: https://example.com/    ← held, not dropped
21:28:27.294  nav QUEUED delivered: https://example.com/      ← +12.6 s, first load settled
21:28:27.372  reload: frame superseded seq 3 -> 4 rc=0        ← first load's own reload discarded
21:29:14.727  diag ... url=https://example.com/ title=Example Domain ... rs=C pending=0
```

The delivered page IS the final page. Without the queue this exact sequence produced
`nav DROPPED` on 0.2.3.56 and the diag still said Nokia. The supersede is clean: OnNavDone of
the first load delivered the queued ask (`_opSeq` 3 → 4), and that load's own zoom-reload found
itself superseded and bowed out. Batch completed 13/13 (`display: released`).

**2. Why the 0.2.3.58 probe missed the gate — an autocmd `wait` releases the ENGINE thread.** The
v1 probe put `wait 5000` between the two gotos. Log: the wait elapsed at +5 s, but the second
goto did not run until **+12 s**, after the first load's nav-done had already cleared
`_loading` — the gate was open and the queue never engaged. The mechanism: the wait verb arms
`Task.Delay` on the pool and then re-posts the rest of the batch **to the engine thread**
(`MainPage.AutoCmd.cs`, the wait verb), and the engine thread is busy for the whole session load.
Any verb that must land DURING a load has to be issued back-to-back. The code was never at fault;
the probe was. Recorded in ROADMAP S2 and in the memory notes.

**3. Display-scale slider moved: window 360x494 → 480x707 DIP.** The maintainer slid the
"size of text, apps and elements" slider to minimum (the OS rebooted to apply it). The .58/.59
tap rows measure `area=480x707`, i.e. display scale is now exactly 3.0 (1440/480) — which pins
the ambiguity from the .57 section: the previous 360-DIP width was scale 4.0, not a 720p
downscale. Consequences: S7 still applies (480 < 720 and 707 < 1080 — the phone still sees only
the top-left corner of the fixed frame), and the softness improves from ~4x4 to ~3x3 physical px
per engine pixel. The slider is a partial, manual mitigation of S7; the scaled blit remains the
cure.

**4. Side observations.** (a) After a finger tap on a non-link area, the first `url=` field of
diag goes EMPTY while the engine URL stays live (`url= | url=https://example.com/`) — reproduced
on both .58 and .59; this is the S3 family (the address box's `_currentUrl` is not the engine's)
with a new member: the tap path can *clear* it. (b) The maintainer's post-batch hand test:
two-finger pinch changes zoom (`pinch: z=0,56` → `z=1,12`), but one-finger hold+drag "does not
scroll" — on the batch's final page (example.com) this is expected: `contents=720x1080` equals
the viewport, the page has nothing to scroll. A check on a page with real overflow (Nokia
archive) is pending; if the finger path is dead there too, it belongs to M4's input matrix.

---

### 0.2.3.60 on the Lumia: S8 taps are live, and `rc=0` was never "the page moved" (2026-10-21)

**1. S8 taps, verified by the maintainer's finger — twice.** While a Nokia load was still held by
the hung Usercentrics chain, the maintainer closed the CMP overlay's "Accept All" AND the site's
own 7-step help wizard. Both taps reached the engine (`tap during load: dip=360,540` in the
batch; the finger's own session afterwards). Before S8 both would have died silently at
`HandleTapAt`'s `_loading` gate. S8's tap half is CLOSED; the watchdog kill question from the
probe header did not materialise (no `swipe ignored: no session` in any run).

**2. The scroll verdict: the shell delivered every drag; the engine measured "nothing to
scroll".** The .60 batch and the maintainer's manual session produced dozens of
`scroll dx= dy= at X,Y rc=0` lines at real finger coordinates — the input path
(OnImageManipDelta → FreeScrollBy → PumpScroll → WebCoreScrollByAt) worked end to end. The
missing piece was the meaning of `rc=0`: `WebCoreScrollByAtImpl` returns kOK after it *painted*,
whether or not anything moved (Src/port/WebCoreDriver.cpp:5989). The measurement lives in
`gpu-steps.txt` as `[SCT]` lines, and every one from today's Nokia sessions reads
`out=0|||` (the JS walk found no overflow element under the point) plus
`[SCT] frame ... cur=0,0 -> 0,0 range=0,0..0,0` (the document itself has zero scroll range).
Contrast yesterday's scroll51 session on the same site, fully loaded:
`out=1|DIV|mobile-view|720x1080` — the site's own inner scroller took 48+336 CSS px of delta
before exhausting its ~384 px range. Same shell, same walk, opposite page state.

**3. Root: the half-loaded CMP state, not an input defect.** Every diag of the .60 Nokia
sessions keeps Usercentrics modules pending (`uct?v=...&sid=phRLsmsuzQm7xo(s1)`,
`UcGdprCmpView...(s1)`, `phRLsmsuzQm7xo(s1)`); the batch page rendered 81 % white vs 96 % on the
clean .59 load — the archive's content area never received a scrollable range while the CMP
chain hung. Two corollaries worth keeping: (a) the archive is a 100vh-style app — its layout
fills the CSS viewport exactly at every zoom, so pinch can NEVER manufacture document-level
scroll range there (zoom-out just enlarges the viewport the site re-fills); the scroll lives, if
at all, inside `DIV.mobile-view`, and only when the content has loaded; (b) `rc=0` from the
scroll ABI is "completed and painted", never "moved" — the probe's reading guide that blamed
the blit-supersede path was wrong and has been corrected in probe-s8-input.txt. S8 is DONE; the
remaining "Nokia does not scroll" is M3's CMP hang (and, for seeing the whole frame, S7).

---

### 0.2.3.61 on the bench: S6 lands — the traces are halved on open, because the rotation was dead (2026-10-21)

**1. Why the 4 MB rotation never fired once in an install's life.** `portTraceRotateIfNeeded`
(Src/port/WebCoreDriver.cpp:346, mirrored in PortPlatformStrategies.cpp for the loader's twin) does
`remove(previous) + rename(path, previous)` past a 4 MB ceiling — and it never got that far: it
`return`s at the first error. `apoTraceAppend` (WTF/RunLoopGeneric.cpp:57) holds `port-trace.txt`
open with a cached `FILE*` for the WHOLE process, MSVC's `fopen` omits `FILE_SHARE_DELETE`, and
the `rename()` over the open handle fails with a sharing violation. Measured on the Lumia
2026-10-09: port-trace.txt reached 6,223,695 B with no `.1`/`.2` chunk anywhere, still carrying
sessions from four builds back. The WebKit-side cure (open with `_SH_DENYNO`, or re-open per
line) means editing WebKit sources — a full ~10 h rebuild on BOTH lines — for a diagnostic-file
bound.

**2. The fix is managed and provably race-free.** `App()` (Src/Apotheosis/App.xaml.cs) now calls
`TrimTraceOldestHalf()`: past 4 MB, `port-trace.txt` / `gpu-steps.txt` / `autocmd-log.txt` get
their OLDEST HALF dropped and an ASCII seam `trim: oldest half dropped on open (was N bytes)`
written at the head; each trim also lands `trim <file> <was> -> <now>` in boot.txt. The call site
is the only moment those files are provably unowned: `App()` runs before MainPage, before the first
P/Invoke, i.e. before WebCoreDriver.dll is even loaded — the trim cannot collide with any native
handle. The tail is copied verbatim from just past the first `\n` at/after the midpoint (a file
whose only "newline" is inside one giant line falls back to the raw midpoint, so it cannot trim
itself to nothing). One halving per launch bounds the life-of-install growth; a single session
still writes what it writes.

**3. Measured on the bench, .61.** Three files seeded to ~5.1 MB each with the app closed; app
launched, 12 s, stopped. boot.txt: `trim port-trace.txt 5100045 -> 2550055` /
`trim gpu-steps.txt 5100044 -> 2550055` / `trim autocmd-log.txt 5100046 -> 2550055`, all stamped
23:23:07.3xx — before `onlaunched` (.688) and `mainpage-ctor` (.704). port-trace.txt and
autocmd-log.txt keep the seam at their heads. The gpu-steps.txt seam is overwritten by the first
run — expected: `WebCoreSetGpuInitLogFile` (WebCoreDriver.cpp:2811) truncates that file once per
process by design, so the managed trim of it is belt-and-braces; autocmd-log.txt is the file whose
long-haul reclaim the trim actually buys (it appends ACROSS installs). S6 is DONE; the ROADMAP
carries the per-file verdicts.

---

### 0.2.3.62 on the bench: S4's `?` bytes do not exist, but its hint named a real defect (2026-10-22)

**1. The audit: nothing in the C# line can write a literal 0x3F into the box.** Every native
string crosses the P/Invoke boundary as a UTF-8 `byte[]` (the 2026-10-03 marshalling fix; no
`CharSet.Ansi` remains), `Encoding.UTF8.GetString` on invalid bytes yields U+FFFD and never 0x3F,
history/bookmark files are read as UTF-8, and the string tables (`Str.cs`), the XAML and the
home/error page builders (`BuildHomeHtml`, `MakeErrorHtml`) contain no destroy literals. The
family the S4 evidence pointed at — `DESTROYED-STRINGS.md` — is the HARNESS's source comments
plus nine repaired literals, status "nothing is open". No device log from any session contains a
`?`-run in a url field.

**2. The measurement: two "engine URL unknown" scenarios, no destroy bytes.** `.61` probe
(Src/tools/_probe-s4.ps1): (a) `testurl.txt` first-load to a dead host → the error page renders
and the engine diag reads `url= | url=https://nonexistent.invalid/` (the port renders via
`writer.begin(URL())`, document.url() empty) while the box shows `https://nonexistent.invalid/`;
(b) `goto example.com`, then a tap on a non-link spot → `url= | url=https://example.com/`. Neither
screenshot holds a `?` byte, let alone a run. S4's premise does not reproduce in the product.

**3. But the probe caught the `.58/.59` anomaly in the act — and it was a clearing bug.**
`ApplyEngineFrame` tested `navUrl != null`, and the tap path passes `""` when the engine URL is
unchanged or unreadable (`WebCoreGetUrl` answers `""` until `writeDiag` has run once). An empty
string is not null, so the tap branch took `""` as a URL: it set `_currentUrl = ""` and
`SetUrlBoxText("")`, blanking the address box while the page stayed put. Measured on .61: the
second screenshot shows example.com rendered and the box EMPTY — the same disappearance the .58/.59
device runs logged as `url= | url=https://example.com/`. Fixed in `.62`: an empty navUrl now means
"the engine could not name the document", and the previous URL is kept — only `NavigateTo` clears
the box, and only because it has a real target. Same probe on `.62`: box holds
`https://example.com/` after the tap (s4-2-tap-nonlink.png, 0.2.3.62 title bar visible).

**4. Housekeeping from the run.** The first probe version drove both navigations with `goto` from
about:home and measured nothing: on this bench the home page re-renders ~10x/s, so both gotos were
`nav QUEUED` then `nav QUEUED superseded` inside 86–147 ms (the known `fitwindow-home-adopt-loop`).
The fix was the documented one — the first load comes from `testurl.txt`, not from a verb posted
against a busy shell. The probe now lives as Src/tools/_probe-s4.ps1 so the S3 residue can reuse
its shape.

---

### 0.2.3.67 on the bench: S7 — the fixed frame fits the window, and input follows the scale (2026-10-23)

**1. The presentation half, measured.** The fixed-viewport line (`fitwindow=0`) on the bench
window 1355x751 DIP now logs `frame fit scale=0.695 frame=720x1080 area=1355x751`: the whole
720x1080 frame is drawn 0.695x smaller, top-left anchored, so the page's full width and height
are visible instead of the top-left quarter (s7-1-fixed.png). The fitted line is untouched —
`frame fit scale=1.000 frame=1355x751 area=1355x751` (s7-2-fitwindow1.png) — and S1's rotation
adopt moves the frame to 1080x720 at scale 1.000. The scale is never above 1.0 by design, so a
window that already fits the frame keeps the pre-S7 1:1 presentation.

**2. The input half, measured end-to-end.** The window was resized to a 987x814 DIP content
area, where the landscape frame needs 1080 px of width: `frame fit scale=0.914`. The engine's
own link table then read `[0] 500,666 80x23 https://iana.org/help/example-domains`, i.e. centre
(540,677) in frame pixels. A scripted finger-space tap at `dip=494,619` (the centre divided by
0.914, rounded) logged `tap dip=494,619 area=987x814 fit=0.914 session=1` and navigated to
`https://www.iana.org/help/example-domains`; the trailing diag names that document on both sides
of the `|`. The tap path is `OnPageTapped`/`tap` -> `HandleTapAt` -> `MapTapToEngine`, the same
function the finger uses, so the division is proved on the shipping path and not on a probe-only
shortcut. `MapEngineToDip` (taplink's inverse) was kept symmetric in the same change.

**3. Scope.** The code is C#-only (`MainPage.xaml`, `MainPage.xaml.cs`, `MainPage.AutoCmd.cs`),
so the ARM line needs a recompile of the same version, not a WebKit rebuild. The phone-side
probe is `Src/tools/device-accept/probe-s7.txt` (fixed viewport, expected
`frame fit scale=0.654 frame=720x1080 area=480x707`, then `taplink 0` to iana.org); it ships
with the next authorized ARM acceptance appx. S7 is DONE on the bench and queued for the device.

---

### 0.2.3.73 on the bench: M1 - the cookie jar survives a restart (2026-10-23)

**1. The storage path.** `WebCoreSetCookieJarPath` now points the session at
`LocalState\cookie.jar.db`; `CookieJarDB` is the source of truth, not a per-request `:memory:` jar.
Each curl submit seeds from the whole jar and writes the response's `Set-Cookie` values back before
the next request.

**2. The measurement.** On a clean install the DB opened with `rows=0`. A request to
`httpbin.org/cookies/set?apo=m1` produced `Set-Cookie`; the cookie hash moved
`0x58496e67` -> `0x6e41bac1` -> `0x9f9ccae4`. After a full app restart the DB still had `rows=1`,
`seed=1`, and the same hash. The row was
`('apo','m1','httpbin.org','/',1792838096056,0,0,0)` with `user_version=1`. M1 is done on x64;
the ARM package was compiled and audited only, as required.

**3. Publication.** The source tree is now the release artifact: the WebKit port is the 110-path,
287,687-byte `WebKitPatches/apotheosis-webkit-2.52.4.patch` against `webkitgtk-2.52.4`
(`7acdf5e1`), and `WebKit/` is ignored. The archive scripts, SQLite/psl/cairo dependency scripts,
source-release audit, and clean-machine guide were added so a clone can be rebuilt without the
patched engine tree. The native JSC in the verified `.73` package remains the old
`3483A0C3...` binary; a fresh x64 build stages its own JSC from `build-x64-gpu\bin`.

---

### 0.2.3.73 line on the bench: M2 - localStorage survives a restart (2026-10-23, after the .73 release cut)

**1. The backend.** `PortStorage.cpp` now has a `PortStorageDatabase` in front of the old
`PortStorageArea` map. `WebCoreSetLocalStoragePath`, called from `SetupRuntimeEnv` before the first
Page, opens `LocalState\localstorage.db` through the same WinRT SQLite VFS as M1's cookie jar. One
`ItemTable` stores `(origin, key, value, ordinal)` for every origin; `journal_mode=DELETE` and
`synchronous=FULL` make a committed write survive a force-stop without a `-wal`/`-shm` pair. An area
loads its origin's rows when created, and set/remove/clear commit before the storage event is
dispatched. Session and transient-local storage stay in memory by design. If the database cannot be
opened, the local areas fall back to the old process-scoped map and log `backend=memory` instead of
pretending to be durable.

**2. The measurement.** The test package was a side-by-side
`ME.Apotheosis.M2Test_0.2.3.74_x64__cj4nrm0qt9s1y`, registered from the complete
`bin\x64\Release\ilc\AppxManifest.xml` layout; the source identity stayed
`ME.Apotheosis 0.2.3.73`, so the installed `.73` app and its LocalState were untouched. The test
pages were served as URL-safe base64 through `https://httpbin.org/base64/...` (standard base64 with a
`/` in it is treated as a path separator and returns 404; URL-safe base64 with `-`/`_` works). The
set page wrote `m2-durable=durable-0.2.3.74` and reported
`title=M2 SET value=durable-0.2.3.74 len=16`; the DB row was
`('https://httpbin.org','m2-durable','durable-0.2.3.74',1)`. A force-stop left the row in place. The
next launch logged `storage: db path=...localstorage.db rows=1`,
`storage: namespace local quota=5242880 session=1 backend=sqlite`, and
`storage: restored origin=https://httpbin.org keys=1 bytes=52`; the check page's title read
`M2 CHECK value=durable-0.2.3.74 len=16 total=1`. A second page exercised
`setItem`/`removeItem`/`clear`/re-set and reported `M2 OPS rm=true cl=true re=true len=1`; the DB
then held only the durable row (ordinal 5), and a second force-stop/restart check still read the
value. `localstorage.db` was 16 KB with one row.

**3. Packaging scope at the x64 cut.** The ARM driver compiled and linked with the new
`WebCoreSetLocalStoragePath` export; the ARM Release app built and
`Test-AppxPayload.ps1 -ExpectedArch arm` passed: 30/30 binaries ARM, `WebCoreDriver.dll` at the
package root (31.4 MB), `resources.pri` present, 48/48 driver exports. At that point nothing had
been installed or run on ARM; the acceptance follows in the next section. The `.73` release ZIPs
were built before M2 and remain unchanged; M2 is a post-release source change, and the manifest must
be bumped before the next package is cut from HEAD.

### 0.2.3.76 line on the phones: ARM M2 acceptance and the fitted viewport (2026-10-25, after the .73 release cut)

**1. M2 on ARM.** The side-by-side `ME.Apotheosis.M2Test` identity was installed on the Lumia 950
(`192.168.1.35`, OS 15254.603) and the Lumia 640 (`192.168.1.39`, OS 15063.1868); the installed
`.73` app and its LocalState were untouched. On both phones `0.2.3.74` ran the SET page and the
title read `M2 SET durable-0.2.3.74 len=16 total=1`; a real force-stop followed, then `0.2.3.75`
ran the CHECK page and the title read `M2 CHECK durable-0.2.3.74 len=16 total=1`. Each
`localstorage.db` held one row, `('https://httpbin.org','m2-durable','durable-0.2.3.74',1)`, with
`user_version=1`; the restart logged `rows=1` and
`storage: restored origin=https://httpbin.org keys=1 bytes=60`. Evidence lives in
`Src/tools/device-logs/20261025-m2-l950-set`, `...-check`, `...-l640-set` and `...-check`; the
batches are `Src/tools/device-accept/probe-m2-arm.txt` and `probe-m2-check-arm.txt`.

**2. The fitted viewport.** The first ARM run measured the defect x64 could not show:
`frame fit scale=0,458 frame=720x1080 area=360x494` on the L950 and `scale=0,457` on the L640 - the
fixed 720-px frame was being shrunk by the display scale, so the home page and all of its text were
half size. `0.2.3.76` (48,847,063 bytes, SHA256
`B651A49706A7CF5FFF7C5FD9B94EB3B5688EF00D65B52912F194C3B07AC659AD`, 30/30 ARM binaries, 48/48
driver exports) turns `_fitWindow` on by default on `Windows.Mobile`, writes `fitwindow_migrated=1`
so a later explicit off is not overwritten, and makes `BuildHomeHtml` responsive (`clamp()` sizes,
`repeat(auto-fit,minmax(260px,1fr))`). Both phones then logged `applysettings: ... fit=1`,
`adopt: no session 360x494 z=1,00 same=False url=about:home`,
`frame fit scale=1,000 frame=360x494 area=360x494`; `settings.ini` carried `fitwindow=1` and
`fitwindow_migrated=1`. The maintainer confirmed the home page is correctly sized on both devices.

**3. The remaining blur.** The L950 still looks softer than the L640. Software present hands the
compositor a `360x494`-DIP bitmap; the L950 panel is 1440x2560 (4x) and the L640 is 720x1280 (2x),
so the same DIP frame is upscaled twice as far on the 950. This is a present-resolution limit, not
a viewport-fit regression. WDP on these phones exposes no screenshot API
(`GET /api/control/screenshot` returns 404 and the portal workspace list has no capture tool), and
the app has no frame-dump command, so no L950 screenshot was captured. A physical-resolution software
present - page zoom equal to CompositionScale, display scale `1/scale` - remains the candidate fix
and is deliberately deferred.

### dzen.ru on the x64 bench: the white first visit was a stale form-action guard (2026-10-25, after the .73 release cut)

**1. The report and the reproduction.** The maintainer's friends saw `dzen.ru` open as a white
window on first use. On the x64 bench the first visit committed
`https://sso.dzen.ru/install?uuid=...` with an empty title, `body=0` and `nonwhite=0/777600`, and
stayed there; the last error was `didFailProvisionalLoad type=Cancellation`. This was not an
`<iframe>` problem and L1 would not have changed it: the install document had already committed, it
simply refused to leave. The chain is `dzen.ru` 302 -> `sso.passport.yandex.ru/push` (a script that
POSTs a form) -> `sso.dzen.ru/install` (200, CSP `form-action 'none'`, inline
`window.location.replace`) -> the real `dzen.ru` page. The redirect was refused with
`Refused to load https://dzen.ru/?... because it does not appear in the form-action directive of the
Content Security Policy`; a plain curl of the same URL returns 200 and 2.3 MB of HTML.

**2. The mechanism and the fix.** `FrameLoader::submitForm` records the action URL in
`m_submittedFormURL`, and `DocumentLoader::willSendRequest` checks every later request against the
current document's CSP. Upstream clears the recorded URL on a main-resource error and on a user
gesture, but not on a successful commit, so the install response left the guard naming its own URL
and applied its `form-action 'none'` to the next navigation. The port now calls
`resetMultipleFormSubmissionProtection()` from `dispatchDidCommitLoad` in
`Src/port/LoadingFrameLoaderClient.cpp`. A new form is still checked by `submitForm` against its own
document's CSP, and redirects during a submission are still checked because the guard survives until
that submission commits.

**3. The measurement.** The fix was verified on x64 with the `0.2.3.73` app line and the rebuilt
driver, installed as a side-by-side `ME.Apotheosis.DzenTest 0.2.3.77` package: the installed `.73`
app and its LocalState were untouched. The real first visit then reached
`https://dzen.ru/?sso_failed=blocked&uuid=102e6343-66e4-4d46-bfcc-02fa666e23f0` with `body=1`,
`nonwhite=228105/777600`, 49 scripts and a non-empty title, and `gpu-steps.txt` held no
`form-action` error. A local two-origin harness (ports 8100/8101) reproduced the chain
deterministically and ended at title `REDIRECT OK`; the negative control -- a form on an
engine-loaded page with `form-action 'none'` -- was still refused with the same CSP message, and a
plain form still submitted (`FORM OK`). The test package was removed afterwards. The source
identity remains `ME.Apotheosis 0.2.3.73` and the `.73` release ZIPs remain frozen; this is a
post-release source change, and the manifest must be bumped before the next package is cut from
HEAD. The trap for future probes is recorded in [LESSONS.md](../LESSONS.md): a top-level `goto` is
fed through `DocumentWriter`, so CSP headers on that document are never applied and a CSP test must
load its page through the engine.

### dzen.ru on the x64 bench: the browser-switch-trap and the Yandex Browser ad are two gates (2026-10-25, post-.73)

**1. The trap was a UA mismatch.** After the form-action fix, the x64 bench still painted dzen's
`browser-switch-trap` ("your browser is outdated"). The document's `navigator.userAgent` was the
iPhone string while the top-level fetch sent Safari-on-Windows. dzen reads that pair as a desktop
browser that is neither Edge nor Yandex and refuses it. The fix was to give both halves of a load
one source of truth, `Src/port/PortUserAgent.h`: `LoadingFrameLoaderClient::userAgent()` and all
three `CURLOPT_USERAGENT` sites in `WebCoreDriver.cpp` now use the same effective string. The
desktop default follows the device: a window that is not `Windows.Mobile` starts on the desktop
Edge UA, a phone starts on the iPhone UA. `ua_migrated=1` replaces the old automatic `ua=0` once;
an explicit choice sticks afterwards. With Edge the trap is gone.

**2. The ad was a second, host-specific gate.** Edge then received a Yandex Direct splashscreen
("Часть сайтов недоступна" / "Скачайте Яндекс Браузер"; `product:"browser"`, `paas:"browser3"`,
`utm_term=Edge`). A Yandex Browser UA removes it. A global Yandex UA is not acceptable: it is the
wrong identity for ordinary sites, and `microsoft.com` blocks this bench's client either way
(measured with Edge too -- the block is IP/TLS/header policy, not a UA branch). The port therefore
chooses the desktop UA by host: `dzen.ru`, `yandex.ru`, `yandex.com`, `yandex.net`, the regional
`yandex.by/kz/ua/com.tr`, `ya.ru` and `yastatic.net` get `YaBrowser/24.1.0.0`; everything else keeps
`Edg/120`. The matcher is suffix-based with a label boundary, so `notdzen.ru`,
`dzen.ru.example.com` and `dzen.ru.evil.com` stay on Edge. Custom UA still wins; mobile stays
iPhone.

**3. The measurement.** `.78` verified the unified pair on a local probe: the top-level `GET` and
`navigator.userAgent` both carried Edge, and with a custom Yandex UA both carried Yandex. `.79`
added the host list and was measured on x64: dzen painted the normal desktop page with no
splashscreen (screenshot), `ya.ru` loaded (`body=1`, 66 scripts), `example.com` and `lenta.ru`
loaded on Edge (`body=1`), and `ua mobile` made the probe send the iPhone string before `ua desktop`
restored the default. The matcher passed 21 URL cases. Microsoft's "Your request has been blocked"
appeared with Edge as well as with the earlier Yandex custom UA, so it is not a regression from this
change and not evidence about the host branch.

**4. Versions and packaging.** The strings stay at Chrome/120, YaBrowser/24.1 and iPhone Safari
16.4: these are the compatibility baseline for the old WebKit, not the newest releases. Bumping
them is a separate change with its own site testing. The bench manifest is now `0.2.3.79`; the
`0.2.3.73` release ZIPs remain frozen and no release is cut from this HEAD.

---

### L1 on the x64 bench: an iframe is a real frame, and the popup hole is now visible (2026-10-25, `0.2.3.80`)

**1. The hole.** `LoadingFrameLoaderClient::createFrame` was inherited from
`EmptyFrameLoaderClient` and returned `nullptr`. `SubframeLoader` then had no frame to load into,
so `<iframe>` painted the element's border and nothing else, issued no request, and
`target="name"` had nothing to find. This was not a renderer bug and not a missing widget: it was
the absence of a `LocalFrame` with its own document, loader, script world and view.

**2. The recipe.** `createFrame` now mirrors WebKitLegacy's `_createFrameWithPage`: the child's
effective sandbox flags are the owner element's plus the parent frame's, its referrer policy is the
owner's unless the owner is on the empty-string default (then the top document's policy applies),
and the frame is built with `LocalFrame::createSubframe`, named with `tree().setSpecifiedName()` and
initialized with `FrameLoader::init()` -- the initial empty document. `LocalFrame`'s constructor
already calls `ownerElement.setContentFrame(*this)`, so `SubframeLoader`'s "did a frame appear?"
check succeeds without a second call. `transitionToCommittedForNewPage` creates a fresh subframe
`LocalFrameView` per commit; the main frame is skipped because the driver creates and owns that
view. Attachment works in both orders: `LocalFrame::createView` sets the owner renderer's widget
when layout has already run, and `HTMLFrameElementBase::didAttachRenderers` covers the renderer
arriving later.

Three unresolved externals came from the new translation unit, not from the logic:
`Document::frame()`, `Frame::page()` and `LocalFrame::view()` are defined in
`FrameDestructionObserverInlines.h`, `DocumentPage.h` and `DocumentView.h` (plus `NodeDocument.h`).
`FrameTreeSyncData::create()` needs the generated `FrameTreeSyncData.h`. Without those includes the
file compiles and the driver fails at link time.

**3. The measurement.** A local harness served two origins on one Python server:
`http://127.0.0.1:8765/` and `http://localhost:8765/`, logging `Host` and `Referer` per request.
The packaged browser needs the AppContainer loopback exemption
(`CheckNetIsolation LoopbackExempt -a '-n=ME.Apotheosis_cj4nrm0qt9s1y'`; the quotes around `-n=`
are required). On `.80`, driven only through `LocalState\autocmd.txt` and measured against the
server log, the `FRAME:` trace and screenshots:

* **Seven initial frames** -- same-origin, cross-origin, a nested outer+inner pair, a clipped
  scrolling frame, a sandboxed frame and a `srcdoc` frame -- were created, committed (`FRAME:
  created` then two `committed view` lines each) and painted. The server saw every URL with the
  expected `Referer`; the cross-origin one was origin-only under
  `strict-origin-when-cross-origin`.
* **Lifecycle**: removing an iframe from the DOM tore it down and repainted the parent without a
  crash; changing `src` reused the same frame for a second, cross-origin commit; creating an
  iframe with `document.createElement` loaded it; changing `width`/`height` resized the frame and
  reflowed its document.
* **Sandbox**: without `allow-scripts` the child stayed gray and sent no marker request; with
  `allow-scripts` it turned red and sent exactly one.
* **`srcdoc`**: committed as `about:srcdoc` with the inline document painted and no request for the
  document itself.
* **`<frameset>`**: two `<frame>` columns at 50%, both loaded through the same client.
* **`target="slot"`**: a link click navigated the existing named iframe; the `FRAME: created`
  count stayed at 28 across the click and the server saw `/same.html` with the parent as referer.
* **Origin isolation**: a same-origin child read and wrote `parent.document` (`SAME-ORIGIN PARENT
  ACCESS OK`) and sent its marker; a cross-origin child's access threw and sent no marker -- 1 vs
  0 in the server log.
* **Regression**: `dzen.ru` still paints with its ad iframes (3 new frames, `nonwhite=268131`,
  process responding), `example.com` and `lenta.ru` load as before, and the process stayed at
  296-352 MB across the whole pass.

**4. What remains.** Popups are the other half of the original L1 item and are deliberately not in
this pass: `PortChromeClient::createWindow` still returns `nullptr`, so `window.open` and
`target=_blank` remain no-ops; giving them a window is a host-side decision (new window or new
tab) with its own chrome and lifetime rules. Phone acceptance of iframes/frames is deferred until
a Lumia is back on the bench. The bench manifest is `0.2.3.80`; the `0.2.3.73` release ZIPs remain
frozen and no release is cut from this HEAD.

### L1 on the Lumia 950: the phone iframe batch passes, popups are still the open half (2026-10-25, `0.2.3.85`)

**1. What ran.** The ARM line shipped `0.2.3.85` (manifest `source=0.2.3.85 7362`) and executed the
packaged acceptance batch `device-accept/probe-l1-arm-85.txt` on the Lumia 950 (`192.168.1.35`,
OS 15254). One session, `21:53:07` (app-ctor) to `22:02:02` (suspending, normal) — about nine
minutes, display held awake for the whole batch, no crash, no watchdog timeout, no
`CRASHVERDICT`-style missing marker. Evidence: `Src/tools/device-logs/20261025-2201-l1-arm-85/`
(`autocmd-log.txt`, `boot.txt`, `gpu-steps.txt`, `stage.txt`) plus `Src/tools/device-accept/probe-l1-arm-85.txt`.

**2. The sequence (the reported timeline checks out).** `wait 60000ms` on the home page (the
"minute pause"), then `goto .../?run=85` and ten harness pages in order, then `goto
https://dzen.ru/` (21:57:11, title + `bodyKids=57` + 52 scripts + `nonwhite=64636/177840` after the
50 s wait; ~250 finger scrolls, all `rc=0`), then `goto https://example.com/` — the final diag
reads `url=https://example.com/ | url=https://example.com/ title=Example Domain rs=C`, `stage.txt`
records `after-load url=https://example.com/ rc=0`, and the batch ends with `display: released`.
Every harness page reported `nav=L0/I0/S1` — an active session for the whole pass.

**3. What the iframe pages proved (harness title → engine markers in gpu-steps.txt).**
* **Main harness page**: `title=L1 iframes=6 same=SAME-ORIGIN PARENT ACCESS OK srcdoc=SRCDOC OK`,
  `contents=538x1132`, `body=1` — six iframes painted, a same-origin child read/wrote
  `parent.document`, and `document_count=8` names every sub-document (same, cross, nested-outer,
  nested-inner, about:srcdoc, sandboxed, long). Cross-origin isolation held: the `:8766/cross.html`
  child committed but threw on `parent` access.
* **Sandbox**: three `sandboxed.html` frames created and committed, and the JS engine logged
  `Blocked script execution ... allow-scripts is not set` for each — the gray "script blocked"
  frame is real enforcement, not a stub.
* **`srcdoc`**: committed as `about:srcdoc` with `substitute=1` and zero network request for the
  document itself — matches the x64 bench finding.
* **`<frameset>`**: `FRAME: created name=left` / `name=right`, both got 200s and committed;
  page title `FRAMESET frames=2 names=left,right w=179/179`.
* **Named target**: `FRAME: created name=slot`; the probe's `clicklink` navigated the *existing*
  slot frame to `/same.html` (the post-click line is `FRAME: committed view name=slot` — no new
  frame), title `NAMED slot=/same.html ... SAME ORIGIN OK`.
* **Remove / reload / dynamic**: `title=REMOVED OK iframes=0` with the page drawn nearly empty
  (`nonwhite=2669/177840`); reload re-created and committed the same-origin and cross-origin
  children; `DYNAMIC OK iframes=1` after a script-created frame.
* **Resize**: `title=RESIZE OK actual=600x220` — the page's own iframe resize landed.

**4. Verdict.** The claim that the run went routinely is supported by the logs: pause → harness →
iframe/remove/resize → dzen.ru → example.com, ending clean. Phone acceptance of the *iframe half*
of L1 on the Lumia 950 is hereby recorded; it does NOT close L1.

**5. What is still open — the "доделка".**
* **Popups**: `window.open` and `target=_blank` are still no-ops (`PortChromeClient::createWindow`
  returns `nullptr`). This is the structural half of the original L1 item and needs a host-side
  window/tab decision implemented on the x64 bench first.
* **Lumia 640**: this batch ran on the 950 only — the same probe needs one pass on the 640
  (1 GB, Snapdragon 400) before any "both devices" wording; ROADMAP Phase 0 explicitly requires it.
* One 10-minute run is one run: no three-consecutive-runs claim, no memory figure from this pass.

### L1 popups on the x64 bench: two funnels, one window (2026-10-26, `0.2.3.86`)

**1. The hole had two doors, not one.** The roadmap blamed only `PortChromeClient::createWindow`
returning nullptr — but that is just the `window.open()` door. A `target=_blank` link or a
popup-targeted form never reaches it: it dies earlier, in
`LoadingFrameLoaderClient::dispatchDecidePolicyForNewWindowAction`, which was a bare
`policyFunction(PolicyAction::Ignore)` — no request, no marker, not one line in any log. So
`window.open` goes LocalDOMWindow::open → `WebCore::createWindow` → `Chrome::createWindow` →
our ChromeClient, while links go FrameLoader → `checkNewWindowPolicy` → our FrameLoaderClient —
two independent silences, both now fixed.

**2. The v1 host decision: single-window policy.** Tabs are out of 1.0 (L3), so an auxiliary Page
has nowhere to be presented. Both funnels now park their URL in a driver slot
(`g_pendingPopupUrl`) and write an engine marker; the shell's live tick — the same engine-thread
poller S3 already uses — TAKES the URL once (`WebCoreTakePopupUrl`, new export #49 in
`WebCoreDriver.exports.def`) and feeds it to the normal `NavigateTo(..., pushHistory:true)`, so
history, the address box, the 40 s watchdog and the S2 busy-queue all apply for free. A queued
URL is dropped at session teardown so a popup from a replaced page cannot fire late.
`window.open()` still returns null to the script (there is no second window to return) — that is
the honest v1 answer, and it now leaves a trace on both sides.

**3. Measured, x64 `0.2.3.86`, harness `Src/tools/l1-popup-harness/` (python server on :8765):**
* **Funnel A, `target=_blank` click:** `POPUP: new-window-policy target=_blank queued
  url=.../popup-target.html?src=blank` (gpu-steps) → `popup queued: ...src=blank` in autocmd-log
  1.35 s after `clicklink 0` → final diag `url=...src=blank title=POPUP TARGET OK`, server logs
  `GET /popup-target.html?src=blank` 200. Before this fix the same click produced nothing
  anywhere.
* **Funnel B, `window.open()` inside a click gesture:** `POPUP: window.open ... queued
  url=...src=open` → `popup queued: ...src=open` → diag `POPUP TARGET OK` with `src=open` — not
  `src=href`, so the queue drove the navigation and the href was cancelled by the page's own
  `onclick return false`. The beacon `GET /beacon?fn=window.open&r=null` proves the return value
  is null, as designed.
* **Timer-based `window.open` is blocked — by WebKit, not by us:** `LocalDOMWindow::allowPopUp`
  requires a user gesture OR `javaScriptCanOpenWindowsAutomatically`; an opener from
  `setTimeout` returned null with NO queue marker (beacon `r=null` at t+5 s, page unchanged).
  That matches Chrome's default popup blocking and the port leaves the gate untouched. Note the
  blocked path still logs nothing engine-side — a known honesty gap, not a regression.

**4. Regressions measured on the same build (batches 3–5):**
* **Named frame:** `<iframe name="slot">` + `target="slot"` does NOT enter the queue: no
  `popup queued`, main URL stays `regress.html`, the frame's document ran
  `parent.document.title='REG SLOT OK ...'`. First attempt used only `id=` on the iframe — a
  frame's browsing-context name comes from `name=`, so that target correctly resolved to "no
  such frame" and was treated as a popup: queueing an unknown target IS the right single-window
  interpretation (spec: create a new browsing context).
* **Plain same-window link:** engine navigates, `url follows engine:` (S3) fires, no queue
  capture; engine `POPUP:` marker count stays 3 across batches 4–5 (only deliberate clicks and
  the misnamed trial).
* **S2 interplay, unplanned but observed:** a `goto` issued while the popup navigation was in
  flight logged `nav QUEUED, shell busy` → `nav QUEUED delivered` — the popup path composes with
  the shell's own queue instead of fighting it.

**5. What `0.2.3.86` does NOT prove yet:** Back-from-a-push (pushHistory is the standard path
but no autocmd verb reaches the shell's Back), and anything about the ARM line — driver relink
for ARM and both phones were untouched today per the maintainer's standing x64-only goal.
Cross-origin WAS covered in a sixth batch: a `_blank` from :8765 queued
`http://127.0.0.1:8766/cross-target.html`, the final diag read `url=...:8766/...
title=CROSS TARGET OK`, and the port-8766 server logged the GET — the queue carries an
absolute URL and origin plays no part in it.

## L1 frames on both phones, and the 640's first-load GPU crash (`0.2.3.98` / `0.2.3.99`, Oct 27, 2026)

**Why this run needed a reconstruction first.** The iframe/frame pages behind the `.85` Lumia 950
result had never been committed. The popup subset of `Src/tools/l1-popup-harness/` was tracked; the
frame pages were not, and by this session they were gone. They were rebuilt from the expected-title
list in `Src/tools/device-accept/probe-l1-arm-85.txt` (`index.html` plus `same`, `cross`, `nested`,
`nested-inner`, `scroll`, `sandbox-frame`, `sandbox-direct`, `srcdoc-direct`, `frameset`, `named`,
`remove`, `reload`, `dynamic`, `resize`, `frame-left`, `frame-right`). Before any phone was touched,
x64 `0.2.3.97` reproduced the `.85` titles verbatim with no app change:

| page | diag title on x64 `.97` |
| --- | --- |
| `/` | `L1 iframes=6 same=SAME-ORIGIN PARENT ACCESS OK srcdoc=SRCDOC OK` |
| `/sandbox-direct.html` | `SANDBOX C=SANDBOXED FRAME - gray = script blocked` |
| `/srcdoc-direct.html` | `SRCDOC inline=SRCDOC OKinline document, no network request` |
| `/frameset.html` | `FRAMESET frames=2 names=left,right w=674/675` |
| `/named.html` | `NAMED slot=/same.html text=SAME ORIGIN OK green background, 520x150` |
| `/remove.html` | `REMOVED OK iframes=0` |
| `/reload.html` | `RELOAD cross-blocked` |
| `/dynamic.html` | `DYNAMIC OK iframes=1 text=SAME ORIGIN OK green background, 520x150` |
| `/resize.html` | `RESIZE OK actual=600x220` |

The server log carried the negative evidence too: exactly one `/sandbox-script-ran.png` (the
`allow-scripts` frame's), no `/cross-origin-access-ran.png`, `cross.html` served from :8766 for
the cross frame and the reload, and `nested-inner.html` fetched once.

**The phone batch.** `Src/tools/device-accept/probe-l1-arm-light-98.txt` is the same nine pages
plus `example.com`, with the `.85` 45 s cold-home settle and no `dzen.ru` (deferred: the 640 has
little storage and a Snapdragon 400). It was embedded with `build-app-accept.ps1`, which bumped
the package and removed the staged copy afterwards; both bundles passed `Test-AppxPayload.ps1`
(30/30 ARM binaries, 52/52 exports, `Assets/device-run.txt` present).

**Lumia 950, `.98` -- passed.** All nine titles above, then `example.com`:
`url=https://example.com/ title=Example Domain ... pending=0 rs=C`. The user watching the screen
read the static example page as "frozen"; the diag says the load finished and nothing was pending.
Server log (192.168.1.34): one `sandbox-script-ran.png`, zero `cross-origin-access-ran.png`,
`cross.html` on :8766 twice (root + reload), both frameset frames fetched.

**Lumia 640, `.98` -- the first page load crashed, and the cause was the GPU default.** The APP
reached the root page and published its full title, then the process died. `gpu-steps.txt` ends:

```
[GPU] enter
[GPU] ensureWebCoreInitialized
[GPU] sharedDisplay
```

`gpu-crash.flag` held `1`, `gpuinit.txt` was never written. `settings.ini` had `gpudefault=1`; the
shell auto-calls `EnableGpu()` after the first real session (`MainPage.xaml.cs` around the
`_gpuDefault && !_gpuOn && !_gpuAutoTried` guard), and on this device
`PlatformDisplay::sharedDisplay()` is a hard native crash, not a catchable failure. The 950 did not
show it because its `settings.ini` already had `gpudefault=0` from earlier GPU experiments. On the
next 640 launch the crash-loop guard consumed the flag, saved `gpudefault=0`, deleted it, and the
home screen rendered (`after-home-render rc=0`). This is a first-run defect on the 640, not a frame
defect: the very same pages passed immediately afterwards.

**Lumia 640, `.99` -- passed.** The same batch, same app code, only a new version (a device rerun
requires one): all nine titles, then `title=Example Domain pending=0 rs=C`; the user watched the
removal frame disappear and a frame grow, which is the page's own script doing what the title
reports. Server log marker pattern was identical to the 950's. The `.99` run is the first L1 frame
acceptance on the 640.

**Tooling change.** `pull-device-logs.ps1` gained `-Scheme`/`-Port` (defaults unchanged:
https/443). The 640's https:443 listener times out from this bench while http:80 answers, and
every other device script already took a scheme; the log pull was the one step with no fallback.

**What this closes, and what it does not.** L1 frame acceptance now exists on both phones
(`.85`/`.98` on the 950, `.99` on the 640), with the popup fix carried by the published `.97` ARM
release. It does NOT prove popups on ARM (the phone batches are frames only), Back-from-a-popup
(no verb reaches the shell's Back), or three-consecutive-run stability. `.98`/`.99` are
embedded-batch acceptance builds, not releases. Open release fix from this session: do not let a
640's first page load reach `WebCoreGpuInit` by default -- GPU off on ARM, or explicit-toggle only.

---

*"Reviving the Windows Phone web for the people who never let it die."*

---

### AI agents contributing

- **opencode** - https://opencode.ai · model deepseek-v4-flash-free (opencode/deepseek-v4-flash-free) · Aug 2026
- **Claude Opus 5** - (to be added)
