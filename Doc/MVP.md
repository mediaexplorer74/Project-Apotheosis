# MVP v1 — acceptance contract

## Goal

A small, honest browser for the Lumia 950. It is allowed to be slow, limited, and imperfect. It is not
allowed to look finished while failing to show a loaded page.

## Must work

1. The app starts and reaches the browser UI.
2. The address bar is visible, accepts a URL, and navigates on Enter/Go.
3. At least ten selected sites load in sequence without a blank content surface.
4. A loaded page paints visible content, not only a correct title or HTTP response.
5. Back returns to the previous page.
6. Scrolling moves a page that is taller than the viewport.
7. History and URL suggestions show saved entries and do not cover the address bar.
8. A failed or unavailable site reports failure; it does not silently show a white page.
9. The same smoke list is run on x64 and then on the real Lumia 950.
10. The app remains usable after the smoke list without restart.

## Explicitly out of scope for v1

GPU presentation repair, subframes, authentication flows, video, MSE, performance tuning, and cosmetic
features unrelated to opening and reading pages. Software presentation is the MVP-safe path.

## Current baseline

- The shipping target is the **C# UWP app** (`Src/Apotheosis`, `ME.Apotheosis`); the C++/CX harness is
  a debugging tool for the native side. The acceptance run below was measured on
  `ME.Apotheosis_0.2.1.4_x64`; the current build is `0.2.3.9_x64`, and the viewport/scaling work that
  landed in between is not yet covered by it (see the note at the end of this file).
- The thirteen-site list runs unattended over the `LocalState` `autocmd` channel, in one process,
  13/13 PASS. The per-site record is written to `smoke.csv`.
- Software presentation is the MVP-safe path. GPU direct-present is not required for v1 and is off by
  default in the bench settings; the GPU investigation is tracked in [GPU-LIVENESS.md](GPU-LIVENESS.md).
- Device validation is still required before calling v1 complete: nothing in this document has been
  demonstrated on ARM32.

## Record a result

For each site record: URL, load result, painted pixels/content, scroll result, back result, and crash or
hang marker. A test that only checks `rc=0` is incomplete.

---

## Status, reviewed 2026-10-03

The contract above is unchanged. This section records what has actually been demonstrated, so that
"done" means demonstrated rather than intended. Evidence is `LocalState` files and return codes, per
the rule in [INDEX.md](INDEX.md); a screenshot counts only as proof that pixels reached the screen.

The 2026-10-03 x64 run is the first full pass of the list. It is one process, thirteen sites, driven
entirely through `LocalState` by [run-smoke.ps1](../Src/tools/run-smoke.ps1); the per-site record is
`smoke.csv` under `%USERPROFILE%\Documents\apotheosis-smoke`, with a screenshot per site and, for the
engine's own view of the page, `diag:`. `13/13 PASS`, `alive=True` on every row.

| # | Requirement | x64 | Lumia 950 | Evidence |
|---|---|---|---|---|
| 1 | starts and reaches the UI | **met** | **met** | appx installs and launches; `boot.txt` shows `app-ctor → onlaunched → mainpage-ctor → mainpage-xaml-ok`; on the Lumia `0.2.3.23` launches and paints dzen.ru — a blank screen on one attempt, real content on the next |
| 2 | address bar, navigate on Enter | **met** | not run | navigation via `testurl.txt`, the `goto` verb and a real tap |
| 3 | ten sites in sequence | **met** (13) | not run | `smoke.csv`: 13 sites, one process, no restart, no death |
| 4 | paints visible content | **met** | not run | engine `diag: nonwhite` per site (99.64 % nokia-archive, 1.86 % info.cern.ch) plus a screenshot each |
| 5 | back returns to previous page | **met** | not run | driven by the new `back` / `forward` verbs through the controller, on the ENGINE's own history list. Measured `0.2.3.36`, one process, `example.com` → tap a link → `www.iana.org`: `hist` reports `engine back=1`, `back` answers `rc=0 moved=1` and lands on `https://example.com/`, `forward` answers `rc=0 moved=1` and returns to the link target, and the frame is repainted at each step. Before the fix the same probe reported `engine back=0 fwd=0` — the list was empty (see the note below) |
| 6 | scrolling moves a tall page | **met** | **failed** | a real touch-path swipe per site, 667–872 ms, logged as `swipe end totalMs=`; on the Lumia the process dies **idle, mid-load**, ~2 s after a completed teardown (`Document::notificationClient()`, in a `WebCore::Timer` callback) — the trace shows no scroll ever reached the engine, so the blocked scroll is the symptom; port-side fixes are deployed in `.25` and again in `.29` (null `SocketProvider`), both **unverified** |
| 7 | history and suggestions | **met** | not run | the home page renders `history.tsv` tiles with correct Cyrillic, and the dropdown is now inside the scripted path: `suggest dzen` → `rows=6 vis=1`, `+700 vis=1 h=251 boxfoc=1` (never more than a third of the content area, one clipped line per row, scheme-less URLs), `enter` → `vis=0` with the page clean 14 s later |
| 8 | failure reported, not a white page | **met** | not run | `12-bad-domain` → `rc=-16`, error page drawn, process alive — a failure is *reported* |
| 9 | same list on x64 then device | **partial** | **started** | the x64 pass is complete; on the Lumia the app launches and paints, and the process dies **idle, mid-load** before any gesture reaches it — the list has not been attempted in sequence |
| 10 | usable after the list | **met** (x64) | **failed** | process alive after all thirteen; the list is followed by no restart, no `WEDGE`, no `VEH:` — on the Lumia the process dies idle mid-load, so nothing past the first paint survives |

### Requirement 5: Back is the engine's list, not the host's stack (2026-10-14)

Back was the last requirement with no measurement at all. It now has one, and getting there named a
fourth member of the `pageConfigurationWithEmptyClients` family.

**The list was empty, and could not have been otherwise.** `EmptyBackForwardClient` answers every
question with nothing -- `addItem` is `{ }`, `backListCount()` returns 0 -- so
`BackForwardController::canGoBackOrForward(-1)` was permanently false however many links the user
followed. `PortBackForward.cpp` replaces it with a recording client (a `Vector` plus a cursor),
exactly as `PortStorage.cpp` replaced `EmptyStorageNamespaceProvider` and `PortWebSocket.cpp` replaced
`EmptySocketProvider` before it.

**Three stubs had to be lifted together, not one.** Removing the empty client alone would have left
Back dead, because two history calls in the port's own `LoadingFrameLoaderClient` were still stubs
copied from `EmptyFrameLoaderClient`: `createHistoryItemTree` returned `nullptr` (and
`updateBackForwardListClippedAtTarget` bails on a null item before it ever reaches `addItem`), and
`shouldGoToHistoryItem` answered `No`, a policy veto that makes `HistoryController::goToItem` return
on its first line. All three are fixed, and each one is a call that *looks* like a working default.

**The port's own loads were never written down.** A tap synthesises real mouse events, so the engine
follows the anchor itself through `FrameLoader::load()` and that navigation *was* recorded — the
trace showed exactly one item, `n=1 cursor=0`, for the link target, and none for the document the
port had painted. The port fetches with curl and feeds the bytes straight into the active
`DocumentWriter`, so `transitionToCommitted()` never runs and neither does its
`case FrameLoadType::Standard: history().updateForStandardLoad()`. `apoRecordSessionNavigation`
(WebCoreDriver.cpp) closes that gap: it writes the URL into the loader `FrameLoader::init()` created
from an *empty* `ResourceRequest` (`urlForHistory()` reads it, and upstream's own guard drops an empty
one), then reproduces `updateBackForwardListClippedAtTarget`'s three lines from the exported pieces.

**The measured result** (`0.2.3.36`, one process, driven entirely through `LocalState\autocmd.txt`):

```
hist   engine back=1 fwd=0 cached=1/0 host idx=2/2 url=https://www.iana.org/help/example-domains
back   history back rc=0 moved=1 can=0/1 url=https://example.com/
hist   engine back=0 fwd=1 cached=0/1 host idx=1/2 url=https://example.com/
forward history fwd rc=0 moved=1 can=1/0 url=https://www.iana.org/help/example-domains
```

`rc=0` is the engine answering, not the host fallback (`rc=-99`). The engine's list is per session —
a new `goto` builds a new page and therefore a new client — so the host stack still carries Back
across sessions, and `NavigateHistory` consults the engine first and falls back.


### The Lumia column is no longer empty (2026-10-03)

`0.2.3.23` was built, passed `Test-AppxPayload.ps1` (30 binaries all arm, driver at the package root,
`WebCore.dll` absent, 40/40 declared symbols exported) and deployed as
`ME.Apotheosis_0.2.3.23_arm__cj4nrm0qt9s1y`. So requirement 9 has begun, and the first two device
answers are in the table above: **the app starts and paints, and scrolling kills it.**

**This is a browser defect, not a site defect.** The maintainer's own runs reproduce it on dzen.ru
twice; the dying object's `vtbl` resolves to `??_7Timer@WebCore@@6B@`, so the process ends inside the
callback of a one-shot `WebCore::Timer` on the main thread, right after four tracker loads, with MIME
content-type parsing the last work that completed. It leaves no SEH line, no dump and no WER record.
Requirement 6's Lumia cell is therefore a real measurement, and the most valuable one the device has
produced: before this run the same column read `not run` for everything, and "we have never watched it
work on the phone" is not a state a 1.0 can be argued from.

The next device run is the maintainer's, with a real finger — synthetic input does not reach the UWP
input stack, so this death cannot be produced from the bench — and the trace now carries a death net
(`VEH:`, `DEATHNET:`, `ABRT:` lines) for it to be caught by.

### The mechanism is now known, and it is not the scroll (2026-10-03, after late night)

The `0.2.3.24` run — the instrumented one, whose whole purpose was to tie a death to a gesture — says
something the table above could not. Across four tests and 5,838 trace lines, **the only input markers
are `close` and the idle `live` tick: no scroll, no click, no zoom, no resize ever reached the engine.**
Both fatal tests completed their teardown (`teardown: exit why=load dt=17ms` / `8ms`), ran ~1,400 more
lines of load work, and then died **idle**: an access violation reading `0x8` inside
`Document::notificationClient()`, in a one-shot `WebCore::Timer` callback. So the maintainer's blocked
scroll, which this file reported as the cause, is the *symptom* of a process that was already dying.

**The fault is a missing supplement, and it is the port's gap, not WebKit's bug.**
`Document::notificationClient()` has no null check, and it finds its `NotificationController` through a
`Supplement<Page>` that only the *embedder* installs — the only in-tree caller of
`provideNotification()` is `WebKitLegacy/mac/WebView/WebView.mm`. This port builds every Page with
`pageConfigurationWithEmptyClients()`, so the supplement is absent and the lookup faults at `+8`.
`0.2.3.25` installs a `PortNotificationClient` (all permissions denied) at all four `Page::create`
sites the driver has; it is a **relink, not a rebuild**. Built for x64 first, relinked for ARM,
`AUDIT PASSED`, deployed to the Lumia **unlaunched** as
`ME.Apotheosis_0.2.3.25_arm__cj4nrm0qt9s1y`. **Unverified until the maintainer's next run** — the
device cells above stay `failed` until that run says otherwise.

Two more results from the same pull: `.24` has **no abort deaths at all** (the `.23` teardown-abort
class did not recur), and four tests produced **three** engine processes, so one launch never reached
engine init.

**2026-10-09: the `.25` AV did not recur, and the assert that replaced it is now named and fixed —
also unverified.** On the `.28` pull the process still dies, but not at `Document::notificationClient()`:
the assertion is `RELEASE_ASSERT(m_channel)` at `WebCore/Modules/websockets/WebSocket.cpp:288`, because
`pageConfigurationWithEmptyClients()` installs `EmptySocketProvider`, whose `createWebSocketChannel()`
returns `nullptr`, and dzen.ru opens a WebSocket (Yandex Metrika). It is the **same empty-client trap as
the missing `NotificationController` supplement**, one supplement over — named by hand from the `.28`
stack window, not by the reader, because the reader had a second bug of its own (`ApoImageString()`
never copied the string it validated, so both of its nets matched uninitialised stack garbage).
`0.2.3.29` fixes both and installs a port-side `SocketProvider` whose channel answers
`ConnectStatus::KO`; deployed as `ME.Apotheosis_0.2.3.29_arm__cj4nrm0qt9s1y` (`HTTP 202 in 88 s`).

**2026-10-09: the `.29` device run answers it — the assert did not fire, and an older, already-cured
defect did.** The trace (pulled after the browser was closed; it 404s while its owner lives) shows the
first session loading `sso.dzen.ru/install`, painting `nonWhite=0` (the maintainer's white window),
tearing the load down, and then aborting in **`RedBlackTree<RunLoop::TimerBase::ScheduledTask,
MonotonicTime>::insert`** → `crashDueToCheckedPtrToDeadObject` — both inside
**`JavaScriptCore.dll`** — i.e. the RunLoop `CheckedPtr`-to-dead-object class found on the bench on
2026-09-18 and cured in `WebKit/Source/WTF/wtf/RedBlackTree.h` on 2026-09-19. That cure is a **header**
change, and the ARM line is a **relink**: the ARM `JavaScriptCore.dll` on the phone is dated
**2026-08-23** and does not contain the fix or its `TREE-BAD` probes, while the bench's is 2026-09-27 and
does. So the Lumia cells above stay `failed`, but the blocker is now named and it is not the engine's
behaviour — it is a stale ARM binary. **`JavaScriptCore.dll` must be rebuilt for ARM32**; see
[BUILD-ARM32.md](BUILD-ARM32.md), docket step 6. The second `.29` session survived many scroll and pinch
gestures with no death at all.

### Two cautions about reading that table

**`nonwhite` is the engine's framebuffer count, not a screen measurement.** The verdict originally
used a screen-capture pixel share and it returned an identical **99.68 %** for all twelve sites —
because the app's dark chrome fills the window, so the number described the window and not the page.
dzen.ru is the row that shows why the distinction matters: `rc=0` **and** `nonwhite=0.00`, a load that
reported success and painted nothing.

**Requirement 8's row cannot report a page number.** On the failure path the loader writes its `diag`
before it renders the error HTML, and a failed load has not replaced the engine's document, so that
`diag` still describes the page before it. The row reports `n/a`; the evidence for requirement 8 is
`rc=-16` and the process staying alive, not a paint figure.

### Are we near 1.0?

**No, and the reason is unchanged: 1.0 is gated on device validation.** Eight of the ten requirements
are met on x64, against four on 2026-09-27 — but none of them has been demonstrated on
the phone, and requirement 9 is the phase exit criterion in [INDEX.md](INDEX.md).

What the x64 pass actually bought is a working acceptance instrument and a server-side of evidence:
the list runs unattended, each row carries its own URL, return code, painted-pixel count and swipe
cost, and two of the three ways that record could have lied have been found and closed. The remaining
work in order:

1. ~~add a `back` verb and drive Back through the controller, closing requirement 5~~ — **done
   2026-10-14** (`0.2.3.36`): `back` / `forward` / `hist` are in the scripted channel and the engine's
   own history list answers them; see the requirement 5 note above. The suggestion list had moved
   into the scripted path on 2026-10-03 (`suggest` / `enter`), so requirement 7 closed with it;
2. run the same list on the Lumia 950, closing requirement 9 — the C# line has not been deployed there
   at all, and the App Container, the ABI, the JIT and the font stack each differ. The ARM package
   itself now exists as of 2026-10-03 (`Apotheosis_0.2.3.19_arm.appxbundle`, Release|ARM: 22 ARMNT
   engine DLLs at the package root, 39 exports, the string PRI present), so the remaining work here is
   the deploy path and the run — see [BUILD-ARM32.md](BUILD-ARM32.md). Deploy **Release**, not Debug:
   the Debug manifest requires `.Debug` framework packages a retail Lumia does not have;
3. a pass/fail decision on the remaining cosmetic Cyrillic fallback artifacts.

Two known limitations are recorded rather than scheduled, because neither is an MVP path: the
failure-path `diag` above, and the fact that the fitted-viewport work (the content area drives the
engine viewport, with the page zoom as the only width lever) is **not yet covered by the acceptance
table** — that table was produced on `0.2.1.4`, while the fixed `720x1080` viewport was still in
place, so every row predates it. The white-side-fields symptom that limitation used to describe is
gone as of `0.2.3.9`: on a fitted 1355-DIP window the desktop layout is exactly the content area and
the mobile layout fills it edge to edge. `0.2.3.14` removed the last of the earlier design: mobile
mode no longer re-lays the page out at a fixed 412 CSS px and magnifies it by `clamp(areaW / 412,
1.0, 3.5)` (the measured 3.29 whose effect was "масштаб сайта конский"), and page *scale* is gone
everywhere, so the zoom is one user-level setting and a pinch commits it. Re-running the list, and
covering the `zoom` verb, is the honest way to close the gap, and it belongs with the GPU pass below.
GPU presentation remains explicitly out of scope (software presentation is the MVP-safe path). The
2026-10-03 re-check answered the question it was asked: the C# line does **not** crash on the GPU path,
it fails cleanly at a named call (`WebCoreGpuInit rc=-20`, `GLContext::create -> null`) and presents
through software, while the C++/CX harness on the same bench reaches `Composite=0` on the same engine —
so the gap is in the C# line and not on the bench. See [GPU-LIVENESS.md](GPU-LIVENESS.md) §10.

