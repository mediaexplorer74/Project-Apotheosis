# Project Apotheosis — Development Plan

> Updated: June 28, 2026
> See also: `Summary.md`, `WEBKIT-UPGRADE.md`, `AGENTS.md`, `README*.md`

---

## 1. x64 Build Setup

### Status: Infrastructure created, dependencies needed

The following files have been created to enable x64 UWP builds:

| File | Purpose |
|------|---------|
| `Src/port/Toolchain-x64-UWP-clang.cmake` | clang-cl toolchain targeting `x86_64-unknown-windows-msvc` |
| `Src/port/vcpkg-triplets/x64-uwp.cmake` | vcpkg overlay triplet for x64 UWP (VS2022 v143) |
| `Src/port/configure-gpu-x64.ps1` | CMake configure for `build-x64-gpu` |
| `Src/port/link-driver-gpu-x64.ps1` | lld-link to produce `WebCoreDriver-x64.dll` |

### Build steps (prerequisites needed)

1. **Install vcpkg x64-uwp dependencies**:
   ```powershell
   vcpkg install cairo pixman freetype harfbuzz fontconfig expat libcurl openssl `
              libxml2 sqlite3 libpng libjpeg-turbo libwebp zlib bzip2 brotli `
              --triplet x64-uwp --overlay-triplets=Src/port/vcpkg-triplets
   ```

2. **Build ICU for x64-uwp** — place at `C:\icu-x64-uwp\lib\` (follow ICU build guide for UWP)

3. **Get ANGLE for x64-uwp** — either from Windows Store ANGLE NuGet or build from source, place at `Src/angle/x64/`

4. **Configure WebKit for x64**:
   ```powershell
   pwsh -File Src/port/configure-gpu-x64.ps1
   ```

5. **Build WebKit libs**:
   ```powershell
   . Src/port/arm32-uwp-env.ps1  # may not be needed for x64; sets up env vars
   ninja -C build-x64-gpu WTF JavaScriptCore PAL WebCore
   ```

6. **Build port driver**:
   ```powershell
   pwsh -File Src/port/compile-driver-gpu-x64.ps1
   pwsh -File Src/port/link-driver-gpu-x64.ps1
   ```

7. **Update Harness.vcxproj**:
   - Change `AdditionalLibraryDirectories` from ARM to x64 paths
   - Change DLL deployment from `arm-uwp\bin\` to `x64-uwp\bin\`
   - Change `Package.appxmanifest` `ProcessorArchitecture` from `arm` to `x64`

### Problems
- All third-party libs currently ARM-only — need to build x64-uwp variants
- Repo hardcodes `E:\Apotheosis\` paths everywhere
- ICU expected at `C:\icu-arm-uwp\` — need `C:\icu-x64-uwp\` for x64

---

## 2. Multilingual UI (en/ru/zh)

### Status: Completed

All 3 languages fully implemented with both hardcoded fallback and .resw files:

### Implementation

- **String storage**: Dual-source — `.resw` resource files (primary) + `kStr[3][S_COUNT]` table (fallback)
- **Loading strategy**: `GetStr()` tries `ResourceLoader::GetString()` first, falls back to hardcoded table
- **Language switch**: `ComboBox` in Settings page → `OnLangChanged()` → `ApplyLanguage()` → `SaveSettings()`
- **Persistence**: `lang=0|1|2` in `LocalState\settings.ini`

### Resource files created

| File | Language |
|------|----------|
| `Src/harness/Resources/en-US/Resources.resw` | English (US) |
| `Src/harness/Resources/zh-Hans/Resources.resw` | Chinese (Simplified) |
| `Src/harness/Resources/ru-RU/Resources.resw` | Russian |

### New string IDs added

| Enum | Purpose |
|------|---------|
| `S_TOAST_BOOKMARKED` | "Bookmarked" toast |
| `S_TOAST_UNBOOKMARKED` | "Bookmark Removed" toast |
| `S_TOAST_HIST_CLEARED` | "History Cleared" toast |
| `S_TOAST_FAV_CLEARED` | "Bookmarks Cleared" toast |
| `S_TOAST_DL_CLEARED` | "Downloads Cleared" toast |
| `S_TOAST_CANNOT_FIND` | "Cannot Find in This Page" toast |

### All hardcoded Chinese strings replaced

All user-visible toast/status strings in code-body are now `GetStr(m_uiLang, ...)` calls:
- Toolbar/menu labels (via `ApplyLanguage()` XAML traversal)
- Status: loading, processing, timeout, cancelled, copied
- Bookmarks: add, remove, cleared, list empty
- History: cleared, empty
- Downloads: started, completed, failed, cleared, empty
- Find: no results, no more, cannot find
- Share: nothing to share
- Settings: all labels, update check messages, export messages
- Drawer list: all tab labels, item delete buttons

### Files modified

| File | Change |
|------|--------|
| `MainPage.xaml` | Added `ComboBox` language selector in Settings |
| `MainPage.xaml.h` | Added `m_uiLang`, `ApplyLanguage()`, `OnLangChanged()` |
| `MainPage.xaml.cpp` | ~55 string × 3 language table, .resw+fallback loading, toast i18n |
| `Package.appxmanifest` | Added `en-US`, `ru-RU`, `zh-Hans` resource declarations |

### Future improvements

1. Migrate XAML static labels to `x:Uid` binding (currently set in code-behind)
2. Auto-detect system language on first launch
3. Localize `kHomeHtml` (browser home page HTML)

---

## 3. WebKit Upgrade: 2.52.4 → 2.53.4

See `Doc/WEBKIT-UPGRADE.md` for full analysis.

**Summary**: Low-Medium effort, mostly Skia-focussed changes — low impact on our TextureMapper path. Recommend proceeding at medium priority.

---

## 4. Critical Fixes Needed

### 4.1. Hardcoded paths
- `E:\Apotheosis\` everywhere — use `$(ProjectDir)` or env vars
- `C:\vcpkg\installed\arm-uwp\` — use vcpkg's `VCPKG_ROOT` env var
- `C:\icu-arm-uwp\` — make configurable

### 4.2. Certificate & signing
- Signing cert password in clear text
- Publisher name `CN=EdgeHTMLReborn` hardcoded

### 4.3. ARM build env dependency
- VS2022+ doesn't ship ARM32 vcvars — `arm32-uwp-env.ps1` is a fragile workaround
- SDK 26100 removed ARM32 libs — locked to SDK 22621

---

## 5. Architecture

### String loading (current)

```
settings.ini → lang=0|1|2
     ↓
LoadSettings() → m_uiLang
     ↓
Any UI code → GetStr(m_uiLang, S_XXX)
     ↓
1. Try ResourceLoader::GetString("S_XXX")
2. Fall back to kStr[m_uiLang][S_XXX]
```

### x64 build dependency chain

```
webkitgtk-2.52.4 source → clang-cl (x86_64-unknown-windows-msvc)
                                ↓
WTF.lib + JavaScriptCore.lib + PAL.lib + WebCore.lib
                                ↓
port/*.cpp → clang-cl → obj files
                                ↓
lld-link /DLL /MACHINE:X64 + ANGLE + Cairo + ICU + vcpkg deps
                                ↓
WebCoreDriver-x64.dll → Harness (MSBuild x64)
                                ↓
Harness.appx (x64)
```
