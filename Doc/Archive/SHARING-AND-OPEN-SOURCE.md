# Sharing this project: what goes in a repository and what does not

Written 2026-08-21, because the working tree is about 22 GB spread over several places and the question
"how would anyone else get this?" had no answer. The short version: **you do not share the 22 GB. You
share the recipe for recreating it**, which is a few megabytes.

## Measured, so the decision rests on numbers

| Directory | Size | Share it? |
|---|---|---|
| `Doc/` | under 1 MB | **yes** |
| `Src/` | 3094 MB | **yes, but only the sources** — the bulk is build debris |
| `WebKit/` | 227 MB | **no** — share the *edits*, not the tree |
| `build-x64-gpu/` | 11013 MB | never |
| `build-arm32-gpu/` | 4698 MB | never |
| `deps-build/` | 89 MB | never — rebuilt by script |
| `C:\vcpkg` | 3205 MB | never — reinstalled by script |
| `C:\icu-x64-uwp` | 47 MB | never — rebuilt by `make-icu75-libs.ps1` |

Note `Src/` in particular. Three gigabytes there are not source: they are the loose `*.obj`, `*.lib`,
`*.dll`, `*.log`, `repro_*.cpp` and `_*.bat` residue left over from fighting the link, which `CLAUDE.md`
already warns agents to ignore. The actual hand-written code — the driver, the harness, the scripts — is
a few megabytes. A repository containing it fits comfortably in any free plan.

## The three categories, and the rule for each

**1. Hand-written and irreplaceable — must be versioned.** `Src/port`, `Src/harness`, `Src/tools`,
`Doc/`, `CLAUDE.md`, the READMEs. If this is lost, the project is lost. Today it is versioned nowhere,
which is the real problem behind "scattered across the disk".

**2. Downloadable at a known version — record the version, not the bytes.** The upstream WebKit tree
(webkitgtk-2.52.4), vcpkg packages, ICU, ANGLE binaries, fonts. Anyone can fetch these; the repository
stores *which* version and the script that fetches it. `Src/tools/fetch-webkit-source.ps1` and
`Src/tools/arm-bootstrap.ps1` already do exactly this.

**3. Generated — never versioned.** Everything under `build-*`, the appx packages, the PCHs, the logs.
Reproducible from 1 and 2, and larger than both by three orders of magnitude.

## The edits to WebKit are the crown jewels, and they need their own home

Every change this port makes to upstream is marked `#if defined(WK_WINUWP)` with an `Apotheosis:`
comment. That is the fork's actual intellectual content, and right now it exists only inside a 227 MB
copy of someone else's source tree on one laptop.

The standard way to keep it — and what "base plus diffs" means — is a **patch series**: the pristine
upstream release, plus a set of patch files that transform it into this fork's tree. Then someone else
runs two commands, gets an identical tree, and can rebase onto a newer WebKit later by re-applying the
series. Two ways to get there, in increasing order of comfort:

- **Patch files.** Compare the working tree against a freshly unpacked pristine copy, save the result as
  `Patches/*.patch`, and add an `apply-patches.ps1`. Simple, no new tooling, but the discipline of
  regenerating them after every edit is manual.
- **A git checkout of upstream with a branch on top.** Then each edit is a commit, `git format-patch`
  produces the series for free, and updating WebKit is a rebase rather than an archaeology exercise.
  A shallow clone of the one release tag avoids WebKit's enormous history. This is how ports are
  normally maintained and it is worth the initial setup.

Either way the repository stays small: the patches for this fork are text, and text compresses.

## GitHub specifics, in plain terms

**The free plan is not a constraint here.** Its limits are about Actions minutes on private
repositories and Large File Storage quotas. A public repository of source and documentation, a few
megabytes in size, sits far below anything that is metered.

**GitHub Actions are not needed, and their absence costs nothing.** Nothing in this build could run
there anyway: the CI images have no Windows SDK 10.0.19041.0, no MSVC ARM32 toolset, and no way to reach
a Lumia over Device Portal — and a full engine build takes hours on hardware we control. The role CI
would play is already filled locally by scripts that fail loudly and leave a verdict:
`Src/tools/arm-bootstrap.ps1` stops at the first failing step and writes `verdict.txt` naming it, and
`Src/tools/verify-xaml-connect.ps1` runs inside every harness build and fails it when the generated XAML
code has drifted.

**What a newcomer would actually need**, and therefore what the README has to promise: clone the repo,
run the fetch script to get upstream WebKit, run the patch script, run `arm-bootstrap.ps1`
(or the x64 equivalent), wait some hours. That is a normal open-source experience for an engine port,
and it is achievable from a few megabytes of repository.

## Order of work when this is taken on

1. Write `.gitignore` first — `build-*/`, `WebKit/`, `deps-build/`, `*.obj`, `*.lib`, `*.dll`, `*.appx`,
   `*.pdb`, `*.log`, `*.pfx`. Getting this wrong once and committing a build directory is unpleasant to
   undo.
2. List the debris in `Src/port` explicitly and remove it, separately from anything else, so the deletion
   is reviewable.
3. `git init`, commit `Src/`, `Doc/` and the root documents. Check the resulting size before pushing.
4. Extract the WebKit patch series and add the apply script.
5. Only then make the repository public, with a README that states the prerequisites honestly — one
   Windows SDK version, one MSVC toolset version, hours of build time, and a device for the ARM half.
