"""Draw the built exhibit's geometry over the original scan, to see where they differ.

Writes overlay.png beside this script: the scan, with every polyline, line and
arc from the DXF drawn in red at the scale the sketch was traced at (5.74 px per
inch, property line at x=1315, sidewalk crossing at y=2170). Red on ink is a
match; red beside ink is a place to re-measure.
"""
import math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE, ORIGIN_X, ORIGIN_Y = 5.74, 1315, 2170

def to_pixels(x, y):
    return (ORIGIN_X + SCALE * x, ORIGIN_Y - SCALE * y)

def entities(path):
    text = open(path, encoding="utf-8", errors="replace").read().replace("\r", "")
    lines = [l.strip() for l in text.split("\n")]
    i = 0
    while i < len(lines) - 1:
        if lines[i] == "0" and lines[i + 1] in ("LWPOLYLINE", "ARC", "LINE"):
            kind, j, groups = lines[i + 1], i + 2, {}
            # Bulges are kept in vertex order: a 42 belongs to the 10 before it.
            order = []
            while j < len(lines) - 1 and lines[j] != "0":
                code, value = lines[j], lines[j + 1]
                groups.setdefault(code, []).append(float(value) if code in
                                                   ("10", "20", "11", "21", "40", "42", "50", "51") else value)
                if code in ("10", "42"):
                    order.append((code, float(value)))
                j += 2
            groups["_order"] = order
            yield kind, groups
            i = j
        else:
            i += 2


def text_groups(kind, g, xs):
    return g.get("_order", [])


def bulges_of(order):
    """One bulge per vertex from the ordered 10/42 groups; zero where absent."""
    bulges = []
    for code, value in order:
        if code == "10":
            bulges.append(0.0)
        elif bulges:
            bulges[-1] = value
    return bulges


def arc_points(start, end, bulge, steps=24):
    """Points along the arc from start to end with the given bulge, excluding the ends."""
    (x1, y1), (x2, y2) = start, end
    chord = math.hypot(x2 - x1, y2 - y1)
    if chord == 0.0:
        return []
    theta = 4.0 * math.atan(bulge)            # included angle, signed
    radius = chord / (2.0 * math.sin(abs(theta) / 2.0))
    mx, my = (x1 + x2) / 2.0, (y1 + y2) / 2.0
    # Centre: from the chord midpoint, perpendicular, on the side the bulge says.
    d = math.sqrt(max(radius * radius - (chord / 2.0) ** 2, 0.0))
    nx, ny = -(y2 - y1) / chord, (x2 - x1) / chord
    sign = 1.0 if abs(theta) <= math.pi else -1.0
    if bulge < 0:
        sign = -sign
    cx, cy = mx + sign * d * nx, my + sign * d * ny
    a0 = math.atan2(y1 - cy, x1 - cx)
    return [to_pixels(cx + radius * math.cos(a0 + theta * k / steps),
                      cy + radius * math.sin(a0 + theta * k / steps)) for k in range(1, steps)]

def main():
    scan = Image.open(os.path.join(HERE, "Original Sketch.tif")).convert("RGB")
    draw = ImageDraw.Draw(scan)
    count = 0
    for kind, g in entities(os.path.join(HERE, "Exhibit-A-Landscape-Agreement.dxf")):
        if kind == "LWPOLYLINE":
            # Bulged segments are arcs; each is drawn as such, not as its chord.
            points = []
            xs, ys = g["10"], g["20"]
            bulges = bulges_of(text_groups(kind, g, xs))
            for i, (x, y) in enumerate(zip(xs, ys)):
                points.append(to_pixels(x, y))
                if i + 1 < len(xs) and bulges[i] != 0.0:
                    points += arc_points((x, y), (xs[i + 1], ys[i + 1]), bulges[i])
        elif kind == "LINE":
            points = [to_pixels(g["10"][0], g["20"][0]), to_pixels(g["11"][0], g["21"][0])]
        else:
            cx, cy, r, a0, a1 = g["10"][0], g["20"][0], g["40"][0], g["50"][0], g["51"][0]
            if a1 < a0:
                a1 += 360
            points = [to_pixels(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
                      for a in (a0 + (a1 - a0) * k / 200 for k in range(201))]
        if len(points) > 1:
            draw.line(points, fill=(255, 0, 0), width=5)
            count += 1
    out = os.path.join(HERE, "overlay.png")
    scan.crop((900, 400, 2450, 2450)).resize((1085, 1435)).save(out)
    print(f"{count} entities drawn over the scan -> {out}")

if __name__ == "__main__":
    main()
