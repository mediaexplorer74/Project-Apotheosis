# Developer setup - x64 first, ARM compile-only

This is the from-a-clean-machine guide for **Apotheosis 0.2.3.73**. It is written for
someone who has never built WebKit and has a normal, possibly slow, internet
connection. Read the whole page once before starting: the x64 path is long but
linear, and the ARM path is deliberately not a runtime path.

## 0. What is in the repository, and what is not

Included:

- `Src/port/` - the Apotheosis WebCore driver, port-layer clients, stubs and build
  scripts.
- `Src/Apotheosis/` - the C# UWP browser that ships (`Apotheosis.csproj`).
- `Src/harness/` - the older C++/CX diagnostic host; useful, not the product.
- `Src/angle/include/` - ANGLE headers; the binaries are downloaded.
- `Src/Apotheosis/Assets/fonts/` - the fonts the AppContainer can actually use.
- `WebKitPatches/apotheosis-webkit-2.52.4.patch` - **all** of the WebKit changes,
  as a reviewable 288 KB patch against `webkitgtk-2.52.4`
  (`7acdf5e11af175e98cf7dcfcbcfcde11a50bc6ce`).

Not included, and never uploaded:

- `WebKit/` - the multi-gigabyte engine checkout.
- `build-x64-gpu/`, `build-arm32-gpu/`, `deps-build/`.
- DLL/LIB/EXE/PDB/appx outputs, downloaded ANGLE binaries, generated response
  files, LocalState logs, PFX/CER files.

`git` contains only sources; `.gitignore` enforces the rest. If you are reviewing
a clone, `Src/tools/audit-source-release.ps1` proves this before publication.

## 1. Two build lines and the ARM policy

| Line | What it is for | Allowed in this project |
|---|---|---|
| x64 | Develop, build, install, run, take screenshots, run the M1 cookie test | yes |
| ARM32 | Compile and audit a package for the Lumia 950 | **compile/audit only - do not execute** |

The x64 line is the fast diagnostic mirror. The ARM package is built for the
Windows 10 Mobile target but is not run in this repository's workflow; ARM
execution is forbidden by project policy. When this guide says ARM, it means
"build the files and inspect their PE headers", not "launch them".

A realistic machine:

- Windows 10 21H2 or Windows 11, x64.
- 16 GB RAM recommended; 8 GB works; the historical 4 GB box needs `-j1`.
- 25 GB free disk (WebKit checkout + two build trees + dependencies).
- A GitHub account is not needed to build. It is only needed to publish.

## 2. Install the toolchain

Install Git first, then use an **Administrator** PowerShell for the machine-wide
tools.

```powershell
winget install --id Git.Git -e
winget install --id LLVM.LLVM -e
winget install --id Kitware.CMake -e
winget install --id Python.Python.3.12 -e
winget install --id RubyInstallerTeam.Ruby.3.4 -e
```

`configure-gpu-x64.ps1` expects Ninja at `C:\ProgramData\chocolatey\bin\ninja.exe`.
The easiest way to satisfy that exact path is Chocolatey:

```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
iex ((New-Object Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
choco install ninja -y
```

Keep the installed paths at their defaults if possible. The scripts pin:

- Visual Studio: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- MSVC: `14.44.35207`
- Windows SDK: `10.0.19041.0`
- CMake: `C:\Program Files\CMake\bin\cmake.exe`
- LLVM: `C:\Program Files\LLVM`
- vcpkg: `C:\vcpkg`

If your Visual Studio is in another edition or drive, edit `Src/setenv.ps1`,
`Src/port/x64-uwp-env.ps1` and `Src/port/arm32-uwp-env.ps1` **before** building.
Do not mix a stale CMake cache with new paths; see Troubleshooting.

### Visual Studio 2022 workloads

Open Visual Studio Installer -> Modify and select:

- **Universal Windows Platform development**
- **Desktop development with C++**
- Individual components:
  - `Windows 10 SDK (10.0.19041.0)`
  - `MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44)`
  - `C++ (v143) Universal Windows Platform tools`
  - for ARM only: `MSVC v143 - VS 2022 C++ ARM build tools (v14.44)`

The versioned ARM component matters: VS 2022 17.14 does not ship a
`vcvarsamd64_arm.bat` that points at 14.44. The repository's
`Src/tools/arm-bootstrap.ps1` creates the small SDK junction/shims the ARM line
needs. Run it only if an ARM build complains about missing ARM libs.

### Git long paths

WebKit contains long paths. Enable long-path support before cloning:

```powershell
git config --global core.longpaths true
```

## 3. Install vcpkg dependencies

Clone and bootstrap vcpkg once:

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\vcpkg
```

Open a **new** PowerShell so `VCPKG_ROOT` is visible. From the repository root:

```powershell
$vcpkg = 'C:\vcpkg\vcpkg.exe'
& $vcpkg install `
  brotli bzip2 "curl[non-http,openssl]" expat `
  "freetype[brotli,bzip2,png,zlib]" icu libiconv libjpeg-turbo libpng `
  "libwebp[libwebpmux,nearlossless,simd,unicode]" "libxml2[iconv,zlib]" `
  openssl pixman pthreads zlib `
  --triplet x64-uwp `
  --overlay-ports=Src\port\vcpkg-ports
```

This is the x64 set. It is intentionally explicit rather than manifest-driven so
the versions are reproducible on a weak connection. Expect roughly 10-20 minutes
and 1-2 GB of downloads.

For the ARM compile-only line, install the same set with `--triplet arm-uwp`, but
**omit `pixman` and `pthreads`**: the ARM pixman/cairo/harfbuzz line uses the
scripts in `Src/port` and the `arm-uwp` prefix. The ARM dependency scripts also
expect the Cairo/HarfBuzz source archives under `C:\vcpkg\buildtrees`; if those
directories are absent, the ARM line is maintainer-only until those archives are
obtained.

### ANGLE

ANGLE is not committed. Download the last Microsoft UWP package and place both
architectures where the projects expect them:

```powershell
pwsh -File Src\tools\Get-Angle.ps1
```

Expected result:

```
Src\angle\x64\libEGL.dll  libEGL.lib  libGLESv2.dll  libGLESv2.lib
Src\angle\arm\libEGL.dll  libEGL.lib  libGLESv2.dll  libGLESv2.lib
Src\angle\include\angle_windowsstore.h
```

The script checks SHA-256 against the known 2.1.13 DLLs. If the download is
interrupted, rerun it; it replaces the temporary extract each time.

## 4. Get the WebKit source (exact tag + port patch)

From the repository root:

```powershell
pwsh -File Src\tools\fetch-webkit-source.ps1
```

What it does:

1. Creates `WebKit/` (git-ignored).
2. Configures a sparse checkout for the directories the port patch touches.
3. Fetches the **exact** tag `webkitgtk-2.52.4`, commit `7acdf5e1`, shallow and
   blobless. There is deliberately no fallback to `main`; a newer WebKit will not
   accept this patch.
4. Applies `WebKitPatches/apotheosis-webkit-2.52.4.patch` with `git apply --3way`.

Re-running is safe: if `git apply --check --reverse` succeeds, the tree is already
patched and the script leaves it alone. To inspect an unpatched upstream tree:

```powershell
pwsh -File Src\tools\fetch-webkit-source.ps1 -SkipPatch
```

Disk use is roughly 3-6 GB including `.git`. The full WebKit repository, tests and
LayoutTests are not downloaded. If the fetch fails, check the tag exists and retry;
do not switch the patch to a different WebKit revision.

Verify the patch:

```powershell
git -C WebKit apply --check --reverse ..\WebKitPatches\apotheosis-webkit-2.52.4.patch
git -C WebKit status --short --untracked-files=all
```

The reverse check must succeed. The status should list the patched `Source/` files,
not `Tools/` or build outputs.

## 5. Build the x64 native dependencies

The build scripts write into `build-x64-gpu/libs` and `C:\vcpkg\installed\x64-uwp`.
They are idempotent; rerun with `-Clean` only when you mean it.

### 5.1 SQLite with the WinRT VFS

Download the SQLite amalgamation (the project uses 3.49.1) from
<https://www.sqlite.org/download.html>, extract it, then:

```powershell
pwsh -File Src\port\build-sqlite-x64.ps1 `
  -Amalgamation C:\src\sqlite-amalgamation-3490100
```

The directory must contain `sqlite3.c` and `sqlite3.h`. The script compiles with
`SQLITE_OS_WINRT=1` and `/MT`, then copies `sqlite3.lib` and `sqlite3.h` into the
vcpkg prefix **and** `build-x64-gpu\libs\sqlite3.lib` (the WebKit CMake file points
at the latter).

### 5.2 The libpsl stub

The UWP curl build has no libpsl, but one WebCore object references four libpsl
names. Build the small, tracked stub:

```powershell
pwsh -File Src\port\build-psl-stub.ps1 -Arch x64
```

Output: `build-x64-gpu\libs\psl\psl.lib`. It exports only `psl_builtin`,
`psl_free`, `psl_is_public_suffix2` and `psl_registrable_domain`, matching the
archive the original driver linked.

### 5.3 Static Cairo

`WebCore` compiles against Cairo headers, but the driver links a complete static
Cairo archive. vcpkg's x64 Cairo archive is incomplete for this port. The script
downloads Cairo 1.18.4 from the official release site (about 32 MB), compiles the
image + Win32 + FreeType subset, and archives it:

```powershell
pwsh -File Src\port\build-cairo-x64.ps1
```

Output: `build-x64-gpu\libs\cairo-complete-x64-uwp.lib`.

If your connection is slow, pre-download
`https://cairographics.org/releases/cairo-1.18.4.tar.xz` and pass it:

```powershell
pwsh -File Src\port\build-cairo-x64.ps1 -Archive C:\Downloads\cairo-1.18.4.tar.xz
```

The script refuses to write an archive if a required source fails. Quartz, Xlib,
XCB, GL, PDF, PS, SVG, Script and Tee backends are intentionally excluded; the
driver uses the image/Win32/FreeType backend, and the disabled features are 0 in
`cairo-features.h`.

## 6. Configure and build WebKit for x64

Start a fresh PowerShell for this section. The environment scripts are not
persistent across terminals.

```powershell
. .\Src\setenv.ps1
. .\Src\port\x64-uwp-env.ps1
pwsh -File Src\port\configure-gpu-x64.ps1 -Config Release
```

`configure-gpu-x64.ps1` runs CMake with the WinUWP toolchain and enables JIT +
TextureMapper/ANGLE. It asserts that the shell is not ARM-primed before it writes
the cache. If CMake reports a stale compiler or an ARM path, delete
`build-x64-gpu\CMakeCache.txt` (or the whole build directory) and reconfigure.

Build the engine. This is the long step: hours on a 4 GB machine, less on a modern
one. Use `-j1` on a low-memory box; it is not optional there because WebKit's
unified translation units can exceed the free RAM.

```powershell
ninja -C build-x64-gpu WebCore JavaScriptCore -j1
```

The targets produce:

```
build-x64-gpu\lib\WebCore.lib
build-x64-gpu\lib\JavaScriptCore.lib
build-x64-gpu\lib\PAL-x64-19041.lib   (if the CMake target emitted it)
build-x64-gpu\bin\JavaScriptCore.dll
```

If `PAL-x64-19041.lib` is missing, that is expected on this CMake line; the next
step creates it.

### 6.1 Static archives

The driver links WebCore statically and needs two archives:

```powershell
pwsh -File Src\port\archive-webcorefull.ps1 -Arch x64
pwsh -File Src\port\archive-pal.ps1 -Arch x64
```

Expected counts (2026-10-23): **938** WebCore objects and **24** PAL objects.
`archive-webcorefull.ps1 -CheckOnly` verifies the object list without writing the
3.2 GB archive. The scripts use response files with absolute member paths, so
rerun them after moving or renaming the build tree.

Do not use the old `build-x64-gpu\run-webcorefull-archive.ps1`; it was a generated
file and is not part of the source release. Use the tracked scripts above.

### 6.2 Link the driver

```powershell
pwsh -File Src\port\build-driver-dll-arm32.ps1 -Arch x64
```

Despite the filename, it builds either architecture. It compiles the port layer
with clang-cl, links `WebCoreFull-x64-19041.lib` + `PAL-x64-19041.lib`, and stages
`WebCoreDriver.dll` into the payload. The script backs up the previous payload
driver under `Src\port\_driver-backups`.

Check the result before packaging:

```powershell
llvm-readobj --file-headers Src\port\WebCoreDriver-x64.dll
llvm-readobj --coff-imports Src\port\WebCoreDriver-x64.dll
```

It must be `IMAGE_FILE_MACHINE_AMD64`, import `JavaScriptCore.dll` (not
`WebCore.dll`), and import the desktop CRT (`MSVCP140.dll`,
`VCRUNTIME140.dll`), not the `_APP` CRT names. The build script handles the x64
store-CRT trap; if you link by hand, read `Doc/BUILD-X64.md` section 7.

### 6.3 Stage the native payload

ANGLE, ICU, curl/OpenSSL, image codecs and the other DLLs are not committed. The
staging script walks the driver's import table recursively and copies only what is
needed:

```powershell
pwsh -File Src\tools\Stage-NativeDeps.ps1 -Arch x64
```

On a fresh clone there is no existing `JavaScriptCore.dll`, so it copies
`build-x64-gpu\bin\JavaScriptCore.dll` and the desktop CRT. The `_APP.dll` family and
`D3DCOMPILER_47.dll` are framework/OS dependencies and are deliberately not staged.
If you are preserving a known-good JSC that ships in a package and do not want the
freshly built one, add `-KeepExistingJsc`; do this knowingly, because the driver
and JSC should come from the same WebKit build.

The dependency closure is the driver plus 28 imported DLLs (29 files total),
including `icudt78.dll`, `libGLESv2.dll`, `libEGL.dll`, `libcurl.dll`,
`libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `libwebp.dll`, `freetype.dll`,
`harfbuzz.dll`, `harfbuzz-icu.dll`, `pixman-1-0.dll`, `jpeg62.dll`,
`libpng16.dll`, `libxml2.dll`, `z.dll` and the desktop CRT. The verified `.73`
payload also carried `brotlienc.dll` and `icuio78.dll`, which no import table
references; they are harmless extras, not part of the closure. No `WebCore.dll`.

## 7. Build, register and run the C# app

The C# project is `Src/Apotheosis/Apotheosis.csproj`; it targets SDK 19041 and
copies `native\x64\*.dll` to the package root. Build it with the VS 2022 MSBuild
(the `dotnet` SDK is not a UWP XAML build):

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild Src\Apotheosis\Apotheosis.csproj /restore `
  /p:Configuration=Release /p:Platform=x64 `
  /p:AppxPackageSigningEnabled=false `
  /v:minimal
```

The output layout is `Src\Apotheosis\bin\x64\Release\` and contains
`AppxManifest.xml`, `Apotheosis.exe`, the managed assemblies and the native
payload. That is enough for a local **register** install; no certificate is
needed:

```powershell
# Settings -> Update & Security -> For developers -> Developer Mode must be on.
Add-AppxPackage -Register Src\Apotheosis\bin\x64\Release\AppxManifest.xml
```

Launch it:

```powershell
Start-Process 'explorer.exe' 'shell:AppsFolder\ME.Apotheosis_cj4nrm0qt9s1y!App'
```

The package family name is derived from the manifest publisher and name; if it
differs on your machine, use:

```powershell
(Get-AppxPackage ME.Apotheosis).PackageFamilyName + '!App'
```

`Add-AppxPackage -Register` fails if the same version is already installed. For a
clean test:

```powershell
Get-AppxPackage ME.Apotheosis | Remove-AppxPackage
```

That removes the app's LocalState, including the M1 cookie DB. Back up
`LocalState` first if you need the evidence.

### Optional: a signed appx

For a package you can copy to another machine, create a test certificate whose
subject matches `Package.appxmanifest` (`CN=media`), export it to
`Src\Apotheosis\Apotheosis_TemporaryKey.pfx`, and pass the thumbprint:

```powershell
$cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=media' `
  -KeyUsage DigitalSignature -FriendlyName 'Apotheosis test' `
  -CertStoreLocation 'Cert:\CurrentUser\My'
$pwd = ConvertTo-SecureString -String 'apotheosis' -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath Src\Apotheosis\Apotheosis_TemporaryKey.pfx -Password $pwd
$cert.Thumbprint
```

Build with `/p:PackageCertificateThumbprint=<thumbprint>
/p:PackageCertificatePassword=apotheosis` and install the generated `.appx` with
`Add-AppxPackage`. The PFX and CER are ignored by git; never commit them.

## 8. First smoke test and the M1 cookie check

After launch:

1. Wait for the address bar to become usable.
2. Open `https://example.com`; the page should paint, not just report a load.
3. Open
   `%LOCALAPPDATA%\Packages\ME.Apotheosis_cj4nrm0qt9s1y\LocalState\` and confirm
   `stage.txt`, `cookie.jar.db` and the trace files appear.

The M1 acceptance is specifically about restart persistence:

```powershell
# terminal 1: local cookie server
python Src\tools\cookie-test-server.py 8099
```

Use the machine's LAN address from the app (the AppContainer may not reach
loopback). In the app, navigate to `http://<lan-ip>:8099/set`, then `/check`; the
page title should read `COOKIE=apo=m1`. Close the app completely, launch it again,
and open `/check` again. The same title must appear. `cookie.jar.db` in LocalState
must still contain the row after the restart.

For a scripted check, write `goto http://<lan-ip>:8099/check` to
`LocalState\autocmd.txt` and read `autocmd-log.txt`; the command channel is
documented in `Src/Apotheosis/MainPage.AutoCmd.cs`.

## 9. ARM32: build and audit, never execute

This is a compile-only mirror. It uses the same sources and version, but **do not
run the resulting package** in this project.

Prerequisites:

- The Visual Studio ARM component listed in section 2.
- The SDK 19041 ARM libraries and the `arm-uwp` vcpkg prefix.
- `Src/tools/arm-bootstrap.ps1` has been run if the ARM SDK junction is missing.
- The ARM Cairo/HarfBuzz/fontconfig source archives are present in
  `C:\vcpkg\buildtrees`; the scripts are pinned to those maintainer trees.

Build the ARM dependency archives (in order), then the engine:

```powershell
. .\Src\setenv.ps1
pwsh -File Src\port\build-pixman-arm32.ps1
pwsh -File Src\port\build-cairo-arm32.ps1
pwsh -File Src\port\build-harfbuzz-arm32.ps1
pwsh -File Src\port\build-fontconfig-arm32.ps1
pwsh -File Src\port\build-sqlite-arm32.ps1 -Amalgamation C:\src\sqlite-amalgamation-3490100

. .\Src\port\arm32-uwp-env.ps1
pwsh -File Src\port\configure-gpu-arm32.ps1 -Config Release
ninja -C build-arm32-gpu WebCore JavaScriptCore -j1
pwsh -File Src\port\archive-webcorefull.ps1 -Arch arm
pwsh -File Src\port\archive-pal.ps1 -Arch arm
pwsh -File Src\port\build-driver-dll-arm32.ps1 -Arch arm
pwsh -File Src\tools\Stage-NativeDeps.ps1 -Arch arm
```

Build the ARM appx without launching it:

```powershell
& $msbuild Src\Apotheosis\Apotheosis.csproj /restore `
  /p:Configuration=Release /p:Platform=ARM `
  /p:AppxPackageSigningEnabled=false `
  /v:minimal
```

Audit the package before it leaves the machine:

```powershell
$out = 'Src\Apotheosis\bin\ARM\Release'
Get-ChildItem $out -Filter '*.dll' | ForEach-Object {
    $machine = (llvm-readobj --file-headers $_.FullName |
        Select-String 'Machine:').Line.Trim()
    '{0}  {1}' -f $machine, $_.Name
}
```

Every native DLL must say `IMAGE_FILE_MACHINE_ARMNT`; `WebCore.dll` must be absent;
the driver must export the expected 47 functions. The known `.73` audit found 30
ARM binaries, `WebCoreDriver.dll` at 31.4 MB, and 47/47 exports. Do not execute
them on an ARM device as part of this repository's workflow.

If the ARM scripts complain about missing `C:\vcpkg\buildtrees\...`, stop and
treat the ARM line as incomplete rather than substituting a different Cairo or
HarfBuzz build. The x64 line remains the reproducible path.

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| CMake finds ARM headers in an x64 build | A previous shell was ARM-primed. Start a new shell, dot-source `x64-uwp-env.ps1`, delete `build-x64-gpu\CMakeCache.txt`, reconfigure. |
| `assert-target` fails | The environment marker is stale. Open a new shell and only dot-source the script for the architecture you are building. |
| `git fetch` for WebKit fails | Retry; the script fetches only the tag. Check that `webkitgtk-2.52.4` is reachable. Do not use `main`. |
| Patch does not apply | The checkout is not `7acdf5e1`, or `Tools/` was modified. Re-run the fetch script; it detects an already-patched tree. |
| `sqlite3.lib` not found | Run `build-sqlite-x64.ps1` with the amalgamation directory; it must contain both `sqlite3.c` and `sqlite3.h`. |
| `cairo-complete` missing | Run `build-cairo-x64.ps1`. vcpkg's `cairo.lib` is not a substitute; it is incomplete. |
| Driver imports `MSVCP140_APP.dll` | The store CRT leaked into the x64 link. Re-run `build-driver-dll-arm32.ps1 -Arch x64`; it reorders `LIB` for the link. |
| `Add-AppxPackage` refuses the layout | Turn on Developer Mode, or sign with the test certificate in section 7. Check `bin\x64\Release\AppxManifest.xml` exists. |
| App starts but no page paints | Check `LocalState\stage.txt` and `diag.txt`; confirm `native\x64` has 31 files and no `WebCore.dll`. Do not "fix" it by adding WebCore.dll. |
| Build runs out of memory | Use `ninja -C build-x64-gpu -j1`, close Visual Studio, or reboot. WebKit unified TUs are large. |

## 11. Publishing this tree

Do not push `WebKit/` or the build trees; the repository is a source release plus
the patch set. Before publishing:

```powershell
pwsh -File Src\tools\audit-source-release.ps1
```

The audit must pass: no tracked WebKit/build/DLL/LIB/EXE/PFX paths, no binary
patch hunks, no common token patterns. It prints the largest tracked files and the
full-history bundle size (about 38.5 MB on the 2026-10-23 tree). To create a
source-only ZIP using `HEAD`:

```powershell
pwsh -File Src\tools\audit-source-release.ps1 -ZipPath "$env:TEMP\Apotheosis-source.zip"
```

Then follow `Doc/GITHUB.md`. The normal first push uploads the existing pack; an
orphan single-commit branch is only a fallback for a connection that cannot
complete the normal push.

## 12. Where to go next

- [INDEX.md](INDEX.md) - the documentation map.
- [ROADMAP.md](ROADMAP.md) - the live plan; M1 is the cookie jar.
- [BUILD-X64.md](BUILD-X64.md) - the x64 archive/link details and the CRT trap.
- [BUILD-ARM32.md](BUILD-ARM32.md) - the ARM line and its maintainer-only traps.
- [GITHUB.md](GITHUB.md) - the publication checklist.
- [ARCHITECTURE.md](ARCHITECTURE.md) - the C# host, the C ABI and the SEH contract.
