# drive-csharp.ps1 -- drive the installed C# Apotheosis app through the LocalState
# autocmd channel and capture a screenshot of the result.
#
# Why this exists: run-smoke.ps1 walks a fixed eleven-site list and is the MVP
# acceptance instrument. Iterating on one screen -- settings, a language change,
# one site -- needs the same three primitives (launch if needed, send a batch,
# screenshot) without the list. Everything here is measured the same way the rest
# of the project demands: the picture on screen and the files the app writes.
#
# Usage:
#   .\drive-csharp.ps1 -Cmds 'ui settings;wait 1500' -Shot out.png
#   .\drive-csharp.ps1 -Cmds 'goto https://example.com;wait 8000'
#   .\drive-csharp.ps1 -KeepState -Cmds 'hash'   # do not relaunch; just send cmds
#
# -Cmds is one string, commands separated by ';', because a PowerShell string[]
# parameter bound from a command line that a shell has already flattened is a
# trap: '-Cmds a,b' arrives as the single value 'a,b'.
#
# -FitWindow: the app's window opens larger than this bench's screen, so the address
# bar and the whole bottom chrome fall outside the captured bitmap. That made every
# screenshot of the chrome unverifiable, which is the reason this switch exists: it
# moves and resizes the live window into the screen's working area right before the
# capture. It changes no app code, only where the window sits.
param(
    [string]$Cmds = '',
    [string]$Shot = '',
    [string]$Url = '',          # if set, written to testurl.txt before launch
    [int]$BootMs = 9000,
    [int]$AfterCmdsMs = 7000,    # the batch runs on the engine thread; the shot must wait for it
    [switch]$KeepState,          # do not restart the app
    [switch]$NoLaunch,
    [switch]$FitWindow
)
$ErrorActionPreference = 'Continue'

# DPI awareness, and it has to come before anything asks for a window size.
#
# Without it this process is virtualized along with every other legacy process: WinForms
# reports a 1368x864 working area on a screen whose real surface is larger, SetWindowPos
# resizes the window using those same virtual units (so the window really becomes bigger),
# and CopyFromScreen then crops to the virtual rectangle. The result was a screenshot that
# looked like the app had lost its right edge and its whole bottom chrome -- a capture
# artefact, not a defect, and exactly the kind of thing that makes a measurement lie.
if (-not ('ApoDpi' -as [type])) {
    Add-Type -Namespace '' -Name ApoDpi -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
'@
    [void][ApoDpi]::SetProcessDPIAware()
}

$pkg = Get-AppxPackage -Name 'ME.Apotheosis' | Select-Object -First 1
if (-not $pkg) { throw 'ME.Apotheosis is not installed' }
$ls = Join-Path $env:LOCALAPPDATA ('Packages\' + $pkg.PackageFamilyName + '\LocalState')
if (-not (Test-Path $ls)) { New-Item -ItemType Directory -Force -Path $ls | Out-Null }
Write-Host "LocalState: $ls"

function Start-App {
    $app = ($pkg | Get-AppxPackageManifest).Package.Applications.Application | Select-Object -First 1
    Start-Process explorer.exe -ArgumentList "shell:AppsFolder\$($pkg.PackageFamilyName)!$($app.Id)"
}

function Stop-App {
    Get-Process -Name 'Apotheosis' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# Move the app's window into the screen's working area and report where it ended up, so the
# screenshot can be cropped to the window itself.
#
# Two measurements are behind this. First, Get-Process().MainWindowHandle is 0 for this app: a
# UWP window is not the classic top-level window that property looks for. Second, the window
# whose title is "Apotheosis" has class ApplicationFrameWindow and is owned by
# ApplicationFrameHost.exe, not by Apotheosis.exe, so matching on the app's pid finds nothing
# either. On this bench it also opened at 193,32 sized 1037x807, i.e. wider than it looks --
# which is why the first settings screenshots had their right edge (and the whole bottom
# chrome) outside the picture and could not be used as evidence.
function Fit-AppWindow {
    Add-Type -AssemblyName System.Windows.Forms
    if (-not ('Apo.WinPlace' -as [type])) {
        Add-Type -Namespace Apo -Name WinPlace -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
    }
    # The callback runs from native code, so it writes the SCRIPT scope: a local declared in
    # this function would be shadowed and the value written inside the callback would be lost
    # (the first version of this reported "no window" for exactly that reason).
    $script:apoHwnd = $null
    $cb = [Apo.WinPlace+EnumProc] {
        param($h, $l)
        if ([Apo.WinPlace]::IsWindowVisible($h)) {
            $t = New-Object System.Text.StringBuilder 512
            [void][Apo.WinPlace]::GetWindowText($h, $t, 512)
            # Set-AppVersion stamps the display name with the version ("Apotheosis 0.2.3.66"),
            # so an exact 'Apotheosis' match stopped finding the window at .65 and every -FitWindow
            # probe silently fell back to a full-screen capture. Prefix match against the class
            # check below, which is the part that actually identifies the frame.
            if ($t.ToString() -like 'Apotheosis*') {
                $c = New-Object System.Text.StringBuilder 512
                [void][Apo.WinPlace]::GetClassName($h, $c, 512)
                if ($c.ToString() -eq 'ApplicationFrameWindow') { $script:apoHwnd = $h }
            }
        }
        return $true
    }
    [void][Apo.WinPlace]::EnumWindows($cb, [IntPtr]::Zero)
    $found = $script:apoHwnd
    if (-not $found) { Write-Host 'FitWindow: no ApplicationFrameWindow titled Apotheosis'; return $null }
    $wa = [Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    # SWP_NOZORDER | SWP_NOACTIVATE
    [void][Apo.WinPlace]::SetWindowPos($found, [IntPtr]::Zero, $wa.X, $wa.Y, $wa.Width, $wa.Height, 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds 900
    Write-Host ("FitWindow: hwnd {0} -> {1}x{2} at {3},{4}" -f $found, $wa.Width, $wa.Height, $wa.X, $wa.Y)
    return $wa
}

if ($Url) { Set-Content (Join-Path $ls 'testurl.txt') -Value $Url -Encoding ASCII }
else { Remove-Item (Join-Path $ls 'testurl.txt') -ErrorAction SilentlyContinue }

if (-not $KeepState) {
    Stop-App
    # A leftover autocmd.txt is never replayed by the watcher (the stamp is seeded
    # at startup), and a leftover autocmd-log.txt would mix this run's lines with
    # the previous one's.
    Remove-Item (Join-Path $ls 'autocmd.txt') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $ls 'autocmd-log.txt') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $ls 'stage.txt') -ErrorAction SilentlyContinue
    if (-not $NoLaunch) {
        Start-App
        Start-Sleep -Milliseconds $BootMs
    }
}

if ($Cmds.Trim().Length -gt 0) {
    $lines = $Cmds -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }
    $cmdsPath = Join-Path $ls 'autocmd.txt'
    Set-Content $cmdsPath -Value $lines -Encoding UTF8
    Write-Host ("sent {0} cmds" -f $lines.Count)
    Start-Sleep -Milliseconds 1200
}

if ($Shot) {
    Start-Sleep -Milliseconds $AfterCmdsMs
    $rect = $null
    if ($FitWindow) { $rect = Fit-AppWindow }
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    if ($rect) { $b = $rect } else { $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds }
    $bmp = [Drawing.Bitmap]::new($b.Width, $b.Height)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
    $bmp.Save($Shot, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "screenshot: $Shot"
}
