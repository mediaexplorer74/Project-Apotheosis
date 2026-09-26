# Lessons that still matter

These are short operating rules distilled from the detailed reports in [Archive/](Archive/).

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
- Never reproduce a port compile rule by guessing a short include list. Extract the complete
  `DEFINES`/`FLAGS`/`INCLUDES` from the exact build tree (`build.ninja`); the full list carries generated
  headers and platform-specific paths that short lists silently omit.
- A successful WebKit build plus successful port-object compilation is not yet a working driver DLL. The
  link boundary may still require the native `WebCoreFull`-style archive for WebCore internal symbols.
