# Two failed taps on dzen.ru — 2026-09-18, `0.1.9.110`

**Status:** both diagnosed by measurement. The diagnostic lie and the behavioural fix are done in
`0.1.10.0`, and the fix turned out to be much smaller than §5 first proposed — because the worker
**already had** a correct stall detector and the consumer was simply waiting on the wrong question.
Read §3 before touching that wait. **Verified on the bench the same day** (`0.1.10.2`): the re-arm fires
and preserves a transfer the old build threw away (§7a, §7d) — and proving it exposed a second defect, a
hardcoded threshold in the harness that had been calibrated against the very number §5 replaced (§7b).

**Status, later the same night:** the detector had a **second** blind spot, found while verifying the
first — `dlnow` counts *body* bytes, so an entire 302 chain reads as a transfer that never moved, and the
worker's own callback was killing healthy hops with `crc=42`. Fixed and measured in §8; dzen.ru's SSO
chain now completes (4 hops, 3.46 MB, `http=200`), and with a real document on screen the scripted tap
reaches `branch=nav-link`. **The only unclosed piece of the original report is §3's question** — the
engine still gives up on a slow-but-healthy transfer at a 9.5 s ceiling.

**Status, 2026-09-19 (`0.1.10.17`): §7b's coupling had a second half, and it was firing on healthy
pages.** With the fetch fixed, the *engine* became the watchdog's blind spot: `finished` counts
*completed jobs*, and one `nav-load` job legitimately holds the engine thread for ~7 s on a 3.6 MB page
(parse + inline JS + first paint). Two cold dzen.ru navigations were dumped as `WEDGE`s and loaded
perfectly 1.9–2.6 s later. §9: the port now publishes its own liveness counter
(`WebCoreGetEngineActivity`) and the harness asks *that*, with the residual threshold derived per job
from the port's own bounds instead of one number calibrated against a budget that no longer exists.
Re-measured on the same navigation: one `beat-stuck`, one explicit `beat-gauge … NOT a wedge`, no dump.

Session: `LocalState\prelaunch-0918-1755`, x64 bench, GPU on, dzen.ru loaded from a URL, three-swipe
scroll gesture done earlier, page idle. The maintainer then tapped two links. This is what the logs say
about each.

## 1. Tap 1, 18:07:39.853 — "the page only flinched"

```
18:07:39.853  OnPageTapped: dip=(512.5,330.5) engine=(513,331) session=1 links=59
18:07:39.853  ForwardClickToEngine: px=513 py=331 interacting=0 loading=0
18:07:40.332  beat-stuck #1 busy=1 pending=0 finished=1365 job=tap loading=1
18:07:41.072  beat-stuck #2 busy=1 pending=0 finished=1365 job=tap loading=1
```
and then **nothing** — no `NavigateTo`, no engine `NAV:` chain, until the second tap 8 s later.

The tap itself was sound. The driver's own hit probe named what it landed on:

```
[HIT] 513,331 -> <p> id=[] class=[dzen-desktop--card-top-avatar__text-Pu]
      href=[https://dzen.ru/news/story/d0e6062e-fcc2-5722-a714-9c8dff8e73de?...]
[HIT] ancestors: < a.dzen-desktop--card-top-avata < div.dzen-desktop--card-news-stor < li ...
[HIT] settled: press=0 release=1 connected=1
```

So the finger landed on a `<p>` inside a real `<a href>`; the release was handled by the page
(`release=1`) and the element stayed in the document (`connected=1`). No navigation was started by
either side.

**Where the decision was lost is the harness, not the engine.** `ForwardClickToEngine`'s dispatcher
(`MainPage.xaml.cpp:3433-3447`) resolves the outcome as:

```cpp
if (rcCopy == 0) {
    if (navW->empty() && !changedCopy && !linkHit->empty())
        NavigateTo(linkHit, true);            // harness-driven navigation
    else
        ApplyEngineFrame(rgba, title, navUrl, links);   // "the page handled it in-page"
}
```

`changedCopy` is `WebCoreGetFrameHash()` before-versus-after the click. dzen.ru is a React SPA: a tap
runs the page's own handler, the DOM is marked up (visited state, hover, a prefetch), **the frame hash
changes**, and the harness concludes the page handled the click in-page and deliberately does not
navigate. The visible "flinch" is that branch's `ApplyEngineFrame` re-blit.

**Not proven at the time of that session, and the reason is now removed.** Which of `changedCopy ==
true`, `rcCopy != 0` with an empty `linkHit`, or an empty `navW` actually held could not be settled,
because the dispatcher logged none of its inputs. Every other dead-end in this project was eventually
closed by making the decision observable, so `0.1.10.0` adds exactly that — one line, `TapDone: … branch=`,
whose `branch=` field names the branch taken (see §6). The next tap on this page answers the question
from the log instead of by inference. The `finished` counter advancing 1365 → 1396 across those 8 s
confirms the engine did real work in between; it does not say which branch ran.

> **Superseded, 2026-09-18 — read §8d before reusing the paragraph above.** The dispatcher's gate was
> changed from `changedCopy` (the frame hash) to `refused`, so a page that merely repaints on tap no
> longer has its navigation suppressed; and the fetch fix in §8 is what puts real links under the finger
> in the first place. Both were needed, and a tap on the loaded dzen.ru now reaches `branch=nav-link`.

## 2. Tap 2, 18:07:48.240 — "жуть с error curlcode=28"

Same element class, a different story card, and this time the harness **did** navigate:

```
18:07:48.240  OnPageTapped: engine=(476,439) session=1 links=59
18:07:48.240  ForwardClickToEngine: px=476 py=439
18:07:49.027  beat-stuck #1 ... job=tap loading=1
18:07:49.432  NavigateTo: url=https://dzen.ru/news/story/4f6d7ef9-...   ← 1.19 s after the tap
18:07:49.450  [STAGE] before-load https://dzen.ru/news/story/4f6d7ef9-...
18:07:50.456  beat-stuck #1 ... job=nav-load loading=1
18:07:51.168  beat-stuck #2 ... job=nav-load loading=1
18:07:51.896  beat-stuck #3 ... job=nav-load loading=1
18:07:52.071  [STAGE] after-load ... rc=-16 compositing=0        ← 2.6 s after before-load
ERR: phase=toplevel-fetch type=Curl curlcode=28 domain=curl
     desc=background fetch still running (radio stall?)
```

and in `gpuinit-steps.txt`, in order:

```
SL: curl download url=https://dzen.ru/news/story/4f6d7ef9-...
SL: curl bg timeout -- error page, worker continues
SL: curl done crc=28 http=0 size=0
SL: WebCoreSessionLoad buildSession returned rc=-16
SL: WebCoreSessionLoad buildSession failed, tearing down
SL: fetch eff=https://dzen.ru/news/story/4f6d7ef9-... redirects=0 cookies=13
    size=1016600 http=200 crc=0
SL: hop[0] status=200 setcookie=2 loc=-
```

**The page was fetched successfully and thrown away.** 1 016 600 bytes, HTTP 200, 13 cookies, `crc=0`
— libcurl never failed and never timed out. The failure is the port's own budget:

```cpp
// WebCoreDriver.cpp, WebCoreSessionLoad
bool finished = ft.cvDone.wait_for(lk, std::chrono::milliseconds(2500), [&] { return ft.done; });
```

2 500 ms. The last three `SL:` lines are the worker, still running, finishing the same transfer seconds
after the engine had already rendered the error page for it. `curlcode=28` is not a value libcurl
produced; it is a constant the consumer presets at the top of the function
(`int crc = (int)CURLE_OPERATION_TIMEDOUT;`) and then reports as if it were one.

The diag line confirms the document was never replaced: `url=https://dzen.ru/`, the old title,
`contents=1368x6707`.

**A second thing the same log shows:** `NavigateTo mem 604144/12497952 KB` immediately before this
navigation, against `292652 KB` when dzen.ru was first loaded ten minutes earlier. 300 MB of growth at
idle. Not today's defect, but it is what turns a 2 500 ms budget into a coin flip.

## 3. Why the budget exists (do not simply raise it)

The comment above the budget records the reason: a stalled radio used to park this thread inside
`curl_easy_perform` past the platform's patience, and every silent death in that era unwound to exactly
that call. 2 500 ms was chosen so a **dead** radio produces an error page instead of a hang.

That protection is correct and must survive. But note what the mechanism actually is: a **fixed
deadline**, which cannot tell "dead radio" from "slow but moving transfer". The observed case is the
second, and it is the common one — a 1 MB page on a phone will rarely fit in 2.5 s.

**And the worker had already solved that problem for itself, which the first write-up of §5 missed.**
`ApoFetchChannel::run` sets `CURLOPT_LOW_SPEED_LIMIT = 1` / `CURLOPT_LOW_SPEED_TIME = 2` and installs
an `XFERINFO` callback that returns non-zero — aborting the transfer — after **2.5 s without progress**
or **8 s total**:

```cpp
if (now - w->start > WTF::Seconds(8))          return 1;
if (now - w->lastProgress > WTF::Seconds(2.5)) return 1;
```

So the stall detection already existed and was already correct; it was the *engine thread's wait* that
asked the wrong question. 2.5 s is the right threshold for "no bytes moved", and the wrong threshold for
"the transfer has not finished". The fix is therefore not new machinery — it is to make the wait ask
the worker's question.

## 4. The framework page that carries the tap

`beat-stuck ... job=tap loading=1` on **both** taps, including the one that never navigated: the engine
sets its own loading flag during `WebCoreClickAt` (`pumpLoop(... allowEarlyStopWithoutNav=true ...)`),
so `loading=1` during a tap is expected and does not indicate an engine-initiated navigation. The
`NAV: docloader create url= substitute=0` lines in `gpuinit-steps.txt` are **not** the missing
navigation either: they are emitted by the harness's own `buildSession`, which creates a
`DocumentLoader` with an empty URL by design and then feeds bytes through `DocumentWriter` itself. That
reading was the first hypothesis here and it is wrong; it is recorded so it is not re-derived.

Also pre-existing and not a defect: two `WEDGE: no progress for 6 beats` dumps during a dzen.ru
navigation — one at 17:56:45, `job=nav-load`, self-clearing in 2 s. Older sessions show the same pair.

## 5. The fix — APPLIED in `0.1.10.0`, measured on the bench in §7

**The fixed deadline became a stall detector.** The semantics the message always claimed ("radio
stall?") are the ones now implemented.

- `ApoFetchChannel` carries `std::atomic<long long> progressDl`, reset to `-1` for each job. The
  worker's existing `XFERINFO` callback stores `dlnow` into it on every call — a new pointer field on
  `ApoFetchWatch`, because the watch itself is a stack local of the worker's job and the engine thread
  has to read something with the channel's lifetime.
- `WebCoreSessionLoad` waits in a loop: 2 500 ms; on expiry it compares `progressDl` with the value seen
  at the start of that wait. **Advanced** → re-arm, `DBG_STAGE("curl bg: still moving … re-arming")`.
  **Unchanged** → give up, `DBG_STAGE("curl bg: no progress …")`. A ceiling of **9 500 ms** sits just
  above the worker's own 8 s `CURLOPT_TIMEOUT`, so that in the slow case the worker's error — which
  carries curl's reason — is preferred to this function's guess.

A dead radio makes no progress, so the original protection is intact — and now it is the *worker's own*
threshold that ends the wait, not a second, tighter one on the engine thread. The extra machinery §5
first proposed (a byte counter fed from the write callback) was unnecessary; the counter already
existed inside curl and only had to be published.

**`gaveUp` and the `SL: DISCARDED` marker:** on a stall the engine sets `ft.gaveUp`, and the worker logs
the pair explicitly when a transfer it abandoned later completes. With the fix this should be rare — a
transfer can now only be abandoned if it truly stalls — but the marker stays, because "the error page was
wrong" is exactly the statement that was previously unavailable.

**The trade-off, stated plainly:** in the genuine-stall case the engine thread now blocks for up to
9.5 s where it used to give up at 2.5 s. That is acceptable on this path — the wait is on a condition
variable, not inside `curl_easy_perform`, it is bounded and self-terminating, and the UI thread does not
wait on it (it shows the loading state) — but it is a real change in when the user sees an error page
and it is the part to revert first if the device objects. It also stays far inside `pumpLoop`'s 30 s
watchdog.

## 6. What this closes and what it does not

**Closed:** the `curlcode=28` misdiagnosis. It is filed as
`phase=toplevel-fetch type=PortBudget curlcode=0 domain=port-budget`, the worker writes the explicit
`SL: DISCARDED …` pair when an abandoned transfer lands late, and the wait itself no longer abandons a
transfer that is still moving (§5). Taps are now observable — the dispatcher logs one line naming the
branch it took:

```
TapDone: rc=0 changed=1 navEmpty=1 linkHit=1 hashB=… hashA=… branch=in-page
```

`branch=` is `nav-link` (the harness navigates), `nav-fail` (the click failed but a link was known),
`in-page` (treated as the page's own business, **no** navigation happens here) or `dead`.

Also closed by §7b: the wedge watchdog no longer fires on a healthy slow fetch. It used to be a
duration estimate calibrated against the 2.5 s budget; it now asks the driver for the byte count.

**Still open, and cheap:**

1. **The frame-hash heuristic is unsound for SPAs.** "The frame changed, therefore the page handled the
   click" is false for any framework that repaints on hover or marks a card visited — which is what
   `branch=in-page` on a real `<a href>` will now show. A better test exists in the same dispatcher: the
   page did not change the URL, and the tap landed on an element inside an `<a href>`. Whether to prefer
   the link is a product decision.
2. **The harness never retries.** A stall still ends in an error page with no automatic second attempt,
   even though the next attempt usually succeeds.

## 7. Verified on the bench, 2026-09-18 — and the coupling it broke

The first run that proved the fix also broke the wedge watchdog. Both belong in one place, because they
are the same fact: the engine thread is now legitimately blocked for longer than a hardcoded number in
another file assumed.

### 7a. The re-arm fires, and preserves the transfer

Build `Harness_0.1.10.0_x64` (18:46:08). Before trusting any log, the installed binary was checked for
this code rather than assumed to carry it: `still moving`, `no progress for %lld ms`,
`ceiling %lld ms reached` and `TapDone: rc=` are all present as strings in
`C:\Program Files\WindowsApps\EdgeHTMLReborn.Harness_0.1.10.0_x64__edmb40rfkwsbg\Harness.exe`.

The original article, with the cookie jar already populated by a dzen.ru home load, **now loads**:

```
SL: fetch eff=https://dzen.ru/news/story/4f6d7ef9-… redirects=0 cookies=13 size=1010647 http=200 crc=0
[STAGE] nav: OK … rc=0 title=Трехдневные выборы начались в России — подробности события | Дзен
```

That is the page whose 1 016 600-byte twin was discarded in §2. But it finished **inside** the budget on
that warm connection, so it exercised nothing: a pass is not evidence for §5. The re-arm was therefore
forced with a transfer that cannot fit in 2 500 ms and cannot exceed the worker's own 8 s either.
Measured link speed on this bench, ~1.18 MB/s (20 MB in 16.9 s, `curl`), so 3 MB sits between the two
bounds. Timed by polling `gpuinit-steps.txt` for the fetch line rather than by reading a clock off the
engine:

```
SL: curl bg: still moving (dl=1383670, 2512 ms in), re-arming
SL: fetch eff=https://speed.cloudflare.com/__down?bytes=3000003 redirects=0 cookies=13 size=3000003 http=200 crc=0
SL: curl done crc=0 http=200 size=3000003
```

1 383 670 of 3 000 003 bytes had arrived at 2 512 ms; the wait re-armed instead of giving up, and the
transfer completed 4 457 ms after the navigation was issued, `nav: OK … rc=0`. **Under `0.1.9.110` this
is exactly the failing case**: the same shape of transfer is abandoned at 2 500 ms, an error page is
rendered, and `curlcode=28` is reported as a libcurl timeout while the bytes are still on the wire.

### 7b. The threshold in the other file that named this budget

`MainPage.xaml.cpp` dumps a wedged engine after **six beats** (~4.2 s) of an unchanged `finished`
counter, and said why, in its own comment:

```cpp
// 6 x 700 ms = 4.2 s, comfortably past the 2.5 s fetch ceiling
```

"the 2.5 s fetch ceiling" was *this* file's budget, named from the harness. Raising the legitimate
block to a 9.5 s ceiling invalidated that calibration silently, and the 3 MB run produced the predicted
false positive one second before the page finished loading:

```
18:53:54.249  WEDGE: no progress for 6 beats (finished=549 busy=1 loading=1) -- dumping engine tid=6544
18:53:55.413  [STAGE] after-load url=https://speed.cloudflare.com/__down?bytes=3000003 rc=0 compositing=0
```

The page was loading correctly. The dump cost a full stack walk of every thread plus, per that dumper's
known behaviour, a pair of handled first-chance AVs (`VEH: code=0xC0000005 … n=4` / `n=5` in
`VCRUNTIME140_APP.dll`, the stack-copy artefact, not a fault). On the device the same thing would fire
on **every page slower than ~4.3 s**, which is most of them.

**The fix is to stop estimating.** One new export, added to both copies of the ABI header:

```cpp
long long WebCoreGetFetchProgress(void);   // bytes so far, or -1 when no top-level fetch is in flight
```

`ApoFetchChannel` already carried the byte counter (§5); it now also publishes `inFlight` (it mirrors
`!done`, which is guarded by `mtx` and cannot be read from the UI thread), and the heartbeat treats
advancing bytes as progress even while `finished` stands still:

```cpp
long long nowFetchDl = WebCoreGetFetchProgress();
const bool fetchMoving = (nowFetchDl >= 0 && nowFetchDl != s_lastFetchDl);
s_lastFetchDl = nowFetchDl;
bool noProgress = (nowFinished == s_lastFinished) && !fetchMoving;
```

`beat-stuck` prints `fetchdl=` as well, so a reader can see which of the two shapes it was. Six beats
still means "nothing moved for ~4.2 s" — and that is now *literally* true, where before it meant "no
*job* completed", which a long fetch also satisfies. The pre-body phase of a fetch (DNS, TLS) reports
-1 throughout and still accumulates stuck beats, so a genuinely stuck transfer can still be dumped;
that is the pre-existing behaviour, not a regression.

**The durable lesson, worth more than the bug:** two files were coupled by a magic number and nothing
said so at the site that changed. A comment in one file is not a check in the other. Prefer asking the
other component over estimating it — the estimate goes stale silently, the question does not. The same
reasoning is already in project memory as *"a diagnostic that guesses will eventually lie"*; this is
the second instance of it in this subsystem.

### 7c. What is still not verified

`TapDone: … branch=` (§6) is present in the installed binary, but **no tap has been performed since it
was added** — a scripted `nav.txt` navigation is not a gesture, and synthetic input cannot reach the
UWP manipulation stack on this bench. §1's branch therefore remains an inference, and stays marked as
one until a real tap on dzen.ru is read out of the log.

**Update, 2026-09-18 — the gap is now partially closable, and the closure has a boundary.**
`LocalState\nav.txt` (the same file, the same watcher) gained `tap:<x>,<y>`, `taplink:<n>` and
`taplinkstr:<text>`, routed through `TapFromScript` → `HandleTapAt` — *the same function* the XAML
manipulation stack calls. So the engine-side half of §1 can be replayed on demand: a scripted tap
reaches `WebCoreMousePress`/`WebCoreMouseRelease`, the `[HIT]` markers, and `TapDone: branch=`, which
is what §1 needs. The boundary: it **bypasses XAML gesture recognition**, so a defect that lives in the
manipulation stack — a tap that never becomes a press because the panel swallowed it — still cannot be
reproduced here. The two halves are now separate questions instead of one, which is the improvement.
See `CLAUDE.md`, Commands, for the directive syntax.

### 7d. The watchdog, on the same kind of stimulus — and the counter's cadence

Both fixes were then measured together on one transfer: an endlessly streaming public source
(`https://ice1.somafm.com/groovesalad-128-mp3`, ~44 KB/s, no `Content-Length`), so the engine thread is
blocked while bytes arrive continuously for as long as the worker lets it run.

```
SL: curl bg: still moving (dl=277107, 2504 ms in), re-arming
SL: curl bg: still moving (dl=317231, 5014 ms in), re-arming
SL: curl bg: still moving (dl=359863, 7522 ms in), re-arming
SL: curl done crc=28 http=200 size=366132
```

The wait re-armed **three times** across an 8 s block — 3.2x the old deadline — with the byte count
advancing steadily. Under `0.1.10.0` the same stimulus gives a `PortBudget` error page at 2 500 ms, a
`WEDGE` at ~4.2 s and `SL: DISCARDED`; under `0.1.10.1` there is no `WEDGE` in the log at all for those
eight seconds, and the failure is attributed to the party that actually failed, with curl's own words:

```
ERR: phase=toplevel-fetch type=Curl curlcode=28 domain=curl
     desc=Operation timed out after 8036 milliseconds with 366132 bytes received
```

That is §5's stated intent observed rather than asserted: the worker's 8 s `CURLOPT_TIMEOUT` fires
first, the engine's 9 500 ms ceiling is never reached, and the error carries curl's reason instead of
this port's guess. Note the class of the resource is irrelevant — an endless audio stream is not a
document and *should* fail — what is being measured is which end of the wait decides, and why.

Two residuals were visible in the same run and only one of them was acceptable:

- **`beat-stuck #1` still appeared, twice, eight seconds apart** (`job=nav-load loading=1 fetchdl=-1`)
  and each time the counter had reset to `#1`. `fetchdl=-1` is right for the header phase, but those
  beats were *during* the body. The cause is cadence: `progressDl` was written only from the
  `XFERINFO` callback, and curl's progress meter runs at ~1 Hz — slower than the 700 ms heartbeat — so
  roughly every other beat read an unchanged value, and the stuck counter oscillated `0->1->0->1`
  instead of staying at 0. Six beats (4.2 s) against a ~1 s update is enough margin that the `WEDGE`
  never fired, but the margin is an accident of two unrelated intervals and nothing states it.
- **Fixed by moving the counter to the write callback**, which fires on every body chunk:
  `ApoWriteSink { std::string* body; std::atomic<long long>* progress; }` is now `CURLOPT_WRITEDATA`, so
  the counter tracks the bytes themselves. One beat of silence now means one beat of silence. The
  `XFERINFO` store stays as a backstop for the header phase.

**Re-measured on `0.1.10.2`, same stimulus, and the difference is the whole point.** A positive control
was previously missing: `fetchdl` had only ever been observed as `-1`, which is what "no fetch" reports
too, so the export could have been dead and the log would have looked the same. Now it is not:

```
19:43:08.207  beat-stuck #1 busy=1 pending=0 finished=49 job=nav-load loading=1 fetchdl=-1   ← TLS/header
19:43:09.638  beat-stuck #1 busy=1 pending=0 finished=49 job=nav-load loading=1 fetchdl=0    ← first bytes
SL: curl bg: still moving (dl=279615, 2515 ms in), re-arming
SL: curl bg: still moving (dl=320993, 5027 ms in), re-arming
SL: curl bg: still moving (dl=359863, 7540 ms in), re-arming
19:43:16.237  [STAGE] after-load … rc=-16
```

`fetchdl=0` on the second beat is the proof that the counter is published *and* read while the engine
is blocked — the value the old XFERINFO-only counter could not deliver until ~1 s in. The first beat
reports `-1` correctly (nothing has arrived yet), and each of these two is `#1`, i.e. it reset. After
them, across the remaining **nine beats** of an eight-second block with bytes arriving, there is **no
`beat-stuck` line and no `WEDGE` at all** — where `0.1.10.0` produced a `WEDGE` at ~4.2 s on a
*shorter* block. The fix is measured, not inferred.

**A negative result worth keeping, because it looks like a defect and is not.** The first attempt at a
deterministic slow source was a local HTTP server on this host's LAN address. The app never reached it:
`SL: curl bg: no progress for 2500 ms (dl=-1), giving up` and `SL: curl done crc=28 http=0 size=0`, with
**no request in the server's own log** — a UWP AppContainer can reach the internet but not the private
network without the `privateNetworkClientServer` capability, which this manifest does not declare. The
error page was *correct* there, and the diagnostic said so: `type=PortBudget` for the port's own
give-up, in the port's own words. The stream source was used instead precisely because it needs no
manifest change.

---

## 8. The same detector's second blind spot — a redirect chain reports zero bytes

*Applied 2026-09-18, after §7, and measured on the bench the same night. The before/after pair in §8d
is the same machine, the same URL, hours apart.*

### 8a. The reading that gave it away

```
SL: fetch eff=https://login.vk.com/?act=autologin&redirect_uri=… redirects=1 cookies=9 size=0 http=302 crc=42
SL: curl done crc=42 http=302 size=0
```

`crc=42` is `CURLE_ABORTED_BY_CALLBACK`: curl did not fail — **our own progress callback killed the
transfer**. One hop recorded, `size=0`, and the effective URL is the *second* URL of dzen.ru's SSO
chain: the transfer was ended one hop in, while the chain was behaving exactly as designed.

### 8b. Why — `dlnow` counts body bytes, and a redirect hop has none

The XFERINFO callback's third argument is a download-byte count. For the entire life of a 302 — the
connect, the TLS handshake, the request, the server thinking, the header block coming back — it is
**0**, because no body byte has been written yet. §7's re-arming budget was built to watch bytes, and a
chain of redirects therefore presents itself as a transfer that has never moved: exactly the shape the
old fixed deadline punished. The budget change made the *engine* patient; the callback that aborted was
still in the worker, still armed by the same 2.5 s of "no progress", and it fired first, preempting both
the engine's wait and curl's own timeouts.

Measured on dzen.ru: the 302 to `login.vk.com` was killed at 2.5 s with `dl=0` and one hop recorded.
Nothing was wrong with the network, the TLS, or the budget.

There was a second, quieter defect in the same place. `progressDl` starts at `-1` as "no progress
reported yet" — the same sentinel `WebCoreGetFetchProgress` still publishes as `-1` for "no fetch in
flight". The engine's wait loop compared the first *real* reading, `0` (the value before any body
byte), against that sentinel, read the difference as movement and logged `still moving (dl=0)` for a
transfer that had not moved at all — buying it a re-arm it had not earned. It stayed invisible because
its error is in the *safe-looking* direction: a dead transfer looks healthy for one budget.

### 8c. The fix — three parts, all in `Src\port\WebCoreDriver.cpp`

No WebKit edit, so this cost minutes rather than the ~1 h of a `WebCore.dll` relink pair.

1. **Activity is bytes *or* a completed response header block.** The `HEADERFUNCTION` (ST-4, already
   recording hops for `SL: hop[n]`) counts blocks into `progressHops`; the engine's test becomes
   `moved = (nowDl != seenDl) || (nowHops != seenHops)`. A hop that completes is progress whether or
   not a body followed it.
2. **The callback reports, it does not abort.** Its body no longer returns 1. After 3 s of a genuinely
   unchanged `(dl, hops)` it emits one
   `SL: fetch-silent why=silent-3s dl=… hops=… total=… conn=… tls=… ttfb=… hdr=… conns=… -- reported,
   not aborted: curl's own timeouts will name the cause` and then leaves the ending to
   `CURLOPT_CONNECTTIMEOUT` (3 s), `CURLOPT_TIMEOUT` (8 s) and `CURLOPT_LOW_SPEED_LIMIT/TIME` (1 B over
   2 s) — rules that can each name their own cause, unlike `42`.
3. **The published value stops being a byte count.** `WebCoreGetFetchProgress()` returns
   `dl + (hops << 40)`: the hop count folded in above any body size an HTML document could plausibly
   reach. The harness only ever compares it against its own previous reading, so what has to be visible
   is a *change*, and the fold guarantees a completed hop cannot alias into an unchanged number. The
   heartbeat/stuck field was renamed `fetchdl=` → `fetchprog=`, because the old name asserted a meaning
   the value no longer has. **A stale name is how the next agent reads a number wrongly.**

The first reading is now *adopted* rather than compared, which also covers the connect + first-byte
window where a `moved` verdict cannot exist yet; it costs one extra budget only when nothing at all has
happened, and a dead radio is already being ended by curl's own 3 s connect timeout.

### 8d. Measured — the same URL, hours apart, `0.1.10.12`

Before (23:56 run, `prelaunch-0918-2356\gpuinit-steps.txt`):

```
SL: fetch eff=https://login.vk.com/?act=autologin&redirect_uri=…&state=…&uuid=…&app_id=51441269 redirects=1 cookies=9 size=0 http=302 crc=42
SL: curl done crc=42 http=302 size=0
```

After (23:58 run, `LocalState\gpuinit-steps.txt`):

```
SL: fetch eff=https://dzen.ru/ redirects=3 cookies=11 size=3459216 http=200 crc=0
SL: hop[0] status=302 setcookie=2 loc=https://login.vk.com/?act=autologin&redirect_uri=…
SL: hop[1] status=302 setcookie=2 loc=https://dzen.ru/api/auth/web/autologin-vk?errorCode=11300&errorText=invalid+user
SL: hop[2] status=302 setcookie=0 loc=https://dzen.ru/
SL: hop[3] status=200 setcookie=0 loc=-
SL: curl done crc=0 http=200 size=3459216
SL: cookies applied=4 hops=4 eff=https://dzen.ru/
```

Four hops, 3.46 MB of document, `http=200`, `crc=0` — the same URL that two hours earlier ended at hop
1 with zero bytes. Two details in there are worth more than the headline. `hop[1]` carries
`errorCode=11300 errorText=invalid+user`: **the chain contains a hop that fails by design** (VK's
autologin has no session) and the navigation is supposed to continue through it, so a detector tuned to
"an error appeared" would have to be told which errors are fatal. The byte-or-hop rule needs no such
knowledge. And `cookies=11 … applied=4` shows the ST-2 cookie engine carrying the session across the
hops, which is what the SSO chain needs to converge instead of re-issuing a uuid forever.

The page then actually loads, and the tap on it behaves — where §1 and §2 of this document recorded "the
page only flinched" and then an error page:

```
23:58:36.148  TapDone: rc=0 changed=1 navEmpty=1 linkHit=1 refused=0 hashB=ba087620 hashA=17a8f6af branch=nav-link
```

23:42:28.802  simtap: taplinkstr:<dzen.ru/a/> -- no match in 0 links      ← fetch aborted: empty document
23:58:36.148  TapDone: rc=0 changed=1 navEmpty=1 linkHit=1 refused=0 hashB=ba087620 hashA=17a8f6af branch=nav-link
```

The first line is the same scripted tap (`<dzen.ru/a/>`) in the 23:42 run of the *same tree*, where the
fetch was still being aborted at hop 1 — the page it was tapping had yielded **zero extracted links**,
so "no match in 0 links" was an artefact of an empty document, not of the tap path. The second line is
the same tap 16 minutes later on the fetched document: `linkHit=1` (a real `<a href>` was under the
finger), `refused=0` (the page did not `preventDefault`), `navEmpty=1` (the engine did not navigate on
its own), and therefore `branch=nav-link` — the harness navigated to the link itself. The page moved.

**§1's conclusion needs one correction, and it is a fix that landed separately.** §1 reasoned that the
dispatcher's gate was `changedCopy` (the frame hash) and that dzen.ru's SPA markup therefore swallowed
the tap as "handled in-page". The gate in the current code is **`refused`**, not `changed` — see the
comment above `branch=` (`MainPage.xaml.cpp:3625`): `refused=1` means the page's own click handler
called `preventDefault()`, i.e. the page has taken responsibility and the harness must not undo it;
`refused=-1` (no `click` event recorded at all) is the one case that still falls back to the link under
the finger. `changed=1` in the line above is now only reported, never gating. So the tap defect of §1
was closed by that gate change *and* the fetch fix together, and the flinch is no longer expected on a
loaded page.

### 8e. What this closes, and what it deliberately does not

- **Closed:** dzen.ru's top-level fetch, and the whole `crc=42` class. A transfer can now only die of a
  cause curl itself names; "our callback aborted it" is no longer reachable.
- **Not closed — §3's question, which still stands.** The budget is now a stall detector rather than a
  deadline, but the *engine* still gives up at a 9.5 s ceiling and still discards a transfer that is
  running correctly, only slowly. That is the remaining half of the original defect, and raising the
  ceiling is still the wrong move for the reason §3 gives.
- **Not exercised: the report-only path.** No `SL: fetch-silent` line appears in any recorded run, so
  the 3 s silence report has never fired on a real transfer — it is written and compiled, not proven. If
  a transfer ever stalls honestly, that line is the one to look for, and its absence means the stall was
  in the pre-body phase (where XFERINFO does not tick at all) rather than in the transfer.
- **The coupling that remains, and it is the same shape as §7b's.** The harness compares `fetchprog=`
  against its own previous reading only; it has no absolute meaning and no threshold. A change of
  cadence in the port — a different fold, a different hop unit — silently changes what the harness
  counts as progress without touching the harness. Both sides of this number are documented in
  `Src\port\WebCoreDriver.h` (and its `Src\harness` twin) for that reason.


## 9. The watchdog's other end — `finished` cannot see the engine working, 2026-09-19

§7b fixed half of a coupling: the harness's wedge threshold had been calibrated against the 2.5 s fetch
budget §5 removed, so it was re-pointed at `WebCoreGetFetchProgress()` instead of at a duration. The
same session that verified §8 then produced a **false `WEDGE` with a full engine stack dump for a page
that loaded perfectly** — twice, on the one navigation the bench can reproduce on demand (a cold
dzen.ru, whose SSO chain ends in a 3.6 MB document). The remaining half of the coupling was not in the
fetch at all; it was in what the watchdog believed about the *engine*.

### 9a. The two readings

Both are the same shape, on the same URL, both ending in a healthy page. From `log.txt`:

```
04:35:30.253  NavigateTo: url=https://dzen.ru/ pushHistory=1 loading=0
04:35:31.745  beat-stuck #1 busy=1 pending=1 finished=357 job=nav-load loading=1 fetchprog=-1
   … #2 … #3 … #4 … #5, all identical …
04:35:35.326  WEDGE: no progress for 6 beats (finished=357 busy=1 loading=1) -- dumping engine tid=8676
04:35:37.972  [STAGE] after-load url=https://dzen.ru/ rc=0 compositing=1
04:35:37.972  [STAGE] nav: ok  title=Дзен — главная новостная …
```

```
05:10:06.997  NavigateTo: url=https://dzen.ru/ pushHistory=1 loading=0
05:10:10.510  beat-stuck #1 busy=1 pending=0 finished=101 job=nav-load loading=1 inflight=0 fetchprog=-1 engineact=74
05:10:13.960  [STAGE] after-load url=https://dzen.ru/ rc=0 compositing=1
```

and the port's own record of the first one, which is unambiguous:

```
SL: fetch eff=https://dzen.ru/ redirects=3 cookies=11 size=3642761 http=200 crc=0
SL: hop[0] status=302 … hop[1] status=302 (errorCode=11300 invalid user) … hop[3] status=200
SL: cookies applied=4 hops=4 eff=https://dzen.ru/
SL: WebCoreSessionLoad exit rc=0
```

Two independent facts rule out a wedge: the port finished the job with `rc=0` a couple of seconds after
the dump, and its own fetch reported `http=200 crc=0` with a complete 4-hop chain. The engine thread was
never stuck; it was **parsing 3.6 MB of HTML and running the page's inline JS**, which is one job and
therefore invisible to the counter the watchdog was watching.

### 9b. Why — three separate reasons, and only the third is obvious

1. **`finished` counts jobs, not work.** `WebEngine::m_finished` increments once per completed job, and
   `job=nav-load` is a single job that contains the fetch, the parse, the inline JS, the layout and the
   first paint. A 3.6 MB page measured 6.9 s end to end on this bench, all of it inside that one job, so
   the counter *cannot* move during the only window the watchdog cares about. The port's `job=` label was
   already in the log, in every stuck beat, naming this.
2. **`fetchprog=-1` was not evidence either — it was a defect in the export, fixed in the same pass.**
   `WebCoreGetFetchProgress()` returns `-1` when no top-level fetch is in flight, and the harness reads
   `>= 0` as "in flight, engine legitimately blocked". Both counters (`progressDl`, `progressHops`)
   carried a `-1` sentinel, and the export folds them as `dl + (hops << 40)` — so before the first byte
   and before the first hop completed, the folded value was a large **negative** number, which the
   harness read as "no fetch in flight" *during a fetch*. The counters now start and reset at `0`,
   `-1` is reachable only through `inFlight == false`, and the contract is written into both ABI headers:
   **the sign is the interface.** Measured in §9d: `inflight=1 fetchprog=0`.
3. **One `addData` of a 3.6 MB document is an unlit window.** The port fed the whole body through
   `DocumentWriter::addData` in a single call, inside which the engine parses and runs inline JS with no
   RunLoop, no subresource delivery (nothing can be delivered while no loop runs) and no marker. Even a
   perfectly instrumented watchdog has nothing to observe there.

### 9c. The fix — ask the engine, then bound what cannot be asked

**`WebCoreGetEngineActivity()`, a new export** (`Src\port\WebCoreDriver.cpp`; declared in *both* ABI
headers, as always). A monotonic count, never negative, bumped where the engine thread visibly does
work: the load job's stage boundaries, **every `DocumentWriter` chunk** (the feed is now chunked at
256 KB — which is not a behaviour change, since WebKit's own `DocumentLoader` calls `addData` once per
network chunk), every settle tick of the load pump (`pumpLoop`'s 50 ms timer), and `WebCoreLiveTick`'s
step index, which is folded into the value rather than bumped separately because it already advances
once per operation inside the one job that can run for seconds at a time.

The harness then treats it exactly like `fetchprog`: a change against its own previous reading means the
engine is working, so `beat-stuck` does not accumulate at all. `engineact=` is printed on every
`beat-stuck` and `WEDGE` line.

**A suppressed dump is not allowed to be silent.** The old defect's signature was a stack dump; after the
fix the same run ends in *nothing*, and "nothing" cannot be told from "the defect is still there but the
timing moved". So the beats that only the gauge saved are counted and named once per six:

```
beat-gauge: job counter frozen for 6+ beats but engine activity moved (job=nav-load engineact=81) -- NOT a wedge, no dump
```

**The residual window gets a per-job threshold, each derived from a bound the port itself declares** —
not one number for all cases, because the same beat count means opposite things in different jobs:

| case | beats | why this number |
|---|---|---|
| a top-level fetch is in flight | 16 (~11.2 s) | just past the port's own 9.5 s fetch ceiling, after which the wait loop breaks and the engine returns |
| `job=nav-load` | 30 (~21 s) | the ceiling above + `pumpLoop`'s own 5 s watchdog + the tail (layout, link extraction, first paint); the measured healthy case is 6.9 s |
| anything else | 6 (~4.2 s) | unchanged from 2026-09-03; the live tick's steps advance the gauge, so 6 silent beats there means a *single* step lasted 4.2 s, which is the shape of the device's 2026-08-22 hang |

Six is deliberately **not** raised everywhere, and the reason it was picked in 2026-09-03 still holds in
the third row: the dump has to happen while there is still a process to dump. That reason is about the
**UI** thread, which is alive and beating in every case here (it is executing the tick that writes the
line) — which is exactly why the per-job rows above can afford to be longer.

### 9d. Measured, x64 bench, GPU on

The same stimulus that produced §9a — two navigations to dzen.ru, the second one the full 3.6 MB page.
Bench build `0.1.10.16` (the two-navigation session below); the positive control further down ran on
`0.1.10.17`, whose only difference from `.16` is the wording of the threshold comment in §9c:

```
05:13:02.843  NavigateTo: url=https://dzen.ru/ pushHistory=1 loading=0
05:13:02.921  beat-stuck #1 … job=nav-load loading=1 inflight=0 fetchprog=-1 engineact=53
05:13:06.522  beat-stuck #1 … job=nav-load loading=1 inflight=0 fetchprog=-1 engineact=75
05:13:08.670  beat-gauge: job counter frozen for 6+ beats but engine activity moved (job=nav-load engineact=81) -- NOT a wedge, no dump
05:13:09.667  [STAGE] after-load url=https://dzen.ru/ rc=0 compositing=1
05:13:09.667  [STAGE] nav: OK title=Дзен — главная новостная информационная платформа, …
```

`beat-stuck` reset twice with **no dump**, the gauge advanced 53 → 75 → 81 across the same 6.7 s window
that previously ended in a stack dump, and the page came up with the composition tree live
(`compositing=1`) and the real title.

**Positive control for the sign contract and for the in-flight row** — the endless stream this document's
earlier sections use for exactly this purpose (`ice1.somafm.com/groovesalad-128-mp3`, no
`Content-Length`, ~44 KB/s), i.e. a transfer that is alive and cannot finish:

```
05:15:38.018  [STAGE] before-load https://ice1.somafm.com/groovesalad-128-mp3
05:15:38.890  beat-stuck #1 busy=1 pending=0 finished=49 job=nav-load loading=1 inflight=1 fetchprog=0 engineact=30
05:15:46.099  [STAGE] after-load … rc=-16
ERR: phase=toplevel-fetch type=Curl curlcode=28 domain=curl desc=Operation timed out after 8037 milliseconds with 361117 bytes received
SL: fetch eff=https://ice1.somafm.com/groovesalad-128-mp3?_ic2=… redirects=1 cookies=0 size=361117 http=200 crc=28
```

Three things are confirmed at once: `inflight=1 fetchprog=0` is published **during** a fetch (the `-1`
sentinel is gone, and the contract is observable); **one** `beat-stuck` in an 8.08 s block, because the
byte counter moved every beat so the watchdog never accumulated — the in-flight row of §9c is a
backstop the gauge normally makes unreachable; and the transfer died of **curl's own** `TIMEOUT` with
curl's own reason attached, which is what §8c promised.

### 9e. What this closes, and what it deliberately does not

- **Closed:** the false `WEDGE` class on a healthy page, and with it the reason §7b's coupling kept
  producing dumps: `finished` is no longer asked a question it cannot answer. The two measured false
  positives both now leave an explicit `NOT a wedge` line instead.
- **Closed, and worth naming:** the `-1` sentinel in the export. It was live for one build
  (`0.1.10.14`), and it inverted the meaning of the very value §7b had just wired in.
- **NOT closed — the residual window is real.** A single engine operation that publishes nothing and
  runs longer than its row's number still dumps. The `nav-load` tail (layout → link extraction → first
  paint) has no bound in the port of its own; the gauge is what covers it, and on a phone 5× slower than
  this bench a 256 KB chunk or a single layout can plausibly take longer than the beat it sits in. If
  that happens the fix is to extend the gauge, **not** to raise the number: the dump would be wrong, and
  §7b's lesson is precisely that a number calibrated against something else stays calibrated to nothing.
- **Still not exercised:** `SL: fetch-silent` (unchanged from §8e) and §3's 9.5 s ceiling.
- **Found, not fixed, in the same pass:** `WebCorePortBumpLoad` — whose four counters the diag prints as
  `loads=S…/R…/C…/F…` — **has no callers anywhere in the tree.** No `WK_WINUWP` hook in
  `ResourceHandle.cpp` bumps it, and neither does any file in `Src\port`, so those four fields have been
  structurally zero for as long as they have existed. It was considered as the activity source and
  rejected on the evidence; the correct fix is to drive it from `Src\port\PortLoaderStrategy.cpp`, which
  is where the port owns the load hook, and that is a separate change. Until then, a diag line reading
  `loads=S0/R0/C0/F0` means "this field is dead", not "no resource was requested" — the live traffic is
  in `port-trace.txt` / `gpuinit-steps.txt` as `loader: CALL` / `serve started`.

### 9f. A false alarm met in the same pass: `CRASHVERDICT: CRASHED` after every install

Both measurement sessions in §9d began with

```
CRASHVERDICT: CRASHED last heartbeat=2026-09-19 05:12:19.278 … job=live-tick tickstep=10 stage=WE-job:lambda-done
```

and neither was a crash. **`RunCrashVerdict` decides "crashed" from one fact: `exit-ok.txt` is absent**,
and that file is written **only** by `App::OnSuspending` — a process killed outright never suspends, so it
never writes the marker. `Src\tools\update-local-x64.ps1` stops a running `Harness` with
`Stop-Process -Force` before installing, which is a kill, so the *next* launch always reports `CRASHED`
regardless of how the previous session actually ended. The heartbeat timestamp it prints is simply the
last beat of a session that was working.

How to tell this apart from a real crash, in the order that costs least:

1. **Is there a `LocalState\prelaunch-<MMdd-HHmm>` directory?** Only `update-local-x64.ps1` creates one,
   and it creates it in the same run as the `Stop-Process -Force`. Its presence at the timestamp of the
   reported death is the explanation.
2. **A dump or an exception record settles it in the other direction — but their absence settles
   nothing.** A `.dmp`, `unhandled.txt`, a `wedgedump*` or a WER entry (`Get-WinEvent -LogName
   Application`, provider `Application Error` / `Windows Error Reporting`, process `Harness.exe`) is
   positive evidence of a real fault. **Absence is not evidence of a kill:** this project has a whole
   documented death class that left *every* in-process channel silent at once (no dump, no WER event,
   no handler) — see `Doc/DZEN-SCROLL-DEATH.md` and the `_CALL_REPORTFAULT` fix that closed it in
   `0.1.9.105`. Ruling out a crash by "there was no dump" is the same mistake that document records.
   The decisive measurement, when it is worth the cost, is `cdb` attached externally for the exit code.
3. **Do not try to settle it from `crash-report.txt`.** `RunCrashVerdict` writes that copy whenever the
   marker is missing — which is both cases — so its existence proves nothing either way; it only says
   where the session was when it stopped. The discriminating evidence is (1) and (2).

The device line has the analogous trap and already documents it (`Doc/ARM32-BUILD-GUIDE.md`,
`deploy-launch.ps1`'s comments): launching over WDP with the screen off gets the app suspended and
reaped, which is an ordinary clean shutdown and prints `clean-exit`. **On this project the presence of a
`CRASHVERDICT` line is not evidence by itself; the verdict is a two-marker heuristic, and a forced kill
is indistinguishable from a fault to it.**
