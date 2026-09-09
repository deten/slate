"""Drive the application through interaction scenarios and check what it drew.

The unit tests cover cad_core, cad_io and cad_render. Nothing covers cad_app:
there is no Qt test target, so picking, grips, dimensional input and object snap
tracking -- everything the user actually touches -- had no automated check at
all. Every bug found in that layer so far was found by hand.

This is that missing layer. Each scenario runs the real application headlessly
through a scripted sequence of clicks and keystrokes, saves the result, and
asserts on the geometry that came out. Asserting on the *file* rather than on
the screen is what makes these worth having: a pixel comparison would fail on
every theme change, while "the endpoint is at exactly (266.506351, 175.0)" is
the actual claim.

    python tools/interaction_tests.py
    python tools/interaction_tests.py --only grip
    python tools/interaction_tests.py --keep      # leave the DXFs to inspect

Every scenario here was a bug at some point, or is a guard on one that was.
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "RelWithDebInfo" / "bin" / "Slate.exe"

# Window size every scenario runs at.
SIZE = "1500x950"

# And the size of the drawing area inside it, which is the one that matters.
#
# Every coordinate in these scenarios is a point on the canvas, and what a point
# on the canvas means depends on how big the canvas is -- ZOOMEXTENTS fits the
# drawing to it. Pinned to the canvas rather than to the window because the
# chrome above the drawing is not fixed: when the menu bar, the file buttons and
# the ribbon's tabs were merged into one row, the canvas grew forty-four pixels
# and seventeen scenarios started clicking somewhere other than where they had
# been aimed. The numbers themselves are arbitrary -- they are what the window
# happened to give before -- but they are now stated rather than inherited.
CANVAS = "1238x686"


class Failure(Exception):
    """A scenario's expectation did not hold."""


# --------------------------------------------------------------- DXF reading


def records(path: Path, kind: str) -> list[dict[str, str]]:
    """Every record of one type, as code -> first value.

    A deliberately small reader that shares nothing with the application, so a
    bug in the writer cannot hide behind the same bug in the reader.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = [line.strip() for line in text.replace("\r\n", "\n").split("\n")]
    found: list[dict[str, str]] = []
    i = 0
    while i < len(lines) - 1:
        if lines[i] == "0" and lines[i + 1] == kind:
            entry: dict[str, str] = {}
            j = i + 2
            while j < len(lines) - 1 and lines[j] != "0":
                entry.setdefault(lines[j], lines[j + 1])
                j += 2
            found.append(entry)
            i = j
        else:
            i += 1
    return found


def lines_in(path: Path) -> list[tuple[float, float, float, float]]:
    out = []
    for entry in records(path, "LINE"):
        out.append((float(entry["10"]), float(entry["20"]),
                    float(entry["11"]), float(entry["21"])))
    return out


def close(actual: float, expected: float, tolerance: float = 1e-6) -> bool:
    return abs(actual - expected) <= tolerance


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ------------------------------------------------------------------- running


def run(work: Path, name: str, script: list[str], save: bool = True) -> tuple[str, Path]:
    """Run one scripted session; return its log and the file it saved."""
    output = work / f"{name}.dxf"
    args = [str(EXE), "-platform", "offscreen", "--no-recovery-prompt", "--size", SIZE,
            "--canvas", CANVAS]
    args += script
    if save:
        args += ["--save-as", str(output)]
    completed = subprocess.run(args, capture_output=True, text=True, timeout=180,
                               cwd=str(ROOT))
    log = completed.stdout + completed.stderr
    if save and not output.exists():
        raise Failure(f"saved nothing.\n{log}")
    return log, output


# ----------------------------------------------------------------- scenarios
#
# Two lines, 400 long, at y=0 and y=100. Zoomed to extents in a canvas of the
# size above they land on canvas rows 483 and 202, spanning x 56 to 1181. Those
# numbers are used throughout and are worth stating once.

TWO_LINES = ["--run", "LINE", "--run", "0,0", "--run", "400,0", "--run", "",
             "--run", "LINE", "--run", "0,100", "--run", "400,100", "--run", "",
             "--run", "ZOOMEXTENTS"]


def test_click_picks_what_is_under_it(work: Path) -> None:
    """ERASE removes the line clicked, not one merely hovered beforehand.

    The rollover is what a click resolves to -- that is how Shift+Space cycling
    survives the click -- and it was not being refreshed while a command was
    running. Hovering one object then erasing another deleted the first.
    """
    _, saved = run(work, "stale-hover", TWO_LINES + [
        "--hover", "438,483",      # rollover settles on the y=0 line
        "--run", "ERASE",
        "--click", "438,202",      # but this is the one being erased
        "--run", "",
    ])
    remaining = lines_in(saved)
    expect(len(remaining) == 1, f"expected one line left, got {len(remaining)}")
    expect(close(remaining[0][1], 0.0),
           f"erased the hovered line instead of the clicked one: {remaining}")


def test_dimension_length_and_angle(work: Path) -> None:
    """250, Tab, 30, Enter puts the point 250 away at 30 degrees, exactly."""
    _, saved = run(work, "dim-typed", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS", "--click", "438,313",
        "--run", "LINE", "--run", "50,50", "--hover", "738,313",
        "--type", "250", "--key", "tab", "--type", "30", "--key", "return",
        "--run", "",
    ])
    drawn = [line for line in lines_in(saved) if close(line[0], 50.0) and close(line[1], 50.0)]
    expect(len(drawn) == 1, f"expected one line from (50,50), got {drawn}")
    _, _, x, y = drawn[0]
    expect(close(x, 50 + 250 * math.cos(math.radians(30)), 1e-6), f"x was {x}")
    expect(close(y, 50 + 250 * math.sin(math.radians(30)), 1e-6), f"y was {y}")


def test_dimension_lock_holds_through_a_click(work: Path) -> None:
    """A locked length stays exact while the pointer chooses the direction."""
    _, saved = run(work, "dim-locked", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS", "--click", "438,313",
        "--run", "LINE", "--run", "50,50", "--hover", "738,313",
        "--type", "250", "--key", "tab",
        "--hover", "838,113", "--click", "838,113", "--run", "",
    ])
    drawn = [line for line in lines_in(saved) if close(line[0], 50.0) and close(line[1], 50.0)]
    expect(len(drawn) == 1, f"expected one line from (50,50), got {drawn}")
    x0, y0, x1, y1 = drawn[0]
    expect(close(math.hypot(x1 - x0, y1 - y0), 250.0, 1e-6),
           f"length was {math.hypot(x1 - x0, y1 - y0)}")


def test_dimension_lock_does_not_survive_a_cancel(work: Path) -> None:
    """Escaping a command lets the locked length go.

    A lock that outlived its command silently held the next point anywhere in
    the drawing at the old length.
    """
    _, saved = run(work, "dim-leak", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS", "--click", "438,313",
        "--run", "LINE", "--run", "50,50", "--hover", "738,313",
        "--type", "250", "--key", "tab", "--key", "escape",
        "--run", "LINE", "--run", "0,0", "--click", "738,313", "--run", "",
    ])
    drawn = [line for line in lines_in(saved) if close(line[0], 0.0) and close(line[1], 0.0)]
    expect(len(drawn) == 1, f"expected one line from the origin, got {drawn}")
    x0, y0, x1, y1 = drawn[0]
    expect(not close(math.hypot(x1 - x0, y1 - y0), 250.0, 1e-3),
           "the abandoned lock was still holding the next point at 250")


def test_hot_grips_move_together(work: Path) -> None:
    """Grips marked with Shift move by the same displacement, not to a point."""
    _, saved = run(work, "hot-grips", TWO_LINES + [
        "--click", "438,202", "--click", "438,483",
        # Both ends marked, then one of the marked ones taken hold of. Grabbing
        # an unmarked grip starts over, which is other CAD software's rule.
        "--shift-click", "1181,202", "--shift-click", "1181,483",
        "--click", "1181,483", "--hover", "1181,343", "--click", "1181,343",
    ])
    ends = sorted((line[1], line[3]) for line in lines_in(saved))
    expect(len(ends) == 2, f"expected two lines, got {len(ends)}")
    # Both far ends rose by the same amount; the near ends did not move.
    moved = [end - start for start, end in ends]
    expect(close(moved[0], moved[1], 1e-9),
           f"the two grips moved by different amounts: {moved}")
    expect(moved[0] > 1.0, f"the grips did not move: {moved}")


def test_polyline_midpoint_grip_adds_a_vertex(work: Path) -> None:
    """Dragging the middle of a straight segment puts a corner there."""
    _, saved = run(work, "poly-vertex", [
        "--run", "PLINE", "--run", "0,0", "--run", "400,0", "--run", "400,300",
        "--run", "", "--run", "ZOOMEXTENTS",
        "--click", "438,651", "--click", "619,651", "--click", "619,513",
    ])
    polylines = records(saved, "LWPOLYLINE")
    expect(len(polylines) == 1, f"expected one polyline, got {len(polylines)}")
    expect(polylines[0]["90"] == "4",
           f"expected four vertices after the drag, got {polylines[0]['90']}")


def test_snap_tracking_finds_the_crossing(work: Path) -> None:
    """Two acquired points hold a corner where no geometry exists."""
    _, saved = run(work, "tracking", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "CIRCLE", "--run", "600,100", "--run", "50",
        "--run", "ZOOMEXTENTS", "--click", "438,313", "--key", "F11",
        "--run", "LINE",
        "--hover", "748,83", "--hover", "1094,429", "--hover", "1098,87",
        "--click", "1098,87", "--run", "1400,100", "--run", "",
    ])
    starts = [(line[0], line[1]) for line in lines_in(saved)]
    expect(any(close(x, 600.0, 1e-6) and close(y, 300.0, 1e-6) for x, y in starts),
           f"no line starts at the crossing of the two paths: {starts}")


def test_array_rectangular(work: Path) -> None:
    # Arrays the y=0 line of the standard pair, which is the one on row 670.
    # A click point needs geometry whose screen position is known, and that is
    # the whole reason the pair is a fixture.
    _, saved = run(work, "array-rect", TWO_LINES + [
        "--run", "ARRAY", "--click", "438,483", "--run", "",
        # 130 between rows, not 100: at 100 the array puts a copy exactly on top
        # of the y=100 line that was never selected, and no filter can then tell
        # the two apart.
        "--run", "R", "--run", "3", "--run", "4", "--run", "130", "--run", "150",
    ])
    # The y=100 line was never selected, so it is not part of the array.
    copies = [line for line in lines_in(saved)
              if not (close(line[0], 0.0) and close(line[1], 100.0)
                      and close(line[2], 400.0))]
    expect(len(copies) == 12, f"expected twelve lines from the array, got {len(copies)}")
    expect(sorted({line[0] for line in copies}) == [0.0, 150.0, 300.0, 450.0],
           f"columns are wrong: {sorted({line[0] for line in copies})}")
    expect(sorted({line[1] for line in copies}) == [0.0, 130.0, 260.0],
           f"rows are wrong: {sorted({line[1] for line in copies})}")


def test_array_polar(work: Path) -> None:
    _, saved = run(work, "array-polar", TWO_LINES + [
        "--run", "ARRAY", "--click", "438,483", "--run", "",
        "--run", "P", "--run", "0,0", "--run", "8",
    ])
    copies = [line for line in lines_in(saved)
              if not (close(line[0], 0.0) and close(line[1], 100.0)
                      and close(line[2], 400.0))]
    expect(len(copies) == 8, f"expected eight lines, got {len(copies)}")
    # Every copy is a rotation about the origin of a line that starts there, so
    # it is their *far* ends that sit evenly around the circle.
    angles = sorted(round(math.degrees(math.atan2(y, x)) % 360, 6)
                    for _, _, x, y in copies)
    expect(angles == [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0],
           f"items are not evenly spaced: {angles}")
    radii = {round(math.hypot(x, y), 6) for _, _, x, y in copies}
    expect(radii == {400.0}, f"items are not all at the same radius: {radii}")


def test_block_is_picked_by_its_geometry(work: Path) -> None:
    """Clicking the empty middle of a block selects nothing.

    A reference knows its extents and not its contents, so left to answer for
    itself it claims every point inside its bounding box. On a plan full of
    furniture that means clicking bare floor grabs a symbol.
    """
    source = work / "block.dxf"
    write_corner_block(source)
    _, saved = run(work, "block-pick", [
        "--open", str(source), "--run", "ZOOMEXTENTS",
        "--run", "ERASE", "--click", "488,288", "--run", "",
    ])
    # Asserted against the file rather than the log: what matters is that the
    # reference survived, and log wording is not a contract.
    expect(len(records(saved, "INSERT")) == 1,
           "a click in the empty middle of a block selected and erased it")


def test_distance_reports_separation_and_angle(work: Path) -> None:
    """A 3-4-5 triangle: 500 long, 53.13 degrees, 300 across and 400 up.

    These commands draw nothing, so the log is the whole output and asserting
    on it is asserting on the result. The numbers are what is checked, not the
    wording around them.
    """
    log, _ = run(work, "dist", ["--run", "DIST", "--run", "0,0", "--run", "300,400"],
                 save=False)
    expect("Distance = 500" in log, f"distance was not 500: {log}")
    expect("53.1301" in log, f"angle was not 53.1301 degrees: {log}")
    expect("Delta X = 300" in log and "Delta Y = 400" in log, f"deltas are wrong: {log}")


def test_area_of_a_closed_object(work: Path) -> None:
    """A 400 by 300 rectangle is 120000 with a perimeter of 1400."""
    log, _ = run(work, "area-object", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300", "--run", "ZOOMEXTENTS",
        "--run", "AREA", "--run", "O", "--click", "438,32",
    ], save=False)
    expect("Area = 120000" in log, f"area was not 120000: {log}")
    expect("Perimeter = 1400" in log, f"perimeter was not 1400: {log}")


def test_area_of_an_open_object_reports_length(work: Path) -> None:
    """An open shape has no area, and saying so beats inventing one.

    Closing it silently would answer a question nobody asked, with a number
    that looks just as authoritative as a real one.
    """
    log, _ = run(work, "area-open", [
        "--run", "PLINE", "--run", "0,0", "--run", "400,0", "--run", "400,300",
        "--run", "", "--run", "ZOOMEXTENTS",
        "--run", "AREA", "--run", "O", "--click", "619,651",
    ], save=False)
    expect("Length = 700" in log, f"length was not 700: {log}")
    expect("Area" not in log, f"an area was reported for an open shape: {log}")


def test_describe_reads_a_labelled_room(work: Path) -> None:
    """A 4 x 3 metre rectangle with OFFICE and 101 inside reads as one room.

    DESCRIBE is what the assistant is told about the drawing, printed for the
    person; the room's name, number and traced walls are the whole point.
    """
    log, _ = run(work, "describe", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "4000,3000",
        "--run", "TEXT", "--run", "1500,1600", "--run", "100", "--run", "0",
        "--run", "OFFICE", "--run", "",
        "--run", "TEXT", "--run", "1500,1400", "--run", "100", "--run", "0",
        "--run", "101", "--run", "",
        "--run", "DESCRIBE",
    ], save=False)
    expect("Rooms (1, 1 with walls traced)" in log, f"the room was not traced: {log}")
    expect("OFFICE 101: 12 sq m, 4000 x 3000, centre 2000,1500" in log,
           f"the room was not read: {log}")


def polyline_vertices(path: Path) -> list[tuple[float, float]]:
    """Vertices of the first polyline, in file order."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = [line.strip() for line in text.replace("\r\n", "\n").split("\n")]
    xs: list[float] = []
    ys: list[float] = []
    i = 0
    while i < len(lines) - 1:
        if lines[i] == "0" and lines[i + 1] == "LWPOLYLINE":
            j = i + 2
            while j < len(lines) - 1 and lines[j] != "0":
                if lines[j] == "10":
                    xs.append(round(float(lines[j + 1]), 3))
                elif lines[j] == "20":
                    ys.append(round(float(lines[j + 1]), 3))
                j += 2
            return list(zip(xs, ys))
        i += 1
    return []


def circle_in(path: Path) -> tuple[float, float, float]:
    found = records(path, "CIRCLE")
    expect(len(found) == 1, f"expected one circle, got {len(found)}")
    return float(found[0]["10"]), float(found[0]["20"]), float(found[0]["40"])


# A 400 by 300 rectangle. Zoomed to extents in the standard window its corners
# land on (466, 840), (1295, 840), (1295, 219) and (466, 219).
RECTANGLE = ["--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
             "--run", "ZOOMEXTENTS"]


def test_two_hot_grips_on_one_object_stretch_together(work: Path) -> None:
    """Both marked corners move, and the shape stays a shape.

    Applied per grip rather than per object, this previewed as a bow tie: two
    ghosts, each with one corner moved and the other still at home.
    """
    _, saved = run(work, "shape-rect-stretch", RECTANGLE + [
        "--click", "438,653",          # the rectangle
        "--shift-click", "204,653",    # mark both bottom corners
        "--shift-click", "1033,653",
        "--click", "1033,653",         # then drag one of them
        "--hover", "1033,513", "--click", "1033,513",
    ])
    vertices = polyline_vertices(saved)
    expect(len(vertices) == 4, f"expected four corners, got {vertices}")
    heights = sorted({y for _, y in vertices})
    expect(len(heights) == 2, f"the two bottom corners did not end up level: {vertices}")
    expect(close(heights[1], 300.0), f"the top edge moved: {vertices}")
    expect(heights[0] > 50.0, f"the bottom edge did not rise: {vertices}")


def test_two_hot_grips_move_the_object_once(work: Path) -> None:
    """Move, Rotate, Scale and Mirror act on the object, not on each grip.

    Run once per hot grip, two marked grips moved the object exactly twice as
    far as it was dragged, and three would have tripled it.
    """
    _, saved = run(work, "shape-rect-move", RECTANGLE + [
        "--click", "438,653", "--shift-click", "204,653", "--shift-click", "1033,653",
        "--click", "1033,653",
        "--key", "Space",              # Stretch -> Move
        "--hover", "1033,513", "--click", "1033,513",
    ])
    vertices = polyline_vertices(saved)
    expect(len(vertices) == 4, f"expected four corners, got {vertices}")
    heights = sorted({y for _, y in vertices})
    # One drag of that distance is about 68 units; two would be about 136.
    expect(close(heights[0], 67.982, 0.5), f"the object did not move once: {vertices}")
    expect(close(heights[1] - heights[0], 300.0, 0.01),
           f"a move should not change the size: {vertices}")


def test_two_midpoint_grips_each_add_a_corner(work: Path) -> None:
    """Two hot midpoint grips on one polyline add two corners, not one.

    A midpoint grip inserts a vertex, which renumbers the grips after it *and*
    moves the boundary between vertex indices and midpoint indices. Read by
    index, the second grip landed in the vertex range and overwrote a corner
    instead of adding one.
    """
    _, saved = run(work, "shape-poly-midpoints", [
        "--run", "PLINE", "--run", "0,0", "--run", "400,0", "--run", "400,300",
        "--run", "", "--run", "ZOOMEXTENTS",
        "--click", "438,651",
        "--shift-click", "619,651",    # both leg middles marked
        "--shift-click", "1033,341",
        "--click", "1033,341",         # then one of them dragged
        "--hover", "1083,291", "--click", "1083,291",
    ])
    vertices = polyline_vertices(saved)
    expect(len(vertices) == 5, f"expected five vertices, got {vertices}")

    # The *order* is the assertion, not the set of points.
    #
    # Read by a stale index the second grip inserts into the wrong segment, and
    # the result contains exactly the same five positions -- the corners all
    # survive and both midpoints move by the right displacement -- but strung
    # together in an order that makes the polyline zig-zag back on itself.
    # Anything order-independent passes on it.
    displacement = (vertices[1][0] - 200.0, vertices[1][1] - 0.0)
    expected = [(0.0, 0.0),
                (200.0 + displacement[0], 0.0 + displacement[1]),
                (400.0, 0.0),
                (400.0 + displacement[0], 150.0 + displacement[1]),
                (400.0, 300.0)]
    for index, (want, got) in enumerate(zip(expected, vertices)):
        expect(close(got[0], want[0], 1e-3) and close(got[1], want[1], 1e-3),
               f"vertex {index} is {got}, expected {want}: {vertices}")
    expect(abs(displacement[0]) > 1.0 or abs(displacement[1]) > 1.0,
           f"the midpoints did not move: {vertices}")


def test_circle_dragged_by_its_centre_keeps_its_radius(work: Path) -> None:
    """A whole-object grip is applied before the ones measured against it.

    A quadrant grip sets the radius *from the centre*. With both hot and the
    centre applied second, the quadrant had already re-measured against the old
    centre, and a circle dragged by its middle came out smaller.
    """
    _, saved = run(work, "shape-circle-centre", [
        "--run", "CIRCLE", "--run", "0,0", "--run", "100", "--run", "ZOOMEXTENTS",
        "--click", "308,342",          # the circle, by its west quadrant
        "--shift-click", "308,342",    # mark the quadrant and the centre
        "--shift-click", "618,342",
        "--click", "618,342",          # then drag the centre
        "--hover", "668,292", "--click", "668,292",
    ])
    x, y, radius = circle_in(saved)
    expect(close(radius, 100.0, 1e-6), f"the radius changed to {radius}")
    expect(math.hypot(x, y) > 10.0, f"the circle did not move: centre ({x}, {y})")


def test_circle_stretched_by_a_quadrant_keeps_its_centre(work: Path) -> None:
    """Two opposite quadrants hot: the radius changes, the centre does not."""
    _, saved = run(work, "shape-circle-quadrants", [
        "--run", "CIRCLE", "--run", "0,0", "--run", "100", "--run", "ZOOMEXTENTS",
        "--click", "308,342",
        "--shift-click", "929,342",   # mark both quadrants
        "--shift-click", "308,342",
        "--click", "308,342",          # then drag the west one outward
        "--hover", "258,342", "--click", "258,342",
    ])
    x, y, radius = circle_in(saved)
    expect(close(x, 0.0, 1e-6) and close(y, 0.0, 1e-6), f"the centre moved to ({x}, {y})")
    expect(radius > 105.0, f"the radius did not grow: {radius}")


def test_the_dragged_grip_lands_under_the_pointer(work: Path) -> None:
    """On an arc, whose grips are not independent, the held one still wins.

    Both endpoints share a radius, so moving either moves the other. Applied in
    index order, the grip being dragged was carried off by the one merely marked
    and ended up somewhere the pointer never was.
    """
    _, saved = run(work, "shape-arc-endpoints", [
        "--run", "ARC", "--run", "0,0", "--run", "200,150", "--run", "400,0",
        "--run", "ZOOMEXTENTS",
        "--click", "595,132",          # the arc, at its apex
        "--shift-click", "56,552",    # mark the left endpoint
        "--click", "1181,552",         # drag the right one up
        "--hover", "1181,452", "--click", "1181,452",
    ])
    found = records(saved, "ARC")
    expect(len(found) == 1, f"expected one arc, got {len(found)}")
    entry = found[0]
    cx, cy = float(entry["10"]), float(entry["20"])
    radius = float(entry["40"])
    ends = []
    for code in ("50", "51"):
        angle = math.radians(float(entry[code]))
        ends.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    # The dragged endpoint was taken to roughly (399.74, 36.26).
    expect(any(close(x, 399.742, 0.01) and close(y, 36.257, 0.01) for x, y in ends),
           f"the dragged endpoint is not under the pointer: {ends}")


def test_a_grip_takes_a_typed_length_and_angle(work: Path) -> None:
    """Adjusting a point is typed the same way as placing one.

    A held grip is a point being placed, so it gets the dimension boxes too:
    "adjust this corner to exactly 250" is the same question as "put this
    corner exactly 250 away".
    """
    _, saved = run(work, "grip-dim", TWO_LINES + [
        "--click", "438,483",          # the y=0 line
        "--click", "1181,483",         # take its far endpoint
        "--hover", "1038,413",
        "--type", "250", "--key", "tab", "--type", "30", "--key", "return",
    ])
    moved = [line for line in lines_in(saved) if close(line[1], 0.0)]
    expect(len(moved) == 1, f"expected the y=0 line, got {moved}")
    _, _, x, y = moved[0]
    expect(close(x, 400.0 + 250.0 * math.cos(math.radians(30)), 1e-4), f"x was {x}")
    expect(close(y, 250.0 * math.sin(math.radians(30)), 1e-4), f"y was {y}")


def test_typed_input_carries_every_hot_grip(work: Path) -> None:
    """A typed length moves the marked grips too, by the same displacement."""
    _, saved = run(work, "grip-dim-hot", TWO_LINES + [
        "--click", "438,202", "--click", "438,483",
        "--shift-click", "1181,202",   # mark both far ends
        "--shift-click", "1181,483",
        "--click", "1181,483",         # then drag one of them
        "--hover", "1038,413",
        "--type", "250", "--key", "tab", "--type", "90", "--key", "return",
    ])
    ends = sorted((line[3] for line in lines_in(saved)))
    expect(len(ends) == 2, f"expected two lines, got {ends}")
    # Both far ends raised by exactly 250, from 0 and from 100.
    expect(close(ends[0], 250.0, 1e-6) and close(ends[1], 350.0, 1e-6),
           f"the hot grips did not both move by 250: {ends}")


def arc_in(path: Path) -> dict[str, object]:
    """The single arc in a file, as centre, radius and its three points."""
    found = records(path, "ARC")
    expect(len(found) == 1, f"expected one arc, got {len(found)}")
    entry = found[0]
    cx, cy = float(entry["10"]), float(entry["20"])
    radius = float(entry["40"])
    first, last = float(entry["50"]), float(entry["51"])
    middle = (first + last) / 2.0 if last > first else (first + last + 360.0) / 2.0
    at = lambda a: (cx + radius * math.cos(math.radians(a)),
                    cy + radius * math.sin(math.radians(a)))
    return {"centre": (cx, cy), "radius": radius,
            "start": at(first), "middle": at(middle), "end": at(last)}


# An arc through (0,0), (200,150) and (400,0). Zoomed to extents in the standard
# window its grips land at (316, 738), (879, 316) and (1441, 738); its centre is
# at (200, -58.333), which is off the bottom of the view.
ARC = ["--run", "ARC", "--run", "0,0", "--run", "200,150", "--run", "400,0",
       "--run", "ZOOMEXTENTS", "--click", "595,132"]


def test_taking_hold_of_an_unmarked_grip_starts_over(work: Path) -> None:
    """Marking two grips then dragging a third moves only the third.

    other CAD software's rule. Treating the grabbed grip as one more hot grip turned
    "mark both ends of an arc, then drag its middle" into a move of all three
    -- which on an arc is a rigid translation, and looks nothing like what the
    gesture suggests.
    """
    _, saved = run(work, "arc-cold-grip", ARC + [
        "--shift-click", "54,551",     # mark both ends
        "--shift-click", "1179,551",
        "--click", "617,129",           # then take hold of the middle
        "--hover", "617,229", "--click", "617,229",
    ])
    arc = arc_in(saved)
    for name, want in (("start", (400.0, 0.0)), ("end", (0.0, 0.0))):
        got = arc[name]
        expect(close(got[0], want[0], 1e-3) and close(got[1], want[1], 1e-3),
               f"the {name} should not have moved: {got}")
    expect(arc["middle"][1] < 140.0, f"the middle did not move: {arc['middle']}")


def test_two_marked_arc_ends_move_by_one_displacement(work: Path) -> None:
    """Hot grips move together, exactly.

    Every grip an arc has comes out of the same centre and radius, so applying
    them one after another had each partly undo the last: two ends dragged by
    an equal amount landed a unit apart and the centre wandered.
    """
    _, saved = run(work, "arc-hot-ends", ARC + [
        "--shift-click", "54,551",
        "--shift-click", "1179,551",
        "--click", "1179,551",          # drag one of the marked ends
        "--hover", "1179,651", "--click", "1179,651",
    ])
    arc = arc_in(saved)
    moved = [(arc["start"][0] - 400.0, arc["start"][1] - 0.0),
             (arc["end"][0] - 0.0, arc["end"][1] - 0.0)]
    expect(close(moved[0][0], moved[1][0], 1e-6) and close(moved[0][1], moved[1][1], 1e-6),
           f"the two ends moved by different amounts: {moved}")
    expect(abs(moved[0][1]) > 10.0, f"the ends did not move: {moved}")


def test_dragging_one_arc_end_keeps_the_centre(work: Path) -> None:
    """Resizing an arc about its centre, which is how a swing radius is set.

    A single grip is not the case the multi-grip solve is for, and routing it
    through the same three-point rebuild quietly changed what one drag does --
    the centre started wandering on an ordinary endpoint drag.
    """
    _, saved = run(work, "arc-one-end", ARC + [
        "--click", "1179,551",
        "--hover", "1088,513", "--click", "1088,513",
    ])
    arc = arc_in(saved)
    centre = arc["centre"]
    expect(close(centre[0], 200.0, 1e-6) and close(centre[1], -58.3333333, 1e-4),
           f"the centre moved to {centre}")
    expect(arc["radius"] < 200.0, f"the radius did not shrink: {arc['radius']}")


def canned(work: Path, name: str, text: str) -> Path:
    """A file standing in for what a model would have said."""
    path = work / f"{name}.txt"
    path.write_text(text, encoding="utf-8")
    return path


def test_hatch_picked_inside_a_room_traces_the_walls(work: Path) -> None:
    """Pick an empty spot inside four walls and the fill finds them.

    The walls overshoot their corners, as drawn walls do, so the boundary has
    to be cut where they cross rather than chained end to end. One loop, solid,
    associative to the walls it was traced from.
    """
    _, saved = run(work, "hatch-pick", [
        "--run", "LINE", "--run", "-10,0", "--run", "210,0", "--run", "",
        "--run", "LINE", "--run", "200,-10", "--run", "200,110", "--run", "",
        "--run", "LINE", "--run", "210,100", "--run", "-10,100", "--run", "",
        "--run", "LINE", "--run", "0,110", "--run", "0,-10", "--run", "",
        "--run", "HATCH", "--run", "100,50", "--run", "", "--run", "S",
    ])
    hatches = records(saved, "HATCH")
    expect(len(hatches) == 1, f"expected one hatch, found {len(hatches)}")
    expect(hatches[0].get("91") == "1", f"expected one boundary loop:\n{hatches[0]}")
    expect(hatches[0].get("70") == "1", "the fill was not written as solid")
    expect(hatches[0].get("71") == "1", "the hatch is not associative to its walls")
    expect(hatches[0].get("93") == "4", f"expected four edges, got {hatches[0].get('93')}")


def test_xattach_brings_another_drawing_in_as_a_reference(work: Path) -> None:
    """A second drawing attached shows its geometry and saves as a reference.

    The block record carries the path and the reference flags, the insert
    points at it by name, and the contents were actually loaded, so the
    circle is on screen and not merely promised.
    """
    _, other = run(work, "xref-source", ["--run", "CIRCLE", "--run", "0,0", "--run", "25"])
    log, saved = run(work, "xref-host", [
        "--run", "XATTACH", "--run", str(other), "--run", "",
        "--run", "100,100", "--run", "", "--run", "",
    ])
    expect("Reference xref-source loaded: 1 object(s)" in log,
           f"the reference was not loaded:\n{log}")
    expect('Reference "xref-source" attached' in log, f"the attach was not reported:\n{log}")
    blocks = [b for b in records(saved, "BLOCK") if b.get("2") == "xref-source"]
    expect(len(blocks) == 1, f"expected one block record for the reference:\n{blocks}")
    expect("xref-source" in blocks[0].get("1", ""), f"the block has no path:\n{blocks[0]}")
    expect(int(blocks[0].get("70", "0")) & 4 == 4, f"the block is not flagged as a reference:\n{blocks[0]}")
    inserts = [i for i in records(saved, "INSERT") if i.get("2") == "xref-source"]
    expect(len(inserts) == 1, f"expected one insert of the reference:\n{inserts}")
    expect(inserts[0].get("10", "").startswith("100"), f"the insert is not at 100,100:\n{inserts[0]}")


def test_pdf_and_image_attach_and_save_with_their_definitions(work: Path) -> None:
    """A PDF page and a picture are placed, drawn, and written out whole.

    Written whole means the definitions other software looks up: the
    dictionary entries, the definition objects, and the class declarations
    without which a reader drops the underlay on the floor.
    """
    pdf = work / "sketch.pdf"
    subprocess.run([str(EXE), "-platform", "offscreen", "--no-recovery-prompt",
                    "--run", "CIRCLE", "--run", "50,50", "--run", "40",
                    "--export-pdf", str(pdf)], capture_output=True, text=True, timeout=180,
                   cwd=str(ROOT))
    expect(pdf.exists(), "the PDF used as the underlay was not exported")
    picture = work / "picture.png"
    subprocess.run([str(EXE), "-platform", "offscreen", "--no-recovery-prompt", "--size", "400x300",
                    "--screenshot", str(picture)], capture_output=True, text=True, timeout=180,
                   cwd=str(ROOT))
    expect(picture.exists(), "the picture used as the image was not made")

    log, saved = run(work, "attach-pdf-image", [
        "--run", "PDFATTACH", "--run", str(pdf), "--run", "0,0", "--run", "", "--run", "",
        "--run", "IMAGEATTACH", "--run", str(picture), "--run", "300,0", "--run", "0.5",
        "--run", "",
    ])
    expect("PDF page 1 of 1 attached" in log, f"the PDF was not attached:\n{log}")
    expect("Image attached, 400 by " in log, f"the image was not attached:\n{log}")

    underlays = records(saved, "PDFUNDERLAY")
    expect(len(underlays) == 1, f"expected one PDF underlay, found {len(underlays)}")
    definitions = records(saved, "PDFDEFINITION")
    expect(len(definitions) == 1 and definitions[0].get("2") == "1",
           f"the PDF definition does not name its page:\n{definitions}")
    images = records(saved, "IMAGE")
    expect(len(images) == 1 and images[0].get("13", "").startswith("400"),
           f"the image was not written with its pixel size:\n{images}")
    expect(len(records(saved, "IMAGEDEF")) == 1, "the image definition was not written")
    classes = [c.get("1") for c in records(saved, "CLASS")]
    for needed in ("PDFUNDERLAY", "PDFDEFINITION", "IMAGE", "IMAGEDEF"):
        expect(needed in classes, f"class {needed} is not declared:\n{classes}")
    text = saved.read_text(encoding="utf-8", errors="replace")
    expect("ACAD_PDFDEFINITIONS" in text and "ACAD_IMAGE_DICT" in text,
           "the dictionaries naming the definitions were not written")


def test_the_assistant_draws_what_it_proposes(work: Path) -> None:
    """A sentence in, geometry out, with no network in the way.

    The provider is an interface precisely so this can be run: everything
    either side of the transport -- the prompt, the reply format, what is
    allowed, and the edit it makes -- is exercised here. The transport itself
    is not, and no automated run here covers it.
    """
    reply = canned(work, "room",
                   "A 400 by 300 room with a column.\n"
                   "```\n"
                   "RECTANGLE 0,0 400,300\n"
                   "CIRCLE 200,150 25\n"
                   "```\n")
    log, saved = run(work, "agent-draw", [
        "--agent-reply", str(reply),
        "--agent", "draw a room with a column",
    ])
    expect("2 of 2 step(s) completed" in log, f"the plan did not run:\n{log}")

    circles = records(saved, "CIRCLE")
    expect(len(circles) == 1, f"expected one circle, got {len(circles)}")
    expect(close(float(circles[0]["10"]), 200.0) and close(float(circles[0]["40"]), 25.0),
           "the circle is not where the plan put it")
    expect(len(records(saved, "LWPOLYLINE")) == 1, "the rectangle was not drawn")


def test_the_plan_is_shown_before_it_runs(work: Path) -> None:
    """Every step is a command, and every command is reported before applying.

    That is the whole reason an assistant here emits commands rather than
    reaching into the drawing: a person can read exactly what is about to
    happen. Applying without showing would make the property worthless.
    """
    reply = canned(work, "shown", "```\nLINE 0,0 100,100\n```\n")
    log, _ = run(work, "agent-shown", [
        "--agent-reply", str(reply), "--agent", "draw a diagonal",
    ])
    expect("Assistant proposes: LINE 0,0 100,100" in log,
           f"the step was applied without being reported:\n{log}")


def test_the_assistant_can_ask_for_facts_and_ask_the_user(work: Path) -> None:
    """A canned exchange in three parts: the model asks Slate about a room,
    then asks the user a question, then answers with a command.

    A scripted run has no one to answer, so the question takes its own
    suggestion; the history shows both what was asked and what was told.
    """
    reply = canned(work, "converse",
                   "```\nASK ROOM OFFICE\n```\n"
                   "===\n"
                   "? Which way should the duct run? [east to west]\n"
                   "===\n"
                   "```\nLINE 0,150 400,150\n```\n")
    log, saved = run(work, "agent-converse", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "TEXT", "--run", "150,160", "--run", "10", "--run", "0",
        "--run", "OFFICE", "--run", "",
        "--agent-reply", str(reply),
        "--agent", "run a duct through the office",
    ])
    expect("Asked: ASK ROOM OFFICE" in log, f"the fact round was not logged:\n{log}")
    expect("? Which way should the duct run? -> east to west" in log,
           f"the question did not take its suggestion:\n{log}")
    expect("1 of 1 step(s) completed" in log, f"the final plan did not run:\n{log}")
    expect(len(lines_in(saved)) == 1, "the duct line was not drawn")


def test_a_project_beside_the_drawing_is_told_to_the_assistant(work: Path) -> None:
    """A project file listing the saved drawing as sheet M2.1 puts the set,
    and which sheet is open, into what the assistant is told.

    The sheet's reading is made from its file, so the room drawn here shows
    up in the sheet's line. A sheet whose file is missing is said to be.
    """
    folder = work / "set"
    folder.mkdir(exist_ok=True)
    drawing = folder / "M2.1.dxf"
    run(work, "project-draw", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "4000,3000",
        "--run", "TEXT", "--run", "1500,1600", "--run", "100", "--run", "0",
        "--run", "OFFICE", "--run", "",
        "--save-as", str(drawing),
    ], save=False)
    (folder / "project.slate").write_text(
        "project: Test Set\n"
        "discipline: M\n"
        "sheet: M2.1 | Duct Plan | M2.1.dxf | | | draft\n"
        "sheet: M0.1 | Notes | M0.1.dxf\n", encoding="utf-8")
    log, _ = run(work, "project-context", [
        "--open", str(drawing), "--panel", "project", "--agent-context",
    ], save=False)
    expect("No panel named" not in log, f"the project panel is missing:\n{log}")
    expect("Project: Test Set (M)" in log, f"the project was not found beside the drawing:\n{log}")
    expect("M2.1 Duct Plan, draft - 1 rooms" in log, f"the sheet was not read:\n{log}")
    expect("M0.1 Notes - not read" in log, f"the missing sheet was not reported:\n{log}")
    expect("The open drawing is sheet M2.1 Duct Plan." in log,
           f"the open sheet was not named:\n{log}")
    expect((folder / "project.slate.index").exists(), "the sheet readings were not cached")


def minimal_title_block_template(path: Path) -> None:
    """A template drawing: one block TB with SHEET and TITLE attributes,
    placed once, written in the smallest DXF the reader accepts."""
    def group(code: int, value) -> str:
        return f"{code}\n{value}\n"

    text = group(0, "SECTION") + group(2, "BLOCKS")
    text += group(0, "BLOCK") + group(8, "0") + group(2, "TB") + group(70, 2)
    text += group(10, 0) + group(20, 0) + group(30, 0) + group(3, "TB")
    for tag, y in (("SHEET", 0), ("TITLE", 20)):
        text += group(0, "ATTDEF") + group(8, "0") + group(10, 0) + group(20, y) + group(30, 0)
        text += group(40, 10) + group(1, "") + group(3, tag) + group(2, tag) + group(70, 0)
    text += group(0, "ENDBLK") + group(8, "0")
    text += group(0, "ENDSEC")
    text += group(0, "SECTION") + group(2, "ENTITIES")
    text += group(0, "INSERT") + group(8, "0") + group(66, 1) + group(2, "TB")
    text += group(10, 100) + group(20, 100) + group(30, 0)
    for tag, y in (("SHEET", 100), ("TITLE", 120)):
        text += group(0, "ATTRIB") + group(8, "0") + group(10, 100) + group(20, y) + group(30, 0)
        text += group(40, 10) + group(1, "X-0") + group(2, tag) + group(70, 0)
    text += group(0, "SEQEND") + group(8, "0")
    text += group(0, "ENDSEC") + group(0, "EOF")
    path.write_text(text, encoding="utf-8")


def test_sheet_files_are_made_from_the_template_with_the_title_block_filled(work: Path) -> None:
    """Two listed sheets with no files yet are made from the template, each
    with its number and title in the title block, and the list learns the
    file names. A sheet whose file exists is left alone."""
    folder = work / "newset"
    folder.mkdir(exist_ok=True)
    minimal_title_block_template(folder / "template.dxf")
    (folder / "project.slate").write_text(
        "project: New Set\n"
        "discipline: M\n"
        "template: template.dxf\n"
        "sheet: M1.1 | Mechanical Plan Level 1 |  |  |  | draft\n"
        "sheet: M2.1 | Duct Plan |  |  |  |\n", encoding="utf-8")
    log, _ = run(work, "make-sheets", [
        "--project", str(folder / "project.slate"), "--make-sheets",
    ], save=False)
    expect("2 sheet file(s) made." in log, f"the sheets were not made:\n{log}")
    made = folder / "M2.1.dxf"
    expect(made.exists(), f"M2.1.dxf was not written:\n{log}")
    values = {r.get("2"): r.get("1") for r in records(made, "ATTRIB")}
    expect(values.get("SHEET") == "M2.1", f"the sheet number was not filled: {values}")
    expect(values.get("TITLE") == "Duct Plan", f"the title was not filled: {values}")
    listing = (folder / "project.slate").read_text(encoding="utf-8")
    expect("M1.1 | Mechanical Plan Level 1 | M1.1.dxf" in listing,
           f"the list did not learn the file name:\n{listing}")

    # Run again: nothing to make, nothing overwritten.
    log, _ = run(work, "make-sheets-again", [
        "--project", str(folder / "project.slate"), "--make-sheets",
    ], save=False)
    expect("0 sheet file(s) made." in log, f"existing sheets were remade:\n{log}")


def test_the_assistant_learns_house_standards_from_a_reference_folder(work: Path) -> None:
    """A folder of the office's drawings teaches the layers it uses.

    One drawing cannot say which layer a wall belongs on. The drawings the
    office already made can, and a note left beside them says the rest. Both
    go into what the assistant is told, and --agent-context shows that text
    without a key or a network.
    """
    reference = work / "reference"
    reference.mkdir(exist_ok=True)
    _, made = run(work, "ref-make", [
        "--run", "LAYER", "--run", "S-REF-WALL", "--run", "1",
        "--run", "LINE", "--run", "0,0", "--run", "300,0", "--run", "",
    ])
    shutil.copyfile(made, reference / "office-plan.dxf")
    (reference / "layers.md").write_text("Walls go on S-REF-WALL.\n", encoding="utf-8")

    log, _ = run(work, "agent-context", [
        "--agent-reference", str(reference), "--agent-context",
    ], save=False)
    expect("House standards, learned from 1 drawing" in log,
           f"the reference drawing was not read:\n{log}")
    expect("S-REF-WALL: colour 1 (red), Continuous, in 1 drawing, 1 object (1 Line)" in log,
           f"the layer and what sits on it were not reported:\n{log}")
    expect("Notes from layers.md:" in log and "Walls go on S-REF-WALL." in log,
           f"the note was not passed along:\n{log}")
    expect("Layers (" in log and "0: colour 7" in log,
           f"the open drawing's own layers were not described:\n{log}")


def test_an_invented_command_is_refused_and_the_rest_still_run(work: Path) -> None:
    """A model naming a command that does not exist is the common failure."""
    reply = canned(work, "invented",
                   "```\n"
                   "LINE 0,0 400,0\n"
                   "SUMMON-DRAGON 5\n"
                   "CIRCLE 200,150 25\n"
                   "```\n")
    log, saved = run(work, "agent-refused", [
        "--agent-reply", str(reply), "--agent", "do several things",
    ])
    expect("SUMMON-DRAGON" in log and "Refused" in log,
           f"the invented command was not reported:\n{log}")
    expect(len(lines_in(saved)) == 1, "the good line did not run")
    expect(len(records(saved, "CIRCLE")) == 1, "the step after the refusal did not run")


def test_a_step_the_drawing_rejects_does_not_stop_the_rest(work: Path) -> None:
    reply = canned(work, "partial",
                   "```\n"
                   "LINE 0,0 400,0\n"
                   "CIRCLE not-a-point 25\n"
                   "LINE 0,100 400,100\n"
                   "```\n")
    log, saved = run(work, "agent-partial", [
        "--agent-reply", str(reply), "--agent", "draw three things",
    ])
    expect("2 of 3 step(s) completed" in log, f"expected two of three to run:\n{log}")
    expect(len(lines_in(saved)) == 2, "the step after the failure did not run")


def test_an_assistant_edit_can_be_undone(work: Path) -> None:
    """Its steps are ordinary commands, so UNDO takes them back one by one."""
    reply = canned(work, "undoable", "```\nLINE 0,0 400,0\nCIRCLE 0,0 25\n```\n")
    _, saved = run(work, "agent-undo", [
        "--agent-reply", str(reply), "--agent", "draw two things",
        "--run", "UNDO",
    ])
    expect(len(records(saved, "CIRCLE")) == 0, "UNDO did not take back the last step")
    expect(len(lines_in(saved)) == 1, "UNDO took back more than the last step")


def test_without_a_key_it_says_so(work: Path) -> None:
    """Turned off or unconfigured is reported, not silently nothing.

    A user who has just pasted a key into the dialog needs to be told which of
    the several ways this can fail actually happened.
    """
    log, _ = run(work, "agent-nokey", ["--agent", "draw a line"])
    expect("assistant" in log.lower(), f"nothing explained the refusal:\n{log}")
    expect("Assistant proposes" not in log, f"something ran without a provider:\n{log}")


def test_every_command_is_on_a_menu(work: Path) -> None:
    """Nothing the application can do is unreachable from the menu bar.

    The menus are generated from the command registry rather than written out,
    so a new command -- including one a plugin brought -- appears the first time
    it is loaded. This is the check that the promise holds: a hand-written menu
    is a second list of what the application does, and it is wrong the moment
    anything is added.
    """
    log, _ = run(work, "menu-coverage", ["--dump-menus"], save=False)
    expect("MENU-COVERAGE complete" in log,
           f"some commands are not on any menu:\n"
           f"{[line for line in log.splitlines() if 'MENU-COVERAGE' in line]}")


def test_the_menu_bar_has_the_menus_a_drafter_looks_for(work: Path) -> None:
    """Grouped and ordered the way a CAD menu bar is, not alphabetically.

    Categories are how commands are grouped for the ribbon and there are more of
    them than a menu bar wants: a two-item Block menu beside a one-item Insert
    menu is a worse place to look than one Insert menu with three things on it.
    """
    log, _ = run(work, "menu-shape", ["--dump-menus"], save=False)
    menus = []
    for line in log.splitlines():
        if not line.startswith("MENU "):
            continue
        name = line[len("MENU "):].split(" > ")[0]
        if name not in menus:
            menus.append(name)

    for wanted in ("File", "Edit", "View", "Draw", "Annotate", "Modify", "Insert", "Tools"):
        expect(wanted in menus, f"there is no {wanted} menu: {menus}")
    # Drawing menus before the application's own settings.
    expect(menus.index("Draw") < menus.index("Tools"),
           f"Draw should come before Tools: {menus}")
    expect(menus.index("Draw") < menus.index("Modify"),
           f"Draw should come before Modify: {menus}")


def test_a_menu_item_runs_its_command(work: Path) -> None:
    """Being listed is not the same as working.

    Every generated item carries the command it runs, so this drives one the
    way a person would: pick it from the menu, then answer the prompts.
    """
    _, saved = run(work, "menu-runs", [
        "--menu-pick", "CIRCLE", "--run", "100,100", "--run", "40",
    ])
    circles = records(saved, "CIRCLE")
    expect(len(circles) == 1, f"the menu item drew nothing: {circles}")
    expect(close(float(circles[0]["10"]), 100.0) and close(float(circles[0]["40"]), 40.0),
           "the menu item ran something other than what it says")


def test_the_menus_are_reachable_without_a_menu_bar(work: Path) -> None:
    """The bar holds the menus; the ribbon's Menu button is where they are shown.

    Hiding the bar bought back a row of chrome, and it is only an improvement
    while every menu on it is still one press away. A run that checked the bar
    alone would pass with nothing reachable by hand at all, so this checks the
    button -- and that the button is carrying the *same* menus, not a copy that
    a later category would leave behind.
    """
    log, _ = run(work, "menu-button", ["--dump-menus"], save=False)

    button = ""
    for line in log.splitlines():
        if line.startswith("MENU-BUTTON "):
            button = line[len("MENU-BUTTON "):]
    on_button = [name.strip() for name in button.split(",") if name.strip()]

    bar = []
    for line in log.splitlines():
        if not line.startswith("MENU "):
            continue
        name = line[len("MENU "):].split(" > ")[0]
        if name not in bar:
            bar.append(name)

    expect(on_button == bar,
           f"the Menu button and the menu bar disagree: {on_button} vs {bar}")
    expect("MENU-BAR hidden" in log,
           "the menu bar is showing; the point of the button was to retire the row")


def test_no_command_is_listed_on_a_menu_twice(work: Path) -> None:
    """One place per command.

    The fixed entries and the generated ones used to collide: Edit offered Undo
    twice, View offered Zoom Extents twice, Tools offered Quick Select twice --
    the same command written by hand and then generated on top of it. A menu
    that says the same thing twice makes a reader wonder what the difference is.
    """
    log, _ = run(work, "menu-unique", ["--dump-menus"], save=False)

    seen: dict[str, list[str]] = {}
    for line in log.splitlines():
        if not line.startswith("MENU ") or not line.endswith("]"):
            continue
        path = line[len("MENU "):]
        command = path[path.rindex("[") + 1:-1]
        seen.setdefault(command, []).append(path[:path.rindex(" [")])

    doubled = {name: where for name, where in seen.items() if len(where) > 1}
    expect(not doubled, f"listed more than once: {doubled}")


def test_the_classic_menu_bar_can_be_brought_back(work: Path) -> None:
    """Off by default is a default, not a decision taken for the user.

    Toggled twice on purpose: the setting is saved, and a scenario that left it
    on would change the application for whoever runs the tests next.
    """
    log, _ = run(work, "menu-bar-toggle", [
        "--menu-pick", "Menu Bar", "--dump-menus",
        "--menu-pick", "Menu Bar", "--dump-menus",
    ], save=False)
    states = [line[len("MENU-BAR "):] for line in log.splitlines()
              if line.startswith("MENU-BAR ")]
    expect(states == ["shown", "hidden"],
           f"View > Menu Bar did not show then hide the bar: {states}")


def layers_in(path: Path) -> list[str]:
    """Layer names in a saved drawing, in table order."""
    return [entry["2"] for entry in records(path, "LAYER") if "2" in entry]


def test_copy_and_paste_puts_the_geometry_where_it_is_asked(work: Path) -> None:
    """Ctrl+C then Ctrl+V is the shortcut every application on the machine has.

    The copy is measured from the lower-left of its own extents, so pasting the
    line from (0,0) to (400,0) at (500,500) has to land it exactly there --
    not at the cursor plus wherever it happened to sit in the source drawing.
    """
    _, saved = run(work, "clip-paste", TWO_LINES + [
        "--click", "438,483",             # the y=0 line
        "--run", "COPYCLIP",
        "--run", "PASTECLIP", "--run", "500,500",
    ])
    pasted = [line for line in lines_in(saved) if close(line[1], 500.0)]
    expect(len(pasted) == 1, f"expected one pasted line, got {lines_in(saved)}")
    expect(close(pasted[0][0], 500.0) and close(pasted[0][2], 900.0),
           f"pasted line is not 400 long from (500,500): {pasted[0]}")
    expect(len(lines_in(saved)) == 3, "the originals should still be there")


def test_cut_removes_the_original_and_paste_puts_it_back(work: Path) -> None:
    """PASTEORIG is what makes cut-and-paste-to-another-sheet safe.

    Pasting to the original coordinates must not re-base the geometry on the
    copy's own base point, which is what "paste" does and what this must not.
    """
    _, saved = run(work, "clip-cut", TWO_LINES + [
        "--click", "438,483",
        "--run", "CUTCLIP",
        "--run", "PASTEORIG",
    ])
    remaining = lines_in(saved)
    expect(len(remaining) == 2, f"expected two lines back, got {remaining}")
    restored = [line for line in remaining if close(line[1], 0.0)]
    expect(len(restored) == 1, f"the cut line did not come back: {remaining}")
    expect(close(restored[0][0], 0.0) and close(restored[0][2], 400.0),
           f"the cut line came back somewhere else: {restored[0]}")


def test_cut_alone_leaves_the_drawing_short_of_it(work: Path) -> None:
    """The other half of the pair: cut really does erase, and undoably."""
    _, cut = run(work, "clip-cut-only", TWO_LINES + [
        "--click", "438,483", "--run", "CUTCLIP",
    ])
    expect(len(lines_in(cut)) == 1, f"cut left {len(lines_in(cut))} lines, expected 1")

    _, undone = run(work, "clip-cut-undone", TWO_LINES + [
        "--click", "438,483", "--run", "CUTCLIP", "--run", "UNDO",
    ])
    expect(len(lines_in(undone)) == 2,
           f"undo did not bring the cut line back: {lines_in(undone)}")


def test_a_paste_brings_the_layer_it_was_drawn_on(work: Path) -> None:
    """Geometry without its layer arrives in the wrong colour, on the wrong pen.

    Copied out of the sample drawing, which has a "Walls" layer, and pasted into
    one that has only "0". The layer has to come with it or the paste is a lie
    about what was copied.
    """
    _, saved = run(work, "clip-cross", [
        "--open", "samples/sample.dxf", "--run", "ZOOMEXTENTS",
        "--click", "@4000,2000",          # a wall, addressed in drawing units
        "--run", "COPYCLIP",
        "--open", "samples/wide-polylines.dxf", "--run", "ZOOMEXTENTS",
        "--run", "PASTECLIP", "--run", "0,0",
    ])
    names = layers_in(saved)
    expect("Walls" in names, f"the pasted geometry lost its layer: {names}")
    walls = [entry for entry in records(saved, "LINE") if entry.get("8") == "Walls"]
    expect(len(walls) == 1, f"expected one line on Walls, got {len(walls)}")


def test_pasting_the_same_layer_twice_does_not_make_a_second_one(work: Path) -> None:
    """A paste matches layers by name, so repeating it must not grow the table.

    The alternative -- importing "Walls" a second time under a made-up name --
    is how a drawing ends up with forty layers that differ only by suffix.
    """
    _, saved = run(work, "clip-twice", [
        "--open", "samples/sample.dxf", "--run", "ZOOMEXTENTS",
        "--click", "@4000,2000", "--run", "COPYCLIP",
        "--open", "samples/wide-polylines.dxf", "--run", "ZOOMEXTENTS",
        "--run", "PASTECLIP", "--run", "0,0",
        "--run", "PASTECLIP", "--run", "0,1000",
    ])
    names = layers_in(saved)
    expect(names.count("Walls") == 1, f"Walls was imported more than once: {names}")
    walls = [entry for entry in records(saved, "LINE") if entry.get("8") == "Walls"]
    expect(len(walls) == 2, f"expected two pasted lines, got {len(walls)}")


def test_paste_is_refused_when_the_clipboard_holds_no_drawing(work: Path) -> None:
    """Refused with a sentence, not attempted and then failed halfway."""
    log, _ = run(work, "clip-empty", [
        "--run", "PASTECLIP",
    ], save=False)
    expect("not available" in log or "nothing this drawing can paste" in log,
           f"PASTECLIP on an empty clipboard said nothing useful: {log}")


def test_a_leader_points_where_it_was_told_and_carries_its_note(work: Path) -> None:
    """The command a drawing set is made of: an arrow and the words for it.

    Checked in the file rather than on screen because the two halves are two
    entities, and the fault worth catching is the link between them going
    missing -- an arrow with no words, or words floating with no arrow.
    """
    _, saved = run(work, "leader", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS",
        "--run", "LEADER", "--run", "100,100", "--run", "250,220", "--run", "350,220",
        "--run", "", "--run", "TYP. 2x6 STUD WALL",
    ])
    leaders = records(saved, "LEADER")
    expect(len(leaders) == 1, f"expected one leader, got {len(leaders)}")
    leader = leaders[0]

    # The arrow point is the first vertex, and it is the one that has to be
    # exactly where it was picked -- it is pointing at something.
    expect(close(float(leader["10"]), 100.0) and close(float(leader["20"]), 100.0),
           f"the arrow is not at the point it was given: {leader.get('10')},{leader.get('20')}")
    # Three picked, plus the landing written as a real fourth vertex: a file
    # that leaves the landing to a hookline flag invites the reader to invent
    # one, and other CAD software invented something else.
    expect(leader.get("76") == "4", f"expected four vertices, got {leader.get('76')}")
    expect(leader.get("75") == "0", "the leader asked the reader to reconstruct its landing")
    expect(leader.get("71") == "1", "the leader lost its arrowhead")

    notes = [entry for entry in records(saved, "MTEXT")
             if entry.get("1") == "TYP. 2x6 STUD WALL"]
    expect(len(notes) == 1, f"the note was not written: {len(notes)} found")
    # Not group 340: an associated note is laid out by other CAD software's own rules,
    # which Slate does not model, so a leader Slate made is written unassociated
    # and carries its note link in XData instead.
    expect("340" not in leader, "the leader was written associated (340)")
    expect(leader.get("1001") == "SLATE" and leader.get("1005") == notes[0].get("5"),
           f"the leader does not carry its note in XData: 1001={leader.get('1001')} "
           f"1005={leader.get('1005')} note={notes[0].get('5')}")


def test_a_leader_with_no_words_is_still_a_leader(work: Path) -> None:
    """Enter at the note prompt draws the arrow and nothing else.

    A pointer to a surface rather than to a labelled object is a real drawing
    convention, and the file has to say there is no annotation rather than
    claiming an MTEXT it does not name.
    """
    _, saved = run(work, "leader-bare", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS",
        "--run", "LEADER", "--run", "100,100", "--run", "250,220", "--run", "",
        "--run", "",
    ])
    leaders = records(saved, "LEADER")
    expect(len(leaders) == 1, f"expected one leader, got {len(leaders)}")
    expect(leaders[0].get("73") == "3",
           f"a leader with no note should say so, got 73={leaders[0].get('73')}")
    expect("340" not in leaders[0], "a leader with no note named one anyway")
    expect(not records(saved, "MTEXT"), "an empty note was written as text")


def test_a_single_click_does_not_make_a_leader(work: Path) -> None:
    """One point is a click that has not become anything yet."""
    _, saved = run(work, "leader-short", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS",
        "--run", "LEADER", "--run", "100,100", "--run", "",
    ])
    expect(not records(saved, "LEADER"), "a one-point leader was drawn anyway")


def test_a_leader_and_its_note_copy_as_one(work: Path) -> None:
    """Selecting the arrow is how a drafter selects the callout."""
    _, saved = run(work, "leader-copy", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS",
        "--run", "LEADER", "--run", "100,100", "--run", "250,220", "--run", "350,220",
        "--run", "", "--run", "NOTE",
        "--click", "@175,160",            # on the leader's first segment
        "--run", "COPYCLIP",
        "--run", "PASTECLIP", "--run", "0,-400",
    ])
    leaders = records(saved, "LEADER")
    notes = [entry for entry in records(saved, "MTEXT") if entry.get("1") == "NOTE"]
    expect(len(leaders) == 2, f"expected two leaders after the paste, got {len(leaders)}")
    expect(len(notes) == 2, f"the note did not come with the leader: {len(notes)} found")

    # The arrow point is the lower-left of what was copied, so pasting at
    # (0,-400) puts the arrow exactly there.
    pasted = [entry for entry in leaders
              if close(float(entry["10"]), 0.0) and close(float(entry["20"]), -400.0)]
    expect(len(pasted) == 1, f"the pasted leader is not where it was put: {leaders}")
    # And the copy names the copy of the note, not the original's handle.
    handles = {entry.get("5") for entry in notes}
    expect(pasted[0].get("1005") in handles,
           f"the pasted leader names no note in this file: {pasted[0].get('1005')}")
    expect(pasted[0].get("1005") != leaders[0].get("1005") or leaders[0] is pasted[0],
           "both leaders name the same note")


def test_a_shipped_macro_draws_what_it_says(work: Path) -> None:
    """A macro file beside the executable is a command like any other.

    NORTHARROW is three lines and a letter. Checked in the saved file because
    the claim is that a macro produces real geometry, not a special kind.
    """
    _, saved = run(work, "macro-north", ["--run", "NORTHARROW"])
    drawn = lines_in(saved)
    expect(len(drawn) == 3, f"expected three lines, got {len(drawn)}")
    # The staff runs from the origin straight up.
    expect(any(close(a, 0.0) and close(b, 0.0) and close(c, 0.0) and close(d, 40.0)
               for a, b, c, d in drawn),
           f"the staff is not where the macro puts it: {drawn}")
    letters = [entry for entry in records(saved, "MTEXT") if entry.get("1") == "N"]
    expect(len(letters) == 1, "the N is missing")


def test_a_macro_asks_for_its_parameter_and_pauses_for_the_user(work: Path) -> None:
    """The shape almost every real macro has.

    SQUARE asks a side length, then hands the next prompt back so the corner is
    picked on the drawing, then finishes from the answer. Both halves matter:
    a macro that cannot pause is a recording, and one that cannot take a
    parameter has to be rewritten to be reused.
    """
    _, saved = run(work, "macro-square", [
        "--run", "RECTANGLE", "--run", "0,0", "--run", "400,300",
        "--run", "ZOOMEXTENTS",
        "--run", "SQUARE", "--run", "200",
        "--click", "@100,50",
    ])
    # The scaffold rectangle plus the macro's square.
    polylines = records(saved, "LWPOLYLINE")
    expect(len(polylines) == 2, f"expected two polylines, got {len(polylines)}")

    # The square is 200 on a side, from wherever the click landed -- the pause
    # supplied the corner and `@$SIDE,$SIDE` expanded from it.
    corners = [entry for entry in polylines if entry.get("90") == "4"]
    expect(len(corners) == 2, f"expected two four-corner shapes, got {len(corners)}")


def test_escape_during_a_macro_stops_the_whole_macro(work: Path) -> None:
    """Not just the step it happened to be inside.

    Otherwise a drafter has to press Escape once per remaining step, which is
    indistinguishable from the application ignoring them. The macro has a
    second command after the pause on purpose: with only one, Escape would
    look like it worked whatever the queue did.
    """
    script = work / "two.macro"
    script.write_text("LINE" + chr(10) + "0,0" + chr(10) + chr(92) + chr(10) + ";" +
                      chr(10) + "CIRCLE" + chr(10) + "50,50" + chr(10) + "25" + chr(10),
                      encoding="utf-8")
    _, saved = run(work, "macro-escape", [
        "--run", "SCRIPT", "--run", str(script),
        "--key", "escape",
    ])
    expect(not records(saved, "CIRCLE"),
           "Escape at the pause did not stop the rest of the macro")
    expect(not lines_in(saved), "the cancelled line was drawn anyway")


def test_a_script_file_runs_once_without_registering_a_command(work: Path) -> None:
    """SCRIPT is "do this now"; a macro is "give me a command that does this"."""
    script = work / "setup.macro"
    script.write_text("RECTANGLE" + chr(10) + "0,0" + chr(10) + "40,30" + chr(10),
                      encoding="utf-8")
    _, saved = run(work, "macro-script", [
        "--run", "SCRIPT", "--run", str(script),
    ])
    expect(len(records(saved, "LWPOLYLINE")) == 1,
           "the script did not draw its rectangle")

    log, _ = run(work, "macro-script-2", ["--run", "SETUP"], save=False)
    expect("Unknown command" in log,
           "running a script registered a command under the file's name")


def test_a_macro_that_goes_wrong_stops_rather_than_running_on(work: Path) -> None:
    """A step the command will not take must not be fed to the one after it."""
    script = work / "bad.macro"
    script.write_text("LINE" + chr(10) + "0,0" + chr(10) + "sideways" + chr(10) +
                      "100,100" + chr(10) + ";" + chr(10) + "CIRCLE" + chr(10) +
                      "50,50" + chr(10) + "25" + chr(10), encoding="utf-8")
    log, saved = run(work, "macro-bad", ["--run", "SCRIPT", "--run", str(script)])
    expect("sideways" in log, f"the failure did not say what went wrong: {log[-400:]}")
    expect(not records(saved, "CIRCLE"),
           "the macro carried on into the next command after a bad step")


def test_a_drawing_dropped_on_the_window_opens_in_a_new_tab(work: Path) -> None:
    """Dragging a file from a folder onto Slate is File > Open.

    In a new tab, not in place of the drawing already there: the drawing being
    worked on is still open afterwards, and the dropped one is in front. (An
    untitled drawing nobody has touched gives way instead, as it does for File
    > Open -- hence the line drawn first.)
    """
    log, saved = run(work, "drop-open", [
        "--run", "LINE", "--run", "0,0", "--run", "10,0", "--run", "",
        "--drop", "samples/sample.dxf", "--dump-menus",
    ])
    expect("Opened sample.dxf" in log, f"the dropped drawing did not open:\n{log[-600:]}")
    tabs = [line for line in log.splitlines() if line.startswith("FILE-TABS ")]
    expect(tabs and tabs[-1].startswith("FILE-TABS 2:"),
           f"expected two file tabs after the drop, got {tabs}")
    # The active drawing is the dropped one.
    expect("Walls" in layers_in(saved), "the saved drawing is not the dropped one")


def test_a_file_that_is_not_a_drawing_is_refused_when_dropped(work: Path) -> None:
    """The drag is declined, so the desktop shows a refusal rather than a drop
    that then does nothing -- and nothing opens."""
    notes = work / "notes.txt"
    notes.write_text("not a drawing", encoding="utf-8")
    log, _ = run(work, "drop-refused", [
        "--drop", str(notes), "--dump-menus",
    ], save=False)
    expect("not a drawing" in log, "a refused drop was not reported")
    tabs = [line for line in log.splitlines() if line.startswith("FILE-TABS ")]
    expect(tabs and tabs[-1].startswith("FILE-TABS 1:"),
           f"a non-drawing drop changed the tabs: {tabs}")


def test_a_sheet_made_here_takes_its_own_geometry_and_a_viewport(work: Path) -> None:
    """LAYOUT New makes a sheet; what is drawn on it stays on it; MVIEW opens a
    window onto the model at a scale; PAGESETUP sets the paper. All of it lands
    in the file the way other CAD software writes it."""
    log, saved = run(work, "sheet", [
        "--run", "LINE", "--run", "0,0", "--run", "1000,500", "--run", "",
        "--run", "LAYOUT", "--run", "N", "--run", "Sheet1",
        "--run", "LINE", "--run", "10,10", "--run", "50,10", "--run", "",
        "--run", "MVIEW", "--run", "20,20", "--run", "120,80", "--run", "1:50",
        "--run", "PAGESETUP", "--run", "A3", "--run", "L", "--run", "1:1",
    ])
    expect("Layout \"Sheet1\" created" in log, f"no sheet was made: {log[-600:]}")
    lines = records(saved, "LINE")
    on_sheet = [l for l in lines if l.get("67") == "1"]
    in_model = [l for l in lines if l.get("67") != "1"]
    expect(len(on_sheet) == 1 and len(in_model) == 1,
           f"expected one line on the sheet and one in the model, got {len(on_sheet)} / {len(in_model)}")
    windows = [v for v in records(saved, "VIEWPORT") if v.get("69") == "2"]
    expect(len(windows) == 1, f"expected one viewport numbered 2, got {len(windows)}")
    # And the paper's own view, numbered 1, which other CAD software expects every sheet to carry.
    paper = [v for v in records(saved, "VIEWPORT") if v.get("69") == "1"]
    expect(len(paper) == 1, f"expected the sheet's paper viewport numbered 1, got {len(paper)}")
    expect(close(float(windows[0]["45"]), 3000.0), f"viewport shows {windows[0]['45']} of model, not 3000")
    expect(windows[0].get("67") == "1", "the viewport is not flagged as paper space")
    # A LAYOUT record carries code 1 twice (page setup name, then the layout
    # name); records() keeps the first, so the sheet is found by its paper.
    layouts = records(saved, "LAYOUT")
    expect(len(layouts) == 2, f"expected Model and Sheet1 LAYOUT records, got {len(layouts)}")
    expect("Sheet1" in saved.read_text(encoding="utf-8", errors="replace"), "the sheet's name was not written")
    a3 = [l for l in layouts if close(float(l["44"]), 297.0) and close(float(l["45"]), 420.0)]
    expect(len(a3) == 1, "no LAYOUT record carries the A3 paper PAGESETUP set")


def test_stepping_into_a_viewport_works_in_the_model_through_it(work: Path) -> None:
    """MSPACE on a sheet with one viewport steps into it: typed points are model
    coordinates, a line drawn lands in the model, ZOOM moves the model behind
    the frame rather than the sheet, and PSPACE steps back out."""
    log, saved = run(work, "mspace", [
        "--run", "LINE", "--run", "0,0", "--run", "1000,500", "--run", "",
        "--run", "LAYOUT", "--run", "N", "--run", "Sheet1",
        "--run", "MVIEW", "--run", "20,20", "--run", "220,150", "--run", "1:10",
        "--run", "MSPACE",
        "--run", "LINE", "--run", "100,100", "--run", "300,100", "--run", "",
        "--run", "ZOOM", "--run", "0,0", "--run", "500,250",
        "--run", "PSPACE",
    ])
    expect("Model space, through the viewport" in log, f"MSPACE did not step in: {log[-500:]}")
    expect("Paper space" in log, "PSPACE did not step out")
    lines = records(saved, "LINE")
    drawn = [l for l in lines if l.get("10", "").startswith("100")]
    expect(len(drawn) == 1 and drawn[0].get("67") != "1",
           "the line drawn through the viewport is not in the model")
    windows = [v for v in records(saved, "VIEWPORT") if v.get("69") == "2"]
    expect(len(windows) == 1, "the viewport is missing")
    # 500 x 250 of model into a 200 x 130 frame: the width decides, 325 tall.
    expect(close(float(windows[0]["45"]), 325.0), f"viewport shows {windows[0]['45']} tall, not 325")
    expect(close(float(windows[0]["12"]), 250.0) and close(float(windows[0]["22"]), 125.0),
           f"viewport centre is {windows[0].get('12')},{windows[0].get('22')}, not 250,125")


def write_corner_block(path: Path) -> None:
    """A block with geometry only in two opposite corners of a wide square."""
    groups: list[tuple[int, str]] = [
        (0, "SECTION"), (2, "BLOCKS"),
        (0, "BLOCK"), (5, "400"), (100, "AcDbEntity"), (8, "0"),
        (100, "AcDbBlockBegin"), (2, "Corners"), (70, "0"),
        (10, "0.0"), (20, "0.0"), (30, "0.0"), (3, "Corners"), (1, ""),
        (0, "LINE"), (5, "401"), (100, "AcDbEntity"), (8, "0"),
        (100, "AcDbLine"), (10, "0.0"), (20, "0.0"), (30, "0.0"),
        (11, "10.0"), (21, "0.0"), (31, "0.0"),
        (0, "LINE"), (5, "402"), (100, "AcDbEntity"), (8, "0"),
        (100, "AcDbLine"), (10, "90.0"), (20, "100.0"), (30, "0.0"),
        (11, "100.0"), (21, "100.0"), (31, "0.0"),
        (0, "ENDBLK"), (5, "403"), (100, "AcDbEntity"), (8, "0"),
        (100, "AcDbBlockEnd"),
        (0, "ENDSEC"),
        (0, "SECTION"), (2, "ENTITIES"),
        (0, "INSERT"), (5, "404"), (100, "AcDbEntity"), (8, "0"),
        (100, "AcDbBlockReference"), (2, "Corners"),
        (10, "0.0"), (20, "0.0"), (30, "0.0"),
        (0, "ENDSEC"), (0, "EOF"),
    ]
    with path.open("w", encoding="utf-8", newline="\r\n") as handle:
        for code, value in groups:
            handle.write(f"{code}\n{value}\n")


SCENARIOS = [
    test_click_picks_what_is_under_it,
    test_dimension_length_and_angle,
    test_dimension_lock_holds_through_a_click,
    test_dimension_lock_does_not_survive_a_cancel,
    test_hot_grips_move_together,
    test_polyline_midpoint_grip_adds_a_vertex,
    test_snap_tracking_finds_the_crossing,
    test_array_rectangular,
    test_array_polar,
    test_block_is_picked_by_its_geometry,
    test_distance_reports_separation_and_angle,
    test_area_of_a_closed_object,
    test_area_of_an_open_object_reports_length,
    test_describe_reads_a_labelled_room,
    test_the_assistant_can_ask_for_facts_and_ask_the_user,
    test_a_project_beside_the_drawing_is_told_to_the_assistant,
    test_sheet_files_are_made_from_the_template_with_the_title_block_filled,
    test_two_hot_grips_on_one_object_stretch_together,
    test_two_hot_grips_move_the_object_once,
    test_two_midpoint_grips_each_add_a_corner,
    test_circle_dragged_by_its_centre_keeps_its_radius,
    test_circle_stretched_by_a_quadrant_keeps_its_centre,
    test_the_dragged_grip_lands_under_the_pointer,
    test_a_grip_takes_a_typed_length_and_angle,
    test_typed_input_carries_every_hot_grip,
    test_taking_hold_of_an_unmarked_grip_starts_over,
    test_two_marked_arc_ends_move_by_one_displacement,
    test_dragging_one_arc_end_keeps_the_centre,
    test_hatch_picked_inside_a_room_traces_the_walls,
    test_xattach_brings_another_drawing_in_as_a_reference,
    test_pdf_and_image_attach_and_save_with_their_definitions,
    test_the_assistant_draws_what_it_proposes,
    test_the_plan_is_shown_before_it_runs,
    test_an_invented_command_is_refused_and_the_rest_still_run,
    test_the_assistant_learns_house_standards_from_a_reference_folder,
    test_a_step_the_drawing_rejects_does_not_stop_the_rest,
    test_an_assistant_edit_can_be_undone,
    test_without_a_key_it_says_so,
    test_every_command_is_on_a_menu,
    test_the_menu_bar_has_the_menus_a_drafter_looks_for,
    test_a_menu_item_runs_its_command,
    test_the_menus_are_reachable_without_a_menu_bar,
    test_no_command_is_listed_on_a_menu_twice,
    test_the_classic_menu_bar_can_be_brought_back,
    test_copy_and_paste_puts_the_geometry_where_it_is_asked,
    test_cut_removes_the_original_and_paste_puts_it_back,
    test_cut_alone_leaves_the_drawing_short_of_it,
    test_a_paste_brings_the_layer_it_was_drawn_on,
    test_pasting_the_same_layer_twice_does_not_make_a_second_one,
    test_paste_is_refused_when_the_clipboard_holds_no_drawing,
    test_a_leader_points_where_it_was_told_and_carries_its_note,
    test_a_leader_with_no_words_is_still_a_leader,
    test_a_single_click_does_not_make_a_leader,
    test_a_leader_and_its_note_copy_as_one,
    test_a_shipped_macro_draws_what_it_says,
    test_a_macro_asks_for_its_parameter_and_pauses_for_the_user,
    test_escape_during_a_macro_stops_the_whole_macro,
    test_a_script_file_runs_once_without_registering_a_command,
    test_a_macro_that_goes_wrong_stops_rather_than_running_on,
    test_a_drawing_dropped_on_the_window_opens_in_a_new_tab,
    test_a_file_that_is_not_a_drawing_is_refused_when_dropped,
    test_a_sheet_made_here_takes_its_own_geometry_and_a_viewport,
    test_stepping_into_a_viewport_works_in_the_model_through_it,
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default="", help="run scenarios whose name contains this")
    parser.add_argument("--keep", action="store_true", help="keep the saved drawings")
    options = parser.parse_args()

    if not EXE.exists():
        print(f"Build first: {EXE} not found", file=sys.stderr)
        return 2

    # The offscreen platform has no font database, and text metrics feed into
    # what some of these scenarios click on.
    os.environ.setdefault("QT_QPA_FONTDIR", "C:/Windows/Fonts")

    work = Path(tempfile.mkdtemp(prefix="slate-interaction-"))
    failures = []
    ran = 0
    try:
        for scenario in SCENARIOS:
            name = scenario.__name__.removeprefix("test_")
            if options.only and options.only not in name:
                continue
            ran += 1
            try:
                scenario(work)
            except Failure as error:
                failures.append((name, str(error)))
                print(f"  FAIL  {name}\n        {error}")
            except Exception as error:  # noqa: BLE001 - report and keep going
                failures.append((name, repr(error)))
                print(f"  ERROR {name}\n        {error!r}")
            else:
                print(f"  ok    {name}")
    finally:
        if options.keep:
            print(f"\nDrawings kept in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    print(f"\n{ran - len(failures)} of {ran} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
