# Project Apotheosis — Phase 2: Repo Reorganization & Build-from-Scratch

> Updated: June 30, 2026
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

1. **`Src/setenv.ps1`** created — sets `$env:APOTHEOSIS_ROOT` and sub-variables.
2. **87 path replacements** across all `.ps1` scripts.
3. **83 quoting fixes** — `'$env:...'` → `"$env:..."` so PowerShell expands at runtime.
4. **`harness-cmd.bat`** — uses `%APOTHEOSIS_ROOT%` (CMD syntax).
5. **`Harness.vcxproj`** — uses `$(ProjectDir)..\` relative paths.

**Result:** Zero `E:\Apotheosis\` references remain in tracked files.

### 1.3 Files Created/Modified

| File | Change |
|------|--------|
| `Src/setenv.ps1` | **New** — root env var setting |
| `Src/harness/Harness.vcxproj` | Fixed paths |
| `Src/port/harness-cmd.bat` | `%APOTHEOSIS_ROOT%` for cmd.exe |
| `Src/port/*.ps1` (22 files) | Path fixes |
| `Src/tools/*.ps1` (17 files) | Path fixes |
| `Doc/PLAN.md` | Updated with all progress |
| `Doc/Summary.md` | Updated |
| `Doc/WIKI_EN.md`, `WIKI_CN.md` | **New** - Wiki in 3 languages |
| `README.md`, `_CN.md`, `_RU.md` | **Rewritten** — clean, structured |

---

## 2. Current Build Status (June 30, 2026)

### 2.1 x64-uwp Build Progress

| Step | Component | Status | Notes |
|------|-----------|--------|-------|
| 1 | LLVM/clang-cl 22.1.8 | ✅ | |
| 2 | Ninja | ✅ | VS2022 bundled |
| 3 | WebKit source (webkitgtk-2.52.4) | ✅ | commit e4ab5336 |
| 4 | ANGLE x64 binaries | ✅ | NuGet → `Src\angle\x64\` |
| 5 | vcpkg x64-uwp deps (16 pkgs) | ✅ ALL INSTALLED | Community triplet |
| 6 | ICU x64-uwp | ✅ | `C:\icu-x64-uwp\` |
| 7 | SQLite3 UWP | ✅ | amalgamation, `SQLITE_OS_WINRT=1` |
| 8 | CMake configure | ✅ | `build-x64-gpu` generated |
| 9 | WTF compiled | ✅ | 3 WK_WINUWP patches |
| 10 | bmalloc compiled | ✅ | getpid→GetCurrentProcessId |
| 11 | PAL headers generated | ✅ | 1203 ninja steps |
| 12 | **JavaScriptCore** | ⛔ **BLOCKED** | LowLevelInterpreter.cpp: GNU inline assembly incompatible with clang-cl MSVC mode |
| 13 | WebCore | ❌ | |
| 14 | Port driver | ❌ | |
| 15 | Harness appx | ❌ | |

### 2.2 PAL Build Analysis

Key discovery: **PAL is an OBJECT library** — no `PAL.lib` target. PAL's `.obj` files (19 `.cpp` files) compile and link directly into `WebCore.dll`. The `ninja PAL` target only triggers header generation (1203 steps). PAL compilation happens as part of `ninja WebCore`.

### 2.3 JavaScriptCore Blocker

**Error:** `LowLevelInterpreter.cpp` inline assembly uses GNU AT&T syntax (`int $3`, `push %rbp`, `movq %rsp, %rbp`, etc.) which clang-cl with `--target=x86_64-unknown-windows-msvc` cannot parse. clang-cl defaults to MASM/Intel syntax for x86-64 targets.

**Impact:** Blocks both `LowLevelInterpreterLib` and `JavaScriptCore` targets.

**Potential fixes:**
1. Compile with `-fgnu-inline-asm` flag (may not exist in clang-cl)
2. Switch to clang (not clang-cl) for this file
3. Add Intel-syntax inline assembly under `#if defined(WK_WINUWP)` guard
4. Use cl.exe (MSVC) for this translation unit with `-D__ASSEMBLER__` tricks
5. Skip LLInt C++ backend entirely (use JIT-only for x64)

### 2.4 Ninja Build Observations

| Issue | Solution |
|-------|----------|
| Tool kills subprocesses on timeout | Always use `ninja -j8` with 7200000ms timeout |
| Stale `.ninja_lock` after killed build | Must delete before resuming |
| Zombie clang-cl processes | Kill before each build |
| Build log corruption after kill | Clean `.ninja_log` to force re-evaluation |
| Inline assembly error in LowLevelInterpreter.cpp | **Code-level blocker** — needs WK_WINUWP patch |

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

Cross-build ICU 78 for target arch.

### Step 5: Acquire ANGLE

Get `libEGL.lib` + `libGLESv2.lib` for target arch.

### Step 6: Configure + Build WebKit

```powershell
. .\Src\setenv.ps1
pwsh -File Src/port/configure-gpu.ps1    # ARM32
# or
pwsh -File Src/port/configure-gpu-x64.ps1 # x64
ninja -C build-x64-gpu -j8 JavaScriptCore WebCore
```

### Step 7: Link Port Driver

```powershell
pwsh -File Src/port/link-driver-gpu.ps1    # ARM32
# or
pwsh -File Src/port/link-driver-gpu-x64.ps1 # x64
```

### Step 8: Build Harness Appx

```powershell
pwsh -File Src/port/build-harness.ps1
```

---

## 4. Path System Reference

### Environment Variables (set by `Src/setenv.ps1`)

| Variable | Points To |
|----------|-----------|
| `APOTHEOSIS_ROOT` | Repo root |
| `APOTHEOSIS_PORT` | `Src\port\` |
| `APOTHEOSIS_HARNESS` | `Src\harness\` |
| `APOTHEOSIS_TOOLS` | `Src\tools\` |
| `APOTHEOSIS_ANGLE` | `Src\angle\` |
| `APOTHEOSIS_CRASH` | `crash\` |
| `APOTHEOSIS_VCPKG` | `C:\vcpkg` |
| `APOTHEOSIS_ICU` | `C:\icu-arm-uwp` |
| `APOTHEOSIS_ARCH` | Target arch (`x64`/`arm`) |

---

## 5. Repository Layout (Current)

```
<repo>\                 ← APOTHEOSIS_ROOT
├── WebKit\             ← webkitgtk-2.52.4 (gitignored)
├── build-x64-gpu\      ← x64 build output (gitignored)
├── Src\
│   ├── port\           ← Port layer + build scripts (tracked)
│   ├── harness\        ← UWP app (C++/CX, XAML)
│   ├── tools\          ← WDP deploy/diag scripts
│   ├── angle\          ← ANGLE headers + binaries
│   └── setenv.ps1      ← Environment setup
├── Doc\                ← Documentation (tracked)
│   ├── PLAN.md         ← Development plan
│   ├── Summary.md      ← Engineering summary
│   ├── Summary_1.md    ← This file
│   ├── WIKI_EN.md      ← English wiki
│   ├── WIKI_RU.md      ← Russian wiki
│   ├── WIKI_CN.md      ← Chinese wiki
│   ├── HANDOFF.md      ← Phase 0 handoff
│   ├── M2-HANDOFF.md   ← GPU rendering
│   ├── MEDIA-PLAN.md   ← Video/audio
│   └── WEBKIT-UPGRADE.md
├── AGENTS.md           ← Codex guidance
├── README.md / _CN.md / _RU.md
```

---

## 6. Lessons Learned

### Build Flow

1. **PAL is an OBJECT library** — no `.lib` file. Objects link into WebCore.
2. **Ninja log corruption** — killed builds corrupt `.ninja_log`. Always delete before resume.
3. **Lock file discipline** — `.ninja_lock` prevents concurrent ninja. Must be removed manually.
4. **Process tracking** — opencode's tool kills descendant processes. Long builds need max timeout.
5. **Stale clang-cl** — zombie processes stall indefinitely. Kill before each build.
6. **GNU inline assembly** — `LowLevelInterpreter.cpp` uses AT&T syntax incompatible with clang-cl x64.

### Documentation Updates

- All 10 documentation files updated (PLAN, Summary, 3 READMEs, 3 Wikis, Summary_1)
- Clean, structured English README replaces garbled machine translation
- Wiki created in all 3 languages (EN/CN/RU)
- Full build status and blocker documented

---

*"One `E:\Apotheosis\` at a time."*
