 
  # Project Apotheosis — Architect's RnD Report & Development Roadmap
  
  > Research date: July 6, 2026 · Repo: `mediaexplorer74/Project-Apotheosis` (fork of `Jimmyxiao2009/Project-Apotheosis`)
  > Goal: "museum" WebKit browser for Windows 10 Mobile (Lumia 950 / 640 / 1520‑class devices) + x64 PC debug line
  > Build machine constraint: Surface Pro 5, 4 GB RAM, 40 GB free disk

## 0. Current status — Sep 17, 2026

> Everything below is **historical** (July 2026 R&D report) — kept intact, not erased. Current
> state: [STATUS-2026-09-17.md](STATUS-2026-09-17.md) (x64 0.1.9.93 deployed in-place; GPU readback anomaly —
> ignored `makeCurrent=0`, NULL GL context, yet reported success; later `0xc0000005` crash
> symbolized to a CSS StyleCalculationTree std::variant access — NOT a proven JSC/EGL cause;
> no crash dump; plan: fail-closed GPU handling, live dumps, independent rendering/JS tests,
> ARM later, Git optional).

---

  
  ---
  
  ## 1. What the repository actually is
  
  The repo is **NOT a browser source tree** — it is a **port layer + host app + DevOps scripts** around an *external, gitignored* WebKit checkout (webkitgtk‑2.52.4, tag commit `7acdf5e`):
  
  | Layer | Location | Size / state |
  |---|---|---|
  | **Harness** (UWP browser shell, C++/CX + XAML, MSVC v143) | `Src/harness/` | ~2,700 lines in `MainPage.xaml.cpp`; tabs, URL bar, settings, gestures, GPU `SwapChainPanel` + SW `WriteableBitmap` fallback, en/ru/zh UI |
  | **WebCoreDriver** (port layer, C ABI boundary) | `Src/port/` | `WebCoreDriver.cpp` (~2,200 lines), `PortChromeClient`, `LoadingFrameLoaderClient`, `PortPlatformStrategies`, ~1,050 lines of stubs (crypto/ax/pasteboard/network/loader) |
  | **Build scripts** (PowerShell) | `Src/port/*.ps1` | ~3,800 lines: configure/compile/link for 4 build lines (phase0, webcore, jit, gpu) × (ARM32, x64) |
  | **Toolchains** | `Src/port/Toolchain-*.cmake` | ARM32‑UWP‑clang (active), x64‑UWP‑clang (new), ARM32 pure‑MSVC (abandoned) |
  | **Device tooling** | `Src/tools/` | WDP deploy, crash minidump parsing/symbolizing, watch‑and‑deploy loops |
  | **Docs** | `Doc/` | Excellent: PLAN, Summary ×3, HANDOFF (zh), night logs, WebKit‑upgrade research |
  | **Committed build tree** ⚠️ | `build-x64-gpu/` | ~100+ MB of ninja state, logs, generated `.vcxproj`/`.sln` — accidentally committed |
  
  **Architecture (sound and well‑layered):**
  
  ```
  Harness (MSVC v143, C++/CX)  ──C ABI (WebCoreDriver.h)──►  WebCoreDriver.dll (clang-cl + lld-link)
                                                                  │
                                                WebKit/WebCore/JSC/WTF (clang-cl, WK_WINUWP patches)
                                                Cairo SW paint  |  ANGLE D3D11 FL9.3 + TextureMapper GPU
  ```
  
  Key design decisions that are worth keeping: single engine thread with serialized calls, UI thread never blocks on engine, stable C ABI so the shell and engine can use *different compilers* (MSVC vs clang‑cl), runtime SW/GPU switch.
  
  **Proven on‑device status (Lumia 950, 15254):** WTF+JSC (JIT, 5–50× vs CLoop), WebCore + Cairo, GPU compositing, live interaction, tabs/settings — all ✅ per docs. The ARM32 line works; the repo is essentially the *record* of how.
  
  ---
  
  ## 2. Confirmed Problem #1 — the patched WebKit does not exist anywhere public
  
  - Your repo tracks only the port layer. `WebKit\` is **gitignored** on the build machine.
  - The upstream fork `Jimmyxiao2009/Project-Apotheosis` is **923 KB** — same layout, also **no WebKit source and no patch files**.
  - The ~dozens of `WK_WINUWP` patches (per docs: `VirtualAlloc→VirtualAllocFromApp`, `CreateFileW→CreateFile2`, `BCryptGenRandom`, SEH removal, `mpark::variant→std::variant`, RunLoopWin rewrite to generic event loop, DbgHelper/OSAllocator/MemoryPressure/Signals guards, new `OptionsWinUWP.cmake` + `PlatformWinUWP.cmake` files) live **only in local working copies**.
  
  **Hard evidence from your own committed x64 logs** (`build-x64-gpu/build-jsc-out*.txt`): the newest failures are exactly the *unpatched* files failing to compile —
  
  ```
  DbgHelperWin.h(37): error: unknown type name 'SYMBOL_INFO'
  OSAllocatorWin.cpp(117): error: use of undeclared identifier 'VirtualUnlock'
  RunLoopWin.cpp(40): error: use of undeclared identifier 'getWindowPointer'
  RunLoopWin.cpp(48): error: no class named 'ScheduledTask' in RunLoop::TimerBase
  clang++: error: unknown argument: '-imsvcC:\icu-x64-uwp\include'
  ```
  
  i.e., your machine's WebKit checkout is missing (or only partially carries) the WK_WINUWP patch set that the docs describe. **This is the single biggest project risk: the build is currently unreproducible by anyone, including future‑you.** Everything else in the roadmap depends on fixing this first.
  
  ## 3. Confirmed Problem #2 — x64 status (better than "not found", but blocked)
  
  Contrary to "only ARM32 target found", the repo already contains a real x64 line:
  
  | x64 asset | State |
  |---|---|
  | `Toolchain-x64-UWP-clang.cmake` (`x86_64-unknown-windows-msvc`, App Container) | ✅ exists, clean |
  | `configure-gpu-x64.ps1`, `compile-driver-gpu-x64.ps1`, `link-driver-gpu-x64.ps1`, `build-x64-deps.ps1`, vcpkg triplets | ✅ exist |
  | Deps: 16 vcpkg x64‑uwp pkgs, ICU 78, SQLite (WinRT VFS), ANGLE x64 (NuGet) | ✅ per docs, installed on build machine |
  | CMake configure of `build-x64-gpu` | ✅ (June 29) |
  | WTF + bmalloc x64 | ✅ compiled |
  | PAL header generation (1,203 steps) | ✅ |
  | AT&T‑asm files (`LowLevelInterpreter.cpp`, `MacroAssemblerX86_64.cpp`) via clang++ GNU‑driver ninja rules | ✅ (after `-imsvc→-isystem` patching) |
  | **JavaScriptCore.dll** | 🔄 ~8/111 unified sources; blocked by missing WK_WINUWP patches on current checkout |
  | WebCore.dll → WebCoreDriver‑x64.dll → x64 Harness | ❌ not reached |
  
  Fragile spots discovered in logs/docs (must be engineered away, not re‑fought each time):
  1. **CMake 4.0 Ninja generator bug** — emits only ~10 of 34 rules; worked around by `patch-build-ninja-gnu.ps1` auto‑scanner. Silent 38 MB `build.ninja` truncation if the patch script is interrupted.
  2. **Mixed flag dialects** — GNU‑driver rules receiving MSVC flags (`/Fo`, `/showIncludes`, `-imsvc`), seen failing in `build-jsc-out*.txt`.
  3. **Hardcoded machine paths** — `configure-gpu-x64.ps1` hardcodes `C:\Users\Admin\...`‑era ninja path and `C:\icu-x64-uwp`; the repo path `!OpenCode` (with `!`) is asking for quoting trouble.
  4. **Zombie clang processes / stale `.ninja_lock`** after interrupted builds.
  
  ## 4. Problem #3 — the "MSBuild dream": honest feasibility assessment
  
  > *Dream: WebKit built by VS/MSBuild — no ninja, no perl, no clang… possible or never?*
  
  Split the dream into 3 independent claims:
  
  | Claim | Verdict | Why |
  |---|---|---|
  | **(a) No ninja → MSBuild/.sln** | ✅ **Achievable** | CMake generates VS solutions: `cmake -G "Visual Studio 17 2022" -T ClangCL`. Your `build-x64-gpu` already contains generated `WebKit.sln` + `.vcxproj` files as a side effect. This also *kills the CMake‑4.0‑ninja‑rules bug entirely* — the bug is in the Ninja generator. |
  | **(b) No clang → pure MSVC cl.exe** | ❌ **Not in this life (for WebKit ≥2.5x)** | Upstream `OptionsMSVC.cmake` states clang‑cl only; source is full of `__attribute__`, `__PRETTY_FUNCTION__`, GNU inline asm, computed goto in LLInt. Reverting that is a multi‑person‑year fork. **BUT**: the *ClangCL platform toolset* is a first‑class citizen of MSBuild/VS — you keep clang as the compiler while VS drives the build, F5‑debugging works. Your Harness stays pure MSVC. This satisfies the spirit of the dream. |
  | **(c) No perl/ruby/python (codegen)** | ⚠️ **Half‑achievable via "pre‑generated DerivedSources"** | Ruby/Perl/Python are needed only for **DerivedSources** (IDL bindings, LLInt asm, unified sources). They run at *generation* time, not every compile. Strategy: run codegen **once** (on any machine, even WSL/CI), commit/vendor the `DerivedSources/` + `unified-sources/` trees as an artifact, then day‑to‑day builds are pure MSBuild+clang‑cl with zero scripting runtimes. Cost: regenerate the artifact whenever WebKit sources or feature flags change. |
  
  **Recommended end‑state ("Dream v1.1"):** `WebKit.sln` generated by CMake (VS generator, ClangCL toolset) + vendored pre‑generated DerivedSources + Harness.sln referencing the produced DLLs. One‑click F5 on the Surface. No ninja, no perl at build time. Clang stays, but hidden inside the VS toolset — which is the practically attainable version of the dream.
  
  ## 5. Constraint check — Surface Pro 5 (4 GB RAM / 40 GB disk)
  
  This is tight but workable **for x64 Release** builds with discipline:
  
  - **Disk (~35 GB budget):** WebKit *sparse checkout* only `Source/{WTF,JavaScriptCore,WebCore,cmake,bmalloc,ThirdParty/capstone}` (~3–4 GB) — the docs already do this for ARM. Build tree for JSC+WebCore Release ≈ 15–25 GB. **Disable PDB generation** (`/Zi` off, or `/DEBUG:NONE` link) except for the handful of files you're actively debugging — PDBs for WebCore alone can eat 10+ GB. Never commit `build-*` trees (see Phase 0).
  - **RAM (4 GB):** compile with **max 2 parallel jobs** (`/m:2` for MSBuild, `-j2` ninja); a single WebCore unified‑source TU peaks at 1.5–2 GB. Linking `WebCore.dll` is the worst step: prefer **lld‑link** (much lower memory than MS link.exe), no LTCG, no /OPT:REF on debug iterations, and set a **fixed 16 GB pagefile** on SSD. Close VS during full engine rebuilds; only open it for Harness/driver work.
  - **Practical cadence:** full engine rebuild = overnight job; day work = incremental driver/Harness builds (seconds–minutes). This matches the project's existing "detached build + watcher" script pattern.
  
  ---
  
  ## 6. Roadmap
  
  ### Phase 0 — Reproducibility ("Rescue the patches") — *highest priority, ~1–2 weeks*
  The project is one dead SSD away from losing its core IP.
  
  - **P0.1** On the machine that successfully built ARM32: `git diff` the local WebKit checkout vs pristine `webkitgtk-2.52.4` → export as an ordered patch series `Src/patches/webkit-2.52.4/00xx-*.patch` (one patch per subsystem: WTF, bmalloc, JSC, WebCore, cmake/OptionsWinUWP). Include the *new* files (`OptionsWinUWP.cmake`, `PlatformWinUWP.cmake` ×3) either in patches or under `Src/webkit-overlay/` copied in by script.
    - If Jimmy's machine is the only one with the full set — coordinate with him now; your x64 log errors prove your local checkout is incomplete.
  - **P0.2** Write `Src/port/get-webkit.ps1`: shallow+sparse clone of the exact tag → apply patch series → verify with a hash manifest. This turns "No patched WebKit" into a 15‑minute bootstrap for anyone.
  - **P0.3** Repo hygiene: remove `build-x64-gpu/` from git history (BFG/`git filtergit-repo`), add `.gitignore` for `build-*`; keep only the *status/error summaries* as docs. Frees clone size (139 MB → ~10 MB) and your 40 GB disk.
  - **P0.4** De‑hardcode remaining machine paths (`configure-gpu-x64.ps1` ninja/ICU paths → `setenv.ps1` vars); rename working folder to avoid `!` in path.
  - **Exit criterion:** a clean machine can go from `git clone` to "WTF compiles for ARM32 *and* x64" using only scripts in the repo.
  
  ### Phase 1 — Finish the x64 debug line (ninja as‑is) — *~2–4 weeks*
  Don't mix the MSBuild migration into this; first make x64 *work* on the current system.
  
  - **P1.1** Apply the Phase‑0 patch series to the x64 machine's checkout → the `SYMBOL_INFO` / `VirtualUnlock` / `RunLoopWin` errors from `build-jsc-out9.txt` disappear (they are unpatched‑file errors, not x64‑specific bugs).
  - **P1.2** Finish `JavaScriptCore.dll`: remaining ~103/111 unified sources; keep the two GNU‑driver rules for AT&T‑asm files; make `patch-build-ninja-gnu.ps1` atomic (write to temp + rename, verify line count) to kill the build.ninja truncation hazard.
  - **P1.3** `WebCore.dll` x64: expect a new round of ~10–30 small WK_WINUWP fixes (same categories as ARM: Win32‑only APIs under `WINAPI_FAMILY_APP`). Feed every fix back into the Phase‑0 patch series *immediately*.
  - **P1.4** `WebCoreDriver-x64.dll` (scripts exist) → **x64 Harness** configuration in `Harness.sln` (new platform, link against x64 ANGLE) → run on the Surface itself.
  - **P1.5** Smoke matrix: `jittest` app + Harness on x64 (SW path first, then ANGLE/D3D11 — Surface's Intel GPU is ≥FL11, so FL9.3 shader paths need a quick sanity check).
  - **Exit criterion:** F5‑debuggable Harness.appx on the Surface Pro 5 rendering bing.com — your "serious app testing" unlock. All fixes captured as patches.
  
  ### Phase 2 — MSBuild‑ification ("Dream v1.1") — *~2–3 weeks, parallelizable after P1.2*
  - **P2.1** Generate `WebKit.sln`: `cmake -S WebKit -B build-x64-vs -G "Visual Studio 17 2022" -T ClangCL -A x64` with the same toolchain defines folded into a cache preset (`CMakePresets.json`). Verify WTF+bmalloc build inside VS.
  - **P2.2** Solve the two AT&T‑asm files in MSBuild: custom `<ClCompile>` item override or a pre‑build `clang++` custom build step for exactly those 2 files (equivalent of the GNU ninja rules).
  - **P2.3** Codegen freeze: script `generate-derived-sources.ps1` (runs ruby/perl/python once), then vendor `DerivedSources/` as a versioned zip artifact (GitHub Release asset — keeps repo small). MSBuild builds consume the artifact; contributors never install perl.
  - **P2.4** Wire Harness.sln → project references / post‑build copy of `JavaScriptCore.dll`, `WebCore.dll`, `WebCoreDriver.dll`, ANGLE DLLs into the Appx layout. One‑click build+deploy from VS.
  - **P2.5 (stretch)** Same VS‑generator treatment for the ARM32 line (VS generator + ClangCL can target ARM with the 14.44 toolset env from `arm32-uwp-env.ps1`; if it fights back, ARM32 stays on ninja — acceptable, since ARM32 already works).
  - **Exit criterion:** ninja and perl not required on a dev machine; VS build end‑to‑end on the Surface within RAM budget (`/m:2`).
  
  ### Phase 3 — Engine completeness & stability — *ongoing, ~1–2 months*
  - **P3.1** Real network stack (curl + openssl + libpsl were deferred): HTTPS, cookies (`PortNetworkStorageSession` is mostly stub), disk cache, redirects; replace `stubs-network.cpp` progressively.
  - **P3.2** De‑stub audit: `stubs-crypto` (WebCrypto → BCrypt), pasteboard (UWP Clipboard), loader edge cases; a11y stubs can stay.
  - **P3.3** Crash‑hardening loop using existing `Parse-Minidump.ps1`/`Symbolize-Crash.ps1`; top‑20 sites test list; memory ceiling tuning for 3 GB devices (Lumia 950) vs 1 GB (Lumia 640!).
  - **P3.4** WebKit upgrade train: skip 2.53.4 (dev, Skia‑centric — your own `WEBKIT-UPGRADE.md` analysis is correct), rebase the patch series onto **2.54 stable** when released. With Phase 0 done, a rebase is `get-webkit.ps1 -Tag 2.54 + fix rejects` instead of archaeology.
  
  ### Phase 4 — The "Museum Browser" product — *~1 month, fun part*
  - **P4.1** Device matrix: Lumia **950** (reference, 3 GB, done), **640** (1 GB RAM, Adreno 305 → verify FL9.3 GPU path + low‑memory mode: SW render default, single tab), **1520/"1200"‑class** (older SoC — confirm actual model & 15254 availability; pre‑W10M devices can't run UWP at all, so exhibit scope may need adjusting).
  - **P4.2** Kiosk/exhibit mode: autostart, home‑page playlist of era‑appropriate sites, idle reset, guided "tour" overlay (the multilingual `.resw` system is already perfect for museum labels in en/ru/zh).
  - **P4.3** Offline exhibit content: local HTTP server (`Test-HttpServer.ps1` is a seed) or `file://` archive of retro sites — museums can't rely on Wi‑Fi, and modern TLS may reject old cipher stacks anyway.
  - **P4.4** Release engineering: signed appx bundles per‑arch, install guide over WDP (tooling already in `Src/tools/`), versioned releases with the DerivedSources artifact.
  
  ### Phase 5 — Community & longevity
  - Publish the patch series + writeup (the Reddit thread proved demand); invite Jimmy to co‑maintain a shared `webkit-winuwp-patches` repo so both forks stop diverging; optional GitHub Actions x64 build using the Phase‑2 artifact (hosted runners have 16 GB RAM — your CI can be *bigger* than your laptop).
  
  ---
  
  ## 7. Priority order & effort snapshot
  
  | # | Phase | Effort | Risk | Depends on |
  |---|---|---|---|---|
  | 1 | **P0 Reproducibility** | 1–2 wk | Low | access to a machine with the full patched tree |
  | 2 | **P1 x64 finish** | 2–4 wk | Medium (WebCore unknowns) | P0 |
  | 3 | **P2 MSBuild** | 2–3 wk | Medium (asm files, codegen freeze) | P1.2 |
  | 4 | **P3 Engine/network** | 1–2 mo | Medium | P1 |
  | 5 | **P4 Museum product** | ~1 mo | Low | P3 partial |
  | 6 | **P5 Community** | ongoing | Low | P0 |
  
  **Bottom line:** the architecture is genuinely good and already proven on real hardware; the project's existential risk is not technical but archival — the WK_WINUWP patch set must be extracted into the repo *first*. After that, x64 is close (your blockers are mostly "patches missing on this machine" + build‑system fragility), and the MSBuild dream is realistically achievable in its v1.1 form: **VS + ClangCL toolset + pre‑generated sources — no ninja, no perl, and clang tucked invisibly inside Visual Studio.** Not "never in this life" — more like "next month, with a good pagefile." ^‑^
  
Result:
ok: true

Loaded interface-management instructions
▶
Arguments:
path: /tasklet/system/capabilities/interface-management/CAPABILITY.md
Result:
---
name: interface-management
description: |
  You interact with human users via the Tasklet interface. Interface Management allows you to manipulate the visual interface beyond responding via typical chat messages.
  
  Use the Interface Management capability to:
  - Build Instant Apps, which are personalized mini React apps that work with all the tools and resources you have access to (connections, filesystem, sandbox, database, etc.)
  - Show files and Instant Apps in a preview panel on the right half of the screen
  - Suggest intelligence changes via a UI form
  - Update the name for this chat
---

# Managing the agent interface

Control the user-facing surfaces of the agent: the right-side preview panel (files, instant apps, the sandbox browser), prompts to change the agent intelligence level, and the chat title.

## Tools

Following tools are available in this skill:

- `create_instant_app`
- `show_user_preview`