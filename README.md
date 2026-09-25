# Apotheosis — EdgeHTML Reborn

> **Start here:** [Doc/INDEX.md](Doc/INDEX.md) · [MVP](Doc/MVP.md) · [Plan](Doc/PLAN.md)
> Historical investigations are preserved in [Doc/Archive/](Doc/Archive/).

Apotheosis is a small WebKit/WebCore browser ported to Windows 10 Mobile (ARM32, UWP) for the
Lumia 950. The MVP has an address bar, Back, a painted page, scrolling, and useful navigation.

## Build status

- The x64 Release harness is the current daily verification line.
- The Lumia 950 ARM32 device line remains the target.
- Software presentation is the current safe path; GPU presentation work is deferred.
- This repository publishes the **Apotheosis port and harness**, not a complete WebKit fork. WebKit is a
  separately fetched upstream dependency. A verified WebKit patch series is not published yet, so this
  repository is not a one-command WebKit build.

## For developers

Start with the [document map](Doc/INDEX.md), then use:

- [x64 build notes](Doc/BUILD-NOTES-X64.md) for the verified x64 line.
- [ARM32 build guide](Doc/ARM32-BUILD-GUIDE.md) for the Lumia 950 line.
- [GPU liveness notes](Doc/GPU-LIVENESS.md) for the presentation investigation.
- [XAML contract](Doc/XAML.md) before changing generated C++/CX glue.
- [Repository constraints](CLAUDE.md) for the hard build rules.

### Visual Studio

Open `Apotheosis.sln`. The harness requires the C++/UWP build tools and the pinned v143 toolset.
Visual Studio builds the UWP harness; WebKit itself is built separately with its configured
clang-cl/CMake/Ninja line. Opening this solution does not rebuild WebKit.

### WebKit boundary

Until the WebKit patch series is prepared and verified against a clean upstream baseline, WebKit is an
external prerequisite. The intended next artifact is a small, ordered `Patches/` series plus a
reproducible fetch/apply script; that work is not claimed as complete here.

## Publish

Read [Doc/GITHUB.md](Doc/GITHUB.md) before publishing. Keep the external backup until the remote branch
has been reviewed and verified.

## Scope

See [Doc/MVP.md](Doc/MVP.md) for the acceptance checklist. GPU repair, subframes, authentication, video,
MSE, and broad performance work are not MVP blockers.

## License

The Apotheosis port and harness are MIT-licensed. Upstream WebKit and dependencies retain their own
licenses and are not relicensed by this repository.
