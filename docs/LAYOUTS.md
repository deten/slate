# Sheets: layouts, viewports and page setup

A drawing has a Model tab and any number of sheets. Each sheet is a layout: a
piece of paper with its own geometry (a title block, notes, viewport frames)
and one or more viewports looking into the model at a scale.

## The current space

Whatever tab is showing is where drawing commands draw. A line drawn on
Sheet1 is on Sheet1; switch to Model and the next line is in the model. The
same holds while editing a block in place. The database carries this as
`DrawingSettings::currentSpaceHandle`, which the shell keeps in step with its
tabs, and every command adds through `Transaction::addToCurrentSpace`.

## Commands

| Command | What it does |
|---|---|
| `LAYOUT` (LO) | Switch to a sheet by name. `New` makes one (A4 landscape, 1:1, 10 mm margins), `Delete` removes one with everything on it, `Rename` renames one. All three undo. |
| `MVIEW` (MV) | On a sheet: a viewport by two corners, or `Fit` to fill the printable area. Then a scale as `1:100` (or just `100`), or `Fit` to zoom the model's extents into the window. |
| `PAGESETUP` | Paper (A4 to A0, Letter, Legal, Tabloid), orientation, and the plot scale for the current sheet. Enter keeps a setting as it is. |

A viewport's scale is paper to model: at `1:50`, one paper unit shows fifty
model units, so a 60-unit-tall window shows 3000 units of model. MVIEW
refuses to run on the Model tab, because a viewport there would be a window
onto itself.

## In the file

Sheets go out in the standard DWG form. The first sheet takes the
`*Paper_Space` block; later ones are `*Paper_Space0`, `*Paper_Space1`, and so
on. Entities on a sheet carry group 67; viewports are numbered from 2, with
1 reserved for the sheet's own paper view; the page is stored as the portrait
paper in millimetres plus a rotation.

## Working through a viewport

Double-click inside a viewport, or type `MSPACE` (MS), to step into it. The
frame turns the selection colour, and from then on the model is what you are
working in: typed and picked points are model coordinates, snaps and picks
find model objects, drawing commands draw in the model, and the coordinates
read out are the model's. Zoom and pan, with the wheel, the middle button or
the ZOOM and PAN commands, move the model behind the frame rather than the
sheet, and what they leave showing is what the viewport shows from then on.
Double-click outside the frame, or type `PSPACE` (PS), to step back out.
Switching tabs steps out too.

## Not yet

Locking a viewport's scale and per-viewport layer freezing are read and
drawn from files but cannot yet be changed here.
