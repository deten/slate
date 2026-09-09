# Slate

Slate is a 2D CAD program for Windows. It opens and saves DWG and DXF files.

It is early. It draws lines, polylines, arcs, circles, ellipses, splines, hatches, text, blocks with attributes, dimensions and leaders. It has layers, layouts with viewports, and an undo history.

## Running it

Download the zip from the latest release, unzip it, and run `Slate.exe`.

DWG files need the free ODA File Converter. Download it from https://www.opendesign.com/guestfiles/oda_file_converter, install it, then copy its folder to `converters\ODAFileConverter` next to `Slate.exe`. DXF files work without it.

## Building it

Run `build.ps1` in PowerShell. It needs Visual Studio 2022, CMake, Ninja and Qt 6. See `docs/BUILDING.md` for details.

## Docs

- `docs/BUILDING.md` - building and packaging
- `docs/LAYOUTS.md` - sheets, viewports and page setup
- `docs/ATTRIBUTES.md` - block attributes
- `docs/REFERENCES.md` - other drawings, PDFs and pictures
- `docs/AI-ASSISTANT.md` - the AI assistant and the reference folder
- `docs/PROJECTS.md` - a set of sheets described beside its drawings
- `docs/SYSTEMS.md` - drawing the way the drawing is drawn: LIKE, and DUCT
- `docs/MACROS.md` - custom commands
- `docs/PLUGINS.md` - the plugin API

## Status

Version 0.2.0. Expect rough edges. Keep backups of drawings you care about.
