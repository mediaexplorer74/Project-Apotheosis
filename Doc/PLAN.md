# Project Apotheosis — Development Plan

> **This is the current-state view.** It holds the live status, the v1.0 definition, and the open
> work. Every closed item, dated snapshot, investigation narrative and July-Aug phase log was moved
> verbatim to [Archive/PLAN-HISTORY.md](Archive/PLAN-HISTORY.md) — nothing was deleted. Old cross-references of the
> form `PLAN.md §...` resolve through the index below; each names where the section now lives.
> Last reorganised 2026-09-26 (phase RnD: C#/UWP host, x64 + ARM build lines).

---

## 0. 2026-10-03 (second pass) — the post-MVP docket

Section 0a closed the two MVP asks and left five items open. **This section is the plan for them** —
it is the first place the post-MVP work is written down, which is what the maintainer asked for. The
order below is the maintainer's order, and the ARM line (item 7) starts only after item 6.

**Status, evening of the same day (`0.2.3.9`).** Items 1–5 are implemented and measured, and two
claims further down this section are superseded. The "measurement that decides items 4 and 5" below
was taken while the viewport was still the fixed `720x1080`, and item 4 has since removed that
constant; on a fitted 1355-DIP bench window the two modes are now visibly different, so that
measurement no longer describes item 5. Both suspicions about Nokia Design Archive are closed with
reasons. The item-4 zoom cap settled at `3.5`, not `2.2`, and the re-arm after a navigation needed a
fix this plan did not anticipate — both explained under item 4. Evidence:
[Archive/Summary.md](Archive/Summary.md), "Status update — Oct 3, 2026 (evening)".

### The measurement that decides items 4 and 5

> **Superseded by item 4.** This measurement was taken while the viewport was still the fixed
> `720x1080`; item 4 removed that constant, and the conclusion below ("the two modes are visually the
> same") no longer holds — on a fitted 1355-DIP window the mobile mode now serves a real `412` CSS px
> phone layout. It is kept because the *reason* it reached that conclusion is still worth knowing: the
> site is viewport-driven, so the UA alone changes nothing.

Nokia Design Archive was loaded twice from a fresh process into the same 1368-DIP window, once with
the mobile UA (`ua=0`) and once with the desktop UA (`ua=1`):

    contents=720x1080 body=1 nonwhite=774952/777600      (both runs, identical)
    js=1/1 sheets=1 scripts=17  res: 24 entries, the same list in both

**The desktop UA changed nothing** — the same `diag`, the same resource list, and two screenshots
that are visually the same. The site is **viewport-driven, not UA-driven**: what selects the layout it
serves is the width of the CSS layout viewport, and that width is `720` because `kW` is `720`. The
"huge white fields left and right" are the rest of the window the 720-DIP page never covers. The UA
path itself is wired correctly (`LoadingFrameLoaderClient.cpp:600-614`; a custom UA wins, else iPhone
Safari 16.4 or Chrome/Edge 120 on Windows) — it is simply not a lever this site reads.

That also means the missing **"Network" view is not a UA question** and is not fixed by the scaling
work. It stays a separate hypothesis (item 5b) with its own probe.

### 1. The script-made screenshots move into `Images/`

`Src/tools/run-smoke.ps1` already writes one PNG per site; they land in
`%USERPROFILE%\Documents\apotheosis-smoke`, which is outside the repository and is wiped by any
cleanup. The thirteen site captures plus a `smoke.csv` copy become a checked-in record under
`Images/smoke-<version>/`, with a short `README` naming the build and the date. This is documentation,
not test output: the point is that a future session can see what "13/13 PASS" looked like.

### 2. A Settings switch for the touch-scroll FAB

`ScrollFab` (the `▲`/`▼` pair in `MainPage.xaml`) is a touch-emulation aid with **no way to turn it
off**, and both target platforms have a real touchscreen, so on those it is pure clutter over the
page. Add a General-tab toggle bound to a new `Str` key, persisted as `touchfab=0|1` in
`settings.ini`, and AND it into the existing visibility gate that `_sessionActive` already drives.
Default on for x64, off for a device with a touchscreen is *not* proposed — the platform cannot tell
the two apart reliably, so the default is on everywhere and the switch is the answer.

### 3. Desktop and Mobile are separate test cases

The smoke list runs one mode; the user has to change `ua` in `settings.ini` by hand to see the other.
Two additions, both on the same side of the ABI:

- a new `autocmd` verb `ua mobile|desktop` that drives the **real control** the way `lang` drives the
  language picker, so the test exercises the shipping path and not a back door;
- `run-smoke.ps1 -Mode mobile|desktop`, which writes the mode into `smoke.csv` before the first `goto`
  and records both modes as two files. The acceptance table then has a mode axis instead of a footnote.

### 4. The engine viewport becomes the window (the scaling decision)

The fixed `const int kW = 720, kH = 1080` is the root of item 5's white fields and is now scheduled
rather than deferred. The design uses only exports that already exist:

| mode | presentation size | `WebCoreSetPageZoom` | result |
|---|---|---|---|
| **desktop** | content area, device px | `1.0` | the desktop layout fills the content area exactly — item 5's "чётко вписываться" |
| **mobile** | content area, device px | `clamp(areaW / 412, 1.0, kMobileMaxZoom)` | a phone-width layout *re-laid out and painted at the window's own resolution* (crisp), stretched to fill |

`WebCoreSessionResize(w, h, outRGBA)` already re-sizes the session and keeps the compositor's idea of
the surface in step, and `WebCoreSetPageZoom` re-lays out, so the layout viewport becomes
`viewport / zoom` CSS px. Two constraints are already measured and must be respected:

- **zoom is reset by a navigation** ([GPU-LIVENESS.md](GPU-LIVENESS.md), `desktopZoom`) — and the
  mechanism is now named exactly: `WebCoreSessionLoad` calls `teardownSession()` unconditionally
  (`WebCoreDriver.cpp:4432`) and `teardownSession` ends with `g_pageZoom = 1.0f` (`:1832`). Re-arming
  after every `after-load` is therefore **not sufficient on its own**: the shell's `_pageZoom` cache
  survives the navigation, so `ApplyViewport`'s no-op test compares two equal numbers and answers
  "nothing to do", and the re-arm never runs. Measured in `0.2.3.8`: `ua mobile` logged
  `fitwindow … z=3,29`, the following `goto` presented an unzoomed frame, and no further `fitwindow:`
  line was written for the rest of the session. Fixed in `0.2.3.9` by invalidating the cache in the
  navigation's completion, guarded on the **wanted** zoom so desktop spends no repaint;
- on the GPU path **zoom must equal `CompositionScale`** — the surface is the panel's physical pixels
  while the viewport is DIPs, and `deviceScaleFactor` cannot substitute.

**The cap settled at `3.5`, not the `2.2` this plan first proposed** (`kMobileMaxZoom`). The target is
not "a bounded tablet variant": `412` is a *phone* CSS width and the point of the mobile mode is to
serve a phone layout, so the zoom should reach it whenever the window allows. On this bench (1355 DIP
of content area) the cap is not even touched — `1355 / 3.5 = 387`, so the layout is a true `412` CSS px
and the page fills the area edge to edge, which is the "незаметно". Only a window wider than
`412 × 3.5 = 1442` DIP hits the cap, and from there the layout starts to widen again and margins
appear — the "совсем уж широкая морда" the maintainer allowed but does not welcome. Because this
changes the present path that currently works on ARM, it lands behind a setting, default off, and is
proven on x64 before the ARM line picks it up.

> **Superseded 2026-10-03 (`0.2.3.14`).** The `clamp(areaW / 412, …)` row of that table is gone, and
> with it the whole two-mode idea of the zoom: the UA no longer decides any geometry. Measured on the
> bench, the mobile row produced `1355 / 412 = 3.29` — 1 CSS px at 3.29 DIP, 6.58 physical, a 16 px
> label at 106 physical px, reported by the maintainer as "масштаб сайта конский". The magnification
> also bought nothing: `zoom = width / cssWidth` fills the window for *any* `cssWidth`, so paying 3.29x
> for a layout that is 412 CSS px wide is a choice, not a requirement. The zoom is now the Settings
> value and nothing else (50–200 %, `ViewportZoom()`), a pinch writes the same value, and **page scale
> is removed from the C# line entirely** — it magnified the finished layer tree without re-running
> layout, which is what a finger on dzen.ru reported as *"всплывашка живёт своей жизнью словно в другом
> слое"*. See [LESSONS.md](LESSONS.md), "Page zoom replaces page scale". The `zoom` verb replaces the
> need to hand-edit `settings.ini` to test any geometry.

### 5. Nokia Design Archive: two separate suspicions, two separate probes

- **5a. The white fields** — **CLOSED.** They are not reproducible in `0.2.3.9`: on the fitted
  1355-DIP bench window the desktop mode lays out at exactly the content area and the mobile mode fills
  it edge to edge, and the Nokia page shows no white on either side in either mode. They matched the
  blit-scratch artefact (a frame larger than the scratch buffer) fixed in `0.2.3.3`.
- **5b. The "Network" view that does not appear** — **CLOSED, and it was not the port.** The view is
  there: on desktop the graph renders with its node labels, the filter panel and the date histogram,
  and on mobile the page serves its phone layout. What covered it is the **usercentrics consent
  modal**, whose backend this bench cannot reach — `nslookup uct.service.usercentrics.eu` answers
  `0.0.0.0` from DNS.COMSS.ONE, and the engine logs `curlcode=6 Could not resolve hostname`. The modal
  is local markup, so it paints; it simply cannot complete its own handshake. A network-policy
  artefact of the stand, not a loader finding — the loader served every resource the page asked for
  (`res:` lists all 18, no `RH: fail`). The hypothesis this bullet proposed (a JS route the loader
  never serves) is **falsified**. Recorded gap, now closed: dismissing the modal by script needs a page
  tap, and the C# line *did* lack one when this was written — `tap`/`taplink`/`links` have since been
  added to the scripted channel and reach the same `HandleTapAt` a finger uses
  ([LESSONS.md](LESSONS.md), "Driving the C# app from a script"). What is still open is different and
  narrower: nobody has driven a tap onto *this* modal and confirmed it dismisses, because
  `uct.service.usercentrics.eu` cannot resolve on this bench, so the modal's own Accept handler may be
  dead — verify, do not assume.

### 6. GPU mode, re-checked

`gpu=1` is still off by default and still not proven on x64. The work is: run the smoke list once with
`gpudefault=1`, keep `alive` and the engine `nonwhite` as the verdict, and treat any process death as a
finding to name — with the known trap that most "crashes" in old logs were the wedge dumper's own
first-chance AVs, so a death is not a death until the exit code says so (`cdb` first, per
[this memory](../MEMORY.md) and §0a). The three GPU-hand-back guards documented for `.23` are the
first thing to re-read before blaming the engine.

### 7. Then, and only then, the ARM line

The C# app has never been deployed to the Lumia. Requirement 9 is the phase exit criterion, and the
device differs in the App Container, the ABI, the JIT and the font stack. Items 1-6 are all
x64-visible work, so the ARM line starts from a known-good x64 state.

**Opened as the priority on 2026-10-03.** Items 1-6 are done (screenshots into `Images/`, the
`touchfab` switch, desktop/mobile as separate cases, the scaling decision, Nokia Archive, GPU
re-checked), the maintainer is putting the device on charge, and item 7 is now the head of the queue.
The measured state of the ARM payload, the ordered docket and the named suspects are in
[BUILD-ARM32.md](BUILD-ARM32.md).

**Docket steps 1-2 are done (2026-10-03), and both were worth doing before touching the phone.**
Step 1: the staged ARM payload exported **37** symbols against **39** on x64, missing
`WebCoreSessionResize` and `WebCoreSetPageZoom` — the first device run would have thrown
`EntryPointNotFoundException` at startup. Relinked from current sources; the *packaged* DLL now
carries all 39. Step 2: the ARM appx had never been built and now builds in both configurations,
with every binary checked to be ARMNT and the resource PRI checked to carry its strings. The one
obstacle found is not in the engine: **a Debug UWP package depends on `.Debug` framework packages a
retail Lumia does not have**, so the device run must deploy **Release|ARM**, and the phone's
`Microsoft.VCLibs.140.00` must be at least `14.0.33519.0` or the install is refused before our code
runs. Both were found by inspecting artifacts, i.e. before spending a single minute of device time.

**The first C# device run has happened (2026-10-03 evening), and it ended in a death.** `0.2.3.23` was
built, passed `Test-AppxPayload.ps1` (30 binaries all arm, driver at the package root, `WebCore.dll`
absent, 40/40 declared symbols exported) and deployed (`ME.Apotheosis_0.2.3.23_arm__cj4nrm0qt9s1y`,
HTTP 202). On the phone the app **launches and paints dzen.ru** — once a blank white screen, once real
content — and then **a real-finger scroll kills the process within about two seconds**. So requirement
1 is demonstrated on the device and requirement 6 is refuted there, in the same run.

The death is localised. Its `vtbl` resolves to `??_7Timer@WebCore@@6B@`, so the process ends **inside
the callback of a one-shot `WebCore::Timer`** on the main thread, right after four tracker loads, with
MIME content-type parsing the last work to complete (see [BUILD-ARM32.md](BUILD-ARM32.md) for the
resolution table and the base-recovery method). It leaves no SEH line, no dump and no WER record.

It cannot be probed from the engine side: `ninja -n -C build-arm32-gpu WebCore` wants **7400 edges**,
and the single unified object carrying `ThreadTimers.cpp` still needs **6462**, because the generated
`DerivedSources` are stale and the PCH regenerates everything. So the instrument is **port-only** — a
Vectored Exception Handler plus `signal(SIGABRT)` and a cleared `_CALL_REPORTFAULT`, shipped in `.23`,
which reaches `thumbv7a` because a VEH is a runtime call rather than a compiler feature. The next
device run must scroll with a **real finger**: synthetic input cannot reach the UWP input stack, so a
scripted gesture cannot produce this death.

The net has since been exercised, and it changed the question. `.23`'s four engine sessions left
**three** death signatures — one is an access violation reading `0x8` inside
`Document::notificationClient()`, reached from a timer callback; the other two are `abort()`s at the
very end of a **teardown** — and the memory quota was excluded by measurement (peak 179.5 MB against
1536 MB). What that run also showed is the gap in the instrument: the trace has **no input
vocabulary at all** (`swipe`, `pinch`, `zoom`, `tap`, `gesture`, `scroll`, `input`, `touch` all
counted at zero), so it cannot tie a death to the gesture the maintainer made, and a teardown abort
cannot be told apart from the app being closed. `0.2.3.24` (deployed 2026-10-03 night) closes that
port-side, with `gesture:` bursts and a `teardown: enter/exit why=` bracket — the next real-finger run
is the one that decides between a gesture and a close. See [Archive/Summary.md](Archive/Summary.md)
for the grammar and [BUILD-ARM32.md](BUILD-ARM32.md) for how to read the pull.

**The `.24` run answered it, and the answer is "neither" (2026-10-03, after late night).** In the
maintainer's four tests — two of which ended in the frozen pinch and scroll they reported — the trace
carries `gesture:` bursts for `close` and the idle `live` tick **only**: no `scroll`, `click`, `zoom`
or `resize` anywhere. Both fatal sessions completed their teardown (`exit why=load dt=17ms` / `8ms`),
ran ~1,400 more lines of load work, and then died **idle**: an access violation reading `0x8` inside
`Document::notificationClient()`, in a one-shot `WebCore::Timer` callback — the same site as `.23`. The
frozen gesture is a consequence of a dead process, not its cause.

**The cause is named to the instruction and fixed port-side in `0.2.3.25`.** `Document::notificationClient()`
has **no null check**, and it resolves its `NotificationController` through a `Supplement<Page>` that
only the *embedder* installs (the only in-tree caller of `provideNotification()` is
`WebKitLegacy/mac/WebView/WebView.mm`). This port builds every Page with
`pageConfigurationWithEmptyClients()`, so the supplement is absent and the lookup faults at `+8`.
`.25` installs a `PortNotificationClient` (permissions denied) at all four `Page::create` sites the
driver has — a **relink, not a rebuild**, because `provideNotification` is already exported by
`WebCoreFull-*`. Built for x64 first (clean compile, `unresolved=0`, 40 exports), relinked for ARM
(40 exports), `AUDIT PASSED` on the bundle, and deployed to the Lumia **unlaunched**:
`ME.Apotheosis_0.2.3.25_arm__cj4nrm0qt9s1y`. **The cure is unverified — the maintainer's next run is
the test.** Two smaller results from the same pull: `.24` contains **no abort deaths at all** (the `.23`
teardown-abort class did not recur), and four tests produced three engine processes, so one launch never
reached engine init.

**The assert reader shipped twice and named nothing; `0.2.3.28` is the build that can (2026-10-09).**
`.26` shipped `ApoPortScanForAssert()`, which reads `WTFCrashWithInfo`'s four spilled arguments off the
stack — there is no symbol to take, because the helper is `inline` in `Assertions.h` and is emitted
into our own object. Both `.26` and `.27` were deployed and reproduced on the device, and the assert
came back **identical every time**: the same `+0x1B` inside the helper (`+0xECEB` / `+0xEEC5` /
`+0xEECD` across the three builds), so the death is deterministic and its class has not changed. But
neither build named it, and the wide window `.27` printed was 384 lines **of the same word**: the walk
was written as `void** p = (void**)&anchor;` with an out-of-bounds `p[i]`, which is undefined, so clang
folded all 384 reads into one load and hoisted it out of the loop. `llvm-objdump` shows the loop
loading `[sp,#0x20]` every iteration. The nets were dead code on both builds — the failure was never
the window size. `.28` reads from the caller's captured-frame array (a pointer parameter) through a
`void* volatile const*`, prints each slot's own address, and keeps both nets and the wide window.
It is built on both lines (ARM `Compiled 14/14`, `duplicate=142`, `Exports: 40`, `SHA256 BF549277…`;
x64 `Compiled 13/13`, `duplicate=0`, `Exports: 40`, `SHA256 77B3CA74…`), audited (`AUDIT PASSED`,
30 binaries all arm, 40/40 exports, and linked == staged == packaged = `bf54927727c36400…`), and
deployed (`ME.Apotheosis_0.2.3.28_arm__cj4nrm0qt9s1y`, `HTTP 202 in 509 s`). **The reader is still
unverified on the device** — the next reproduction is the proof, and it must name the assert or print a
window whose `slot` addresses differ line to line.

**The maintainer's `.28` reproduction answered both questions at once, and `0.2.3.29` ships the result
(2026-10-09).** Pulled as `Src/tools/device-logs/20261009-105721/port-trace.txt` (1,225,857 bytes). Two
independent facts came out of it:

* **The instrument had a second, fatal bug.** `ApoImageString()` computed the length of the string it
  validated but never copied the characters into `out` — it wrote only the terminator. So `file[]`/`fn[]`
  were uninitialised stack buffers, `ApoFileSuffixOk()` inspected garbage, and **both nets were dead code
  from the day they shipped**, which is why `.26`, `.27` and `.28` all reported "no argument triple".
  Found by disbelieving the source — a host simulation *did* match the window, so the compiled code had to
  differ — and confirmed in `llvm-objdump`, which shows only `strb r3,[r7,r2]` (the terminator) and then
  the `buf[n-4..n-1]` suffix read. Fix: `out[n] = c;`.
* **The assert, read off the window by hand.** The argument spill sits at window slots 105–108
  (`line=0x120=288`, file ptr, function ptr, `counter=0x13D=317`) — four words **above** slot 104, which
  holds the `frames[2]` return address `5BE3F2CD`, so the frame net's `a = i - 4` was the wrong offset.
  Resolved through the packaged DLL's PE (base `0x5BE30000`): `WebSocket.cpp:288`, function
  `ExceptionOr<void> WebCore::WebSocket::connect(const String &, const Vector<String> &)` — i.e.
  **`RELEASE_ASSERT(m_channel)`**. The channel is null because `pageConfigurationWithEmptyClients()`
  installs `EmptySocketProvider`, whose `createWebSocketChannel()` returns `nullptr`, and dzen.ru opens a
  WebSocket (Yandex Metrika, the last `loader: CALL` before the abort). **The same empty-client trap as
  the `.25` `NotificationController` supplement, one supplement over.**

`.29` fixes both: the reader copies its strings, and `Src/port/PortWebSocket.{h,cpp}` installs a
port-side `SocketProvider` whose channel answers `ConnectStatus::KO`, which `WebSocket::connect()` turns
into `failAsynchronously()` — an ordinary page-level error instead of a process abort. Relink on both
lines (ARM `Compiled 15/15`, `duplicate=142`, `Exports: 40`, `SHA256 14E73E2A…`; x64 `Compiled 14/14`,
`duplicate=0`, `Exports: 40`, `SHA256 B212351D…`), `AUDIT PASSED` with linked == staged == packaged =
`14e73e2aa6a84516…`, deployed `ME.Apotheosis_0.2.3.29_arm__cj4nrm0qt9s1y` (`HTTP 202 in 88 s`,
`Success`). **Unverified on the device:**
the expectation is now *no death at all* on dzen.ru, and if something else kills it the repaired reader
should name it.

#### Readiness, assessed 2026-10-03

| Line | Ready | Basis |
|---|---|---|
| The MVP contract (the ten requirements in [MVP.md](MVP.md)) | **~75 %** | 8 of 10 met on x64, each with a measurement. Open: requirement 5 (Back is not in the scripted channel) and requirement 9 — the same list on the Lumia, which *is* the phase exit criterion |
| The product, for a real user on a Lumia | **~35 %** | the app launches and paints on the device, then dies **idle, mid-load**, ~2 s after a completed teardown — the blocked scroll the maintainer reported is the symptom, not the trigger; the `.25` fix is **confirmed against its own site** — the access violation reading `0x8` did not recur — but a release assert replaced it, and `.28` is the build that names it; no stable memory price across navigations, hangs with no way out, subframes absent entirely |
| A general browser (what MVP excludes: GPU presentation, subframes, video/MSE) | **~15 %** | next phase by definition |

The first figure is close to arithmetic. The second is a judgement, and it is the one that matters:
**the project's remaining risk is concentrated in one place — the phone.** The five most mature
pieces: the managed-host contract (SEH wrappers turning a native fault into `rc=-17` instead of a
`FailFast(7)` death out of P/Invoke, plus the native `FONTCONFIG_FILE`), the ABI boundary, loading and
painting (13 sites, `nonwhite` each, Cyrillic, TLS, a reported failure), the scripted `autocmd`
channel as the acceptance instrument, and the single scale lever with the blit ring. The five rawest:
the ARM line above, memory across navigations, GPU presentation (which does not even engage on the
bench — `WebCoreGpuInit rc=-20`, `[GPU] paint path -> cairo`), hangs with no reliable kill-the-page
path (the JSC watchdog fires, the termination does not stick), and the functional holes with the
widest blast radius (no subframes, no auth flows, no video, `decodedB=0`, and the stale `ValueMap`
handle that the containment guards only cover three call sites of). The "49 MB `port-trace.txt` inside
the shipping appx" item that stood here is **withdrawn**: neither the ARM nor the x64 package contains
any log file, because `port-trace.txt` is a runtime `LocalState` artifact and was never packaged.

---

### Deferred and switched-off features — a register, so nothing is lost quietly

The maintainer asked for this on 2026-10-09: if a behaviour has to be turned off for a while to get a
measurement, it gets an entry here with what turns it back on, so it is restored in a later phase
instead of being rediscovered as a bug. **Nothing is switched off today** — both entries below are
decisions to *keep* behaviour, recorded because both were open questions this week.

| Feature | Status 2026-10-09 | Why it is the way it is | What restores / changes it |
|---|---|---|---|
| The saved page zoom is re-applied after **every** load in the fixed-viewport mode (`fitwindow=off`, the ARM configuration) | **ON — confirmed by the maintainer 2026-10-09** | `MainPage.xaml.cs` re-runs `CommitPageZoom(_defaultZoom / 100.0f, "reload")` when `!_fitWindow && _sessionActive && _defaultZoom != 100`. The maintainer chose to keep it: a fixed `zoom=50` on every load is a *known, repeatable* configuration, which is what a measurement needs, even though 50 % lays the page out at twice the window width. Under `fitwindow=on` the same zoom is re-derived by `AdoptViewport` instead, so the two modes disagree on purpose | Deleting that one call. Do not — it is now a decision, not a leftover |
| Pinch-to-zoom on the fixed-viewport line | **ON — kept, on watch** | The device report "питч не отрабатывает вообще" was traced to `_vpDirty` having no drain when `fitwindow` is off; `DrainPendingZoom()` (2026-10-09) is that drain and is unverified on the phone. If a device run still shows the pinch misbehaving, disable it rather than let it corrupt a measurement | **Disable:** gate `OnImageManipDelta` on a flag (the whole gesture enters through `_pinching` there, and the commit is `CommitPageZoom(_vpZoom * live, "pinch")` in `OnImageManipCompleted`), and record it in the MVP table so the requirement does not silently read as met. **Restore:** flip the flag back — the commit path (`PostZoomAndRepaint`) and the `.15` double-tap fit toggle are untouched by the disable, so the feature is not half-removed |

---

## 0a. 2026-10-03 — the C# MVP: four of the ten requirements close on x64, ARM still untouched

**The maintainer's two MVP asks are demonstrated on x64.** (1) Nokia Design Archive opens and paints;
(2) the settings switch — including the interface language — takes effect without a restart. The
thirteen-site acceptance list now runs end to end in one process.

Installed and measured: `ME.Apotheosis_0.2.1.4_x64`.

| | |
|---|---|
| Nokia Design Archive | `after-load url=https://nokiadesignarchive.aalto.fi/ rc=0 compositing=0`; `diag: contents=720x4431 body=1 nonwhite=766352/777600`; screen: hero, item image, Green "View on Aalto repository" `<a>`, correct Cyrillic |
| Language switch | `lang 1` → every settings caption and both pivot headers repaint; `RetranslateUi()` is one idempotent pass; OS-supplied `OnContent`/`OffContent` replaced by the table's `Вкл.`/`Откл.` |
| Thirteen-site list | **13/13 PASS** in one process, no restart, `alive=True` on every row — see [MVP.md](MVP.md) |
| Home page Cyrillic | `Частые сайты`, `Яндекс — быстрый поиск в интернете`, hero `·` — the same strings rendered `??????` before |

**Three UI defects, all three rooted in the UWP-correct route being the wrong route here:**

1. **Cyrillic died between C# and the engine** as literal `?` bytes (F1). Every string-taking
   `DllImport` was declared `CharSet.Ansi`; with UTF-8 bytes handed to an ANSI marshaller the
   non-ASCII text did not survive. Fixed by marshalling by hand: a private `…Raw(byte[])` `DllImport`
   plus a public `string` wrapper that encodes UTF-8 into a NUL-terminated `byte[]`. No `CharSet`, no
   `[MarshalAs(UnmanagedType.LPUTF8Str)]` — the attribute's behaviour under .NET Native is unverified
   and a blittable pinned array needs no verification. Both consumers (`history.tsv` tile titles and
   the home HTML) go through `WebCoreRenderHtml`, which is why one fix repaired both.
2. **Every settings label rendered empty** (F2). `x:Uid="SettingsTitle"` looks up
   `SettingsTitle.Text` in the `.resw`; the files define bare `SettingsTitle`, so every lookup missed.
   The page shipped with uncaptioned controls and nothing said so. Retired `x:Uid` for `x:Name`.
3. **"The language setting does not repaint the UI"** (F3) is a property of the platform, not a bug:
   `ResourceLoader.GetForCurrentView()` is bound to the **view language**, fixed at launch, so it
   answered English for every key regardless of the picker. The `.resw` is no longer consulted at all;
   the table in [Str.cs](../Src/Apotheosis/Str.cs) is authoritative and `Str.SetLang` is a live switch.
   The three `.resw` files stay in the package as the fork's translation corpus.

**A fourth defect was in the test channel, and it made twelve sites untestable.** The `goto` verb
refused to run when `_sessionActive` was false — but a visit to Home calls `WebCoreCloseSession` and
clears that flag, and only a navigation can bring a session back. The channel could not start itself:
a smoke run produced **fifteen batches of `goto ignored: no session` and not one request**. The guard
is gone (`goto` is exactly the thing that must stay reachable), and the channel is now verified to
survive a Home visit.

**Two instrument defects, because a measurement that lies is worse than no measurement:**

- The site verdict read `nonwhite` from a **screen capture**, and the app's dark chrome fills the
  window: all twelve sites scored an identical **99.68 %**. That is a question about the window
  answered by a number about the page. The verdict now uses the engine's own `diag: nonwhite=A/B`.
  Calibration from the checked-in `smoke.csv` ([Images/smoke-0.2.1.4/](../Images/smoke-0.2.1.4/)), and
  the spread is what makes it usable: example.com 100 % (its body background is `#f0f0f2`, so almost
  no pixel *is* white), ycombinator 99.99 %, nokia-archive 99.64 %, wikipedia-m 81.68 %,
  reddit 75.91 %, w3.org 43.23 %, ya.ru 34.18 %, lenta.ru 34.03 %, dzen.ru 31.59 %, rbc.ru 7.85 %,
  habr 4.04 %, info.cern.ch 1.86 %.
- The run slept a fixed interval per site, so on a heavy site it read the **previous** site's result:
  in an **earlier** run reddit came back with an empty `rc`, and dzen and bad-domain both reported the
  identical `31.59`. Two sites cannot paint the same page. The loop now waits for the site's own URL
  to appear in an `after-load` line.

> **Correction, second pass (2026-10-03).** This section was first written mixing two runs' numbers:
> the `dzen.ru 0.00 %` calibration figure above belonged to a run *before* the fixed-sleep fix, where
> dzen was the one site that reported `rc=0` and painted nothing — the same failure the ARM line had
> seen, and the reason requirement 8 exists. In the **final** run, the one whose `smoke.csv` is
> checked in, dzen paints **31.59 %** and its screenshot shows a full feed with images. The earlier
> `0.00` reading still stands for the run it came from; it is not the current state. **The artifact is
> the authority, not the prose.**

**Open, and deliberately not fixed now:**

- **The engine viewport is a fixed `const int kW = 720, kH = 1080`** shared by the session, the GPU
  surface and the `WriteableBitmap`, so on a large window the page occupies the left part and the rest
  is white. **Superseded the same day: this is now scheduled work, not a deferral — see §0 item 4.**
  A second measurement closed the UA explanation for it: the desktop UA renders this page byte for
  byte identically to the mobile UA, so the white fields are the fixed viewport, not the user agent.
- **On the failure path the stage `diag` describes the previous document**, because the loader writes
  its stage block — `rc`, then `diag` — *before* it renders the error HTML. The failed row therefore
  reports `n/a` rather than a number that belongs to the page before it.
- Back navigation (requirement 5) and history/suggestions (7) are still unverified; the smoke list
  does not drive them.

**Install note, for the next iteration:** `Add-AppxPackage` on the `0.2.1.3 → 0.2.1.4` update failed
twice with `0x80073CF9`/`0x80004005` — `Failure in the SystemRegisterRemove state handler`, then
`failed to register the windows.MRT extension: The data is invalid` — while the deployment **rolled
back cleanly** to the old version. The working route is back up `LocalState`, `Remove-AppxPackage`,
`Add-AppxPackage`, restore `LocalState`; `Remove-AppxPackage` alone deletes `LocalState`, so the
backup is not optional. New tooling: `_build-csharp-x64.bat` (the C# bundle — distinct from
`_build-appx-x64.bat`, which builds the C++ harness), `install-apotheosis-x64.ps1`,
`drive-csharp.ps1 -FitWindow` (DPI-aware capture; without `SetProcessDPIAware` the shot is a crop).

---

## 0b. 2026-09-26 evening — the first page renders, and the next blocker is named

**The phase goal is met: a real page is drawn.** Until today the x64 app died on a null call inside
`Frame::init()` (`WCRH:frame-init`, faulting address `WebCoreDriver.dll+0x0`). That is gone.

| | |
|---|---|
| Driver link | `EXIT=0  unresolved=0  duplicate=0`, 40 748 KB, 30 exports, both architectures |
| Package | 52.2 MB appx; `WebCore.dll` (44 MB of ballast) removed and `Exclude`d |
| Driver identity | SHA256 **inside the appx** matches the link output — verified, not assumed |
| Process | 452 s uptime, 111 MB RAM, stable; `app-ctor → onlaunched → mainpage-ctor → mainpage-xaml-ok` |
| Home page | `after-home-render rc=0`; CSS gradient, `border-radius`, `box-shadow`, grid all correct on screen |
| Real page | `example.com` → `title=Example Domain`, `contents=720x1080`, `js=1/1`, `err=0`, `blk=0` |

**What was actually wrong with the link (three independent defects):**

1. the script had been switched to import mode on the reasoning that "`__imp_` references resolve at
   load time" — true only for *exported* names, and `RenderTheme::RenderTheme()` is a `protected`
   constructor. Static `WebCoreFull-x64-19041.lib` is required, as on ARM.
2. `PAL-x64-19041.lib` was missing (144 unresolved `PAL::TextEncoding` / `PAL::UTF8Encoding` /
   `PAL::SessionID`), and `psl.lib` was referenced as `lib\psl.lib` but lives in `libs\psl\`.
3. **The link silently picked the UWP ("store") CRT.** `…\lib\x64\store` is first in `$env:LIB`, and its
   `msvcprt.lib` binds to `MSVCP140_APP.dll` / `VCRUNTIME140_APP.dll`, which exist nowhere on this
   machine. The link reported `EXIT=0` and produced a DLL that could not load. x64 now reorders `$env:LIB`
   for the link step only. **ARM deliberately keeps the store CRT** — its vcpkg `cairo.lib` is built
   against it, and removing it leaves every cairo object with an unresolved `__security_cookie`.

**The next blocker: no text renders at all.** Shapes, colours, gradients, borders and layout are correct;
every glyph is missing. `example.com` is a white page with black text, so it appears blank.

The evidence is the linked image, read through the new `/MAP`:

| library | in the link? |
|---|---|
| FreeType | **yes** — ~40 functions (`FT_Init_FreeType`, `FT_New_Face`, `FT_Load_Glyph`, `FT_Render_Glyph`, `FT_Get_Char_Index`) |
| HarfBuzz | **almost not** — exactly two symbols: `hb_script_get_horizontal_direction`, `hb_icu_script_to_script` |
| Cairo | yes, static via `cairo-complete-x64-uwp.lib` |
| **fontconfig** | **absent — not imported, not linked, not in the payload, and not available to link** |

`llvm-nm` on the driver reports "no symbols" because the DLL is stripped, which is why the `.map` matters.
And a word of warning about it: searching the `.map` for `Fc[A-Z]` gives **65 false positives** — those are
`FCGBFNCI@WCRH…` fragments inside MSVC-mangled RTTI type descriptors, not fontconfig. The real count of
fontconfig symbols is **zero**.

WebCore's font back-end is built on fontconfig: enumerating faces, matching families, resolving the
generic `sans-serif`. With none of it present, no face is ever selected, so FreeType is never handed
anything to rasterise and HarfBuzz is never asked to shape. Rasterisation *would* work — the machinery is
all in the binary. The missing piece is font selection.

**Why this cannot be fixed by adding one library, the way ICU-78 was.** For x64 there is nothing to add:

```
C:\vcpkg\installed\x64-uwp\lib\fontconfig.lib      НЕТ
C:\vcpkg\installed\x64-uwp\bin\fontconfig-1.dll    НЕТ
```

fontconfig builds under meson, which will not cross-compile to UWP — this is the same wall documented for
harfbuzz in [BUILD-X64.md](BUILD-X64.md) §3. ARM is *not* affected: vcpkg's `arm-uwp` does have
`fontconfig.lib` and `fontconfig-1.dll`, and both are already staged in `native\ARM`.

Also worth recording: the two built-in font probes are **dead code**. `APO_HAVE_FREETYPE` and
`APO_HAVE_FONTCONFIG` are never defined anywhere in the tree, so `ApoDumpFontConfig` always prints
`ft=not-probed | fc=not-probed`. The full probe bodies exist and are correct; the macros are simply
unset, because including `<ft2build.h>` / `<fontconfig.h>` in that TU collides with the WebKit/ctk macro
soup. Hand-declaring the fontconfig C API (opaque pointers, no headers) would avoid that, exactly as the
`GetModuleHandleExW` declaration does.

And one metric to stop trusting: `nonwhite=777600/777600` in `stage.txt` claims every pixel is non-white
while the screen is white. A screenshot is the only honest check.

---

## 0a. Phase state as of 2026-09-26 — measured, not assumed

**What was just established (each item is a measurement, not an intention):**

1. **The managed host now survives a native fault.** `rc = -17` (`kErrSeh`) reaches managed code and
   the process stays alive, where before it died in `FailFast(7)` / WER `0x1007` with nothing
   catchable. The SEH guard is opt-in, so the native harness line is unchanged.
2. **`FONTCONFIG_FILE` was never visible to the engine.** The managed `Environment.SetEnvironmentVariable`
   does not reach the native process environment block. The driver now sets it with `_putenv_s`; the
   probe confirms the engine sees the full path.
3. **The font payload is correct and is not the cause.** All five TTFs are present with valid sfnt
   magic (`segoeui.ttf=959752`, `arial.ttf=1016724`, `times.ttf=1190736`, `cour.ttf=817164`,
   `simhei.ttf=9751960`), and the fontconfig cache directory now exists (`cachedirPresent=yes`).
   Shipping the fonts did **not** move the crash site, so the fault is not a font-resolution failure.
4. **The crash is narrower than the old WER stack suggested.** `stage-port.txt` reads
   **`WCRH:frame-init`** — the fault happens inside `Frame::init()`, not in the later
   `FontCascadeFonts` / `cairo_scaled_font_create` path the first WER dump pointed at.
5. **The fault is a call through a null function pointer.** Faulting address
   `WebCoreDriver.dll+0x0`, with a real unwind behind it:
   ```
   SEH stack[0]:  WebCoreDriver.dll+0x14D31      (the filter)
   SEH stack[1]:  VCRUNTIME140.dll+0xF47F
   SEH stack[2-4]: ntdll.dll+0x169D3F / 0xAD2BE / 0x16967E
   SEH stack[5]:  WebCoreDriver.dll+0x0          <-- the call target
   SEH stack[6..]: WebCoreDriver.dll+0x22121C, 0x220B78, 0x2C93C, 0xC0ED, 0x18D1EE, 0x57D90, ...
   ```
   Frames 6+ are inside the engine, statically linked into the driver DLL. The symbol names are not
   in the trace by design (it records module+offset only) — resolving them needs a PDB, which this
   link does not produce.
6. **Both build lines are now scripted and working on the x64 side.** `build-driver-dll-arm32.ps1`
   had four independent defects that made it unable to build *either* architecture; all are fixed
   and the x64 run is verified end to end (13/13 objects, link exit 0, 44 MB DLL, appx installs and
   launches). Details in [BUILD-ARM32.md](BUILD-ARM32.md) §1.
7. **The ARM driver line has no WebKit blocker.** The engine archives are already built in
   `build-arm32-gpu/lib`, so a driver rebuild is minutes, not the half-day WebKit build. What it does
   need is decided: import mode against the existing ARM `WebCore.dll` (recommended), or an ARM full
   static WebCore archive (multi-hour, only if import mode fails).

**What is therefore the next real step:** symbolise frame 6+ of the fault, i.e. find which
`Frame::init()` callee is a null function pointer on the managed path. The candidates are the
platform-strategy / loader-strategy vtables the port installs — note the trace shows
`PS: install ... didInstall=1` and `PS: createLoaderStrategy` completing immediately before the
fault, which makes an incompletely installed strategy the obvious first suspect.

---

## 0. Section index — where old `PLAN.md §...` references went

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

