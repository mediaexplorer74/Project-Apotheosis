# Project Apotheosis — Phase 1–3 Task Tracker

Scope: turns the "no patched WebKit committed anywhere" and "no x64" problems into trackable work. Covers Phase 1 (patch reproducibility), Phase 2 (R&D spike: rebase onto WebKit's "Win" port), and Phase 3 (x64 engine build, headless/decoupled from UWP).

How to use: check items off as you go (`[x]`). Each phase has an explicit **exit criteria** block — don't start the next phase until those are true, even if it's tempting to jump ahead.


## 0. Current status — Sep 17, 2026

> Historical report follows this section; its earlier scope and phase gates are not today's priorities.
> See [STATUS-2026-09-18.md](STATUS-2026-09-18.md) for the current state (x64 0.1.9.93 deployed; GPU readback shows
> ignored `makeCurrent=0`/NULL GL context; later `0xc0000005` at WebCore+0x204901f, symbolized to
> a CSS StyleCalculationTree std::variant access — location, not proven cause; no crash dump from
> the original run; plan: fail-closed GPU errors, live dumps, independent rendering/JS tests, ARM
> later, Git optional). Git history remains an open item below, and is **optional** per the owner.

---

## Phase 0 — Baseline (reference only, do not regress)

These are already verified working on real Lumia 950 hardware (ARM32, Win10M 15254) per the `dev`/`gpu-path1` branches. Listed here so Phase 1–3 work has a regression floor to test against.

- [x] WTF + JavaScriptCore CLoop running on-device
- [x] WebCore + Cairo software rendering of real sites (Bing/GitHub/Apple/Microsoft)
- [x] Persistent interactive session — real touch/click/form/link dispatch, scroll-triggered lazy load, OSK input
- [x] JSC JIT working under App Container (`codeGeneration` capability)
- [x] ANGLE (D3D11 FL9_3) + WebCore TextureMapper compositing direct to `SwapChainPanel`
- [x] Smooth scroll / pinch-zoom (direct-present fast scroll + live zoom transform + re-rasterize at new scale)

---

## Phase 1 — Make the WebKit patch set a first-class, reproducible artifact

**Problem this solves:** the `WK_WINUWP`-guarded patches against webkitgtk-2.52.4 currently exist only as a local checkout + prose notes in AI "project memory" files (`MEMORY.md`, `WEBKIT-UPGRADE.md`). Nothing in `git log` proves what was changed or why. This is the single biggest risk to the whole project right now.

### 1.1 Inventory the current patch set
- [ ] Diff your local patched webkitgtk-2.52.4 tree against a clean upstream checkout of the same version (`git diff` or `diff -ruN` if it's not already a git repo)
- [ ] Bucket the diff by subsystem: build system (CMake/toolchain), ARM32/thumbv7 codegen, App Container sandbox stubs (crypto/network/pasteboard/AX/loader), JIT executable-memory handling, ANGLE/TextureMapper wiring, TLS/cert handling (`WebCoreSetCACertPath`), other
- [ ] Cross-reference the bucketed diff against whatever is currently written in `MEMORY.md` / `WEBKIT-UPGRADE.md` — flag any patch in the tree that has **no** corresponding note, and any note that has **no** corresponding patch (both are bugs)
- [ ] Produce a single `PATCH-INVENTORY.md` listing every changed file, which bucket it's in, and a one-line rationale (even a rough one) before moving to 1.2

### 1.2 Stand up a real fork as the patch's home
- [ ] Decide the base: webkitgtk-2.52.4 (current) vs. waiting for the Phase 2 spike to possibly switch base — **recommendation: do 1.2–1.3 on the current webkitgtk base now, don't block this phase on Phase 2's outcome**, since Phase 2 is a separate decision that can land later as a rebase
- [ ] Create `mediaexplorer74/webkit-w10m` (or similar) as a real fork/mirror, pinned to the exact upstream commit/tag your patches are based on
- [ ] Confirm the fork builds clean (unpatched) before applying anything, so you have a known-good baseline commit to diff against forever
- [ ] Add a `WK_WINUWP-baseline` tag at that clean commit

### 1.3 Replay patches as real, reviewable git history
- [ ] Apply patches from each Phase 1.1 bucket as **separate commits** (not one giant squash) — build system, then ARM32 codegen, then App Container stubs, then JIT, then ANGLE/TextureMapper, then TLS, then misc
- [ ] Write a real commit message per commit: what changed, why, what broke without it (pull the "root cause / trial-and-error" detail out of `MEMORY.md` and put it where git blame can find it)
- [ ] Verify each commit builds in isolation where feasible (or at least that the cumulative tree builds after each bucket) — this catches hidden inter-patch dependencies early
- [ ] Tag the final commit `WK_WINUWP-v0.1.8.5` (or match your current Apotheosis version) so Project-Apotheosis can pin to it

### 1.4 Wire the fork into Project-Apotheosis as a real dependency
- [ ] Decide submodule vs. fetched-tarball-in-build-script (submodule is simpler for a GB-scale tree if you're already excluding it from the main repo; a build-script fetch keeps clone size down)
- [ ] Update `port/configure-*.ps1` to fetch/checkout the pinned tag from the new fork instead of assuming a pre-existing local `E:\Apotheosis\WebKit` checkout
- [ ] Remove (or clearly mark as legacy/superseded) the prose patch-list responsibility from `MEMORY.md` — it should point to the fork + `PATCH-INVENTORY.md`, not duplicate it
- [ ] Smoke-test: clone Project-Apotheosis to a **fresh machine/directory with nothing pre-staged**, run the documented build steps, confirm WebCore actually builds using only what's in git — this is the real test of "reproducible"

### 1.5 Documentation cleanup
- [ ] Rewrite `WEBKIT-UPGRADE.md` as an index pointing at the fork's commit history, not a restatement of it
- [ ] Update `AGENTS.md` / `CLAUDE.md` "项目记忆" section to reflect the new source of truth
- [ ] Add a short `CONTRIBUTING.md` note (even to yourself, for future-you) on the `WK_WINUWP:` comment convention and the "guard everything in `#if defined(WK_WINUWP)`" rule

**Phase 1 exit criteria:**
- [ ] A fresh clone + documented steps reproduces a building patched WebKit tree with zero manual/undocumented steps
- [ ] Every `WK_WINUWP` patch has a traceable commit with a real message
- [ ] No patch knowledge exists *only* in AI memory files

---

## Phase 2 — R&D spike: is WebKit's "Win" port a better baseline than webkitgtk?

**Time-box this to a few days.** The goal is a go/no-go decision, not a finished rebase. WebKit's own "Win" port (formerly "WinCairo," renamed 2023) already uses Cairo for graphics and libcurl for networking and builds with clang-cl — much closer to your stack than the GTK/Linux-oriented webkitgtk tree you're currently patching.

### 2.1 Build the stock Win port as a control
- [ ] On an x64 Windows dev box, clone `https://github.com/WebKit/WebKit` (main)
- [ ] Follow `https://docs.webkit.org/Ports/WindowsPort.html` to install deps (CMake, Perl, Python 3.11, Ruby, gperf, LLVM, Ninja — via choco or winget) and build with `perl Tools/Scripts/build-webkit --release`
- [ ] Confirm `MiniBrowser.exe` runs and renders a real page — this is your unmodified baseline
- [ ] Note the build time and toolchain quirks (clang-cl version, vcvars setup) for comparison against your current ARM32 toolchain pain points

### 2.2 Compare patch surface area
- [ ] Walk through your Phase 1.1 patch buckets one at a time and ask, for each: "does the Win port's CMake/platform layer already do something equivalent, or would I still need this exact patch?" — focus especially on build-system and Cairo-backend patches, since those are most likely to already be solved upstream
- [ ] Specifically check: does `Source/cmake/OptionsWin.cmake` (the renamed `OptionsWinCairo.cmake`) already parameterize things your hand-patched webkitgtk build system currently hardcodes?
- [ ] Spot-check whether the Win port's ANGLE/TextureMapper compositing path is closer to what you built for GPU compositing than webkitgtk's was (this is the highest-value comparison — compositing was clearly the hardest part of your existing port)
- [ ] Produce a rough effort estimate: "X of N patch buckets would shrink/disappear, Y would stay the same, Z would get harder" — written down, not just a gut feeling

### 2.3 Decision point
- [ ] If the Win port meaningfully shrinks the ARM32/App-Container patch surface → schedule a rebase: redo Phase 1.2–1.3 against `WebKit/WebKit` main instead of webkitgtk-2.52.4, porting the ARM32/UWP-specific deltas onto the new base
- [ ] If not (e.g. Win port's x64-only assumptions are baked in deeply enough that ARM32 retrofitting is just as much work) → stay on webkitgtk-2.52.4, but keep this spike's findings in `PATCH-INVENTORY.md` as a documented "why not" for future reference
- [ ] Either way: write the decision down with the reasoning — this is exactly the kind of thing that otherwise ends up undocumented and re-litigated later

**Phase 2 exit criteria:**
- [ ] A written go/no-go decision on rebasing, backed by an actual stock-Win-port build and a patch-by-patch comparison (not speculation)

---

## Phase 3 — x64 engine build, decoupled from UWP

**Problem this solves:** all current engine validation requires a Lumia over flaky WiFi. x64 lets you iterate on the dev machine directly. Note this is for **fast local testing**, not shipping — the Lumia hardware is ARM32-only forever, so x64 builds are a development tool, not a release target.

### 3.1 Toolchain setup
- [x] Confirm/finish `Toolchain-x64-UWP-clang.cmake` targets `x86_64-unknown-windows-msvc` correctly with clang-cl
- [x] Confirm the `x64-uwp` vcpkg triplet builds your dependency set (brotli, curl, zlib, ICU, SQLite, ANGLE) for x64
- [x] Get a minimal "hello world" clang-cl x64 binary building and running — isolates toolchain problems from engine problems

**Build environment:** Surface Pro 5 (i5-7300U, 4GB RAM); ICU 75 built from source at `C:\icu-x64-uwp\`; vcpkg x64-windows installed.

### 3.2 Engine compile (WTF → JSC → WebCore)
- [x] Build WTF for x64 (smallest, fewest platform dependencies) — 173/350 .obj compiled; all WK_WINUWP patches applied
- [ ] Build JavaScriptCore (CLoop first, JIT later — see 3.4) for x64 — **185/1200 .obj compiled**; stopped at JIT incomplete type error in `JITInlineCacheGenerator.h`
- [ ] Build WebCore for x64, reusing the `WK_WINUWP` guards from Phase 1 — expect some guards to need an `#if defined(WK_WINUWP) && defined(_M_ARM)`-style split where ARM32-specific assumptions don't hold on x64 (calling convention, struct packing, pointer-width-dependent code)
- [ ] Track every place a guard needs splitting in a running list — this tells you how "ARM32-specific" vs. genuinely "UWP-specific" your patches actually were, which is useful data for Phase 2's rebase decision too if it's still in flight

### 3.3 Port-layer (`port/`) x64 compile
- [ ] Compile `WebCoreDriver.cpp/h`, `PortChromeClient`, `LoadingFrameLoaderClient`, `PortPlatformStrategies`, `PortNetworkStorageSession` for x64
- [ ] Compile the `stubs-*.cpp` platform stub files for x64 — check for any ARM32-assumption bugs (e.g. anything touching capability/sandbox APIs that differ between architectures)
- [ ] Get `WebCoreDriver-gpu.lib` linking successfully for x64 via `lld-link`

### 3.4 JIT backend validation (separate from CLoop)
- [ ] Build JSC with JIT enabled for x64 — this exercises a **different JSC backend** than the ARM32 thumb JIT, so don't assume parity from the ARM32 work
- [ ] Run JSC's own test262/JIT correctness tests if feasible, or at minimum execute a handful of JS-heavy real pages and confirm correct output
- [ ] Confirm executable-memory allocation works under whatever sandbox model you're testing in on x64 (desktop UWP App Container semantics may differ subtly from Mobile's)

### 3.5 Headless test harness (the actual payoff)
- [ ] Build a minimal x64 console/Win32 executable (not a UWP appx yet) that links `WebCoreDriver-gpu.lib`, loads a URL, and dumps `paintToRGBA` output to a PNG
- [ ] Add a small CLI surface: load URL, wait N seconds / wait for load event, dump screenshot, exit — enough to script a handful of regression pages
- [ ] Build a tiny regression page set (the same Bing/GitHub/Apple/Microsoft pages already used for ARM32 validation, plus a couple of JS-heavy ones) and capture baseline screenshots
- [ ] Confirm this headless harness builds and runs **without any UWP packaging step** — this is what actually decouples iteration from the Lumia/WiFi loop

**Phase 3 exit criteria:**
- [ ] WTF/JSC/WebCore/port layer all compile clean for x64 on the dev machine
- [ ] A headless x64 test executable can load a real page, run JS, and produce a rendered screenshot with no device deployment involved
- [ ] A documented list of which `WK_WINUWP` guards turned out to be ARM32-specific vs. truly UWP-specific (feeds back into Phase 1's inventory and Phase 2's rebase decision if still open)

---

## Appendix: WinCairo / "Win" port reference (for Phase 2)

- Source (it's just the main repo, no separate WinCairo clone exists anymore): `https://github.com/WebKit/WebKit`
- Build instructions: `https://docs.webkit.org/Ports/WindowsPort.html`
- Prebuilt third-party dependency libs (Cairo/ICU/libcurl/etc., auto-fetched by `build-webkit`, also downloadable standalone): `https://github.com/WebKitForWindows/WebKitRequirements/releases`
- Prebuilt nightly `MiniBrowser` builds, no compiling required, useful as a reference/control: `https://build.webkit.org/#/builders/1192` (Windows-64-bit-Release-Build) — download a green build's "Archive" artifact + matching WebKitRequirements release + `vc_redist.x64.exe`

## Open questions / risk register

- [ ] Does the App Container capability trick that unlocks `codeGeneration` for ARM32 JIT behave the same on x64 desktop UWP, or does it need separate research?
- [ ] Is the `x64-uwp` vcpkg triplet actually producing UWP-compatible (App Container-safe) binaries, or just plain x64 desktop binaries? Matters once you move past the headless harness to an actual x64 UWP appx (Phase 4, not covered here).
- [ ] If Phase 2 picks a rebase, how much of Phase 3's "which guards are ARM32 vs UWP" data is salvageable across the rebase vs. needing to be redone?
