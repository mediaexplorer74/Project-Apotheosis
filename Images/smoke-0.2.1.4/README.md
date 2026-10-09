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
