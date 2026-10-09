# The ARM32 launch crash: a dangling reference in SecurityOrigin

The Lumia 950 built, deployed and started the browser, then died within a second of showing the home
page — every single time, across five builds. This is how it was found and what it was, written down
because the path to it is more reusable than the fix.

Dates: 2026-08-18 (first ARM32 appx) to 2026-08-19 (root cause). Device: Lumia 950, Windows 10 Mobile
15254.603, ARM32, over Device Portal at 192.168.1.35.

## The fault

```
UEF: unhandled exception code=0x80000002 at addr=00000021 tid=3648
UEF: PC=5A17983D [WebCore.dll] SP=18BAD2F0 LR=656FEA71
UEF: R0=1C0F3588 R1=000062A0 R2=1C0CE0B2 R3=00000021
```

`0x80000002` is `STATUS_DATATYPE_MISALIGNMENT`, not the access violation one expects from a bad
pointer. That detail is the first real clue: ARM's `LDREX` / `STREX` — the instructions behind
`std::atomic` — require 4-byte alignment, and `0x21` is odd. So an **atomic** operation through a
garbage pointer reports a misalignment, while an ordinary load through the same pointer would have
reported `0xC0000005`. Anything that faults at an odd address on ARM is therefore likely to be an
atomic, which in WebKit means a refcount.

## The stack, once symbolised

```
Harness!WebEngine::loop                                   MainPage.xaml.cpp:825
Harness!WebCoreRenderHtml
WebCore!FrameLoader::init                                 FrameLoader.cpp:439
WebCore!DocumentLoader::startLoadingMainResource          DocumentLoader.cpp:2186
WebCore!DocumentLoader::maybeLoadEmpty                    DocumentLoader.cpp:2123
WebCore!DocumentLoader::finishedLoading                   DocumentLoader.cpp:515
WebCore!DocumentLoader::commitData                        DocumentLoader.cpp:1279
WebCore!DocumentWriter::begin / ::createDocument          DocumentWriter.cpp:163 / :149
WebCore!DOMImplementation::createDocument                 DOMImplementation.cpp:165
WebCore!HTMLDocument::HTMLDocument                        HTMLDocument.cpp:101
WebCore!Document::Document                                Document.cpp:727
WebCore!Document::initSecurityContext                     Document.cpp:8520
WebCore!ContentSecurityPolicy::ContentSecurityPolicy      ContentSecurityPolicy.cpp:122
WebCore!ContentSecurityPolicy::updateSourceSelf           ContentSecurityPolicy.cpp:296
WebCore!ContentSecurityPolicySource::ContentSecurityPolicySource  ContentSecurityPolicySource.cpp:49
WebCore!WTF::String::String                               WTFString.h:90   (copy constructor)
WebCore!WTF::RefPtr<StringImpl>::RefPtr                   RefPtr.h:51
WebCore!WTF::DefaultRefDerefTraits<StringImpl>::refIfNotNull  Ref.h:51
WebCore!WTF::StringImpl::ref                              StringImpl.h:1190
WebCore!std::_Atomic_integral<unsigned,4>::fetch_add      atomic:1435   <- faults
```

Reading it plainly: creating the initial empty document builds a Content Security Policy, which builds
its "self" source from the document's security origin, which copies a `String` — and that copy
increments a refcount through a pointer worth `0x21`.

## The root cause

```cpp
// SecurityOrigin.h
const String& protocol() const { return m_data.protocol(); }
const String& host()     const { return m_data.host(); }

// SecurityOriginData.h
String protocol() const;   // by value
String host() const;       // by value
```

Both accessors return **a reference to a temporary**. `m_data.protocol()` materialises a `String`,
the accessor returns a reference to it, and the temporary is destroyed at the end of that return
expression. Every caller then reads a dead object. This is undefined behaviour in the plain sense —
not a platform quirk, not a compiler bug.

x64 survives it because the generated code happens not to reuse that stack slot before the caller
copies out of it. ARM32 clang-cl does reuse it, so the `RefPtr<StringImpl>` copy inside
`ContentSecurityPolicySource` refs whatever now occupies the slot. In this case `0x21` — which,
measured in the dump, is the **refcount word of a static empty `StringImpl`** read as though it were
the pointer to one:

```
memory at 65931F58 (a StaticStringImpl inside JavaScriptCore.dll):
  + 0: 00000021   <- m_refCount: 0b100001 = static-string flag set, count 16
  + 4: 00000000   <- m_length = 0
  + 8: 657AA7AD   <- character pointer
  +12: EC889E1C   <- m_hashAndFlags
```

## The fix

`SecurityOrigin::protocol()` and `::host()` now return `String` by value under `WK_WINUWP`. There are
35 call sites of those two accessors in WebCore and none takes the address of the result, so it is a
drop-in change that fixes every one of them at once. The cost is a `String` copy — a refcount
increment — on paths that were previously getting a reference they had no right to keep.

An earlier attempt patched only the `protocol()` call inside `updateSourceSelf`. That moved the crash
to `host()`, the very next argument, with an identical signature and an identical stack. **A fix that
relocates a crash by one argument is a strong hint that the defect is in the callee, not the call.**

## How to do this again

Nothing here needed a debugger attached to the phone, which is fortunate, because VS 2022 dropped
ARM32 device debugging.

```powershell
# 1. turn on dump collection once
pwsh -File Src\tools\Wdp-Crash.ps1 -Enable -DeviceIp <ip>
# 2. launch the app and let it die, then fetch the dump
pwsh -File Src\tools\Wdp-Crash.ps1 -Pull -DeviceIp <ip>
# 3. symbolise it against our own PDBs -- and ONLY ours
cdb -z crash\Harness.exe.<pid>.dmp -y "build-arm32-gpu\bin" -lines -c "r;.ecxr;kb 40;q"
```

`cdb.exe` comes from the Windows SDK's "Debugging Tools for Windows" feature, which is easy to leave
unticked. Install it with `winsdksetup.exe /features OptionId.WindowsDesktopDebuggers`, elevated. Do
**not** add `.symfix+` to the cdb command: it points at Microsoft's symbol server and turns a
two-second analysis into a long download.

Two things in the harness did most of the work before cdb was available, and are worth keeping:

* `ctor-trace.txt` — one line per `MainPage` constructor checkpoint, written before `LogInit` makes
  `log.txt` live. All eleven lines were present here, which ruled out the entire startup path in one
  glance and pointed at the engine instead.
* the UEF dump in `log.txt` — exception code, module bases, registers. That alone gave the exception
  code and the faulting address, which is what made the alignment reasoning possible.

## Two false trails, and why they were false

**The probes were the crash.** Before this, the ARM32 build was dying inside
`apoTraceLowercaseInput`, an instrumentation function added to `StringImpl.cpp` to investigate the
very same failure. It took word 0 of a `StringImpl` — the refcount — cast it to a pointer, and
dereferenced it: guaranteed to fault for any static string. A second probe in `WTFString.cpp` read
sixteen bytes *before* its own object and, worse, contained `return { }` on an error path inside a
conversion function, so a diagnostic could silently replace a result with an empty string. Both were
rewritten to record the caller first, then validate before touching anything.

The lesson is narrow and worth stating: **a probe must not change what it observes and must not be
able to fault.** The readings those probes produced — "a 16-bit string with a 1.2 GB length", "m_impl
points at a bogus stack object" — were artefacts of reading the caller's stack frame and
misinterpreting fields, and they sent the investigation after a phantom for a long time.

**An arithmetic slip named the wrong functions.** Computing an RVA as `PC - moduleBase` by hand, the
subtraction was done wrong by `0x1000000`, and the symbolizer duly resolved that address to
`ClassInfo::isSubClassOf`, `castThisValue` and `JSDedicatedWorkerGlobalScope`. All plausible, all
wrong. A whole hypothesis about `StructureID` decoding on 32-bit was built on it before cdb — which
computes the RVA itself — showed the real function was `StringImpl::ref`.

Guard against both: let the tool compute the RVA (`cdb`, or `llvm-symbolizer --adjust-vma`), and treat
any symbol that arrives with a suspiciously exotic template argument as unverified until a second
source agrees. `/OPT:ICF` folds identical template instantiations, so the *name* attached to an
address can be one of many candidates even when the address is right.

## Still open

`SecurityOriginData::protocol()` and `::host()` returning by value is the upstream shape, and other
classes in this tree may hand out references to temporaries in the same way. Nothing systematic has
been done to look. A grep for `const String& \w+() const { return [a-z_]+\.\w+(); }` across
`Source/WebCore` would find candidates cheaply, and on ARM32 each one is a launch crash waiting for
the right allocation pattern.
