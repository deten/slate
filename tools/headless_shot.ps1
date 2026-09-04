# Drive the application and photograph it without a window appearing.
#
# capture_window.ps1 has to raise the real window and send it real keystrokes,
# which takes the screen and the keyboard away from whoever is using the
# machine. This does neither: Qt's offscreen platform renders into memory and
# QWidget::grab writes the result straight to a PNG.
#
#   .\tools\headless_shot.ps1 -Out shot.png -Run "line","0,0","500,300","","ze"
#   .\tools\headless_shot.ps1 -Out shot.png -File "drawing.dxf"
#
# Use capture_window.ps1 only when the thing being checked genuinely needs a
# real window: focus behaviour, native dialogs, the pointer.

param(
    [string]$Out = "headless.png",
    [string[]]$Run = @(),
    [string]$File = "",
    # Window coordinates to move the pointer through before capturing, for
    # checking rollover highlighting and snap markers. Applied in order, since
    # extension snapping is acquired by hovering an object and moving away.
    [string[]]$Hover = @(),
    # Canvas clicks, for selecting objects and dragging grips.
    [string[]]$Click = @(),
    # Presses of Shift+Space, to step through overlapping objects.
    [int]$Cycle = 0,
    [int]$TimeoutSeconds = 60
)

$exe = ".\build\RelWithDebInfo\bin\Slate.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Build first: $exe not found"
    exit 1
}

# The offscreen platform has no font database of its own.
$env:QT_QPA_FONTDIR = "C:\Windows\Fonts"

$quoted = @("-platform", "offscreen")
if ($File) { $quoted += '"' + (Resolve-Path $File).Path + '"' }
foreach ($line in $Run) {
    # Quoted individually so an empty entry survives as an empty argument --
    # that is Enter on a blank command line, which ends a command.
    $quoted += "--run"
    $quoted += '"' + $line + '"'
}
foreach ($spot in $Hover) {
    $quoted += "--hover"
    $quoted += '"' + $spot + '"'
}
foreach ($spot in $Click) {
    $quoted += "--click"
    $quoted += '"' + $spot + '"'
}
if ($Cycle -gt 0) {
    $quoted += "--cycle"
    $quoted += "$Cycle"
}
$quoted += "--screenshot"
$quoted += '"' + $Out + '"'

Remove-Item $Out -ErrorAction SilentlyContinue
$process = Start-Process $exe -ArgumentList ($quoted -join " ") -PassThru -NoNewWindow
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "timed out after ${TimeoutSeconds}s" -ForegroundColor Yellow
    Stop-Process -Id $process.Id -Force
    Remove-Item Env:\QT_QPA_FONTDIR -ErrorAction SilentlyContinue
    exit 1
}
Remove-Item Env:\QT_QPA_FONTDIR -ErrorAction SilentlyContinue

if (Test-Path $Out) {
    Write-Host "captured $((Get-Item $Out).Length) bytes to $Out"
} else {
    Write-Error "no screenshot produced"
    exit 1
}
