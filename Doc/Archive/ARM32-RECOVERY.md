# ARM32 environment recovery — runbook (Aug 16, 2026)

*Companion to `Doc/ARM32-BUILD-GUIDE.md` (the "how it works" letter). This file is the
"the machine was reinstalled, rebuild everything" checklist, verified against this exact
machine on 2026-08-16. State of every line below was checked with `Test-Path`/`Get-ChildItem`,
not reconstructed from memory. The x64 line survived the reinstall (prebuilt `build-x64-gpu`
+ the recovery steps in AGENTS.md "Toolchain reinstall recovery"); the **ARM32 build trees were
wiped**. **Corrected 2026-08-16 (later revision): `Src\angle\arm` DID survive** — the earlier
"no ARM artifact anywhere" claim was a wrong-path check (`<root>\angle` instead of
`<root>\Src\angle`, see §4). That leaves **exactly one** dependency that cannot be rebuilt from
local files and must be re-obtained externally: **ICU 75 for ARM** (§3).*

## 0. Status table (verified Aug 16, 2026)

| Prereq | Path | State |
|---|---|---|
| WebKit source tree | `WebKit\` | ✅ present (gitignored, GB-scale) |
| SDK 19041 ARM libs | `C:\Program Files (x86)\Windows Kits\10\Lib\10.0.19041.0\{ucrt,um}\arm` | ✅ present |
| LLVM 22.1.8 (clang-cl/lld-link) | `C:\Program Files\LLVM\bin\clang-cl.exe` | ✅ present |
| pkg-config (msys2) | `C:\vcpkg\downloads\tools\msys2\<hash>\usr\bin\pkg-config.exe` | ✅ present |
| gperf | `C:\vcpkg\installed\x64-windows\tools\gperf` | ✅ present |
| **MSVC `lib\arm`** (v143 14.44 CRT for thumbv7) | `...\VC\Tools\MSVC\14.44.35207\lib\arm` | ❌ **missing** (only `arm64`, `arm64ec` installed) |
| **Ruby** (WebCore IDL/bindings generators) | `ruby` on PATH | ❌ **missing** |
| **vcpkg `arm-uwp`** deps | `C:\vcpkg\installed\arm-uwp` | ❌ **missing** (x64-uwp exists) |
| **ICU 75 ARM** | `C:\icu-arm-uwp` | ❌ **missing** (x64 exists) |
| **ANGLE ARM** | `Src\angle\arm` + `Src\angle\include` | ✅ **present** — see §4 (do NOT test `<root>\angle`, there is no such dir) |
| ARM32 scripts (sync applied) | `Src\port\configure-gpu-arm32.ps1`, `link-driver-gpu-arm32.ps1`, `build-cairo-arm32.ps1`, `build-fontconfig-arm32.ps1`, `build-harfbuzz-arm32.ps1`, `arm32-uwp-env.ps1`, `make-icu75-libs.ps1` | ✅ present |
| vcpkg arm triplet | `Src\port\vcpkg-triplets\arm-uwp.cmake` | ✅ present (NOT in `C:\vcpkg\triplets\`) |
| Disk free | `C:` | 52.3 GB free of 118.3 GB (re-measured Aug 16, later in the day) |

Context from `setenv.ps1` (the env the ARM scripts expect): `APOTHEOSIS_ARCH=arm` →
`APOTHEOSIS_ICU = C:\icu-arm-uwp`, `APOTHEOSIS_VCPKG_TRIPLET = arm-uwp`,
`APOTHEOSIS_ANGLE = <root>\Src\angle`, `APOTHEOSIS_ANGLE_ARCH = arm`,
`APOTHEOSIS_MSVC = ...\14.44.35207`, `APOTHEOSIS_SDK_VER` must be set to `10.0.19041.0`
for the ARM line (the default in setenv.ps1 is `10.0.26100.0`, the x64 value; `arm32-uwp-env.ps1`
does not call vcvarsall — it assembles INCLUDE/LIB by hand, so it is the authoritative env).

## 1. Step 0 — machine prerequisites (before anything else)

1. **Restore MSVC `lib\arm`.** `arm32-uwp-env.ps1` requires
   `VC\Tools\MSVC\14.44.35207\lib\arm\store` (the v143 14.44 toolset still ships working ARM
   `cl.exe` + `lib\arm\store`; only the *component* is not installed here). Open the VS Installer →
   *Modify* → *Individual components* → install the **MSVC v143 – VS 2022 C++ ARM build tools**
   component, then verify `lib\arm\store` exists. Fallback if the component is unavailable: copy
   `lib\arm` from the original author's machine / another preserved VS install into
   `14.44.35207\lib\arm`. Note the ARM32 **harness** (C++/CX, `bin\Hostx64\arm\cl.exe`) needs
   this too.
2. **Install Ruby** for the WebCore bindings generators (`winget install RubyInstallerTeam.Ruby`
   or add an existing install to PATH). A *fresh* ARM32 configure + WebCore build runs the IDL
   generators; the x64 tree never needed Ruby because it is pre-built.
3. **Disk budget:** a fresh `build-arm32-gpu` tree + `deps-build\` needs ~12–15 GB on top of the
   existing 7.55 GB x64 tree. 40.9 GB is enough, but do not add anything else to `C:` during the
   build. If space gets tight, `build-arm32-webcore` (rung 1) is the cheapest to keep.

## 2. Step 1 — vcpkg `arm-uwp` third-party stack

```powershell
cd C:\vcpkg
.\vcpkg install curl openssl libxml2 sqlite3 zlib bzip2 brotli libjpeg-turbo libpng libwebp expat pixman freetype --triplet arm-uwp --allow-unsupported
```

- The triplet file lives in the repo at `Src\port\vcpkg-triplets\arm-uwp.cmake` — it is **not**
  in `C:\vcpkg\triplets\`. Either copy it there or pass `--overlay-triplets
  $env:APOTHEOSIS_ROOT\Src\port\vcpkg-triplets`.
- **Do NOT install the vcpkg `icu` port.** Same trap as x64 (AGENTS.md): it builds ICU 78 and
  would load a second ICU next to the ICU-75 engine → conflict. The ARM line uses
  `C:\icu-arm-uwp` (step 2).
- **Do NOT install `harfbuzz` or `fontconfig` via vcpkg** — meson cannot cross-compile to UWP on
  this machine (BUILD_FAILED, same as the x64 recovery). The repo has dedicated source builds:
  `build-harfbuzz-arm32.ps1`, `build-fontconfig-arm32.ps1` (§4).
- Expect a long build (arm-uwp is a cross-compile for every package; do it on a fresh boot).
- After install: `pkg-config` path for the configure script is
  `C:\vcpkg\installed\arm-uwp\lib\pkgconfig` — it must exist (configure-gpu-arm32.ps1 sets
  `PKG_CONFIG_PATH` to it).

## 3. Step 2 — ICU 75 for ARM (`C:\icu-arm-uwp`)

The engine is compiled against **ICU 75** (`icuuc75.dll`/`icuin75.dll`/`icudt75l.dat`). The
x64 line recovered its ICU-75 import libs from the *preserved x64 AppPackages unpack* with
`Src\port\make-icu75-libs.ps1` (dumpbin → .def → `lib.exe /machine:X64`). **For ARM there is
no preserved appx**, so:

- The **DLLs** (`icuuc75.dll`, `icuin75.dll`, `icudt75.dll` in ARM32 flavor) must come from
  somewhere external: the original author's backup / another machine, or a source-build of
  ICU 75 for `thumbv7-uwp` (ICU does not ship official ARM32 Windows binaries). This is the
  **hardest external dependency** — treat finding it as a prerequisite, not a build step.
- Once the ARM DLLs exist, clone the make-icu75-libs.ps1 procedure for ARM:
  `dumpbin /EXPORTS <arm dll>` → `.def` → `lib.exe /machine:ARM /def:...` →
  `C:\icu-arm-uwp\lib\{icuuc,icuin,icudt}.lib`, and drop the DLLs + `icudt75l.dat` into
  `C:\icu-arm-uwp\bin`. (`make-icu75-libs.ps1` is currently hardcoded x64 — `/machine:X64`,
  `C:\icu-x64-uwp\lib`; an ARM copy needs `/machine:ARM`.)

## 4. Step 3 — ANGLE for ARM (`Src\angle\arm` + `Src\angle\include`) — ✅ ALREADY SATISFIED

**This step is DONE — nothing to re-obtain.** `Src\angle\arm` is populated with genuine ARM32
binaries (verified Aug 16 with `llvm-readobj --file-headers`, all four files
`IMAGE_FILE_MACHINE_ARMNT (0x1C4)` + `IMAGE_FILE_32BIT_MACHINE`):

| File | Size | Notes |
|---|---|---|
| `libEGL.dll` | 32 912 B | imports `libGLESv2.dll`, `VCRUNTIME140_APP.dll` |
| `libEGL.lib` | 16 066 B | import lib (`__IMPORT_DESCRIPTOR_libEGL`), has `eglInitialize` / `eglCreateWindowSurface` / `eglMakeCurrent` / `eglSwapBuffers` / `eglSwapBuffersWithDamageEXT` |
| `libGLESv2.dll` | 1 392 784 B | imports `d3d11.dll` + `D3DCOMPILER_47.dll` + `api-ms-win-core-winrt-*` + `MSVCP140_APP.dll` → **Store/App-Container D3D11 flavor**, the FL9_3 path the device needs |
| `libGLESv2.lib` | 248 736 B | import lib |

Internal file dates are 2016-12-28 (original ANGLE-for-Windows-Store build); they were copied
onto this machine 2026-08-14. Distinct files from the x64 set (different md5; x64 = `AMD64`).

> ⚠️ **PATH TRAP — this is what made an earlier audit (and a second AI agent) report "ANGLE ARM
> missing entirely".** There is **no `angle\` directory at the repo root**. The only ANGLE tree is
> **`Src\angle\`**. A `Test-Path "$env:APOTHEOSIS_ROOT\angle\arm"` returns `False` and is the
> **wrong check**. The correct check is `Test-Path "$env:APOTHEOSIS_ANGLE\arm"`, because
> `setenv.ps1:13` sets `APOTHEOSIS_ANGLE = <root>\Src\angle`.

All three real consumers already point at the populated directory:

- `Src\setenv.ps1:13` → `APOTHEOSIS_ANGLE = <root>\Src\angle`
- `Src\port\link-driver-gpu-arm32.ps1:43` → `/LIBPATH:"$env:APOTHEOSIS_ANGLE\arm"`
- `Src\harness\Harness.vcxproj:65,150-151` → `$(ProjectDir)..\angle\arm` (= `Src\angle\arm`) for the
  `Release|ARM` library path and as `DeploymentContent` for both DLLs

Provenance remains unrecorded (no NuGet package id in the repo), so **do not delete or "clean"
`Src\angle\arm`** — it is not reproducible from anything on this machine. Back it up (≈1.7 MB)
before any repo-wide cleanup.

## 5. Step 4 — deps-build source libs (Cairo / fontconfig / harfbuzz)

```powershell
. .\Src\setenv.ps1
pwsh -File .\Src\port\make-icu75-libs.ps1        # x64-only today — ARM copy needed (see §3)
pwsh -File .\Src\port\build-cairo-arm32.ps1
pwsh -File .\Src\port\build-fontconfig-arm32.ps1
pwsh -File .\Src\port\build-harfbuzz-arm32.ps1
```

These land in `deps-build\`. The `-arm32` variants are the ARM line; the `-x64` variants are
the x64 line — do not mix. Cairo's other deps (pixman/freetype/zlib/png) come from vcpkg
`arm-uwp` (§2). Do not re-enable vcpkg harfbuzz/fontconfig.

## 6. Step 5 — configure + build the engine (multi-hour)

```powershell
. .\Src\setenv.ps1
pwsh -File .\Src\port\configure-gpu-arm32.ps1
& "C:\Program Files\CMake\bin\ninja.exe" -C build-arm32-gpu WebCore -j1
```

- `configure-gpu-arm32.ps1` writes **`build-arm32-gpu`** (post-sync naming; the old
  `build-clang-gpu` name is gone everywhere). Flags are documented in the script header and
  `ARM32-BUILD-GUIDE.md` §4 — the essentials: clang `thumbv7-unknown-windows-msvc`,
  `WK_WINUWP=1`, `/APPCONTAINER`, `ENABLE_JIT=ON`, `DFG_JIT/FTL_JIT` off, `USE_SYSTEM_MALLOC=ON`,
  `APOTHEOSIS_GPU=ON`, `CMAKE_PREFIX_PATH=C:\icu-arm-uwp;C:\vcpkg\installed\arm-uwp`,
  `CMAKE_{C,CXX}_COMPILER_WORKS` forced TRUE.
- **Upstream CMake edits — only ONE is actually required** (verified Aug 16):
  `OptionsWinUWP.cmake` needs the `APOTHEOSIS_GPU` branch — ✅ present at
  `WebKit\Source\cmake\OptionsWinUWP.cmake:202` (sets `USE_GRAPHICS_LAYER_TEXTURE_MAPPER`,
  `USE_TEXTURE_MAPPER`, `USE_ANGLE_EGL` ON). The second edit the script header asks for
  (`Source/CMakeLists.txt` must skip `add_subdirectory(ThirdParty/ANGLE)` under
  `APOTHEOSIS_GPU`) is **moot in this tree**: `Source/CMakeLists.txt:22` guards the add with
  `EXISTS ${CMAKE_SOURCE_DIR}/ThirdParty/ANGLE/CMakeLists.txt`, and
  `WebKit\Source\ThirdParty\ANGLE` **does not exist** in this sparse checkout → the condition is
  already false, no in-tree ANGLE is ever configured or built. (Stale `ANGLE.vcxproj` files under
  `build-x64-gpu\Source\ThirdParty\ANGLE\` are leftovers from an older configure; no ANGLE lib was
  ever produced there.) Do not "restore" ThirdParty/ANGLE — the prebuilt `Src\angle` is what both
  lines link.
- `-j1` is mandatory (unified-source clang jobs exhaust 4 GB RAM; a killed job leaves a tree
  ninja won't recover). **Never hand-stamp an object** in these trees.
- ⚠️ **`harness-cmd.bat` is a SHARED single-arch capture — the one real x64/ARM32 collision
  point in the repo.** It currently holds an **ARM32** capture (`thumbv7-unknown-windows-msvc`,
  8× `build-arm32-webcore` paths). Consumers: the ARM scripts
  (`compile-driver-arm32.ps1`, `compile-driver-gpu-arm32.ps1`, `compile-driver-jit-arm32.ps1`,
  `build-driver-dll-arm32.ps1`) **and** the legacy x64 ones (`compile-driver-x64.ps1`,
  `link-driver-x64.ps1`, `recompile-stubs-x64.ps1`, `hx.ps1`), which rewrite the target/dir.
  The **active x64 GPU line is immune** — `compile-driver-gpu-x64.ps1` and
  `link-driver-gpu-x64.ps1` do not read this file at all (0 references). Re-capturing it for ARM
  is therefore safe for the current x64 workflow, but if you ever need the legacy x64 scripts,
  split the file per arch (`harness-cmd-arm32.bat` / `harness-cmd-x64.bat`) instead of flipping it
  back and forth.
- If the configure changed the include/define set, re-capture `harness-cmd.bat`
  (`ninja -C build-arm32-gpu -t commands <some WebCore obj>` → take the clang-cl line). The
  repo copy is an ARM32 capture (`thumbv7`, `build-arm32-webcore`) and the compile scripts
  rewrite that dir → `build-arm32-gpu`.
- **Skip rungs**: the ladder is `build-arm32-webcore` (soft) → `build-arm32-jit` →
  `build-arm32-gpu`. For a pure recovery, going straight to `build-arm32-gpu` is acceptable;
  keep rung 1 if disk allows (cheapest insurance, soft-render fallback for the device).

## 7. Step 6 — port driver (`WebCoreDriver-gpu.lib`)

```powershell
pwsh -File .\Src\port\link-driver-gpu-arm32.ps1
```

Compiles all 12 TUs to **`.arm32.obj`** (post-sync suffix; old `.gpu.obj` name is gone) and
archives `WebCoreDriver-gpu.lib` with `llvm-lib`. Two sync-era fixes are already in:
the `$P = "$env:APOTHEOSIS_ROOT\port"` path bug is fixed (it now derives `Src\port` like the
x64 scripts — ARM32-BUILD-GUIDE §9.10's warning is outdated, see the note at the bottom of this
file), and `recompile-stubs-arm32.ps1` was renamed `recompile-stubs-x64.ps1` (it was an x64
script). The lld `/DLL /MACHINE:ARM` probe inside the script surfaces unresolved symbols early.

## 8. Step 7 — harness appx (Release|ARM)

```powershell
msbuild Src/harness/Harness.vcxproj /p:Configuration=Release /p:Platform=ARM /p:AppxPackage=true
```

- `Harness.vcxproj` has exactly `Release|ARM` and `Release|x64`. The ARM configuration links the
  static engine (`libEGL.lib`, `libGLESv2.lib`, `WebCoreDriver-gpu.lib`, `WebCore.lib`,
  `JavaScriptCore.lib`, `PAL.lib`, `WTF.lib`, JSC+WTF again, then curl/OpenSSL, graphics/text
  stack, ICU) with `/FORCE:MULTIPLE /NODEFAULTLIB:libcmt.lib` and library dirs
  `..\angle\arm`, `C:\vcpkg\installed\arm-uwp\lib`, `C:\icu-arm-uwp\lib`. If WindowsApp/PSL
  symbols fail, that is the documented asymmetry vs x64 (ARM32-BUILD-GUIDE §6) — `arm32-uwp-env.ps1`
  puts `um\arm\WindowsApp.lib` on `LIB`.
- Bump `<Identity Version>` in `Package.appxmanifest`; the deploy `-Ver` must match. Package
  `cacert.pem`, `Assets\fonts`, and `Microsoft.VCLibs.140.00 (ARM)` dependency (most Lumias
  that run another `-Reborn` app have it). Measure the appx with `.Length`, never `ls -l`.
- The appx version is the **same number as x64** — one shared manifest; MSBuild overrides
  `ProcessorArchitecture` per platform.

## 9. Step 8 — deploy + verify (manual, phone)

```powershell
pwsh -File .\Src\tools\deploy-launch.ps1 -Ip <device-ip> -Ver <version>
```

Use `Src\tools\Deploy-Robust.ps1` (phone Wi-Fi drops; retry is the norm). `auto-diag2.ps1` only
arms on `autodiag.txt`. Climb ARM32-BUILD-GUIDE §8's ladder in order: launch + `log.txt` +
`jitresult.txt` → `WebCoreRenderHtml` static paint → `https://example.com` (`vua=1 sheets=1
pending=0`) → subresources (HN) → real events → JIT → GPU on D3D11 FL9_3 → smooth scroll/pinch.
The GPU ladder includes the new check: after `WebCoreGpuInit`, the surface must track ContentArea
via `WebCoreGpuResize` — the phone's 1440×2560 (L950) / 720×1280 (L640) must **not** be stretched
to a fixed 720×1080 surface (the x64 distortion bug; see AGENTS.md "Rendering distortion").

## 10. Landmines checklist (all current as of Aug 16)

1. `std::partial_ordering` sret ABI split — apply the `ALWAYS_INLINE` WK_WINUWP header fix
   **before** first deploy (memory: `partial-ordering-sret-abi-split`).
2. Exceptions off (`_HAS_EXCEPTIONS=0`, `/EHs-c-`) — thumbv7 cannot lower `cleanupret`.
3. `Frame::Navigate` banned → `0xc000027b`; navigate via the C ABI.
4. Ninja trees unreliable — `-j1`, no hand-stamped objects.
5. GPU gated on `g_gpuActive` (default false; unconditional compositing = silent `__fastfail`
   on device). Failure → Cairo soft + EmptyChromeClient (zero regression).
6. C ABI header in **two copies** (`Src\port\WebCoreDriver.h` + `Src\harness\WebCoreDriver.h`)
   — keep in sync.
7. Threading: present only on the engine thread; UI never synchronously waits on the engine.
8. `WebCoreSetCACertBlob` exactly once (curl `CURL_BLOB_NOCOPY` into a Vector owned by
   CurlContext) — second call frees memory curl still holds → intermittent
   `curlcode=60 issuer certificate (20)`. `_putenv_s` also not thread-safe.
9. ARM NEON float→int saturation differs from x87 — Cairo clamp
   (`#if defined(__ARM_PCS_VFP) || defined(__thumb__)`) must be in the tree.
10. CJK sentinel fix (`FontPlatformData.cpp:182` `if (size)`) + `WebCoreGpuResize` are in the
    shared WK_WINUWP source → ARM inherits both automatically.

## 11. Notes that supersede older docs

- **ARM32-BUILD-GUIDE.md §9.10** ("`link-driver-gpu-arm32.ps1` computes `$P =
  "$env:APOTHEOSIS_ROOT\port"`") — **OUTDATED**: the `$P` bug was fixed Aug 16 (derives
  `Src\port` via `$PSScriptRoot`, matching the x64 scripts). All ARM scripts were also
  bulk-renamed `build-clang-*` → `build-arm32-*` and obj suffixes unified
  (`.arm32.obj` / `.arm32-jit.obj` / `.arm32-soft.obj`) the same day.
- **ARM32-BUILD-GUIDE.md §0** free-space figures (10.3 GB) — outdated; 52.3 GB free now.
- The "re-downloadable" claim for `angle/arm` + `angle-windowsstore` (AGENTS.md/guide) is
  **irrelevant now: `Src\angle\arm` is present and verified ARM32** (§4). The earlier statement in
  this very file that ANGLE-arm was missing and had to be located first was a **wrong-path check**
  (`<root>\angle` vs the real `<root>\Src\angle`). The only remaining external blocker is
  **ICU 75 ARM** (§3). Because ANGLE's provenance is still unrecorded, treat `Src\angle\arm`
  as irreplaceable and back it up (≈1.7 MB) rather than re-deriving it.
- **`CLAUDE.md`'s build-config table** claims `build-x64-gpu` includes `FTL_JIT` — **wrong**:
  `configure-gpu-x64.ps1:40` sets `-DENABLE_FTL_JIT=OFF`, same as ARM. A full flag diff of
  `configure-gpu-x64.ps1` vs `configure-gpu-arm32.ps1` shows **exactly one** difference:
  ARM adds `-DUSE_SYSTEM_MALLOC=ON` (x64 keeps bmalloc). That single divergence is the only
  engine-config gap between the "emulator" and the device line — see
  `Doc/X64-AS-ARM-EMULATOR.md`.
