# Apotheosis — Document Map

This is the small front door to the project documentation. The detailed historical record is in
[Archive/](Archive/), kept for reference and not used as the current specification.

## Start here

- [MVP.md](MVP.md) — the v1 scope and acceptance checklist.
- [PLAN.md](PLAN.md) — live state, next actions, and measured open items.
- [LESSONS.md](LESSONS.md) — short record of failures worth not repeating.
- [XAML.md](XAML.md) — the XAML/C++/CX contract and safe UI rules.
- [GITHUB.md](GITHUB.md) — publishing this tree safely.

## Build and platform

- [BUILD-NOTES-X64.md](BUILD-NOTES-X64.md) — x64 build and harness details.
- [ARM32-BUILD-GUIDE.md](ARM32-BUILD-GUIDE.md) — Lumia 950 / ARM32 build line.
- [GPU-LIVENESS.md](GPU-LIVENESS.md) — GPU investigation and current presentation status.

## Policy

The MVP is a small browser that can open a useful set of sites, paint them, scroll them, and navigate
back. Do not start broad refactors while the acceptance checklist is still open. Preserve code that is
already measured to work; make the smallest change that makes the next measurement possible.
