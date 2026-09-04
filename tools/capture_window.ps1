# Capture the Slate window to a PNG.
#
# Screen-grabbing the whole desktop catches whatever else is open; this finds
# the application's own window, brings it forward, and captures only its
# rectangle. Used to check UI work without asking the user for a screenshot.
#
#   .\tools\capture_window.ps1 -Out shot.png [-Args "samples\sample.dxf"]
#   .\tools\capture_window.ps1 -Out shot.png -Keys "m "   # type after opening
#   .\tools\capture_window.ps1 -Out shot.png -Click "708,1327"  # window-relative
#   .\tools\capture_window.ps1 -Out shot.png -Wheel 5 -WheelAt "1800,700"

param(
    [string]$Out = "window.png",
    [string]$Arguments = "",
    [string]$Keys = "",
    # Left-click at these window-relative pixels before capturing. Reaching a
    # layout tab or a panel needs a click; there is no keyboard route to them.
    [string]$Click = "",
    # Move the pointer here and leave it, without clicking — for checking
    # rollover behaviour, which needs a real pointer over a real window.
    [string]$HoverAt = "",
    # Wheel notches (positive zooms in), applied at -WheelAt or the centre.
    [int]$Wheel = 0,
    [string]$WheelAt = "",
    [int]$WaitSeconds = 10,
    # Seconds to wait after the keys before capturing, for anything on a timer
    # -- auto-save, a background load -- that has to be given time to happen.
    [int]$HoldSeconds = 0
)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern void mouse_event(
        uint flags, int dx, int dy, int data, UIntPtr extra);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, WHEEL = 0x0800;
}
"@

$exe = ".\build\RelWithDebInfo\bin\Slate.exe"

# This script ends by killing the process, which looks exactly like a crash and
# leaves recovery files behind. Without this the *next* run -- including one the
# person at the machine started themselves -- opens with a dialog offering to
# recover work that was only ever a screenshot.
$launchArgs = "--no-recovery-prompt"
if ($Arguments) { $launchArgs += " $Arguments" }
$proc = Start-Process $exe -ArgumentList $launchArgs -PassThru

Start-Sleep -Seconds $WaitSeconds
$proc.Refresh()
if ($proc.MainWindowHandle -eq 0) {
    Write-Host "No window appeared." -ForegroundColor Yellow
    $proc | Stop-Process -Force
    exit 1
}

# SW_MAXIMIZE, so the capture shows the layout at a realistic size rather than
# whatever the default geometry happens to be. Retried until the rectangle
# stops changing: a capture taken mid-maximise reports the old geometry, and
# then every -Click computed from it lands somewhere else entirely.
$r = New-Object Win32+RECT
$previous = ""
for ($attempt = 0; $attempt -lt 8; $attempt++) {
    [Win32]::ShowWindow($proc.MainWindowHandle, 3) | Out-Null
    [Win32]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 700
    [Win32]::GetWindowRect($proc.MainWindowHandle, [ref]$r) | Out-Null
    $current = "$($r.L),$($r.T),$($r.R),$($r.B)"
    if ($current -eq $previous) { break }
    $previous = $current
}

# Window-relative, and clamped to the window: a click computed for a different
# window size otherwise lands on the desktop or the taskbar, which silently
# raises some other application over the one being captured.
function Move-To([string]$spec) {
    $parts = $spec.Split(",")
    $x = [Math]::Min([Math]::Max([int]$parts[0], 0), $r.R - $r.L - 1)
    $y = [Math]::Min([Math]::Max([int]$parts[1], 0), $r.B - $r.T - 1)
    if ($x -ne [int]$parts[0] -or $y -ne [int]$parts[1]) {
        Write-Host "clamped ($($parts[0]),$($parts[1])) to ($x,$y)" -ForegroundColor Yellow
    }
    [System.Windows.Forms.Cursor]::Position =
        New-Object System.Drawing.Point ($r.L + $x), ($r.T + $y)
    Start-Sleep -Milliseconds 250
}

# Wheel before click: zooming is how you get to the thing worth clicking, and
# the wheel zooms about the cursor, so parking on the target keeps it there.
if ($Wheel -ne 0) {
    if ($WheelAt) { Move-To $WheelAt }
    else { Move-To "$([int](($r.R - $r.L) / 2)),$([int](($r.B - $r.T) / 2))" }
    for ($i = 0; $i -lt [Math]::Abs($Wheel); $i++) {
        [Win32]::mouse_event([Win32]::WHEEL, 0, 0, [Math]::Sign($Wheel) * 120, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 600
}

# Park the pointer without clicking, for rollover behaviour.
if ($HoverAt) {
    Move-To $HoverAt
    Start-Sleep -Milliseconds 600
}

if ($Click) {
    Move-To $Click
    [Win32]::mouse_event([Win32]::LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    [Win32]::mouse_event([Win32]::LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 900
}

if ($Keys) {
    # Park the pointer over the middle of the drawing area first. Several
    # things only happen when the canvas believes it has a cursor -- the
    # crosshair, snap markers, and the prompt that follows the cursor -- so a
    # capture taken without one is not showing the real interface.
    [System.Windows.Forms.Cursor]::Position =
        New-Object System.Drawing.Point (($r.L + $r.R) / 2), (($r.T + $r.B) / 2)
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
    Start-Sleep -Milliseconds 800
}

if ($HoldSeconds -gt 0) {
    Start-Sleep -Seconds $HoldSeconds
}

$w = $r.R - $r.L
$h = $r.B - $r.T

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

$proc | Stop-Process -Force
Write-Host "captured ${w}x${h} to $Out"
