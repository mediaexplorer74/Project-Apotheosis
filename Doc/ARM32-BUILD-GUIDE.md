# Building the ARM32 build — Lumia 950 / Windows 10 Mobile

> **Update Aug 16, 2026 — this machine's ARM32 environment was fully wiped by an OS reinstall.**
> The step-by-step "rebuild everything from scratch" checklist (what exists, what is missing,
> and the two external dependencies that must be re-obtained — ICU75-arm and ANGLE-arm) lives in
> **`Doc/ARM32-RECOVERY.md`**. The sections below still describe *how* the pieces work; the
> recovery runbook is the *order* to rebuild them.

*A letter to whoever picks this up next. Everything below was read out of the actual scripts in
this repository on 2026-08-12, not reconstructed from memory. Where a fact could not be
verified on this machine it says so explicitly — an unverified step presented as a verified one
is worse than no documentation at all.*

The ARM32 build is the point of the whole project. The x64 build is scaffolding: a machine that
can be rebuilt in minutes so that engine bugs are found on a keyboard instead of on a phone
that has to be re-flashed. Everything x64 proves — the renderer, the loader, the event path,
JIT, GPU compositing — has to be proven again on ARM32, because ARM32 is where the ABI, the
register allocator and the App Container are all different.

---

## 0. What is actually on disk right now (read this before planning anything)

| Thing | State on this machine |
|---|---|
| `WebKit/` (upstream webkitgtk-2.52.4 sparse checkout) | **present** (gitignored, GB-scale) |
| `build-x64-gpu` | **present, 7.55 GB** — the current dev line |
| `build-arm32-webcore`, `build-arm32-jit` (ARM trees) | **absent** |
| `build-arm32-gpu` (ARM tree) | **present (Aug 18)** - WebCore.dll + JavaScriptCore.dll linked, driver lib archived, **Harness_0.1.9.3_ARM.appx packaged**; only device deployment pending (see `Doc/2026-08-18-arm32-webcore-link-fixes.md`) |
| `angle/arm`, `angle-windowsstore` (ANGLE ARM binaries) | **absent** (gitignored, re-downloadable) |
| `Src/port/harness-cmd.bat` | **present, and it is an ARM32 capture** — see §5 |
| Free space on `C:` | **10.3 GB of 118.3 GB** |

Two consequences follow, and they are the two things that will actually stop you:

1. **ARM is a from-scratch configure and a full WebCore build, not an incremental step.** There
   is no ARM build tree to resume. Budget hours, not minutes.
2. **It will not fit.** The x64 GPU tree alone is 7.55 GB and only 10.3 GB is free; a comparable
   ARM tree plus its 3.4 GB `WebCoreFull.lib` needs roughly 8–12 GB of its own. Either free the
   x64 tree first (and accept that you then cannot iterate on x64 until you rebuild it), or do
   the ARM build on a machine with ≥ 60 GB free. Decide this before you start, because
   discovering it at 80 % of a `-j1` WebCore build is expensive.

A third thing worth knowing, since it explains the state of the ARM scripts: the ARM32 harness
was originally built by this project's first author, on their machine, against a patched WebKit
they never published. Every ARM artifact in this repository is inherited from that work. The
scripts are real and were once known to work; they have **not** been run on this machine, and
some have drifted (see §5 and §9.10). Treat them as a well-informed starting point, not as a
tested pipeline.

---

## 1. Three toolchains, and why each one has to be what it is

| Layer | Compiler | Linker | Why not something simpler |
|---|---|---|---|
| Engine: WTF, JavaScriptCore, WebCore | **clang-cl** (LLVM 22.1.8), `--target=thumbv7-unknown-windows-msvc` | lld / cmake-driven | Upstream WebKit dropped pure-MSVC support; MSVC cannot compile modern WebCore |
| Port layer: `Src/port/*.cpp` | **clang-cl**, same target | **lld-link** `/MACHINE:ARM` | Must match the engine's ABI and C++ mangling exactly |
| Harness: the UWP app (C++/CX, XAML) | **MSVC v143** (VS 2022, toolset 14.44.35207), `bin\Hostx64\arm\cl.exe` | MSVC `link` via msbuild | clang cannot compile C++/CX (`^` handles, `ref new`, WinRT metadata). **This is why VS 2022 must stay installed.** |

Two hard rules fall out of the toolchain choice:

* **C++ exceptions must be off** in the engine and port: `_HAS_EXCEPTIONS=0` plus `/EHs-c-`.
  clang's `thumbv7-windows-msvc` backend cannot lower `cleanupret`, the instruction Windows
  exception unwinding is built on. This is not a preference; enabling exceptions produces a
  backend crash, not a slow binary.
* **VS 2026's `vcvarsall` no longer offers an `arm` (32-bit) target.** The v143 14.44 toolset
  still ships a working ARM `cl.exe` and `lib\arm\store`, so `Src/port/arm32-uwp-env.ps1`
  assembles `PATH`/`INCLUDE`/`LIB` by hand instead of calling `vcvarsall`. If that script stops
  working, the first thing to check is whether `VC\Tools\MSVC\14.44.35207\lib\arm\store` still
  exists — a VS update can remove it.

---

## 2. Prerequisites

* An **ASCII path with no spaces**. The Ruby code generators and meson break on non-ASCII paths;
  the original Chinese path broke the generators outright. Everything goes through
  `$env:APOTHEOSIS_ROOT`, set by `Src\setenv.ps1` — never hardcode an absolute path in a script.
* **LLVM 22.1.8** at `C:\Program Files\LLVM` (`clang-cl.exe`, `lld-link.exe`, `llvm-lib.exe`).
* **VS 2022 Community**, toolset `14.44.35207`, with the ARM libraries present.
* **Windows SDK** with `Lib\<ver>\{ucrt,um}\arm` and `um\arm\WindowsApp.lib`.
* **Ruby** on `PATH` (WebCore's bindings/IDL generators).
* **vcpkg** at `C:\vcpkg`, triplet **`arm-uwp`**, for the third-party stack.
* **ICU 75 for ARM** at `C:\icu-arm-uwp` (`Src\port\make-icu75-libs.ps1`).
* **ANGLE ARM binaries** in `angle\arm` (`libEGL.lib`, `libGLESv2.lib` + DLLs). Not in the repo;
  re-downloadable. `angle\include` *is* tracked.
* The upstream tree in `WebKit\` (`Src\tools\fetch-webkit-source.ps1`).

---

## 3. Dependencies, in order

```powershell
. .\Src\setenv.ps1
pwsh -File .\Src\port\build-pixman-arm32.ps1
pwsh -File .\Src\port\build-cairo-arm32.ps1
pwsh -File .\Src\port\build-fontconfig-arm32.ps1
pwsh -File .\Src\port\build-harfbuzz-arm32.ps1
pwsh -File .\Src\port\build-sqlite-arm32.ps1
```

These land in `deps-build\`. The `-x64` suffixed variants (`build-fontconfig-x64.ps1`,
`build-harfbuzz-x64.ps1`) are the x64 line; do not mix them. The remaining libraries — curl,
OpenSSL, libxml2, zlib, bzip2, brotli, libjpeg, libpng, libwebp, expat,
freetype — come from vcpkg's `arm-uwp` triplet, and **ICU 78** comes from the same triplet
(vcpkg `icu` port; the x64 line's custom ICU 75 at `C:\icu-x64-uwp` is not used for ARM).
Pixman is built by hand (build-pixman-arm32.ps1)
rather than from vcpkg because the stock pixman port fetches from gitlab.freedesktop.org, which
answers scripted clients with an Anubis bot-check page; the source tarball comes from
cairographics.org. Each build-*.ps1 runs the full meson cycle (setup + compile + install) into
`C:\vcpkg\installed\arm-uwp`.

Note on sqlite3: the vcpkg sqlite3 port cannot build for arm-uwp at all — modern amalgamations
have no `SQLITE_OS_WINRT` code path, so compiling under `WINAPI_FAMILY_APP` dies on undeclared
`CreateFileA`/`LoadLibraryA`/... — the vcpkg x64-uwp line never had it either. The ARM line
follows the x64 line's recipe instead: the amalgamation (3.49.1, matching the x64 header at
`C:\icu-x64-uwp\include\sqlite3.h`) compiled as plain Win32 with `SQLITE_OS_WINRT=1` and `/MT`
(no `WINAPI_FAMILY` define, so the 19041 SDK exposes the full API surface). The WinRT VFS only
calls `FromApp`/`CreateFile2` APIs, which resolve against `WindowsApp.lib` at link time.
`build-sqlite-arm32.ps1` installs `sqlite3.lib` + headers into `C:\vcpkg\installed\arm-uwp`.

Cairo/harfbuzz build notes (all fixed on 2026-08-18; keep if you rebuild from clean):
- freetype's `generic` member is renamed to `GenericFromFreeTypeLibrary` under
  `WINAPI_FAMILY != DESKTOP` and `#undef`'d after the struct definitions (`freetype.h:1230/2305`);
  harfbuzz's `hb-ft.cc` re-defines it after including freetype (patch lives in the vcpkg
  buildtrees copy; `-DWINAPI_FAMILY_APP=1` alone does not fix it). The harfbuzz cross file must
  define `WINAPI_FAMILY=WINAPI_FAMILY_APP` **and** `WINAPI_FAMILY_APP=1`, and must NOT pass
  `-Ddeprecated=disabled` (no such option in harfbuzz 14.3.0).
- cairo needs `#include <io.h>` in `cairo-compiler-private.h` (`_access` undeclared at
  `cairo-ft-font.c:630`; clang-cl defines `_MSC_VER` so cairo's POSIX-name mapping fires), and
  `user32.lib` in the cross file's link args (`FillRect`/`GetDC`/`ReleaseDC` are user32, not
  gdi32). `_csi_array_execute` in `util/cairo-script/cairo-script-objects.c` is declared
  `inline`; clang's C99 semantics emit no out-of-line definition (MSVC would), so drop the
  `inline` (patched). Delete stale `src\cairo-features.h`/`src\config.h` from the source tree —
  quoted includes resolve to the source dir before the meson build dir.
- All three cross files must point their `/LIBPATH` at the actually-installed SDK
  (`10.0.19041.0` → `100190~1.0`); the stale 22621 short name produces "could not open
  'ucrt.lib'" only in manual runs without the LIB env var.

Note on TLS: curl is built with its own OpenSSL rather than Schannel. The App Container has no
system certificate store, and the OS Schannel path only reaches TLS 1.2, so the port ships
`cacert.pem` and injects it at startup. See §9.8.

---

## 4. Configure and build the engine

```powershell
. .\Src\setenv.ps1
pwsh -File .\Src\port\configure-gpu-arm32.ps1
& "C:\Program Files\CMake\bin\ninja.exe" -C build-arm32-gpu WebCore -j1
```

`configure-gpu-arm32.ps1` generates `build-arm32-gpu` with exactly this flag set — worth understanding
rather than copying blindly:

| Flag | Meaning |
|---|---|
| `-DCMAKE_TOOLCHAIN_FILE=Src\port\Toolchain-ARM32-UWP-clang.cmake` | `SYSTEM_NAME=WindowsStore`, `SYSTEM_VERSION=10.0`, `SYSTEM_PROCESSOR=ARM`, clang target `thumbv7-unknown-windows-msvc`, `WK_WINUWP=1`, `/APPCONTAINER`. It also sets `CMAKE_{C,CXX}_COMPILER_WORKS TRUE` to skip CMake's compiler probe, which cannot link a test binary in this configuration. |
| `-DPORT=WinUWP` | the port's own CMake port name |
| `-DENABLE_STATIC_JSC=ON` | JSC as a static archive |
| `-DENABLE_C_LOOP=OFF`, `-DENABLE_JIT=ON` | real JIT, not the CLoop interpreter |
| `-DENABLE_DFG_JIT=OFF`, `-DENABLE_FTL_JIT=OFF` | baseline JIT only on ARM32 (x64 additionally enables FTL) |
| `-DENABLE_SAMPLING_PROFILER=OFF` | needs thread suspension APIs the App Container lacks |
| `-DUSE_SYSTEM_MALLOC=ON` | bmalloc is not ported |
| `-DAPOTHEOSIS_GPU=ON` | selects the GPU branch: `USE_TEXTURE_MAPPER`, `USE_GRAPHICS_LAYER_TEXTURE_MAPPER`, `USE_ANGLE` on; `USE_GRAPHICS_LAYER_WC` off |
| `-DCMAKE_PREFIX_PATH=C:\vcpkg\installed\arm-uwp` | ARM dependencies (ICU 78, vcpkg ports, meson-built deps all live there; the old `C:\icu-arm-uwp` path does not exist anymore) |

**Two upstream CMake edits are prerequisites** for `APOTHEOSIS_GPU` (both recorded in the
script's own header): `OptionsWinUWP.cmake` needs the `APOTHEOSIS_GPU` branch, and
`Source/CMakeLists.txt` must skip its `add_subdirectory(ANGLE)` when `APOTHEOSIS_GPU` is set —
we link prebuilt ANGLE instead of building it. Both must be `#if defined(WK_WINUWP)`-style
guarded and carry an `Apotheosis:` comment, like every other upstream change.

`-j1` is mandatory, not conservative: parallel clang jobs on WebCore's unified sources exhaust
RAM on this class of machine, and a killed job leaves the tree in a state ninja will not
recover from. Related: **never `touch` an object file in a build tree.** Ninja's incremental
logic in these trees is already unreliable; a hand-stamped object produces a link that silently
mixes ABIs.

---

## 5. The port driver (`WebCoreDriver-gpu.lib`)

```powershell
pwsh -File .\Src\port\link-driver-gpu-arm32.ps1              # compiles all 12 TUs, then archives
pwsh -File .\Src\port\compile-driver-gpu-arm32.ps1 <src.cpp> <out.obj>   # one file, for fast error hunting
```

Twelve translation units make up the driver:

```
WebCoreDriver  PortPlatformStrategies  LoadingFrameLoaderClient  PortNetworkStorageSession
PortChromeClient  webcore-driver-stubs  stubs-crypto  stubs-pasteboard  stubs-network
stubs-ax  stubs-other  stubs-loader
```

`link-driver-gpu-arm32.ps1` runs an `lld-link /DLL /MACHINE:ARM` probe (to surface unresolved symbols
early, when they are still cheap to read) and then archives the objects with `llvm-lib` into
`WebCoreDriver-gpu.lib`, which is what the harness actually links.

**`harness-cmd.bat` is the most fragile artifact in the entire pipeline, and you must know what
it is.** `compile-driver-gpu-arm32.ps1` does not contain a compiler command line. It *reads* one from
`Src\port\harness-cmd.bat` — a command line captured verbatim out of the WebCore ninja build —
strips `/showIncludes`, `/Fo`, `/Fd` and the unified-source `-c --` tail, rewrites
`build-arm32-webcore` → `build-arm32-gpu`, appends the WebKitLegacy and `angle\include` include
paths, and runs it. That is how the port gets WebCore's exact several-hundred-entry include set
and define set without duplicating it.

The copy currently in the repository is an **ARM32** capture (`--target=thumbv7-unknown-windows-msvc`,
`build-arm32-webcore`), which is good news: the ARM compile recipe survived. Two cautions:

* If the WebCore configure changes its include or define set, this file goes stale silently —
  you get "cannot open include file" or, worse, an ODR mismatch. Recapture it with
  `ninja -C build-arm32-gpu -t commands <some WebCore object>` and take the `clang-cl` line.
* It carries `-DBUILDING_WebCore` (alongside `-DBUILDING_WEBKIT`, `-DBUILDING_WINUWP__`,
  `-DBUILDING_WITH_CMAKE`). That define controls whether `WEBCORE_EXPORT` means *dllexport* or
  *dllimport*. It is harmless when the ARM engine is a static archive and **wrong** if the ARM
  line is ever switched to a WebCore DLL, as x64 was. If you make ARM produce a DLL, strip
  `-DBUILDING_WebCore` from the port's command line or every `WEBCORE_EXPORT` symbol the port
  touches will be mangled as an export.

---

## 6. The harness (the UWP app)

```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=ARM /p:AppxPackage=true
```

`Harness.vcxproj` declares exactly two configurations, `Release|ARM` and `Release|x64`. The ARM
one links, in this order: `libEGL.lib`, `libGLESv2.lib`, `WebCoreDriver-gpu.lib`, `WebCore.lib`,
`JavaScriptCore.lib`, `PAL.lib`, `WTF.lib`, **then `JavaScriptCore.lib` and `WTF.lib` again**,
then curl/OpenSSL, then the graphics and text stack (cairo, pixman, freetype, fontconfig, expat,
harfbuzz, jpeg, png, webp, sharpyuv, xml2, sqlite3, z, bz2, brotli), then ICU. Library
directories are `..\angle\arm` and `C:\vcpkg\installed\arm-uwp\lib`, and the
extra options are `/FORCE:MULTIPLE /NODEFAULTLIB:libcmt.lib`.

ICU version: the ARM line is compiled against vcpkg's ICU **78** (import libs bound to
`icuuc78.dll`/`icuin78.dll`/`icudt78.dll`, data inside `icudt78.dll` — no separate `.dat`).
Packaging uses the matching vcpkg DLLs; shipping a different ICU version than the import libs
are bound to is the "ICU version trap" (AGENTS.md) and fails at load.

The repeated `JavaScriptCore.lib`/`WTF.lib` is deliberate: it resolves circular references
between static archives. `/FORCE:MULTIPLE` then papers over the duplicate-symbol complaints that
follow. Both are signatures of a **statically linked** engine, whereas the x64 line loads
`WebCore.dll` and `JavaScriptCore.dll` at runtime via `LoadPackagedLibrary`. **Verify which shape
your ARM engine build actually produced before trusting this list** — this is the one part of the
ARM configuration this guide could not check, because no ARM tree exists here to check against.

Also note the ARM list does *not* include `psl.lib` or `windowsapp.lib`, both of which the x64
list does. If ARM linking fails on `WindowsApp`/PSL symbols, that asymmetry is the first place
to look; `arm32-uwp-env.ps1` does put `um\arm\WindowsApp.lib` on `LIB`.

Packaging checklist:

* Bump `<Identity Version="...">` in `Src\harness\Package.appxmanifest`. The deploy script's
  `-Ver` argument must match it exactly.
* The package depends on `Microsoft.VCLibs.140.00 (ARM)`. Most Lumias that already run another
  `-Reborn` app have it.
* `cacert.pem` must be packaged (see §9.8), along with the fonts in `Assets\fonts`.
* Measure the resulting appx with PowerShell `.Length`, **never** `ls -l` — Windows owner names
  contain spaces and shift the columns, so you will read the wrong number.

---

## 7. Deploy to the phone

Device prerequisites: powered on, on the same Wi-Fi, and **Settings → For developers → Device
Portal** enabled.

```powershell
pwsh -File .\Src\tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>
```

* `Src\tools\Deploy-Robust.ps1` — retrying deploy. Use it by default: the phone drops Wi-Fi to
  save power and a half-transferred appx is the most common failure of the whole loop.
* `Src\tools\auto-diag2.ps1` — the full automated cycle (uninstall → install → launch → poll
  `LocalState` for dumps and BMP screenshots). It only arms itself when `autodiag.txt` exists on
  the device, which is deliberate: it must never trigger during an interactive test.
* `Src\tools\Wdp-Crash.ps1`, `Parse-Minidump.ps1`, `Symbolize-Crash.ps1` — crash retrieval and
  symbolization.

If you are working remotely with no device on the network, build the appx only and hand it over.
An ARM32 appx cannot run on this x64 laptop; the "x64 emulator" is a Win11 x64 App Container,
which validates engine logic but nothing ARM-specific.

---

## 8. Verification ladder on the device

Climb it in order; each rung has been reached on real hardware before, so a rung that fails is a
regression with a known-good predecessor, not unexplored territory.

1. App launches, `LocalState\log.txt` written, JIT probe result in `jitresult.txt`.
2. `WebCoreRenderHtml` on a static string paints — proves WTF + JSC + WebCore + Cairo.
3. `https://example.com` paints. Expect `vua=1 sheets=1 pending=0` in the diag string, and a
   *middling* `nonwhite` count. (Since the surface is pre-filled opaque white,
   `nonwhite=0/777600` means "white page", and `nonwhite=777600/777600` means every pixel was
   repainted — example.com's `#f0f0f2` background does exactly that.)
4. A page with subresources — `https://news.ycombinator.com`. This exercises
   `PortLoaderStrategy` (see §9.11) and is the current frontier on x64 as well.
5. Real events: tap, scroll, link navigation.
6. JIT enabled (`ENABLE_JIT=ON`) — watch for the ABI landmine in §9.1 first.
7. GPU: ANGLE on D3D11 FL9_3, TextureMapper compositing presenting into the `SwapChainPanel`.
8. Smooth scrolling and pinch zoom.

---

## 9. The landmines, in the order they are likely to bite

**9.1 `std::partial_ordering` sret ABI split.** This is the one that cost the most time on x64
and it *will* be present in a fresh ARM tree: a mismatch in how a small struct is returned makes
`WTF::operator<=>` read a stale register, and every real page load crashes. Apply the
`ALWAYS_INLINE` `WK_WINUWP` header fix and re-verify the invariant **before** deploying, not
after the first crash. Details in the project memory (`partial-ordering-sret-abi-split`).

**9.2 Exceptions stay off.** `_HAS_EXCEPTIONS=0` + `/EHs-c-`. See §1.

**9.3 `Frame::Navigate` is permanently banned** — it produces `0xc000027b` in this host. All
navigation goes through the C ABI.

**9.4 Ninja incremental builds in these trees are unreliable.** Never hand-stamp an object.
Always `-j1`.

**9.5 GPU compositing must stay gated on `g_gpuActive`** (default `false`, set true only after
`WebCoreGpuInit` succeeds). Unconditional compositing once caused a silent `__fastfail` on the
real device with no dump at all. When GPU is off the port falls back to Cairo software rendering
plus `EmptyChromeClient`, which is a zero-regression path.

**9.6 The C ABI header exists in two copies** — `Src\port\WebCoreDriver.h` and
`Src\harness\WebCoreDriver.h`. Adding or changing an export means editing **both**. They are not
generated from one another and nothing checks them.

**9.7 Threading.** Present happens **only** on the engine thread. The UI thread must **never**
synchronously wait on the engine: ANGLE marshals surface create/resize back to the panel
dispatcher, so a UI-thread wait is a deadlock, and `RunOnUIThread`'s timeout calls
`std::terminate`. Every C ABI call is serialized onto the single engine thread.

**9.8 The CA certificate blob must be published exactly once.** `WebCoreSetCACertBlob` hands
curl a raw pointer with `CURL_BLOB_NOCOPY` into a `Vector<uint8_t>` owned by
`CurlContext::singleton().sslHandle()`. A second call replaces that Vector and frees the buffer
curl is still holding, so OpenSSL parses freed memory and the request fails with
`curlcode=60 … unable to get local issuer certificate (20)` — intermittently, which is what makes
it expensive to diagnose. The harness now wraps its setup in `std::call_once`; keep it that way.
`_putenv_s` in the same function is also not thread-safe.

**9.9 Only the phone can validate ARM32.** There is no ARM32 emulator here.

**9.10 Some ARM scripts have drifted from the current layout.** **FIXED Aug 16** —
`Src\port\link-driver-gpu-arm32.ps1` used to compute `$P = "$env:APOTHEOSIS_ROOT\port"`, from
before the sources moved under `Src\`. It now derives `Src\port` via `$PSScriptRoot` like the
x64 scripts (ditto `link-driver-jit-arm32.ps1`). The same day all ARM scripts were bulk-renamed
`build-clang-*` → `build-arm32-*` and obj suffixes unified (`.arm32.obj` / `.arm32-jit.obj` /
`.arm32-soft.obj`). If a "missing source" warning or empty archive appears anyway, audit every
`*-arm32.ps1` / `*-jit.ps1` for a stale absolute path before blaming the compiler.

**9.11 The loader strategy is shared with x64 and it is new.** `PortPlatformStrategies.cpp`
used to return a `StubLoaderStrategy` whose `loadResource()` had an empty body, so no subresource
— no stylesheet, script, image or font — ever loaded, and because a pending render-blocking
stylesheet keeps `Document` holding `VisualUpdatesPreventedReason::RenderBlocking`,
`RenderLayer::shouldSuppressPaintingLayer()` refused to paint *anything at all*. Every real site
showed a blank window. It now returns a real `PortLoaderStrategy` that creates a
`SubresourceLoader` and starts it one run-loop turn later. This is the same file on both
architectures, so ARM inherits both the fix and any bug in it.

---

## 10. Ordering advice

Do not try to land ARM32 GPU in one pass. The three configurations exist as a ladder for a
reason, and the ladder is cheaper than it looks because each rung reuses the previous
dependency build:

1. `build-arm32-webcore` — Cairo software rendering, CLoop. Proves the toolchain, the App
   Container, fonts and the render path.
2. `build-arm32-jit` — adds `ENABLE_JIT=ON`. Proves executable-memory allocation under App
   Container restrictions.
3. `build-arm32-gpu` — adds ANGLE + TextureMapper. Proves D3D11 FL9_3 and the
   `SwapChainPanel` present path.

The `gpu-path1` branch is the GPU line; `master` holds the JIT-only line. `configure-phase0.ps1`
and `configure-phase1.ps1` produce rungs 1's ancestors if you need to bisect that far back.

If disk space forces a choice, build rung 1 first and keep it: a working software-rendering
ARM32 build on the device is worth more than a half-finished GPU tree, and it is the base
everything else is measured against.

## 11. Bringing the line up on a stock VS 2022 box (measured 2026-08-17)

The ARM32 line needs four things that no document mentioned, and each one costs a failed vcpkg run to
discover. `Src\tools\arm-bootstrap.ps1` now asserts or fixes all four in its preflight, so this
section is the explanation rather than the procedure.

**1. The MSVC ARM32 toolset is a separate VS component, and the versioned one is the right one.**

```
"C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify ^
    --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" ^
    --add Microsoft.VisualStudio.Component.VC.14.44.17.14.ARM --passive --norestart
```

Take the versioned component, not `Microsoft.VisualStudio.Component.VC.Tools.ARM` ("latest"): the whole
tree is pinned to toolset **14.44.35207**, and "latest" would install whatever is current. Installing it
took about two and a half minutes and brought both `bin\Hostx64\arm\cl.exe` and `lib\arm\store` — the
store libraries come with the architecture component, so no separate UWP component is needed. Note that
`VC.Tools.ARM64` and `ARM64EC` were already installed and are irrelevant here: ARM64 is not ARM32.

**2. VS 2022 17.14 ships no arm32 `vcvars` wrappers, and vcpkg needs them to exist.**

Only `vcvarsamd64_arm64.bat` and `vcvarsx86_arm64.bat` are installed. vcpkg enumerates target
architectures by looking for exactly these files, so without the arm32 pair it reports

```
error: in triplet arm-uwp: Unable to find a valid toolchain for requested target architecture arm.
The available toolchain combinations are: x86, amd64, x86_arm64, amd64_arm64
```

and refuses before reading any triplet — it will not even `--only-downloads`. `vcvarsall.bat` itself
still supports the target: `vcvarsall.bat amd64_arm` prints *"Environment initialized for: 'x64_arm'"*
and resolves `cl.exe` to the ARM compiler. Each wrapper is therefore one line:

```bat
@call "%~dp0vcvarsall.bat" x64_arm 10.0.19041.0 %*
```

**3. `vcvarsall` builds `LIB` from two different SDKs, and one half does not exist for ARM.**

This is the subtle one. Invoked without an explicit SDK it produces:

```
...\Windows Kits\10\lib\10.0.19041.0\ucrt\arm     <- 19041, because 26100 has no ucrt\arm
...\Windows Kits\10\lib\10.0.26100.0\um\arm       <- 26100, and this directory does not exist
```

so every ARM link dies with `LINK : fatal error LNK1104: cannot open file 'WindowsApp.lib'` — inside
vcpkg's own compiler probe, long before a port is built. `vcvarsall` ignores `WindowsSdkVersion` from
the environment, and vcpkg never passes the SDK positionally, so pinning it in the wrapper above does
not help either: vcpkg calls `vcvarsall.bat` directly and only uses the wrappers as a feature probe.
The practical fix is to make the path it insists on resolve to the only ARM libraries that exist:

```bat
mklink /J "C:\Program Files (x86)\Windows Kits\10\lib\10.0.26100.0\um\arm" ^
          "C:\Program Files (x86)\Windows Kits\10\lib\10.0.19041.0\um\arm"
```

A junction needs no elevation, and `rmdir` on the link removes it. Both this and the wrappers live
under `C:\Program Files`, so a VS or SDK update may wipe them; the preflight recreates them.

**4. The overlay triplet is mandatory, and two of its settings were actively harmful.**

vcpkg's stock community `arm-uwp` triplet does not pin the SDK, so CMake picks 26100 and the probe
fails as above. Use the fork's:

```powershell
vcpkg install <ports>:arm-uwp --overlay-triplets=<repo>\Src\port\vcpkg-triplets
```

`Src\port\vcpkg-triplets\arm-uwp.cmake` used to also set `VCPKG_VISUAL_STUDIO_PATH` and
`VCPKG_PLATFORM_TOOLSET v143`. Together they made vcpkg report *"Unable to find a valid Visual Studio
instance ... with toolset version v143"* for the only instance installed, which does hold that
toolset. Both were removed; vcpkg finds the instance perfectly well on its own.

**What the ports list should be.** Mirror the working x64-uwp triplet **with its features**, which
`arm-bootstrap.ps1` does by parsing `vcpkg list`. Half the rows are feature rows and they are not
cosmetic — `curl[openssl]` is what gives the port its own TLS 1.3, the reason HTTPS works inside an App
Container with no system certificate store. Installing bare `curl:arm-uwp` would quietly build a weaker
stack whose failure would only appear later, on the device.

**Expect transient download failures.** The first full run died on
`curl operation failed with response code 429` from GitHub while fetching brotli — rate limiting, after
vcpkg had already used its own three attempts. The bootstrap now retries the whole vcpkg step up to
four times with a 90-second pause, because a step that is safe to repeat should not end an overnight
run.
