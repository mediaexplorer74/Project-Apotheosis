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
   (1 GB, Snapdragon 400). The 640 entered the validation set on 2026-10-27 (L1 frames, `.99`),
   but the thirteen-site list has still not run there — it is the machine that will feel every
   memory and speed defect first, so every Phase M/L verification below runs on it, not only on
   the 950.
2. **A memory number exists.** Peak RSS on the 640 stays under the budget M5.3 sets, measured by
   the smoke list on the heavy rows. No budget, or a budget nobody measured, fails this criterion.
3. **Memories survive a restart.** Cookies and localStorage persist (M1 + M2); a login made before
   closing the app is still made after opening it.
4. **The input matrix is green** (M4): every orientation, mode and gesture either works or fails
   with a named, honest reason — no silent refusals.
5. **`<iframe>` paints and issues a request** (L1). A "Chrome in miniature" that renders embedded
   content as an empty box is not one. Measured on both phones 2026-10-27 (`.98` on the 950,
   `.99` on the 640); see Phase L.
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
  **Connect-timeout follow-up, `0.2.3.95` (2026-10-27):** the maintainer reported dzen opening only
  on roughly the fifth attempt while Chrome opened it first try. The trace says the site was never
  down: every failed hop read `SL: hop[0] status=301/302 ...` followed by
  `SL: curl done crc=28 http=301 size=0` at 3.2-4.4 s, i.e. `CURLE_OPERATION_TIMEDOUT` raised by
  `CURLOPT_CONNECTTIMEOUT=3` while resolving/connecting the SECOND host of the SSO chain
  (`sso.passport.yandex.ru`, `login.vk.com`) — DNS resolution is part of that 3 s. Dzen has belonged
  to VK since 2022, so the VK ID hops are its normal account path, not a suspicious redirect. The
  same log's successful load took 7.7 s against the 8 s TOTAL limit, one slow response away from the
  same false failure. Raised to connect 10 s / total 30 s; the 1 B-per-2 s low-speed rule and the
  app's 40 s watchdog still end a genuinely dead transfer. Measured on `.95`: three consecutive
  dzen loads, engine `SessionLoad` 6528 / 7104 / 6969 ms, all rc=0; the middle one exceeds the old
  8 s wall clock and would have been a fourth false failure under the previous limits.

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
  *Result, 0.2.3.94 (2026-10-27):* the cookie jar plus the `Edg/156` UA close the CMP arm: consent
  persists in `LocalState/cookie.jar.db` and `localStorage.ucData`, and the banner does not return.
  The remaining NDA symptom — graph nodes piled in the canvas' top-left corner and entry A0001
  auto-opened — was NOT the CMP. Root cause measured with the new `js` autocmd verb and a local
  viewport probe (`Src/tools/l1-popup-harness/vw.html`): the first document was created at the
  fixed 720x1080 frame because the ctor navigated before the XAML tree was measured; the fitted
  1026x695 resize arrived 1602 ms AFTER `load` (boot, DOMContentLoaded and load all read
  720x1080). NDA branches on `innerWidth` once at module init (mobileBreakpoint 870), so 720
  selected its mobile path: `?node=A0001` auto-selected and the d3 force simulation was paused at
  its initial phyllotaxis layout — centred on (0,0) of the site's 1860x1600 canvas, exactly the
  top-left pile. Fix: `_bootUrl` / `BootNavigate` / `TryBootNavigate` in `MainPage.xaml.cs` hold
  the boot load until the first ContentArea pass and write the fitted geometry into kW/kH BEFORE
  `WebCoreSessionLoad`. Measured after .94: `vw.html` boots at 1026x695 with no resize events; NDA
  boots at `https://nokiadesignarchive.aalto.fi/` with no node overlay and the graph laid out
  across the visible canvas (`Src/tools/shots/nda94-full.png`, `nda94-fresh.png`). The 1860x1600
  canvas is the SITE's own floor (`Math.max(innerWidth,2200)` by `Math.max(innerHeight,1600)`),
  not an engine defect; on a 1355x751 window the site shows the top-left of its 2200x1600 design
  and the force centre is at (949,648). The missing Welcome/CMP modals are persisted state, not a
  regression: cookie `intro_v1_network=true`, consent in `localStorage.ucData`.
  A maintainer resize check on the same build confirmed the responsive branch is live rather than
  frozen at boot: narrowing the window switches NDA to Node Detail, widening restores the graph.
  The initial-viewport fix therefore seeds the correct first document without pinning the layout to
  one branch. One expectation needs a guard before the phone batch: the measured ContentArea on the
  Lumias is 360x494 DIP, so landscape is about 494 CSS px, still below NDA's 870 px mobile
  breakpoint. The phone is therefore expected to keep the Node Detail branch in BOTH orientations;
  a missing graph there is the site's responsive design, not a boot-fix regression. The batch
  should verify that Node Detail is usable and that no top-left node pile appears, not force the
  desktop graph onto a 494 px viewport. (If the graph is ever required on the phone, that is a
  site-breakpoint or page-zoom decision, not an engine fix.)
  Maintainer recall (2026-10-27) explains the older observation: before `fitwindow` became the
  mobile default in 0.2.3.76, the fixed frame was swapped to 1080x720 in landscape, so NDA saw
  1080 CSS px (>870), took its desktop branch and rendered the graph downscaled to the panel. The
  fitted line sees 494 px and takes mobile. The old graph was a desktop canvas rendered small, not
  proof that the phone can show the desktop layout at 1:1; both behaviours are correct for their
  respective viewport, and neither is a boot-fix regression.
  *ARM synchronization package, 0.2.3.94 (2026-10-27):* the ARM driver was rebuilt from the same
  port sources and the app bundle was built and audited without deployment. `WebCoreDriver.dll`
  now carries 52 exports including `WebCoreEvalJS`; `Test-AppxPayload.ps1 -ExpectedArch arm`
  reports 30/30 binaries ARM, the driver at the package root, 52/52 declared exports and the
  expected capabilities (`AUDIT PASSED`). The bundle is
  `Src/Apotheosis/AppPackages/Apotheosis_0.2.3.94_Test/Apotheosis_0.2.3.94_arm.appxbundle`
  (46.6 MB). It has NOT been deployed or run on either Lumia, so the boot fix and the `js` verb
  remain x64-measured only.
  *ARM release candidate, 0.2.3.95 (2026-10-27):* rebuilt from the same sources as the x64
  `.95` build, carrying M5.1 `SL: pmem`, the dzen timeouts and the hardware-keyboard route.
  `Test-AppxPayload.ps1 -ExpectedArch arm` again reports `AUDIT PASSED` (30/30 ARM binaries,
  52/52 exports, driver at the package root); `WebCoreDriver.dll` in the bundle hashes to
  `F05029135A2FBE90EBC3C2A1377C70EED0296B7E703FC363C78A8C2D179A7038`, matching the staged
  driver. The bundle is
  `Src/Apotheosis/AppPackages/Apotheosis_0.2.3.95_Test/Apotheosis_0.2.3.95_arm.appxbundle`
  (46.6 MB). Sideload archive for the maintainer's GitHub Release upload:
  `C:\Users\media\Desktop\Apotheosis_0.2.3.95_ARM_sideload.zip`, 52,509,709 bytes (50.08 MB),
  SHA256 `3BCBBF5F95D2F9B4C2C68276F55438BDF45B594B691240EF64C9E49D4346D973`. It contains the
  bundle, the public `CN=media` certificate (thumbprint
  `9775F34A02030380B99FFF4A1104CBFF8993A183`, valid to 2027-09-26), the three ARM dependency
  packages, `Add-AppDevPackage.ps1` / `Install.ps1` and a README. NOT deployed or run on either
  Lumia; the device-half checks stay deferred to the next light batch.
  *ARM release candidate, 0.2.3.97 (2026-10-27):* rebuilt from the same sources as x64 `.97`,
  including the 12 s fetch window and the double-tap / Fit-toggle removal. `Test-AppxPayload.ps1
  -ExpectedArch arm` reports `AUDIT PASSED` (30/30 ARM binaries, 52/52 exports, driver at the
  package root). Bundle SHA256
  `60D27372DAC4572754D87D8A11E4358A04721EB8A10811BFF96AB07B71AEDAF5`. Sideload archive on the
  desktop: `C:\Users\media\Desktop\Apotheosis_0.2.3.97_ARM_sideload.zip`, 52,509,308 bytes
  (50.08 MB), SHA256 `719EEC24778057748F09C250AADDD488BD84D98C64B997CA3E28EBC7F658E10E`.
  It contains the bundle, the `CN=media` certificate, the three ARM dependency packages,
  `Add-AppDevPackage.ps1` / `Install.ps1` and an updated README; the `.95` archive is left beside it
  for rollback. NOT deployed or run on either Lumia; the device checks stay deferred.
  *ARM release, 0.2.3.100 (2026-10-29):* the first ARM release to ship the GPU default fix --
  ARM starts with `gpudefault=0`, the GPU path is opt-in via Settings or `settings.ini`, and the
  640's first-load crash is gone (proved on a fresh install; see M1). This is also the build that
  passed the popup and Back acceptance on both phones (L1 below). Rebuilt clean with
  `build-app-arm.bat`, so the bundle contains no `Assets/device-run.txt`;
  `Test-AppxPayload.ps1 -ExpectedArch arm` reports `AUDIT PASSED` (30/30 ARM binaries, 52/52
  exports). Opening the inner ARM `.appx` confirms no `Assets/device-run.txt`; the acceptance copy
  still has it. Bundle: 48,864,049 bytes, SHA256
  `4B8F9F43946C5FBD0BF04B73F61F6C4AE4C827D8D7DEF9E107A267FBC26B7B79`. Sideload archive for the
  maintainer's GitHub Release upload:
  `C:\Users\media\Desktop\Apotheosis_0.2.3.100_ARM_sideload.zip`, 52,508,002 bytes, SHA256
  `D75B17F54ADF6F65DC80E8196FF34A97BDF2285A3D5C8B45C49C95E479906EC2`. It contains the bundle,
  the `CN=media` certificate (thumbprint `9775F34A02030380B99FFF4A1104CBFF8993A183`), the three
  ARM dependency packages, `Add-AppDevPackage.ps1` / `Install.ps1` and a `.100` README. The test
  phones currently carry the embedded-batch acceptance bundle of the same version, so re-installing
  the clean bundle over it would be refused (`0x80073CFB`); users coming from `.97` are unaffected.
  *Connectivity and viewport simplification, 0.2.3.96-97 (2026-10-27):* `.96` replaced the short
  fetch-stall window with 12 s and a 32 s ceiling, matching the 10 s connect and 30 s total curl
  timeouts (the app's 40 s watchdog stays the outer bound). One x64 process then loaded five targets:
  rbc.ru 11385 ms boot + 3385 ms reload, en.m.wikipedia.org 10408 ms, reddit.com 17272 ms (its JS
  challenge returns an 8406-byte response and then the real front page), dzen.ru 6611 ms — all
  `rc=0` — and a nonexistent host failed honestly in 7993 ms with `rc=-16 type=Curl curlcode=6
  Could not resolve host`. No `DISCARDED` line appeared, so the 12 s window never fired; the earlier
  "false failures" were connect timeouts inside redirect chains, not dead sites.
  NDA's graph geometry is the site's own: on x64 `.96` the page reports `#plot w=1860 x=340` and a
  `1860x1600` canvas while the window is 1355x751; desktop Chromium at `innerWidth 1062` reports the
  identical `x=340, w=1860` and canvas. The off-centre graph is therefore not an engine defect, and
  no viewport change should be made for it.
  The "three-column Node Detail + Network Graph" state was our double-tap-to-fit: a scripted `dtap`
  took page zoom 1.00 -> 2.00, which halved the CSS viewport to 677 px and crossed NDA's 870 px
  breakpoint; the site's own resize handler then selected `?node=A0001` 0.6 s later **with no click
  sent anywhere**. Zooming back to 1.00 restored 1355 px but left the detail open over the graph.
  ChromeView does not do this because a desktop double-click is two ordinary clicks.
  `.97` removes the whole ambiguity instead of gating it by device: the double-tap gesture, the
  250 ms tap hold, `_lastDoubleTapMs`, the `DoubleTapFit`/`dtapfit` code path and the `dtap` verb are
  gone, and every `Tapped` is forwarded immediately. The Settings Fit toggle is gone too — the fitted
  viewport is the only line, and Zoom is the single user-facing scale control. The fixed 720x1080
  frame is kept only as the fallback for a window above `kViewportCeiling` (2048 DIP). `settings.ini`
  `fitwindow=` keys are ignored; `Str.FitPageToWindow` stays in the positional string table but no
  UI reads it.
  Verified on x64 `.97`: `dtap` answers `unknown verb`; two back-to-back `tap 394 192` on NDA's
  `#zoom-out` logged two `[HIT] ... id=[zoom-out]` lines with `iw=1355` and a clean URL before and
  after (no page zoom, no `?node=A0001`); Settings shows no Fit switch; `Test-AppxPayload.ps1
  -ExpectedArch x64` reports `AUDIT PASSED` (35/35 binaries, 52/52 exports), and the installed
  `WebCoreDriver.dll` hash matches the staged one.
- **M4. Input matrix per orientation and mode** — the 0.50 zoom floor reads as "pinch is broken";
  the fitted viewport's pinch, rotation and inner-scroller cases need one test matrix (verb
  batches + the finger path) instead of spot fixes. The device probes `probe-s7.txt`,
  `probe-landscape.txt` and `probe-screensize.txt` still describe the retired `fitwindow=0` line and
  are marked RETIRED at the top; revise their expectations before the next phone batch.
  *First concrete cases, maintainer report 2026-10-26, x64 bench:* (a) **mouse wheel vertical
  scroll does not work**; (b) **horizontal pan after a press-and-sweep on a touch screen does not
  scroll**. The bench is a Surface Pro 5 WITH a touch screen, so both cases are drivable by a real
  finger/wheel on x64 (synthetic input cannot reach UWP), no Lumia needed for development.
  *Triage and result, `0.2.3.87` (2026-10-27):*
  (a) ROOT CAUSE OF THE WHEEL — `ContentArea` subscribed only to manipulation/tap events:
  `PointerWheelChanged` was wired to nobody, so a notch reached the void with no trace anywhere.
  Fixed: `OnContentWheel` converts one notch (±120) to 75 DIP through the same fitScale mapping
  the finger uses, maps the CONTENT CENTER as the inner-scroller point (a wheel event carries no
  position), and feeds `FreeScrollBy` — same accumulator, engine post, `scroll` log line and blit
  as a drag; Shift+wheel is horizontal via `CoreWindow.GetKeyState(VirtualKey.Shift)` (the
  `KeyModifiers` enum is not spelled `CoreVirtualKeyKeys` in the 19041 contract winmds — measured,
  it appears nowhere in the 88 refs; and UWP's handler type is `PointerRoutedEventArgs`, not
  `PointerWheelRoutedEventArgs`, which cost two failed builds to learn). Verified on the bench by
  the maintainer's own hand: 75 `wheel d=±120 sh=0 -> dy=±75` lines in autocmd-log, both
  directions, verdict «всё отлично» — before this build the wheel produced zero lines. The Shift
  branch is NOT yet exercised (the bench mouse has no tilt wheel).
  (b) HORIZONTAL TOUCH: still partially open. On `hscroll.html` (inner div, real overflow)
  horizontal finger swipes now log `scroll dx=72..174 dy=-4..` and the engine answers
  `[SCT] out=1|DIV` — the inner scroller moves. BUT nothing functional was changed for this path,
  and the new `manip start` marker (installed for triage) shows the gesture stack always worked
  here, so the ORIGINAL complaint most likely involved either a page with no horizontal RANGE
  (frame clamps and rc=0 looks like "no scroll") or a context outside this repro; the frame-level
  horizontal case has not been reproduced. Next probe: a page with a document-level horizontal
  overflow (body wider than the viewport) to separate range-0 from a map failure.
  *M4(b) frame-level probe, 0.2.3.92 (2026-10-27):* `hwide.html` gives the document a real
  horizontal body (`contents=5000x751`) and the engine reports a real range:
  `[SCT] frameby d=300,0 cur=0,0 -> 300,0 range=0,0..3645,0`; three `scroll 300 0` commands moved
  the frame 0 -> 300 -> 600 -> 900. The range-0 hypothesis is therefore NOT reproducible at the
  engine layer. The actual touch path on the Surface's screen still needs the maintainer's finger
  (synthetic input cannot reach UWP). Evidence: `Src/tools/shots/m4b-hwide-92.png`.
  *M4(c) hardware keyboard, `0.2.3.95` (2026-10-27):* maintainer report — an in-page field could
  only be typed into after summoning the on-screen keyboard, so physical keys had no route into the
  page. The route was structurally absent, not broken: the only text path was
  `ImeBox.TextChanged -> SendKeyToEngine`, and ImeBox is focused only by `OpenKeyboard()`, which runs
  only when a tap reported an editable. Added a CoreWindow `CharacterReceived` + `KeyDown` hook:
  printable characters go to `WebCoreTypeText`, Backspace/Enter to `WebCoreKeyAction`, both gated on
  `WebCoreFocusedEditable`; ImeBox and XAML text controls keep priority so nothing is typed twice;
  Ctrl+L / F5 / Ctrl+R / Alt+Left are wired as window shortcuts. Measured on the x64 bench:
  `hwkeys: hooked CharacterReceived+KeyDown`; a physical Cyrillic key logged
  `hwkeys: first char U+0442` and reached the page; `js` read back the inserted Latin text.
  Maintainer verdict: «hardware keyboard ожил». `ime: open editable=1 focus=Keyboard` also shows
  that ImeBox focus itself was never the broken half. The test page is
  `Src/tools/l1-popup-harness/keytest.html` (input/textarea/contenteditable, keydown counter).
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
    *M5.1 result, `0.2.3.95` (2026-10-27):* the whole-process blind spot is CLOSED on x64.
    `K32GetProcessMemoryInfo` resolves from the already-mapped kernel32 and works inside the
    AppContainer; `SL: pmem at entry / after teardown / after build` now carries
    `wsB / peakWsB / privateB / pagefileB / faults` in the same log stream as `SL: mem` and
    `SL: docs`, at each sampling point. Measured across the boot page plus three dzen navigations:
    `after build` WS 191.9 -> 242.1 -> 412.0 -> 468.5 MB while private went
    404.8 -> 483.6 -> 646.4 -> 689.6 MB. The app-side `mem.txt` at 10:45:56 read 656.3 MB against
    pmem's 689.6 MB — same order, same direction, and the difference is the ledger's later poll
    plus allocator drift. The earlier "no process reading in the AppContainer" conclusion therefore
    holds for `memoryFootprint()`/`QueryWorkingSet`, NOT for K32GetProcessMemoryInfo. `decodedB=0`
    and the `evictResources()` lever remain open; this closes only the whole-process gap.
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
  **Recon 2026-10-26 (code, no fix yet):** the swallow point is now named —
  `JSEventListener::handleEvent`'s exception path: the WORKER branch reacts to
  `vm.isTerminationException` with `scriptController->forbidExecution()` (JSEventListener.cpp:247),
  while the **page/main branch just does `reportException(...); return`** — the TerminationException
  is consumed as an ordinary uncaught exception and `handleEvent` returns normally, so the C++ cycle
  beneath it continues. Supporting mechanics, all measured or read in tree: (1)
  `m_hasTerminationRequest` is cleared by exactly ONE site, `VM::executeEntryScopeServicesOnExit`
  (VM.cpp:1766), which for the §10c spin never runs — the nested `EventLoop::run` never leaves the
  outer entry scope; (2) `Watchdog::shouldTerminate` returns `true` WITHOUT arming a next timer, and
  the next arming would come from `enteredVM` or the live tick's `apoArmJsWatchdog` — both dead
  while the tick is stuck, which is why the measured trace has ONE `js watchdog fired` line and 18
  minutes of silence at `tickstep=1`; (3) `vm.forbidExecutionOnTermination()` sets a VM-global
  `executionForbidden` that by upstream's own comment is kept worker-only "until we're sure clients
  using the JSC watchdog aren't broken" — calling it from the main path would kill ALL future JS in
  this process unless a driver-level reset path exists. Fix directions to choose between: **F1**
  mirror the worker branch (re-throw termination through the dispatch chain so the spin unwinds to
  `RunLoop::cycle` and the tick returns) — surgical but needs strict propagation proofs through
  `fireEventListeners`/`dispatchEvent`; **F3** re-arm a follow-up timer from inside the watchdog
  callback so re-enters get re-detected (cheap, diagnostic value only); **F4** VM-global forbid +
  a port API to clear it at session teardown (true kill, needs JSC-side edit). Repro notes: plain
  `while(true)` IS already killed; the cheap microtask replay did NOT freeze; dzen's cycle is
  gesture-armed — see DZEN-SCROLL-DEATH.md §10c–§10e.
  *M6 v1 implemented in `0.2.3.88` (2026-10-27), driver-only — no WebCore/JSC source edit:*
  (1) the watchdog callback now COUNTS every fire (`g_runawayFires`, marker
  `js watchdog fired #N topCallFrame=...`) and RE-ARMS itself via `apoArmJsWatchdog()` — a
  closed re-arm gap JSC leaves open: `shouldTerminate` returns without restarting the timer,
  `enteredVM` re-arms only on a VM exit the dzen spin never makes, and no live tick can re-arm
  while the engine thread owns the wedge (that is the measured "ONE fired line, 18 minutes of
  silence" of §10c). On a healthy box the per-tick apoArm renewal keeps the stale queued check
  short-circuiting on a pushed deadline — harmless; during a wedge, where no tick flows, the
  re-arm chain re-detects every 15 s. (2) Two new C ABI getters, exports #50
  `WebCoreGetRunawayFires` and #51 `WebCoreGetM6Heartbeat`; the heartbeat bumps on every
  `WebCoreLiveTick` CALL, so it FREEZES exactly when the engine thread wedges. (3) The shell's
  always-alive 250 ms PollAutoCmd timer runs `M6Check()`: a SECOND fire across a frozen
  heartbeat = the dzen shape -> `m6: runaway stuck ... exiting app` + `Application.Current.Exit()`
  — M6's kill: orderly, marker-flushed, never a zombie window. A recovered `while(true)` moves
  the heartbeat between fires and structurally cannot reach 2.
  **Measured, negative path, x64 `0.2.3.88` (2026-10-27):** `m6-neg.html` timed `while(true)` →
  `js watchdog fired #1 topCallFrame=000000DC53BFE5B0` → the page's recovery timer beacons
  `GET /beacon?fn=m6neg&r=recovered` in the SAME SECOND (termination stuck, block unwound) →
  shell logs `m6: watchdog fire #1 hb=78 stallFires=0` → app alive, no exit. The
  `chain disarmed` branch stayed silent because healthy per-tick re-arms outrun the stale check —
  expected, not a miss. **The positive wedge path (exit at ≥45 s) is code-complete and
  mechanically traced but NOT yet reproduced** — the stuck spin still needs a real dzen gesture
  session; M6's kill is currently negative-tested only, and that is the honest status.
- **M7. Performance baseline on the device (speed is its own axis, not a side effect of memory).**
  MVP explicitly scoped performance tuning out; 1.0 does not (Phase 0 criterion 6). Measure on
  BOTH devices with the instruments that already exist (`engineact`, `finished`, `[SCT]` in
  gpu-steps.txt, the heartbeat): startup → first paint, a typical page load end-to-end, scroll
  cadence on a tall page. Record the numbers here, then take
  the three most expensive findings into the queue as follow-ups.

  *Recon + FIRST NUMBER, x64 bench `0.2.3.88` (2026-10-27):* the instrument name-check matters
  more than expected — `engineact`, `finished` and the heartbeat producers live ONLY in the C++
  harness (`Src/harness/MainPage.xaml.cpp`); the C# app has none of them, so M7's app-side baseline
  needs two small additions: (a) a first-paint stamp (`boot.txt` times ctor→xaml-ok but nothing
  ever stamps the first blit), (b) a load-duration marker in ms (gpu-steps markers carry NO
  timestamps and batch deltas are dominated by planned `wait`s, which measure sleep and not the
  page). READY TODAY: `scrolln <dx> <dy> <n>` times each engine step individually, and
  `mem.txt`/`boot.txt` are timestamped health streams. **Measurement #1 — scroll cadence**,
  software present, `hscroll.html` 1080×3019, two runs of n=40 (down dy=30, up dy=-30),
  80 steps: **mean 4.4 ms, median 4 ms, p90 6 ms, min 3 ms, max 18 ms** — the max is the first
  three steps after `goto` (cold path: layout+paint+link rebuild), then the cadence settles at
  3–5 ms/step, i.e. a finger at 60 Hz asks for 16.7 ms/step and the engine answers ~4× faster
  on this bench. Next: implement (a)+(b), take startup→first-paint and load end-to-end, then the
  device half (Phase 0 criterion 6 wants Lumia numbers too — deferred to a device session).

  *Both instruments shipped in `0.2.3.89` (2026-10-27) and paid out immediately:*
  (a) `startup: first blit N ms after mainpage-ctor` — stamped at the top of `BlitToBitmap`,
  before the GPU early-return so a gpu build gets its own answer; (b) `timing: SessionLoad
  t=N ms rc=? url=...` — a steady_clock around `WebCoreSessionLoadImpl` in the wrapper
  (APO_SEH_RETURN_INT had to be re-expanded by hand, the macro's `return` would preempt the
  log). **Measurement #2 — startup→first paint:** **2989 ms** mainpage-ctor → first pixels
  (bench, .NET Native Debug bundle, software present). **Measurement #3 — document load
  end-to-end** (the driver's own duration, not the batch's waits): local harness page
  `hscroll.html` **1254 ms**; `example.com` **1297–2482 ms** (cold vs warm sample);
  `lenta.ru` **6234 ms** as a heavy real-world document — note the diag right after it still
  reads `rs=I pending=100`: the 6.2 s buys the main document and its first paint, and
  subresources keep arriving for tens of seconds more (that distinction is exactly why the
  instrument exists).

  Decision to make from those measurements, not by default: the GPU
  present question ([GPU-LIVENESS.md](GPU-LIVENESS.md) is still on pause) — software present on a
  Snapdragon 400 may cost proportionally far more than on the bench, and "software is the MVP-safe
  path" was an MVP verdict, not a performance one.

  **Measured on the Lumia 640 (2026-10-27): the automatic path is a hard crash there.** With the
  default `gpudefault=1`, the first real page load called `WebCoreGpuInit`; `gpu-steps.txt` ends
  at `[GPU] sharedDisplay`, `gpuinit.txt` was never written and `gpu-crash.flag` held `1`. The
  next launch consumed the flag, saved `gpudefault=0`, and the same nine-page set then passed.
  A 1.0 install on a 640 must not need that first crash to learn the lesson.

  **Fixed in `0.2.3.100` (2026-10-29): ARM starts with the GPU path off.** `_gpuDefault` is now
  `GpuDefaultForThisDevice()` — false when `Package.Current.Id.Architecture == Arm`, true on the
  x64 bench. A `settings.ini` `gpudefault` key or the Settings toggle still overrides either
  architecture, and the existing crash-loop flag is untouched. Measured on the 640 by
  uninstalling `.99` (fresh LocalState) and installing `.100`: the popup batch completed,
  `settings.ini` ended `gpudefault=0`, `gpu-steps.txt` has zero `[GPU] enter` and `sharedDisplay`
  lines, and neither `gpuinit.txt` nor `gpu-crash.flag` exists. The 950 (updated in place,
  `gpudefault` already 0) shows the same negative marker set.

## Phase L — long term (months each)

- **L1. Frames and popups** — **DONE on x64 and both phones.** iframes/frames landed on x64 in
  `0.2.3.80` (2026-10-25), on the Lumia 950 in `0.2.3.85` and on the Lumia 640 in `0.2.3.99`;
  popups landed on x64 in `0.2.3.86` (single-window policy) and on both phones in `0.2.3.100`
  (2026-10-29), including Back-from-a-popup. The 640 pass, the ARM popup build and the popup/Back
  run that this intro used to list as open are closed. Three-consecutive-run stability belongs to
  Phase 0 criterion 1, not L1. `LoadingFrameLoaderClient::createFrame` now mirrors WebKitLegacy's
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
  and `lenta.ru` are unchanged.
  *Phone acceptance, Lumia 950 (`.85`, 2026-10-25):* batch `probe-l1-arm-85.txt` — 60 s home
  pause, 10 harness pages, dzen.ru (~250 finger scrolls all `rc=0`), final
  `url=https://example.com/ ... rs=C`, normal suspend ~9 min in, no crash markers. Harness
  titles/markers match the bench: `L1 iframes=6 same=SAME-ORIGIN PARENT ACCESS OK srcdoc=SRCDOC OK`,
  three sandboxed frames each logging the real `allow-scripts` JS error, `about:srcdoc` with no
  request, frameset `name=left/right` both committed, `clicklink` navigating the existing
  `name=slot` (no new frame), `REMOVED OK iframes=0`, `DYNAMIC OK iframes=1`,
  `RESIZE OK actual=600x220`. Evidence: `Src/tools/device-logs/20261025-2201-l1-arm-85/`,
  Summary 2026-10-25 section. **Popups DONE on x64 in `0.2.3.86` (2026-10-26), single-window policy
  (попапы открываются в текущем окне — вкладок в v1 нет).** The hole had TWO doors: `window.open`
  enters through `PortChromeClient::createWindow` (the one the roadmap named) and `target=_blank`
  links/forms through `LoadingFrameLoaderClient::dispatchDecidePolicyForNewWindowAction`, which
  was a bare `PolicyAction::Ignore` — both were silent. Both now queue the URL in the driver slot
  + write a `POPUP:` marker in gpu-steps; the shell's live tick takes it once via the new C ABI
  `WebCoreTakePopupUrl` and opens it through the normal `NavigateTo(pushHistory:true)`.
  Measured on the bench (`Src/tools/l1-popup-harness/`, batches 1–2): `_blank` click →
  `POPUP: new-window-policy` → `popup queued` (+1.35 s) → diag
  `url=...src=blank title=POPUP TARGET OK` + server 200; gesture `window.open` →
  `POPUP: window.open` → `popup queued: ...src=open` → target page, beacon `r=null` (the null
  return is by design). Timer-based `window.open` is blocked earlier by WebKit's own
  `allowPopUp` gesture gate (Chrome-like; left untouched, logs nothing). Regressions (batches
  3–5): named `target=slot` bypasses the queue and the frame's script still runs; plain `_self`
  links navigate normally with `url follows engine`; an unknown target is queued — correct
  single-window interpretation. **Cross-origin popup verified too (batch 6):** a `_blank` from :8765
  queued `http://127.0.0.1:8766/cross-target.html` and the final diag read `url=...:8766/...
  title=CROSS TARGET OK` with the 8766 server logging the GET.
  **Light ARM acceptance on both phones (`.98`/`.99`, 2026-10-27):** the pages the `.85` batch
  used had never been tracked and were missing from the working tree, so they were reconstructed
  (now in `Src/tools/l1-popup-harness/`: `index.html`, `same.html`, `cross.html`, `nested.html`,
  `nested-inner.html`, `scroll.html`, `sandbox-frame.html`, `sandbox-direct.html`,
  `srcdoc-direct.html`, `frameset.html`, `named.html`, `remove.html`, `reload.html`,
  `dynamic.html`, `resize.html`, `frame-left.html`, `frame-right.html`) and validated first on
  x64 `.97`: all nine titles matched the `.85` log verbatim. Lumia 950 `.98`: all nine titles
  plus `example.com`
  (`title=Example Domain pending=0 rs=C`); server log showed exactly one
  `/sandbox-script-ran.png` (the `allow-scripts` frame), no `/cross-origin-access-ran.png`, and
  `/cross.html` on :8766 for the root and reload frames. Lumia 640 `.99`: same nine titles and
  `example.com`, same marker pattern. Evidence:
  `Src/tools/device-logs/20261027-1935-l1-light-98-950/` and
  `Src/tools/device-logs/20261027-2030-l1-light-99-640/`.
  **The 640's `.98` run found a first-load crash, not a frame defect:** with the default
  `gpudefault=1`, the first real page load auto-called `WebCoreGpuInit`; `gpu-steps.txt` ends at
  `[GPU] sharedDisplay`, `gpuinit.txt` was never written and `gpu-crash.flag` held `1`. The next
  launch consumed the flag, saved `gpudefault=0`, and the `.99` run completed all nine pages
  (no code change between .98 and .99 beyond the embedded batch). See M1 for the release fix.
  **ARM popups and Back-from-a-popup (`0.2.3.100`, 2026-10-29).** The batch
  `Src/tools/device-accept/probe-popup-arm-100.txt` ran three funnels on both phones, each in the
  current window: `_blank` → `POPUP: new-window-policy` → `popup queued` → diag
  `url=...popup-target.html?src=blank title=POPUP TARGET OK`; gesture `window.open` → diag
  `...?src=open title=POPUP TARGET OK` with the beacon `GET /beacon?fn=window.open&r=null`; and the
  cross-origin popup → `...:8766/cross-target.html?src=blank title=CROSS TARGET OK`. The server log
  for the run holds two of each: 2 `src=blank`, 2 `src=open`, 2 beacons `r=null`, 2
  `cross-target.html` on :8766 — and 4 fetches of each opener page (initial + the reload done by
  Back). `popup-cross.html` used to hardcode `127.0.0.1:8766`, which on a phone is the phone
  itself; it now derives the host from `location.hostname` and keeps the loopback href for the
  bench. The cross request arrived as `host=192.168.1.34:8766`, which is the proof the derivation
  worked.
  **Back-from-a-popup works, but it is a host-stack reload, not engine history.** The top-level
  `back` verb routes through `AutoUi` to the same `NavigateHistory(-1)` as the toolbar button.
  Measured after every popup: `hist engine back=0 fwd=0 cached=0/0`, then
  `history back rc=-99 moved=0 can=0/0`, then `hist host idx` one step down (2/2 → 1/2) and the
  opener title back on screen (`BLANK LINK PAGE` / `JOPEN pending` / `CROSS POPUP PAGE`). The
  engine has no entry because the popup is opened by the shell's `NavigateTo`, which builds a
  fresh session; the host stack then navigates the URL, so the page returns but scroll and form
  state do not. A true engine-history Back across a popup would need the queue to navigate the
  current session instead. Evidence:
  `Src/tools/device-logs/20261029-0631-popup-100-640/` and
  `Src/tools/device-logs/20261029-0631-popup-100-950/`.
  **Remaining limits:** (1) one run per phone is one run — no three-consecutive-runs, no memory
  figure; (2) the `.100` clean release bundle and its sideload ZIP exist (M1 has the hashes), but
  `.97` remains the published ARM release until that ZIP is uploaded; the embedded-batch `.98`,
  `.99` and `.100` acceptance bundles must not be uploaded; (3) the host-reload caveat above for
  Back-from-a-popup.
  Evidence: `Doc/Archive/Summary.md` 2026-10-26 popup section and the 2026-10-27/2026-10-29 L1
  sections.
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
- **Video through a port-registered media engine.** Not scheduled, and not a 1.0 item. The current
  build turns video off (`ENABLE_VIDEO=OFF`, `ENABLE_MEDIA_SOURCE=OFF`, pages call
  `setMediaEnabled(false)`), but WebKit 2.52.4 already exports
  `MediaPlayerFactorySupport::callRegisterMediaEngine`, so a future port can own decode/present
  behind `MediaPlayerPrivateInterface` without touching WebCore's media element. MetroSurf's
  public tree shows the same seam (`g_wp81RegisterMediaEngines`) and confirms the WP8.1 Media
  Object split, but its actual player lives in the unpublished shell. The useful trigger is a
  core site that needs video, not "video is missing"; the ladder (progressive MP4 first, MSE last)
  and the licensing caveat are in `Doc/METROSURF-ANALYSIS.md` section 8.6. WP8.1 itself is out of
  scope: the fleet is W10M-class, so MetroSurf's 512 MB findings are evidence about the GPU path,
  not a hardware target. Revisit triggers are listed in section 9.1 of the analysis; until one
  fires, this stays reference material rather than a backlog item.
  *Bench observation, `0.2.3.95` (2026-10-27):* the RuTube video page
  `https://rutube.ru/video/c28c0ef4b8ae097bd6fdde9f6250a268/` loads with the correct title
  («Мадагаскар 3 …»), so fetch and redirect handling are fine; the page's own player then shows
  «Что-то пошло не так. На сервере что-то сломалось». That is the expected shape of
  `ENABLE_VIDEO=OFF`: the document renders, the media element cannot. Also worth writing down,
  because it cost a session: `www.rutube.com` is not the site — it 302s to the parked
  `ww38.rutube.com`; the real host is `rutube.ru`. This is a page-side failure mode, not a new
  port defect, and by itself it does not fire the "a core site needs video" trigger.

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
