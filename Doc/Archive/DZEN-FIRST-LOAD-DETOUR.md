# dzen.ru's first load: the autologin detour that ends on an empty document

Measured 2026-09-19 on the x64 bench (`0.1.10.18`), GPU off, in one session. Everything below is read
from `log.txt` / `gpuinit-steps.txt`; the chain is quoted verbatim because the whole finding is a sequence.
The chain was then **reproduced** on `0.1.10.19` at 05:52 in a fresh session (GPU armed) — see §4; that
second run is also the one that exposed the probe-key defect, because the two dzen.ru loads of one session
ended on two different documents under a single requested url.

## 1. The two navigations, side by side

The same URL, `https://dzen.ru/`, navigated twice from `nav.txt` in the same app session, eight minutes
apart. The first attempt never rendered dzen; the second rendered it in full.

| | attempt 1, 05:39:32 | attempt 2, 05:42:44 |
|---|---|---|
| fetch chain | `redirects=1`, 2 882 bytes, ends on `sso.passport.yandex.ru/push?…&retpath=…dzen.ru%2F%3Fis_autologin_ya%3Dtrue` | `redirects=3`, 4 hops, `login.vk.com/?act=autologin` → `…autologin-vk?errorCode=11300&errorText=invalid+user` → `dzen.ru/` → 200 |
| engine's final document | `https://sso.dzen.ru/install?uuid=…` | `https://dzen.ru/` |
| `diag:` | `title=` (empty) `body=0 bodyKids=-1 nonwhite=0/1036944` | `title=Дзен — главная новостная…` `body=1 contents=1368x2121 nonwhite=547064/1036944` |
| harness | `empty page: notice shown … requested=https://dzen.ru/` | `after-load rc=0 compositing=1` |
| compositing | `[GPU] Composite -> kErrNoView: … root=0000000000000000` | `compositing=1`, root present |

**The difference is which autologin cookie the jar held**, not the site being unreachable: attempt 1 took
the Yandex SSO path, attempt 2 the VK one. Attempt 1 consumed it, so a retry works — which is exactly why
this is easy to miss and easy to blame on the site or the network.

## 2. Attempt 1, as a chain

```
SL: curl download url=https://dzen.ru/
SL: fetch eff=https://sso.passport.yandex.ru/push?uuid=…&retpath=…dzen.ru%2F%3Fis_autologin_ya%3Dtrue redirects=1 cookies=9 size=2882 http=200 crc=0
SL: hop[0] status=302 setcookie=3 loc=…
SL: hop[1] status=200 setcookie=6 loc=-
SL: cookies applied=9 hops=2
SL: DocumentWriter feed done
NAV: docloader create url=https://sso.dzen.ru/install?uuid=… substitute=0      <- the push page navigated itself
NAV: committedLoad bytes=2955   /   NAV: COMMIT
NAV: docloader create url=https://dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=… substitute=0
NAV: policy navigation url=https://dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=…
NAV: FAIL phase=didFailLoading type=Cancellation code=0 domain= url=
NAV: provisional started   /   NAV: start provisional
[JS] security/ERROR -:0:1 Refused to load https://dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=… because it does not appear in the form-action directive of the Content Security Policy.
NAV: FAIL phase=didFailProvisionalLoad type=Cancellation code=0 domain= url=
[GPU] Composite -> kErrNoView: g_gpuActive=1 … root=0000000000000000
```

Read it as three steps:

1. The **port's own** top-level fetch of `dzen.ru/` followed one redirect and landed on the Yandex
   autologin *push* page (2 882 bytes). That page was fed to the engine as the document.
2. **That page's** JavaScript navigated to `sso.dzen.ru/install?uuid=…` (2 955 bytes, `COMMIT`). From this
   moment the engine's document is a different origin from the URL the harness asked for.
3. The install page decided the SSO had failed — it sent the browser back to
   `dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=…` — and **its own Content Security Policy
   refused that navigation**, because the URL is not in the page's `form-action` directive. The install
   page stayed on screen, with no body and no root layer.

`sso_failed=blocked` is the SSO service's own verdict, set before the port saw anything: the failure is
upstream of the CSP refusal, and the CSP refusal is what *stranded* the browser instead of recovering.

## 3. Two things this is NOT — both checked, do not re-derive them

**It is not "the port cannot POST".** The whole SSO family is form-based, so that was the first
hypothesis. It is false, and the proof is a positive control rather than an inspection: a local page
(`formpost.html`) that submits `<form method="post" action="https://example.com/real-destination">` on load
produces

```
NAV: docloader create url=https://example.com/real-destination substitute=0
NAV: policy navigation … / provisional started / start provisional
NAV: main request url=https://example.com/real-destination redirect=0
NAV: policy response status=405 mime=text/html
NAV: committedLoad bytes=558   /   NAV: COMMIT
```

`405 Method Not Allowed`, where the **same URL answered `404` to a `GET`** earlier in the same session (the
tap test, `Doc/TAP-DISPATCH.md` §6). Same URL, two methods, two different answers: the method really is
transmitted, and the response really is committed. A main-frame form submission reaches the same
`LoaderStrategy::loadResource` hook a page-initiated navigation does (`type=0 main=1`), and its method and
body survive the trip. **This is also an MVP-shaped datapoint in its own right** — login and search forms
are the first thing a "modern page" needs.

**It is not a port CSP bug.** The refusal is upstream WebKit enforcing *the page's own* CSP, and it happens
where upstream puts it: `FrameLoader::submitForm` (`WebKit/Source/WebCore/loader/FrameLoader.cpp:593`)

```cpp
URL formAction = submission->action();
if (!document->checkedContentSecurityPolicy()->allowFormAction(formAction))
    return;                       // <- silent; the console message is emitted inside allowFormAction
```

i.e. the install document submitted a form to a host its own `form-action` does not permit. Every engine
blocks that. The port's role in the sequence is that its `LoaderStrategy` answered the earlier
page-initiated navigation with `PolicyAction::Use` and let it commit — which is what a browser is supposed
to do.

## 4. What it opens, and one consequence worth its own line

- **Open, and the root of the finding:** why the Yandex SSO path returns `sso_failed=blocked` for this
  client. It is the SSO's verdict, so the next step is to look at what the push page's JS sends and what
  the service answers — the port's own logging does not reach inside that page. Note the recovery path is
  *blocked by the site's CSP*, so the failure is not merely "an empty page": the browser is deliberately
  prevented from returning to dzen.ru and nothing retries.
- **Repeated, and therefore no longer a sighting.** The trigger was taken to be a cookie state (a Yandex
  autologin cookie in the jar) that attempt 1 consumes, and this port still exposes no way to seed or clear
  cookies from outside — so the run above was recorded as a sighting rather than an experiment. **On
  2026-09-19 at 05:52, on `0.1.10.19`, in a fresh session, the first load of `https://dzen.ru/` took the
  identical path**: `sso.passport.yandex.ru/push` → `sso.dzen.ru/install?uuid=3df3e0cb-…` (a different
  uuid), `body=0 nonwhite=0/1036944`, same `sso_failed` family. So the honest statement is now stronger
  than "when the Yandex SSO path is taken": **the first load of dzen.ru in a fresh session takes it**, and
  a retry of the same url renders the real page. What is still uncontrolled is *which* cookie decides it.
- **A consequence that is a defect in its own right, and not site-specific: the address bar lies about
  engine-initiated navigations.** At the moment the engine's document was `sso.dzen.ru/install`, the
  harness's URL box still showed `dzen.ru` — the requested URL. The root cause is structural and is the
  same fact `Doc/TAP-DISPATCH.md` §8 records for the tap gate: **`m_currentUrl` is assigned only in
  `NavigateTo`, `ApplyEngineFrame` and `SaveActiveTab`**, so a navigation the *page* starts (a push page's
  redirect, an SPA route change, a `history.replaceState`) never reaches it. `UA=desktop` was in effect,
  so this is not a UA-mode artefact. Fixing the *display* means deciding which history entry a
  page-initiated navigation updates, which is a change to the harness's history semantics and was
  deliberately not bundled with the tap-term fix; it is written down here so it is not re-derived from
  scratch.
- **That same staleness had a second, worse consequence, fixed on 2026-09-19 in `0.1.10.19`: it made the
  GPU unreachable on the bench.** The GPU auto-probe was gated on `m_gpuTriedForUrl != m_currentUrl` — the
  *requested* url — so when a load produced a different document than the one asked for, the probe's
  verdict attached to the requested string. Measured in one session: the first `dzen.ru` load ended on the
  SSO page, was correctly reported as un-presentable (`EnableCompositing=0 Composite=-4`), and that failure
  then suppressed the probe for the **real** dzen.ru eight minutes later, which has `compositing=1` and a
  root layer. Seven probes in that session, zero successes. The key is now the *document's* url, and the
  first real page presented through the GPU at 05:53:22. Full measurement: `Doc/GPU-LIVENESS.md` §7.
  This is the reason to treat the deficit above as more than cosmetic.
