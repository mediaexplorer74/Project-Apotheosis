# ============================================================================
# install-build-tools.ps1 — Install minimal WebKit build toolchain
#
# Installs: LLVM/clang-cl, CMake, Ninja, Perl, Ruby, gperf
# Total: ~800 MB
#
# Usage: Run as Administrator
#   powershell -ExecutionPolicy Bypass -File install-build-tools.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Installing WebKit build toolchain" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# Check admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "!! Run as Administrator!" -ForegroundColor Red
    Write-Host "   Right-click PowerShell → Run as Administrator" -ForegroundColor Yellow
    exit 1
}

# Check choco
$choco = Get-Command choco -ErrorAction SilentlyContinue
if (-not $choco) {
    Write-Host "!! Chocolatey not found. Install from https://chocolatey.org/install" -ForegroundColor Red
    exit 1
}

function Install-IfMissing($name, $checkCmd) {
    $existing = Get-Command $checkCmd -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "[SKIP] $name (already installed: $($existing.Source))" -ForegroundColor Gray
        return
    }
    Write-Host "[INSTALL] $name ..." -ForegroundColor Green
    & choco install $name -y --no-progress 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  !! Failed to install $name" -ForegroundColor Red
    }
}

# 1. LLVM (provides clang-cl, lld-link, llvm-lib, etc.)
Write-Host "`n[1/6] LLVM/Clang ..." -ForegroundColor Yellow
Install-IfMissing "llvm" "clang-cl"

# 2. CMake
Write-Host "`n[2/6] CMake ..." -ForegroundColor Yellow
Install-IfMissing "cmake" "cmake"

# 3. Ninja
Write-Host "`n[3/6] Ninja ..." -ForegroundColor Yellow
Install-IfMissing "ninja" "ninja"

# 4. Strawberry Perl (needed for WebKit code generation)
Write-Host "`n[4/6] Strawberry Perl ..." -ForegroundColor Yellow
$perl = Get-Command perl -ErrorAction SilentlyContinue
if ($perl -and $perl.Source -like "*strawberry*") {
    Write-Host "[SKIP] Perl (Strawberry already installed)" -ForegroundColor Gray
} else {
    Install-IfMissing "strawberryperl" "perl"
}

# 5. Ruby (needed for offlineasm in JSC)
Write-Host "`n[5/6] Ruby ..." -ForegroundColor Yellow
Install-IfMissing "ruby" "ruby"

# 6. gperf (needed for hash table generation)
Write-Host "`n[6/6] gperf ..." -ForegroundColor Yellow
Install-IfMissing "gperf" "gperf"

# Verify
Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host " Verification" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$tools = @(
    @("clang-cl", "LLVM compiler"),
    @("lld-link", "LLVM linker"),
    @("llvm-lib", "LLVM static lib tool"),
    @("cmake", "Build system generator"),
    @("ninja", "Build executor"),
    @("perl", "Code generation scripts"),
    @("ruby", "Offline assembler"),
    @("gperf", "Hash table generator"),
    @("python", "Build scripts")
)

$allGood = true
foreach ($t in $tools) {
    $cmd = Get-Command $t[0] -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Host "[OK] $($t[1]) ($($t[0]))" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] $($t[1]) ($($t[0]))" -ForegroundColor Red
        $allGood = false
    }
}

Write-Host ""
if ($allGood) {
    Write-Host "All tools installed! Ready to build WebKit." -ForegroundColor Green
    Write-Host ""
    Write-Host "Next step:" -ForegroundColor Cyan
    Write-Host "  pwsh -File Src/port/configure-gpu-x64.ps1" -ForegroundColor White
    Write-Host "  (or configure-gpu-arm32.ps1 for ARM32)" -ForegroundColor White
} else {
    Write-Host "Some tools are missing. Install them manually." -ForegroundColor Yellow
}

# Refresh PATH
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")
