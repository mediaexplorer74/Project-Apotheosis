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
