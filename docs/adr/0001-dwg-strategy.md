# ADR 0001 — DWG and DXF compatibility strategy

**Status:** Accepted
**Date:** 2026-08-11

## Context

The product requires opening real professional DWG drawings accurately, including
xrefs, and the brief explicitly forbids inventing a DWG parser.

DWG is a closed, undocumented, version-churning binary format. Writing a reader
from scratch is a multi-year effort that would still trail every other CAD software release.
DXF, by contrast, is fully documented by Autodesk and is a first-class interchange
format that every CAD tool emits.

## Options considered

| Option | License | Coverage | Verdict |
|---|---|---|---|
| **ODA Drawings SDK** | Commercial (membership + royalties) | Full DWG read/write, all versions | Best fidelity; cost and a native addon are premature now |
| **ODA File Converter** | Free, no account or license key required | DWG ↔ DXF, R12–current, CLI, batch, Win/macOS/Linux | **Chosen** for DWG ingest |
| **LibreDWG** | **GPL-3.0** | r13–r2018 read | **Rejected** — GPL is viral and incompatible with shipping a commercially licensed product |
| **Autodesk RealDWG** | Commercial, restrictive | Full | Licensing terms too restrictive |
| npm `dxf-parser` etc. | MIT | Partial DXF only | Insufficient; no structure preservation |
| Hand-written DWG parser | — | — | **Forbidden by the brief, and correctly so** |

## Decision

A three-layer strategy:

1. **DXF is implemented natively in TypeScript.** It is documented, it is the
   format the ODA converter produces, and owning the reader is what makes
   structure preservation (XData, unknown group codes, handles) possible. A
   third-party parser that discards what it does not understand would defeat the
   fidelity requirement.

2. **DWG is read through a pluggable `DwgTranslator` interface.** The first
   implementation shells out to the free ODA File Converter to produce a DXF,
   which the native reader then consumes. The interface — not the converter — is
   the architectural commitment.

3. **The interface leaves room for a direct ODA Drawings SDK binding** as a
   native addon later, for direct read/write without a temp file and for DWG
   *writing*. Swapping it in must not touch any calling code.

## Consequences

- No GPL code enters the product.
- DWG support depends on an external binary being installed. This must be a
  first-class, well-explained setup step in the UI with a clear diagnostic when
  it is absent — not an opaque failure.
- DWG *writing* is out of scope until an SDK binding exists. The native format
  and DXF export cover saving in the meantime, and the original file is never
  overwritten with a lossy round-trip.
- Because everything funnels through DXF today, the DXF reader's fidelity *is*
  the product's DWG fidelity. It gets the corresponding investment in tests.

## What the first real drawing taught us

*Added 2026-08-12, after opening a 900 KB AC1032 (other CAD software) floor plan:
7,915 objects, 60 layers, 149 blocks, 2,030 model-space entities.*

The strategy held — the drawing opened in under two seconds with no
diagnostics. The interesting part is what it found, none of which the synthetic
test corpus could have:

1. **Block records are not blocks.** The `BLOCK` entity in the BLOCKS section
   has its own handle; the `BLOCK_RECORD` it belongs to has a *different* one,
   carried as the BLOCK's owner (code 330). Everything that references a block
   — layouts, dimension style arrowheads, every contained entity's owner link —
   names the record's handle. Keying by the BLOCK's own handle looks right on
   hand-written files, where the two often coincide, and dangles on every
   drawing other CAD software wrote.

2. **Our DXF output was not readable by ODA.** Discovered by converting our own
   file *back* the other way. Three defects, each fatal on its own: no
   `BLOCK_RECORD` table at all, no `APPID` table for the applications named by
   XData, and no handles or subclass markers on the `TABLE` headers. Plus a
   `$HANDSEED` equal to a handle the file itself used — the header is now
   written after the body so the seed can exceed every handle actually
   allocated.

3. **Hatch patterns with a staggered offset drew nothing.** The x component of
   a pattern line's offset slides successive lines *along themselves*, which is
   what makes a brick pattern stagger. A line is unchanged by that, but the
   finite ray used to find boundary crossings was anchored to the shifted
   origin, so it walked further from the region with every line. ANSI31, which
   the tests used, has a zero x offset and never caught it; the drawing's
   custom wall-poché patterns did immediately.

The lesson worth keeping: **the converter is a two-way oracle.** Using it only
to read DWG exercises our reader. Running our *writer's* output back through it
validates our DXF against a mature independent implementation, and that is
where three of the four defects above came from. There is now a test that does
exactly this round trip whenever a converter is present, and reports rather
than silently skipping when one is not.

A fourth finding was not a defect but a gap: real dimensions routinely carry
per-entity style overrides in `ACAD`/`DSTYLE` XData. They were preserved
verbatim but not *applied*, so a dimension with an overridden arrow size drew at
its style's size. Three of the nine dimensions in this drawing carry five
overrides each, which is a fair measure of how ordinary they are. Now applied:
`Database::effectiveDimStyle` layers a dimension's own overrides over its named
style, and a variable this application does not model is skipped rather than
guessed at — the XData is never modified, so it still round-trips.
