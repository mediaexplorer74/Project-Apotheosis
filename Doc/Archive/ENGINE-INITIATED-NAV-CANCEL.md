# Why an ordinary in-page link paints a blank page

**Status: root-caused 2026-09-19 (appx 0.1.10.32). The fix is not written — §8 is the proposed direction,
not a change that exists.** Read §3 before touching the top-level load path: the mechanism is one line of
the port's own trace (`SL: fetch eff=`) and it inverts the obvious guess.

## 1. The symptom, and why it is the MVP's blocker

A tap on a dzen.ru news card loads a **completely blank white page**. Not an error page, not a partial
render — the frame is uniformly `#FFFFFF` and `diag:` reports `body=0`, `contents=720x1080` (nothing),
`rs=I/p0/ig0/pr0/st0/le0` (the load event never fired). The BMP dumped by the autodiag path is
`Src\tools\shots\shot_0_720x1080.png`: 720x1080 of pure white.

This is the phenomenon the maintainer reported as "страница только дёрнулась", and §8d of
`Doc/TAP-DISPATCH.md` recorded that it had **never been reproduced by script** — every scripted tap that
reached the decisive term had the engine navigate successfully. It is now reproduced deterministically,
in isolation, with no tap involved at all.

## 2. The reproduction — no gesture needed

The autodiag channel (`LocalState\autodiag.txt`, one URL per line) loads URLs through
`WebCoreSessionLoad` on the offscreen GPU path and writes `autodump.txt` plus one BMP per URL. Seeding it
with the exact URL the tap targeted reproduces the blank page on every run:

```
https://dzen.ru/news/story/05be4c56-e058-5a7d-a8b8-d63f8e8c15e5?from=main_portal&lang=ru&persistent_id=3425361894&story=0399f114-a5d0-561c-9f17-f7cdfa29f909&t=7506950399966842880
```

`autodump.txt` for it, in full:

```
WebCoreGpuInit(offscreen) rc=0
SessionLoad rc=0
diag: url=https://sso.dzen.ru/install?uuid=896ea643-... title= contents=720x1080 body=0 nonwhite=0/777600
      loads=S0/R0/C0/F0 pending=0 js=1/1 vua=1 sheets=1 scripts=1 rootKids=-1 bodyKids=-1
      rs=I/p0/ig0/pr0/st0/le0 scr=defer=0 err=0 blk=0[] spa=[]
      lasterr=[phase=didFailProvisionalLoad type=Cancellation curlcode=0 domain= desc= url=]
      res:[install?uuid=896ea643-...(s2) log?uuid=896ea643-...(s3) ]
```

The second URL in the same run, `https://news.ycombinator.com`, is the control and it is the opposite
result in every field: `nonwhite=777518/777600` (99.99 % of the surface painted), `contents=720x1513`,
`bodyKids=2`, `le1`, all five subresources `s2`, and `setScrollPosition(0,300)` lands exactly
(`scrollPos=0,300 (expected 0,300)`). So the engine, the GPU readback, the loader, text and scroll all
work on a real modern page; the blank is specific to what happens under the dzen URL.

## 3. The chain, verbatim (`gpuinit-steps.txt`, URL 1 of the autodiag run)

Line numbers are from that file and are stable for this run only; the *order* is the evidence.

```
 11  SL: WebCoreSessionLoad enter url=https://dzen.ru/news/story/05be4c56-...
 14  SL: WebCoreSessionLoad calling teardownSession          <- teardown of the PREVIOUS session
 16  SL: WebCoreSessionLoad calling buildSession
 25  NAV: docloader create url= substitute=0                  <- the blank bootstrap document
 27  SL: curl download url=https://dzen.ru/news/story/05be4c56-...
 28  SL: fetch eff=https://sso.passport.yandex.ru/push?uuid=d5e9eb7a-...&retpath=https%3A%2F%2Fdzen.ru%2Fnews%2Fstory%2F05be4c56-...&is_autologin_ya=true
         redirects=1 cookies=9 size=3055 http=200 crc=0       <-- THE LINE THAT INVERTS THE GUESS
 29  SL: hop[0] status=302 setcookie=3 loc=https://sso.passport.yandex.ru/push?...
 30  SL: hop[1] status=200 setcookie=6 loc=-
 33  SL: DocumentWriter feed                                   <- 3055 bytes of the SSO page fed by hand
 35  NAV: cancelPolicyCheck -- a policy decision in flight was abandoned
 36  SL: DocumentWriter feed done
 37  NAV: docloader create url=https://sso.dzen.ru/install?uuid=d5e9eb7a-...   <- that page's JS
 40  NAV: policy navigation url=https://sso.dzen.ru/install?uuid=d5e9eb7a-...
 43  NAV: policy response status=200 mime=text/html url=https://sso.dzen.ru/install?uuid=...
 45  NAV: committedLoad bytes=3471
 46  NAV: COMMIT
 48  NAV: finishedLoading
 49  NAV: finish document load
 55  NAV: docloader create url=https://dzen.ru/news/story/05be4c56-...   <- that page's JS bounces BACK
 56  NAV: cancelPolicyCheck -- a policy decision in flight was abandoned
 58  NAV: cancelPolicyCheck -- a policy decision in flight was abandoned
 59  NAV: cancelPolicyCheck -- a policy decision in flight was abandoned
 60  NAV: FAIL phase=didFailLoading type=Cancellation code=0 domain= url=
 61  NAV: provisional started
 62  NAV: start provisional
 68  NAV: FAIL phase=didFailProvisionalLoad type=Cancellation code=0 domain= url=
 70  SL: pumpLoop done                                         <- all of the above is INSIDE buildSession
 74  SL: WebCoreSessionLoad exit rc=0
 75  SL: WebCoreSessionLoad enter url=https://news.ycombinator.com
 78  SL: WebCoreSessionLoad calling teardownSession
 79  loader: MARK teardown enter gen=0 started=2 inflight=0 pending=0 hosts=0
 80  NAV: cancelPolicyCheck -- a policy decision in flight was abandoned
 81  loader: MARK teardown after stopAllLoaders gen=0 started=2 inflight=0 pending=0 hosts=0
```

Two things this ordering settles outright:

- **The kill is inside URL 1's own load, not in URL 2's teardown.** Lines 56-68 sit between
  `DocumentWriter feed` and `pumpLoop done`, i.e. inside `buildSession`. The teardown at lines 78-83 is
  the *next* navigation's and its own `cancelPolicyCheck` (line 80, emitted synchronously inside
  `stopAllLoaders`) abandons nothing that mattered. A first hypothesis — "the next `WebCoreSessionLoad`
  tears down the session and kills a navigation still in flight" — is therefore **falsified**, and the
  `teardown PROVISIONAL-IN-FLIGHT` mark added in `0.1.10.32` did not fire, because by then there was no
  provisional loader to lose.
- **No request for the article ever left the process.** There is no `loader: CALL` line for
  `dzen.ru/news/story/...` anywhere in `port-trace.txt` — only for `sso.dzen.ru/install` and
  `sso.dzen.ru/log`. The article URL is reached, cancelled at the policy check, and never fetched.

## 4. The mechanism: the port feeds the document by hand while WebKit's own loader is live

The port does not serve the top-level document through its `LoaderStrategy`. `WebCoreSessionLoad` fetches
it with curl on a worker thread and then pushes the bytes into the parser itself:

```
SL: curl download url=...        <- the port's own transfer, redirects followed by curl
SL: fetch eff=...                <- where that transfer actually ended up
SL: DocumentWriter feed          <- the body is handed to DocumentWriter directly
```

Everything else on the page — every `<script>`, image, stylesheet — goes through
`PortLoaderStrategy::loadResource` instead, the real path, which on the HN control served five
subresources cleanly. So **two mechanisms can own the document at the same time**: the hand-fed
`DocumentWriter` and WebKit's `DocumentLoader`/provisional machinery. They do not agree, and the
hand-feeding wins by cancelling the other:

`NAV: cancelPolicyCheck` fires *between* `DocumentWriter feed` (line 33) and `DocumentWriter feed done`
(line 36). That is WebKit abandoning a policy check it had started, because the document is being
replaced under it by the hand-fed bytes. The subsequent engine-initiated navigations — the SSO page's
`location` assignments — are then racing a document whose ownership has already been taken away, and they
die the same way, with the platform's bare cancellation:

```
NAV: FAIL phase=didFailProvisionalLoad type=Cancellation code=0 domain= url=
```

The empty `domain` and `url` are diagnostic, not noise. **This port's own error factories cannot produce
that error**: `cancelledError`, `blockedError`, `cannotShowURLError` and `interruptedForPolicyChangeError`
in `PortPlatformStrategies.cpp:608-612`, and `blockedError` in `PortNetworkStorageSession.cpp:78`, all
`return { }`, i.e. a `Null`-typed error, and the only `Type::Cancellation` the fork constructs on purpose
(`LoadingFrameLoaderClient.cpp:140`) carries `domain=WebKitInternal` and a description. An error with
type `Cancellation` and **nothing else** comes from WebCore's platform-agnostic cancellation — the same
value `ResourceErrorBase::isNull()` does *not* test for, which is why the failure looks like a real one
here while the port's own no-op failures look like silence. `Doc/CLAUDE.md`'s rule about `lasterr=`
applies with the type read correctly: `type=Cancellation` means **the load was stopped by the engine**,
and the only question is who stopped it.

## 5. Why `SL: fetch eff=` is the line to read first

`SL: fetch eff=` is the URL the port's transfer **ended** on after redirects, and on this URL it is
`sso.passport.yandex.ru/push?...&retpath=<the article>&is_autologin_ya=true` — Yandex's auto-login push
page, not the article. The port followed the 302 chain (`hop[0] status=302`, `hop[1] status=200`) and then
fed the **last hop's body** to the document as if it were the requested page.

That is the wrong body to render, and it is also why the page then bounces: the push page is an
interstitial whose script immediately sets `location` — first to `sso.dzen.ru/install`, then back to the
article. The article's second attempt is the one that dies. So the blank page is not "dzen is blocking
us": it is **the port rendering an interstitial as a document and then abandoning the interstitial's own
redirect**, with the abandonment caused by the hand-fed DocumentWriter §4 describes. `SL: hop[N]` lines
carry the whole chain (`status`, `setcookie` count, `loc`), so a future report can be settled from them
without a rerun.

## 6. What this closes, and what it does not

- **Closes** §8d of `Doc/TAP-DISPATCH.md`: the reported symptom now has a scriptable reproduction and a
  named cause. The tap dispatcher is **innocent** and needs no change — on the real tap at 08:44:25 it
  saw `refused=-1 navEmpty=0 linkHit=1`, and `refused=-1` on a tap that navigated is exactly the
  documented "the document was replaced before the readback" (§8c), not a swallowed tap. The
  `harness-tap-dispatcher-swallows-spa-taps` conclusion drawn earlier from that run is **wrong** and has
  been corrected.
- **Closes** the shape of the `dzen-blank-is-not-a-lost-load` memory: it recorded "the document reports a
  URL that was never fetched, so the failure is engine-initiated navigation, not the network". Confirmed,
  and the mechanism is now §4.
- **Does NOT close** the encoding defect seen in the same HN screenshot (§7) — a separate bug.
- **Does NOT close** `sso.dzen.ru/log?...` (`s3`, LoadError) — one SSO subresource fails; with everything
  else about that page blank it is not yet worth chasing.

## 8. The direction of the fix (proposed, not implemented)

The honest fix is to stop hand-feeding the top-level document and let **one** mechanism own it. The port
already has the mechanism: `PortLoaderStrategy::loadResource` serves every subresource with real curl and
real TLS, and `CachedResource::load()` reaches it for `MainResource` too —
`DocumentLoader::loadMainResource()` arrives through `CachedResourceLoader::requestMainResource()`, and
upstream's `WebResourceLoadScheduler` answers every type with `SubresourceLoader::create` (this is the
`loader: CALL type=0 main=1` path, already traced). If the main resource went through the same loader as
everything else, WebKit would own the document, a redirect chain would be WebKit's own business, and an
interstitial's `location` assignment would be an ordinary navigation instead of a race against a
`DocumentWriter` that is writing into the same frame.

What that costs, and why it is not a one-line change:

- `WebCoreSessionLoad` is a **synchronous** C ABI call that returns a painted frame. The loader path is
  asynchronous, so the driver's pump has to become the thing that waits for the real loader rather than
  for a worker thread it started itself.
- The 2 500 ms top-level budget, the 8 s worker ceiling and the `SL: DISCARDED` behaviour
  (`Doc/TOPLEVEL-FETCH-BUDGET.md`) all live in that worker. Moving the fetch into the loader moves the
  budget question with it — and §5 of that document already says the budget should become
  progress-aware, so the two changes want to be designed together.
- `SL: fetch eff=`, `SL: hop[N]`, `size=`, `http=`, `crc=`, `cookies=`, `redirects=` are the loader
  diagnostics the dzen work has relied on all week. They must keep being emitted from wherever the fetch
  ends up, or a whole class of future questions becomes unanswerable. The `loader:` channel and the `SL:`
  channel would merge.

A cheaper intermediate exists and is worth measuring first: **do not feed the body when the transfer ended
on a different origin than the one requested** (`SL: fetch eff=` host ≠ requested host, or `redirects>0`
crossing a host boundary). That single guard would leave the interstitial to the engine — the engine
already fetched and committed `sso.dzen.ru/install` correctly through its own loader on lines 40-49, which
is the proof that the engine's own path works when it is allowed to run. It is a mitigation, not the fix,
and it should be documented as such if it is taken.

## 9. What is instrumented now, and how to read it

Added in `0.1.10.32`, both cheap and both deliberate:

| marker | where | what it answers |
|---|---|---|
| `NAV: cancelPolicyCheck -- a policy decision in flight was abandoned` | `LoadingFrameLoaderClient::cancelPolicyCheck`, previously an empty override | *when* a navigation died at the policy gate. Its position relative to `DocumentWriter feed` / `stopAllLoaders` names the culprit — in the run above it appears between the two `teardown` marks at line 80, proving `stopAllLoaders()` emits it synchronously |
| `loader: MARK teardown PROVISIONAL-IN-FLIGHT (stopAllLoaders is about to cancel it)` | `teardownSession`, before `stopAllLoaders()` | whether a teardown had a provisional loader to lose. It did **not** fire on this bug, which is what falsified §3's first hypothesis |

Both are one line each and neither changes behaviour. Reading them together: a `cancelPolicyCheck`
**between** `teardown enter` and `teardown after stopAllLoaders` is a teardown killing a navigation; a
`cancelPolicyCheck` anywhere else is WebKit or the document-feed path doing it, and §4 is where to look.
`provisionalDocumentLoader()` is inline in `FrameLoader.h`, so the driver's check needs no export —
worth knowing before reaching for `Frame::page()`-style accessors that do not exist
(`Doc/UNKNOWN-EXPORTS.md`).

## 10. The harness bug fixed in the same session

The first autodiag run on this URL did not produce a blank page — it produced a **hard access violation
on startup**, before any navigation:

```
VEH: code=0xC0000005 addr=00007FF69428C220 tid=10912 op=0 faultaddr=00000082183FF4E2
symbolised: WriteBmp32 MainPage.xaml.cpp:1635  <- autodiag lambda MainPage.xaml.cpp:2407
            <- WebEngine::loop
```

`kW`/`kH` are mutable globals (`MainPage.xaml.cpp:1654`, defaulting to 720x1080) that
`ApplyViewportSize` rewrites **on the UI thread** (`kW = ew; kH = eh;`, the `m_sessionActive==0` branch at
line 3456) while engine-thread jobs read them. The autodiag job allocated its frame buffer at 720x1080,
`ApplyViewportSize` then set 1368x758, and the dump used the new values: `WriteBmp32` starts at
`rgba + (h-1)*w*4` = ~1 MB **past the end** of the 3.1 MB buffer, so the first pixel read faulted. It wrote
16384 bytes (one filesystem block) before dying, which is why a truncated `shot_0.bmp` exists in
`prelaunch-0919-0852`.

Three fixes, all in `MainPage.xaml.cpp`:

1. the autodiag job snapshots `aw`/`ah` once and uses them for the allocation, every
   `WebCoreSessionLoad` and the dump, and puts the size in the file name
   (`shot_<idx>_<w>x<h>.bmp`) so the artifact states the size it was taken at;
2. the `about:home` render path (`WebCoreRenderHtml`) had the **same** defect — its UI-thread lambda
   `memcpy`'d `kW*kH*4` at blit time — and now carries its own `hw`/`hh` snapshot into the lambda;
3. `WriteBmp32` gained a null/size guard and a comment saying it takes a raw pointer and therefore
   **cannot** check its buffer, unlike `BlitToBitmap` which drops a stale frame when
   `rgba.size() != W*H*4` (line 1601). That guard is why the ordinary paint paths survive a resize and
   this one did not.

**Latent on the phone**, where the engine size never leaves the Lumia's 720x1080 — which is exactly why it
survived to be found by the first x64 autodiag run. Same shape as the `CONTEXT`-members episode in
`CLAUDE.md`: a file that has not been exercised on an architecture is not a file that works on it.



