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
