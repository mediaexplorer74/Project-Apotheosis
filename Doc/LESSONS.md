# Lessons that still matter

These are short operating rules distilled from the detailed reports in [Archive/](Archive/). See also
[MANAGED-HOST.md](MANAGED-HOST.md) for the diagnostics the C# host needs, and
[INDEX.md](INDEX.md) for the current phase.

- Measure the actual surface. A successful HTTP load or `rc=0` is not proof that pixels reached the user.
- Keep software presentation as the safe baseline. A GPU/swapchain path is a separate feature.
- A loaded document is not the same as a presentable document. Check `nonwhite`, content size, and a
  screenshot when diagnosing a blank page.
- Keep engine-thread work off the UI thread. The UI may post work and wait for a marshalled result, but
  must not synchronously block on the engine.
- Do not regenerate or replace `MainPage.g.hpp` casually. The generated connection table is a build
  input contract; a mismatched table can produce null XAML fields and startup crashes.
- Keep the two C ABI headers (`Src/port/WebCoreDriver.h` and `Src/harness/WebCoreDriver.h`) in sync.
- Do not rebuild the engine for a harness-only presentation question unless a measurement requires it.
- Preserve the Chinese comments in the original build/deploy scripts; they are an intentional tribute.
  The `en/ru/zh` UI strings are an intentional feature.

## UWP / C# line (2026-09-26)

- **`VisualStudioVersion` must match the installed XAML targets folder.** The old-style csproj hardcoded
  `14.0`, but VS 2022 only ships `MSBuild\Microsoft\WindowsXaml\v17.0`. The `Import` then silently resolved
  to nothing, the XAML compiler never loaded, and every type failed with `WMC9999 "Type universe cannot
  resolve assembly System.Runtime"`. **No NuGet package fixes this.** The symptom mimics a missing
  platform-reference problem and sends you down the wrong path. Signature: `WMC9999`/`WMC0003` *plus*
  `CS0246` for basic BCL types such as `Thread`.
- **`.NET Native` does not compile `System.Threading.Thread`.** With `UseDotNetNativeToolchain=true` you get
  `ILT0005` (type not included) and, decisively, `ILT0003: Method '<your ctor>' will always throw an
  exception`. The build *succeeds*, so it hides easily in a wall of `CS4014` warnings — but the constructor
  throws at runtime and kills the engine. `PackageReference System.Threading.Thread` 4.3.0 does **not**
  help (`ILT0010`). Use `Task.Factory.StartNew(loop, TaskCreationOptions.LongRunning)`: it gets a dedicated
  thread and preserves single-threaded queue semantics.
- **Native DLLs must be staged at the package root.** `<Content Include="native\x64\*.dll">` without
  `Link` places them under `native\x64\` inside the appx, and `DllImport("WebCoreDriver.dll")` then fails in
  the AppContainer (no `SetDllDirectory` there). Add `<Link>%(Filename)%(Extension)</Link>`.
- **`TargetPlatformMinVersion` below 15138 requires the UWP targeting pack.** C# resolves platform refs via
  NuGet (`uap10.0.<min>`), unlike C++/CX which reads `.winmd` contracts directly from the Windows Kits. With
  `TargetPlatformMinVersion=10.0.14393.0` and no targeting pack installed, restore resolves
  `Microsoft.NETCore.UniversalWindowsPlatform` to the empty `ref/netcore50/_._` placeholder and
  `System.Runtime` / `System.Threading` / `System.Runtime.WindowsRuntime` all vanish. `15063` behaves the
  same way; `16299`+ picks up `ref/uap10.0.15138`. This is why the C++ harness targets 14393 while the C#
  project currently cannot.
- **`TargetPlatformMinVersion` is a *runtime floor*; `TargetPlatformVersion` is the *compile* SDK.** The two
  are independent, and `Package.appxmanifest`'s `TargetDeviceFamily MinVersion` must agree with the former.
- **Editing CRLF files: the `editor` tool's multi-line `old_text` silently fails.** Symptom:
  `No replacement performed: text not found` even though the lines print byte-identical; single-line
  replacements still work. Do those edits with `[IO.File]::ReadAllText` + `.Replace()` and re-validate with
  `[xml]` — a partially applied match leaves an unbalanced `<ItemGroup>` that MSBuild reports as
  `MSB4025`/`MSB4232`.
- **A UWP window may be hosted by `ApplicationFrameHost`, not by the app PID.** `EnumWindows` filtered on
  the app process returns nothing and `MainWindowHandle` is 0 even while the app is running fine. Check
  `ApplicationFrameHost` before concluding the app failed to start.
- **The appx is the ground truth for packaging.** `Add-Type -AssemblyName System.IO.Compression.FileSystem`
  plus `ZipFile.OpenRead(...).Entries` verifies DLL placement, manifest identity and architecture without
  installing anything.
- **PowerShell refuses `.ps1` by default.** The effective policy is `Restricted`, so any
  `build-monitor.ps1` / `run-*.ps1` call fails with *"running scripts is disabled on this system"*. This is
  not a broken file. Either run `pwsh -NoProfile -ExecutionPolicy Bypass -File <script>`, or set
  `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` once. A `Zone.Identifier` stream (download mark)
  additionally needs `Unblock-File` under `RemoteSigned`.
- **A P/Invoke needs three edits, and the third one fails silently at runtime.**
  `Src/port/WebCoreDriver.exports.def` is the authority on what `WebCoreDriver.dll` exports — the DLL is
  linked with `/DEF:` and carries exactly the names in that file, no matter what `__declspec` says. A
  function can be declared in `WebCoreDriver.h`, implemented in `WebCoreDriver.cpp` and bound by a
  `DllImport` in `WebCoreDriver.cs`, and the call still fails with `EntryPointNotFoundException` — at
  runtime, never at compile or link time. On x64: add the name under a group in the `.def`, then
  `pwsh -File Src/port/build-driver-dll-arm32.ps1 -Arch x64` (it recompiles nothing when the
  `Src/port/*.x64.obj` objects are newer than their sources, so a pure export change is a relink), then
  copy `Src/port/WebCoreDriver-x64.dll` into `Src/Apotheosis/native/x64/` — the appx payload is a *copy*,
  and `build-app-x64.bat` does not refresh it. Verify with
  `llvm-readobj --coff-exports <dll> | grep <name>` rather than by rebuilding the app.
- **Never `catch { }` around a native call you have not yet seen succeed.** The window-fitted viewport
  shipped with `try { rc = ...Resize(...); } catch { }` and logged only its own `-1000` sentinel, so the
  `EntryPointNotFoundException` above cost a full build-install-measure cycle to find. Log
  `ex.GetType().Name` and `ex.Message`; the type names the cause outright.
- **The count in a hand-maintained list comment is a trap.** `WebCoreDriver.exports.def` said "30
  essential exports" while listing 37. The stale number made "is the name I need in here" a question
  about the count rather than about the list, which is exactly backwards. Trust the list.

## Ninja / build-tree mechanics (2026-09-26)

- **`ninja -n` DESTROYS `.ninja_log`. Do not use it as a "dry run" to inspect a tree.** Ninja opens the
  build log for *writing* during start-up, before it consults `-n`, so the old log is dropped immediately.
  If the log version is unrecognised you get `warning: build log version is too old; starting over` and
  the next real build rebuilds the whole tree. This cost `build-arm32-gpu` its 6.8 MB log. **The compiled
  `.obj` files were untouched** (1516 objects, 3.4 GB) — only the record of what was current was lost.
  To ask "what is out of date", *read* `.ninja_log`/`.ninja_deps` yourself (see `build-monitor.ps1`),
  or copy the tree's log aside first.
- **Renaming a build tree must sweep response files, not just the ninja/CMake metadata.** A rename is
  three separate operations: (1) text-sweep `build.ninja`, `.ninja_deps`, `CMakeCache.txt`; (2) re-point
  every **symlink** whose target encoded the old name; (3) rewrite every `*.rsp`. The `*.rsp` files are
  easy to miss because nothing references them from `build.ninja` — `WebCoreFull-x64-19041.rsp` (939
  entries) and `PAL-x64-19041.rsp` (24) both still named `build-x64-19041-probe` after `build.ninja`,
  `.ninja_deps` and all `.ps1` had been fixed. Verify a rename with a recursive scan for the old name
  *plus* a real invocation of the affected script, not just a compile.
- **Verify a response file's members exist before trusting it.** Enumerating the 939 paths showed all
  present, and the rebuild returned `WEBCOREFULL_EXIT=0` — a cheap, decisive check that the renamed tree
  is self-consistent.
- **`lib.exe` archives are large but cheap to confirm.** `WebCoreFull-x64-19041.lib` = 3.2 GB from 939
  objects; rebuilding it took ~90 s, so a "static library is stale" worry is cheap to disprove.
- **The port stubs are compiled into the engine object tree.** Both trees contain
  `Source/WebCore/CMakeFiles/WebCore.dir/C_/.../Src/port/stubs-ax.cpp.obj`, i.e. `stubs-ax.cpp` and
  friends are WebCore members, not a separate library. An engine rebuild therefore also rebuilds the
  port layer, and a stale engine tree means stale stubs.
- **`build-monitor.ps1` works without a log.** If `.ninja_log` is missing or headerless it counts the
  `.obj` files actually present on disk against the `.obj` edges parsed from `build.ninja`, and labels
  the reading `(via disk)`. This is the correct reading for a full rebuild, where there is no log yet.

## SEH and target support (2026-09-26)

- **`clang-cl` cannot compile `__try` for ARM32.** Building the ARM32 driver gives 15 ×
  `error: SEH '__try' is not supported on this target` (`thumbv7a-windows-msvc`), and the failure is
  *silent downstream*: `WebCoreDriver.cpp` yields no object, so the link reports
  `LNK2001: unresolved external symbol WebCoreClickAt` / `WebCoreCloseSession` / … for **every**
  export, which reads like a missing-import-library problem and is not one. The real error is 40
  lines earlier in the log. **Always read the compile step, not just the link errors.**
- This is a *clang* limitation, not an OS one: MSVC's ARM32 backend does support `__try/__except`,
  and clang does support SEH on AArch64. The Lumia 950 / W10M target is ARM32.
- The wrappers are therefore expressed through `APO_SEH_RETURN_INT` / `APO_SEH_CALL_VOID` in
  `Src/port/WebCoreDriver.cpp`. x64 expands to the original `__try/__except` and is unchanged; ARM32
  forwards directly and so has **no native guard** — a fault there still reaches the CLR's
  `FailFast(7)`. `APOTHEOSIS_REQUIRE_SEH` turns that gap into a build error rather than a device
  surprise. Getting a real guard onto ARM32 means compiling that TU with MSVC, not a macro.
- **`GetModuleHandleExW` is not declared on the ARM32 include path.** `ApoFormatAddress()` in
  `WebCoreDriver.cpp` relies on `libloaderapi.h` reaching the TU transitively, which it does for x64
  but not for ARM32 — `error: use of undeclared identifier 'GetModuleHandleExW'`. Interestingly
  `GetModuleFileNameW` on the next line *is* declared on both. The file is now explicit about it,
  guarded by `#if !defined(_LIBLOADERAPI_H_)` so x64 does not get a duplicate declaration.
- **On ARM32 `JavaScriptCore.lib` is a real static archive, not an import library.**
  `llvm-nm --defined-only` counts **16 094** defined symbols, and the members are named after the DLL
  (`JavaScriptCore.dll`), which is why `LNK2005` says "already defined in
  `JavaScriptCore.lib(JavaScriptCore.dll)`". It therefore already contains the WTF objects, and the
  ARM link list — which names `JavaScriptCore.lib` and `WTF.lib` twice each for the JSC↔WTF circular
  dependency — produces `LNK2005` for every shared WTF symbol (`StringImpl::destroy`,
  `WTF::fastMalloc`, `String::utf8`, …). Those are **duplicates, not missing symbols**. Fixed with
  `/FORCE:MULTIPLE` for ARM only; it is deliberately not applied to x64.
- **Measure an archive instead of trusting its name.** `WebCore.lib` and `JavaScriptCore.lib` read
  alike, but on x64 `WebCore.lib` is a genuine import library (33 718 `__imp_` symbols) while the ARM
  `JavaScriptCore.lib` is a full object archive. One `llvm-nm --defined-only <lib> | Measure-Object`
  settles it in seconds and would have prevented a wrong link model.
- **Both architectures now link WebCore statically, and `WebCore.dll` is ballast in both payloads.**
  The reasoning that briefly pushed the build to import mode — "as `__imp_` references they resolve at
  load time" — holds only for **exported** names. What `stubs-other.cpp` needs is not exported: the
  protected constructors `RenderTheme::RenderTheme()` and `SystemFontDatabase::SystemFontDatabase()`,
  the data global `WebKitFontFamilyNames::standardFamily`. No `__imp_` thunk is generated for those, so
  there is nothing to resolve at load time and the link fails outright. This bit x64 too, not just ARM.
- **Check which ICU an import library is bound to by reading its first archive member name**, not by
  counting symbols. `llvm-nm --defined-only C:\vcpkg\installed\x64-uwp\lib\icuuc.lib` reports ~12 000
  "defined" symbols and looks like static ICU, but the first member is named `icuuc78.dll` and only
  3 030 entries are `__imp_` thunks — the rest are `.idata$` bookkeeping. It is an import library for
  **ICU 78**, which is what both the engine objects and the shipped driver use.
- **`C:\icu-x64-uwp` is ICU 75 and is now a trap.** `Doc/BUILD-X64.md` and the driver build script both
  carried instructions to prefer it over vcpkg. Those are obsolete: `icuuc75.dll` there, `icuuc78.dll`
  everywhere else. Any `u_*_75` unresolved symbol means that directory has leaked back into
  `LIB`/`/LIBPATH` — check `$IcuLibDir` first.
- **In an AppContainer there is no GDI, and `gdi32.lib` does not help.** vcpkg's ARM `cairo.lib` uses
  the win32 GDI backend, so it references `__imp_SaveDC`, `GetGlyphOutlineW`, `GetFontData`,
  `GetTextMetricsW`, `DeleteObject`, … Linking the SDK `10.0.19041.0 um\arm\gdi32.lib` still leaves
  every one of them unresolved. The resolution is `Src/harness/gdi-stubs.cpp`, which the x64 harness
  gets through its vcxproj and a standalone driver link has to compile in itself.
- **A static library records its target machine.** ARM archives must be built with
  `...\MSVC\14.44.35207\bin\Hostx64\arm\lib.exe`; an x64 `lib.exe` produces a library that
  `link.exe /MACHINE:ARM` rejects. And the ARM SDK is **10.0.19041.0** — 26100 does not ship ARM32 libs.
- **A stale PCH poisons every later compile, and ninja will not repair it.** WebCore's
  `cmake_pch.cxx.pch` is a *side effect* of the `cmake_pch.cxx.obj` edge — `/Fo` and `/Fp` in one
  command. Ninja tracks only the `.obj`, so after the SDK was pinned to 19041 the PCH still held
  `corecrt.h` from 26100, and every WebCore TU that used it died with four
  `redefinition of '_CrtEnableIf'` / `'_Mbstatet'` errors blaming a deep include chain
  (`simde/arm/neon.h` -> `fenv.h`). Deleting the `.pch` alone is not enough — ninja skips the edge.
  To force a PCH rebuild you must delete **both** `cmake_pch.cxx.obj` and `cmake_pch.cxx.pch`.
  Beware the misleading shape of this failure: the error names `fenv.h`, so it reads like a simde
  problem, and it appears only in a TU that happens to be the first to include `fenv.h` after the
  PCH — which is why it looked like my own file was at fault.
- **Do not cache or pool a `WriteableBitmap` that is on screen, and do not pool the frame buffer
  it is filled from — measured 2026-09-27, both were tried and both reverted.** Reusing one
  `WriteableBitmap` and re-assigning it as `Image.Source` every frame felt *faster* (it removes a
  2.97 MB array and a surface allocation per frame) and then made images on `ya.ru` visibly blink out
  and back. The compositor holds a reference to the surface it is presenting, so overwriting one in
  place is not safe; a **fresh** `WriteableBitmap` per frame is exactly what lets the old one stay
  intact until the new one is ready. A 3-slot pool for the RGBA frame buffer has the same hazard from
  the other side: the array is captured by a dispatcher closure and read on the UI thread after the
  P/Invoke returns, so a slot can be recycled while a queued frame still references it. A frame
  showing a mix of two states is what both produce.
- **What did help, and is safe: allocate the 2.97 MB BGRA scratch array in `BlitToBitmap` once.**
  `720*1080*4` exceeds the 85 KB Large Object Heap threshold, and the LOH is never compacted, so one
  such array per presented frame dragged the process into repeated Gen2 collections. That is what
  made long-page scrolling stutter progressively and stall before the footer; the touch bench
  reported exactly that shape. The per-frame RGBA buffer was left alone — the real fix is to have
  `paintToRGBA` emit BGRA so no conversion buffer is needed at all, which is a driver change.
- **The GPU is not in play on the C# line: `WebCoreGpuInit` returns -20.** `LocalState\gpuinit.txt`
  records it, `_gpuPresent` stays false, and every frame goes through the software path
  (`BlitToBitmap`). So the scroll numbers above are CPU rasterisation into a `WriteableBitmap`, not
  GPU compositing — worth knowing before optimising for the GPU path. Note the crash-loop guard at
  `MainPage.xaml.cs`: a leftover `gpu-crash.flag` file turns `_gpuDefault` off permanently, so a
  single historical crash will keep the GPU off for every later run until the flag is removed.
- **Text rendered nothing at all because `APOTHEOSIS_FONTS_DIR` was never set — fixed 2026-09-27.**
  The single cause, and it had nothing to do with font selection. `FontPlatformData.cpp`'s
  `apotheosisBundledFontFaceForName()` — the FreeType path that loads a bundled TTF into a cairo FT
  face — starts with `if (!std::getenv("APOTHEOSIS_FONTS_DIR")) return nullptr;`. The C++ harness set
  it in `SetupRuntimeEnv`; **the managed C# host only ever set `FONTCONFIG_FILE`**. So the bundled
  face was skipped, `apotheosisBundledFontFace()` fell through to `cairo_toy_font_face_create`, and the
  toy face resolves through fontconfig — which is not deployed (`Doc/BUILD-X64.md`: neither
  `cairo-2.dll` nor `fontconfig-1.dll` ships). `cairo_scaled_font_create` then returned an **in-error
  scaled font, `CAIRO_STATUS_NO_MEMORY`**, and every glyph operation on it is a silent no-op. Layout,
  colour, borders and paint all worked; not one glyph appeared, on any page, ever.
  - **The tell was the status, not the pointer.** `scaledFont()` was non-null, so the code reported
    `OK` — it never asked `cairo_scaled_font_status()`, which is the only thing that reports this.
  - Proof, from the probe in `stubs-font-uwp.cpp`: `cairo_status=1` before, `cairo_status=0` after.
  - Fix: `WebCoreSetFontConfigFile` now also parses `<dir>` out of the `fonts.conf` it was handed and
    sets `APOTHEOSIS_FONTS_DIR` from it. One source of truth, and no new import.
  - A **separate** `WebCoreSetFontsDir` P/Invoke does **not** work and must not be reintroduced:
    .NET Native rejects a freshly added import at the first call with
    `TypeLoadException: method 'WebCoreSetFontsDir' has no implementation (no RVA)`. Fold the work
    into an existing export instead.
- **`SetupRuntimeEnv`'s single outer `try` hides unrelated failures as unrelated symptoms.** A throw
  from one P/Invoke silently skipped `WebCoreSetSehGuard` and `WebCoreSetCACertBlob`, after which the
  page failed TLS with `curl 60` and looked like a network fault. `Crumb()` in `MainPage.xaml.cs`
  writes append-only breadcrumbs to `LocalState\setup-trace.txt` for exactly this; `stage.txt`
  overwrites and cannot answer "how far did setup get".
- **`createFontPlatformData` is not why nothing renders — measured, 2026-09-27.** It used to
  `return nullptr` unconditionally, which is a very satisfying explanation for "layout and colour
  work but no glyphs", and it is wrong. With a real bundled-font implementation instrumented, a load
  of `https://example.com/` calls it and it *succeeds* every time (`OK segoeui.ttf at 16.00px`,
  `OK segoeui.ttf at 24.00px`, plus SimHei via `systemFallbackForCharacterCluster`) — a face is built,
  a scaled font exists, the cache is warm, and the screen still shows no text. The fault is strictly
  **downstream of font selection**, in shaping or rasterisation. Do not re-diagnose this as a missing
  font backend; the bundled FreeType + cairo-ft path is present and reachable. See the trace block in
  `Src/port/stubs-font-uwp.cpp` for the captured evidence.
- **The family reaching font selection is usually EMPTY, not missing.** `isNull=0 isEmpty=1` on most
  calls. This is the same null/`AtomString`-default-constructed-family condition that
  `Doc/Archive/FONT-NULL-FAMILY-CRASH.md` documents as a crash producer; here it is benign because
  the code maps an unrecognised or empty family to a default face instead of returning `nullptr`.
- **Every port trace is file-local, so no other TU can log.** `WebCorePortTrace` in
  `WebCoreDriver.cpp` is in an **anonymous namespace** — `llvm-nm` reports
  `?WebCorePortTrace@?A0xC8417F77@@...` — and `portLoaderTraceBoth` in
  `PortPlatformStrategies.cpp` is `static`. Declaring either from another translation unit gives
  `LNK2019` under *any* linkage: `extern "C"` looks for an unmangled name, plain C++ for
  `?WebCorePortTrace@@YAXPEBDZZ`, and neither exists. A trace added to a WebCore port stub must
  derive `port-trace.txt` from `FONTCONFIG_FILE` itself (the driver's own rule: strip the filename,
  append `port-trace.txt`), which costs one TU rebuild instead of re-plumbing the driver.
- **A renamed build tree leaves stale paths in `CMakeFiles/TargetDirectories.txt` too.** After the
  `build-x64-19041-probe` → `build-x64-gpu` rename, `build.ninja`, `.ninja_deps` and the `*.rsp` files
  were fixed, but 15 CMake-generated files were not: `CMakeFiles/TargetDirectories.txt` (101
  occurrences), `CMakeFiles/rules.ninja`, every `cmake_install.cmake`, and the two `CPack*.cmake`.
  The symptom appears only on the *next* ninja run, and it is nasty: ninja decides CMake is stale,
  and `TargetDirectories.txt` sends the regeneration into a directory that no longer exists —
  `Configuring incomplete, errors occurred!`. The rule template in `build.ninja` itself was correct,
  so grepping `build.ninja` finds nothing and the cause looks impossible. A rename sweep must cover
  `CMakeFiles/**` and `**/cmake_install.cmake`, not just the ninja files.
- **Never use `Get-ChildItem | Length` to check header integrity — symlinks report 0.** 4663 of the
  7770 `.h` files under `build-x64-gpu` are symlinks, and PowerShell reports `Length = 0` for every
  one, which looks exactly like a tree destroyed by an interrupted build. It is not: read through the
  link (`Get-Content`) or stat the target. A genuine check is
  `Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }` — that left 26 real
  files, all `.lut.h` dated 2026-09-25, i.e. legitimately empty generated headers from the original
  build. I raised a false alarm on this and killed two builds for nothing.
- **Re-running CMake invalidates the whole ninja build.** The regeneration rewrites `build.ninja`, so
  every recorded command hash changes and the next `ninja <single-target>` still wants thousands of
  edges (4280 observed). A one-file change in a WebCore TU is therefore *not* a minutes-long
  operation once CMake has re-run — budget a full WebCore rebuild, or compile the TU by hand.

## Driving the C# app from a script (2026-09-27)

- **`System.Threading.Thread` does not exist in this project's UWP profile.** Only
  `System.Threading.Tasks` is referenced, so `Thread.Sleep(ms)` fails to compile with
  `CS0234 ... 'Thread' does not exist in the namespace 'System.Threading'`. Use
  `Task.Delay(ms).Wait()`. This is a *profile* limitation, not a missing assembly, so adding the
  `using` cannot fix it.
- **The C# line had no way to be scripted.** `nav.txt` did this for the C++/CX harness, but that
  watcher was never ported; the C# app only reads `testurl.txt` **once at startup**, so every check
  cost a full install-restart cycle. `Src/Apotheosis/MainPage.AutoCmd.cs` restores it: a 250 ms
  watcher on `LocalState\autocmd.txt` running `goto` / `scroll` / `scrolln` / `wait` / `hash` /
  `diag` on the engine thread, appending per-command timings to `LocalState\autocmd-log.txt`. The
  batch is dispatched with `WebEngine.Instance.Post` because every P/Invoke needs the engine thread.
- **Seed the file stamp at startup.** Comparing `LastWriteTimeUtc` across ticks means a leftover
  `autocmd.txt` never replays by itself; only a fresh write re-arms it. Otherwise every app launch
  silently re-runs the last test.
- **`WebCoreGetFrameHash()` is the cheap flicker detector.** It is stable for a static page, so N
  samples 400 ms apart alternating between two values means the compositor is repainting different
  content. Seven consecutive identical hashes on `ya.ru` after reverting the frame-pooling
  experiment is a much stronger signal than eyeballing a screenshot.
- **Measured scroll cost, software rendering, `ya.ru`:** first step 173 ms (cold paint), then
  121 / 88 / 43 ms, then a flat **11-13 ms** for the remaining 19 steps. The flatness matters more
  than the mean: cost does not grow with distance scrolled, which is what "fine then grinds to a
  halt" would look like. This drives `WebCoreScrollBy` directly, so it measures engine + present and
  *not* the touch path with its accumulated deltas, link sync and live tick.
- **Memory for the same run:** 171 MB working set, 409 MB private, 619 handles, with 80 MB physical
  RAM free - the machine is genuinely tight, worth remembering before blaming the browser for every
  allocation hiccup.
- **`testurl.txt` takes a bare URL**, not a command. Writing `goto https://ya.ru/` navigates
  nowhere and `stage.txt` reports the whole string as the URL.

## Measuring the UI and the sites (2026-10-03)

- **A number about the window is not a number about the page.** The site verdict used to read
  `nonwhite` from a screen capture, and the app's dark chrome fills the window: all twelve sites
  scored an identical **99.68 %**, the same for a painted page and a blank one. It was not a wrong
  answer — it was the wrong question. Use the engine's own `diag: nonwhite=A/B`, which counts
  framebuffer pixels differing from white. Calibration is what makes it readable: example.com 100 %
  (its body background is `#f0f0f2`, so almost no pixel *is* white), nokia-archive 99.64 %,
  wikipedia-m 81.68 %, w3.org 43.23 %, habr 4.04 %, info.cern.ch 1.86 %, **dzen.ru 0.00 %**. The
  last is the point: dzen returned `rc=0` and painted nothing — the failure `rc`-only checking
  cannot see.
- **A fixed sleep eventually reads the wrong page.** Sleeping a constant interval per site then
  reading `stage.txt` meant that on a heavy site the load outlived the sleep and the script read the
  *previous* site's `after-load` line and `diag`. The tell was two sites reporting the identical
  `31.59`. Wait for the site's own URL to appear in an `after-load` line — the loader's own statement
  that it finished — instead of guessing how long a load takes.
- **A failure row cannot have a page number.** The loader writes its stage block (`rc`, then `diag`)
  *before* it renders the error HTML, and a failed load has not replaced the engine's document, so
  that `diag` describes the previous page. It showed up as `bad-domain` reporting dzen's reading byte
  for byte. Report `n/a` rather than a number that belongs to another site.
- **Screenshot the app window, DPI-aware.** This bench is 2736x1728 at 200 %. A DPI-unaware
  PowerShell reports a 1368x864 working area, resizes the window in those virtual units, and then
  `CopyFromScreen` crops — the first settings screenshots had no right edge and no bottom chrome, and
  it looked like an app defect. `SetProcessDPIAware()` before any size query, then move the window
  into the working area and capture exactly that rectangle.
- **`Get-Process Apotheosis | .MainWindowHandle` is 0 for a UWP app.** The top-level window is
  `ApplicationFrameWindow`, owned by `ApplicationFrameHost.exe`, and titled `Apotheosis`. Find it by
  class and title. In the `EnumWindows` callback, write the **script** scope — a local declared in the
  calling function is shadowed by the callback's own scope and the value is lost, which cost a round
  of "no window found".
- **A native string parameter declared `CharSet.Ansi` destroys non-ASCII text silently.** Cyrillic
  arrived at the engine as literal `?` bytes (0x3F), in tile titles and in generated HTML alike.
  Marshal by hand: a private `…Raw(byte[])` `DllImport` plus a public wrapper that encodes UTF-8 into
  a NUL-terminated `byte[]`. A pinned blittable array needs no charset conversion and no
  `[MarshalAs(UnmanagedType.LPUTF8Str)]`, whose behaviour under .NET Native is unverified. Note the
  same files carried working Cyrillic elsewhere, so this is never a file-encoding problem.
- **`x:Uid` looks up `<Key>.Text`, and a bare key in the `.resw` makes every label render empty
  without an error.** `ResourceLoader.GetForCurrentView()` is additionally bound to the view
  language, so it cannot serve a language picker: it answers the launch language forever. Use
  `x:Name` plus one `RetranslateUi()` pass over a positional table.
- **`Add-AppxPackage` can fail on the PRI stage of an in-place update while rolling back cleanly.**
  `0x80073CF9`/`0x80004005`, `Failure in the SystemRegisterRemove state handler`, then `failed to
  register the windows.MRT extension: The data is invalid`. The recovery is back up `LocalState`,
  `Remove-AppxPackage`, `Add-AppxPackage`, restore `LocalState` — and the backup is not optional,
  because `Remove-AppxPackage` deletes `LocalState`, which is where `settings.ini`, `history.tsv` and
  every log live.
- **Two frames that differ in no pixel say "this was never repainted", not "the change is invisible".**
  A desktop-UA shot and a mobile-UA shot of the same URL in the same fitted window (2736x1728, one per
  run, every second pixel sampled with a 12/255 tolerance) came out **identical**, and the useful
  reading of that zero is not "the zoom does nothing to the picture" but "the second run put no new
  frame on screen at all". A re-navigation cannot legitimately repaint byte-identically. Reach for
  cross-run frame differencing early: it costs one script and it separates "the effect is subtle" from
  "the effect never happened", which eyeballing a downscaled PNG cannot.
- **A cache of engine state must be invalidated by the event that invalidates the state, not by
  watching the value.** The shell cached `_pageZoom`; the port resets its own `g_pageZoom` to `1.0f` in
  `teardownSession`, which `WebCoreSessionLoad` calls unconditionally, so **every navigation** built a
  session laid out at zoom 1.0 while the shell still believed 3.29. Both numbers "agreed" — the cache
  and the state were equal in value and different in existence — so the guard that compares them
  (`|_pageZoom - _vpZoom| < 0.001` → "nothing to do") was silently a no-op, and the port's own
  re-assert guard was disarmed by the same reset. The fix is at the producer: forget the cached value
  at navigation completion. Two habits generalise: reset engine-derived caches on the lifecycle event
  rather than trusting equality, and when a no-op path can be reached *by two different causes*, log
  which one was taken (the port already does this for `WebCoreSetPageZoom`) — the missing log is what
  made a four-hour hunt possible.
- **Read `contentsSize()` as frame-view units, never as CSS px.** `[GPU] page zoom 1.00 -> 3.29
  (layout 1355x2216 CSS-ish, viewport 1355x751)` looks like proof that the zoom failed to narrow the
  layout, because the layout width equals the viewport width. It is the opposite: frame-view
  coordinates already carry the page zoom, so `1355 / 3.29 = 412` and `2216 / 3.29 = 673` — a 412 CSS
  px phone layout. The marker's own "CSS-ish" hedge is the warning. Confirm it with a page whose
  document box equals its viewport (the Nokia archive logs `layout 1355x751` at the same zoom, i.e.
  `412 x 3.29`), and leave the software paint scale alone: `FrameView::paint` applies the factor
  itself, which is exactly why the size it is handed is in the enlarged units.

## Page zoom replaces page scale (2026-10-03, `0.2.3.14`)

- **Page scale is a magnification and page zoom is a re-layout, and only one of them can be the
  only lever.** `WebCoreSetPageScale` "MAGNIFIES the finished layer tree without re-running layout
  (the layout viewport is unchanged, so `documentElement.clientWidth` stays put)" — the port's own
  words. So the layout the engine hit-tests against and the pixels on screen are two different
  geometries, and every consumer that picks one of them (fixed-position overlays, tap mapping,
  scroll extents) disagrees with the others. Reported by a finger on dzen.ru: *"всплывашка живёт
  своей жизнью словно в другом слое — скроллю в её зоне, она не двигается, а скроллится сайт под
  ней."* Page scale is now **removed from the C# line entirely** (the export stays declared; nothing
  calls it), and this is the whole fix: one geometry, always.
- **`contentsSize()` is the discriminator, and it is a one-line A/B.** Same page, same window, mobile
  UA, `diag` after each: `zoom 100` → `contents=1355x1360`; `zoom 200` → `contents=1355x3062`. The
  width is unchanged *because frame-view units carry the zoom* (see the bullet above): 1355 / 2 = 677
  CSS px, and the list re-wrapped there, which is why the height grew 2.25x rather than 2x. A page
  *scale* would have left both numbers untouched and stretched the pixels — that difference, not the
  screenshot, is what proves the re-layout happened.
- **One body applies a zoom, and both callers use it.** `PostZoomAndRepaint(w, h, zoom, why)` is
  called by `ApplyViewport` (geometry or setting changed) and by `CommitPageZoom` (a pinch ended).
  The retired pinch path had its own copy of the sequence — its own buffer, its own `_opSeq` test
  that could return early and leave the engine magnified while the shell kept the old value, and its
  own transform-clearing that the early return skipped. Two implementations of one protocol is what
  made that possible; there is now one, and the supersede case logs `… frame superseded seq A -> B`
  and still clears `_interacting` (the first measured hit was `seq 4 -> 5` on the very first run).
- **A resize is a repaint at any size, and that is what makes zoom-without-geometry possible.**
  `WebCoreSetPageZoom` deliberately does not paint ("a full repaint costs ~1.2 s ... both callers in
  the harness resize right after"), and `WebCoreSessionResize` has **no same-size early-out** — it
  assigns `g_session->w/h` and calls `finishInteractionPaint` unconditionally. So a pinch commits as
  `SetPageZoom(z)` + `SessionResize(kW, kH)` with the size *unchanged*. This is also why the old
  fitwindow-off branch needed page scale at all, and that reason is now gone.
- **A gesture cannot set a private field and call it a measurement, so the zoom became a verb.**
  `zoom <50..200>` goes through the real `SetZoomSlider`: `ShowSettings()` → set `Value` → 
  `HideSettings()`, which is the only path that reads the slider, calls `ApplySettings()` and saves.
  Assigning `_defaultZoom` directly would have skipped the repaint (`OnZoomChanged` only repaints the
  *label*) — i.e. it would have measured a field, not the feature. Same argument as `ua`.
- **The pinched zoom is the setting, not a per-page overlay.** A pinch writes `_defaultZoom` (clamped
  to `kZoomMin`/`kZoomMax`, the same pair the slider and `ApplySettings` use), so there is exactly one
  authority for the zoom and `AdoptViewport`'s recompute of `_vpZoom = ViewportZoom()` can never
  fight it. `Tab.PageScale` is deleted for the same reason: a per-tab copy is a second authority.
- **A scripted verb runs on the engine thread, so it may not touch XAML.** The first instrument added
  to `diag` read `RenderImage.RenderTransform`; every `diag` in the batch died with
  `RPC_E_WRONG_THREAD` / `0x8001010E` — "Приложение обратилось к интерфейсу, относящемуся к другому
  потоку" — because a `DependencyObject` belongs to the UI thread. The verb now touches plain fields
  only. A related trap: `xform=` (was the transform still on the Image?) is not moved elsewhere but
  *deleted*, because with one geometry there is nothing left for it to report — the commit clears the
  transform in the same dispatcher call that blits.
- **`WebCoreGetDiag` (and every sibling `Get*`) returns `snprintf`'s return value — the length it
  *would* have written — so it can exceed the buffer it was handed.** Used as the count for
  `Encoding.UTF8.GetString(buf, 0, n)` it throws `ArgumentOutOfRangeException` and the whole line is
  lost: measured on dzen.ru, where the run reported an exception instead of the page's own answer.
  2048 was also too small for one page's `res:[...]` list; the buffer is 4096 now, read whole and
  trimmed at the first NUL like every other `Get*` caller, and truncation is stated out loud because
  a clipped `res:` list reads as "the page requested nothing".

## The whole window blinks while live mode runs (2026-10-03, `0.2.3.14`)

A finger on dzen.ru: *"очень неприятное подергивание всех картинок сайта (ощущение, что они
перезагружаются 1000 раз, поджирая оперативную память)"*. After it had stopped on its own:
*"картинки перестали перерисовываться наконец… время пропадания картинок ощущалось в пол-секунды"*.
The report is accurate and the mechanism is not a leak.

- **Measure the frame rate before blaming a repaint loop.** `hash` sampled at 500 ms
  (`autocmd` batch 5, 15:03) gives `1c7764ee → 01617366 → 01617366 → 5d0428c1 → 5d0428c1 →
  63ab9a26`: the engine's frame changes **~1×/s**, not 5×/s. So there is no runaway relayout — a
  page JS timer spinning at 15 Hz would have moved the hash at 15 Hz. What is wrong is the *cost of
  one frame*, not the number of frames.
- **Each changed frame replaces the entire window surface.** `BlitToBitmap`
  (`MainPage.xaml.cs`) allocates `new WriteableBitmap(kW, kH)`, swizzles `kW*kH` pixels and assigns
  `RenderImage.Source`. Because the *Source* is swapped rather than the pixels under it, the
  compositor drops the old texture at once and the new one lands a frame or two later — every image
  on screen disappears together for a moment. That is the reported "all images reload at once", and
  the "half a second" is that gap, not a re-fetch.
- **The blink stops by itself because live mode stops itself.** `OnLiveTick` ends after
  `_liveStaticTicks >= 40` (8 s of *unchanged* frames) or `_liveTotal >= 300`. On a live feed the
  static condition never accumulates, so the run goes the full 300 ticks — ~30 s at 200 ms plus
  ~150 s at the 1 s interval — and then quiesces. Hence "blinked for a few minutes, then stopped",
  with no code change. A symptom that ends without an edit is still a defect; it is just bounded.
- **It is churn, not a leak — say so with two readings.** `Get-Process Apotheosis` twice, minutes
  apart: `RSS_MB=448 priv_MB=1267 handles=627` both times, identical (`cpu_s=501.8`, 20 threads,
  one process since 14:24). A 4 MB `WriteableBitmap` plus a 4 MB `NewFrameBuffer()` per tick is
  ~1.2 GB of short-lived garbage across a 300-tick run; the GC keeps up, so RSS is flat. "Поджирает
  оперативную память" is a true perception of allocation pressure and a false diagnosis of a leak.
- **The fresh bitmap per frame is deliberate — read `git show 2b17643` before "fixing" it.** That
  commit is titled *"Stop reusing surfaces across frames: it fixed scrolling and broke images"*:
  reusing one `WriteableBitmap` made images blink on ya.ru, because the compositor holds a reference
  to the surface it is presenting, so writing into it in place is not safe. The commit names the
  proper fix — *have `paintToRGBA` emit BGRA and the conversion buffer disappears entirely*.
- **So the repair has two independent halves, and neither is "cache the bitmap".**
  (1) The CPU/alloc half: emit BGRA from `paintToRGBA` so the 4 MP B/R swizzle and its buffer vanish.
  (2) The blink half: a small **ring** of 2–3 bitmaps, so the surface being written is never the one
  being presented — which is what the single-cached-surface attempt got wrong, and what a pool
  addresses without reintroducing the tearing. Only (2) narrows the blank window; (1) alone would
  make the same blink cheaper.
- **Half (2) landed in `0.2.3.15` as `kBlitRing = 3`.** `BlitToBitmap` writes round-robin into three
  `WriteableBitmap`s built once per geometry (`_blitRingW/_blitRingH` are the key, because a
  `WriteableBitmap`'s size is fixed at construction and every fitted resize rewrites `kW/kH`) and
  assigns the slot it just finished. Slots are filled lazily, so a geometry that only ever presents
  one frame pays one bitmap. Cost: up to 3 × `kW*kH*4` held steady (~12 MB at the fitted bench
  viewport) against 3 fresh 4 MB surfaces and ~1.2 GB of LOH garbage across a 300-tick live run.
  **The number to argue about from here is `kBlitRing`, not "cache it or not".** Half (1) is still
  open and is a native change (`paintToRGBA` in `WebCoreDriver.cpp`), so it is a driver rebuild and
  not an app rebuild.
- **Corollary for the `port-trace.txt` channel:** it is a debug channel shipped on, 41 MB in
  `LocalState` over one session (180 845 `wtimer:` lines). It is not the blink's cause (37 B/s
  while idle, 0.2 `wtimer` enters/s) but it is a shipping-build liability.

## Double-tap to fit (`0.2.3.15`, same day)

- **`Tapped` fires for the FIRST tap of a pair.** That is the whole difficulty of this gesture: a
  handler that forwards the tap immediately has already sent a click into the page by the time
  `DoubleTapped` arrives, and on a feed page the first tap of the pair lands on a card link — so the
  "zoom" gesture would navigate away instead. The second tap raises `DoubleTapped` and *not* a second
  `Tapped`, so cancelling the pending first tap is enough to make the pair do exactly one thing.
  `OnPageTapped` therefore holds every tap for `kDoubleTapMs = 250` and forwards it from a one-shot
  `DispatcherTimer`; `OnPageDoubleTapped` stops that timer. The 250 ms is a real tax on every link
  tap in the app — it is what every mobile browser pays for this gesture, and it is small against the
  engine's own click latency (seconds on a heavy page).
- **The scripted verbs are deliberately NOT delayed.** `tap`/`taplink` call `HandleTapAt` directly: a
  script has no double taps to disambiguate, and paying the wait there would make the bench slower
  than the finger. `dtap` calls the same `DoubleTapFit()` the event handler calls — separated into
  its own function for exactly that reason, because a verb that re-implemented the toggle would
  measure the verb and not the feature (the `ua`/`zoom` rule).
- **A stray second `Tapped` must be survivable.** The documentation says the platform does not raise
  one, but that guarantee is the platform's to keep and not ours to assume; a wanderer would become a
  click 250 ms after a zoom. `_lastDoubleTapMs` is set by the double-tap handler and swallows any
  `Tapped` arriving inside the window. Compared as `uint`, so the subtraction still works when
  `Environment.TickCount` goes negative on a long uptime.
- **CSS page zoom has no focal point, so this gesture is a level toggle, not a zoom-to-point.**
  Chrome's desktop double-tap zooms *around* the tapped point; ours cannot, because page zoom re-lays
  the page out rather than magnifying a finished layer (the same fact that removed page scale). So:
  at the fit zoom (100 %, where the layout width IS the window width) double-tap goes to the last
  zoom the user chose; anywhere else it returns to fit. `_lastFitOffZoom` is written inside
  `CommitPageZoom` — the one function every lever goes through — so no second bookkeeper is needed.
- **Measured through the verb, in the app's own log:** `ui dtap: vz=1,00 default=100` →
  `dtapfit: resize rc=0 zoom rc=0 1355x751 z=2,00` → `pzoom=2000 vz=2000 zoom=200` in the following
  `diag`, then back to `z=1,00` / `pzoom=1000` on the second double-tap. `rc=0` on both halves.
- **The same run also closed the `diag` trap from the entry above:** the buffer fix (4096 bytes, read
  whole, trimmed at the first NUL) is in `0.2.3.15` and the verb now prints the page's full
  `res:[...]` list instead of throwing. Worth stating because the fix had been in source but not in
  the installed package, which is the difference between "fixed" and "measured".
- **A dzen load that "succeeds" can still be a blank document, and `contents=` is how you tell.**
  Two loads in a row: the first ended at `url=https://sso.dzen.ru/install?uuid=…` with
  `lasterr=[phase=didFailProvisionalLoad type=Cancellation]`, `body=0`, `rootKids=-1`,
  `nonwhite=0/1017605` — and `contents=1355x751` at *both* zoom 100 and zoom 200, because an empty
  document's content height is the viewport height. The zoom had still been applied (`pzoom` moved),
  so the geometry test was measuring a blank page and could not have shown the reflow. The second load
  reached `url=https://dzen.ru/` with the real title and settled at a constant frame hash over six
  500 ms samples. **Check the document before reading a geometry out of it.**

## The address-box dropdown (`0.2.3.18`/`.19`, 2026-10-03)

- **One symptom, three causes.** "Гигантское паразитное окно с кривым показом истории" was: Enter
  navigating without hiding the list; `NavigateTo` hiding it only *after* its `_loading`
  early-return, so a navigation dropped as "shell busy" left the panel hanging (the user's own
  screenshot has the ✕ loading glyph in the address bar); and `MaxLines = 1` without
  `TextWrapping = NoWrap` rendering a long title as two full lines. Fix one and the report is still
  open — which is why the row geometry and the two hide paths had to be found in the same pass.
- **A transient "I am writing this myself" flag cannot cover an asynchronous echo.** The commit
  writes the URL into the box, `TextChanged` for that write arrives *after* `_urlSyncing` was
  cleared, and the handler — box focused, text non-empty — matched the URL against the history the
  commit had just appended and reopened the dropdown **after Enter**. `SetUrlBoxText` remembers the
  string it wrote and `OnUrlChanged` treats that equality as a shell write. Any state carried in a
  flag around an assignment is a guess about when the platform delivers the notification.
- **A scripted verb must fake the *state* inputs, not just the text.** `suggest` set `UrlBox.Text`
  without `_urlFocused`, and `OnUrlChanged` thereupon hid what the verb had just shown: log `vis=1`,
  screen empty, twice. The conclusion drawn from that ("the panel does not render") was wrong, and
  the fix was in the verb. A control cannot be focused from a script
  (`synthetic-input-cannot-reach-uwp`), so the verb sets the field the handler actually reads.
- **Read a visibility twice if the question is "did it stay?"** `ui suggest … vis=1` and
  `ui suggest+700 vis=1 h=251 boxfoc=1` are different measurements; the first cannot distinguish
  never-drawn from shown-then-hidden. The deferred probe costs a `DispatcherTimer` and settled a
  question two screenshots had answered wrongly.
- **A dropdown height must be a fraction of the window.** 250 DIP is a minority of a 912-DIP
  notebook window and most of a 640-DIP phone window. `SuggestScroll.MaxHeight` is clamped in
  `ShowSuggestions` to `max(120, min(250, contentArea × 0.34))` — the phone is the shipping target,
  and "окультурить" has to mean "on the device that ships".
- **`DisplayUrl` on the secondary line:** eight `https://` prefixes are eight wasted columns on a
  360-DIP screen. Cosmetic on the bench, half the row on the Lumia.
- **`AppxBundle=Never` is not a shortcut when the installed package has resource packs.** The update
  fails with `0x80073CF3` (dependency or conflict validation) and no useful text; the install script
  was looking for an `.appxbundle` for a reason. Build the C# line with `-p:AppxBundle=Always`.

## The device death was a null WebSocket channel, and the instrument could not say so (`0.2.3.29`, 2026-10-09)

- **`pageConfigurationWithEmptyClients()` is a field of landmines, not a default.** It installs an
  `Empty*` for every client WebCore can ask for, and some of those empties answer `nullptr` where
  WebCore `RELEASE_ASSERT`s that the embedder supplied the real one. Two so far: the
  `NotificationController` supplement (an AV reading `0x8`, `.25`) and `EmptySocketProvider`'s null
  channel (`RELEASE_ASSERT(m_channel)`, `WebSocket.cpp:288`, `.29`). **When a page dies on the device
  and the trace's last work looks unrelated — MIME parsing, here — look for a supplement lookup on the
  fatal path before theorising about the subsystem the last line names.** The last *completed* work is
  where the engine died, not what killed it.
- **A reader that computes the right length and forgets to copy is a reader that lies.** `ApoImageString()`
  validated a string's shape and wrote only its terminator, so both of the assert reader's nets matched
  uninitialised stack buffers and could never fire. Two builds (`.26`, `.27`) and two device runs went
  into nets that could not have fired, and a third (`.28`) at least proved the walk itself now reads
  memory. **The tell is a contradiction between the log and the source: when a host simulation of the
  same logic matches and the build does not, disassemble — do not re-read the source.** Same family as
  `out-of-bounds-stack-scan-is-dead-code` and `diagnostics-that-guess-will-lie`.
- **A stack-spill offset is a codegen fact, not a property of the assert.** On `.28` the
  `WTFCrashWithInfo` argument triple sits four words *above* the `frames[2]` return address, not below
  it (`a = i + 1`, not `a = i - 4`). Scan both offsets, re-measure per build, and prefer naming the
  assert by hand from the raw window when the nets are ambiguous — a 480-word window with real module
  attribution is enough to resolve `file`/`function` in the local PE.

## Back did not work, and three stubs had to go together (`0.2.3.36`, 2026-10-14)

- **The same empty-client trap, a fourth time — and this one was silent rather than fatal.**
  `EmptyBackForwardClient::addItem` is `{ }` and `backListCount()` returns 0, so the engine's
  back/forward list was permanently empty and `canGoBackOrForward(-1)` was permanently false. Because
  nothing aborted and nothing logged, the defect read as "Back is not implemented yet" rather than as a
  client that lies. **`EmptyStorageNamespaceProvider` (a silent success), `EmptySocketProvider` (a
  `RELEASE_ASSERT`), `EmptyBackForwardClient` (a silent nothing): when a feature is dead and no error
  appears, look for the empty client before looking for the missing feature.**
- **Lift the whole behavioural chain, not the one call the symptom names.** Fixing the empty client
  alone left Back dead: the port's own `LoadingFrameLoaderClient` still returned `nullptr` from
  `createHistoryItemTree` (so `updateBackForwardListClippedAtTarget` returned before it ever called
  `addItem`) and `No` from `shouldGoToHistoryItem` (a policy veto that makes `goToItem` return on its
  first line). **Three stubs, one symptom: when a client is a copy of an `Empty*` base, audit every
  override with a behaviour, not just the one on the failing path.**
- **A port that feeds the parser by hand owns the bookkeeping the loader normally does.** This port
  fetches with curl and calls `DocumentWriter` directly, so `FrameLoader::transitionToCommitted()` — and
  with it `case FrameLoadType::Standard: history().updateForStandardLoad()` — never runs for its own
  loads. Nothing is written into history, so a document the port painted is not in the list at all.
  **The control case is what made this visible: a link *tap* goes through a real `FrameLoader::load()`
  and was recorded (`n=1 cursor=0`) while the document before it was not.** When a port substitutes for
  the loader, check which of the loader's *state* duties it has also taken over.
- **A URL the engine stores comes from the loader's request, and this port's loader was born empty.**
  `HistoryController::updateBackForwardListClippedAtTarget` guards on
  `documentLoader->urlForHistory().isEmpty()` — which reads `m_originalRequestCopy.url()` — and the
  loader `FrameLoader::init()` creates is built from `ResourceRequest(URL({ }, emptyString()))`. So the
  URL has to be written down before the item can exist at all.
- **A history cursor reconciled by URL needs a direction.** A `_navStack` such as `[A, B, A]` makes
  `IndexOf` answer with the oldest `A` on the way forward as well as on the way back; measured, one
  Back moved the cursor from 3 to 1 and jumped over the entry it had landed on. **Search outward from
  the cursor in the direction of travel — a move has a direction, and a lookup that ignores it is
  guessing between identical entries.**
- **A port-side subclass of a WebCore interface is linkable here, because the port links WebCore
  statically** (`WebCoreFull-*.lib`), and `Src/port/PortWebSocket.cpp` is the worked example: a
  `SocketProvider` whose channel returns `ConnectStatus::KO`, which `WebSocket::connect()` converts into
  `failAsynchronously()`. Refcount it the way `WorkerThreadableWebSocketChannel` does — `RefCounted<T>`
  **first**, then the interface, with `void ref() const final { RefCounted::ref(); }` —
  `ThreadSafeRefCounted<T>` derives from `ThreadSafeRefCountedBase` and `RefCounted::ref()` does not
  resolve there, while the interface's pure-virtual `ref()`/`deref()` must be supplied by the concrete
  class. Include `<WebCore/WebTransportSession.h>` explicitly: `SocketProvider.h` only forward-declares
  it, and `RefPtr`'s destructor needs the complete type.
- **Fail observably.** The tempting shortcut is a channel that pretends to connect. The one that works
  is the one whose failure the page can see: `connect()` says `KO`, WebCore raises an ordinary error
  event, and the page's own error handling runs. Nothing is silently successful.
- **A header-only cure cannot travel through a relink.** The ARM line relinks pre-built libraries, so a
  change to `Source/WTF/wtf/RedBlackTree.h` — the 2026-09-19 `z->reset()` in `remove()` — reaches the
  phone only if the ARM binary built from that header is rebuilt. It was not: the ARM
  `JavaScriptCore.dll` is dated 2026-08-23, the bench's 2026-09-27, and the fix's own `TREE-BAD` probe
  string is present 5 times in the bench binary and **0** times in the ARM one. The `.29` device death
  is exactly that missing fix. **Check a binary's mtime against the source's date, and grep the binary
  for a string the change introduced, before believing any fix is on the device** — a stale DLL and a
  broken fix look identical at runtime.
- **Not every abort can be named by a reader that looks for strings.** `CheckedRef.h:341` is
  `static NO_RETURN_DUE_TO_CRASH NEVER_INLINE void crashDueToCheckedPtrToDeadObject() { CRASH(); }` —
  plain `WTFCrash()`, not `WTFCrashWithInfo(line, file, function, counter)`. There is no argument triple
  on that stack, so `no argument triple in 1024 words` was the *correct* answer, not a third instrument
  bug. Read the crash function's source before doubting the reader, and expect the WebKit assertion
  machinery to have both shapes.
- **Symbolise ARM `JavaScriptCore.dll` from `jsc.map`, not from the PDB.** For RVA `0x1EA01B`,
  `llvm-symbolizer` against `JavaScriptCore.pdb` answered `Vector<UniqueRef<InspectorAgentBase>>::
  expandCapacity` — plausible, wrong, and 6.6 MB away from the truth; the linker map answered
  `crashDueToCheckedPtrToDeadObject` (atomic instantiation). Same family as
  `llvm-symbolizer-reads-the-pdb`.
- **A guard macro that does not exist suppresses nothing.** `Src/port/WebCoreDriver.cpp` declared
  `GetModuleHandleExW` behind `#if !defined(_LIBLOADERAPI_H_)` — an ARM workaround, because the ARM32
  include chain does not reach `libloaderapi.h`. No SDK on this machine guards that header with
  `_LIBLOADERAPI_H_`; both 19041 and 26100 use `_APISETLIBLOADER_`. So the fallback declaration was
  emitted on **every** line, and on x64 — where the header *is* in the chain — it disagreed with the
  real prototype (`HMODULE` versus the SDK's `BOOL`) and the driver stopped compiling:
  `conflicting types for 'GetModuleHandleExW'`. **A wrong macro name and a missing macro behave
  identically at the `#if` and oppositely at the compiler**, so check the guard against the header's
  own `#ifndef`, do not trust that it "works on ARM". Nobody saw the break for six weeks because the
  x64 driver object predated the block: `WebCoreDriver.gpu.x64.obj` was 2026-08-16 while the
  declaration arrived 2026-09-26 — **a line that is not being built is not verifying anything**, the
  same lesson as `arm-jsc-predates-the-redblacktree-cure` with the two lines swapped.
- **A deferred flag needs a drain on every mode that can set it.** `_vpDirty` in `MainPage.xaml.cs`
  records "the engine was busy, pick this up when it is free", and it is set by four different
  functions. Its only drain was `AdoptViewport()`, which returns immediately when `fitwindow` is off —
  so on the fixed 720x1080 line (the ARM configuration) a zoom committed while the engine was busy was
  dropped outright; the device reported it as "питч не отрабатывает вообще". The cure is
  `DrainPendingZoom()`, which is the same busy test plus, in the fit-off branch, a re-run of the
  deferred commit instead of a geometry adopt. **Grep every write of a deferred flag and check each
  one has a reader on the path it is written from** — a pickup that is guarded by a mode check the
  writer does not share is a silent drop.
- **A compile-capable environment hides a link-time dependency.** The ARM line's environment is two
  scripts: `Src\setenv.ps1` (project paths) and `Src\port\arm32-uwp-env.ps1` (`LIB`/`INCLUDE`/`PATH`
  for the UWP ARM SDK). Sourcing only the first looks fine for hours — `ninja` runs, `clang-cl`
  compiles, WTF objects appear — because every include path is baked into `build.ninja` as `-I` flags,
  and then it dies on the *first link*: `lld-link: error: could not open 'ucrt.lib'`. The failing edge
  is `bin/LLIntSettingsExtractor.exe`, a build-time host generator that CMake links as part of the ARM
  target, and the CRT is located through `LIB`, not through a baked-in path. **A missing `LIB` is
  invisible until something links, so "it compiles" is not evidence that the environment is complete** —
  and the failure lands 1996 steps in, past the point where you would still be watching. The recipe is
  in `Src\port\build-webcore-arm32.ps1`, which sources both before invoking ninja.
- **The two build lines see different Windows SDK header *versions*, so "is it declared?" is not a
  question the preprocessor can answer.** One translation unit (`Src/port/WebCoreDriver.cpp`), one
  `GetModuleHandleExW` declaration needed by hand. Measured on both lines with `#pragma message`
  (2026-10-09): `_APISETLIBLOADER_` is **defined on both**, and
  `WINAPI_FAMILY_PARTITION(DESKTOP|SYSTEM|GAMES)` is **0 on both** — because both compile with
  `-DWINAPI_FAMILY=WINAPI_FAMILY_APP`. Both obvious guards are therefore wrong. What actually differs
  is the SDK: ARM pins `10.0.19041.0` through `arm32-uwp-env.ps1`, and *that* `libloaderapi.h` hides
  the function behind the desktop/system/games partitions; the x64 chain reaches `10.0.26100.0`, which
  declares it for `APP` as well. Found by preprocessing the x64 TU and reading the `#line` marker above
  the hit: `# 309 "...\10.0.26100.0\um\libloaderapi.h"`. **When a UWP build may or may not see a Win32
  declaration, do not guard on a header macro or on the API family — declare it unconditionally as a
  byte-exact copy of the SDK's prototype**, since the SDK's own `WINBASEAPI BOOL WINAPI` *is*
  `__declspec(dllimport) BOOL __stdcall`: where the header already declared it, yours is a legal
  redeclaration; where it did not, yours is the only one. Getting the return type wrong (`HMODULE` for
  the SDK's `BOOL`) is what turned the duplicate into `conflicting types`, which is how the x64 line
  stopped compiling for six weeks without anyone noticing. The general form: **`#pragma message` on the
  real command line beats any amount of reasoning about include chains**, and the same wrong reasoning
  was reached twice, a fortnight apart, by two different routes.

## Packaging: a bundle that passes the audit and is still wrong (2026-10-10)

- **`Test-AppxPayload.ps1` does not look inside the bundle's package list.** It checks architecture,
  payload placement, `resources.pri` and the export table — all of which a *single-package* bundle
  satisfies. What it cannot see is a missing `Apotheosis_<ver>_language-ru.appx`, so an English-only
  artifact reports `AUDIT PASSED` and deploys. Count the inner `.appx` files as a separate check:
  there are **seven** (main + `language-ru` + `language-zh-hans` + four `scale-*`).
- **`/p:UapAppxPackageBuildMode=SideloadOnly` suppresses resource-pack generation on this project.**
  Measured while creating `Src/tools/build-app-arm.bat` by copying the flag from
  `build-app-x64.bat`: the bundle then held only `Apotheosis_<ver>_ARM.appx`, and
  `BundleArtifacts/arm.txt` lost every `ResourcePack=` line. Copying a flag between the two arch
  scripts was the whole mistake — the x64 C# package carries its ru-RU strings inline, so the flag is
  harmless there and destructive here, where they live in the language pack. `BundleArtifacts/arm.txt`
  is the cheap tell: it is rewritten by every build and its line count is the package count.
- The general shape: **a build flag that is correct on one line is not evidence for the other line.**
  Same family as [[two-lines-two-sdk-header-versions]] — verify per line, with an artifact you can
  count, not with a green checkmark.

## A check that reported its own bugs before it reported the app's (`0.2.3.37`, 2026-10-14)

Requirement 5's evidence in this file was a hand-driven three-command probe. Extending it to the
standing thirteen-site list — one Back and one Forward after every site, in the row's own CSV — took
four wrong measurements before it took one right one, and **three of the four were the check's fault,
not the browser's.** Recorded here because each produced a confident, specific, false number.

- **A completion marker must be the verb's OWN answer, not any line of that shape.** The `forward`
  poll accepted "any `hist` line", and the `back` it was measuring had already written one, one
  millisecond earlier. Both samples came back byte-identical and the run reported *forward did not
  return* for all thirteen sites — while the log plainly showed the cursor moving back. **Count the
  lines of that shape before issuing the verb and require the count to grow**; "a line exists" is not
  "this verb answered".
- **Read the complete line, not its opening fields.** `autocmd-log.txt` is append-only and the app
  writes to it *while* the reader reads, so a read can catch a line flushed only as far as `... ur`.
  A prefix regex accepts it and its `url=` field then reads as empty: every row said `moved_to=''`
  while the app was moving correctly. Anchor the pattern on the end (`url=\S*$`) so a partial line
  simply does not match yet.
- **A function's return value travels the pipeline and unrolls a one-element array.** A helper
  returning `@(...)` hands back a bare **string** whenever the log holds exactly one hist line, and a
  string's `[-1]` is its last *character*. Measured: the first sample of every row came back as one
  letter — `m` for `...url=about:home`, `/` for the Nokia URL — because that sample is taken when the
  log holds one line and every later sample when it holds two. **`@()` around every read**, and
  remember the unrolling happens on the way *out* of the function, not at the call site.
- **Silence is a state, and only the app can name it.** Two rows in five runs had a Back with no
  `rc=` at all. Reproduced deliberately (example.com → bad-domain → back) and traced to
  `NavigateHistory`'s `!_sessionActive` early return: the app answered from the host stack and logged
  **nothing**. A Back that was skipped and a Back that never arrived were indistinguishable. Since
  `0.2.3.37` the app says `history back skipped reason=no-session` (and `busy loading=/interacting=`
  for the other branch). **A harness field that reads empty is a question, not a null** — this one
  found an app defect, not a check defect, and it was found only because the field was made
  impossible to ignore.
- **A check's own expectation can destroy the state it measures.** A Back leaves the host cursor one
  entry up, and the next site's `goto` pushes with `pushHistory=true`, which truncates the forward
  tail. Without a Forward after every Back, the stack never grew past `[Home, current]` and the whole
  list would have been "measured" against a two-entry stack. **Step back, step forward, then move on:
  a probe that leaves the cursor where it found it is the only kind that can measure a sequence.**
- **Two outcomes are sometimes three.** Site 13 (`nokiadesignarchive`, an SPA) reported a MISMATCH
  with the engine having served the Back (`rc=0`). The port trace says why:
  `addItem url=<nokia> n=1 cursor=0` for the port's own recorded load, then
  `addItem url=<nokia> n=2 cursor=1` from the document's own history push — the list holds the same
  URL twice, so an engine Back moves between two identical entries and `WebCoreGetUrl` returns the
  URL already displayed. That is browser semantics and the app is right to leave the host cursor
  alone. **When a third legitimate outcome appears, add it to the check by name** (`back_case`), and
  print the breakdown next to the pass count — a bare `13/13` would have hidden which of the two
  successes each row was. The outcome was first a prediction from the trace and then a measured row in
  the desktop run: `back via engine rc=0 … => ok (the page pushed its own URL)`, with
  `fwd_attempts=3`, because the first two Forwards did not move the host cursor — both rows carry the
  same URL, so the loop re-tried. **A re-try count that is part of the record is what makes that
  legible instead of looking like a flaky poll.**
- **A run that is not repeated is not a measurement.** The first desktop-mode run was `10 / 13` with
  two extra misses — `01-example-com` at `rc=-16` on a fresh process and `06-w3-org` painting
  `0.00 %`. Neither recurred in the repeat minutes later, on the same build and the same mode: `01`
  returned `rc=0`, `06` painted `36.85 %`. Only `11-dzen-ru` failed in all three runs, and that one is
  a known item. **The check's own headline moved 10 → 12 with no code change in between**, which is the
  point: the first number was about the process's position, not about the sites. Same discipline as
  [[two-windows-are-not-two-arms]] — and the difference between a flake and a verdict is a second run,
  not a longer look at the first.
- **A harness that cannot write its output must refuse before it starts.** A run was launched with an
  `OutDir` whose `$env:USERPROFILE` the calling shell had eaten, so the path was invalid on Windows and
  `New-Item` failed — and the run proceeded for ten minutes, with every screenshot failing and
  `screen_nonwhite` an empty column, because `$ErrorActionPreference = 'Continue'` applies here too.
  The script now verifies `Test-Path` on the directory and exits with a message. **The rule is the one
  the rest of this script already follows: no silent degradation of the artifact.** A parameter mistake
  is the easiest way to produce one, because it looks like the run is working.

## `dzen.ru` is not a blank page, it is a redirect (2026-10-14)

The acceptance list has carried `11-dzen-ru` as `rc=0` with `nonwhite=0.00` since 2026-10-03, and the
row read as a site that does not render. The redirect behind it was not unknown — the `.29` device
trace in [MVP.md](Archive/MVP.md) already shows the first session loading `sso.dzen.ru/install` and painting
`nonWhite=0`, and a 2026-09-18 analysis had the `https://dzen.ru` → 302 → `sso.passport.yandex.ru/push`
chain. What was missing is that **the row could not name its own document**, so the list kept reporting
a site defect for what is a page. The measurement below closes that, and adds the part that was not
known: the redirect is a property of the **first visit** to `dzen.ru` — a second visit does not
redirect, and because the port's cookie jar is in memory the state dies with the process, so no visit
is ever a second one.

- **The trace is unambiguous, and it names the redirect.** From the acceptance run's `port-trace.txt`,
  in order, for dzen's own `goto`:

  ```
  loader: CALL type=0 main=1 https://sso.dzen.ru/install?uuid=660fcbf6-c828-4b8d-9e78-745634df7e15
  PBF: createHistoryItemTree clip=1 item=https://sso.dzen.ru/install?uuid=660fcbf6-…-745634df7e15
  PBF: addItem url=https://sso.dzen.ru/install?uuid=660fcbf6-…-745634df7e15 n=1 cursor=0 back=0 fwd=0
  tail: paintToRGBA exit rc=0 nonWhite=0
  ```

  `nonWhite=0` is every pixel white. The control is in the same file, one navigation later — the same
  process's **second** visit to the same URL: `PBF: addItem url=https://dzen.ru/ n=1 cursor=0` followed
  by `tail: paintToRGBA exit rc=0 nonWhite=450125` (44 %). **Same URL, same process, two different
  documents: the first visit is the SSO install page, the second is dzen, and both were already in the
  record without either being labelled.**
- **The record could not see it, because every URL field in it is the REQUESTED one.** `after-load
  url=` is written from the port's own `surl`, the app's `_currentUrl` is written by harness-initiated
  navigation, and the host stack stores that same string — so all three say `https://dzen.ru` while the
  engine's document is `sso.dzen.ru/install`. The only field that names the loaded document is the
  engine's own `diag: url=`. **A per-site record that cannot name the document cannot tell "this site
  does not render" from "this site sent us somewhere else"**, and the list carried the first reading
  for weeks.
- **A first visit and a second visit are different experiments, and the difference is state.** First
  visit to `dzen.ru`: redirect, blank. Second load of the same URL in the same process: no redirect,
  40–47 %. Eight probes on warm processes painted (35–47 %); the acceptance runs, which had not been
  near dzen before, all blanked. Both results were *consistent*, which is exactly why they looked like
  a property of the site — the variable was state that the visit itself creates, not the engine and not
  the page. **When a reading is stable within one kind of process and flips in another, the difference
  between the processes IS the experiment.**
- **The trace names the mechanism, and it is the request's cookie set.** `SL: fetch … cookies=N` in
  `gpu-steps.txt` counts the cookies the curl handle holds: within one run it climbs `0 → 11 → 17 → 23
  → 32 → 34`, and in **every** run it starts at 0. dzen's first request therefore arrives with no dzen
  cookies and is answered `302` (three `Set-Cookie`) → `sso.passport.yandex.ru/push?uuid=…&retpath=…` →
  200, 2883 bytes, a JS redirect page → the engine navigates itself to `sso.dzen.ru/install` and
  COMMITs it. That page's own redirect to `dzen.ru/?is_autologin_ya=true&sso_failed=blocked` comes back
  as a cancelled `didFailProvisionalLoad`, which is why what remains on screen is the blank install
  page. On the **second** visit the same request carries cookies and takes another road entirely:
  `redirects=3`, out through `login.vk.com/?act=autologin` and
  `dzen.ru/api/auth/web/autologin-vk?errorCode=11300&errorText=invalid+user`, then `200` with
  3 090 069 bytes of real dzen. **Two different pages, one variable, and the variable is a header.**
- **…and the jar is deliberately per-process, which is why the state never survives.** The port does
  not use curl's on-disk `cookie.jar.db`: `PortNetworkStorageSession.cpp` creates the session with
  `SessionID::generateEphemeralSessionID()`, which makes `NetworkStorageSessionCurl` use a `:memory:`
  jar, and says in its own comment that the App Container cannot write the on-disk path and that the
  cost is "cookies do not persist across restarts". There is **no cookie file anywhere** under the
  package folder — `LocalState` has none, and the App Container's `AC\INetCookies` is an empty
  directory the port never writes to. **So the honest version of the finding is not "a first visit
  redirects" but "a first visit in a process redirects, and no visit is ever the second one."**
- **The way to tell a mechanism from a story is to run it twice on purpose.** The first write-up of
  this called the variable "a cold process". The checked-in 0.2.1.4 artifact is the one number that
  could contradict it — dzen as site 11 of 13, painting `31,59 %` — and it cannot decide the question
  either way, because **two** explanations fit it: that process may have visited dzen before the run
  was launched (nothing records what it had browsed), or the harness of that era read the `diag` at a
  moment that caught the list's own Forward, which reaches dzen a second time. One number, two stories,
  no way in from the outside. What settled it was **two cold processes with the same `LocalState`:
  both blanked** — which is the mechanism's own condition, run on purpose. **A single anomalous number
  is not evidence for a mechanism; a repeated experiment under the mechanism's own condition is.**
- **An acceptance list that restarts the app per run only ever measures the first visit.** dzen appears
  once in the list, so every run is a first visit whatever the mechanism turns out to be. That is the
  right thing for a list to do — every row then starts from the same place — but it means a site whose
  behaviour depends on stored state will always be measured in its coldest state. The answer is not to
  warm it up (that would be editing the site's behaviour to suit the check) and not to call the site
  broken: it is to record **which document the engine actually loaded**, which the harness now does in a
  `final_url` column, with a `redirected` note and a summary line. What the check is allowed to conclude
  from a `nonWhite=0` is now bounded by whether the document is the one that was asked for.
- **The column paid for itself on its first run.** `final_url` flagged two rows, and the second was
  not dzen: `10-reddit-com` requested `https://reddit.com` and landed on
  `https://www.reddit.com/?solution=…&js_challenge=1&jsc_token=…` — reddit's anti-bot challenge URL.
  That row still painted `45.81 %` and passed, but "the requested URL is not the document" is now a
  fact the record states rather than something a reader has to reconstruct from a 180-character diag.
- **What is still open, and much smaller now.** (a) Whether `sso.dzen.ru/install` has visible content a
  desktop browser would show — the measurement says this engine rendered nothing for it, not that the
  page is empty. (b) Whether the jar *should* persist: `PortNetworkStorageSession.cpp` names the
  follow-up itself (`WebCoreSetCookieJarPath` pointed at `LocalState`), and this finding supplies the
  cost of not doing it — **a site with an autologin flow pays its whole SSO dance on every launch**, and
  a user who logged in yesterday is logged out today. That is a product decision, not a defect, and it
  is now written down with a number attached. **Naming the document turned one site-shaped mystery into
  two page-shaped questions, and one of them has a price tag.**

- **A batch is one unit of work.** The pre-Back reading was a batch of its own, and that batch went
  unanswered in some rows (the app skips a command while another is running), leaving an empty
  `hist_before` for a site whose Back was in fact correct. Folding it into the same batch as the verb
  — `hist`, settle, `back`, settle, `hist` — removed the whole class: if the batch runs at all, both
  samples are written.
- **A fresh process pays for the first navigation, and the answer is to wait, not to retry.** The
  first `goto` of a run came back `rc=-16` (`kErrCurlDownload`, `curl_easy_perform` failed) with
  nothing painted, while the same URL issued by hand against a process started the same way returned
  `rc=0`. The list now opens with a handshake that waits until the shell answers. **It deliberately
  does not retry a failing load — a run that retried its failures would be a run that hides them.**
  The handshake reduces the cost, it does not remove it: in the first desktop run the handshake was in
  place and the first site still came back `rc=-16`, while the repeat's first site loaded normally. So
  the honest reading of a first-site failure is "the process was still paying", and only a second run
  says whether it was that or the site.

## The device list ran itself, and the instrument hid two rows (`0.2.3.40`–`.42`, 2026-10-20)

The Lumia cannot be driven from the bench: Device Portal reads `LocalState` and every write returns
HTTP 500. So the acceptance list ships inside the appx (`Assets\device-run.txt`) and is executed by
`MainPage.TryStartPackagedBatch()`, once per package version, with a `device-run.done` marker written
*before* the first command so a crash-loop cannot replay it. That works — 161 commands, 13/13 sites in
one process — and it exposed four instrument defects, none of which the app had.

**A synchronous render is a different code path, and it is easy not to instrument.** The app draws its
error page through `WebCoreRenderHtmlImpl()`, not through the live paint loop. That function never
called `writeDiag`, so the diag after a failed navigation still described the *previous* site, and a
row that looked measured was stale. Worse, the same function does not update `g_lastFrameHash`, so the
frame hash on that row was the previous page's pixels. Rule: when a second path can put pixels on
screen, instrument it as carefully as the first, and do not assume a shared-looking reading
(`hash`, `diag`) comes from the path you are testing. The fixed function now counts non-white in its
swizzle loop and writes the diag before returning; the error row reads `(error page)`, `body=1`,
12.28 % painted.

**A field that can contain another field's name needs an anchored match.** The engine diag begins
`| url=<document>` and later contains `lasterr=[... url=<failed request>]`. The error page's own
`url=` is empty (the document is synthetic), and an unanchored `url=(\S+)` matched the `lasterr` one —
the analyzer reported the *request* as the document. Anchor at the start of the engine half of the
line, and give the empty case a name (`(error page)`) instead of falling back to a nearby value.

**The log appends across installs.** `autocmd-log.txt` lives in `LocalState`, which belongs to the
package *family* — a version update keeps it. The `.41` report therefore opened with all thirteen rows
of `.40`, indistinguishable from the new run. The analyzer now slices from the last
`---- packaged batch` header. The same is true of `port-trace.txt`: its rotation ceiling is checked at
startup, so one session grew to 10 MB before the next launch rotated it in.

**Two WDP traps, both of which produce a confident false answer.** Terminate is a `DELETE` whose
`package` parameter is **base64 of the full name** (a plain name answers HTTP 500) and which needs the
CSRF token copied from a response's `Set-Cookie` header (403 without it) — `Src/tools/device-stop.ps1`.
And the process list names the field **`ImageName`**, not `Name`; a filter on `Name` matches nothing,
which reads exactly like "the app is gone". After stopping, confirm the process is absent before
pulling files the app had open.


## An unattended device run ends when the display sleeps (`0.2.3.46`–`.48`, 2026-10-20)

**The first rule of a long device run: the app must not be the only thing keeping the phone awake.**
`0.2.3.46` ran the thirteen-site list and, 11 s into a `wait 40000`, `boot.txt` wrote `suspending` and
the process was gone by the next pull — no fault, no assert, no crash report, the log ending cleanly.
Nothing had been done to the phone since the previous stop. That is suspend-then-reap: a UWP app that
makes no touch input lets the display time out, and suspension is where an unattended run ends. It is
also the most plausible explanation for `.42`'s silent end, which predates the lifecycle marker. The
`.48` probe is the control: 73 s after its batch released the display request, with nothing else
happening, `boot.txt` wrote `suspending`.

**A lifecycle marker is worth more than a long trace.** Before `.43`, every marker described work —
`rlife:`, `wtimer:`, loader lines. None described *the app going away*, so a suspension and a crash
looked identical. `App` now writes `suspending`, `resuming` and `EXC <type>: <message>` into `boot.txt`,
and the same 3-minute run that was unreadable in `.42` names its channel in `.46`.

**The cure belongs inside the batch, not in the instructions.** "Keep the phone awake" is not an
acceptance run; an instrument that needs a hand on the device does not measure an unattended one. Since
`.47` a packaged batch holds a `Windows.System.Display.DisplayRequest` from the first command to the last
(`MainPage.AutoCmd.cs`, `HoldBatchDisplay`/`ReleaseBatchDisplay`), logs `display: held awake` and
`display: released`, and `.47` then ran 13/13 in 11 min 05 s without a suspension. The request is created
on the UI thread, so it must also be **released** there: `.47`'s release ran on the engine thread and
threw `InvalidCastException … Arg_NoMarshalCreatedObjectUsedOutOfTreadContext`, leaving the display held
until the process was stopped. `.48` marshals the release through `Dispatcher.RunAsync`.


## "Scrolling is broken" can mean the page scrolls an element, not the frame (2026-10-20)

Two reports arrived together from the Lumia: *the Nokia Design Archive content cannot be scrolled*,
and *the site is still the mobile layout even with "Start in Desktop mode" on*. One piece of the
site's own code explains both, and neither cause was the user agent.

The site chooses its layout from the CSS width alone. Its stylesheet carries
`@media only screen and (max-width:870px){#desktop-container{display:none}}`, and the bundle carries
`function nn(e){return e.width<=e.mobileBreakpoint}` with `mobileBreakpoint: 870`. The ARM line lays
the page out at 720 CSS px (`fitwindow=0`, zoom 100), so the site is mobile: its console says
`Simulation paused on mobile` -- the Network graph is stopped by the site itself -- and its content
scrolls inside `.mobile-view{height:100vh; overflow-y:auto}`, an element, not the document. The diag
line said so before any of this was read: `contents=720x1080` equals the viewport, i.e. the frame had
nothing to scroll.

The port could only scroll the frame. `WebCoreScrollByImpl` sets `LocalFrameView::scrollPosition` and
nothing else, so a finger over an inner scroller moved nothing at all. `WebCoreScrollByAt(x, y, dx,
dy, buf)` now does what a browser does with a wheel event: `elementFromPoint` at the finger, the
nearest ancestor with a scrollable overflow takes the delta, chaining upward when it is already at
its end, and the frame is the fallback. The point rides with the coalesced pump, captured from
`ManipulationDeltaRoutedEventArgs.Position` on the same path the tap uses.

Worth keeping:

* a layout switch may be width-only; check the site's CSS/JS before blaming the UA switch. The width
  lever in this port is the page zoom (layout width = viewport / zoom), so 80 % is 900 CSS px here
  and crosses the CSS breakpoint; "desktop mode" is the UA and nothing else by design. Confirm that
  the *JS* agrees before promising a desktop layout -- this site's clamp is `screen.width`;
* `contents` equal to the viewport while a user reports "cannot scroll" is the tell for an inner
  scroller, and it is readable from `stage.txt` without touching the device;
* a scroll path that only sets the frame is a real gap for every mobile SPA, not a quirk of one site.

**Measured, and then explained by the maintainer's own hands (0.2.3.50).** The fix reached the phone
and the maintainer reported the scroll alive by finger. The first packaged probe of it, however, was
contaminated in an instructive way: the log carries real pinches (`pinch: resize … z=0,56`, `1,07`,
`0,55`, `0,50`) during the run -- the maintainer was pinching while the batch navigated. Both swipes
therefore ran on pages *loaded* at zoom 100 and *re-zoomed* by the fingers, and both changes of the
frame hash were zero. The maintainer described the result better than the hash: the site was in a
"mixed Design+Mobile mode", with desktop elements appearing after a pinch. That is the two width
readers disagreeing. The CSS media query re-evaluates the moment the layout width changes, but the
bundle's `nn(e)` check runs where the graph is initialised -- at load -- so a pinch after load splits
the page in two.

**Correction after the clean 0.2.3.51 run (2026-10-20): a load at 80 % does NOT open the JS gate
either.** The .51 probe measured both halves of the page in one process with no fingers on the glass.
At zoom 100 the inner scroller was found and moved -- `[SCT] x=360 y=540 z=1.00 d=0,48 rc=0
out=1|DIV|mobile-view|720x1080`, the frame's own range `0,0..0,0` -- which is the scroll report
answered in the page's own terms. At zoom 80 the same page *loaded* at 900 CSS px still logged
`Simulation paused on mobile` three times, and `[SCT] … z=0.80 out=0|||900x1350` found no scrollable
element under the centre at all. So the mixed mode is not a pinch artifact. The bundle's width is
`Mu() = Math.min(window.innerWidth, document.documentElement.clientWidth, window.screen.width)`:
the third term is the phone's physical CSS width (720), and page zoom cannot change `screen.width`.
The CSS half follows the layout width, the JS half is clamped by the screen, and no zoom setting or
UA switch in this port can lift the clamp. The only honest route to the site's desktop JS is a
port-level desktop geometry, and it is now implemented: see "Desktop mode carries a screen override"
below.

Two consequences for the code:

* the scroll delta needed the same division as the point: `scrollTop` and `ScrollPosition` are CSS px,
  so a finger's N surface px is N/zoom CSS px; `.51` divides both. Without it a scroll at zoom 0.5
  moved half as far as the finger;
* a run that only measures `hash` cannot tell "no scroll range" from "the walk found nothing". `.51`
  writes `[SCT] … out=m|tag|class|innerWidthxinnerHeight` for every pump and `[SCT] frame cur=… →
  range=…` for the fallback, so the next device run names the element that took the delta or proves
  that there was none.

## Desktop mode carries a screen override (`0.2.3.51` x64 bench, 2026-10-20)

The direct way to give the engine a desktop geometry is `LocalFrame::setOverrideScreenSize`. It
exists in WebKit, it is in the ARM static archive, and it is **not** WEBCORE_EXPORTed -- and the x64
line links `WebCore.dll`'s import library, so on that line the symbol is not reachable at all. Do
not spend a build on it; the export list is the answer. The page-side equivalent needs no export:

* `UserContentController::create()`, `addUserScript(mainThreadNormalWorldSingleton(),
  makeUnique<UserScript>(...))` and `Page::setUserContentProviderForWebKitLegacy()` are exported on
  both lines. A `UserScript` defaults to `DocumentStart`, i.e. before the site's bundle runs
  (`HTMLConstructionSite.cpp` injects there), and `Screen`'s WebIDL accessors live on
  `Screen.prototype` with `configurable: true`, so a getter can replace them.
* The semantics that keep both halves honest are `screen.width := innerWidth` (and
  `availWidth := innerWidth`, `height/availHeight := innerHeight`), not a fixed 980: the virtual
  screen must equal the LAYOUT width at every zoom, or the CSS breakpoint and the JS clamp disagree
  again at the next zoom. At zoom 100 that is still 720 and nothing changes; at zoom 80 it is 900.
* The switch is the existing UA/desktop toggle, and the provider is swapped on the live session, so
  the override belongs to the NEXT document -- same as the UA header. Never swap it from inside the
  pump: replacing injected style sheets mid-load is not re-entrant in this port.
* A site is free to re-measure whenever it likes. This one's bundle re-runs
  `Se.setState({width: Mu(), height: innerHeight})` from a `window.addEventListener("resize", Ss)`
  and from `load` (`+100 ms`), and pauses the simulation when `width <= mobileBreakpoint`. A repaint
  of the live page therefore moves only the CSS half of `Mu()`: on a document that started under the
  mobile UA there is no override on `Screen.prototype`, `screen.width` stays the frame width, and the
  gate stays closed no matter how wide the zoom re-lays the page out. The gate opens for a document
  that STARTS in desktop mode with the zoom already armed -- see the next section. Read the site's
  own handler AND which half of the gate a given lever moves before designing a probe around "the
  JS only checks at load".

Measured on the x64 bench with the override armed (desktop), `example.com` and then
`nokiadesignarchive.aalto.fi`: the mobile load injects nothing; the desktop load at 1355x751 logs
`[DG] screen 1355x751 inner 1355x751`, no `Simulation paused on mobile`, d3 loaded and the
simulation started. A home page loaded while `kW/kH` still held 720 writes
`[DG] screen 720x1080 inner 720x1080` and stays gated until the resize, which is the expected
order, not a failure of the override.

## The next load's zoom is armed before the session is built (`0.2.3.55`, 2026-10-21)

`WebCoreSetPageZoom` needs a live session, so it can only land after the document has parsed --
on the fixed-viewport ARM line that is after the first layout already ran at the CSS default. A
page that reads `window.innerWidth`/`screen.width` once, at load, therefore saw the unzoomed
geometry no matter what the Settings slider said, and on the Lumia the Nokia gate read 720 DIP
even with Desktop mode on.

* `WebCoreSetNextSessionZoom(float)` (exported; listed in `WebCoreDriver.exports.def`) writes
  `g_nextSessionZoom`; `buildSession` reads it and calls `setPageZoomFactor` AFTER
  `localMainFrame->init()` but BEFORE the DocumentWriter is fed, so the first layout is already
  `viewport / zoom` -- 720 DIP at 80 % is 899 CSS px, and the port says so in
  `[GPU] session built at zoom 0.80 ...: layout width 899 css in 720x1080 dip`.
* MainPage now arms it UNCONDITIONALLY on the engine thread immediately before every
  `WebCoreSessionLoad` (`zoomPctForLoad`, the Settings value, including 1.00). The call is cheap,
  it makes the slider value part of the load's identity, and it removes the whole class of bugs
  where one code path forgot to arm. `teardownSession` resets `g_pageZoom` but not
  `g_nextSessionZoom`: the next arm always follows.
* The pairing that opens the Nokia gate on the phone is `ua desktop` (arms the document-start
  screen override) + `zoom 80` (arms the pre-build zoom) + a FRESH load. Measured on the x64
  bench: `[DG] doc screen 900x1350` and no `Simulation paused on mobile` after that point. On the
  ARM line the same pair must work in BOTH orientations at fitwindow=0: the engine viewport is the
  fixed 720x1080 DIP frame, so turning the phone does not change the CSS width.
* Probe hygiene from the same session: `diag contents=` does NOT report the CSS layout width -- the
  `[GPU]`/`[DG]` markers above do; and System.Drawing's `GetPixel` lies on the bench screenshots
  (RGBA-PNG read as dark values on a white page), so measure pixels through the image reader, not
  GDI+.

## The form-action guard outlives the form that set it (2026-10-25)

`dzen.ru`'s first visit was reported as a white window, and it was not a failed render: the engine
committed `https://sso.dzen.ru/install?uuid=...`, painted nothing (`body=0`, `nonwhite=0/777600`,
empty title), and stopped there. The chain is a form, not a link: `dzen.ru` 302s to
`sso.passport.yandex.ru/push`, whose empty-body script POSTs to `sso.dzen.ru/install`; that response
has CSP `form-action 'none'` and an inline script that calls `window.location.replace(retpath)`.
The redirect to `https://dzen.ru/?is_autologin_ya=true&sso_failed=blocked&uuid=...` was refused:

```
[JS] security/ERROR -:0:1 Refused to load https://dzen.ru/?... because it does not appear
in the form-action directive of the Content Security Policy.
```

`curl` gets a 200 with 2.3 MB of HTML for that same URL, so the server is fine. The refusal is the
engine applying the install page's CSP to a navigation that is not a form submission at all.

**Mechanism.** `FrameLoader::submitForm` stores the action URL in `m_submittedFormURL`
(`FrameLoader.cpp:617`). `DocumentLoader::willSendRequest` then runs every later request through
`checkIfFormActionAllowedByCSP`, which checks it against the *current* document's CSP
(`DocumentLoader.cpp:654`). Upstream clears the field on a main-resource error
(`receivedMainResourceError`, `FrameLoader.cpp:3949`) and on user gestures
(`EventHandler.cpp:2022/4230`) -- but not on a successful commit. After the install response commits,
the guard still names the install URL, and the install document's `form-action 'none'` is applied to
the next navigation. A comment that calls this a "multiple form submission" guard describes only
half of its life.

**Fix.** The port's `dispatchDidCommitLoad` now calls
`m_portLoader->resetMultipleFormSubmissionProtection()`. The submission is finished when its
response commits. A genuinely new form is still checked by `submitForm` against its own document's
CSP (`FrameLoader.cpp:596`), and redirects *during* a submission are still checked because the guard
survives until that submission commits. The include `<WebCore/FrameLoader.h>` is required: the method
is not on the client interface.

**Verification.** x64, `0.2.3.73` app line with the rebuilt driver, as a side-by-side
`ME.Apotheosis.DzenTest 0.2.3.77` package so the installed `.73` app and its LocalState were
untouched. The real first visit now reaches
`https://dzen.ru/?sso_failed=blocked&uuid=102e6343-66e4-4d46-bfcc-02fa666e23f0`:
`body=1`, `nonwhite=228105/777600`, `scripts=49`, non-empty title, and no `form-action` error in
`gpu-steps.txt`. A local two-origin harness reproduced the exact chain deterministically
(push form -> 8101 install with `form-action 'none'` and `location.replace` -> 8100 final page):
the whole chain ends at title `REDIRECT OK`. The negative control -- a form on a page whose CSP is
`form-action 'none'`, loaded through the engine -- is still refused with the same console message,
and a plain form still submits (`FORM OK`).

**Probe hygiene, learned the hard way.** A top-level `goto` does not go through `DocumentLoader`:
the port fetches with curl and feeds `DocumentWriter` directly (see the `SL: fetch` path), so CSP
headers on a `goto`-loaded main document are not applied to it. My first negative-control page was
loaded with `goto`, its `form-action 'none'` did nothing, and the form went through -- which looked
like the fix had disabled CSP. It had not. Any future CSP test must load the test page through the
engine (`location.href` or a form from a fed page), or it measures the port's bypass instead of the
policy.

## A user-agent string is a contract with one site, not a browser identity (2026-10-25)

`dzen.ru` had two independent gates. The first was a mismatch: the document's
`navigator.userAgent` was the iPhone string while the top-level curl fetch sent
Safari-on-Windows, and dzen's `browser-switch-trap` read the pair as an unsupported desktop browser.
Unifying both halves behind `Src/port/PortUserAgent.h` and defaulting a desktop window to Edge
removed it.

The second gate appeared only after the first was gone: Yandex Direct served a "download Yandex
Browser" splashscreen to Edge. A Yandex Browser UA suppressed it, but making that global immediately
changes what every other site is told about the client; it is not a cosmetic string. The port now
selects the desktop UA by host (`dzen.ru` and the Yandex domains in `PortUserAgent.h` get
`YaBrowser/24.1.0.0`, everything else keeps Edge), with custom UA still first and mobile still
iPhone.

Two probe lessons:

* `microsoft.com` returned "Your request has been blocked" with BOTH the Yandex custom UA and the
  Edge default. Do not use it as a one-bit UA test; its block is IP/TLS/header policy. Measure the
  UA branch on a host you control, where the request headers are visible.
* The suffix matcher is deliberately not a substring test. `notdzen.ru`, `dzen.ru.example.com` and
  `dzen.ru.evil.com` must stay on the ordinary UA; a `strstr` would have put all three on Yandex.
  The matcher passed 21 URL cases on the bench.


## An iframe is a second Page boundary, not a widget (2026-10-25)

`createFrame()` returning nullptr is not a missing renderer: it is the absence of a `LocalFrame`.
The fix has to reproduce what WebKitLegacy's `_createFrameWithPage` does, in this order:

* **Merge policy downwards.** Effective sandbox flags are the owner's flags plus the parent frame's
  effective flags; the child of a sandboxed iframe must not escape it. The referrer policy comes
  from the owner element unless it is the empty string, in which case the *top* document's policy
  applies. Using either default for both cases is wrong for at least one real page.
* **Build the frame, then `init()`.** `LocalFrame::createSubframe` plus
  `tree().setSpecifiedName(name)` plus `FrameLoader::init()` gives the frame its initial empty
  document. `setSpecifiedName` is not cosmetic: without it `target="name"` finds nothing and a link
  click either does nothing or asks for a window that does not exist in this port.
* **The view belongs to the frame, and its creation belongs to the commit.**
  `transitionToCommittedForNewPage` creates a fresh subframe `LocalFrameView`; the main frame is the
  exception because the driver already owns that view. Attachment must work in both orders:
  `LocalFrame::createView` sets the owner renderer's widget if layout has already run, and
  `HTMLFrameElementBase::didAttachRenderers` covers the renderer arriving later. Implementing only
  one of the two leaves a frame that loads but never paints.
* **Detachment is already complete.** `HTMLFrameOwnerElement::disconnectContentFrame()` ->
  `LocalFrame::frameDetached()` -> `FrameLoader::frameDetached()` tears down loaders, children and
  the view; no port override is needed. One would only duplicate the work.

**Compile-time trap.** A FrameLoaderClient implementation that used to live in
`EmptyClients.cpp` calls `Document::frame()`, `Frame::page()` and `LocalFrame::view()`. Their
definitions are in `FrameDestructionObserverInlines.h`, `DocumentPage.h`, `DocumentView.h` and
`NodeDocument.h`, not in the declaring headers; without those includes the new translation unit
links with unresolved externals. `generated/FrameTreeSyncData.h` is also required by the
`FrameTreeSyncData::create()` call.

**Testing frames.** Three instruments, each for one question:

* the harness server log (`Host` + `Referer`) proves that a subframe actually issued the request and
  what policy it carried;
* the `FRAME: created` / `FRAME: committed view` lines in `gpu-steps.txt` prove the frame object and
  its view exist and commit twice (empty document, then the real load). The created count staying
  flat is the proof that `target=name` reused a frame instead of making one;
* a screenshot proves paint, because `diag` is a snapshot from the last render, not a live read: a
  post-load `document.title` or DOM change does not appear in it. Use visible text and network
  markers for anything that happens after the load.

**AppContainer loopback.** A local server is invisible to the packaged browser until the package
family is exempted. On this bench the syntax that works is
`CheckNetIsolation LoopbackExempt -a '-n=ME.Apotheosis_cj4nrm0qt9s1y'`; without the quotes
`-n=...` is parsed as a parameter and the command answers "Invalid Parameters". Remove the
exemption when the harness is done.
