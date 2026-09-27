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

- x64 Release harness builds and runs.
- `ya.ru` and the local encoding probe are known x64 smoke tests.
- Automatic GPU direct-present is currently disabled in the local bench settings after a presentation
  failure; software presentation is the working baseline.
- Device validation is still required before calling v1 complete.

## Record a result

For each site record: URL, load result, painted pixels/content, scroll result, back result, and crash or
hang marker. A test that only checks `rc=0` is incomplete.

---

## Status, reviewed 2026-09-27

The contract above is unchanged. This section records what has actually been demonstrated, so that
"done" means demonstrated rather than intended. Evidence is `LocalState` files and return codes, per
the rule in [INDEX.md](INDEX.md); a screenshot counts only as proof that pixels reached the screen.

| # | Requirement | x64 | Lumia 950 | Evidence |
|---|---|---|---|---|
| 1 | starts and reaches the UI | **met** | not run | appx installs and launches, `stage.txt` shows `after-load rc=0` |
| 2 | address bar, navigate on Enter | **met** | not run | navigation via `testurl.txt` and the `goto` command |
| 3 | ten sites in sequence | **not met** | not run | only `ya.ru` and the encoding probe have been driven. The scripted controller is in place to run the list; the list itself has not been run |
| 4 | paints visible content | **met** | not run | screenshots: text, icons, avatars, product photos, video thumbnails, correct Cyrillic |
| 5 | back returns to previous page | **not verified** | not run | no scripted back navigation has been driven yet |
| 6 | scrolling moves a tall page | **met** | not run | 4800 px of driven touch-path scrolling, cost flat across the drag, content correct at depth |
| 7 | history and suggestions | **not verified** | not run | out of the current scripted path |
| 8 | failure reported, not a white page | **not verified** | not run | needs a deliberately bad URL through the controller |
| 9 | same list on x64 then device | **partial** | not started | x64 line exists; the device line has not been run for the C# app |
| 10 | usable after the list | **partial** | not run | process stayed alive through a 200-step drag, 204 MB working set, but the full list has not been run |

### Are we near 1.0?

**No, and the reason is worth stating plainly: 1.0 is gated on device validation, not on x64
progress.** Seven of the ten requirements are either met on x64 or unverified; none of them has been
demonstrated on the phone. Requirement 9 is the phase exit criterion in [INDEX.md](INDEX.md), and it has
not started.

The x64 software-rendering line is genuinely solid: painting is correct, scrolling is flat under
drive, and the frame path is instrumented. What is missing is not code quality, it is the
transfer of that result to ARM32 under an App Container, where the ABI, the JIT and the font stack
each differ -- and where every previous line of this project has already produced a separate bug.

A credible v1.0 therefore needs, in order:

1. the ten-site list run on x64 with per-site records, closing requirements 3, 5, 7 and 8;
2. the same list on the Lumia 950, closing requirement 9;
3. a pass/fail decision on the remaining cosmetic Cyrillic fallback artifacts.

GPU presentation is explicitly *not* on this list, and that is a deliberate scope decision rather than
an omission: software presentation is the MVP-safe path, and `WebCoreGpuInit rc=-20` remains an open
investigation tracked in [GPU-LIVENESS.md](GPU-LIVENESS.md).

