# WebKit Upgrade: 2.52.4 → 2.53.4

> Research compiled June 28, 2026

## Version Info

| Release | Date | Type | Tag |
|---------|------|------|-----|
| webkitgtk-2.52.4 | June 2, 2026 | Stable | `webkitgtk-2.52.4` (commit `7acdf5e`) |
| webkitgtk-2.53.4 | June 23, 2026 | Development (→2.54) | — |

Source: https://webkitgtk.org/2026/06/23/webkitgtk2.53.4-released.html

## 2.53.4 Changelog

1. **Batched painting in Skia compositor** — performance improvement when many layers share a paint pass
2. **Avoid unnecessary clips in Skia compositor** — rendering optimization
3. **Fix sync issues between main, scrolling, compositing threads** — fixes glitches while scrolling (relevant: our GPU path uses TextureMapper, not Skia)
4. **Limit scrollbar damage region** — repaint optimization
5. **Add `image/webp` support to `canvas.toDataURL()`** — new API feature
6. **Expose search inputs as `WEBKIT_INPUT_PURPOSE_SEARCH`** — Gtk-specific
7. **User-Agent quirk for HBO Max** — web compat fix
8. **Several crash and rendering fixes** — general stability

## Porting Impact Assessment

For Apotheosis (WinUWP port with `WK_WINUWP` guards), the upgrade impact per change:

| Change | Impact on WinUWP | Effort |
|--------|-----------------|--------|
| Skia compositor batched painting | **None** — we use `USE_TEXTURE_MAPPER` (COORDINATED_GRAPHICS=0), not Skia | 0 |
| Skia compositor clip optimization | None | 0 |
| Thread sync for scrolling | **Potentially relevant** — need to check if our TextureMapper path shares code with the scrolling thread code | Low |
| Scrollbar damage region | Low risk — affects scrollbar painting code path | Low |
| `canvas.toDataURL()` webp | Low — gated behind ENABLE_WEBP, should be enum addition | Low |
| `WEBKIT_INPUT_PURPOSE_SEARCH` | **Gtk-specific** — not applicable | 0 |
| UA quirk HBO Max | Low — UA string addition, no integration risk | Low |
| Crash/rendering fixes | **Medium** — need to verify none of our `WK_WINUWP` guarded code is in the changed paths | Medium |

**Overall effort estimate: Low-Medium** (~2-3 days for careful merge + testing)

## Key Risks

1. **Thread synchronization changes**: The scrolling/compositing thread sync fix could touch code we've patched for App Container. Need to review the actual commit diff.
2. **Skia vs TextureMapper**: 2.53.4 focuses heavily on Skia compositor. Since Apotheosis uses TextureMapper, most rendering changes are irrelevant — which also means we miss the performance improvements.
3. **3-week gap**: Only 21 days of development between 2.52.4 and 2.53.4. Patch volume likely modest.

## Recommendation

**Proceed with upgrade at medium priority.** The changes are mostly Skia-focused (low impact on us) with some stability fixes. The real benefit comes from upgrading through the 2.54 stable release which may have more relevant changes.

Process:
1. Fetch the diff: `git diff webkitgtk-2.52.4..webkitgtk-2.53.4 > /tmp/upgrade.diff`
2. Apply to the WebKit source tree, resolve conflicts in `#if defined(WK_WINUWP)` guarded code
3. Rebuild `build-clang-gpu`, test on device
