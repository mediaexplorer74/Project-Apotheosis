# Publishing to GitHub

The repository currently has no reliable local commit history, so the first publication should be a
reviewable checkpoint rather than a force push.

## Before publishing

1. Keep the external backup.
2. Review `git status` and the staged file list. Do not stage `WebKit/`, build trees, appx output,
   `.pfx` files, LocalState, or downloaded binaries.
3. Confirm the version, README, LICENSE, and target architecture notes.
4. Compare the current tree with the separately cloned `Project-Apotheosis` repository before importing
   anything from it.

## Safest first publication

Use Visual Studio 2022 if that is familiar: open `Apotheosis.sln`, use **Team > Manage Remotes** to
inspect/add the GitHub remote, then use **Publish Branch** from Source Control. Before publishing, make
a local commit with a clear message such as `MVP: ascetic browser baseline`.

If GitHub authentication fails, do not change the remote URL or delete `.git`. First check whether the
GitHub account is signed in through VS, Git Credential Manager, or a browser. A personal access token
should be created only if required, scoped to the repository, and never pasted into a remote URL or
committed.

## Suggested remote

`https://github.com/mediaexplorer74/Project-Apotheosis.git`

Use HTTPS + credential manager, not a token embedded in the URL. The default branch should be `main`
only after the old repository history has been compared; otherwise preserve the existing history and
publish a new branch first.

The older repository must be treated as source material, not silently merged. Compare file lists,
licence, history, and actual code before copying anything.
