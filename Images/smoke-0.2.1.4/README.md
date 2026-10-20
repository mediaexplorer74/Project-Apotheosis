# Smoke run — `ME.Apotheosis_0.2.1.4_x64`, 2026-10-03 06:14–06:16

These are the captures of the first complete run of the thirteen-site acceptance list. They are
checked in because a future session should be able to *see* what "13/13 PASS" looked like, not only
read the number.

- **Driver:** [Src/tools/run-smoke.ps1](../../Src/tools/run-smoke.ps1), unattended, one process, no
  restart between sites. Every navigation went through the `LocalState` `autocmd` channel, and the
  screenshot is a DPI-aware capture of the app window fitted to the screen.
- **Verdict:** `smoke.csv` in this folder, copied verbatim from
  `%USERPROFILE%\Documents\apotheosis-smoke`. `painted` is the **engine's** `diag: nonwhite=A/B`, not
  a screen-pixel count — the screen count describes the app's dark chrome and scored an identical
  99.68 % for every site before that was found.
- **Not one of these is proof of a clean browser.** They are proof that pixels reached the screen at
  this build, on this machine, with `gpudefault=0` (software present) and the mobile user agent.

`11-dzen-ru.png` is worth a second look: it is the site that, in an earlier run before the fixed-sleep
race was fixed, returned `rc=0` and painted nothing at all. Here it paints a full feed. The earlier
reading is not a description of this run — see [Doc/PLAN.md §0a](../../Doc/PLAN.md).

**Resolved 2026-10-14 — with one part still open, and this artifact is evidence for both.** The current
acceptance list starts the app fresh for each run, and its `11-dzen-ru` row blanks: `rc=0` with
`nonwhite=0.00`. The trace explains it. A `goto https://dzen.ru` there is answered with a redirect, the
engine performs the navigation itself (`loader: CALL type=0 main=1 …`, then `PBF: createHistoryItemTree`
/ `addItem`), and the document it lands on is `https://sso.dzen.ru/install?uuid=…` — which paints
`nonWhite=0`, a legitimately blank page. The same URL loaded a second time in the same process does not
redirect and paints 44 %. So the row's own note — `SPA + autologin redirect` — was right, and what was
missing was any field naming the document the engine had actually loaded: `after-load url=` and the
app's `_currentUrl` both carry the URL that was *requested*. `Src/tools/run-smoke.ps1` now records
`final_url` / `final_host` / `redirected` for exactly that.

**Why this run's own `11-dzen-ru` says `31,59` — and what that does and does not settle (2026-10-14).**
The engine's document on a first visit is `sso.dzen.ru/install`, which paints nothing; the *second*
visit in the same process is real dzen (3 MB, `redirects=3`). Two explanations fit `31,59`, and this
README cannot choose between them from the outside:

- the app's process had already visited `dzen.ru` before the run started — the run is one process with
  no restart, but nothing records what that process had browsed before the script was launched, and a
  manual visit is enough to make the script's own `goto` a second visit; or
- the `diag` was read at a moment that caught the **forward** — the list issues a Back and a Forward
  after every site, so dzen is reached a second time inside the run, which the text above notes this
  harness was known to mis-time.

Both require the same thing, and that is the part that is measured: the variable is state held **inside
the process**. It cannot be a warm-up that persists, because two later runs of the list — each a cold
process with the same `LocalState` — both blanked this row. The port's cookie jar is in memory:
`Src/port/PortNetworkStorageSession.cpp` creates it with `SessionID::generateEphemeralSessionID()` and
says in its own comment that cookies do not persist across restarts. See
[Doc/LESSONS.md](../../Doc/LESSONS.md), *`dzen.ru` is not a blank page, it is a redirect*.
