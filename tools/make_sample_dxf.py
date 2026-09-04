"""Generate a sample DXF for exercising the application by hand.

Produces a small floor-plan-like drawing with several layers, colours,
linetypes, a block definition with two references, text, and an arc — enough to
see that layers, blocks, colours, text and curve tessellation all render.

    python tools/make_sample_dxf.py samples/sample.dxf
"""

from __future__ import annotations

import sys
from pathlib import Path

Pair = tuple[int, object]


def dxf(pairs: list[Pair]) -> str:
    """Join group pairs into DXF text with CRLF endings, as other CAD software writes."""
    out: list[str] = []
    for code, value in pairs:
        out.append(str(code))
        out.append(str(value))
    return "\r\n".join(out) + "\r\n"


def line(handle: str, layer: str, x1: float, y1: float, x2: float, y2: float) -> list[Pair]:
    return [
        (0, "LINE"), (5, handle), (8, layer),
        (10, x1), (20, y1), (11, x2), (21, y2),
    ]


def rectangle(handle: str, layer: str, x: float, y: float, w: float, h: float) -> list[Pair]:
    """A closed LWPOLYLINE, which is how a real drawing stores a room outline."""
    return [
        (0, "LWPOLYLINE"), (5, handle), (8, layer), (90, 4), (70, 1),
        (10, x), (20, y),
        (10, x + w), (20, y),
        (10, x + w), (20, y + h),
        (10, x), (20, y + h),
    ]


def build() -> str:
    pairs: list[Pair] = []
    handle = 0x100

    def next_handle() -> str:
        nonlocal handle
        value = f"{handle:X}"
        handle += 1
        return value

    # --- header
    pairs += [
        (0, "SECTION"), (2, "HEADER"),
        (9, "$ACADVER"), (1, "AC1032"),
        (9, "$INSUNITS"), (70, 4),          # millimetres
        (9, "$LTSCALE"), (40, 25.0),
        (9, "$CLAYER"), (8, "Walls"),
        (0, "ENDSEC"),
    ]

    # --- tables
    pairs += [
        (0, "SECTION"), (2, "TABLES"),
        (0, "TABLE"), (2, "LTYPE"),
        (0, "LTYPE"), (5, "14"), (2, "Continuous"), (70, 0), (3, "Solid line"),
        (0, "LTYPE"), (5, "15"), (2, "Dashed"), (70, 0), (3, "__ __ __"),
        (49, 12.7), (49, -6.35),
        (0, "LTYPE"), (5, "16"), (2, "Center"), (70, 0), (3, "____ _ ____"),
        (49, 31.75), (49, -6.35), (49, 6.35), (49, -6.35),
        (0, "ENDTAB"),

        (0, "TABLE"), (2, "LAYER"),
        (0, "LAYER"), (5, "20"), (2, "0"), (70, 0), (62, 7), (6, "Continuous"),
        (0, "LAYER"), (5, "21"), (2, "Walls"), (70, 0), (62, 7), (6, "Continuous"), (370, 50),
        (0, "LAYER"), (5, "22"), (2, "Doors"), (70, 0), (62, 3), (6, "Continuous"),
        (0, "LAYER"), (5, "23"), (2, "Furniture"), (70, 0), (62, 4), (6, "Continuous"),
        (0, "LAYER"), (5, "24"), (2, "Grid"), (70, 0), (62, 1), (6, "Center"),
        (0, "LAYER"), (5, "25"), (2, "Hidden"), (70, 0), (62, 6), (6, "Dashed"),
        (0, "LAYER"), (5, "26"), (2, "Text"), (70, 0), (62, 2), (6, "Continuous"),
        (0, "LAYER"), (5, "27"), (2, "Dimensions"), (70, 0), (62, 5), (6, "Continuous"),
        (0, "LAYER"), (5, "28"), (2, "Hatching"), (70, 0), (62, 8), (6, "Continuous"),
        (0, "ENDTAB"),

        (0, "TABLE"), (2, "STYLE"),
        (0, "STYLE"), (5, "30"), (2, "Standard"), (70, 0),
        (40, 0.0), (41, 1.0), (3, "txt.shx"),
        (0, "ENDTAB"),

        # DIMSTYLE is the one table whose records use 105 for their handle
        # rather than 5. Written that way here on purpose, so the reader is
        # tested against the real quirk rather than against its own writer.
        (0, "TABLE"), (2, "DIMSTYLE"),
        (0, "DIMSTYLE"), (105, "31"), (2, "Standard"), (70, 0),
        (40, 10.0),        # DIMSCALE — a 1:10 drawing in millimetres
        (41, 2.5),         # DIMASZ, arrow size
        (42, 0.625),       # DIMEXO
        (44, 1.25),        # DIMEXE
        (140, 2.5),        # DIMTXT
        (147, 0.625),      # DIMGAP
        (271, 0),          # DIMDEC, whole millimetres
        (277, 2),          # DIMLUNIT, decimal
        (340, "30"),
        (0, "ENDTAB"),
        (0, "ENDSEC"),
    ]

    # --- a door block: a leaf and its swing arc
    pairs += [
        (0, "SECTION"), (2, "BLOCKS"),
        (0, "BLOCK"), (5, "40"), (2, "DOOR"), (70, 0), (10, 0), (20, 0), (3, "DOOR"),
        (0, "LINE"), (5, "41"), (8, "Doors"), (10, 0), (20, 0), (11, 0), (21, 900),
        (0, "ARC"), (5, "42"), (8, "Doors"),
        (10, 0), (20, 0), (40, 900), (50, 0), (51, 90),
        (0, "ENDBLK"), (5, "43"),
        (0, "ENDSEC"),
    ]

    # --- model space
    pairs += [(0, "SECTION"), (2, "ENTITIES")]

    # Outer wall, 8000 x 6000.
    pairs += rectangle(next_handle(), "Walls", 0, 0, 8000, 6000)
    # Inner wall face, giving the walls visible thickness.
    pairs += rectangle(next_handle(), "Walls", 200, 200, 7600, 5600)

    # A partition splitting the space.
    pairs += line(next_handle(), "Walls", 4000, 200, 4000, 3800)
    pairs += line(next_handle(), "Walls", 4200, 200, 4200, 3800)

    # Structural grid on a centre linetype, to show dash patterns.
    for i, x in enumerate((0, 2000, 4000, 6000, 8000)):
        pairs += line(next_handle(), "Grid", x, -600, x, 6600)
        pairs += [
            (0, "TEXT"), (5, next_handle()), (8, "Grid"),
            (10, x), (20, 6800), (40, 200), (1, chr(ord("A") + i)),
            (72, 1), (11, x), (21, 6800),
        ]
    for j, y in enumerate((0, 3000, 6000)):
        pairs += line(next_handle(), "Grid", -600, y, 8600, y)
        pairs += [
            (0, "TEXT"), (5, next_handle()), (8, "Grid"),
            (10, -900), (20, y), (40, 200), (1, str(j + 1)),
        ]

    # Something hidden, to prove the dashed linetype resolves through a layer.
    pairs += rectangle(next_handle(), "Hidden", 5000, 4200, 2400, 1200)

    # Furniture: a round table and desks.
    pairs += [
        (0, "CIRCLE"), (5, next_handle()), (8, "Furniture"),
        (10, 6000), (20, 2000), (40, 700),
    ]
    pairs += rectangle(next_handle(), "Furniture", 500, 4600, 1600, 700)
    pairs += rectangle(next_handle(), "Furniture", 500, 3600, 1600, 700)

    # An ellipse, to exercise the numeric curve path.
    pairs += [
        (0, "ELLIPSE"), (5, next_handle()), (8, "Furniture"),
        (10, 2400), (20, 1400), (11, 900), (21, 0), (31, 0),
        (40, 0.55), (41, 0.0), (42, 6.283185307179586),
    ]

    # Two door references, one rotated, to show block instancing.
    pairs += [
        (0, "INSERT"), (5, next_handle()), (8, "Doors"), (2, "DOOR"),
        (10, 4100), (20, 3800), (41, 1), (42, 1), (43, 1), (50, 270),
    ]
    pairs += [
        (0, "INSERT"), (5, next_handle()), (8, "Doors"), (2, "DOOR"),
        (10, 200), (20, 2400), (41, 1), (42, 1), (43, 1), (50, 0),
    ]

    # Room labels.
    for label, x, y in (("OFFICE 101", 1800, 1200), ("MEETING 102", 6000, 3200),
                        ("STORE 103", 6000, 4800)):
        pairs += [
            (0, "TEXT"), (5, next_handle()), (8, "Text"),
            (10, x), (20, y), (40, 250), (1, label),
        ]

    # A multi-line note, to exercise MTEXT.
    pairs += [
        (0, "MTEXT"), (5, next_handle()), (8, "Text"),
        (10, 200), (20, -900), (40, 200), (41, 6000), (71, 1),
        (1, "GENERAL NOTES:\\PAll dimensions in millimetres.\\P"
            "Do not scale from this drawing."),
        (7, "Standard"),
    ]

    # A survey point, for the node snap.
    pairs += [
        (0, "POINT"), (5, next_handle()), (8, "0"), (10, 0), (20, 0),
    ]

    # Dimensions. Written without the anonymous block other CAD software normally
    # generates, which is the harder case: the reader has to produce the
    # geometry from the definition points alone.
    #
    # Overall width, as a rotated linear dimension.
    pairs += [
        (0, "DIMENSION"), (5, next_handle()), (8, "Dimensions"),
        (100, "AcDbDimension"),
        (10, 0), (20, -1400),        # dimension line location
        (11, 4000), (21, -1250),     # text midpoint
        (70, 32),                    # rotated
        (71, 5), (72, 1),
        (3, "Standard"),
        (100, "AcDbAlignedDimension"),
        (13, 0), (23, 0),
        (14, 8000), (24, 0),
        (50, 0.0),
        (100, "AcDbRotatedDimension"),
    ]
    # Overall height, measured vertically.
    pairs += [
        (0, "DIMENSION"), (5, next_handle()), (8, "Dimensions"),
        (100, "AcDbDimension"),
        (10, -1600), (20, 0),
        (11, -1450), (21, 3000),
        (70, 32),
        (71, 5), (72, 1),
        (3, "Standard"),
        (100, "AcDbAlignedDimension"),
        (13, 0), (23, 0),
        (14, 0), (24, 6000),
        (50, 90.0),
        (100, "AcDbRotatedDimension"),
    ]
    # The table's radius.
    pairs += [
        (0, "DIMENSION"), (5, next_handle()), (8, "Dimensions"),
        (100, "AcDbDimension"),
        (10, 6000), (20, 2000),      # centre
        (11, 7200), (21, 2600),      # text
        (70, 36),                    # radius, block-referenced
        (71, 5), (72, 1),
        (3, "Standard"),
        (100, "AcDbRadialDimension"),
        (15, 6495), (25, 2495),      # a point on the circumference
        (40, 400.0),
    ]

    # A hatch on the store room, with a polyline boundary and a real pattern
    # definition — the form a writer uses when it cannot assume the reader has
    # a pattern library.
    pairs += [
        (0, "HATCH"), (5, next_handle()), (8, "Hatching"),
        (100, "AcDbEntity"), (100, "AcDbHatch"),
        (10, 0), (20, 0), (30, 0),
        (210, 0), (220, 0), (230, 1),
        (2, "ANSI31"),
        (70, 0),                     # not solid
        (71, 0),                     # not associative
        (91, 1),                     # one boundary path
        (92, 3),                     # external | polyline
        (72, 0),                     # no bulges
        (73, 1),                     # closed
        (93, 4),                     # four vertices
        (10, 5000), (20, 4200),
        (10, 7400), (20, 4200),
        (10, 7400), (20, 5400),
        (10, 5000), (20, 5400),
        (97, 0),                     # no source objects
        (75, 0),                     # normal island style
        (76, 1),
        (52, 0.0),                   # pattern angle
        (41, 20.0),                  # pattern scale
        (77, 0),                     # not double
        (78, 1),                     # one pattern line
        (53, 45.0), (43, 0.0), (44, 0.0), (45, 0.0), (46, 3.175), (79, 0),
        (47, 1.0),
        (98, 1), (10, 6200), (20, 4800),
    ]

    # A solid fill, to exercise the other path through the renderer.
    pairs += [
        (0, "HATCH"), (5, next_handle()), (8, "Hatching"),
        (100, "AcDbEntity"), (100, "AcDbHatch"),
        (10, 0), (20, 0), (30, 0),
        (210, 0), (220, 0), (230, 1),
        (2, "SOLID"),
        (70, 1),
        (71, 0),
        (91, 1),
        (92, 3),
        (72, 0), (73, 1), (93, 4),
        (10, 500), (20, 3600),
        (10, 2100), (20, 3600),
        (10, 2100), (20, 4300),
        (10, 500), (20, 4300),
        (97, 0),
        (75, 0), (76, 1),
        (47, 1.0),
        (98, 0),
    ]

    pairs += [(0, "ENDSEC"), (0, "EOF")]
    return dxf(pairs)


def main() -> int:
    target = Path(sys.argv[1] if len(sys.argv) > 1 else "samples/sample.dxf")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(build(), encoding="utf-8", newline="")
    print(f"wrote {target} ({target.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
