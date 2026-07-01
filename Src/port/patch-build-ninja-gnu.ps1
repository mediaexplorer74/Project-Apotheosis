# patch-build-ninja-gnu.ps1 — Idempotent patching for GNU inline assembly files.
# After CMake re-gen: adds missing rules (CMake 4.0 workaround), GNU rules for
# LowLevelInterpreter.cpp / MacroAssemblerX86_64.cpp; converts flags/includes.

param([string]$BuildDir = "$env:APOTHEOSIS_ROOT\build-x64-gpu")

$ErrorActionPreference = 'Stop'
$rulesFile = Join-Path $BuildDir "CMakeFiles\rules.ninja"
$buildFile = Join-Path $BuildDir "build.ninja"
foreach ($f in $rulesFile, $buildFile) { if (-not (Test-Path $f)) { throw "Missing: $f" } }

$rulesContent = [System.IO.File]::ReadAllText($rulesFile)
$buildContent = [System.IO.File]::ReadAllText($buildFile)

# Find clang++.exe alongside clang-cl.exe
$clangClMatch = [regex]::Match($rulesContent, '([A-Z]:[^ =]*?\\clang-cl\.exe)')
if (-not $clangClMatch.Success) { throw "Cannot find clang-cl.exe" }
$clangxx = $clangClMatch.Groups[1].Value -replace 'clang-cl\.exe$', 'clang++.exe'
if (-not (Test-Path $clangxx)) { throw "clang++.exe not at $clangxx" }
$shortCxx = (New-Object -ComObject Scripting.FileSystemObject).GetFile($clangxx).ShortPath
$shortClangCl = (New-Object -ComObject Scripting.FileSystemObject).GetFile($clangClMatch.Groups[1].Value).ShortPath
Write-Host "clang++ (GNU driver): $shortCxx"

# ============================================================
# SECTION 1: PATCH rules.ninja — add missing rules
# ============================================================

# --- Remove any existing GNU rules (we'll re-add them) ---
$rulesContent = $rulesContent -replace '(?ms)^rule CXX_COMPILER__(?:LowLevelInterpreterLib|JavaScriptCore)_gnu_Release\r?\n(?:  .*\r?\n)*', ''

# --- Extract all unique rule names used in build.ninja ---
$buildRulePattern = [regex]::Matches($buildContent, '^build .+: ([a-zA-Z_0-9]+)', [System.Text.RegularExpressions.RegexOptions]::Multiline)
$usedRules = @{}
foreach ($m in $buildRulePattern) { $usedRules[$m.Groups[1].Value] = $true }

# --- Categorize missing rules ---
$missingCComp = @()   # C_COMPILER__*_unscanned_Release
$missingCxxComp = @() # CXX_COMPILER__*_unscanned_Release
$missingCxxGnu = @()  # CXX_COMPILER__*_gnu_Release
$missingCExeLink = @()  # C_EXECUTABLE_LINKER__*
$missingCxxExeLink = @() # CXX_EXECUTABLE_LINKER__*
$missingCxxDllLink = @() # CXX_SHARED_LIBRARY_LINKER__*
$missingCxxLibLink = @() # CXX_STATIC_LIBRARY_LINKER__*

foreach ($r in $usedRules.Keys | Sort-Object) {
    if ($rulesContent.IndexOf("rule $r") -ge 0) { continue }
    switch -wildcard ($r) {
        'C_COMPILER__*_unscanned_Release'   { $missingCComp += $r }
        'CXX_COMPILER__*_gnu_Release'       { $missingCxxGnu += $r }
        'CXX_COMPILER__*_unscanned_Release' { $missingCxxComp += $r }
        'C_EXECUTABLE_LINKER__*'            { $missingCExeLink += $r }
        'CXX_EXECUTABLE_LINKER__*'          { $missingCxxExeLink += $r }
        'CXX_SHARED_LIBRARY_LINKER__*'      { $missingCxxDllLink += $r }
        'CXX_STATIC_LIBRARY_LINKER__*'      { $missingCxxLibLink += $r }
        'RERUN_CMAKE'                       { $appended += "rule RERUN_CMAKE`r`n  command = `"C:\Program Files\CMake\bin\cmake.exe`" --regenerate-during-build -S`"C`$`:\Users\Admin\source\repos\!OpenCode\Apotheosis\WebKit`" -B`"C`$`:\Users\Admin\source\repos\!OpenCode\Apotheosis\build-x64-gpu`"`r`n  description = Re-running CMake...`r`n" }
    }
}

# --- Helpers to generate rule text blocks ---
function New-Rule_CComp($name) { return @"
rule $name
  deps = msvc
  command = `${LAUNCHER}${`$CODE_CHECK}$shortClangCl --target=x86_64-unknown-windows-msvc  /nologo `$DEFINES `$INCLUDES `$FLAGS /showIncludes /Fo`$out /Fd`$TARGET_COMPILE_PDB -c -- `$in
  description = Building C object `$out

"@ }
function New-Rule_CxxComp($name) { return @"
rule $name
  deps = msvc
  command = `${LAUNCHER}${`$CODE_CHECK}$shortClangCl --target=x86_64-unknown-windows-msvc  /nologo -TP `$DEFINES `$INCLUDES `$FLAGS /showIncludes /Fo`$out /Fd`$TARGET_COMPILE_PDB -c -- `$in
  description = Building CXX object `$out

"@ }
function New-Rule_GnuComp($name) { return @"
rule $name
  deps = gcc
  depfile = `$out.d
  command = `${LAUNCHER}${`$CODE_CHECK}$shortCxx --target=x86_64-unknown-windows-msvc  -x c++ `$DEFINES `$INCLUDES `$FLAGS -MD -MF `$out.d -o `$out -c -- `$in
  description = Building CXX object [`$out]

"@ }
function New-Rule_ExeLink($name) { return @"
rule $name
  command = C:\Windows\system32\cmd.exe /C "`$PRE_LINK && "C:\Program Files\CMake\bin\cmake.exe" -E vs_link_exe --msvc-ver=1944 --intdir=`$OBJECT_DIR --rc=rc --mt=C:\PROGRA~1\LLVM\bin\llvm-mt.exe --manifests `$MANIFESTS -- C:\PROGRA~1\LLVM\bin\lld-link.exe /nologo `$in  /out:`$TARGET_FILE /implib:`$TARGET_IMPLIB /pdb:`$TARGET_PDB /version:0.0 `$LINK_FLAGS `$LINK_PATH `$LINK_LIBRARIES && `$POST_BUILD"
  description = Linking CXX executable `$TARGET_FILE
  restat = `$RESTAT

"@ }
function New-Rule_DllLink($name) { return @"
rule $name
  command = C:\Windows\system32\cmd.exe /C "`$PRE_LINK && "C:\Program Files\CMake\bin\cmake.exe" -E vs_link_dll --msvc-ver=1944 --intdir=`$OBJECT_DIR --rc=rc --mt=C:\PROGRA~1\LLVM\bin\llvm-mt.exe --manifests `$MANIFESTS -- C:\PROGRA~1\LLVM\bin\lld-link.exe /nologo `$in  /out:`$TARGET_FILE /implib:`$TARGET_IMPLIB /pdb:`$TARGET_PDB /version:0.0 `$LINK_FLAGS `$LINK_PATH `$LINK_LIBRARIES && `$POST_BUILD"
  description = Linking CXX shared library `$TARGET_FILE
  restat = `$RESTAT

"@ }
function New-Rule_LibLink($name) { return @"
rule $name
  command = C:\PROGRA~1\LLVM\bin\llvm-ar.exe /nologo /out:`$out `$in
  description = Linking CXX static library `$out

"@ }

# --- Append all missing rules ---
$appended = @()
foreach ($r in $missingCComp)  { $appended += (New-Rule_CComp $r).TrimEnd() }
foreach ($r in $missingCxxComp) { $appended += (New-Rule_CxxComp $r).TrimEnd() }
foreach ($r in $missingCxxGnu)  { $appended += (New-Rule_GnuComp $r).TrimEnd() }
foreach ($r in $missingCExeLink) { $appended += (New-Rule_ExeLink $r).TrimEnd() }
foreach ($r in $missingCxxExeLink) { $appended += (New-Rule_ExeLink $r -replace 'vs_link_exe','vs_link_exe') }
foreach ($r in $missingCxxDllLink) { $appended += (New-Rule_DllLink $r).TrimEnd() }
foreach ($r in $missingCxxLibLink) { $appended += (New-Rule_LibLink $r).TrimEnd() }

# Ensure standard utility rules exist (CMake 4.0 Ninja generator often omits them)
$legalUtility = @{
    'CLEAN' = "rule CLEAN`r`n  command = cd .`r`n  description = Cleaning...`r`n"
    'HELP'  = "rule HELP`r`n  command = `"C:\Program Files\CMake\bin\cmake.exe`" -E echo `"Build targets:`" ; for /f %t in (`"ninja -t targets all`") do echo %t`r`n  description = Help...`r`n"
    'RERUN_CMAKE' = "rule RERUN_CMAKE`r`n  command = `"C:\Program Files\CMake\bin\cmake.exe`" --regenerate-during-build -S`"C`$`:\Users\Admin\source\repos\!OpenCode\Apotheosis\WebKit`" -B`"C`$`:\Users\Admin\source\repos\!OpenCode\Apotheosis\build-x64-gpu`"`r`n  description = Re-running CMake...`r`n"
}
foreach ($name in $legalUtility.Keys) {
    if ($rulesContent.IndexOf("rule $name") -lt 0) {
        $appended += $legalUtility[$name]
        Write-Host "Added utility rule $name"
    }
}

if ($appended.Count -gt 0) {
    $block = "`r`n`r`n# === Missing rules added by patch script ===`r`n"
    $block += ($appended -join "`r`n`r`n") + "`r`n"
    $rulesContent = $rulesContent.TrimEnd() + $block
    Write-Host "Added $($appended.Count) missing/extra rules"
}
Write-Host "Rules in rules.ninja: $(([regex]::Matches($rulesContent, '(?m)^rule ') | Measure-Object).Count)"

# ============================================================
# SECTION 1b: Insert GNU rules inline after unscanned rules
# ============================================================
$cxxMarker = 'description = Building CXX object $out'
$gnuTargets = @('LowLevelInterpreterLib', 'JavaScriptCore')
foreach ($t in $gnuTargets) {
    $unscannedName = "CXX_COMPILER__${t}_unscanned_Release"
    $gnuRuleName = "CXX_COMPILER__${t}_gnu_Release"
    if ($rulesContent.IndexOf("rule $gnuRuleName") -ge 0) { continue }  # already added above
    $idx = $rulesContent.IndexOf($unscannedName)
    if ($idx -lt 0) { Write-Host "WARNING: $unscannedName not found; GNU rule may be missing"; continue }
    $descIdx = $rulesContent.IndexOf($cxxMarker, $idx)
    if ($descIdx -lt 0) { Write-Host "WARNING: description not found after $unscannedName"; continue }
    $descEnd = $descIdx + $cxxMarker.Length
    $gnuText = (New-Rule_GnuComp $gnuRuleName).TrimEnd()
    $rulesContent = $rulesContent.Substring(0, $descEnd) + "`r`n`r`n" + $gnuText + $rulesContent.Substring($descEnd)
}

[System.IO.File]::WriteAllText($rulesFile, $rulesContent, [System.Text.UTF8Encoding]::new($false))

# ============================================================
# SECTION 2: PATCH build.ninja — GNU file flags/includes
# ============================================================

function Convert-ToGnuFlags($flags) {
    $f = $flags -replace '/D(\w+)', '-D$1'
    $f = $f -replace '/O[b]?\d*', '-O2'
    $f = $f -replace '/EHsc', ''
    $f = $f -replace '/MD', ''
    $f = $f -replace '/bigobj', ''
    $f = $f -replace '/wd\d+', ''
    $f = $f -replace '/Yu[^ ]*', ''
    $f = $f -replace '/Fp[^ ]*', ''
    $f = $f -replace '/FI[^ ]*', ''
    $f = $f -replace '-clang:(-std=c\+\+23)', '$1'
    $f = $f -replace '\s+', ' '
    $f = $f.Trim()
    if ($f -notmatch '-std=c\+\+23') { $f += ' -std=c++23' }
    if ($f -notmatch '-D_DLL') { $f += ' -D_DLL' }
    return $f
}

# --- LowLevelInterpreter.cpp ---
$lliMarker = "LowLevelInterpreter.cpp.obj: CXX_COMPILER__LowLevelInterpreterLib_unscanned_Release"
$buildContent = $buildContent.Replace($lliMarker, ($lliMarker -replace 'unscanned_Release', 'gnu_Release'))

$lliFlagsMatch = [regex]::Match($buildContent, "(?s)(LowLevelInterpreter\.cpp\.obj.*?FLAGS = )([^\r\n]+)")
if ($lliFlagsMatch.Success) {
    $prefix = $lliFlagsMatch.Groups[1].Value
    $newFlags = Convert-ToGnuFlags $lliFlagsMatch.Groups[2].Value
    $buildContent = $buildContent.Replace($prefix + $lliFlagsMatch.Groups[2].Value, $prefix + $newFlags)
    Write-Host "Converted LowLevelInterpreter flags"
}

$lliIncludesMatch = [regex]::Match($buildContent, "(?s)(LowLevelInterpreter\.cpp\.obj.*?INCLUDES = )([^\r\n]+)")
if ($lliIncludesMatch.Success) {
    $prefix = $lliIncludesMatch.Groups[1].Value
    $newIncludes = $lliIncludesMatch.Groups[2].Value -replace '-imsvc', '-isystem'
    if ($newIncludes -ne $lliIncludesMatch.Groups[2].Value) {
        $buildContent = $buildContent.Replace($prefix + $lliIncludesMatch.Groups[2].Value, $prefix + $newIncludes)
        Write-Host "Converted LowLevelInterpreter INCLUDES (imsvc→isystem)"
    }
}

# --- MacroAssemblerX86_64.cpp ---
$masmMarker = "MacroAssemblerX86_64.cpp.obj: CXX_COMPILER__JavaScriptCore_unscanned_Release"
$buildContent = $buildContent.Replace($masmMarker, ($masmMarker -replace 'unscanned_Release', 'gnu_Release'))

# Remove PCH deps from MacroAssemblerX86_64
$buildContent = $buildContent -replace '(?m)^(build .+MacroAssemblerX86_64\.cpp\.obj: CXX_COMPILER__JavaScriptCore_gnu_Release .+?) \| .+? \|\| (.+)', '$1 || $2'
if ($? -and $LASTEXITCODE -eq 0) { Write-Host "Removed PCH deps from MacroAssemblerX86_64" }

$masmFlagsMatch = [regex]::Match($buildContent, "(?s)(MacroAssemblerX86_64\.cpp\.obj.*?FLAGS = )([^\r\n]+)")
if ($masmFlagsMatch.Success) {
    $prefix = $masmFlagsMatch.Groups[1].Value
    $newFlags = Convert-ToGnuFlags $masmFlagsMatch.Groups[2].Value
    $buildContent = $buildContent.Replace($prefix + $masmFlagsMatch.Groups[2].Value, $prefix + $newFlags)
    Write-Host "Converted MacroAssemblerX86_64 flags"
}

$masmIncludesMatch = [regex]::Match($buildContent, "(?s)(MacroAssemblerX86_64\.cpp\.obj.*?INCLUDES = )([^\r\n]+)")
if ($masmIncludesMatch.Success) {
    $prefix = $masmIncludesMatch.Groups[1].Value
    $newIncludes = $masmIncludesMatch.Groups[2].Value -replace '-imsvc', '-isystem'
    if ($newIncludes -ne $masmIncludesMatch.Groups[2].Value) {
        $buildContent = $buildContent.Replace($prefix + $masmIncludesMatch.Groups[2].Value, $prefix + $newIncludes)
        Write-Host "Converted MacroAssemblerX86_64 INCLUDES (imsvc→isystem)"
    }
}

[System.IO.File]::WriteAllText($buildFile, $buildContent, [System.Text.UTF8Encoding]::new($false))
Write-Host "==> Patch complete"
