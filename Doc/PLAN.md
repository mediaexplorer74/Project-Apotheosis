# Project Apotheosis — Development Plan

> Updated: June 28, 2026
> See also: `Summary.md` (architecture), `Summary_2.md` (repo reorg), `WEBKIT-UPGRADE.md`

---

## 1. Repository Cleanup & Path Migration ✅

### Status: Completed

All `E:\Apotheosis\` hardcoded paths have been replaced with `$env:APOTHEOSIS_*` environment variables throughout `.ps1`, `.bat`, `.cmake` files. The `Harness.vcxproj` uses relative `$(ProjectDir)` syntax.

| Task | Status |
|------|--------|
| Remove `repro_*`, `mangle-repro*`, `_*.bat`, `*.obj`, `*.log` from `port/` | ✅ ~60 files deleted |
| Create `Src/setenv.ps1` with `APOTHEOSIS_ROOT` + sub-vars | ✅ |
| Fix 87 path occurrences in scripts | ✅ |
| Fix 83 quoting issues (single→double quotes) | ✅ |
| Fix `Harness.vcxproj` (vcxproj uses MSBuild properties) | ✅ |
| Fix `harness-cmd.bat` (bat uses `%VAR%` syntax) | ✅ |
| Verify zero `E:\Apotheosis\` in `.ps1`/`.bat`/`.cmake` | ✅ Clean |

### Key files created

| File | Purpose |
|------|---------|
| `Src/setenv.ps1` | Sets `$env:APOTHEOSIS_ROOT`, `PORT`, `HARNESS`, `TOOLS`, `ANGLE`, `CRASH` |
| `Doc/Summary_2.md` | Full repo reorganization documentation |

---

## 2. x64 Build Infrastructure ✅

### Status: Scripts created, dependencies needed

| File | Purpose |
|------|---------|
| `Src/port/Toolchain-x64-UWP-clang.cmake` | clang-cl targeting `x86_64-unknown-windows-msvc` |
| `Src/port/vcpkg-triplets/x64-uwp.cmake` | vcpkg overlay triplet for x64 UWP (VS2022 v143) |
| `Src/port/configure-gpu-x64.ps1` | CMake configure for `build-x64-gpu` |
| `Src/port/link-driver-gpu-x64.ps1` | lld-link to produce `WebCoreDriver-x64.dll` |

### Blocked on prerequisites (see Phase 4)

---

## 3. Multilingual UI (en/ru/zh) ✅

### Status: Completed

| Feature | Status |
|---------|--------|
| `.resw` files for en-US, zh-Hans, ru-RU | ✅ Created |
| `GetStr()` dual-source loading (`.resw` + fallback table) | ✅ Implemented |
| All hardcoded Chinese toasts replaced | ✅ 20+ locations |
| Language persistence (`settings.ini` → `lang=0|1|2`) | ✅ |
| `ComboBox` language selector in Settings | ✅ |

### Future improvements

1. Migrate XAML static labels to `x:Uid` binding
2. Auto-detect system language on first launch
3. Localize `kHomeHtml` (browser home page HTML)

---

## 4. WebKit Upgrade: 2.52.4 → 2.53.4

See `Doc/WEBKIT-UPGRADE.md` for full analysis.

**Status:** Research complete. **Effort:** Low-Medium (~2-3 days). Mostly Skia-focussed changes — low impact on our TextureMapper path.

---

## 5. Upstream Sync (gpu-path1, June 28)

### Status: Patches applied

The upstream `gpu-path1` branch received 14 new commits. Key changes and our sync status:

| Commit | Change | Our sync |
|--------|--------|----------|
| `22c9721` | Anti-OOM: memory pressure + disable back-forward cache | ✅ Patched `WebCoreDriver.cpp` |
| `1601b87` | MEDIA-PLAN.md (video/audio backend plan) | ✅ Copied to `Doc/` |
| `a5a1a20` | CryptoDigest → real OpenSSL SHA (fix SRI) | ✅ Patched `stubs-crypto.cpp` |
| `ca387e7` | OOBE language selection + back key | ⚠️ Analyzed; our i18n (3 langs + .resw) is more advanced |
| `aaa928d` | Custom XAML codegen (XamlCompiler workaround) | ⚠️ Not needed yet (not building for ARM) |
| `c18e0fd` | MinVersion lowered to 14393 | ✅ Patched `Package.appxmanifest` |
| `5ff1a18` | License cleanup (MIT + NOTICE) | ✅ Kept our LICENSE |
| `cc5cce6` | Deploy-Robust retry improvements | ✅ Already had in our version |
| `00d5427` | HTTP→HTTPS redirect fix | 🔄 In WebCore (needs WebKit source patch) |
| `ee69402` | LBrowser-style UI redesign | ⚠️ Different approach from our i18n UI |

### Files patched in our repo

| File | Change |
|------|--------|
| `Src/port/stubs-crypto.cpp` | `CryptoDigest` → real OpenSSL SHA (was all-zero) |
| `Src/harness/Package.appxmanifest` | Version `0.1.8.4`→`0.1.8.5`, MinVersion `15063`→`14393` |
| `Src/port/WebCoreDriver.cpp` | Added `BackForwardCache::setMaxSize(0)`, `MemoryCache` caps, `WebCoreReleaseMemory()` |
| `Src/port/WebCoreDriver.h` | Added `WebCoreReleaseMemory()` declaration |

### Status: Machine surveyed, tools need installation

| Step | Component | How | Status |
|------|-----------|-----|--------|
| 5.1 | **LLVM/clang-cl** | `winget install LLVM.LLVM` | ❌ Not installed |
| 5.2 | **Ninja** | `winget install Ninja-build.Ninja` | ❌ Not installed |
| 5.3 | **Ruby** (JSC offlineasm) | `winget install Ruby` | ❌ Not installed |
| 5.4 | **Strawberry Perl** | `winget install StrawberryPerl.StrawberryPerl` | ❌ Not installed |
| 5.5 | **vcpkg** | Clone + bootstrap | ❌ Not installed |
| 5.6 | **ICU 78** (target arch) | Custom cross-build | ❌ |
| 5.7 | **ANGLE binaries** (target arch) | NuGet or build from source | ❌ |
| 5.8 | **WebKit source** | `git clone --branch webkitgtk-2.52.4` | ❌ Not cloned |
| 5.9 | **vcpkg deps** | `vcpkg install cairo pixman ...` | ❌ |
| 5.10 | **Configure + build WebKit** | CMake + Ninja | ❌ |
| 5.11 | **Link port driver** | clang-cl + lld-link | ❌ |
| 5.12 | **Build harness appx** | MSBuild | ❌ |

### Build chain visualization

```
Prerequisites → vcpkg deps + ICU + ANGLE → WebKit libs → Port driver → Appx
                                                   ↗
                                            WebKit source
```

---

## 6. Critical Fixes (Technical Debt)

### 6.1. Path system ⚠️ RESOLVED
- ✅ `E:\Apotheosis\` removed from all scripts
- ✅ `$env:APOTHEOSIS_ROOT` + sub-variables in place
- ✅ `$(ProjectDir)` relative paths in vcxproj

### 6.2. Remaining
- **Certificate & signing**: Signing cert password in clear text; `CN=EdgeHTMLReborn` hardcoded
- **ARM build env**: VS2022+ doesn't ship ARM32 vcvars — `arm32-uwp-env.ps1` is a fragile workaround
- **SDK lock**: SDK 26100 removed ARM32 libs — locked to SDK 19041
- **ICU path hardcoded**: `C:\icu-arm-uwp\` not configurable (but documented in setenv.ps1)

---

## 7. Architecture (String Loading)

```
settings.ini → lang=0|1|2
     ↓
LoadSettings() → m_uiLang
     ↓
Any UI code → GetStr(m_uiLang, S_XXX)
     ↓
1. Try ResourceLoader::GetString("S_XXX")  ← .resw
2. Fall back to kStr[m_uiLang][S_XXX]      ← hardcoded table
```

---

## 8. Build Configurations

| Dir | Flags | Status |
|-----|-------|--------|
| `build-clang-webcore` | Cairo only, no JIT | ARM32 only (Phase 1b) |
| `build-clang-jit` | + JSC JIT | ARM32 only (JIT line) |
| `build-clang-gpu` | + GPU (TextureMapper+ANGLE) | ARM32 active; x64 scripts ready |
| `build-x64-gpu` | Same as GPU but x64 | ❌ Not built yet |

---

## 9. Roadmap

### Phase A: Tool Installation (next)

Concrete steps to set up the build environment on this x64 machine:

| # | Step | Command / How | Path concern |
|---|------|--------------|-------------|
| 1 | **LLVM/clang-cl** | `winget install LLVM.LLVM` | System-wide |
| 2 | **Ninja** | `winget install Ninja-build.Ninja` | System-wide |
| 3 | **Ruby** | `winget install Ruby` | System-wide |
| 4 | **Strawberry Perl** | `winget install StrawberryPerl.StrawberryPerl` | System-wide |
| 5 | **vcpkg + ARM deps** | Clone, bootstrap, `vcpkg install cairo pixman freetype harfbuzz curl openssl...` | `C:\vcpkg\installed\arm-uwp\` — hardcoded in vcxproj |
| 6 | **ICU 78 for ARM** | Custom cross-build from source | `C:\icu-arm-uwp\` — hardcoded in setenv.ps1 |
| 7 | **ANGLE ARM DLLs** | From upstream `gpu-path1` branch or NuGet | `Src\angle\arm\` — relative, cleaner |
| 8 | **WebKit source** | `git clone --branch webkitgtk-2.52.4` | `WebKit/` — gitignored |

**Note on `C:\` paths** (`C:\vcpkg\`, `C:\icu-arm-uwp\`): hardcoded in `setenv.ps1`, `Harness.vcxproj`, and `link-driver-*.ps1`. Changing them would require updating ~10 files. Accepted as architectural convention — the build scripts depend on these fixed locations.

### Phase B: First Build (x64 or ARM32)
1. Configure + build WebKit libs (CMake + Ninja → `build-clang-gpu/`)
2. Link port driver (`link-driver-gpu.ps1` → `WebCoreDriver-gpu.lib`)
3. Build harness appx (`build-harness.ps1` → appx package)
4. Deploy to device (`deploy-launch.ps1`) or x64 local test

### Phase C: Upgrade & Refactor
1. Upgrade WebKit to 2.53.4
2. Consolidate stubs, reduce code duplication
3. Add CI

### Phase D: Features
1. WebGL support (already partially working)
2. Service Worker / PWA support
3. Upstream WK_WINUWP patches

---

*Progress is measured in deleted `E:\Apotheosis\` references.*
