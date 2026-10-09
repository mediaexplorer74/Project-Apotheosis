# Why hh.ru stalls: a resource load that never completes

*(The file is named after the first diagnosis, which was wrong twice before it was right. The name is
kept because CLAUDE.md and the task list point at it; the history below is deliberate, not clutter —
each correction records how a plausible reading survived longer than it should have.)*

One root cause behind two symptoms that were tracked separately: hh.ru never reaching
`readyState Complete`, and buttons on hh.ru doing nothing when tapped. Measured on x64 0.1.8.68,
2026-08-17, from a real browsing session.

> **Correction, same day, after a second session.** The conclusion below — that these bundles never
> execute — **does not hold**, and the reason is worth reading before trusting any `ready=` number.
> `m_readyToBeParserExecuted` is assigned in exactly one place in all of WebCore,
> `WebKit/Source/WebCore/dom/ScriptElement.cpp:334`, in the branch for **inline** parser-inserted
> scripts held up only by pending stylesheets (`!hasSourceAttribute()`). It is never set for an
> external `<script src=...>` and never reset. So `ready=0` on a bundle is false *by construction*
> and says nothing about whether it loaded or ran.
>
> The empirical half of the correction: in one session of eight taps on the same site, all seven
> bundles reported `ready=0 err=0` **every time**, including the taps where the button visibly
> worked and the wizard advanced. A field that is constant across success and failure cannot be the
> cause of the failure. And `__reactFiber$...` was present on the tapped elements throughout, which
> means React was mounted — impossible if `appEntryPoint.js` and `App.js` had never executed.
>
> What actually varies is `press=` / `release=` (`wasHandled()` on the synthesized events) and
> `connected=`. Note that `connected=` is sampled *after* `pumpLoop`, so `connected=0` can just as
> easily mean the click **succeeded** and React replaced the subtree. The real behaviour is a flaky
> one — the same button on the same URL responds on one visit and not the next — which points at
> timing, not at a missing script. See the section "What the second session actually showed" at the
> end of this document.
>
> Kept below unedited, because the measurements are real and the reasoning is a useful record of how
> a misread field can look exactly like a root cause.
>
> **Second correction, later the same day.** With the field replaced by flags that mean something, the
> answer changed again — and this time there is a reproducible stall with a named culprit. Read
> "Third measurement: the real stall" at the end first; it supersedes both the section below and the
> first correction. The short version: the page's scripts do run, the scheduler drains to empty, and
> WebCore is nonetheless left holding a handful of resources at `CachedResource::Pending` — one of
> them the script the parser is blocked on.

## What the user sees

On `https://hh.ru`:

- the main page renders and scrolls, but the load never finishes;
- on `https://balashiha.hh.ru/account/login?role=applicant&...`, the **Зарегистрироваться** button
  does not respond to a tap at all;
- and yet the cookie banner's **Понятно** button dismisses the banner, and the header **Войти**
  navigates to the login page.

Those three facts together are what make the diagnosis unambiguous: JavaScript is alive, event
dispatch works, and only a specific class of script never runs.

## The measurement

From `gpuinit-steps.txt` (the `[HIT]` instrumentation), for the tap on **Зарегистрироваться**:

```
[HIT] 435,353 -> <span> id=[] class=[] href=[-]
[HIT] settled: press=1 release=1 connected=1
[HIT] js rc=0 rs=complete fw=__reactFiber$z2or00wynme@0 src=19/30 bodyLast=IFRAME
[HIT] scripts n=30 defer=7 inorder=0 err=0 parserOwned=[
        #5:vendors.e5b8f4f92075e032.js        ready=0 err=0;
        #6:bloko.aa5f2bf803afd12c.js         ready=0 err=0;
        #7:magritte.a824bd1f2421bc57.js      ready=0 err=0;
        #8:components-common-js.ed372e12fe882d89.js ready=0 err=0;
        #9:runtime.9e1cf7cb71954d9c.js       ready=0 err=0;
        #10:appEntryPoint.73efd3d2f751920f.js ready=0 err=0;
        #11:App.599037c1e3a80a90...          ready=0 err=0]
```

and, for the main page, the `diag:` line says the same thing in its own vocabulary:

```
rs=I/p1/ig0/pr0/st0/le0 pending=33 scripts=43
scr=defer=4 err=0 blk=4[#5:vendors/r0 #6:bloko/r0 #7:magritte/r0 #8:components-common-js/r0]
```

Same four bundle names, same `ready=0`. The login page simply gets further down the list before
stalling.

## Reading the fields

From `Src/port/WebCoreDriver.cpp:2085-2107`:

- the list only contains scripts for which `willBeParserExecuted()` is true. In WebKit that covers
  both parser-blocking scripts **and** deferred ones, so a `defer` bundle legitimately appears here;
- `ready=` is `script->readyToBeParserExecuted()`;
- `err=` is `script->errorOccurred()`;
- `defer=` counts `willExecuteWhenDocumentFinishedParsing()`.

So `ready=0 err=0` means **loaded-but-never-signalled**, not *failed*. Nothing errored; the engine is
simply still waiting for a completion that never arrives. That distinction is the whole finding: a
network or TLS problem would have set `err=1`, and a hit-test or event problem would not have shown
`press=1 release=1 connected=1`.

## Why the three behaviours differ

- **Войти** is a plain `<a href>`. Navigation is the engine's own job and needs no page script.
- **Понятно** is handled by an inline or non-deferred script, which does run.
- **Зарегистрироваться** is a `<span>` inside a React-controlled button. Its handler lives in
  `appEntryPoint.js` and `App.*.js` — both listed above with `ready=0`. The handler was never
  attached, so the tap lands on a live element and nothing is listening. `press=1 release=1
  connected=1` proves the engine did its part.

## Where the bug is

The hypothesis is already written in the code, at `Src/port/WebCoreDriver.cpp:2076-2078`:

> the parser is waiting for a load that, judging by the `res:` list, has already been cached, so the
> completion was never signalled back to the `PendingScript`.

The `res:` list does show those bundles present, which matches. So the suspect path is
`CachedScript` -> `PendingScript::notifyFinished`, i.e. the port's loader telling a waiting script
that its resource is done — most likely the case where the resource is already in the memory cache
when the client is added, and the notification has to be scheduled rather than delivered inline.
Start from `PortPlatformStrategies` and the LoaderStrategy, not from the event-dispatch path.

Fixing this should move both symptoms at once. Verify with both: the main page must reach `rs=C`, and
the login page's **Зарегистрироваться** must respond.

## Open oddity, to explain rather than ignore

The login page reports `rs=complete` while seven deferred scripts have never run. Deferred scripts
execute before `DOMContentLoaded`, so the document should have stayed at `Interactive`. Either the
load watchdog forces the transition, or the reading is taken from a different document — note
`bodyLast=IFRAME`, so iframes are present. Whichever it is, it means `readyState` alone is not a
trustworthy completion signal here, and `parserOwned=` is the field to trust.

## Instrumentation debt found while reading this

`writeDiag` at `WebCoreDriver.cpp:876-880` increments its `defer` / `errored` / `blocking` counters
*after* the `break` that fires when the 130-byte name buffer fills, so on a page with many scripts
those counts are truncated undercounts — `defer=4` and `blk=4` on the main page are probably low. The
equivalent loop at `2085-2107` has the same `break` but counts before it, so only its name list
truncates. Make the first behave like the second before drawing conclusions from the numbers.

## Reproducing, and where the evidence is

1. Navigate to `https://hh.ru`, then to the login page, and tap **Зарегистрироваться**. Since
   2026-09-18 the tap itself is scriptable — write `taplinkstr:<text>` (or `taplink:<n>`) into
   `LocalState\nav.txt`, the same file the URL goes into; it routes through the same `HandleTapAt` a
   finger reaches. See `CLAUDE.md`, Commands.
2. Read `gpuinit-steps.txt` — in the app via burger -> Settings -> Diagnostics, or directly at
   `%LOCALAPPDATA%\Packages\EdgeHTMLReborn.Harness_<hash>\LocalState`.
3. The session this document is built from is preserved outside LocalState at
   `%TEMP%\localstate-hhru-session` (LocalState itself is wiped by `Remove-AppxPackage`, which both
   `x64-cycle.ps1:87-89` and the local install helper do on every run).

Note that `glyph.log` in that session is 9.95 MB. Anything that reads the whole diagnostics set has
to read tails, not files.

## What the second session actually showed

Eight taps, one install (0.1.8.69), the same hh.ru registration route walked three times. Preserved at
`%TEMP%\localstate-hhru-warm`. Only the fields that vary are shown; `defer=7 inorder=0 err=0` and all
seven `ready=0` were identical in every one of the eight.

| # | element | `settled:` | outcome |
|---|---|---|---|
| 1 | `<span class=magritte-button__label…>` | `press=0 release=0 connected=0` | dead |
| 2 | `<span>` under `href=…/account/login…` | `press=0 release=1 connected=1` | navigated |
| 3 | `<span class=[]>` | `press=1 release=1 connected=1` | worked |
| 4 | `<span class=magritte-button__label…>` | `press=0 release=0 connected=0` | dead |
| 5 | `<span class=magritte-button__label…>` | `press=0 release=0 connected=0` | dead |
| 6 | `<span>` under `href=…/account/login…` | `press=0 release=1 connected=1` | navigated |
| 7 | `<span class=[]>` | `press=1 release=1 connected=1` | worked |
| 8 | `<span class=magritte-button__label…>` | `press=0 release=1 connected=1` | worked |

Read this way it says something quite different from the section above:

- **`release=` is the discriminator.** Every failure has `release=0`, every success has `release=1`.
  `release=` is `handleMouseReleaseEvent(...).wasHandled()`, and the DOM `click` plus the default
  action happen on release, so `release=0` means nothing in the page consumed the click at all.
- **`press=0` is normal**, including on successes — most elements do not consume mousedown.
- **The same element class both fails and succeeds** (#4, #5 dead; #8 worked). Same URL, same install,
  minutes apart. That is a race, not a missing handler, and it is the single most important fact
  here: a deterministic cause was ruled out by the page working on retry.
- The sequence in `WebCoreClickAt` is: `elementFromPoint` for logging
  (`WebCoreDriver.cpp:1975`) → `handleMouseMoveEvent` / `Press` / `Release`
  (`2011-2016`) → optional `focus()` for text fields (`2021-2033`) → `pumpLoop`
  (`2036`) → the `settled:` line (`2042`). So there is **no** settle between hit-test and dispatch;
  the earlier suspicion of a hit-test/dispatch race is wrong. What is still open is whether React has
  finished attaching its listeners for that subtree at the moment the release is dispatched.

### What to instrument next, before changing behaviour

The present fields cannot separate "not hydrated yet" from "hydrated but our event misses the
listener", and one of them is measuring nothing at all. Concretely:

1. ~~Stop printing `readyToBeParserExecuted()`.~~ **Done, 2026-08-17.** Both loops now print, per
   parser-owned script, three flags that come from state that actually moves:
   `l` = `LoadableScript::isLoaded()`, `e` = `LoadableScript::hasError()`, `f` =
   `ScriptElement::haveFiredLoadEvent()` (the element dispatched its load event, i.e. the script
   executed). The `diag:` line's `blk=[...]` list reads `#5:vendors.js/l1e0f0` instead of the old
   `/r0`; the `[HIT]` list spells the same three out as `loaded= err= fired=`. The combination to
   look for is **`l1 e0 f0`** — the bundle arrived, nothing failed, and it never ran. A dash means the
   element has no `LoadableScript`, which is normal for an inline script.
   Fixed in the same pass: both loops used to `break` out entirely when their name buffer filled,
   which truncated the `defer` / `err` / `blk` counters on any script-heavy page — so hh.ru's
   `defer=4 blk=4` were undercounts of a page with seven parser-owned bundles among 43 scripts. They
   now stop naming and keep counting, and append `...` when the list was cut.
2. Log whether a DOM `click` event was dispatched at all, not only whether something claimed it.
   `wasHandled()` false and "no listener existed" are different statements.
3. Log the tapped element's listener situation directly — whether the node or an ancestor has a
   registered `click` listener — which turns "is React bound here yet?" into a measurement.
   **Blocked, and worth knowing why before trying:** `EventTarget::hasEventListeners(AtomString)`
   would answer it in one line, but `EventNames.h` is not in the sparse `WebKit\` checkout and the
   symbol is not in `Src/port/webcore-exports.def`, so reaching it costs a WebCore relink. As a
   cheap stand-in, `[HIT] ancestors:` now prints up to four ancestors with their classes, which at
   least distinguishes "the tap landed on the label inside the button" from "the tap missed".
4. Only then decide whether the fix is to wait for hydration before dispatching, to re-dispatch when
   the first release is unclaimed, or something else. Guessing before step 1 is what produced the
   section above.


## Third measurement: the real stall

Measured on x64 0.1.9.2, 2026-08-17, with the `l/e/f` script flags, the fixed counters and the new
post-load `latediag` probes. hh.ru was set as the home page so the load happens unattended, with no
tap and no interaction to muddy the picture.

**Two runs of the same page, twenty minutes apart, with opposite outcomes.**

Run A, healthy:

```
scr=defer=24 err=0 blk=25[#5:vendors/l1e0f1 #6:bloko/l1e0f1 #7:magritte/l1e0f1 #8:components-common-js/l1e0f1 ...]
loader: serve started=152 inflight=0 pending=0 hosts=0
```

Every parser-owned bundle: loaded, no error, load event fired — they executed. The scheduler served
152 requests and drained to nothing.

Run B, stalled. Five `latediag` samples, five seconds apart, **byte-identical**:

```
rs=L/p1 body=0 bodyKids=-1 nonwhite=0/710656 pending=7 sheets=0 scripts=27
scr=defer=24 err=0 blk=25[#2:vendors.6546808ae3f2/l0e0f0 #3:bloko/l1e0f0 #4:magritte/l1e0f0 #5:components-common-js/l1e0f0 ...]
res:[... vendors.6546808ae3f243a1.js(s1) ... magritte.7e12c24ae768cf69.css(s1) __fonts_...css(s1) ...]
loader: serve started=149 inflight=0 pending=0 hosts=0
```

Read together, those lines say something quite specific:

- `readyState` is **Loading**, not Interactive, and never moves. There is no body at all
  (`body=0`, `bodyKids=-1`, zero non-white pixels) — the page is blank, not half-rendered.
- The first parser-owned script, `vendors...js`, is **`l0`**: its load never finished. The three
  behind it are `l1e0f0` — downloaded, no error, never executed, because the parser will not step
  over #2 to reach them. That is correct WebCore behaviour, not a bug.
- `res:` confirms it independently: `vendors...js(s1)`, and `(s1)` is `CachedResource::Pending`.
  Seven resources are in that state, including the CSS and the font sheets, which is why `sheets=0`.
- **The scheduler is idle**: `started=149 inflight=0 pending=0 hosts=0`. It believes it has nothing
  left to do.
- No `loader: DECLINED` lines, so `SubresourceLoader::create` accepted every request. No curl errors
  at all this run (`lasterr=[]`), so nothing failed on the wire.

So WebCore is waiting for seven loads that the scheduler does not think it owns. Either those requests
never reached the scheduler, or they reached it and their completion never came back. The comment
already in `loadResource` names the mechanism exactly — *"a dropped CompletionHandler is exactly how a
resource ends up Pending forever"* — but the `DECLINED` path it guards is not the one firing here.

That it is **intermittent** is itself the strongest clue: the same URL, the same build, minutes apart,
once fine and once stuck. This is a race in scheduling or completion, not a missing feature and not a
site-specific quirk.

### What was added to catch it

`PortPlatformStrategies.cpp` now traces a matched pair per request: `loader: +<ptr> <leaf-name>` when
`serveHost` starts a load, and `loader: -<ptr> h=<0|1>` when `remove()` retires it. The pointer is the
identity — unique and stable for the load's lifetime — so **a `+` with no matching `-` is the stuck
request, and it carries its own name**. The `h=` flag records whether `remove()` still found the host:
it looks the host up by the loader's *current* URL, so a load that crossed origins, or whose host was
already dropped from the map, would silently skip releasing its slot.

Three outcomes to expect on the next stalled run, and what each would mean:

- a `+` for `vendors...js` with no `-` → the request started and its completion was lost. Follow the
  curl bridge in `ResourceHandle.cpp` and `ResourceLoader`'s terminal path.
- no `+` for it at all → it never reached the scheduler. Follow `loadResource` and `scheduleLoad`:
  `SubresourceLoader::create` succeeded (no DECLINED), so the loss is between that and the pending
  queue.
- `h=0` on some `-` line → the host bookkeeping is the leak, and `crossOriginRedirectReceived` is the
  first place to look.

### Corrected along the way

The old counters were undercounts, exactly as suspected: hh.ru reports `defer=24 blk=25`, not the
`defer=4 blk=4` that had been quoted for weeks. Both loops used to `break` out entirely when their
name buffer filled, which stopped the counting too. They now stop naming and keep counting, and mark a
truncated list with `...`.

### Fourth measurement: the scheduler is exonerated

Ran with the paired trace in place, x64 0.1.9.3, seven more unattended loads of hh.ru. The stall did
not reproduce in its hard form (`rs=L`, `body=0`) — but the *common* form did, six times out of seven,
and it is stable enough to work with:

```
rs=I/p1  body=1  nonwhite=92068/710656  pending=31  sheets=1  scripts=43  defer=24 err=0 blk=25
```

`nonwhite=92068` came out **identical** in every one of those runs: the page paints the same fraction
and stops. One run out of seven went all the way (`rs=I/p0`, `nonwhite=623030`), which is the same
intermittency seen before, now with a much narrower gap between the two outcomes.

The paired trace settles the first two candidates:

```
loader: serve started=156 inflight=0 pending=0 hosts=0
starts / finishes : 156 / 156     (153 distinct addresses, so some were recycled)
host lookup misses: 0
declined creates  : 0
STILL OPEN        : 0
```

Every request the scheduler started was retired, `remove()` always found its host, nothing was
declined, and no curl error was recorded. **The scheduler does not lose loads.** So the search moves to
WebCore's own notification path: a `CachedResource` that finished without the `PendingScript` or the
parser being told.

One concrete lead came out of the cross-check: of the 25 resources the document holds at
`CachedResource::Pending`, exactly one name never appears in a start line —
`17f09f88-babf-439e-92d2-0039ac9953e0`, which looks like a third-party beacon endpoint rather than
anything the page needs. Treat it as a lead and not as proof: the trace keeps only a truncated leaf
name, so the comparison is a prefix match, and a resource that was fetched and then re-requested can
legitimately appear at `(s1)` again.

Two false negatives were burned on the way to this, both now encoded in
`Src/tools/analyze-loader-trace.ps1` so they cannot recur:

- MSVC's `%p` prints a pointer with **no** `0x` prefix. A pattern requiring `0x` matched nothing and
  cheerfully reported a clean run.
- Loader addresses are recycled. Testing set membership therefore lies; the pairing has to count
  starts and finishes per address.

Reproduce, unattended, in about two minutes: set `home=https://hh.ru` in `settings.ini`, launch, wait
110 seconds for the loader to go idle and the five `latediag` samples to land, then run
`pwsh -File Src\tools\analyze-loader-trace.ps1`.
