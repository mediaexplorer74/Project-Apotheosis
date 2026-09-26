# Apotheosis: synthesise a one-finger drag over the running Harness window.
#
# Why this exists. The harness scrolls the engine by a real gesture: ContentArea has
# ManipulationMode="TranslateX,TranslateY,TranslateInertia,Scale" and OnImageManipDelta turns each
# ManipulationDelta into FreeScrollBy. A script cannot call that handler directly -- it is XAML
# plumbing, not a harness export -- so the only way to reproduce "I scrolled and it died" without a
# human finger is to make Windows deliver the input. This exists for the same reason LocalState\
# nav.txt does: a reproduction that needs a person is a reproduction you do not have when you need it.
#
# TOUCH, not mouse. The first version of this script injected a mouse drag and produced nothing at
# all -- measured 2026-09-18, zero ManipDelta lines, while the maintainer's own hand-drag on the same
# page in the same session produced them immediately and killed the browser. Two reasons to inject
# touch instead: it is the input class the target device actually produces (the Lumia has no mouse),
# and XAML's manipulation stack treats a touch contact as the first-class case. Mouse input is
# promoted to manipulation only along a special path, which is exactly where the synthetic drag
# disappeared.
#
# DPI: the process is made per-monitor-DPI-aware before any window is measured. An unaware process
# gets a virtualised GetWindowRect (measured: 1037x807 reported where the app logged a 1368x758
# viewport), so a drag computed from that rect is aimed at the wrong physical pixels -- which is a
# silent miss, not an error.
#
# Negative Dy drags the pointer UP the screen, which scrolls the page DOWN (content follows the
# finger). That is the direction the dzen.ru death was reported in.

param(
    [string]$ProcessName = 'Harness',
    [int]$Dx = 0,
    [int]$Dy = -640,
    [int]$Steps = 40,
    [int]$StepDelayMs = 12,
    [switch]$Mouse
)

$ErrorActionPreference = 'Stop'

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class ApoInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINTER_INFO {
        public uint pointerType;
        public uint pointerId;
        public uint frameId;
        public uint pointerFlags;
        public IntPtr sourceDevice;
        public IntPtr hwndTarget;
        public POINT ptPixelLocation;
        public POINT ptHimetricLocation;
        public POINT ptPixelLocationRaw;
        public POINT ptHimetricLocationRaw;
        public uint dwTime;
        public uint historyCount;
        public int InputData;
        public uint dwKeyStates;
        public ulong PerformanceCount;
        public int ButtonChangeType;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINTER_TOUCH_INFO {
        public POINTER_INFO pointerInfo;
        public uint touchFlags;
        public uint touchMask;
        public RECT rcContact;
        public RECT rcContactRaw;
        public uint orientation;
        public uint pressure;
    }

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }

    // INPUT is a union; MOUSEINPUT is its largest member on x64, so a mouse-only array marshals
    // correctly at the 40-byte stride SendInput expects.
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public MOUSEINPUT mi; }

    public const uint INPUT_MOUSE = 0;
    public const uint MOUSEEVENTF_MOVE = 0x0001;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;

    // SendInput, not mouse_event. The legacy call is a thin wrapper that modern input stacks are
    // free to ignore; measured 2026-09-18, a full press-move-release through mouse_event produced
    // zero ManipulationDelta lines while the maintainer's own mouse drag on the same page produced
    // them at once. SendInput goes in at the same level as the driver.
    public static void MouseAbs(int x, int y) {
        int sw = GetSystemMetrics(78), sh = GetSystemMetrics(79);   // SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN
        int ox = GetSystemMetrics(76), oy = GetSystemMetrics(77);   // SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN
        if (sw <= 1) sw = 1; if (sh <= 1) sh = 1;
        var arr = new INPUT[1];
        arr[0].type = INPUT_MOUSE;
        arr[0].mi.dx = (int)(((long)(x - ox) * 65535) / (sw - 1));
        arr[0].mi.dy = (int)(((long)(y - oy) * 65535) / (sh - 1));
        arr[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, arr, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void MouseButton(uint flag) {
        var arr = new INPUT[1];
        arr[0].type = INPUT_MOUSE;
        arr[0].mi.dwFlags = flag;
        SendInput(1, arr, Marshal.SizeOf(typeof(INPUT)));
    }
    [DllImport("user32.dll")] public static extern bool InitializeTouchInjection(uint maxCount, uint dwMode);
    [DllImport("user32.dll")] public static extern bool InjectTouchInput(uint count, POINTER_TOUCH_INFO[] contacts);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr, out RECT val, int size);

    // Physical screen pixels, always.
    //
    // GetWindowRect is virtualised for any process that is not per-monitor DPI aware, and
    // SetProcessDpiAwarenessContext CANNOT fix that once the host process already has a context --
    // it fails and leaves you unaware, which is how a maximised window over a 1824-pixel-tall
    // desktop got reported as 2748x1739. Measured 2026-09-18: SetCursorPos(400,300) and SendInput
    // both moved the real cursor correctly while the window rect stayed wrong, so the injection was
    // fine and every drag was simply aimed at the wrong pixels.
    //
    // Two fixes, both applied: SetThreadDpiAwarenessContext is per-thread and therefore works where
    // the process-wide call is a no-op, and DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)
    // returns physical coordinates regardless of DPI awareness.
    public static bool TryDwmRect(IntPtr h, out RECT r) {
        return DwmGetWindowAttribute(h, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, out r, Marshal.SizeOf(typeof(RECT))) == 0;
    }

    public const uint LEFTDOWN = 0x0002;
    public const uint LEFTUP   = 0x0004;

    public const uint TOUCH_FEEDBACK_NONE = 3;
    public const uint TOUCH_MASK_CONTACTAREA = 0x0001;
    public const uint TOUCH_MASK_PRESSURE    = 0x0004;

    public const uint FLAG_INRANGE  = 0x0002;
    public const uint FLAG_INCONTACT= 0x0004;
    public const uint FLAG_PRIMARY  = 0x000200;
    public const uint FLAG_DOWN     = 0x00010000;
    public const uint FLAG_UPDATE   = 0x00020000;
    public const uint FLAG_UP       = 0x00040000;
    public const uint PT_TOUCH      = 2;

    // A UWP app has no top-level window of its own: ApplicationFrameHost owns the frame and the app
    // owns a child of class Windows.UI.Core.CoreWindow. So the host window is found by scanning
    // children for one whose owning process is the app we are after. Get-Process MainWindowHandle
    // returns 0 for such a process, which is why this scan exists at all.
    public static IntPtr FindHostForPid(uint want) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr top, IntPtr lp) {
            EnumChildWindows(top, delegate(IntPtr child, IntPtr lp2) {
                var sb = new StringBuilder(256);
                GetClassName(child, sb, sb.Capacity);
                if (sb.ToString() != "Windows.UI.Core.CoreWindow") return true;
                uint pid; GetWindowThreadProcessId(child, out pid);
                if (pid == want) { found = top; return false; }
                return true;
            }, IntPtr.Zero);
            return found == IntPtr.Zero;
        }, IntPtr.Zero);
        return found;
    }

    // Granting this process ownership of the foreground is refused unless it currently owns it, so
    // share the input queue of whoever does -- the standard AttachThreadInput dance. Without this
    // SetForegroundWindow returns false, the harness never becomes the active window, and XAML
    // drops the injected contact on the floor.
    public static bool ForceForeground(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint fgThread = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, out uint _);
        uint me = GetCurrentThreadId();
        bool attached = false;
        if (fgThread != 0 && fgThread != me) attached = AttachThreadInput(fgThread, me, true);
        BringWindowToTop(h);
        bool ok = SetForegroundWindow(h);
        if (attached) AttachThreadInput(fgThread, me, false);
        return ok && GetForegroundWindow() == h;
    }

    // Reports why activation failed: SetForegroundWindow's own return value and whether the target
    // ended up foreground are two different answers, and the earlier version collapsed them into
    // one "False" that could not be told apart -- an already-foreground window and a refused
    // activation looked identical.
    public static string ForegroundReport(IntPtr h) {
        IntPtr before = GetForegroundWindow();
        uint fgThread = before == IntPtr.Zero ? 0 : GetWindowThreadProcessId(before, out uint _);
        uint me = GetCurrentThreadId();
        bool attached = false;
        if (fgThread != 0 && fgThread != me) attached = AttachThreadInput(fgThread, me, true);
        BringWindowToTop(h);
        bool ok = SetForegroundWindow(h);
        if (attached) AttachThreadInput(fgThread, me, false);
        IntPtr after = GetForegroundWindow();
        return string.Format("setForeground={0} wasForeground={1} isForeground={2} hwnd=0x{3:X} target=0x{4:X}",
            ok, before == h, after == h, (long)after, (long)h);
    }

    public static POINTER_TOUCH_INFO Contact(int x, int y, uint flags) {
        var c = new POINTER_TOUCH_INFO();
        c.pointerInfo.pointerType = PT_TOUCH;
        c.pointerInfo.pointerId = 1;
        c.pointerInfo.pointerFlags = flags;
        c.pointerInfo.ptPixelLocation.X = x;
        c.pointerInfo.ptPixelLocation.Y = y;
        c.pointerInfo.ptPixelLocationRaw = c.pointerInfo.ptPixelLocation;
        c.touchFlags = 0;
        c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
        c.rcContact.Left = x - 2; c.rcContact.Top = y - 2; c.rcContact.Right = x + 2; c.rcContact.Bottom = y + 2;
        c.rcContactRaw = c.rcContact;
        c.pressure = 320;
        return c;
    }

    public static bool Touch(uint flags, int x, int y) {
        var arr = new POINTER_TOUCH_INFO[1];
        arr[0] = Contact(x, y, flags);
        bool ok = InjectTouchInput(1, arr);
        if (!ok) LastError = Marshal.GetLastWin32Error();
        return ok;
    }
    public static int LastError;
}
'@
Add-Type -TypeDefinition $sig -ErrorAction SilentlyContinue

# Per-monitor-v2, before anything measures a window. -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
# The process-wide call is a no-op when the host already set a context (pwsh inherits one), so the
# per-thread call is what actually takes effect here.
[void][ApoInput]::SetProcessDpiAwarenessContext([IntPtr](-4))
[void][ApoInput]::SetThreadDpiAwarenessContext([IntPtr](-4))

$p = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$h = [ApoInput]::FindHostForPid([uint32]$p.Id)
if ($h -eq [IntPtr]::Zero) { throw "no host window for $ProcessName (pid $($p.Id)) -- is the app minimised?" }

# Activate FIRST, then measure. An inactive UWP window can be in a snapped or restored state that
# changes the moment it is activated -- measured 2026-09-18: the rect read before activation was
# 1037x807 at (172,33), and the app logged ApplyViewportSize 1368x758 immediately after, so a drag
# computed from the pre-activation rect flew off the top of the window and delivered nothing.
$fg = [ApoInput]::ForceForeground($h)
Write-Host ("foreground: " + [ApoInput]::ForegroundReport($h))
Start-Sleep -Milliseconds 900

$r = New-Object ApoInput+RECT
$dwm = New-Object ApoInput+RECT
if ([ApoInput]::TryDwmRect($h, [ref]$dwm)) {
    $r = $dwm                      # physical pixels, DPI-awareness independent
} elseif (-not [ApoInput]::GetWindowRect($h, [ref]$r)) { throw "GetWindowRect failed" }
$w = $r.Right - $r.Left
$hgt = $r.Bottom - $r.Top
Write-Host "window $($r.Left),$($r.Top) ${w}x${hgt} (pid $($p.Id))"

# Stay inside the window. The harness toolbar occupies roughly the top 110 px of the frame; the
# gesture is aimed at the content area below it with a margin at each end.
$top = $r.Top + 140
$bottom = $r.Bottom - 40
$usable = $bottom - $top
if ($usable -lt 200) { throw "content area only $usable px tall -- window too small to drag in" }

$dy = [Math]::Max($Dy, -($usable - 40))
$y0 = $bottom - 60
$y1 = $y0 + $dy
$x0 = $r.Left + [int]($w / 2)
$x1 = $x0 + $Dx
Write-Host "drag ($x0,$y0) -> ($x1,$y1)"

if ($Mouse) {
    Write-Host "mode: mouse drag (SendInput)"
    [ApoInput]::MouseAbs($x0, $y0)
    Start-Sleep -Milliseconds 200
    [ApoInput]::MouseButton([ApoInput]::MOUSEEVENTF_LEFTDOWN)
    Start-Sleep -Milliseconds 40
    for ($i = 1; $i -le $Steps; $i++) {
        [ApoInput]::MouseAbs($x0 + [int](($x1 - $x0) * $i / $Steps), $y0 + [int](($y1 - $y0) * $i / $Steps))
        Start-Sleep -Milliseconds $StepDelayMs
    }
    Start-Sleep -Milliseconds 60
    [ApoInput]::MouseButton([ApoInput]::MOUSEEVENTF_LEFTUP)
} else {
    Write-Host "mode: touch injection"
    if (-not [ApoInput]::InitializeTouchInjection(1, [ApoInput]::TOUCH_FEEDBACK_NONE)) {
        throw "InitializeTouchInjection failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }
    $down = [ApoInput]::FLAG_DOWN -bor [ApoInput]::FLAG_INRANGE -bor [ApoInput]::FLAG_INCONTACT -bor [ApoInput]::FLAG_PRIMARY
    if (-not [ApoInput]::Touch($down, $x0, $y0)) { throw "touch DOWN failed err=$([ApoInput]::LastError)" }
    Start-Sleep -Milliseconds 40
    for ($i = 1; $i -le $Steps; $i++) {
        $x = $x0 + [int](($x1 - $x0) * $i / $Steps)
        $y = $y0 + [int](($y1 - $y0) * $i / $Steps)
        if (-not [ApoInput]::Touch(([ApoInput]::FLAG_UPDATE -bor [ApoInput]::FLAG_INRANGE -bor [ApoInput]::FLAG_INCONTACT), $x, $y)) {
            Write-Host "touch UPDATE $i failed err=$([ApoInput]::LastError)"; break
        }
        Start-Sleep -Milliseconds $StepDelayMs
    }
    Start-Sleep -Milliseconds 30
    [void][ApoInput]::Touch([ApoInput]::FLAG_UP, $x1, $y1)
}

Write-Host "drag done in $Steps steps"
