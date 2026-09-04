# Photograph the application doing the things it claims to do.
#
# Tests say a feature works. They cannot say it *looks* right, and every visual
# defect found in this project so far -- a formatting bar with no panel behind
# it, buttons shorter than their own labels, a font substituted into the wrong
# alphabet, headers reading "Proper..." -- passed its tests while looking
# broken. So this boots the real application, drives it through a list of
# scenes, and leaves a folder of PNGs to look at.
#
#   .\tools\visual_sweep.ps1                     # every scene
#   .\tools\visual_sweep.ps1 -Only text          # scenes matching "text"
#   .\tools\visual_sweep.ps1 -List               # names only, run nothing
#
# Add a scene whenever something visible is built. A scene that is never
# photographed is a scene nobody is checking.
#
# Every pointer position here is a point in the *drawing*, written `@x,y`, not
# a pixel on the canvas. Pixels were what these used when the chrome above the
# drawing was three rows tall; merging it into one moved the canvas up and made
# it taller, and the clicks silently began landing on empty grid -- one scene
# selected nothing and photographed a prompt, another zoomed to a blank patch
# between the sheets. A point in the drawing means the same thing whatever the
# window does to it, and it also says what the scene is aiming at.

param(
    [string]$Out = "",
    # Substring filter on scene names.
    [string]$Only = "",
    [switch]$List,
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\RelWithDebInfo\bin\Slate.exe"

# A drawing large enough that zooming to its extents gives an ordinary working
# view, rather than one object filling the screen. Scenes that need geometry
# build it here so the sweep depends on no file that might not be present.
$scaffold = @(
    "--run", "RECTANGLE", "--run", "0,0", "--run", "400,260",
    "--run", "CIRCLE", "--run", "300,180", "--run", "70",
    "--run", "LINE", "--run", "0,0", "--run", "400,260", "--run", "",
    "--run", "MTEXT", "--run", "40,60", "--run", "260,110", "--run", "18",
    "--run", "390.58",
    "--run", "ZOOMEXTENTS"
)

$scenes = [ordered]@{
    # The application as it is first seen. Ribbon, panels, status bar, canvas.
    "01-startup"       = @()
    # Geometry at an ordinary zoom -- line weights, grid, colours.
    "02-drawing"       = $scaffold
    # An object selected: grips, highlight, properties panel, contextual tab.
    # Two objects picked one after the other, with no modifier held: the
    # other CAD software rule, where picks accumulate. The count in the status bar is the
    # thing to read. The coordinates have to land *on* the geometry -- the
    # pickbox is a few pixels, and an earlier version of this scene missed the
    # rectangle by nine and photographed an empty selection for weeks.
    "03-selection"     = $scaffold + @("--click", "@200,260", "--click", "@300,110")
    # The in-place text editor and its formatting bar.
    "04-text-edit"     = $scaffold + @("--double-click", "@70,95")
    # ...with a selection formatted, which is what the bar is for.
    "05-text-bold"     = $scaffold + @("--double-click", "@70,95", "--key", "Ctrl+B")
    # Every ribbon tab, because a tool in the wrong group is invisible to tests.
    "06-tab-home"      = @("--tab", "Home")
    "07-tab-insert"    = @("--tab", "Insert")
    "08-tab-annotate"  = @("--tab", "Annotate")
    "09-tab-view"      = @("--tab", "View")
    "09a-tab-draw"     = @("--tab", "Draw")
    "09b-tab-modify"   = @("--tab", "Modify")
    "09c-tab-tools"    = @("--tab", "Tools")
    # The Home tab at the width a drafter actually has, and at a narrow one.
    # It is the widest tab and the first to run out of window, so both cases
    # are worth looking at: the first should simply fit, and the second should
    # scroll rather than cut tools off at the frame.
    "09d-home-wide"    = @("--size", "1920x1080", "--tab", "Home")
    "09e-home-narrow"  = @("--size", "1280x800", "--tab", "Home")
    # Two drawings open at once. The scaffold draws into the untitled drawing,
    # which makes it real work rather than a placeholder, so opening a file
    # beside it gives two tabs rather than replacing it.
    "10a-two-files"    = $scaffold + @("--open", "samples\sample.dxf")
    # A grip held by a click, with the readout that says what the edit
    # measures. Two clicks and a move, no button held down.
    # Its own rectangle rather than the shared scaffold, because the point of
    # this scene is a grip at a known place: one shape, drawn to fill the
    # window, so the coordinates mean the same thing every run.
    "10b-grip-edit"    = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,260",
                           "--run", "ZOOMEXTENTS", "--click", "@200,260",
                           "--click", "@400,260", "--hover", "@470,180")
    # The same grip, two presses of the space bar later: a different mode, a
    # different prompt, and a preview that turns rather than stretches.
    # About the *bottom-left* grip, and by a small angle. Held at the top-right
    # corner the ghost swung off the top of the canvas, so the scene showed a
    # readout and no result -- which is the half worth checking.
    "10c-grip-rotate"  = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,260",
                           "--run", "ZOOMEXTENTS", "--click", "@200,260",
                           "--click", "@0,0", "--hover", "@120,45",
                           "--key", "Space", "--key", "Space", "--hover", "@120,45")
    # A MOVE part-way through, showing the ghost of where the selection lands.
    # Its own geometry, arranged so the ghost falls clear of the original -- the
    # whole point of the scene is seeing two shapes, not one.
    "10d-move-ghost"   = @("--run", "RECTANGLE", "--run", "0,0", "--run", "200,120",
                           "--run", "LINE", "--run", "600,400", "--run", "700,450",
                           "--run", "", "--run", "ZOOMEXTENTS", "--click", "@100,0",
                           "--run", "MOVE", "--run", "0,0", "--hover", "@430,290")
    # Several grips marked and stretched as one. Two lines, both picked, both
    # far ends ringed by a Shift-click and one of them then dragged: the picture
    # is worth taking because the thing to check is that *both* ghosts move, and
    # by the same amount.
    #
    # Both ends have to be marked. Marking one and plain-clicking the other
    # makes the second the only hot grip, and the scene then photographs a
    # single ghost -- which is the picture of the feature not working.
    "10e-hot-grips"    = @("--run", "LINE", "--run", "0,0", "--run", "400,0", "--run", "",
                           "--run", "LINE", "--run", "0,100", "--run", "400,100", "--run", "",
                           "--run", "ZOOMEXTENTS", "--click", "@200,100", "--click", "@200,0",
                           "--shift-click", "@400,100", "--shift-click", "@400,0",
                           "--click", "@400,0", "--hover", "@400,50")
    # A polyline segment's midpoint grip mid-drag, which adds a corner where
    # the pointer is. Worth a picture because the grips themselves are the
    # thing to check: one per vertex *and* one per segment.
    "10f-poly-vertex"  = @("--run", "PLINE", "--run", "0,0", "--run", "400,0",
                           "--run", "400,300", "--run", "", "--run", "ZOOMEXTENTS",
                           "--click", "@200,0", "--click", "@200,0",
                           "--hover", "@200,80")
    # PLINE in arc mode, mid-draw. The preview has to be the arc itself rather
    # than the chord across it, and it has to leave the straight run tangent --
    # a kink at the join is the whole thing arc mode exists to avoid.
    "10g-pline-arc"    = @("--run", "PLINE", "--run", "-500,-250", "--run", "0,-250",
                           "--run", "A", "--hover", "@250,100")
    # Object snap tracking, holding a point at the crossing of two alignment
    # paths -- level with a rectangle's corner and directly above a circle's
    # centre, where there is no geometry at all. Both paths and both acquired
    # points have to be on screen: the paths say which way, and the crosses say
    # which points they were measured from.
    "10h-snap-tracking" = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
                            "--run", "CIRCLE", "--run", "600,100", "--run", "50",
                            "--run", "ZOOMEXTENTS", "--click", "@200,0", "--key", "F11",
                            "--run", "LINE", "--hover", "@400,300", "--hover", "@600,100",
                            "--hover", "@600,300")
    # Dimensional input: the boxes that take an exact size. Two scenes, because
    # the states are the point -- one field taking keystrokes, and one locked by
    # Tab with the other now listening. If those two look alike the feature is
    # unusable, and only a picture says whether they do.
    "10i-dim-typing"   = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
                           "--run", "ZOOMEXTENTS", "--click", "@200,0",
                           "--run", "LINE", "--run", "50,50", "--hover", "@300,200",
                           "--type", "250")
    "10j-dim-locked"   = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
                           "--run", "ZOOMEXTENTS", "--click", "@200,0",
                           "--run", "LINE", "--run", "50,50", "--hover", "@300,200",
                           "--type", "250", "--key", "tab", "--type", "30")
    # Two ellipses, drawn both ways round: axis endpoints then half the other
    # axis, and from the centre. Worth photographing because the outline is the
    # test -- a tessellation that under-samples reads as a faceted blob and no
    # count of segments in a log would say so.
    "10k-ellipse"      = @("--run", "ELLIPSE", "--run", "0,0", "--run", "400,0", "--run", "80",
                           "--run", "ELLIPSE", "--run", "C", "--run", "200,-300",
                           "--run", "200,-180", "--run", "250", "--run", "ZOOMEXTENTS")
    # A grip being adjusted with the dimension boxes up. Two things share the
    # cursor here and both have to be legible: the line above says what the
    # object is becoming, the boxes below take the exact drag. Drawn on top of
    # each other -- which is what happened first -- neither can be read.
    "10l-grip-dim"     = @("--run", "LINE", "--run", "0,0", "--run", "400,0", "--run", "",
                           "--run", "LINE", "--run", "0,100", "--run", "400,100", "--run", "",
                           "--run", "ZOOMEXTENTS", "--click", "@200,0",
                           "--click", "@400,0", "--hover", "@350,25", "--type", "250")
    # Polylines drawn with a width, which is a filled shape and not a thick
    # line. Three of them: a uniform run with a bend, a taper to a point, and a
    # bulged segment. Worth a picture because every fault here is a shape fault
    # -- a notch at a corner, a taper that does not taper, a hole where two
    # segments overlap -- and none of them shows up in a number.
    "10m-wide-polyline" = @("--open", (Join-Path $root "samples\wide-polylines.dxf"),
                            "--run", "ZOOMEXTENTS")
    # A paste mid-flight: the clipboard's contents ghosted under the pointer,
    # waiting for an insertion point. Worth a picture because the ghost is the
    # only thing that makes the insertion point choosable rather than guessed,
    # and because a ghost drawn in the wrong layer colour -- the clipboard's
    # handles mean nothing in this drawing -- looks like working software right
    # up until it lands.
    "10n-paste-ghost"  = @("--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
                           "--run", "CIRCLE", "--run", "200,150", "--run", "80",
                           "--run", "ZOOMEXTENTS", "--click", "@200,230",
                           "--run", "COPYCLIP", "--run", "PASTECLIP",
                           "--hover", "@300,120")
    # A multiline: several parallel lines drawn as one object, which is what a
    # wall is in a drawing made with the tool other CAD software provides for walls. Worth
    # a picture because the only thing that can be wrong is the mitre -- the
    # elements have to turn the corner together, the inner one short and the
    # outer one long, and a wall that changes thickness at a corner is obvious
    # in an image and invisible in a vertex count.
    "10q-mline"        = @("--open", (Join-Path $root "samples\mline-walls.dxf"),
                           "--run", "ZOOMEXTENTS")
    # A multileader: one note with two arrows sharing a landing, which is what
    # other CAD software's MLEADER command has produced since 2008 and what every drawing
    # made with it is full of. Worth a picture because the note comes out of a
    # nested block whose group codes repeat at three depths -- read wrongly it
    # draws the marker "LEADER_LINE{" on the sheet, or puts one arrowhead where
    # there should be two, and the file still saves perfectly either way.
    "10r-multileader"  = @("--open", (Join-Path $root "samples\multileader.dxf"),
                           "--run", "ZOOMEXTENTS")
    # A leader, and a leader being drawn. Worth two pictures because the parts
    # that can be wrong are all visual: an arrowhead pointing the wrong way or
    # too small to read, a landing on the wrong side of the bend, and a note
    # that sits on the line instead of beside it. None of that shows in a file.
    #
    # Drawn small on purpose. The arrow is 2.5 units and the drawing is 40, so
    # it is the size a drafter would actually see it at rather than a sliver.
    "10o-leader"       = @("--run", "RECTANGLE", "--run", "0,0", "--run", "40,30",
                           "--run", "LEADER", "--run", "10,10", "--run", "25,22",
                           "--run", "35,22", "--run", "", "--run", "TYP.",
                           "--run", "LEADER", "--run", "30,8", "--run", "16,4",
                           "--run", "8,4", "--run", "", "--run", "SIM.",
                           "--run", "ZOOMEXTENTS")
    "10p-leader-drag"  = @("--run", "RECTANGLE", "--run", "0,0", "--run", "40,30",
                           "--run", "ZOOMEXTENTS",
                           "--run", "LEADER", "--run", "10,10", "--hover", "@26,21")
    # Dialogs, which a picture of the main window never shows and which are
    # where any list the user has to read actually lives. "{OUT}" is replaced
    # with this scene's own file.
    "11-purge-dialog"  = $scaffold + @("--dialog-shot", "purge={OUT}")
    "12-references"    = @("--dialog-shot", "references={OUT}")
    # Where a user points the application at their own assistant. Worth looking
    # at because it is the one dialog that asks for a secret, and what it says
    # about where that secret ends up has to be legible without hunting.
    "13-ai-assistant"  = @("--dialog-shot", "agent={OUT}")
    # The command line mid-prompt, with the dynamic input under the crosshair.
    "10-prompting"     = $scaffold + @("--run", "LINE", "--run", "100,100", "--hover", "@250,180")
}

# Scenes that need the real drawing. Every scene above builds its own geometry,
# so the sweep runs anywhere; these do not, and are skipped when the file is
# absent. They are worth having anyway: a synthetic rectangle exercises none of
# what a real drawing has -- sixty layers, nine sheets, blocks nested three
# deep, and a floor plan dense enough for a rendering fault to hide in.
#
# The file is a client drawing and is not in the repository.
$sample = Join-Path (Join-Path $root "input files") "2nd Floor Remo Sketch.dxf"
if (Test-Path $sample) {
    $scenes["20-real-model"] = @("--open", $sample, "--run", "ZOOMEXTENTS")
    # One plan close enough to read, which is where a fault in the geometry
    # would actually show.
    #
    # The window is given in the drawing's own units rather than picked with
    # two clicks on the canvas. It used to be clicks, and when the chrome above
    # the drawing was merged into one row the canvas moved and this scene
    # quietly began photographing an empty patch of grid between the sheets --
    # a scene that is never looked at is a scene nobody is checking, and one
    # that photographs the wrong thing is worse.
    $scenes["21-real-plan"] = @("--open", $sample, "--run", "ZOOMEXTENTS",
                                "--run", "ZOOM", "--run", "0,100", "--run", "700,650")
    # A paper space sheet: its own geometry, and model space seen through a
    # viewport at the viewport's own scale. Those two disagreeing is the fault
    # this scene is here to catch.
    $scenes["22-real-sheet"] = @("--open", $sample, "--layout", "A-2.1 New First")
}

if ($List) {
    $scenes.Keys | ForEach-Object { Write-Host $_ }
    exit 0
}

if (-not (Test-Path $exe)) {
    Write-Error "Build first: $exe not found"
    exit 1
}

if (-not $Out) {
    $Out = Join-Path $root "build\visual-sweep"
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# A full sweep clears the folder first. Renaming a scene otherwise leaves its
# old picture behind, and a stale picture in a folder whose whole purpose is
# being looked at is worse than a missing one -- it shows a version of the
# interface that no longer exists and says nothing about being old.
if (-not $Only) {
    Get-ChildItem -Path $Out -Filter *.png -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# The offscreen platform has no font database of its own, and without one the
# text this sweep exists to inspect is drawn in a fallback face.
$env:QT_QPA_FONTDIR = "C:\Windows\Fonts"

$failed = @()
foreach ($name in $scenes.Keys) {
    if ($Only -and $name -notlike "*$Only*") { continue }

    $shot = Join-Path $Out "$name.png"
    Remove-Item $shot -ErrorAction SilentlyContinue

    # Quoted individually. This repository lives under a path containing both
    # spaces and parentheses, and an unquoted argument list splits on the first
    # of them into arguments the application then tries to open as drawings.
    #
    # --no-recovery-prompt because a modal would hang a run nobody is watching.
    # A scene that photographs a dialog names its own output and must not also
    # grab the main window, which would overwrite it with the wrong picture.
    $parts = @("-platform", "offscreen", "--no-recovery-prompt")
    # `.Replace` and a braced token, deliberately. `-replace` is a regex *and*
    # case-insensitive, so it rewrote the "out" inside "--layout" into a file
    # path and the application rejected the option it had never been given.
    # Any argument with those three letters in it was one edit from the same
    # fate.
    $sceneArgs = @($scenes[$name] | ForEach-Object { $_.Replace("{OUT}", $shot) })
    $parts += $sceneArgs
    if (-not ($sceneArgs -contains "--dialog-shot")) {
        $parts += @("--screenshot", $shot)
    }
    $arguments = ($parts | ForEach-Object { '"' + $_ + '"' }) -join " "

    $process = Start-Process $exe -ArgumentList $arguments -PassThru -NoNewWindow
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force
        Write-Host "  $name  TIMED OUT" -ForegroundColor Red
        $failed += $name
        continue
    }
    if (Test-Path $shot) {
        Write-Host ("  {0,-18} {1,8:N0} bytes" -f $name, (Get-Item $shot).Length)
    } else {
        Write-Host "  $name  NO IMAGE (exit $($process.ExitCode))" -ForegroundColor Red
        $failed += $name
    }
}

Remove-Item Env:\QT_QPA_FONTDIR -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "$Out"
if ($failed.Count -gt 0) {
    Write-Error "$($failed.Count) scene(s) produced nothing: $($failed -join ', ')"
    exit 1
}
