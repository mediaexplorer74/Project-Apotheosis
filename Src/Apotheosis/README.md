# Src/Apotheosis — the C# harness experiment (stopped, kept on purpose)

**Status: research reference, not a shipping project.** The working browser is the native C++/CX
harness in `Src/harness`. Nothing in the working browser depends on this directory.

This is a deliberately reduced C#/XAML harness: address bar and content view. It is not a half-finished
copy of `Src/harness` and it is not a drop-in replacement for the native harness.

## Why it exists

C# would give us readable UI code, catchable managed exceptions, and a smaller XAML surface. The
measured objections to managed marshalling are not currently a reason to declare the idea impossible:
blittable `DllImport` calls are cheap relative to a WebCore tick, and a pinned byte buffer can pass a
frame without an extra managed copy. The unknown is the UWP toolchain, especially ARM32 .NET Native.

The native harness also contains engine-thread plumbing, job labels, heartbeat diagnostics, packaged
test switches, navigation guards, and the GPU/software fallback. Replacing it with a small C# UI would
lose those measurements until they are deliberately ported.

## Architecture boundary

The C# project must talk to a C ABI, not to WebKit static libraries:

```text
C# UWP app
    ↓ P/Invoke
WebCoreDriver.dll
    ↓
WebCore.dll / JavaScriptCore.dll
```

`WebCoreDriver.cs` declares 30 P/Invoke calls. The C++/CX harness instead links native archives such as
`WebCoreDriver-gpu-x64.lib`, `WebCore.lib`, and `JavaScriptCore.lib`; those are native link inputs, not
managed references. `WebCoreFull.lib` is a special native workaround archive for unresolved WebCore
internals and is not a C# reference. Do not copy WebKit `.lib` files into `Apotheosis.csproj`.

A managed probe can be useful for a harmless C ABI round-trip and package check. It is not proof that a
site paints until the real WebKit runtime is loaded and the frame is inspected.

## What the project currently contains

- `App.xaml`, `MainPage.xaml`, and managed UI code;
- `WebCoreDriver.cs` with the 30-call managed ABI surface;
- SDK 19041 UWP project settings and x64/ARM configurations;
- a certificate reference and package assets, but no verified `WebCoreDriver.dll` packaging path;
- no project reference or native library reference to WebCore.

`Src/Apotheosis.sln` contains this project alone. `Apotheosis-mix.sln`, when present, is a visual
research solution only: opening it does not mean that Build Solution produces the browser. The engine
is still built separately with CMake/Ninja/clang-cl, and the driver DLL must be built and packaged
before a managed runtime probe is meaningful.

## If resuming it

0. Measured state 2026-09-26 (x64, Debug 0.2.0.5): builds, signs, installs and starts; the
   managed side reaches `WebCoreRenderHtml` and then the process dies inside the first paint at
   `FontCascadeFonts::realizeFallbackRangesAt` -> `cairo_scaled_font_create`; the native fault
   crosses the P/Invoke boundary and .NET Native turns it into `FailFast(7)` (WER 0x1007, module
   `SharedLibrary.dll`), so no managed catch can see it. Two earlier blocks are fixed and should
   stay fixed: the `Apotheosis_TemporaryKey.pfx` password is EMPTY (`PackageCertificatePassword`
   must stay empty or every build fails cert validation), and `native\x64` must ship the six
   transitive deps the engine needs - brotlidec/brotlicommon/brotlienc/bz2/iconv-2/libsharpyuv -
   plus the five fonts in `Assets\fonts`, or the process never reaches managed code at all
   ("The app didn't start", 0x8025025B). Next lever is native, not managed: wrap the `WebCore*`
   entry points in `Src/port/WebCoreDriver.cpp` in `__try/__except` so no exception can escape
   into the CLR, and log what fontconfig resolved there.
1. Finish the current x64 SDK 19041 WebKit build and record its result.
2. Build an empty C# UWP ARM32 package first. If that cannot be built and installed under VS 2022,
   stop: the next questions are irrelevant.
3. Build a real `WebCoreDriver.dll` for the chosen architecture and place it in the appx package.
4. Verify architecture, package identity, AppContainer deployment, and a harmless ABI round-trip.
5. Only then connect the UI to `WebCoreSessionLoad`, `WebCoreLiveTick`, and the paint buffer.
6. Port engine-thread plumbing, job queue, heartbeat, diagnostics, and navigation state before calling
   the managed harness a browser.

## Measurement honesty

A compile-only C# success means "managed project can be compiled." It does not mean WebKit works, ARM32
deployment works, or a page painted. Those are separate acceptance gates and must be reported as such.

The native line remains the shipping path:

```text
WebKit CMake/Ninja → WebCore/JavaScriptCore → port archive → C++/CX Harness.vcxproj → appx
```
