# ADR 0006 — Attached content is placed by the engine and decoded by the backend

**Status:** Accepted
**Date:** 2026-08-18

## Context

Real drawings display things that are not geometry. The sample drawing this
project is built against contains all three kinds:

- an **IMAGE**, a 7177 × 4833 scan of a hand sketch, attached from a PNG on a
  drive that no longer exists;
- an **OLE2FRAME**, 3.8 megabytes of embedded OLE compound document, pasted from
  some Windows application years ago;
- a **PDFDEFINITION** naming a PDF the drawing was traced over — its placement
  since deleted, the definition left behind.

Until now all three were read as `UnsupportedEntity`. That preserved them
faithfully on save and drew nothing, which produced the worst behaviour
available: a rectangle of empty space that the user could neither see nor
click, on a sheet that plainly showed content in other CAD software. It is how this work
started — somebody asked what the invisible thing in the middle of their sheet
was, and there was no way to find out from inside the application.

Drawing them properly means decoding a PNG, rasterising a PDF page, and asking
Windows to render an OLE object. `cad_core` links nothing but the C++ standard
library (ADR 0002), so it can do none of that, and it should not start.

## Decision

**The engine models where attached content is and what it should look like. A
backend that owns a decoder supplies what it contains. Neither knows the
other's job.**

Concretely:

- `RasterImageEntity`, `UnderlayEntity` and `OleFrameEntity` live in `cad_core`
  and carry placement, clipping and display settings — origin and pixel vectors,
  clip boundary, brightness, contrast, fade. No pixels.
- `ImageDefinition` and `UnderlayDefinition` carry the *stored* path, which is
  never rewritten, plus a resolved absolute path filled in at load time by
  `resolveAttachedFiles` using the same `ResourceResolver` xrefs use.
- `cad_render` emits a `DrawItem::Kind::Image` and a `SceneImage` payload
  holding a corner and two edge vectors in world coordinates. It resolves the
  raster's top-to-bottom flip *once*, so no backend rediscovers it.
- `cad_app` decodes through `ImageCache`, which scales during decode and evicts
  by least-recent use, and draws with a `QTransform` built from the three
  corners.

**Content that cannot be drawn is drawn as a dashed, captioned frame.** Not as
nothing, and not as a solid rectangle.

## Why a frame rather than nothing

Nothing is what it did before, and nothing is a lie by omission: the entity is
in the drawing, it is on a layer, it plots, it has extents, and the user cannot
tell any of that. Dashed rather than solid because a solid rectangle is
indistinguishable from a rectangle somebody drew, and the one thing the user
needs to know is that this is a boundary around content they are not seeing.
Captioned because the caption names the file, and the file name is the only part
of the problem the user can act on.

The caption is deliberately subordinate — a third of the frame's width, a tenth
of its height, and dropped entirely below seven pixels. A caption that fills the
box it is describing has stopped being a label.

## Why PDF underlays are placed but not rasterised

Rasterising a PDF needs a PDF engine. Qt has one — `QtPdf` — and it is not in
this build's Qt. Adding it is a defensible future change and this design
accommodates it without moving anything: `UnderlayEntity::pageSize` exists for a
backend that can open the file to fill in, and everything falls back to the clip
boundary until something does.

What is *not* deferred is the rest of it. An underlay is on a layer, it is
selected, moved, clipped and plotted, and it round-trips. Those are the parts
that make it a drawing object rather than a picture, and they do not depend on
anybody being able to draw a PDF.

## Why OLE2FRAME interprets almost nothing

Two corners, and everything else preserved verbatim.

The published group codes for OLE2FRAME disagree with what applications write —
the sample drawing carries its object type in code 73 where the reference says
71 — so the only honest reading of the rest is no reading. Labelling a frame
"embedded" because a code happened to be 2 is a guess wearing the clothes of a
fact. The corners are unambiguous, and they are what the frame needs.

The 3.8-megabyte payload is written back byte for byte. An OLE object that
vanished on save would take somebody's schedule table with it.

## Consequences

- Attached images render, clip, fade and adjust for brightness and contrast, and
  a rotated or mirrored placement comes out right because the entity stores edge
  vectors rather than an angle.
- A missing file is reported by path, per placement, at load — in the command
  history and from `dwg_info --attachments`. A user who has moved a folder can
  see exactly what to put back.
- An image inside a block reference is transformed with the block, because the
  scene applies the block transform to the payload's vectors like any other
  geometry.
- `cad_core` still links only the standard library.
- A PDF underlay shows its outline and not its page. This is the known gap, and
  it is a gap in one function rather than in the design.
