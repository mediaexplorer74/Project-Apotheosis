# ============================================================================
# assert-target.ps1 — refuse to run one architecture's build step in a shell that
# was primed for the other.
#
# Why this exists. The ARM32 line works by hand-building INCLUDE / LIB / PATH:
# `arm32-uwp-env.ps1` rewrites all three *in the current PowerShell session*,
# because SDK 19041's ARM32 layout cannot be reached through vcvarsall. The x64
# build steps, in contrast, lift their include paths and flags out of
# `build-x64-gpu\build.ninja` and otherwise rely on whatever the session already
# has — their own headers say "Run after Src\setenv.ps1".
#
# Put those two facts together and there is exactly one way the two lines can
# poison each other, and it is not a shared file: dot-source the ARM environment,
# then run an x64 step in the same shell. `lld-link` reads %LIB%, so an x64 link
# would search `Lib\10.0.19041.0\um\arm` and could bind ARM32 import libraries;
# clang-cl reads %INCLUDE% the same way. The failure that follows is a pile of
# unresolved or malformed symbols with nothing in it pointing at the real cause.
#
# The reverse direction is already safe by construction: every `*-arm32.ps1`
# script dot-sources `arm32-uwp-env.ps1` itself and therefore overwrites the
# environment before it compiles anything. This guard closes the direction that
# was open.
#
# Contract: each architecture's env script stamps $env:APOTHEOSIS_TARGET, and each
# build step asserts the value it needs *before* touching the environment. An
# unset marker always passes — a clean shell, or one that has only run
# `Src\setenv.ps1`, is the normal starting state and must keep working. That is
# also why `setenv.ps1` deliberately does not stamp the marker: it is the shared
# session bootstrap, not an architecture choice.
#
# Usage, as the first executable line of an architecture-specific script:
#     & "$PSScriptRoot\assert-target.ps1" -Expect x64
# ============================================================================
param([Parameter(Mandatory)][ValidateSet('arm32', 'x64')][string]$Expect)

$have = $env:APOTHEOSIS_TARGET
if ($have -and $have -ne $Expect) {
    # $MyInvocation.PSCommandPath is the *calling* script, which is what we want to name -- but it is
    # null when this guard is invoked straight from a prompt, and Split-Path then throws. A guard whose
    # own error message crashes tells the user nothing, so fall back instead.
    $callerPath = $MyInvocation.PSCommandPath
    $caller = if ($callerPath) { Split-Path -Leaf $callerPath } else { 'console' }
    Write-Host ''
    Write-Host "  !! wrong build environment" -ForegroundColor Red
    Write-Host "     this shell is primed for $have, this step targets $Expect" -ForegroundColor Red
    Write-Host "     INCLUDE / LIB / PATH still point at the $have toolchain, so the" -ForegroundColor DarkYellow
    Write-Host "     result would silently mix architectures." -ForegroundColor DarkYellow
    Write-Host "     Fix: open a fresh PowerShell, run  . .\Src\setenv.ps1  and retry." -ForegroundColor DarkYellow
    Write-Host ''
    throw "APOTHEOSIS_TARGET=$have but $Expect was required (called from $caller)"
}
