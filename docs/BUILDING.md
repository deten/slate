# Building

## The name

The product is **Slate**. Its full name is **Slate CAD**, and the "CAD" is there
to be *found*: somebody who needs drafting software searches for what they want
to do, not for a name they have never heard.

So the two forms are used in different places, and neither is a shortening of
the other. They live in one header, `src/app/Branding.hpp`:

| Form | Where it goes | Why |
|---|---|---|
| `Slate CAD` | executable version resource, installer, Start menu entry, Add/Remove Programs | every one of these is a string that gets *searched* |
| `Slate` | title bar, dialogs, settings folder, anything inside the running app | by then the finding is done, and a title bar that repeats a search keyword at its user is an advertisement |

The binary is `Slate.exe`, because that is the product's name and it is what a
user looks at in a folder for the rest of the software's life. Findability is
carried by the things that are actually indexed, in descending order of weight:

1. **The Start menu shortcut**, named `Slate CAD` — by far the strongest signal,
   and the one that matches both "slate" and "cad".
2. **The version resource** compiled into the executable (`src/app/Slate.rc`) —
   `ProductName`, `FileDescription` and `Comments`. Windows Search indexes these
   and the Details tab shows them.
3. **The Add/Remove Programs entry**, which the Settings app searches.
4. **File associations** for `.dwg` and `.dxf`, which put the application in
   front of anyone right-clicking a drawing.

All of that says **DWG compatible**, which is what this software does: it reads
and writes DWG files. That is the claim that is ours to make, and it matches the
searches that matter.

The installer definition is `packaging/windows/Slate.wxs` and needs the [WiX
Toolset](https://wixtoolset.org/); it is not part of the CMake build.

**Renaming did not cost anyone their settings.** On first run, if there is no
`Slate` folder and there is an older one, the whole thing is copied across —
configuration *and* recovery files. That second part is the reason it exists:
renaming an application renames the folder it keeps unsaved work in, and a user
who had a crash waiting to be recovered would otherwise have found a fresh
application looking somewhere that had never existed. It copies rather than
moves, so a failure leaves the original untouched.

## Requirements

| Component | Version used | Required for |
|---|---|---|
| Visual Studio 2022 with **Desktop development with C++** | MSVC 19.44 | everything |
| Windows SDK | 10.0.22000 | everything |
| CMake | 4.4.2 | everything |
| Ninja | 1.13.1 | everything |
| Qt 6 | 6.8.3 LTS | the application shell only |

The core engine and its tests need **no Qt and no GPU**. That is an
architectural property, not a convenience: `cad_core` links nothing but the
standard library (see ADR 0002), so it builds and its tests run on any machine
with a C++20 compiler.

## Portable tooling

CMake and Ninja are used as portable archives under `.tools/`, so no
machine-wide install or elevation is needed. `.tools/` is git-ignored.

If it is missing, recreate it:

```powershell
$tools = ".\.tools"
New-Item -ItemType Directory -Force -Path $tools | Out-Null
$ProgressPreference = 'SilentlyContinue'

Invoke-WebRequest `
  -Uri 'https://github.com/Kitware/CMake/releases/download/v4.4.2/cmake-4.4.2-windows-x86_64.zip' `
  -OutFile "$tools\cmake.zip" -UseBasicParsing
Expand-Archive "$tools\cmake.zip" -DestinationPath $tools -Force
Remove-Item "$tools\cmake.zip"

Invoke-WebRequest `
  -Uri 'https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip' `
  -OutFile "$tools\ninja.zip" -UseBasicParsing
Expand-Archive "$tools\ninja.zip" -DestinationPath "$tools\ninja" -Force
Remove-Item "$tools\ninja.zip"
```

Qt is fetched the same way, into `.tools/Qt`:

```powershell
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir .tools/Qt
```

`build.ps1` finds it automatically. If Qt is absent the build still succeeds —
it just skips the application shell and builds the engine and tests, which is
the layering working as intended.

## DWG support

DWG is a closed binary format, read by shelling out to the free ODA File
Converter (ADR 0001). It is fetched the same portable way, and *extracted*
rather than installed — no registry entries, no Start menu, no elevation:

```powershell
$tools = ".\.tools"
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest `
  -Uri 'https://www.opendesign.com/guestfiles/get?filename=ODAFileConverter_QT6_vc16_amd64dll_27.1.msi' `
  -OutFile "$tools\ODAFileConverter.msi" -UseBasicParsing

# An administrative install unpacks the payload without registering anything.
msiexec /a "$((Resolve-Path "$tools\ODAFileConverter.msi").Path)" /qn `
  TARGETDIR="$((Resolve-Path $tools).Path)\ODA"
```

`build.ps1` then links `.tools/ODA` into `build/<Config>/bin/converters/` so
the built application finds it with no configuration. Removing `.tools/ODA`
undoes the whole thing.

The converter is looked for in this order:

1. the `CAD_ODA_CONVERTER` environment variable;
2. `converters/ODAFileConverter/` beside the executable;
3. the usual install locations (`C:\Program Files\ODA\ODAFileConverter <version>\`).

Without it, DXF still works and opening a DWG reports what is missing, where it
looked, and how to fix it — rather than failing silently.

```powershell
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg               # what is in it
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --annotations # dimensions, hatches, text angles
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --sheets      # build every layout headlessly
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --dxf out.dxf # keep the intermediate
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --attachments # images and underlays
```

`dwg_info` is also how you tell a translator problem from a reader problem: the
intermediate DXF is exactly what the reader was given.

### What a save must not delete

A save rewrites the whole file, so anything the reader did not carry is
*deleted* — silently, from a drawing that opened without complaint. That makes
preservation a correctness requirement rather than a nicety, and it works at
three levels:

| Level | Mechanism | What it protects |
|---|---|---|
| Group codes | `DbObject::preserved` | Codes no property mapped to |
| Extended data | `DbObject::xdata` | Third-party application data, on *any* record |
| Sections and tables | `Database::preservedSections` | CLASSES, ACDSDATA, VPORT — structures nothing here parses |

`tools/roundtrip_check.py` counts group codes independently of the C++ reader,
so the two cannot agree by sharing a bug. On the reference drawing a read/write
cycle now differs from its input by 439 group codes out of 344,000, and every
one of them is accounted for:

- values the writer omits because they are at their default (a zero Z, a zero
  polyline width, a zero viewport twist);
- an ACI colour written alongside a true colour, where the true colour is
  authoritative and survives exactly;
- 18 `APPID` registrations belonging to no XData in the file;
- one `STYLE` record with an empty name, which is a SHAPE file entry.

Two of those are worth fixing and are not fixed yet: the empty-named STYLE, and
the second code 70 on a LAYOUT, which is a case of one code meaning two things
in two subclasses. Both are recorded here rather than in a comment nobody reads.

`--sheets` answers "why is this sheet blank" without a screenshot. It reports
each viewport's paper rectangle, the model window it looks through, how many
entities the index finds there, and how many items the layout actually builds —
so a viewport aimed at empty space is distinguishable from one that is switched
off, mis-scaled, or culled.

`--fields` evaluates every field and compares each with the value the authoring
application cached, which is the fastest way to tell a reader problem from a
drawing that has simply changed since its fields were last updated.

`--annotations` groups MText by rotation. Nearly all text in an architectural
drawing is horizontal, so a spread of odd angles is the signature of rotation
being read out of the wrong group.

`python tools\dxf_probe.py file.dxf MTEXT --codes 1,11,21,50 --limit 5` dumps
selected records when the question is what is actually *in* the file.

## Running the application

```powershell
.\build\RelWithDebInfo\bin\Slate.exe
.\build\RelWithDebInfo\bin\Slate.exe samples\sample.dxf
```

The Qt runtime is copied next to the executable by `windeployqt`, so
`build/<Config>/bin/` is a directory that can be copied and run elsewhere —
the stated initial distribution model.

## Plugins

Plugins are shared libraries in `bin/plugins/`, loaded whenever a drawing is
opened or created. Each load result is reported in the command history, so a
plugin that failed says so rather than simply being absent.

The example plugin builds automatically and lands there, which is why
`BOLTCIRCLE` is available in a fresh build. See [PLUGINS.md](PLUGINS.md) for how
to write one and [ADR 0004](adr/0004-plugin-abi.md) for why the boundary is
shaped the way it is.

Generate a sample drawing to open:

```powershell
python tools\make_sample_dxf.py samples\sample.dxf
```

## Headless tools

`dxf_roundtrip` reads a DXF and writes it straight back, for checking the
read/write path without the UI. It links only `cad_core` and `cad_io`, which is
also standing proof that the engine is genuinely UI-free — it could not build
otherwise.

```powershell
.\build\RelWithDebInfo\bin\dxf_roundtrip.exe samples\sample.dxf out.dxf
python tools\roundtrip_check.py samples\sample.dxf out.dxf
```

`roundtrip_check.py` is a deliberately independent group-code counter sharing
no code with the C++ reader, so the two cannot agree by sharing a bug.

`bench_scene` measures scene building at several drawing sizes and zoom levels.
It is what settled the GPU-backend question in [ADR 0003](adr/0003-gpu-backend.md),
and it is the tool to re-run before claiming a rendering change made anything
faster.

```powershell
.\build\RelWithDebInfo\bin\bench_scene.exe                 # the standard table
.\build\RelWithDebInfo\bin\bench_scene.exe --max 50000     # quicker
.\build\RelWithDebInfo\bin\bench_scene.exe --csv > out.csv # for comparison
```

### Controls

**Navigation**

| Input | Action |
|---|---|
| Mouse wheel | Zoom about the cursor |
| Middle-drag, or Alt + left-drag | Pan |
| `Ctrl+Shift+E` | Zoom extents |
| `Ctrl+PgUp` / `Ctrl+PgDn` | Previous / next layout tab |

**Selection** (when no command is running)

| Input | Action |
|---|---|
| `Esc` | Cancels a running command; with none, clears the selection |
| Hover | Outlines the object a click would select |
| `Shift+Space` | Step through overlapping objects under the cursor |
| Left-click | Select the entity under the cursor |
| Shift + left-click | Add to / remove from the selection |
| Drag left-to-right | Window select — only what is fully enclosed |
| Drag right-to-left | Crossing select — anything the rectangle touches |
| `Esc` | Clear the selection |
| `Delete` | Erase the selection |
| Drag a grip | Reshape the entity directly |

Grips are the small squares on a selected entity. The one under the cursor
enlarges and the cursor becomes a hand, so it is clear what a press would grab.
Dragging shows a dashed preview and honours object snaps; `Esc` abandons the
drag, which costs nothing because the drawing is only changed on release.

**Drawing**

Just type. The command line holds the keyboard at all times, so `m` then Enter
or Space runs MOVE without clicking into anything first — and what you type is
mirrored beside the crosshair, so your eyes stay on the drawing. Clicks reach
the canvas regardless of focus, which is what lets the two coexist.

Space ends a command name exactly as Enter does. It stays an ordinary space
only when a command is asking for *text*, which is the one place a space is
content rather than a terminator.

### The ribbon

Commands are also on the ribbon, arranged by a curated table in
`src/app/RibbonLayout.cpp` rather than derived from the command registry's
categories — because those answer different questions.

A category is a *taxonomy*: it says what kind of command something is, which is
the right organising idea for a menu. A ribbon answers "what am I doing next".
MOVE, COPY and ROTATE belong together because a drafter reaches for them in the
same breath; they share a category with TRIM and FILLET, which they have almost
nothing to do with in use. Deriving the ribbon from categories produced one flat
run of every Draw command in registration order, three deep, with no grouping
the eye could use — and a plugin's "Bolt Circle" wedged between Arc and Circle.

So each tab holds captioned groups with a rule between them, and the groups are
chosen by task:

| Tab | Groups |
|---|---|
| Home | Layers · Draw · Modify · Annotate — the frequently-reached subset of several tabs, which is what makes it the tab you can stay on |
| Draw | Lines · Curves · Regions |
| Annotate | Text · Dimensions · Fields |
| Modify | Position · Reshape · Objects · History |
| Insert | Block |
| View | Navigate · Sheets |
| Tools | Selection |

**A command the table does not mention still appears** — under its category, in
the tab that category maps to, captioned "More" when the category and the tab
share a name. That is what keeps a plugin reachable without `RibbonLayout.cpp`
knowing it exists, and it is why the curated table is not a way to lose
commands. Anything in an unrecognised category lands in **Tools** rather than
nowhere.

**A contextual tab appears for what is selected.** Select a run of text and a
`Text` tab arrives at the end of the strip, in the accent colour so it reads as
something that came and went rather than as the ribbon rearranging itself; it
holds Edit Text. Select a schedule and a `Table` tab offers Update Fields, which
for a table full of fields is the one thing that genuinely applies.

Three rules make it usable rather than distracting:

- **A mixed selection gets no tab.** There is no honest answer to "what would
  you like to do with these six things of four types", and offering the first
  one's tools would be a guess dressed as an answer.
- **It is raised when it appears, not on every selection.** A ribbon that moves
  under the hand each time something is clicked is a ribbon nobody can aim at.
- **Dismissing it returns you to the tab you were on.** Removing the current tab
  otherwise leaves Qt on whatever is now at that index — so a drafter who was on
  Home, selected a table, then deselected would find themselves on Tools, having
  asked for neither.

Types with nothing worth offering get no tab at all. An image, an underlay and
an OLE frame all have no commands of their own yet, and a contextual tab holding
one irrelevant button is worse than none: it teaches the user to ignore the
place where the relevant thing will eventually be.

Sizes are shared through `RibbonLayout.hpp` so a control contributed from
elsewhere — the layer strip — lines up with the buttons beside it. Guessing at
them is what produced "Layer Pro...ies" next to three state switches whose
labels ran into one another, and small buttons an inch shorter than their own
text.

### Palettes and the status bar

The **properties palette** fits its name column to its content and then *caps*
it, so the value column always keeps enough width to be legible. A share of the
panel was the obvious rule and it is wrong in both directions — it wastes space
on short names and truncates long ones in a narrow panel. Capping puts the
truncation on the names, which a reader can guess at, rather than on the values,
which they cannot. Category rows are banded rather than merely bold: one heavier
row is not a boundary.

The **status bar** reads coordinates and zoom in a fixed-pitch face. In a
proportional one every digit is a different width, so the numbers shuffle
sideways as the pointer moves and the readout is unreadable while doing the one
thing it exists for. Rules separate the fields, because four labels in a row
with nothing between them read as one sentence made of unrelated words.

| Command | Aliases |
|---|---|
| `LINE` | `L` |
| `PLINE` | `PL` |
| `CIRCLE` | `C` |
| `ARC` | `A` |
| `RECTANG` | `REC` |
| `POLYGON` | `POL` |
| `POINT` | `PO` |
| `MOVE` `COPY` `ROTATE` `SCALE` `MIRROR` | `M` `CO` `RO` `SC` `MI` |
| `ERASE` `OFFSET` `TRIM` `EXTEND` `FILLET` | `E` `O` `TR` `EX` `F` |
| `CHAMFER` `BREAK` `JOIN` `EXPLODE` `STRETCH` | `CHA` `BR` `J` `X` `S` |
| `UNDO` `REDO` | `U` |
| `DIMLINEAR` `DIMALIGNED` | `DLI` `DAL` |
| `DIMRADIUS` `DIMDIAMETER` | `DRA` `DDI` |
| `DIMANGULAR` `DIMORDINATE` | `DAN` `DOR` |
| `TEXT` `MTEXT` `TEXTEDIT` | `DT` `DTEXT` · `T` `MT` · `DDEDIT` `ED` `MTEDIT` |
| `UPDATEFIELD` | `FIELDEVAL` |
| `BLOCK` `INSERT` | `B` `BMAKE` · `I` `DDINSERT` |
| `HATCH` | `H` `BH` `BHATCH` |
| `ZOOM` `ZOOMEXTENTS` `PAN` | `Z` · `ZE` · `P` |
| `LAYOUT` `QSELECT` | `LO` · `QS` |
| `BOLTCIRCLE` (example plugin) | `BC` |

**The aliases are other CAD software's**, taken from stock `acad.pgp` rather than invented:
`c` is CIRCLE, `cp` and `co` are COPY, `z` is ZOOM, `p` is PAN. Muscle memory is
the entire point of an alias, so they are pinned by a test rather than left to
drift as commands are added and someone reaches for a letter already spoken for.

`ZOOM` takes other CAD software's options — `z` then `e` is zoom extents, and `ze` does it
in one word. Also Window, All, Center and Previous. `ZOOM Previous` returns to
the view before the last *command-driven* change; wheel notches are not recorded,
because "previous" should mean the view you were working in, not one click back.

`BLOCK` turns a selection into a definition and replaces it with a reference —
the selection is consumed, as in other CAD software, because being left with the loose
originals *and* a block would be a surprise and a duplicate. Contents are stored
relative to the base point, so a reference places that point at its insertion
point. A name already in use is refused rather than redefined: redefining
silently changes every existing reference in the drawing.

`INSERT` takes a name, a point, an X scale, a Y scale defaulting to the X, and
a rotation. It refuses a layout's block record, which would nest a sheet inside
the model.

`TEXT` keeps prompting after each line and stacks the next below it, so a
three-line note is one command. An empty line ends it. `MTEXT` takes a
rectangle first: its width is the wrap column and its top-left corner is what
the paragraph hangs from.

Both take their style from the current text style and **skip the height prompt
when that style fixes a height**, which is how other CAD software behaves and why a
drawing set with a house style needs one fewer answer per note.

### Formatted paragraph text

MText is not a string with a font — it is a string with formatting *inside* it,
so a note can carry a bold heading over a smaller body with one word in red
because it changed. All of that used to be stripped before drawing, and the
whole note came out in one size and one colour.

`parseMText` splits contents into runs of uniform formatting, honouring bold,
italic, underline, overline, strikethrough, height, width, obliquing, colour and
font family, with braces scoping a run the way the format defines. Heights are
*factors* and an unset colour means the entity's own, so a run survives the
entity being restyled.

Two things it deliberately does not do: a code it does not model is dropped
rather than shown, because a code rendered as its own characters is worse than
one not rendered at all; and stacked fractions read as `1/2` rather than
stacking, which needs a second baseline the layout does not have.

**`\L` was the bug worth naming.** It is a toggle carrying no argument, and it
had been grouped with the codes that end in a semicolon — so an underline
swallowed everything up to the next `;`, usually the rest of the note. An
underlined run did not render wrong; it disappeared.

**The in-place editor is a rich one.** Double-click a note and it opens showing
its formatting rather than its codes, with a bar above it for bold, italic,
underline, size and colour — acting on the *selection*, so two words in a
paragraph can be bolded without touching the rest. Ctrl+B, Ctrl+I and Ctrl+U
work as they do everywhere.

Showing the codes instead would be honest and useless: nobody composes a note
reading `{Arial|b1|i0|c0|p0;GENERAL NOTES}\P`. But the round trip through a
rich document is where notes get quietly damaged, so two things guard it:

- **Codes this application does not model are carried through** as an opaque
  property on the run that followed them, and written back where they were. A
  paragraph indent survives somebody correcting a typo.
- **A note that comes back unchanged is written back byte for byte**, as it
  arrived, rather than re-serialised into this application's own spelling of the
  same codes. Opening a note to read it must not produce a diff.

`formatRuns` is the inverse of `parseMText`, and the pair is pinned by a
round-trip test: parse, serialise, parse again, and the runs must match.

Two limits worth knowing. MText has **no standalone bold code** — weight is part
of a font selection — so making text bold needs a font family to make it bold
*of*; the text style's own typeface is used, and with none available the weight
is dropped rather than written as something that will not render. And a font a
drawing *names* is often not one this machine *has*: `txt.shx` is a shape file,
not a system font. Asking the font database first and keeping the entity's own
face when the answer is no is the difference between text in the wrong typeface
and text in the wrong alphabet — Qt substitutes a symbol face for an unknown
name, and "SITE PLAN" comes out as "SITE PLA—¬".

**Double-click any annotation to change its words.** That runs `TEXTEDIT`, which
also takes a name of its own or a pre-selected object, and covers single-line
text, paragraph text, block attributes — a title block is attributes, and
changing one is the most common text edit in a drawing set — and a dimension's
text override.

**The editor opens over the text, at the size the text is drawn.** A note is
written *against* the drawing: it has to fit between two walls, not collide with
a dimension, and wrap at a column the drafter chose. None of that is visible in
a one-line box at the bottom of the window, so writing a note there is a guess
followed by a look followed by a correction.

Enter commits a single line. In a paragraph, Enter is a line break and
**Ctrl+Enter** commits — other CAD software's rule, and the only one that lets a note be
written at all. Escape cancels, and a click anywhere else on the drawing
accepts. Losing focus deliberately does *not* commit: focus moves for reasons
that have nothing to do with the user finishing, and each of those would end the
edit silently.

The command line keeps the same prompt throughout, so a script or a headless
test drives either path.

The existing words arrive **in the editor, selected**, so typing replaces them
and an arrow key amends them. A prompt that shows the old text and makes you
retype it is not an editor, and it is why annotation in a half-finished CAD
program feels like data entry. Paragraph text is handed over with its formatting
codes intact rather than as plain text, because stripping them would quietly
reformat a note the moment it was edited. The one exception is `\P`, the
paragraph break, which becomes a real line break in the editor and is re-encoded
on the way out — it is layout the user is looking at rather than formatting.
`MTextEntity::paragraphsToLines` and its inverse are pinned by a round-trip test,
because editing a note *without changing it* must not change it. Enter on an unchanged line leaves the
text alone: emptying it instead would make Enter a way to delete a label by
accident. The command keeps asking for the next object, so correcting a sheet's
worth of labels is one command.

Editing text that is driven by a **field** says so. The number in
"MASTER BEDROOM 423.08" comes from the area of a polyline and the next
`UPDATEFIELD` will put it back, so typing over it looks like it worked and
silently will not last.

**Fields** are text whose value is derived rather than typed: a room label
reading `MASTER BEDROOM 423.08` is the name of a space, the area of the polyline
bounding it, and a format. Stretch the polyline and the number follows. That is
what makes an area schedule on a drawing worth trusting.

Field text is drawn with a **grey background**, as the `FIELDDISPLAY` setting in other CAD software does.
It is the only thing that says a number is derived — that it will move on its
own, and that typing over it throws that away. Plots leave the shading off: it
is an editing aid, not ink.

`UPDATEFIELD` (`FIELDEVAL`) recomputes them. It is a command rather than
something that happens on every edit, because a drafter halfway through moving a
wall does not want the schedule flickering. It reports what changed, and what it
could not evaluate and why — a field that quietly kept a stale number is
indistinguishable from one that was right.

Four evaluators are implemented, which between them cover every field in the
sample drawing:

| Evaluator | What it does |
|---|---|
| `_text` | Literal text with holes, each naming a child field |
| `AcObjProp` | A property of another object — `Area`, `TextString`, `Layer` |
| `AcExpr` | Arithmetic over child fields |
| `AcVar` | Drawing variables: file name, path, layout |

`AcObjProp` reads through the **same property registry** the properties palette
uses, so a property an entity type contributes is available to a field with no
change to the field code — including one added by a plugin. other CAD software's names are
mapped onto ours where they differ (`TextString` is our `text.contents`) and
matched on the display name otherwise.

The formats are other CAD software's terse ones — `%lu2%pr2%ct8[0.0069444444444444]` is
"decimal, two places, times 1/144", which is how an area of 60924 square inches
becomes a label reading `423.08`. Precision falls back to the drawing's `LUPREC`
when the format states none.

A field this application cannot evaluate — a table formula such as
`SUM(B2:B11)`, or one naming an object that has been erased — keeps **the value
the authoring application last computed**, which the file stores alongside the
code. A stale number is worth far more on a construction drawing than a blank or
an error, and `UPDATEFIELD` says which ones they are.

```powershell
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --fields
```

reports every field, what it evaluates to here, and whether that agrees with what
the drawing was saved with. Disagreement means either the drawing changed since
its fields were last updated — which is the whole point of a field — or that our
evaluation is wrong, and the two are easy to tell apart from the output. All 87
fields in the sample DWG agree, to the digit.

### Tables

A schedule is one of the parts of a drawing set that is *read* rather than
looked at, and `ACAD_TABLE` is how other CAD software stores one. Rows, columns, merged
cells and cell values are all read, and the grid is drawn from the row heights
and column widths.

**Generated, not replayed.** The file also carries an anonymous block holding
the lines and text other CAD software drew, and a cached bitmap of the same — 11 kilobytes
of proxy graphics on the reference drawing. Neither is used, for the reason
[ADR 0005](adr/0005-derived-annotation.md) gives for dimensions: a table drawn
from its block is a *picture* of a table, and the moment a value changes it is a
lie.

**Most of a schedule is fields.** The cells hold a field handle and no text of
their own — the room names come from the labels on the plan and the areas from
the polylines that bound them. Unlike an MTEXT, whose evaluated contents are in
the file, a cell's value has to be composed: the cell's own field is a template
like `2nd %<\_FldIdx 0>%` and only its *child* caches anything. Reading the
cache alone gives an empty grid, which is what this looked like at first.
`refreshTableCells` works them out after a read and again on `UPDATEFIELD`.

Two known gaps, both visible on the reference drawing:

- A cell whose value is a table formula — `SUM(B2:B11)/144` — falls back to the
  value the drawing was saved with, because the formula evaluator does not
  understand ranges yet. That is the same rule the rest of the field code
  follows and the numbers are right; they just will not move.
- A paragraph break inside a value composed from a child field shows as `\P`
  rather than breaking the line. The escaping that puts it there is what makes
  all 87 of the drawing's fields agree with other CAD software to the digit, so it is left
  alone rather than traded for one column of one table.

Editing a table is not implemented, so nothing writes one: the record is read
for its meaning and written back for its bytes, on the same terms as a FIELD.

### Attached files

A drawing can display things that are not geometry: a scanned sketch attached as
a raster image, a consultant's PDF traced over as an underlay, a spreadsheet
fragment pasted in as an OLE object. All three are read, placed, selected,
plotted and saved. See [ADR 0006](adr/0006-attached-content.md) for why the
engine places them and the Qt backend decodes them.

Raster images draw for real — clipped, faded, and adjusted for brightness and
contrast, with the placement's own pixel vectors so a rotated or mirrored image
comes out right. Decoding goes through a cache that scales *during* decode, so a
7000-pixel scan shown 400 pixels wide never allocates 130 megabytes.

PDF underlays and OLE objects are placed but not drawn: rasterising a PDF needs
a PDF engine and rendering an OLE object means asking Windows to do it. They
show as a dashed, captioned outline, which is the honest picture — something is
here, this is exactly where, and its contents are not being shown.

```powershell
.\build\RelWithDebInfo\bin\dwg_info.exe drawing.dwg --attachments
```

lists every attached file, whether it was found, and how many entities place it.

**`REFERENCES`** (also `XREF`, `IMAGE`, `ER`) opens the reference manager, which
shows the same list and can repair it. Select a missing file, choose Locate, and
point it at where the file actually is. Until this existed the application could
say precisely which path it had looked for and could do nothing about it, which
makes a report a complaint rather than a diagnosis — and a drawing arriving from
another office with its images on a drive letter nobody here has is the ordinary
case, not the exceptional one.

Repathing writes the **stored** path, relative to the drawing when the new file
is inside the drawing's own folder. Writing the absolute path we happened to
resolve would fix the file on this machine and break it for everyone else, which
is the failure the whole resolution mechanism exists to avoid. It goes through a
transaction, so pointing an image at the wrong file is one Ctrl+Z away.
The same summary appears in the command history on open, naming each missing
path — because the path is the only part of a broken attachment a user can act
on.

**On paper, an outline is not printed.** It is an editing aid, and printing it
would put a mark on a deliverable that is not in the drawing. The plot says how
many items it left blank instead, and Plot Preview shows where they are.

`CHAMFER` takes two distances, the second defaulting to the first, so a
symmetric bevel is one keystroke and an asymmetric one is still available.
`BREAK` uses the point you selected the object at as the default first break
point, and pressing Enter at the second breaks *at* the first instead — keeping
both halves, which is other CAD software's BREAK AT POINT.

`EXPLODE` goes one level deep: a block containing blocks explodes into those
blocks rather than all the way to lines, because the nesting is information the
drawing was built with and exploding past it cannot be undone by re-blocking.

`STRETCH` asks for a crossing window after the selection. Objects with one end
inside are reshaped; objects wholly inside are moved. It works through the same
per-entity grip logic that dragging a grip uses, so every entity type stretches
by the same rules it already reshapes by.

`HATCH` takes its boundary from a *selection*: pick the objects that bound the
region, then press Enter or right-click. Closed polylines and circles are used
exactly; loose lines and arcs are chained into loops end to end, and a chain
that does not close is refused rather than being closed with an invented
segment.

While a command runs: click to pick points, or type coordinates as `x,y` —
`@dx,dy` is relative to the last point. Keywords are entered by their
capitalized prefix, so `C` closes a polyline. `Enter` accepts a default or ends
the command; `Esc` cancels. On an empty command line, `Enter` or right-click
repeats the last command.

**Object snaps** run by default for endpoint, midpoint, centre, node, quadrant,
intersection, perpendicular and extension — each with its own marker glyph, so
it is clear what the cursor has latched onto without reading anything.

The **Osnap** button in the status bar toggles them all; its dropdown chooses
which run. The state is stored as `osmode` in the `[drafting]` section, the same
bitmask other CAD software uses, so a value carried over from a drawing or a colleague
means the same thing here. Turning snapping off keeps the chosen set, so it can
be switched back on without reconfiguring.

Tangent and Parallel are off by default: both need a base point and are reached
for deliberately. **Nearest** is off because it matches *everywhere* along an
object — with it running, a point near a line can never be placed freely, which
is the usual complaint about over-eager snapping.

**Extension** works by acquisition, as in other CAD software: hover an object, then move
past its end and the extension appears. It has to work that way. The snap
reports a point *off* the object, so the object is always further from the
cursor than the aperture, and the index search around the cursor can never find
it — which is why remembering the last few hovered objects is the mechanism
rather than a wider search.

**Properties palette** — shows what the selection has in common and edits all of
it at once. Rows disagreeing across the selection read `*VARIES*`, and editing
one sets every object. Rows come from the core's property registry, so an entity
type contributed by a plugin appears here without the palette knowing about it.
Angles are shown in degrees and stored in radians; entities on a locked layer
are skipped.

**Layers** are on the ribbon's Home tab: the current layer in a drop-down, and
on / freeze / lock for it beside them. That is the part a drafter touches many
times an hour, so it sits in the reach of the drawing hand rather than in a
palette competing with the drawing for width.

**Layer Properties** opens the full manager for everything else — on, frozen,
locked and plot are one click each; double-click a colour to change it or a name
to make it current. Every change is undoable. The filter box matters on real
drawings: the sample DWG in `input files/` has sixty layers.

**Panels** (Properties, Layers, History) can all be closed, and come back from
**View → Panels**. A palette that cannot be dismissed is a palette that is in
the way.

**Layout tabs** sit under the canvas, or `Ctrl+PgUp` / `Ctrl+PgDn`. Model is the
working area; each sheet is a paper-space layout showing the model through one
or more viewports at a chosen scale. Switching tabs zooms to the sheet, which is
drawn as an actual page with its printable area marked — geometry outside that
will not plot, and it is worth knowing before the plot rather than after. The
tab bar hides itself when a drawing has only a Model tab.

A sheet's size is stored in millimetres whatever the drawing's units, so an
imperial drawing states US Letter as 215.9 × 279.4 and draws its title block at
11 × 8.5. The page is drawn in the layout's own units with the plot rotation
applied, which is what makes the sheet and the geometry on it agree.

The **plot scale** applies too. A layout set to "fit to paper" has a scale that
is not 1, and then the page is not the size the plot settings state: an 11 × 8.5
sheet fitted at 1.309 is 8.40 × 6.49 in the units its own title block is drawn
in, and its corner is not at the paper-space origin. The layout's stored limits
say where it lands, and that is what the sheet is drawn from.

A viewport's `viewCenter` is measured from its `viewTarget`, not from the world
origin — other CAD software stores a view as a target plus an offset. A viewport that has
been panned, which is every viewport on a real sheet, carries a non-zero target,
and reading the offset alone shows the right scale of the wrong place.

Hovering outlines the whole object, not just a snap point — other CAD software calls it
rollover highlighting, and it answers the question every pick starts with:
*which* of these overlapping objects am I about to get?

When several objects are stacked under the cursor a badge appears reading
`2/3 ⇧Space`, and **Shift+Space** steps through them. Overlapping geometry is
the normal state of a real drawing — a wall line under a hatch boundary under a
viewport edge — and without cycling the topmost object is the only one a click
can ever reach. The status bar names each one as you go (`Line on A-WALL`),
because two coincident lines are identical to look at.

A click takes whatever the outline is showing, cycled or not. Re-picking at
click time would quietly select the topmost object again and make the cycling
look broken.

**`QSELECT`** (`QS`) builds a selection by querying properties: every door
block, every dimension on the demolition layer, every circle bigger than 5. On a
drawing with eight thousand objects that is not a convenience — a window
selection cannot express it at all.

The filter runs through the **same property registry** the properties palette
reads, rather than a hand-written list of filterable fields, so a property an
entity type contributes is queryable with no change to the query code. The type
list shows only the types present in the drawing, and a layer is chosen from the
layers that exist rather than typed — typing a layer name is how a query
silently matches nothing because of a capital letter.

Results can replace, add to, or remove from the current selection, so successive
queries narrow: "every dimension", then "of those, the ones on A-DEMO". Objects
on a locked or hidden layer are never matched — a selection you cannot then edit
is a trap.

## Plotting

**File → Plot…**, **Plot Preview…** and **Export PDF…**. What is plotted is the
current tab: a sheet plots the *paper*, fitted to the page, and the Model tab
plots the drawing's extents.

All three go through one `paintPlot`, and that in turn through the same
`paintScene` the canvas uses. A preview drawn by separate code is a preview that
can lie, and a plot that differs from its own preview costs a sheet of A1 to
discover.

Plots are **monochrome** by default — a drawing authored against a dark
background is largely white, and white ink is no ink. Set `monochrome = false`
under `[plot]` to keep colours. Line widths and the greeking threshold scale
with the device, or a 600 dpi plot comes out ghostly and greeks annotation that
would have printed perfectly legibly.

PDF export matches the page to the sheet, so an A1 layout does not arrive
letterboxed on A4.

`LAYOUT` (`LO`) switches sheets by name, which the tabs and `Ctrl+PgUp/PgDn` do
by position. That is what a script or a batch plot needs.

```powershell
# Plot one sheet without a window, for checking output or batch work.
.\build\RelWithDebInfo\bin\Slate.exe -platform offscreen drawing.dwg `
    --run "layout" --run "A-1.1 Exist" --export-pdf sheet.pdf
```

`--plot-image` renders the same plot to a PNG instead, which is how plot output
gets inspected and compared — paper being the one thing a test cannot look at.

## Not losing work

Auto-save writes a recovery copy **beside** your drawing, never over it, so a
crash mid-auto-save cannot damage the file it is protecting. Copies live in
`%LOCALAPPDATA%\Slate\recovery` — not next to the drawing, which may be on
a read-only share or a network path that is slow or gone.

```ini
[files]
autosave.enabled = true
autosave.minutes = 10
backup.enabled = true     # keep the previous contents as <name>.bak on save
```

It never fires mid-command: a command in progress may hold an open transaction,
and serialising then would capture a drawing halfway through an edit — a
recovery file that restores something the user never saw.

After a crash, the next launch offers the work back. The prompt has no
destructive option: declining leaves the files on disk, which is harmless and
self-correcting, because they are cleared on the next clean exit once the work
is saved. Deliberate deletion is **Tools → Discard Recovered Work**. Recovered
drawings open untitled, so saving is a decision rather than an accident.

Files are named after the drawing *and the writing process*, which is how a
second window can tell "that session crashed" from "another window has this open
right now" — a live session's auto-saves are never offered for recovery.

## Headless UI testing

The interface can be driven and photographed with no window at all, which is how
it is checked without taking the screen and keyboard from whoever is at the
machine:

```powershell
.\tools\headless_shot.ps1 -Out shot.png -Run "circle","0,0","100","","ze"
.\tools\headless_shot.ps1 -Out shot.png -File drawing.dxf -Hover "507,517"
```

`--run` submits command-line input exactly as typing does, so a script cannot
reach a path a user could not. `--hover` sends a real mouse-move event, so
rollover and snap markers are verifiable too. `--click` and `--double-click`
send real button events; a double-click is delivered as press, release,
`MouseButtonDblClick`, release, which is the sequence a mouse actually produces
rather than a shortcut into the handler. `QWidget::grab` renders through Qt
rather than reading the screen, so it needs no display.

`--type` sends text as keystrokes to whatever holds the keyboard and `--key`
sends one named key (`return`, `escape`, `ctrl+return`). They exist because the
in-place text editor is not the command line: without them there is no way to
reach it, and the feature would have no headless test at all.

**The options replay in the order they were typed**, across kinds. They used to
be grouped by option, so every `--run` happened before every `--click` however
they were written — fine for "draw this, then photograph it" and useless for
anything interactive. Editing a label is *click the label, then type the new
words*, and there was no way to say that:

```powershell
# Double-click a note, retype it in place, commit with Ctrl+Enter.
Slate.exe -platform offscreen --run "ze" --double-click "824,620" `
          --type "REVISED NOTES" --key "return" --type "SEE SHEET A-2.2" `
          --key "ctrl+return" --run "" --screenshot shot.png
```

`tools\capture_window.ps1` remains for the few things that genuinely need a real
window — focus behaviour, native dialogs, the pointer. It raises the window and
steals focus, so prefer the headless path.

## Customization

The cursor is other CAD software's: a crosshair with the **aperture box** in the gap at its
centre. The box is not decoration — it is the target. A click catches whatever
falls inside it, and a snap triggers on the same distance, so its size tells you
exactly how close you have to be.

Both are adjustable in **Tools → Settings**: the aperture in pixels, and the
crosshair arms as a percentage of the drawing area, matching other CAD software's
`CURSORSIZE`. At 100 the arms span the window, which is how some drafters line
work up across a sheet.

**Text too small to read is drawn as a bar** rather than as characters —
the `QTEXT` setting in other CAD software, and what page layout tools call greeking. Drawing *nothing*
was the old behaviour and it was worse: zoomed out far enough to see a whole
floor plan, every room name and every dimension simply left the drawing, and the
annotation a drafter is looking for at that zoom is exactly what vanished. A bar
keeps the placement, length and density of the text visible at a fraction of the
cost of rasterizing glyphs nobody could read.

The threshold is **Simplify text below** in Settings, or `text.greeking` under
`[drafting]`. Set it to 1 to always draw real characters, however small.

```ini
[drafting]
text.greeking = 3      # pixels of text height; below this, draw a bar
image.frames = false   # IMAGEFRAME: border attached images
```

**Frame attached images** is the `IMAGEFRAME` setting in other CAD software, and it is off for the same
reason other CAD software's is: the border plots. Turn it on to make an image easy to pick.
An image whose file could not be found is outlined regardless — there would
otherwise be nothing to see at all.

**Tools → Settings** covers auto-save and backups, the pick aperture, crosshair
size, the text greeking threshold, image frames, rollover highlighting, and
monochrome plotting. It is deliberately small: every row in it
does something. The brief lists a dozen categories, and filling a dialog with
the ones whose features do not exist yet would produce controls that look
configurable and are not — which is worse than an honest gap, because a user who
finds one decorative setting stops trusting the rest.

Settings apply immediately, not on next launch, and are written to the same file
described below. **File → Open Recent** keeps the last ten drawings, recorded on
open as well as on save.

Aliases and keyboard shortcuts live in a plain text file, reachable from
**Tools → Customize Aliases and Shortcuts…**:

```
%LOCALAPPDATA%\Slate\slate.ini
```

It is written, documented, on first run. **Ctrl+Shift+R** reloads it without
restarting.

```ini
[aliases]
c = COPY              # override the built-in C for CIRCLE
zz = MTEXT

[shortcuts]
CIRCLE = Ctrl+Shift+K
ERASE = Del
```

A user alias **overrides** a built-in one — remapping `C` from CIRCLE to COPY is
a common preference, and refusing it would make the feature useless to the
people most likely to want it. What cannot be overridden is a command's own
name: `MOVE` always runs MOVE, so a bad line can never make a command
unreachable from inside the application that has to fix it.

Aliases are applied after plugins load, so one can point at a plugin's command.
A line naming a command that does not exist is reported in the history rather
than ignored — an alias that silently does nothing is the worst outcome, since
the user has no way to learn why.

Editing a value rewrites that one line. Comments, blank lines, ordering, unusual
spacing and sections this build knows nothing about all survive: a settings file
that reformats itself teaches people not to edit it, and then not to trust it.

## Building

```powershell
.\build.ps1                      # configure, build, test (RelWithDebInfo)
.\build.ps1 -Config Debug        # debug build
.\build.ps1 -Clean               # from scratch
.\build.ps1 -NoTest              # skip the test run
.\build.ps1 -Target cad_core     # one target
```

`build.ps1` locates Visual Studio with `vswhere`, imports the MSVC environment
from `vcvars64.bat`, puts the portable Ninja on `PATH`, then configures and
builds. Output goes to `build/<Config>/`, with binaries in
`build/<Config>/bin/`.

## Running tests directly

```powershell
.\build\RelWithDebInfo\bin\cad_tests.exe                    # all
.\build\RelWithDebInfo\bin\cad_tests.exe -ts="Vec3"         # one suite
.\build\RelWithDebInfo\bin\cad_tests.exe -tc="*bulge*"      # matching cases
.\build\RelWithDebInfo\bin\cad_tests.exe --list-test-suites
```

## Warnings

Warnings are errors by default (`CAD_WARNINGS_AS_ERRORS=ON`). This is
deliberate: `/W4` with `/permissive-` catches narrowing conversions and
sign-mismatch bugs, which in a numerical kernel are exactly the ones that
produce geometry that is subtly wrong rather than obviously broken.

`/fp:precise` is set for the same reason — the optimiser must not reassociate
floating-point arithmetic, or intersection results would differ between Debug
and Release, which is a miserable class of bug to chase.

To disable temporarily while investigating:

```powershell
.\build.ps1 -Clean   # after setting -DCAD_WARNINGS_AS_ERRORS=OFF in CMakeLists
```

## Layout

```
src/core/     cad_core   — engine. No Qt, no graphics API, no OS UI.
src/io/       cad_io     — DXF/DWG interop. Depends on core only.
src/render/   cad_render — rendering abstraction + GPU backend.
src/abi/      cad_abi    — C-compatible plugin boundary + dynamic loader.
src/app/      cad_app    — Qt 6 shell.
examples/     plugins    — example plugins, built as part of the build.
tests/                   — doctest suite.
third_party/doctest/     — vendored single-header test framework.
prototype-ts/            — TypeScript prototype, reference only (ADR 0002).
```

Subdirectories are added to the build only once they exist, so the tree stays
buildable throughout the port.
