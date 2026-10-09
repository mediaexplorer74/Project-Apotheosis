# Tap dispatch: who decides where a tap goes

Status: the rule was replaced on 2026-09-18 in `0.1.10.7` and **verified on the same build** (§6). Its
second term was corrected on 2026-09-19 in `0.1.10.18` (§8), where `refused=-1` also got its explanation
(§8c). Everything here is measured on the x64 bench; nothing is inferred from what the code looks like it
should do.

The dispatcher lives in `Src\harness\MainPage.xaml.cpp` (`ForwardClickToEngine`'s `post("tap", …)`
lambda and the `DispatchedHandler` it hands the result to). The engine half is `WebCoreClickAt`
(`Src\port\WebCoreDriver.cpp`).

## 1. The defect

Three local pages, all with a real `<a href="https://example.com/real-destination">`, differing only in
what the page's own `click` handler does. Scripted tap on the link (`nav.txt` = `taplinkstr:real-destination`,
i.e. the same `HandleTapAt` a finger reaches — see CLAUDE.md).

| # | page | handler | `TapDone …` | `[HIT] dom …` | correct? |
|---|---|---|---|---|---|
| 1 | `linkframe.html` | sets text + background, **no** `preventDefault` | `rc=0 changed=1 navEmpty=0 linkHit=1 branch=in-page` | `dp=0 … dpAfter=0` | ✅ engine navigated itself (`navEmpty=0`), harness stayed out |
| 2 | `linkframe2.html` | `preventDefault()` **and repaints** | `rc=0 changed=1 navEmpty=1 linkHit=1 branch=in-page` | `dp=0 … dpAfter=1` | ✅ but *by luck*: the repaint, not the refusal, is what saved it |
| 3 | `linkframe3.html` | `preventDefault()` only, no repaint | `rc=0 changed=0 navEmpty=1 linkHit=1 branch=nav-link` | `dp=0 … dpAfter=1` | ❌ **the harness navigated against the page's explicit refusal** |

Row 3 is the defect. The old gate was

```
branch = nav-link  iff  rc==0 && navW->empty() && !changed && !linkHit->empty()
                                          ^^^^^^^^^^
```

and `changed` is the *frame hash* before vs after the click — a synchronous repaint. It answers a
presentation question ("did anything redraw?"), and it was being used to answer a behavioural one
("did the page take this tap?"). A page that calls `preventDefault()` and then updates itself on a
microtask, a `setTimeout`, or a router transition is indistinguishable from a page that ignored the tap.
Row 3 is the cheapest possible instance of that class.

`navEmpty=1` is not the discriminator either: it only says the engine started no navigation, which is
exactly what a successful `preventDefault()` *looks like*. Neither is the frame hash in the other
direction — row 1 has `changed=1` and `navEmpty=0`, so the two rows that must be treated oppositely
share their `changed` value with rows that must be treated the same.

### 1a. `wasHandled()` cannot answer this, and that was measured before anything was built on it

`WebCoreClickAt` dispatches the click through
`lf->eventHandler().handleMouseReleaseEvent(up).wasHandled()`, and a WebEngine verdict is exactly what a
dispatcher wants. It does not discriminate: `[HIT] settled: press=0 release=1` in **row 1 and row 3
alike** — for row 1 because the engine followed the link, for row 3 because the page consumed the event.
In 2.52.4 the return type is `HandleUserInputEventResult`, which is a plain `bool` wrapper
(`WebCore\page\HandleUserInputEventResult.h`) — there is no `isDefaultPrevented()` to ask for.

The only signal that separates the two rows is the `click` event's `defaultPrevented`, and the only way to
observe it is through a listener.

## 2. The rule, and why each term is there

```
nav-link  iff  rc == 0 && navW->empty() && refused != 1 && !linkHit->empty()
in-page   otherwise (when rc == 0);  nav-fail / dead when the click itself failed
```

| term | means | why it is in the gate |
|---|---|---|
| `rc == 0` | the driver dispatched the tap and returned | a failed click has its own branch (`nav-fail`) |
| `navW->empty()` | the engine's document URL is unchanged | if the engine already navigated, there is nothing to add |
| `refused != 1` | the page did **not** call `preventDefault()` | the page is the authority on its own taps |
| `!linkHit->empty()` | the finger was on a link | nothing to navigate otherwise |

`refused` is `WebCoreLastClickDefaultPrevented()`:

| value | meaning | dispatch |
|---|---|---|
| `1` | the page's handler called `preventDefault()` | **never navigate** — the page took the tap |
| `0` | a `click` event existed and nothing refused it | fall back to the link under the finger |
| `-1` | no `click` event was recorded at all | fall back too: the anchor's default action never had a turn either, which is the case the fallback was written for |

`-1` is deliberately *not* treated as a refusal. The fallback's original purpose survives: a tap that the
page's DOM never saw (element removed mid-gesture, hit test landing between elements) still gets the link
under the finger. What is gone is the harness navigating *over* a page that answered.

`changed` (the frame hash) is still computed and still printed. It is no longer a gate in either
direction: it flips rows 1 and 3 to the same value while they must be treated oppositely, and it also
produced the opposite failure — a real `<a href>` tap whose page repainted for unrelated reasons was kept
in-page and the tap was swallowed.

## 3. Where the verdict comes from

```
click event  ──►  the driver's DOM probe (document-level capture listener, installed per document)
                  window.__apoLastClick = e           ← the event object, kept, not copied: WebKit does
                                                        not pool events, so defaultPrevented is readable
                                                        later and reflects the final value
             ──►  read back at the end of WebCoreClickAt, after the settle pump, as `dpAfter=` in the
                  `[HIT] dom …` line (the authoritative one; the per-record `dp=` is capture-phase and
                  therefore always pristine — a documented false start, see CLAUDE.md)
             ──►  parsed into a file-static `g_lastClickDefaultPrevented`
             ──►  `WebCoreLastClickDefaultPrevented()` — a new export in **both** `WebCoreDriver.h` copies
```

Why the probe rather than C++: see §1a. Why it is acceptable as a *behavioural* input and not only as a
diagnostic: it is already installed unconditionally on every tap (not behind a build flag), it is
capture-phase at the document so an author handler calling `stopPropagation()` cannot hide the event from
it, and its readback happens after the settle pump — i.e. after every handler has run. Its failure mode is
`dpAfter` absent / `-1`, which degrades to the old fallback rather than to a wrong navigation.

The export is added to both header copies because the C ABI header exists twice; they must not disagree.

## 4. How to read a report

One `TapDone` line per tap names the branch that was actually taken:

```
TapDone: rc=0 changed=0 navEmpty=1 linkHit=1 refused=1 hashB=98ff8740 hashA=98ff8740 branch=in-page
                │         │          │        │          └─ the page's own verdict (1 = it refused)
                │         │          │        └─ a link was under the finger
                │         │          └─ 1 = the engine started no navigation
                │         └─ 1 = the frame repainted synchronously (informational only)
                └─ 1 = the bitmap changed; see `changed`
```

`refused=1 branch=in-page` is the SPA case working. `refused=0 branch=nav-link` is the harness covering for
a page that did not react. `refused=-1` means the DOM probe saw no click — worth investigating on its own,
because a real tap should always produce one. **As of 2026-09-19 it has a named and measured cause: on a tap
that navigated, the commit replaces the document before the probe's readback, and the state is gone —
§8c.** It is not "the page ignored the click".

The line also carries `preEmpty=`, `termDiff=`, `pre=` and `cur=` since `0.1.10.18` — the two candidate
readings of the gate's URL term, and whether they disagreed. §8a.

## 5. Reproducing it

`Src\tools\x64-cycle.ps1` or the manual loop, with the pages in LocalState and a scripted tap:

```
nav.txt := file:///…/linkframe3.html        # or linkframe.html / linkframe2.html
nav.txt := file:///…/linkframe4.html        # same, but the page changes its own fragment at load
nav.txt := file:///…/linkframe5.html        # fragment change and nothing that can navigate
nav.txt := taplinkstr:real-destination      # taps link 0, centre, by URL substring
nav.txt := tap:100,45                       # raw ContentArea DIP coordinates, for a non-link target
```

`log.txt` gets the `TapDone` line; `gpuinit-steps.txt` (and `port-trace.txt`) get the `[HIT]` chain,
including `[HIT] settled: press= release=` and `[HIT] dom … dpAfter=`. `linkframe4.html`/`linkframe5.html`
were written on 2026-09-19 for §8; they are bench stimulus, not part of the app.

## 6. Verification: the same three pages on `0.1.10.7` (2026-09-18)

All three re-measured on the fixed build, in the order 3 → 2 → 1, one app launch each. The `TapDone` lines
below are in the format of that build — the fields `preEmpty=`, `termDiff=`, `pre=` and `cur=` were added
later, in `0.1.10.18` (§8a), and are absent here.

| # | page | handler | `TapDone …` | `dpAfter` | outcome |
|---|---|---|---|---|---|
| 3 | `linkframe3.html` | `preventDefault()` only, no repaint | `rc=0 changed=0 navEmpty=1 linkHit=1 refused=1 … branch=in-page` | `1` | **fixed** — was `branch=nav-link`; the document is still `linkframe3.html` in the final diag, i.e. no navigation happened |
| 2 | `linkframe2.html` | `preventDefault()` + repaint | `rc=0 changed=1 navEmpty=1 linkHit=1 refused=1 … branch=in-page` | `1` | still correct, but now for the right reason |
| 1 | `linkframe.html` | repaints, no `preventDefault` | `rc=0 changed=1 navEmpty=0 linkHit=1 refused=0 … branch=in-page` | `0` | engine navigated itself; the harness stayed out |

Row 1 is worth reading in full because it is the end-to-end case, not just a gate: the engine's own
navigation chain appears in `gpuinit-steps.txt`'s `NAV:` markers
(`docloader create` → `policy navigation` → `provisional started` → `main request` → `policy response
status=404 mime=text/html` → `committedLoad bytes=559` → `COMMIT` → `finishedLoading` → `finish load`),
and the next `diag:` line reports

```
diag: url=https://example.com/real-destination title=Example Domain contents=1368x758 body=1
      nonwhite=1036944/1036944 … res:[real-destination(s2)]
```

— a real page fetched over the network after a tap, **every pixel painted**
(`nonwhite == contents`), with the resource `Cached` (`s2`). The 404 is example.com's own answer for that
path, and it is the site's error document that rendered.

Note the ordering trap this run also demonstrates: `TapDone` (`navEmpty=0`) is printed from the tap's own
readback, while the diag that shows the *destination* is written later, on the next `latediag` beat. A
`TapDone` line and the diag next to it in the file need not describe the same document.

## 7. The other face: `navW->empty()` is computed against a stale URL (fixed 2026-09-19 — see §8)

The gate's second term, `navW->empty()`, is "the engine's document URL is unchanged **by this click**".
What the code actually compared was not that:

```cpp
std::wstring prevUrl = m_currentUrl;          // MainPage.xaml.cpp:3554 -- UI thread, BEFORE the post
…
WebEngine::instance().post("tap", [… prevUrl …]() {
    rc = WebCoreClickAt(px, py, rgba->data());          // engine thread
    char u[1024] = ""; WebCoreGetUrl(u, sizeof u);      // read AFTER the click
    std::wstring newUrl = ToWide(u);
    if (!newUrl.empty() && newUrl != prevUrl) navUrl = newUrl;   // :3584
```

`prevUrl` is a snapshot taken when the finger landed, on a different thread, and the tap is queued — it
can wait behind a load or a long settle pump. If the engine's URL changed *for any reason* in that window
(a page-initiated navigation the harness has not applied a frame for yet, so `m_currentUrl` never moved),
then `newUrl != prevUrl` is true for a tap that changed nothing, `navUrl` becomes non-empty, the branch is
`in-page`, and **the harness shows the engine frame instead of navigating the link under the finger** —
the same "the page only flinched" report §1 was about, arriving through a different term.

The measurement that distinguishes the two is free: read `WebCoreGetUrl` **immediately before**
`WebCoreClickAt` and compare against that. Then the term means what the gate says it means — "this click
changed the document URL" — with no dependence on thread timing or on how current `m_currentUrl` is.
**Applied on 2026-09-19 in `0.1.10.18`; the measurements are §8.**

It was deferred on 2026-09-18, deliberately: the crash verification running that day tapped a real link, so
changing the dispatcher in the same build would have left two candidate causes for one result. That
verification finished (it is §6), so the deferral expired. `changed` is not the term to reuse here —
§2 is the record of what happened when a presentation signal was asked a behavioural question.

## 8. The fix, and what it measured — `0.1.10.18`, 2026-09-19, x64 bench

### 8a. What changed

Three things, all in `ForwardClickToEngine` (`Src\harness\MainPage.xaml.cpp`):

1. The term is read on the engine thread, immediately **before** `WebCoreClickAt`:
   ```cpp
   std::wstring urlBeforeClick;
   try { char ub[1024] = ""; WebCoreGetUrl(ub, sizeof ub); urlBeforeClick = ToWide(ub); } catch (...) {}
   ```
   and compared as `newUrl != urlBeforeClick`. The UI-thread capture of `m_currentUrl` is gone from the
   capture list.
2. The old term is **kept for the log only**, as `cur=` on the `TapDone` line — it feeds no branch. It is
   there because the reason it was wrong is a claim about timing that no run had ever shown in a log.
3. `pre=` (the engine-side read), `preEmpty=` and `termDiff=` (`pre` != `cur`) were added to that line. The
   comparison had no line of its own before, which is why §7's hypothesis could not be settled from a log.

`preEmpty=1` names the one case where the new term degrades: if the engine had no document URL to read
before the click, the comparison is against nothing and that tap falls back to the old behaviour.

### 8b. Measured — four scripted taps, all on one build and one launch

Two new local pages were added beside §5's three. `linkframe4.html` sets `location.hash='#apo-stale'` at
load and has one plain link (no handler); `linkframe5.html` sets the same fragment and has **no** element
whose default action could navigate. Nothing in the harness refreshes `m_currentUrl` from the engine
(`m_currentUrl` is assigned only in `NavigateTo`, `ApplyEngineFrame` and `SaveActiveTab`), so the fragment
change leaves the snapshot disagreeing with the engine for the rest of the page's life.

| page | stimulus | `TapDone …` | probe readback |
|---|---|---|---|
| `linkframe4.html` | `taplinkstr:real-destination` | `rc=0 changed=1 navEmpty=0 linkHit=1 refused=-1 preEmpty=0 termDiff=1 branch=in-page` | `dom rc=0 no-logger` |
| `linkframe4.html` | same, repeated | identical | `no-logger` |
| `linkframe.html` | same | `rc=0 changed=1 navEmpty=0 linkHit=1 refused=0 preEmpty=0 termDiff=0 branch=in-page` | `down|a|… ;; up|… ;; click|a|…` |
| `linkframe5.html` | `tap:100,45` (lands on `<body>`) | `rc=0 changed=0 navEmpty=1 linkHit=0 refused=0 preEmpty=0 termDiff=1 branch=in-page` | `down|body|… ;; up|… ;; click|body|… ;; dpAfter=0` |

`termDiff=1` in the first and fourth rows is the finding §7 was missing: **the two candidate terms
genuinely disagree in a real run**, and the state that makes them disagree is reachable by a page doing
something entirely ordinary. It had never been observed before because neither value was printed.

### 8c. `refused=-1` on a tap that navigates — a race, not a page property

Row 1 reported `refused=-1`, which §4 says is "worth investigating on its own". Row 4 is the answer, and it
is the reason `linkframe5.html` exists: with the fragment change but **no** navigation-capable element, the
readback is *complete* — three records and `dpAfter=0`. So the fragment change does not destroy the probe.

What destroys it is the navigation the tap itself starts. The probe keeps its state in
`window.__apoClickLog` (`Src\port\WebCoreDriver.cpp`), and its readback runs after the settle pump; when the
commit lands inside that window the window is replaced and the readback reports `no-logger`, i.e. `-1`.
Whether the readback or the commit wins is not stable — rows 1/2 lost it twice on `linkframe4.html`, row 3
won it on `linkframe.html`, with the same destination and the same build. Both pages' taps navigated; the
difference is timing, and the page correlation between first and third row is incidental as far as this
evidence goes.

**This is benign where it happens:** `refused` is consulted only as `!refused`, and whenever a tap navigated,
`navW` is non-empty, so the branch was `in-page` under `refused=0` and `refused=-1` alike. But it must be
read correctly: **`refused=-1` on a tap that navigated means "the document was replaced before the
readback", not "the page ignored the click".**

### 8d. What this closes, and what it deliberately does not

- **Closed:** §7. The term is read where it means something, and it is logged.
- **Closed:** the "worth investigating" note in §4 — `refused=-1` now has a named, measured, benign cause.
- **NOT closed — no branch-level reproduction.** The decisive state is `refused` falsy, `linkHit`
  non-empty, and a click that changed no URL; every scripted tap that reached the term had the engine
  navigate, which makes the term's answer "non-empty" under *both* readings. So this build is measured to
  be **no worse** (all four taps branch correctly) and the fix's premise is measured to be reachable
  (`termDiff=1`), but **the reported symptom — "the page only flinched" — has not been reproduced by
  script, and this change is not shown to be its cure.** On a real page the decisive state is reachable in
  one more way than on this bench, because a page-initiated navigation (the `NAV:` chain, no harness
  involvement) moves the engine URL with no tap at all; reaching it needs a finger and a live site, not a
  script — the same ceiling §5 records.


