# Project Apotheosis — Phase 2: Repo Reorganization & Build-from-Scratch

> Updated: June 28, 2026
> Author: AI-assisted refactoring session
> See also: `Summary.md` (architecture & origins), `PLAN.md` (development plan)

---

## 1. What Was Done

This document covers the **repo reorganization and path migration** work — making the project **buildable from scratch on a new machine** without depending on the original author's `E:\Apotheosis\` layout, pre-built binaries, or existing build directories.

### 1.1 Repository Cleanup

The `port/` directory contained ~60+ debug/repro artifacts from the original author's compiler-triage sessions:

| Category | Examples | Count |
|----------|----------|-------|
| Reproducers | `repro_class_template_alts_*.cpp`, `repro_odr_address_*.cpp`, `repro_variant_return_*.cpp` | ~46 |
| Mangle tests | `mangle-repro*.cpp` | ~5 |
| Batch scripts | `_repro_*.bat`, `_driver_compile*.bat` | ~19 |
| Debug logs | `undef-*.txt`, `*.obj`, `*.log` | ~15 |
| Draft code | `draft-*.cpp` | ~3 |

**All removed.** The `port/` directory now contains only source files, build scripts, and configuration files.

### 1.2 Path Migration: `E:\Apotheosis\` → `$env:APOTHEOSIS_ROOT`

The original author used a flat layout at `E:\Apotheosis\` with all paths hardcoded:

```
E:\Apotheosis\port\       →  <repo>\Src\port\
E:\Apotheosis\harness\    →  <repo>\Src\harness\
E:\Apotheosis\tools\      →  <repo>\Src\tools\
E:\Apotheosis\angle\      →  <repo>\Src\angle\
E:\Apotheosis\WebKit\     →  <repo>\WebKit\
E:\Apotheosis\build-*     →  <repo>\build-*
E:\Apotheosis\crash\      →  <repo>\crash\
```

A systematic migration was performed:

1. **`Src/setenv.ps1`** created — sets `$env:APOTHEOSIS_ROOT` and sub-variables (`APOTHEOSIS_PORT`, `APOTHEOSIS_HARNESS`, `APOTHEOSIS_TOOLS`, `APOTHEOSIS_ANGLE`, `APOTHEOSIS_CRASH`). All scripts should source this before use.

2. **87 path replacements** across all `.ps1` scripts — `E:\Apotheosis\` → `$env:APOTHEOSIS_*` using appropriate sub-variable:
   - `E:\Apotheosis\port\` → `$env:APOTHEOSIS_PORT\`
   - `E:\Apotheosis\tools\` → `$env:APOTHEOSIS_TOOLS\`
   - `E:\Apotheosis\harness\` → `$env:APOTHEOSIS_HARNESS\`
   - `E:\Apotheosis\angle\` → `$env:APOTHEOSIS_ANGLE\`
   - `E:\Apotheosis\crash\` → `$env:APOTHEOSIS_CRASH\`
   - Everything else → `$env:APOTHEOSIS_ROOT\`

3. **83 quoting fixes** — all `'$env:APOTHEOSIS_*'` changed to `"$env:APOTHEOSIS_*"` so PowerShell expands environment variables at runtime.

4. **`harness-cmd.bat`** fixed — uses `%APOTHEOSIS_ROOT%` (CMD variable syntax) since it's executed by `cmd.exe`, not PowerShell.

5. **`Harness.vcxproj`** fixed — all `E:\Apotheosis\` paths replaced with `$(ProjectDir)..\` relative paths:
   - `E:\Apotheosis\angle\include` → `$(ProjectDir)..\angle\include`
   - `E:\Apotheosis\angle\arm` → `$(ProjectDir)..\angle\arm`
   - `E:\Apotheosis\port` → `$(ProjectDir)..\port`
   - Added `$(APOTHEOSIS_BUILD_GPU)` conditional for build output directory

**Result:** Zero `E:\Apotheosis\` references remain in any `.ps1`, `.bat`, `.cmake`, or `.vcxproj` file.

### 1.3 Files Created/Modified

| File | Change |
|------|--------|
| `Src/setenv.ps1` | **New** — root env var setting + validation |
| `Src/harness/Harness.vcxproj` | Fixed paths, added `APOTHEOSIS_BUILD_GPU` conditional |
| `Src/port/harness-cmd.bat` | Changed to `%APOTHEOSIS_ROOT%` for cmd.exe |
| `Src/port/*.ps1` (22 files) | All `E:\Apotheosis\` → `$env:APOTHEOSIS_*` |
| `Src/tools/*.ps1` (17 files) | All `E:\Apotheosis\` → `$env:APOTHEOSIS_*` |
| `AGENTS.md` | Rewritten in Russian, path system documentation |
| `Doc/PLAN.md` | Updated with migration, multilingual, x64 sections |
| `Doc/Summary.md` | Updated with all new sections |
| `Doc/WEBKIT-UPGRADE.md` | **New** — 2.52.4→2.53.4 analysis |

---

## 2. Current Build Status

### 2.1 What's Present (Already in Repo)

| Component | Status |
|-----------|--------|
| Port layer source (`WebCoreDriver.cpp`, stubs, clients) | ✅ Complete |
| Harness UWP app (C++/CX + XAML) | ✅ Complete |
| ANGLE headers (`Src/angle/include/`) | ✅ Present |
| CMake toolchains (ARM32 + x64) | ✅ Created |
| vcpkg triplets (arm-uwp + x64-uwp) | ✅ Created |
| Build/link scripts (ARM32 + x64) | ✅ Created |
| `.resw` files (en, zh, ru) | ✅ Created |
| x64 precompiled harness objects | ✅ Present |

### 2.2 What's Missing (Must Be Installed/Built)

| Component | How to Get | Status |
|-----------|------------|--------|
| **WebKit source** (webkitgtk-2.52.4) | `git clone --branch webkitgtk-2.52.4 https://github.com/WebKit/WebKit.git` | ❌ |
| **LLVM/clang-cl** | `winget install LLVM.LLVM` | ❌ |
| **vcpkg** | `git clone https://github.com/microsoft/vcpkg C:\vcpkg; bootstrap-vcpkg.bat` | ❌ |
| **Ruby** (JSC offlineasm) | `winget install Ruby` | ❌ |
| **Strawberry Perl** (create_hash_table) | `winget install StrawberryPerl.StrawberryPerl` | ❌ |
| **Ninja** (WebKit build) | `winget install Ninja-build.Ninja` | ❌ |
| **ICU 78** for target arch | Custom cross-build | ❌ |
| **ANGLE binaries** (libEGL, libGLESv2) | Windows Store ANGLE NuGet or build from source | ❌ |
| **vcpkg deps** for target arch | `vcpkg install cairo pixman freetype harfbuzz fontconfig expat libcurl openssl libxml2 sqlite3 ...` | ❌ |
| **WebKit build output** (WTF.lib, JSC.lib, PAL.lib, WebCore.lib) | CMake + Ninja build | ❌ |
| **Port driver** (WebCoreDriver-gpu.dll) | clang-cl + lld-link | ❌ |
| **Harness appx** | MSBuild | ❌ |

### 2.3 Machine Environment

The build machine is a **x64 Windows PC** with VS2022 Community, MSVC v143 (14.44.35207), Windows SDK 19041 (ARM32 libs present). ARM32 cross-compiler (`cl.exe` for ARM) is available.

---

## 3. Build-from-Scratch Procedure

### Step 1: Install Prerequisites

```powershell
winget install LLVM.LLVM Ninja-build.Ninja Ruby StrawberryPerl.StrawberryPerl
```

### Step 2: Set up vcpkg

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
# Install for target arch (choose one):
C:\vcpkg\vcpkg install cairo pixman freetype harfbuzz fontconfig expat libcurl openssl libxml2 sqlite3 libpng libjpeg-turbo libwebp zlib bzip2 brotli --triplet arm-uwp --overlay-triplets=Src/port/vcpkg-triplets
```

### Step 3: Fetch WebKit

```powershell
git clone --branch webkitgtk-2.52.4 --depth 1 --filter=blob:none https://github.com/WebKit/WebKit.git
```

### Step 4: Build ICU

Cross-build ICU 78 for target arch (ARM32 or x64-uwp). Place at `C:\icu-arm-uwp\` or `C:\icu-x64-uwp\`.

### Step 5: Acquire ANGLE

Get `libEGL.lib` + `libGLESv2.lib` + `libEGL.dll` + `libGLESv2.dll` for target arch. Place in `Src/angle/arm/` or `Src/angle/x64/`.

### Step 6: Configure + Build WebKit

```powershell
. .\Src\setenv.ps1
pwsh -File Src/port/configure-gpu.ps1    # ARM32
# or
pwsh -File Src/port/configure-gpu-x64.ps1 # x64

ninja -C build-clang-gpu WTF JavaScriptCore PAL WebCore
```

### Step 7: Link Port Driver

```powershell
pwsh -File Src/port/link-driver-gpu.ps1    # ARM32 → WebCoreDriver-gpu.dll
# or
pwsh -File Src/port/link-driver-gpu-x64.ps1 # x64 → WebCoreDriver-x64.dll
```

### Step 8: Build Harness Appx

```powershell
pwsh -File Src/port/build-harness.ps1
# Output: Src/harness/AppPackages/Harness/Harness_<ver>_<arch>_Test/
```

---

## 4. Path System Reference

### Environment Variables (set by `Src/setenv.ps1`)

| Variable | Points To | Example |
|----------|-----------|---------|
| `APOTHEOSIS_ROOT` | Repo root | `C:\...\Apotheosis` |
| `APOTHEOSIS_PORT` | `Src\port\` | `$APOTHEOSIS_ROOT\Src\port` |
| `APOTHEOSIS_HARNESS` | `Src\harness\` | `$APOTHEOSIS_ROOT\Src\harness` |
| `APOTHEOSIS_TOOLS` | `Src\tools\` | `$APOTHEOSIS_ROOT\Src\tools` |
| `APOTHEOSIS_ANGLE` | `Src\angle\` | `$APOTHEOSIS_ROOT\Src\angle` |
| `APOTHEOSIS_CRASH` | `crash\` | `$APOTHEOSIS_ROOT\crash` |
| `APOTHEOSIS_VCPKG` | `C:\vcpkg` | — |
| `APOTHEOSIS_ICU` | `C:\icu-arm-uwp` | — |
| `APOTHEOSIS_ARCH` | Target arch | `x64` (default) or `arm` |
| `APOTHEOSIS_BUILD_GPU` | Build output dir | Used in vcxproj conditional |

### Usage in Scripts

```powershell
# In PowerShell scripts (double quotes for expansion):
. "$env:APOTHEOSIS_PORT\arm32-uwp-env.ps1"
$cmd = (Get-Content "$env:APOTHEOSIS_PORT\harness-cmd.bat" -Raw).Trim()

# In batch files (CMD variable syntax):
%APOTHEOSIS_ROOT%\build-clang-webcore\...
%APOTHEOSIS_PORT%\harness-cmd.bat

# In vcxproj (MSBuild properties):
$(ProjectDir)..\angle\include
$(APOTHEOSIS_BUILD_GPU)   # conditional for build output
```

### External Dependencies (Absolute Paths)

These remain absolute as they are system-wide installs:

| Path | Contents |
|------|----------|
| `C:\vcpkg\installed\arm-uwp\lib\` | vcpkg ARM32 libs |
| `C:\icu-arm-uwp\lib\` | ICU 78 ARM32 libs |
| `C:\Program Files\Microsoft Visual Studio\2022\Community\` | VS2022 |
| `C:\Program Files (x86)\Windows Kits\10\` | Windows SDK |
| `%ProgramFiles%\LLVM\` | LLVM/clang-cl |

---

## 5. Repository Layout (Current)

```
<repo>\                 ← APOTHEOSIS_ROOT
├── WebKit\             ← webkitgtk-2.52.4 (to be cloned, gitignored)
├── build-*\            ← Build outputs (to be created, gitignored)
├── crash\              ← Crash dumps (to be created, gitignored)
├── Src\
│   ├── port\           ← Port layer + build scripts (tracked)
│   │   ├── WebCoreDriver.{cpp,h}
│   │   ├── PortChromeClient.{h,cpp}
│   │   ├── LoadingFrameLoaderClient.{h,cpp}
│   │   ├── PortPlatformStrategies.cpp
│   │   ├── PortNetworkStorageSession.{cpp,h}
│   │   ├── stubs-*.cpp
│   │   ├── Toolchain-ARM32-UWP-clang.cmake
│   │   ├── Toolchain-x64-UWP-clang.cmake
│   │   ├── configure-gpu.ps1 / configure-gpu-x64.ps1
│   │   ├── link-driver-gpu.ps1 / link-driver-gpu-x64.ps1
│   │   ├── compile-driver-gpu.ps1 / compile-driver-gpu-x64.ps1
│   │   ├── build-harness.ps1
│   │   ├── arm32-uwp-env.ps1
│   │   └── vcpkg-triplets/
│   │       ├── arm-uwp.cmake
│   │       └── x64-uwp.cmake
│   ├── harness\        ← UWP app (tracked)
│   │   ├── MainPage.xaml / .h / .cpp
│   │   ├── Package.appxmanifest
│   │   ├── Resources/{en-US,zh-Hans,ru-RU}/Resources.resw
│   │   └── *.pfx, *.cer (signing certs)
│   ├── tools\          ← WDP deploy/diag scripts (tracked)
│   ├── angle\include\  ← ANGLE headers (tracked)
│   ├── setenv.ps1      ← **New** — env var setup
├── Doc\                ← Documentation (tracked)
│   ├── Summary.md          ← Architecture & origins
│   ├── Summary_2.md        ← **New** — This file
│   ├── PLAN.md             ← Development plan
│   ├── WEBKIT-UPGRADE.md   ← 2.52.4→2.53.4 analysis
│   ├── HANDOFF.md          ← Phase 0 handoff
│   └── M2-HANDOFF.md       ← GPU rendering details
├── AGENTS.md           ← Codex/Copilot guidance
├── README*.md          ← Russian/Chinese/English READMEs
```

---

## 6. Lessons Learned

### Path Migration

1. **`E:\Apotheosis\` was everywhere** — 127 occurrences in scripts, config files, docs, and code comments. Systematic grep + replace pattern worked but required two passes (path replacement + quoting fix).

2. **PowerShell quoting matters** — `'$env:VAR'` is a literal string; `"$env:VAR"` expands the variable. The migration script's naive string replacement didn't account for this, requiring a second pass.

3. **`.bat` files need `%VAR%` syntax** — `cmd.exe` doesn't understand `$env:VAR`. The bat file content is read by PowerShell scripts and written to temp bats, but final execution is via `cmd /c`.

4. **`.vcxproj` uses MSBuild properties** — `$(ProjectDir)`, `$(SolutionDir)`, etc. Can't use PowerShell env vars here; must use relative paths or MSBuild properties.

5. **Sub-variables reduce typing** — `$env:APOTHEOSIS_PORT\` is clearer than `$env:APOTHEOSIS_ROOT\Src\port\` and makes the intent explicit.

### Build Flow Understanding

- The `harness-cmd.bat` file is a template containing the clang-cl invocation with all WebKit include paths. It's read by the compile scripts, extended with target-file-specific flags, written to temp `_driver_compile*.bat`, and executed.
- Three parallel WebKit builds (sw-only, JIT, GPU) share the same source tree with different CMake configurations.
- The port layer compiles with clang-cl (same as WebKit) but links with lld-link (not CMake) into a separate DLL loaded by the harness.

---

## 7. Next Steps

### Immediate (when laptop is available)
1. Install LLVM/clang-cl, Ninja, Ruby, Strawberry Perl via winget
2. Set up vcpkg
3. Clone WebKit source
4. Build ICU and ANGLE for target arch
5. Configure + build WebKit libs
6. Link port driver
7. Build harness appx
8. Deploy to device (ARM) or test locally (x64)

### Short-term
1. Upgrade to webkitgtk-2.53.4 (Low-Medium effort, mostly Skia)
2. Build ARM32 version for Lumia 950 testing
3. Refactor port layer — consolidate stubs, reduce ARM/x64 duplication
4. Add CI for build verification

---
## 8. Upstream Sync (gpu-path1, June 28 2026)

The upstream repository received 14 new commits after our fork. Key changes:

| Commit | Impact |
|--------|--------|
| `CryptoDigest → real SHA` | **Critical** — fixed all-zero digest that broke Subresource Integrity. Patched to `Src/port/stubs-crypto.cpp`. |
| `Anti-OOM` | Added BackForwardCache disable + MemoryCache caps + `WebCoreReleaseMemory()`. Patched to `WebCoreDriver.cpp/.h`. |
| `MinVersion 14393` | Lowered minimum Windows version for broader device support. Patched to `Package.appxmanifest`. |
| `Version bump 0.1.8.5` | Updated version. |
| `MEDIA-PLAN.md` | Video/audio playback roadmap — copied to `Doc/`. |
| `OOBE language selection` | Upstream added en/zh OOBE. Our 3-language i18n (.resw + GetStr()) is more advanced. |
| `XAML codegen workaround` | Upstream bypasses broken XamlCompiler. Our harness doesn't build yet, so deferred. |

**Files patched:** `stubs-crypto.cpp`, `Package.appxmanifest`, `WebCoreDriver.cpp`, `WebCoreDriver.h`

---

*"One `E:\Apotheosis\` at a time."*
