# ============================================================================
# verify-xaml-connect.ps1 -- fail the build when MainPage.g.hpp has drifted from
# the markup, instead of shipping an appx that dies on launch.
#
# The bug this exists to prevent, from 2026-08-17: MainPage.xaml.cpp carried a
# hand-pasted copy of Generated Files\MainPage.g.hpp, including a ~490-line
# Connect() switch with hardcoded connection ids. Adding one page to the markup
# renumbered those ids. DiagPage took id 7, which had been TabSwitcher -- also a
# Grid, so safe_cast accepted it without a word -- and ContentArea moved from 76
# to 88, an id the frozen switch had no case for. LoadComponent then built the
# whole tree, bound fields to the wrong controls, and left ContentArea null. In
# C++/CX a null ^ handle is a raw dereference, so `ContentArea->Children` was an
# access violation that catch (...) cannot catch under /EHsc: four builds in a row
# died at startup with an empty LocalState and no WER report. It cost a day.
#
# MainPage.g.hpp is now included rather than copied, which fixes that -- but the
# file is still a build *input* that the VS 2022 XAML compiler does not regenerate
# (it emits only .g.h; MarkupCompilePass2 dies with WMC9999). So the same drift can
# return the moment someone edits the markup. This check is the guard rail.
#
# What it checks, and why these two invariants are enough:
#   1. Every x:Name field declared in the freshly generated MainPage.g.h must be
#      assigned in Connect(), in the same relative order. The generated header
#      lists fields in the compiler's own connection order, not document order
#      (verified: the two orders differ, and .g.h matches .g.hpp exactly), so this
#      catches both a missing case and a reordering. The 0.1.8.64 failure shows up
#      here as ContentArea declared but never assigned.
#   2. The number of case labels in Connect() must equal the number of connectable
#      elements in the markup: an element gets a connection id when it has an
#      x:Name or an event handler. This catches a new handler on an unnamed
#      element -- the menu rows are <Button Tag="..." Click="OnAction"> with no
#      x:Name, so invariant 1 alone would miss them.
#
# It does NOT try to recompute the ids themselves. The compiler's numbering is not
# document order: it groups the Collapsed overlay grids first, then walks their
# children in reverse order of the parents, and ContentArea -- third in the file --
# ends up at 89. Reproducing that from guesswork is how you get a wrong id that
# type-checks, which is precisely the failure above. See Doc/UNIFICATION.md C1.
#
# Usage:  pwsh -File Src\tools\verify-xaml-connect.ps1
# Exit:   0 in sync, 1 drifted (with a report on stdout).
# ============================================================================
[CmdletBinding()]
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$Root = if ($env:APOTHEOSIS_ROOT) { $env:APOTHEOSIS_ROOT } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$H    = Join-Path $Root 'Src\harness'
$gh   = Join-Path $H 'Generated Files\MainPage.g.h'
$ghpp = Join-Path $H 'Generated Files\MainPage.g.hpp'
$xaml = Join-Path $H 'MainPage.xaml'

function Die([string[]]$lines) {
    Write-Host ''
    Write-Host '  !! MainPage.g.hpp is out of sync with MainPage.xaml' -ForegroundColor Red
    foreach ($l in $lines) { Write-Host "     $l" -ForegroundColor Yellow }
    Write-Host ''
    Write-Host '     A stale Connect() binds x:Name fields to the wrong controls or leaves them' -ForegroundColor DarkYellow
    Write-Host '     null, and a null ^ handle in C++/CX is an access violation that catch (...)' -ForegroundColor DarkYellow
    Write-Host '     cannot catch. The appx would build and then die at startup with no log.' -ForegroundColor DarkYellow
    Write-Host '     Recovery is in CLAUDE.md and Doc/UNIFICATION.md section C1.' -ForegroundColor DarkYellow
    Write-Host ''
    exit 1
}

if (-not (Test-Path $xaml)) { Die @("$xaml does not exist.") }
if (-not (Test-Path $ghpp)) {
    # A missing .g.hpp is the /t:Rebuild footgun: Clean deletes it and the compiler does
    # not write it back. Name that specifically -- it is not the same problem as a build
    # that simply has not generated anything yet.
    Die @("$ghpp does not exist.",
          'This is what /t:Rebuild does: Clean deletes it and the XAML compiler does',
          'not regenerate it. Restore it from MainPage.g.hpp.backup in the same',
          'directory, then re-run this check before trusting the result.')
}
if (-not (Test-Path $gh)) {
    # .g.h is produced by MarkupCompilePass1. If it is absent the XAML step has not run
    # yet, so there is nothing to compare against -- and the compile that follows would
    # fail on its own, since MainPage.xaml.cpp includes it. Skip rather than cry wolf.
    Write-Host "xaml/Connect check skipped: $gh not generated yet" -ForegroundColor DarkYellow
    exit 0
}

# --- 1. declared fields, in generated order ---
$declared = @(Get-Content $gh | ForEach-Object {
    if ($_ -match '\^\s+([A-Za-z_][A-Za-z0-9_]*)\s*;') { $matches[1] }
})

# --- assigned fields, in case order ---
$assigned = @(Get-Content $ghpp | ForEach-Object {
    if ($_ -match 'this->([A-Za-z_][A-Za-z0-9_]*)\s*=\s*safe_cast') { $matches[1] }
})

$cases = @(Get-Content $ghpp | Where-Object { $_ -match '^\s*case\s+\d+\s*:' }).Count

$missing = @($declared | Where-Object { $assigned -notcontains $_ })
$extra   = @($assigned | Where-Object { $declared -notcontains $_ })
if ($missing.Count -or $extra.Count) {
    $msg = @()
    if ($missing.Count) { $msg += ("declared in MainPage.g.h but never assigned in Connect(): " + ($missing -join ', ')) }
    if ($extra.Count)   { $msg += ("assigned in Connect() but not declared any more: " + ($extra -join ', ')) }
    $msg += "declared=$($declared.Count) assigned=$($assigned.Count)"
    Die $msg
}
if ($declared.Count -ne $assigned.Count) {
    Die @("field counts differ: declared=$($declared.Count) assigned=$($assigned.Count)")
}
for ($i = 0; $i -lt $declared.Count; $i++) {
    if ($declared[$i] -ne $assigned[$i]) {
        Die @("order diverges at position $($i + 1): MainPage.g.h says '$($declared[$i])', Connect() says '$($assigned[$i])'.",
              'Connection ids are handed out in the compiler''s own order, and the generated',
              'header lists fields in that same order, so a divergence here means the ids in',
              'Connect() no longer match the ones baked into MainPage.xbf.')
    }
}

# --- 2. connectable elements in the markup vs case labels ---
$xn = 'http://schemas.microsoft.com/winfx/2006/xaml'
$doc = New-Object System.Xml.XmlDocument
$doc.Load($xaml)
$script:connectable = 0
function Walk([System.Xml.XmlNode]$n) {
    if ($n.NodeType -ne [System.Xml.XmlNodeType]::Element) { return }
    $named = [bool]$n.GetAttribute('Name', $xn)
    $handler = $false
    foreach ($a in $n.Attributes) {
        # Every handler in this markup is a method named On*, which is a far more
        # reliable test than a hardcoded list of event names that would silently
        # miss the next event someone uses.
        if ($a.NamespaceURI -ne $xn -and $a.Value -match '^On[A-Za-z0-9_]*$') { $handler = $true }
    }
    if ($named -or $handler) { $script:connectable++ }
    foreach ($c in $n.ChildNodes) { Walk $c }
}
# The root Page reserves connection id 1 and gets no case label of its own -- the first
# case in the generated switch is 2, which is RootGrid. So walk its children, not it.
foreach ($c in $doc.DocumentElement.ChildNodes) { Walk $c }

if ($cases -ne $script:connectable) {
    Die @("the markup has $($script:connectable) connectable elements below the root (x:Name or an On* handler),",
          "so Connect() needs $($script:connectable) case labels, but it has $cases.",
          'Something was added to or removed from MainPage.xaml without MainPage.g.hpp',
          'being regenerated. Adding an x:Name or a handler renumbers every id after it.')
}

# --- 3. handler attachments, per event name ---
# Invariants 1 and 2 both miss one case: adding a handler to an element that already
# has an x:Name changes neither the field set nor the number of ids, but it does need
# a new += line inside that element's case. Comparing per-event histograms catches
# that, and also catches a handler whose event was changed (Click to Tapped) or whose
# element lost its handler in the markup while Connect() still wires it up.
$markupEvents = @{}
foreach ($m in ([regex]::Matches((Get-Content $xaml -Raw), '(?<ev>[A-Za-z]+)="On[A-Za-z0-9_]*"'))) {
    $e = $m.Groups['ev'].Value
    $markupEvents[$e] = 1 + ([int]$markupEvents[$e])
}
$connectEvents = @{}
foreach ($m in ([regex]::Matches((Get-Content $ghpp -Raw), '->(?<ev>[A-Za-z]+) \+= ref new'))) {
    $e = $m.Groups['ev'].Value
    $connectEvents[$e] = 1 + ([int]$connectEvents[$e])
}
$evProblems = @()
foreach ($e in (@($markupEvents.Keys) + @($connectEvents.Keys) | Sort-Object -Unique)) {
    $a = [int]$markupEvents[$e]
    $b = [int]$connectEvents[$e]
    if ($a -ne $b) { $evProblems += "  $e : markup has $a, Connect() wires $b" }
}
if ($evProblems.Count) {
    Die (@('handler attachments disagree between the markup and Connect():') + $evProblems +
         @('A handler declared in XAML but not wired in Connect() simply never fires, which',
           'looks like a dead control rather than a build problem.'))
}

$totalEvents = ($markupEvents.Values | Measure-Object -Sum).Sum

if (-not $Quiet) {
    Write-Host ("xaml/Connect in sync: {0} fields, {1} case labels, {2} connectable elements, {3} handler attachments" -f `
        $declared.Count, $cases, $script:connectable, $totalEvents) -ForegroundColor Green
}
exit 0
