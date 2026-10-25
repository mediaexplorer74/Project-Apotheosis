# Project Apotheosis — Roadmap to 1.0.0.0

> Born from the 2026-10-21 architectural review ("how far is 1.0.0.0?"). MVP acceptance is MET on
> both lines (Archive/Plan_MVP.md, commit d334be8, 2026-10-20) and the Nokia desktop graph is verified on the
> Lumia as of 0.2.3.56 ([Archive/Summary.md](Archive/Summary.md)). This document is the PHASED plan
> over the defect inventory that review produced: 10 primary items plus 7 secondary ones. Each item
> names its evidence; the evidence files (memory notes) are the investigation trail, not folklore.
> Maintainer rule for this plan: small steps first — every Phase S item is deliverable in one or
> two sessions.
>
> **Revised 2026-10-22:** added Phase 0 (the 1.0.0.0 exit contract — the roadmap previously said
> "how far" without saying "what counts as arrived"), split M5 into measure/levers/budgets, added
> M7 (device performance as its own axis), moved L3 (tabs) OUT of 1.0 by the maintainer's decision,
> and added the "Beyond 1.0" ideas section. `MVP.md` moved to `Archive/MVP.md` the same day.

## Phase 0 — what 1.0.0.0 means (the exit contract)

The MVP had an acceptance table ([Archive/MVP.md](Archive/MVP.md)); 1.0.0.0 had nothing. A roadmap
toward a number needs the number defined, so 1.0.0.0 is reached when ALL of the following hold:

1. **Device matrix green on BOTH phones.** The thirteen-site smoke list passes three consecutive
   runs with no process death on the Lumia 950 (3 GB, Snapdragon 810) **and** on the Lumia 640
   (1 GB, Snapdragon 400). The 640 has never been in the validation set at all — it is the machine
   that will feel every memory and speed defect first, so every Phase M/L verification below runs
   on it, not only on the 950.
2. **A memory number exists.** Peak RSS on the 640 stays under the budget M5.3 sets, measured by
   the smoke list on the heavy rows. No budget, or a budget nobody measured, fails this criterion.
3. **Memories survive a restart.** Cookies and localStorage persist (M1 + M2); a login made before
   closing the app is still made after opening it.
4. **The input matrix is green** (M4): every orientation, mode and gesture either works or fails
   with a named, honest reason — no silent refusals.
5. **`<iframe>` paints and issues a request** (L1). A "Chrome in miniature" that renders embedded
   content as an empty box is not one.
6. **Speed is a stated quantity, not a guess** (M7): startup-to-first-paint and scroll cadence are
   measured on both devices and recorded in this file.

Tabs (L3) are explicitly NOT part of 1.0 — see L3 below.

## Phase S — short term (days each, ship one at a time)

- **S1. Rotation adopt on the fixed-viewport line** — DONE in 0.2.3.57, **verified on the device
  2026-10-21** (markers `rotate:` → `drain:` → `pinch:` → `dtapfit:` at `1080x720`, plus the
  maintainer's finger reaching the CMP close-X in landscape). The fixed 720x1080 viewport used to
  ignore rotation entirely: the page kept its portrait shape, gestures right of it were outside the
  page. `OnContentAreaSizeChanged` now swaps the viewport pair through `PostZoomAndRepaint`,
  deferred via `_vpDirty` when the engine is busy. Landscape input dead is CLOSED: the rotation
  no-op (fixed here) plus the 0.50 zoom floor (by design).
- **S2. A hung subresource must not swallow the next navigation** — DONE in 0.2.3.58,
  **verified on the device 2026-10-21** (0.2.3.59 batch, probe-s2-queue.txt v2). The shell-busy
  gate used to drop a `goto` SILENTLY while one pending resource hangs (measured: Usercentrics
  `loader.js`, curlcode=28, >60 s on the Lumia, 0.2.3.56 probe). Fixed as a QUEUE: `NavigateTo`
  holds the ask in `_pendingNav` and `SetLoading(false)` delivers it one dispatcher turn later —
  every load end passes through there, including the 40 s watchdog, so a wedged load cannot hold
  the queue forever; a newer ask supersedes an older one. Device markers, in order:
  `nav QUEUED, shell busy: https://example.com/` (+16 ms after the first goto) →
  `nav QUEUED delivered:` (+12.6 s, when the first load settled) → final
  `diag url=https://example.com ... rs=C` (the delivered page IS the final page — without the
  fix this diag still says Nokia), with the superseded reload of the first load cleanly discarded
  (`frame superseded seq 3 -> 4`). Note for probe authors: a `wait` between the two gotos defeats
  the probe — the wait verb re-posts the batch to the engine thread (busy for the whole load);
  issue the gotos back-to-back.
- **S3. The address bar follows the engine** — DONE in 0.2.3.66, **verified on the bench
  2026-10-22**. Two halves were open; both are closed now.
  *The mechanism (0.2.3.63):* `OnLiveTick` reads `WebCoreGetUrl` on the engine thread every
  tick and, when the engine's document url differs from `_currentUrl`, writes the box (guarded
  by `_urlFocused`/`_urlSyncing` so a mid-keystroke overwrite cannot happen). The engine hop
  itself is exercised by the `clicklink` verb (0.2.3.64): `WebCoreClickAt` called directly on
  the engine thread, nothing written to the shell — the shape of an SPA route change.
  *The instrument was broken (0.2.3.65):* `clicklink`'s coordinates came from `WebCoreGetLink`
  as CSS px while `WebCoreClickAt` takes engine-surface px; at the bench's fitwindow zoom 0.80
  the click landed 25 % down-right of the anchor and hit `<html>` instead (measured: `[HIT]
  846,742 -> <a>` from the probe's own elementFromPoint versus the events landing on `html` at
  the same numbers, because elementFromPoint takes CSS px and the event handler converts from
  surface). Fixed in the port: `extractLinks` stores rects as CSS × `g_pageZoom` (surface px —
  the space `WebCoreClickAt` and the shell's `_pageLinks` hit-test both speak), and
  `WebCoreClickAtImpl`'s three elementFromPoint probes divide back to CSS.
  *The watch was dying (0.2.3.66):* even with a landed click, the tick had already stopped —
  `_liveStaticTicks >= 40` (8 s of static frame) called `StopLiveMode()` before `clicklink`
  ran, so nothing noticed the hop (measured on .65: clicklink navigated at 15:23:54, the tick
  had stopped at ~15:23:44, no `url follows engine` line, and the diag at 15:24:02 — repeated
  at 16:49:41 — still read `url=http://example.com/ | url=https://www.iana.org/...` with the
  engine two documents ahead of the shell). Fixed: a static frame now SLOWS the tick to 1 s
  (`live tick: static, slowed to 1s (watch stays on)`) instead of stopping it — the tick is
  the only thing that runs while a page sits still, so it must outlive the stillness. Also
  `AutoLog` moved BEFORE the box write in the update path: the observation that proves S3
  must not sit after `UpdateLockIcon`/`UpdateUrlActionGlyph` inside a `catch{}` that eats
  whatever they throw. Verified on .66: `clicklink` at 16:55:05.132 → `url follows engine:
  https://www.iana.org/help/example-domains` at 16:55:05.174 (42 ms), and the trailing diag
  reads `url=... | url=...` with both sides equal.
- **S4. The `?` bytes in the address box** — DONE in 0.2.3.62, **verified on the bench
  2026-10-22**. The `?`-bytes half did NOT reproduce: a full static audit of the C# line found no
  path that can put a literal 0x3F in `UrlBox` (every native string crosses as a UTF-8 `byte[]` —
  the 2026-10-03 marshalling fix; file reads are UTF-8; the string tables, XAML and the home/error
  page builders carry no destroy literals; device logs across many sessions show zero `?`-runs in
  url fields), and the probe measured it: two "engine URL unknown" scenarios on the .61/.62 bench
  (`error page: url= | url=https://nonexistent.invalid/`, `tap: url= | url=https://example.com/`)
  show no `?` bytes anywhere in the box (docs/Archive/Summary.md, 0.2.3.62 section; the audit trail
  is DESTROYED-STRINGS.md, "repaired 2026-09-18, nothing open"). The *remedy* the item named —
  "render the previous URL instead" — did name a real defect, one gate over: the probe caught the
  `.58/.59` "empty `url=` after a tap" anomaly in the act. `ApplyEngineFrame` tested `navUrl != null`
  and the tap path passes `""` when the engine URL is unchanged/unreadable, so a tap on a non-link
  CLEARED `_currentUrl` and blanked the box while the page stayed put (measured .61: box empty on
  example.com; engine diag still `url=https://example.com/`). Fixed by treating an empty navUrl as
  "the engine could not name the document" — keep the previous URL; only `NavigateTo` clears the
  box, and only because it has a real target. Verified on .62: same probe, box holds
  `https://example.com/` after the tap. Remaining in the S3 family: engine-initiated hops the shell
  never asked for (redirects, SPA).
- **S5. Tooling default fix** — DONE in code 2026-10-21, marked done in this file 2026-10-22.
  `pull-device-logs.ps1`'s `-PackagePattern` default is now `ME.Apotheosis_*` (the shipped app);
  the old default named the C++ harness package, which no line has deployed since the app became
  the product, so every recent device pull passed the pattern by hand.
- **S6. Log trimming, not rotation** — DONE in 0.2.3.61, **verified on the bench 2026-10-21**.
  `rename()` fails in the AppContainer (`apoTraceAppend` holds `port-trace.txt` open for the whole
  process, MSVC `fopen` omits FILE_SHARE_DELETE, and the rotation handler is a bare `return`), so
  the 4 MB rotation never fired once in an install's life. The fix is managed, not WebKit: `App()`
  trims the OLDEST HALF of `port-trace.txt` / `gpu-steps.txt` / `autocmd-log.txt` whenever one
  exceeds 4 MB, at the only moment the files are provably unowned — before MainPage, before the
  first P/Invoke, before WebCoreDriver.dll is loaded. Measured on the bench (.61): three seeded
  files of ~5.1 MB each → all three halves dropped at 23:23:07.3xx (before `onlaunched` .688);
  each trim leaves an ASCII seam `trim: oldest half dropped on open (was N bytes)` at the head and
  a `trim <file> <was> -> <now>` line in boot.txt. Two file-specific facts: `gpu-steps.txt` is
  truncated by the driver itself at startup (`WebCoreSetGpuInitLogFile`, once per process), so the
  trim of it is belt-and-braces — the seam is overwritten by the first run, and the file is bounded
  anyway; `autocmd-log.txt` appends ACROSS installs and is the only file here the trim actually
  reclaims for the long haul. A WebKit-side fix (FILE_SHARE_DELETE) would cost a full rebuild on
  both lines for no user-visible gain.
- **S7. Scale the fixed frame to the window** — DONE in 0.2.3.67, **verified on the x64 bench
  2026-10-23**; the device run is queued (probe ships as `device-accept/probe-s7.txt`, expected
  scale 0.654 at area=480x707). The defect: measured on the Lumia by the .57 batch's own tap row,
  `area=360x494` — ContentArea was 360x494 DIP (window 360x640) while the engine frame is a fixed
  720x1080 drawn Stretch=None at 1:1 DIP with no Viewbox, so the user saw the top-left quarter,
  the rest of the window was white, and each engine pixel covered ~4x4 physical pixels (the
  reported "softness"). The frame itself must NOT shrink to the window: 360 / kZoomMin(0.50) =
  720 CSS < 870, the desktop gate would never open. Fix: scale only the PRESENTATION —
  `FrameFitScale` (a ScaleTransform on the `FrameFit` wrapper) draws the bitmap as large as fits,
  never above 1:1, top-left anchored; `_fitScale` is recomputed by `ApplyDisplayScale()` at every
  coordinate mapping, every blit and every size change, and the tap/manipulation paths divide by
  it (the live pinch preview is a child transform whose focal point is converted to image space).
  Bench evidence (.67): fixed viewport `frame fit scale=0.695 frame=720x1080 area=1355x751` — the
  whole page visible (s7-1-fixed.png); fitwindow=1 unchanged at `frame fit scale=1.000
  frame=1355x751 area=1355x751` (s7-2-fitwindow1.png); rotation adopts landscape 1080x720 at 1.0;
  and a resized 987x814 window gives 0.914, where `tap dip=494,619 area=987x814 fit=0.914
  session=1` on engine link `[0] 500,666 80x23` landed exactly on its target
  `https://www.iana.org/help/example-domains` (both diag url fields equal). Evidence:
  [Archive/Summary.md](Archive/Summary.md) 0.2.3.57 section (defect measurement) and 0.2.3.67
  section (bench verification).
- **S8. Input must not be gated by a hung load** — DONE in 0.2.3.60, **verified on the device
  2026-10-21** (batch probe-s8-input.txt + the maintainer's finger; Summary 0.2.3.60 section).
  `HandleTapAt` refused every tap while `_loading` was true, SILENTLY: a Nokia load held by one
  hung Usercentrics subresource made the CMP's "Accept All" dead however often it was pressed —
  the same disease S2 cured for navigation, one gate over (`EngineScroll` had the same gate).
  Fixed by dropping `_loading` from both (`_interacting`/`_sessionActive` stay); the maintainer
  then closed BOTH the CMP overlay and the site's 7-step wizard with a finger during a hung load.
  The scroll half of the scare ("the page does not scroll") was diagnosed, not fixed, by S8's
  markers: every drag reached the engine (`scroll dx= dy= at X,Y rc=0`), and the `[SCT]` lines in
  gpu-steps.txt show the engine honestly finding nothing to scroll — `out=0|||` plus frame
  `range=0,0..0,0` — on the half-loaded CMP page, while yesterday's clean load moved
  `DIV.mobile-view` by 384 CSS px through the same walk. `rc=0` means "completed and painted",
  never "moved". The dead scroll on Nokia is M3's CMP hang (the archive is a 100vh app: pinch
  cannot create document range at any zoom); seeing the whole frame is S7.
- **S9. The first-visit `dzen.ru` white window** — DONE on x64 2026-10-25, **post-0.2.3.73**.
  The first visit is a form chain, not a blank site: `dzen.ru` -> `sso.passport.yandex.ru/push`
  (script POST) -> `sso.dzen.ru/install` (200, CSP `form-action 'none'`, inline
  `location.replace`) -> the real page. `FrameLoader::submitForm` records the action URL in
  `m_submittedFormURL`; upstream clears it on error or user gesture but not on a successful
  commit, so the install document's `form-action 'none'` was applied to its own redirect and
  `DocumentLoader::willSendRequest` cancelled it. The port now clears the guard in
  `dispatchDidCommitLoad`; real form checks and redirects during a submission are unchanged.
  Measured on x64 with a side-by-side `ME.Apotheosis.DzenTest 0.2.3.77` package: the real visit
  reached `dzen.ru` with `body=1`, `nonwhite=228105/777600`, 49 scripts and a title, no
  `form-action` error; a local two-origin chain ended at `REDIRECT OK`, a `form-action 'none'`
  page still refused its form, and a plain form still submitted. L1 (`createFrame`) is unrelated
  to this defect. The `.73` ZIPs stay frozen; bump the manifest before the next release build.
  **Follow-up, same day (`.79`):** the form fix exposed a second dzen gate. With the unified UA
  pair (`Src/port/PortUserAgent.h`) and a desktop Edge default the `browser-switch-trap` is gone,
  but Yandex Direct still serves a "download Yandex Browser" splashscreen to Edge. A host-specific
  desktop UA now sends `YaBrowser/24.1.0.0` to `dzen.ru` and the Yandex domains listed in
  `PortUserAgent.h`, while ordinary sites keep Edge; custom UA still wins and mobile stays iPhone.
  Measured on x64 `.79`: dzen clean, `ya.ru`/`example.com`/`lenta.ru` load, the mobile probe sends
  the iPhone string, and the suffix matcher passes 21 URL cases. Microsoft blocks this bench client
  with Edge too, so that block is not UA-specific. L1 remains next.

## Phase M - medium term (weeks each)

- **M1. File-backed cookie jar** - **DONE in 0.2.3.73, verified on x64 2026-10-23.** The jar is
  `LocalState\cookie.jar.db`, opened by `WebCoreSetCookieJarPath`; `CookieJarDB` is the source of
  truth and every curl submit seeds from the whole jar and writes `Set-Cookie` back. Measured:
  clean DB `rows=0`; `apo=m1` cookie set at `httpbin.org`; cookie hashes `0x58496e67` ->
  `0x6e41bac1` -> `0x9f9ccae4`; full app restart `rows=1`, `seed=1`, hash unchanged; the DB row is
  `('apo','m1','httpbin.org','/',1792838096056,0,0,0)` with `user_version=1`. The earlier 2026-09-18
  experiment that JOINED two disjoint jars is unrelated and remains closed. M3 now has the persisted
  consent-cookie option it was waiting for.
- **M2. Storage durability** - **DONE on x64 and ARM, verified 2026-10-25 (post-0.2.3.73; not in
  the `.73` release ZIPs).** `localStorage` now has a durable backend. `WebCoreSetLocalStoragePath`
  points the local namespace at `LocalState\localstorage.db`; one `ItemTable` holds every origin,
  with `journal_mode=DELETE` and `synchronous=FULL` so a committed write survives a force-stop. A
  `PortStorageArea` loads its origin's rows when it is created and commits set/remove/clear before
  dispatching the storage event. Session and transient-local storage stay in memory by design; a
  failed DB open falls back to the old in-memory backend and says so in the trace. Measured on a
  side-by-side `ME.Apotheosis.M2Test 0.2.3.74` x64 package (the source identity stays
  `ME.Apotheosis 0.2.3.73`, so the installed `.73` app and its LocalState were untouched): a httpbin
  page set `m2-durable=durable-0.2.3.74`, title `M2 SET value=durable-0.2.3.74 len=16`, DB row
  `('https://httpbin.org','m2-durable','durable-0.2.3.74',1)`; a force-stop left the row; the next
  launch logged `storage: db path=... rows=1`, `storage: namespace local quota=5242880 session=1
  backend=sqlite`, `storage: restored origin=https://httpbin.org keys=1 bytes=52`, and the check
  title read `M2 CHECK value=durable-0.2.3.74 len=16 total=1`. A remove/clear/re-set page reported
  `M2 OPS rm=true cl=true re=true len=1`; the DB held only the durable row (ordinal 5), and a second
  force-stop/restart check passed. The ARM line followed on 2026-10-25: the side-by-side
  `ME.Apotheosis.M2Test` package was installed on the Lumia 950 (`192.168.1.35`, OS 15254.603) and
  the Lumia 640 (`192.168.1.39`, OS 15063.1868). `0.2.3.74` ran the SET page and reported
  `title=M2 SET durable-0.2.3.74 len=16 total=1`; a real force-stop, then `0.2.3.75` ran the CHECK
  page and reported `title=M2 CHECK durable-0.2.3.74 len=16 total=1` on both phones. Each DB held
  the row `('https://httpbin.org','m2-durable','durable-0.2.3.74',1)` with `user_version=1`; the
  restart logged `storage: restored origin=https://httpbin.org keys=1 bytes=60` and `rows=1`.
  Evidence: `Src/tools/device-logs/20261025-m2-l950-*` and `...-l640-*`; batches:
  `Src/tools/device-accept/probe-m2-arm.txt` and `probe-m2-check-arm.txt`. The ARM driver and app
  also passed `Test-AppxPayload.ps1 -ExpectedArch arm` (30/30 binaries ARM, driver at the package
  root, 48/48 exports).

  The first ARM run exposed a viewport defect that x64 could not show: the fixed 720x1080 frame was
  shrunk to 0.457-0.458 by the 360x494-DIP content area, so the mobile home page rendered at half
  size. `0.2.3.76` turns `fitwindow` on by default on `Windows.Mobile` and writes
  `fitwindow_migrated=1` so a later explicit off is not overwritten; `BuildHomeHtml` now uses
  `clamp()` sizes and an `auto-fit minmax(260px,1fr)` grid. Measured on both phones:
  `applysettings: ... fit=1`, `adopt: no session 360x494 z=1,00`,
  `frame fit scale=1,000 frame=360x494 area=360x494`; `settings.ini` carries `fitwindow=1` and
  `fitwindow_migrated=1`; the maintainer confirmed the home page is correctly sized on both. The
  L950 still looks softer than the L640: software present hands the compositor a 360x494-DIP bitmap
  and the 4x panel upscales it twice as far as the 2x panel. That is a present-resolution issue, not
  a fit regression; a physical-resolution software present remains open. WDP on these phones has no
  screenshot endpoint (`/api/control/screenshot` -> 404) and no device screenshot was captured. The
  `.73` ZIPs were built before M2 and remain frozen; bump the manifest before packaging the next
  release from HEAD.
- **M3. Consent/CMP experience** — the Usercentrics overlay dims pages and returns on every launch
  (a consequence of M1 plus their "unsupported browser" check). Options: persist the consent
  cookies once M1 lands; ship a document-start rule that closes a known CMP overlay. Site-side
  behaviour, not a rendering defect.
- **M4. Input matrix per orientation and mode** — the 0.50 zoom floor reads as "pinch is broken";
  the fitwindow=0 pinch branch and the landscape/inner-scroller cases need one test matrix (verb
  batches + the finger path) instead of spot fixes.
- **M5. The memory ceiling — intelligent consumption control.** Heavy pages still cost hundreds of
  MB with every release lever withdrawn. "Ceiling" alone was too vague to schedule, so the item is
  split; the three stages run in order (measurement before levers, levers before budgets — the
  plan's own rule):
  * **M5.1 — close the blind spots (measure first).** Three readings currently cannot inform any
    decision: there is no readable engine-allocator total (`fastMallocStatistics()` is a peak
    working set, `memoryFootprint()` returns 0 in the App Container), decoded bitmaps report
    `decodedB=0` always on the cairo path, and `evictResources()` can drop a live client
    (default-off behind `cacherelease.txt`). Deliverable: one honest per-site memory profile of
    the thirteen-site list on both devices, with each of the three gaps either closed or named
    unfixable-with-why.
  * **M5.2 — levers (influence the consumption, not only watch it).** Wire the UWP pressure signals
    — `MemoryManager.AppMemoryUsage`, `AppMemoryUsageLevel`, `AppMemoryUsageIncreased` /
    `InviteMemoryChange` — to a real "shrink now" path: at `High` the browser compresses itself
    (cache capacity down, decoded surfaces released, history trimmed) instead of waiting for the OS
    to kill it. On the 1 GB 640 that path is the difference between a slow browser and a dead one.
    Plus `MemoryCache::setCapacity()` driven by device class rather than a value chosen for the
    950.
  * **M5.3 — budgets (write the number down).** An agreed peak-RSS budget per device class,
    enforced as a row of the smoke list, and fed by Phase 0 criterion 2. Until a number exists and
    is checked, M5 is not done.
- **M6. Watchdog killability** — the JSC watchdog fires but termination does not stick on a page
  dispatch cycle; a runaway JS loop is detectable (heartbeat tickstep=1) but not curable in-app.
- **M7. Performance baseline on the device (speed is its own axis, not a side effect of memory).**
  MVP explicitly scoped performance tuning out; 1.0 does not (Phase 0 criterion 6). Measure on
  BOTH devices with the instruments that already exist (`engineact`, `finished`, `[SCT]` in
  gpu-steps.txt, the heartbeat): startup → first paint, a typical page load end-to-end, scroll
  cadence on a tall page. Record the numbers here, then take the three most expensive findings
  into the queue as follow-ups. Decision to make from those measurements, not by default: the GPU
  present question ([GPU-LIVENESS.md](GPU-LIVENESS.md) is still on pause) — software present on a
  Snapdragon 400 may cost proportionally far more than on the bench, and "software is the MVP-safe
  path" was an MVP verdict, not a performance one.

## Phase L — long term (months each)

- **L1. Frames and popups** — **iframes/frames DONE on x64 in `0.2.3.80` (2026-10-25); phone
  acceptance pending.** `LoadingFrameLoaderClient::createFrame` now mirrors WebKitLegacy's
  `_createFrameWithPage`: effective sandbox flags are the owner's plus the parent's, the referrer
  policy falls back to the top document only when the owner's is the empty string, the frame is
  built with `LocalFrame::createSubframe`, named with `tree().setSpecifiedName()` and initialized
  with `FrameLoader::init()`; `transitionToCommittedForNewPage` gives every subframe a fresh
  `LocalFrameView` (the driver keeps owning the main frame's view). Measured on the x64 bench with
  a local two-origin harness, the server log as request evidence and `FRAME: created`/`committed`
  lines as lifecycle evidence: seven initial frames (same-origin, cross-origin, nested, clipped/
  scrolling, sandbox, `srcdoc`) all requested and painted; removal, `src` change, dynamic creation
  and resize work; sandbox without `allow-scripts` executes no script while `allow-scripts`
  executes exactly one; `srcdoc` commits as `about:srcdoc` with no network request; a `<frameset>`
  renders two 50% columns; `target="slot"` navigates the existing named frame (created-frame count
  unchanged); a same-origin child reads and writes the parent document while a cross-origin child
  is blocked (1 vs 0 marker requests). `dzen.ru` still paints with its ad iframes; `example.com`
  and `lenta.ru` are unchanged. **Still open:** `window.open` and `target=_blank` remain no-ops
  because `PortChromeClient::createWindow` returns nullptr — popups need a host-side window/tab
  decision and are a separate item from this iframe pass. Phone acceptance is deferred until a
  Lumia is back on the bench.
- **L2. ARM JavaScriptCore currency** — the ARM JSC predates the RedBlackTree insert crash cure, and
  a header-only cure cannot travel through a relink: the fix is a real JSC rebuild in a build dir
  that CANNOT be reconfigured without breaking (one cmake edit costs a full ~10 h rebuild). Plan a
  dedicated build window; back the dir up first.
- **L3. Tabs / multi-session — OUT OF 1.0 (maintainer's decision, 2026-10-22).** Kept here as a
  post-1.0 item, not a v1 one: the maintainer has taken multi-tab out of the v1 frame, and M5
  depends on it anyway (one Page per tab, chrome state, a story for background tabs — throttling,
  memory). M5's motivation restated accordingly: comfort and survivability on a 1 GB device, not
  a feature gate. Reopen after 1.0.0.0 with M5.3's budget in hand.
- **L4. Reproducible build pipeline** — from-scratch configure was never tested; the two lines
  carry two SDK header versions; every release is a manual relink → appx → audit → deploy dance.
  Target: one script from clean checkout to deployed bundle on either line.
- **L5. Runaway-JS recovery** — beyond M6's killability: a heartbeat-driven page-level recovery
  (reload the offending document, keep the session) so one bad page cannot wedge the browser.

## Beyond 1.0 — ideas, deliberately unscheduled

- **Headless / spoken-summary mode.** The page body is never rasterised for the user at all: the
  document is fetched and *analysed*, then delivered as a short textual summary and/or spoken by a
  TTS engine ("what is on this page", read-aloud for the archive — a natural fit for a museum
  browser on a device whose screen is its weakest part). Architecturally this is a second pipeline
  beside render: fetch → extract/summarise → present (text or speech), reusing the loader, history
  and error-reporting paths while bypassing layout/paint entirely — which also makes it a cheap
  mode on weak hardware. The summariser backend (local heuristic vs. an AI service) is a decision
  for whenever this is taken up, not now. **First foothold, landed 2026-10-22:** an experimental
  register-only selector under Settings → Advanced (`SetHeadlessCombo`: Off / Summary / Speak,
  saved as `headless=` in `settings.ini`, localized in all three languages) — it marks the intent
  in the UI, costs nothing, and gives the later work a place to attach to. Nothing reads the value
  yet, by design. Do not schedule the mode itself before 1.0.0.0; the stub must stay harmless (a
  stored enum nothing reads).

## How to run this plan

1. One Phase S item per session, smallest first; every ship goes through
   `build-app-accept.ps1` + `Test-AppxPayload.ps1` + `Wdp-Deploy.ps1`, and a device-visible change
   ships with its probe batch in the same appx.
2. Phase M items start only with a written measurement of the current state (the evidence links
   above ARE that state as of 2026-10-21).
3. L1 and L2 are the two big bets; they should not start on the same day — both consume the ARM
   build directory, and it cannot serve two masters.
4. **Validation runs on both devices.** From the 2026-10-22 revision, a Phase M/L item is not
   "verified" on the strength of the Lumia 950 alone: the same probe runs on the Lumia 640, and
   Phase 0 is judged on the 640's numbers where the two disagree.

*"Reviving the Windows Phone web for the people who never let it die."*
