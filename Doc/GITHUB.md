# Publishing to GitHub

This repository is published as **source + a small WebKit patch set**, never as a
WebKit checkout and never with build/native outputs. The normal first push is about
10 MiB on the 2026-10-23 tree, which is practical on a weak connection.

The target fork used by the updater is:

```
https://github.com/mediaexplorer74/Project-Apotheosis.git
```

Change that URL below if you publish to a different account or repository.

## 1. Before you create the repository

Run the audit from the repository root:

```powershell
pwsh -File Src\tools\audit-source-release.ps1
```

It must print `AUDIT PASSED`. It checks that:

- no `WebKit/`, `build-*`, `deps-build`, native payload, appx output or LocalState is tracked;
- no tracked DLL/LIB/EXE/PDB/OBJ/PFX/CER exists;
- `.gitignore` covers the generated trees;
- the WebKit patch has no `Tools/` paths and no binary hunks;
- no common GitHub token or private-key pattern is in tracked text.

It also prints the largest tracked files and the full-history bundle size. The
largest files are currently the two `simhei.ttf` copies (~9.3 MB each) and a few
screenshots; the object pack is ~10.19 MiB.

### The font decision

The ten `.ttf` files under the app/harness asset folders (two copies of five fonts) are tracked deliberately:
the AppContainer has no usable system fonts, and the app does not start without
them. The repository note claims redistribution rights from the MSDN/developer
program. If you are not certain that claim applies to you, remove the fonts and
expect the app to fail at startup; do not silently push them under a license you
cannot defend. This is a user decision, not something the build scripts can fix.

### Commit what you are publishing

The working tree contains the M1 and publication changes. Review and commit them
before pushing:

```powershell
git status --short
git diff --stat
git add .gitignore Doc Src WebKitPatches README.md
git commit -m "M1: persist cookies; publish source and WebKit patch set"
```

Do not use `git add -f WebKit` or `git add -f Src/Apotheosis/native`. If a file is
ignored, it is ignored for a reason; confirm with `git check-ignore -v <path>`.

## 2. Create the GitHub account and repository

If you do not have an account:

1. Open <https://github.com/signup>, create a free account, and verify the email.
2. Sign in and open <https://github.com/new>.
3. Repository name: `Project-Apotheosis`.
4. Choose Public or Private; both work on the free plan.
5. **Do not** initialize with a README, `.gitignore` or license. The local repository
   already has them; an initialized remote would require a merge on the first push.
6. Click **Create repository**.

GitHub shows an empty-repository page with a clone URL. Keep it open.

## 3. Authenticate without putting a token in the URL

The recommended path on Windows is Git Credential Manager, which is installed with
Git for Windows:

```powershell
git config --global credential.helper manager
```

The first push/`fetch` opens a browser once; sign in and authorize Git. The token is
stored by Windows Credential Manager, not in `.git/config` and not in the remote URL.

If you prefer GitHub CLI:

```powershell
winget install --id GitHub.cli -e
gh auth login
```

Choose HTTPS and "Login with a web browser". Never paste a PAT into the remote URL
and never commit `.git-credentials`.

## 4. Add the remote and push

From the repository root, confirm the branch name:

```powershell
git branch --show-current
# expected: master (if yours is main, use main below)
```

Add the fork and push:

```powershell
git remote add origin https://github.com/mediaexplorer74/Project-Apotheosis.git
git push -u origin master
```

If `origin` already exists:

```powershell
git remote set-url origin https://github.com/mediaexplorer74/Project-Apotheosis.git
git push -u origin master
```

The first push uploads the existing history. On a slow line:

- Let it run; do not close the terminal.
- If the connection drops, run `git push -u origin master` again. Git resumes the
  pack negotiation and does not duplicate the commits.
- If a proxy or antivirus interferes, try HTTP/1.1 once:

```powershell
git config --global http.version HTTP/1.1
git push -u origin master
```

- Do not enable `http.postBuffer` as a first fix; it does not help most failures and
  hides the real error.

## 5. Verify what arrived

Open the repository in the browser and check:

- `WebKitPatches/apotheosis-webkit-2.52.4.patch` exists and is ~288 KB.
- `WebKit/` does **not** exist.
- `build-x64-gpu/`, `build-arm32-gpu/`, `Src/Apotheosis/native/` and `AppPackages/`
  do **not** exist.
- The README shows the 0.2.3.73 status block.
- The commit count/history is plausible.

On the command line:

```powershell
git ls-remote --heads origin
git ls-tree -r --name-only origin/master | Select-String '^(WebKit/|build-|Src/Apotheosis/native/)'
```

The second command should print nothing.

## 6. If the normal push cannot finish

Only if the normal pack repeatedly fails after retries, publish a single-commit
branch instead of the 51-commit history. This is a fallback: it loses history and
should not be used if the normal push merely needs another attempt.

```powershell
# Make sure no ignored output is staged.
git status --short
pwsh -File Src\tools\audit-source-release.ps1
git checkout --orphan public-v1
git add -A
git commit -m "Apotheosis 0.2.3.73 source release"
git push -u origin public-v1:master
```

The orphan commit still contains the full source, patch, fonts and documentation,
but not the old history. Do not force-push an existing repository with unrelated
history unless you have explicitly decided to replace it.

## 7. After the push

- Keep the HTTPS remote and credential manager.
- Do not commit later build outputs; run the audit again before each release.
- The updater must target this fork (`mediaexplorer74/Project-Apotheosis`), not the
  upstream `Jimmyxiao2009/Project-Apotheosis` repository.
- If you publish a release asset, build the source ZIP with
  `pwsh -File Src\tools\audit-source-release.ps1 -ZipPath ...` and upload that; do
  not attach a WebKit tarball or an installed appx from the build tree.
- The older `Jimmyxiao2009/Project-Apotheosis` repository is source material, not a
  branch to merge blindly. Compare licences and file history before importing code.

## Free-account notes

- Public repositories are unlimited; private repositories on the free plan have
  generous limits that this ~10 MiB pack will not approach.
- GitHub does not accept files over 100 MB in a normal push. This tree has no such
  file; the 3.2 GB WebCore archive and 31 MB `icudt78.dll` are build outputs and are
  ignored.
- GitHub Actions is not required. The project's policy is local builds; do not add
  CI just to publish.
