# Project Apotheosis — EdgeHTML Reborn

> Start here: [Doc/INDEX.md](Doc/INDEX.md) · [MVP](Doc/MVP.md) · [Plan](Doc/PLAN.md)
> Historical investigations are preserved in [Doc/Archive/](Doc/Archive/).

Apotheosis ports modern WebKit/WebCore to Windows 10 Mobile (ARM32, UWP) for the Lumia 950.
The MVP is intentionally small: an address bar, Back, a painted page, scrolling, and useful navigation.

The x64 Release harness is the daily verification line. The ARM32 Lumia 950 remains the target device.
Software presentation is the current safe path; GPU presentation work is deferred.

## Build

- [x64 build notes](Doc/BUILD-NOTES-X64.md)
- [ARM32 build guide](Doc/ARM32-BUILD-GUIDE.md)
- [GPU liveness notes](Doc/GPU-LIVENESS.md)
- [Repository constraints](CLAUDE.md)

## Publish

Read [Doc/GITHUB.md](Doc/GITHUB.md) before publishing. The first publication is a reviewed checkpoint,
not a force push. The external backup should remain intact until the new remote commit is verified.

## Scope

See [Doc/MVP.md](Doc/MVP.md) for the acceptance checklist. GPU repair, subframes, authentication, video,
MSE, and broad performance work are not MVP blockers.

## License

MIT for the port and harness; upstream WebKit and dependencies retain their own licenses.
