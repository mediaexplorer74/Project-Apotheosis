# Project Apotheosis — Roadmap

## Sources this roadmap tracks

| Repo / branch | Role |
|---|---|
| `mediaexplorer74/Project-Apotheosis` @ `master` | Frozen mirror of upstream `master` — do not diverge here |
| `mediaexplorer74/Project-Apotheosis` @ `dev` | **Your active branch** — x64 build scaffolding, repo cleanup, localization |
| `Jimmyxiao2009/Project-Apotheosis` @ `gpu-path1` | Upstream's active line — GPU compositing, most recently the `v0.1.7.29` text-input/IME + image-load fixes |
| `WebKit/WebKit` @ `main` | Unpatched, non-UWP upstream — candidate rebase baseline (the "Win" port, ex-WinCairo) |

Two things need to happen to `dev` before it drifts further from `gpu-path1`:
merge/rebase the `v0.1.7.29` fixes in, and decide (Phase 2 below) whether to
keep patching webkitgtk-2.52.4 or start a rebase onto `WebKit/WebKit`'s Win port.

---

## Two tracks, one bridge

Everything here splits into two mostly-independent tracks that converge at
one shared component:

- **Engine Track** — patch reproducibility, x64 builds, device testing. Lives
  almost entirely in `port/` and the WebKit fork. Doesn't care what language
  the shell is written in.
- **Shell Track** — migrating the harness from C++/CX to C#. Lives entirely
  in the UI layer. Doesn't care whether the engine underneath is ARM32 or x64.
- **Convergence point: `Apotheosis.EngineWinRT`.** This bridge is useful to
  *both* tracks — it's the production interop layer for the C# shell on
  real hardware, and it's also a clean seam for a desktop C# "control panel"
  once the Engine Track's headless x64 build exists.

You can work these in either order, but the bridge should exist before the
C# shell does — building `MainPage` against a bridge that doesn't exist yet
just means writing it twice.

---

## Engine Track

### Phase 0 — Baseline (done)
WTF/JSC CLoop, Cairo software rendering, real input dispatch, JIT, ANGLE +
TextureMapper GPU compositing, smooth scroll/zoom — verified on a real Lumia
950. Nothing below should regress this.

### Phase 1 — WebKit patch reproducibility
The patch set against webkitgtk-2.52.4 currently lives only in a local
checkout + AI-memory prose notes — not in git history anywhere. Fix that
before writing more patches. **Full task breakdown:
`Project-Apotheosis-Phase1-3-Checklist.md`.**
- Exit criteria: a fresh clone reproduces a building patched WebKit tree with
  zero undocumented manual steps.

### Phase 2 — R&D spike: rebase onto WebKit's Win port?
Time-boxed comparison of continuing on webkitgtk-2.52.4 vs. rebasing onto
`WebKit/WebKit`'s own Win port (Cairo + libcurl + clang-cl, much closer to
your stack already). **Full task breakdown: same checklist file, Phase 2.**
- Exit criteria: a written go/no-go decision, backed by an actual stock build
  and a patch-by-patch comparison — not a guess.

### Phase 3 — Headless x64 engine build
Get WTF/JSC/WebCore/`port/` compiling for `x86_64-unknown-windows-msvc`, as a
plain console/desktop target first — no UWP packaging, no device deploy.
This is what breaks the "every test needs a flaky-WiFi Lumia" loop. **Full
task breakdown: same checklist file, Phase 3.**
- Exit criteria: a headless x64 executable loads a real page, runs JS, and
  dumps a rendered screenshot, entirely on the dev machine.

### Phase 4 — x64 UWP harness
Once Phase 3's engine runs headless, wrap it in a *second* UWP harness target
(x64, desktop-deployable) for full app-level testing — GPU panel, touch
simulated via mouse, no hardware required. This can be the existing C++/CX
harness retargeted, or (if the Shell Track has caught up by this point) the
new C# shell — see Convergence note above.
- Exit criteria: the full browser UI runs and is testable on the dev box.

### Phase 5 — Build matrix
ARM32 (ship) and x64 (dev/test) as a matched pair: same `port/` source, two
toolchain files, two vcpkg triplets, one parameterized build script. Stand up
CI (even a scheduled self-hosted runner) building x64 on every commit.
- Exit criteria: a single commit's regressions surface on x64 before you ever
  touch a Lumia.

### Phase 6 — Device matrix validation
Reserve real hardware for what only hardware proves: 950 for GPU/performance
ceiling, 640 for low-RAM/software-fallback behavior, 1200 for mid-range
coverage. Lean on the existing `auto-diag2.ps1` / `Deploy-Robust.ps1` tooling
rather than manual deploys.

---

## Shell Track

### Phase S1 — EngineWinRT bridge
Build `Apotheosis.EngineWinRT` (skeleton provided separately) as a thin
C++/WinRT wrapper over `WebCoreDriver.h`. Validate it **from the existing
C++/CX harness** first — swap its direct C-ABI calls for calls through the
bridge, confirm nothing regresses, before writing a single line of C# UI.
This isolates "did the bridge break something" from "did the UI rewrite
break something," which otherwise become impossible to tell apart.
- Exit criteria: C++/CX harness behaves identically when routed through
  `EngineSession` instead of calling `WebCoreDriver.h` directly.

### Phase S2 — C# shell, feature parity
Build out `Apotheosis.Shell` (skeleton provided separately): toolbar,
address bar, `EngineHost` control, navigation. Port 1:1 from the C++/CX
harness — no new features, no redesign, just the language change. Resist
the urge to "improve" the UI here; that's Phase S4.
- Exit criteria: address bar, back/forward/reload, and page rendering all
  work identically to the C++/CX harness, on a real Lumia.

### Phase S3 — Text-input / thread-marshaling regression pass
Specifically re-verify the `v0.1.7.29` fixes (focus + selection before IME
input; render loop ticking while images are pending) under the new C# shell.
Thread-marshaling bugs cluster exactly around input-focus and async-repaint
code paths — the two things that release just fixed — so this is the most
likely place for a silent regression to hide.
- Exit criteria: typing in a page's text fields and image loading both work
  reliably on-device, matching `gpu-path1`'s current behavior.

### Phase S4 — Retire the C++/CX harness
Once S2/S3 are solid, drop the old harness project. From here, all shell
work — favorites, history, downloads, tabs, the "museum tile" visual
identity — happens in C# only.

### Phase S5 — (Optional, opportunistic) Desktop dev shell
If Engine Track Phase 4 lands, `EngineWinRT` is a standard WinRT component
and is equally consumable from a desktop-targeted C# UWP project, or a
Xamarin.Forms/UNO custom renderer on a UWP head. Useful as a dev-side
control panel reusing the same C# skills; not a path to iOS/Android, since
the engine itself is Windows/ARM32/x64-only.

---

## Later, lower-priority

- **Public naming**: "Harness" (engine layer) vs. "Apotheosis" (shell) vs.
  whatever the eventual public-facing name is — worth deciding once S4 lands,
  not before; naming things before they're stable just creates churn.
- **Visual identity**: the Metro/tile aesthetic already used for
  community-facing decks could extend into the actual shell chrome once S2
  is done and there's a stable UI to skin.
- **Multi-tab support**: `EngineHost` was written to make this structurally
  possible (one control = one engine session), but it's not attempted until
  single-tab is rock solid.

## Open risks carried over from prior research

- Whether the App Container `codeGeneration` capability trick for ARM32 JIT
  behaves identically on x64 desktop UWP (Phase 4/Engine Track).
- Whether `x64-uwp`'s vcpkg triplet actually produces App-Container-safe
  binaries or just plain x64 desktop ones (matters once Phase 4 moves past
  the headless harness).
- How much of the "which `WK_WINUWP` guards are ARM32-specific vs.
  UWP-specific" data (surfaces during Phase 3) is salvageable if Phase 2
  picks a rebase.
