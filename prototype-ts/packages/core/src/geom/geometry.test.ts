import { describe, expect, it } from 'vitest';
import { Vec3, v } from '../math/vec3.js';
import { Mat4 } from '../math/mat4.js';
import { Box3 } from '../math/box3.js';
import { TAU, degToRad, normalizeAngle } from '../math/angle.js';
import { LineSegment } from './line-segment.js';
import { Arc } from './arc.js';
import { EllipseArc } from './ellipse-arc.js';
import { Polyline, polylineVertex } from './polyline.js';
import { arbitraryAxis, pointOcsToWorld, pointWorldToOcs } from './ocs.js';
import {
  intersectArcArc,
  intersectCurves,
  intersectCurvesExtended,
  intersectLineArc,
  intersectLineLine,
  selfIntersections,
} from './intersect.js';

/** Assert two points are equal to within a generous drafting tolerance. */
function expectPoint(actual: Vec3, expected: Vec3, precision = 9): void {
  expect(actual.x).toBeCloseTo(expected.x, precision);
  expect(actual.y).toBeCloseTo(expected.y, precision);
  expect(actual.z).toBeCloseTo(expected.z, precision);
}

describe('Vec3', () => {
  it('computes basic vector algebra', () => {
    const a = v(3, 4);
    expect(a.length).toBeCloseTo(5);
    expect(a.dot(v(1, 0))).toBe(3);
    expect(a.cross2(v(1, 0))).toBe(-4);
    expect(a.normalize().length).toBeCloseTo(1);
  });

  it('refuses to normalize a zero vector but offers a null-returning form', () => {
    expect(() => Vec3.ZERO.normalize()).toThrow(RangeError);
    expect(Vec3.ZERO.normalizeOrNull()).toBeNull();
  });

  it('rotates about a centre point', () => {
    const p = v(1, 0).rotateAbout(Vec3.ZERO, Math.PI / 2);
    expectPoint(p, v(0, 1));
  });

  it('distinguishes parallel from codirectional', () => {
    const a = v(1, 0);
    const b = v(-1, 0);
    expect(a.isParallel(b)).toBe(true);
    expect(a.isCodirectional(b)).toBe(false);
    expect(a.isPerpendicular(v(0, 5))).toBe(true);
  });
});

describe('Mat4', () => {
  it('composes transforms in the documented order', () => {
    // Scale then rotate then translate, applied to the unit X point.
    const m = Mat4.blockTransform(v(10, 0), v(2, 2, 1), Math.PI / 2);
    expectPoint(m.transformPoint(v(1, 0)), v(10, 2));
  });

  it('inverts and round-trips a point', () => {
    const m = Mat4.translation(v(5, -3)).multiply(Mat4.rotationZ(0.7)).multiply(Mat4.scaling(3));
    const inv = m.invert();
    expect(inv).not.toBeNull();
    expectPoint(inv!.transformPoint(m.transformPoint(v(2, 9))), v(2, 9));
  });

  it('reports a singular matrix as non-invertible rather than throwing', () => {
    expect(Mat4.scaling(0).invert()).toBeNull();
  });

  it('detects mirroring and conformality', () => {
    expect(Mat4.mirror2(Vec3.ZERO, Vec3.UNIT_X).isMirrored).toBe(true);
    expect(Mat4.rotationZ(1.1).isConformal()).toBe(true);
    expect(Mat4.scaling(v(2, 3, 1)).isConformal()).toBe(false);
    expect(Mat4.scaling(2).isConformal()).toBe(true);
  });

  it('mirrors across an arbitrary line', () => {
    // Mirror across the 45-degree line through the origin swaps X and Y.
    const m = Mat4.mirror2(Vec3.ZERO, v(1, 1));
    expectPoint(m.transformPoint(v(3, 0)), v(0, 3));
  });
});

describe('Box3', () => {
  it('treats the empty box as a union identity', () => {
    const b = Box3.fromPoint(v(1, 1));
    expect(Box3.EMPTY.union(b).equals(b)).toBe(true);
    expect(Box3.EMPTY.isEmpty).toBe(true);
  });

  it('transforms by corners so rotation is handled correctly', () => {
    const box = new Box3(v(-1, -1), v(1, 1));
    const rotated = box.transform(Mat4.rotationZ(Math.PI / 4));
    // A rotated unit square has a half-diagonal extent of sqrt(2).
    expect(rotated.max.x).toBeCloseTo(Math.SQRT2);
  });

  it('separates containment from intersection for window vs crossing selection', () => {
    const outer = new Box3(v(0, 0), v(10, 10));
    const inner = new Box3(v(2, 2), v(3, 3));
    const straddling = new Box3(v(9, 9), v(20, 20));
    expect(outer.containsBox(inner)).toBe(true);
    expect(outer.containsBox(straddling)).toBe(false);
    expect(outer.intersects(straddling)).toBe(true);
  });
});

describe('LineSegment', () => {
  const seg = new LineSegment(v(0, 0), v(10, 0));

  it('parameterizes linearly over [0, 1]', () => {
    expectPoint(seg.pointAt(0.25), v(2.5, 0));
    expect(seg.length).toBe(10);
  });

  it('clamps the closest point to the segment extent', () => {
    const r = seg.closestPoint(v(-5, 3));
    expectPoint(r.point, v(0, 0));
    expect(r.param).toBe(0);
    expect(r.distance).toBeCloseTo(Math.hypot(5, 3));
  });

  it('reports unclamped parameters on the extension', () => {
    const r = seg.closestPointExtended(v(-5, 3));
    expect(r).not.toBeNull();
    expect(r!.param).toBeCloseTo(-0.5);
    expectPoint(r!.point, v(-5, 0));
  });

  it('extends to a point on its own line and rejects points off it', () => {
    const extended = seg.extendTo(v(15, 0));
    expect(extended).not.toBeNull();
    expectPoint(extended!.end, v(15, 0));
    expectPoint(extended!.start, v(0, 0));
    expect(seg.extendTo(v(15, 4))).toBeNull();
  });

  it('splits at interior parameters and ignores out-of-range ones', () => {
    const parts = seg.split([0.5, 1.5, -0.2]);
    expect(parts).toHaveLength(2);
    expectPoint(parts[0]!.end, v(5, 0));
  });

  it('handles zero-length segments without dividing by zero', () => {
    const degenerate = new LineSegment(v(1, 1), v(1, 1));
    expect(degenerate.isDegenerate()).toBe(true);
    expect(degenerate.closestPoint(v(4, 5)).distance).toBeCloseTo(5);
    expect(degenerate.closestPointExtended(v(4, 5))).toBeNull();
  });
});

describe('Arc', () => {
  it('sweeps counter-clockwise and reports arc length', () => {
    const arc = new Arc(Vec3.ZERO, 2, 0, Math.PI / 2);
    expect(arc.sweep).toBeCloseTo(Math.PI / 2);
    expect(arc.length).toBeCloseTo(Math.PI);
    expectPoint(arc.startPoint, v(2, 0));
    expectPoint(arc.endPoint, v(0, 2));
    expectPoint(arc.midPoint, v(Math.SQRT2, Math.SQRT2));
  });

  it('represents a full circle distinctly from a zero sweep', () => {
    const circle = Arc.circle(Vec3.ZERO, 3);
    expect(circle.isClosed).toBe(true);
    expect(circle.sweep).toBeCloseTo(TAU);
    expect(circle.length).toBeCloseTo(TAU * 3);
    expect(circle.signedArea).toBeCloseTo(Math.PI * 9);
  });

  it('computes exact bounds for a partial arc rather than the full circle', () => {
    // First-quadrant arc: bounds are the unit square, not the whole circle.
    const arc = new Arc(Vec3.ZERO, 1, 0, Math.PI / 2);
    const b = arc.bounds;
    expect(b.min.x).toBeCloseTo(0);
    expect(b.min.y).toBeCloseTo(0);
    expect(b.max.x).toBeCloseTo(1);
    expect(b.max.y).toBeCloseTo(1);
  });

  it('builds from three points and passes through the middle one', () => {
    const arc = Arc.fromThreePoints(v(1, 0), v(0, 1), v(-1, 0));
    expect(arc).not.toBeNull();
    expectPoint(arc!.center, Vec3.ZERO);
    expect(arc!.radius).toBeCloseTo(1);
    expect(arc!.containsAngle(Math.PI / 2)).toBe(true);
  });

  it('returns null for collinear three-point input', () => {
    expect(Arc.fromThreePoints(v(0, 0), v(1, 0), v(2, 0))).toBeNull();
  });

  it('round-trips through the DXF bulge representation', () => {
    const original = new Arc(v(0, 0), 5, degToRad(30), degToRad(150));
    const rebuilt = Arc.fromBulge(original.startPoint, original.endPoint, original.bulge);
    expect(rebuilt).not.toBeNull();
    expectPoint(rebuilt!.center, original.center, 6);
    expect(rebuilt!.radius).toBeCloseTo(original.radius, 6);
    expect(rebuilt!.sweep).toBeCloseTo(original.sweep, 6);
  });

  it('follows the DXF bulge sign convention', () => {
    // Positive bulge sweeps counter-clockwise from start to end. For a chord
    // running left to right that puts the centre above and bows the arc below.
    const positive = Arc.fromBulge(v(0, 0), v(10, 0), 0.5)!;
    expect(positive.center.y).toBeGreaterThan(0);
    expect(positive.midPoint.y).toBeLessThan(0);
    expectPoint(positive.startPoint, v(0, 0), 6);
    expectPoint(positive.endPoint, v(10, 0), 6);
    // Included angle is 4*atan(bulge).
    expect(positive.sweep).toBeCloseTo(4 * Math.atan(0.5), 9);

    // Negative bulge is the exact mirror.
    const negative = Arc.fromBulge(v(0, 0), v(10, 0), -0.5)!;
    expect(negative.center.y).toBeCloseTo(-positive.center.y, 9);
    expect(negative.midPoint.y).toBeGreaterThan(0);
    expect(negative.radius).toBeCloseTo(positive.radius, 9);

    // A bulge of 1 is exactly a semicircle.
    const semi = Arc.fromBulge(v(0, 0), v(10, 0), 1)!;
    expect(semi.sweep).toBeCloseTo(Math.PI, 9);
    expect(semi.radius).toBeCloseTo(5, 9);
    expectPoint(semi.center, v(5, 0), 9);
  });

  it('tessellates to within the requested chord tolerance', () => {
    const arc = Arc.circle(Vec3.ZERO, 100);
    const pts = arc.tessellate({ chordTolerance: 0.01 });
    for (let i = 0; i < pts.length - 1; i++) {
      const mid = pts[i]!.midpoint(pts[i + 1]!);
      // Sagitta = r - |midpoint of chord|, and must not exceed the tolerance.
      expect(100 - mid.length).toBeLessThanOrEqual(0.0101);
    }
  });

  it('keeps a circle circular under a uniform scale', () => {
    const scaled = Arc.circle(Vec3.ZERO, 2).transform(Mat4.scaling(3));
    expect(scaled).toBeInstanceOf(Arc);
    expect((scaled as Arc).radius).toBeCloseTo(6);
  });

  it('widens to an ellipse under a non-uniform scale instead of distorting', () => {
    const scaled = Arc.circle(Vec3.ZERO, 2).transform(Mat4.scaling(v(3, 1, 1)));
    expect(scaled).toBeInstanceOf(EllipseArc);
    const e = scaled as EllipseArc;
    expect(e.semiMajor).toBeCloseTo(6);
    expect(e.semiMinor).toBeCloseTo(2);
  });

  it('preserves visible geometry when mirrored', () => {
    const arc = new Arc(Vec3.ZERO, 1, 0, Math.PI / 2);
    const mirrored = arc.transform(Mat4.mirror2(Vec3.ZERO, Vec3.UNIT_X)) as Arc;
    // Mirroring across X maps the endpoints; the class keeps CCW by swapping.
    expectPoint(mirrored.startPoint, v(0, -1));
    expectPoint(mirrored.endPoint, v(1, 0));
    expect(mirrored.sweep).toBeCloseTo(Math.PI / 2);
  });

  it('reports only the quadrant points actually on the arc', () => {
    expect(new Arc(Vec3.ZERO, 1, 0, Math.PI / 2).quadrantPoints).toHaveLength(2);
    expect(Arc.circle(Vec3.ZERO, 1).quadrantPoints).toHaveLength(4);
  });
});

describe('EllipseArc', () => {
  it('reports axes, area and length for a full ellipse', () => {
    const e = new EllipseArc(Vec3.ZERO, v(4, 0), 0.5);
    expect(e.semiMajor).toBeCloseTo(4);
    expect(e.semiMinor).toBeCloseTo(2);
    expect(e.isClosed).toBe(true);
    expect(e.signedArea).toBeCloseTo(Math.PI * 8);
    // Ramanujan's approximation for the perimeter, as an independent check.
    const a = 4;
    const b = 2;
    const h = ((a - b) ** 2) / ((a + b) ** 2);
    const approx = Math.PI * (a + b) * (1 + (3 * h) / (10 + Math.sqrt(4 - 3 * h)));
    expect(e.length).toBeCloseTo(approx, 3);
  });

  it('matches a circle when the ratio is 1', () => {
    const e = new EllipseArc(Vec3.ZERO, v(5, 0), 1);
    expect(e.length).toBeCloseTo(TAU * 5, 6);
    expect(e.isCircular()).toBe(true);
    expect(e.toArcIfCircular()?.radius).toBeCloseTo(5);
  });

  it('finds the closest point on an eccentric ellipse', () => {
    const e = new EllipseArc(Vec3.ZERO, v(10, 0), 0.1);
    // Directly above the centre, the nearest point is the top of the minor axis.
    const r = e.closestPoint(v(0, 5));
    expectPoint(r.point, v(0, 1), 6);
    expect(r.distance).toBeCloseTo(4, 6);
  });

  it('recovers true axes after a shear via Rytz construction', () => {
    const e = new EllipseArc(Vec3.ZERO, v(3, 0), 1);
    // Shear a circle: the result must still be a valid ellipse with
    // perpendicular principal axes.
    const shear = Mat4.fromColumnMajor([1, 0, 0, 0, 0.5, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    const sheared = e.transform(shear);
    expect(sheared.majorAxis.isPerpendicular(sheared.minorAxis)).toBe(true);
    expect(sheared.semiMajor).toBeGreaterThanOrEqual(sheared.semiMinor);
    // A shear preserves area: pi*r^2 for the original circle.
    expect(Math.abs(sheared.signedArea)).toBeCloseTo(Math.PI * 9, 6);
  });

  it('bounds a rotated ellipse tightly', () => {
    const e = new EllipseArc(Vec3.ZERO, v(4, 0), 0.5).transform(Mat4.rotationZ(Math.PI / 2));
    const b = e.bounds;
    expect(b.max.x).toBeCloseTo(2, 6);
    expect(b.max.y).toBeCloseTo(4, 6);
  });
});

describe('Polyline', () => {
  const square = Polyline.rectangle(v(0, 0), v(10, 10));

  it('closes and measures a rectangle', () => {
    expect(square.isClosed).toBe(true);
    expect(square.segmentCount).toBe(4);
    expect(square.length).toBeCloseTo(40);
    expect(square.area).toBeCloseTo(100);
    expect(square.isCounterClockwise).toBe(true);
  });

  it('indexes parameters by segment so vertices land exactly', () => {
    expectPoint(square.pointAt(0.25), v(10, 0));
    expectPoint(square.pointAt(0.5), v(10, 10));
  });

  it('maps arc-length queries independently of the segment parameterization', () => {
    const poly = Polyline.fromPoints([v(0, 0), v(1, 0), v(11, 0)]);
    // Half the total length (5.5) falls inside the long second segment.
    expectPoint(poly.pointAt(poly.paramAtLength(5.5)), v(5.5, 0));
    expect(poly.lengthAtParam(poly.paramAtLength(5.5))).toBeCloseTo(5.5);
  });

  it('builds arc segments from bulges', () => {
    const poly = new Polyline(
      [polylineVertex(v(0, 0), 1), polylineVertex(v(10, 0))],
      false,
    );
    expect(poly.hasArcs).toBe(true);
    expect(poly.segmentAt(0)).toBeInstanceOf(Arc);
    // A bulge of 1 is a half circle: length is pi * r with r = 5.
    expect(poly.length).toBeCloseTo(Math.PI * 5);
  });

  it('includes circular segment area for bulged spans', () => {
    // A closed shape: straight base plus a semicircular cap.
    const poly = new Polyline(
      [polylineVertex(v(0, 0), 1), polylineVertex(v(10, 0))],
      true,
    );
    // Semicircle of radius 5 above the base.
    expect(poly.area).toBeCloseTo((Math.PI * 25) / 2, 6);
  });

  it('reverses while preserving geometry', () => {
    const poly = new Polyline(
      [polylineVertex(v(0, 0), 0.5), polylineVertex(v(10, 0), 0), polylineVertex(v(10, 10))],
      false,
    );
    const rev = poly.reverse();
    expectPoint(rev.startPoint, poly.endPoint);
    expectPoint(rev.endPoint, poly.startPoint);
    expect(rev.length).toBeCloseTo(poly.length, 9);
  });

  it('performs point-in-polygon tests on closed shapes', () => {
    expect(square.containsPointInside(v(5, 5))).toBe(true);
    expect(square.containsPointInside(v(15, 5))).toBe(false);
  });

  it('flips bulge sign under a mirror so arcs bow the right way', () => {
    const poly = new Polyline([polylineVertex(v(0, 0), 1), polylineVertex(v(10, 0))], false);
    const mirrored = poly.transform(Mat4.mirror2(Vec3.ZERO, Vec3.UNIT_X));
    expect(mirrored.vertices[0]!.bulge).toBeCloseTo(-1);
    expect(mirrored.length).toBeCloseTo(poly.length);
  });

  it('trims across multiple segments', () => {
    const trimmed = square.trim(0.125, 0.625);
    expect(trimmed.isClosed).toBe(false);
    expectPoint(trimmed.startPoint, v(5, 0));
    expectPoint(trimmed.endPoint, v(5, 10));
    expect(trimmed.length).toBeCloseTo(20);
  });

  it('finds the closest point across all segments', () => {
    const r = square.closestPoint(v(5, 12));
    expectPoint(r.point, v(5, 10));
    expect(r.distance).toBeCloseTo(2);
  });

  it('tolerates degenerate vertex counts', () => {
    expect(new Polyline([]).isEmpty).toBe(true);
    // A lone vertex has no closing segment, so the closed flag is dropped.
    const single = new Polyline([polylineVertex(v(1, 1))], true);
    expect(single.length).toBe(0);
    expect(single.isClosed).toBe(false);

    // Two vertices may close: with no bulge it degenerates to zero area, but
    // the flag is preserved because DWG round-tripping depends on it.
    const flat = new Polyline([polylineVertex(v(0, 0)), polylineVertex(v(1, 0))], true);
    expect(flat.isClosed).toBe(true);
    expect(flat.area).toBeCloseTo(0);
  });

  it('closes a full circle built from two opposing arc vertices', () => {
    // Two semicircles: the representation other CAD software emits for a circular PLINE.
    const circle = new Polyline(
      [polylineVertex(v(0, 0), 1), polylineVertex(v(10, 0), 1)],
      true,
    );
    expect(circle.segmentCount).toBe(2);
    expect(circle.length).toBeCloseTo(TAU * 5, 6);
    expect(circle.area).toBeCloseTo(Math.PI * 25, 6);
  });
});

describe('OCS / arbitrary axis algorithm', () => {
  it('is the identity for a world Z extrusion', () => {
    const frame = arbitraryAxis(Vec3.UNIT_Z);
    expectPoint(frame.xAxis, Vec3.UNIT_X);
    expectPoint(frame.yAxis, Vec3.UNIT_Y);
  });

  it('produces an orthonormal right-handed frame for a tilted normal', () => {
    const n = v(0.3, -0.7, 0.5).normalize();
    const { xAxis, yAxis, zAxis } = arbitraryAxis(n);
    expect(xAxis.length).toBeCloseTo(1);
    expect(yAxis.length).toBeCloseTo(1);
    expect(xAxis.dot(yAxis)).toBeCloseTo(0);
    expectPoint(xAxis.cross(yAxis), zAxis, 9);
  });

  it('switches reference axis below the 1/64 threshold', () => {
    // A near-horizontal normal must use world Y as the reference.
    const frame = arbitraryAxis(v(0.001, 0.001, 1).normalize());
    expect(frame.xAxis.dot(Vec3.UNIT_Y)).toBeCloseTo(0, 2);
  });

  it('round-trips points through OCS and back', () => {
    const n = v(1, 2, 3).normalize();
    const world = v(5, -2, 7);
    expectPoint(pointOcsToWorld(pointWorldToOcs(world, n), n), world, 9);
  });

  it('places a mirrored-UCS circle at the correct world location', () => {
    // A -Z extrusion mirrors the X axis, which is the classic DWG gotcha.
    const n = v(0, 0, -1);
    expectPoint(pointOcsToWorld(v(10, 0, 0), n), v(-10, 0, 0), 9);
  });
});

describe('Intersections', () => {
  it('finds a crossing between two segments', () => {
    const hits = intersectLineLine(
      new LineSegment(v(0, 0), v(10, 0)),
      new LineSegment(v(5, -5), v(5, 5)),
    );
    expect(hits).toHaveLength(1);
    expectPoint(hits[0]!.point, v(5, 0));
    expect(hits[0]!.onExtensionA).toBe(false);
  });

  it('flags hits that lie beyond the segment extents', () => {
    const a = new LineSegment(v(0, 0), v(1, 0));
    const b = new LineSegment(v(5, -5), v(5, 5));
    expect(intersectCurves(a, b)).toHaveLength(0);
    const extended = intersectCurvesExtended(a, b);
    expect(extended).toHaveLength(1);
    expect(extended[0]!.onExtensionA).toBe(true);
    expect(extended[0]!.onExtensionB).toBe(false);
    expectPoint(extended[0]!.point, v(5, 0));
  });

  it('reports no discrete intersection for parallel or collinear lines', () => {
    expect(
      intersectLineLine(new LineSegment(v(0, 0), v(10, 0)), new LineSegment(v(0, 1), v(10, 1))),
    ).toHaveLength(0);
    expect(
      intersectLineLine(new LineSegment(v(0, 0), v(10, 0)), new LineSegment(v(5, 0), v(15, 0))),
    ).toHaveLength(0);
  });

  it('finds both line/circle crossings', () => {
    const hits = intersectLineArc(
      new LineSegment(v(-10, 0), v(10, 0)),
      Arc.circle(Vec3.ZERO, 5),
    );
    expect(hits).toHaveLength(2);
    const xs = hits.map((h) => h.point.x).sort((p, q) => p - q);
    expect(xs[0]).toBeCloseTo(-5);
    expect(xs[1]).toBeCloseTo(5);
  });

  it('collapses a tangent line/circle hit to a single point', () => {
    const hits = intersectCurves(
      new LineSegment(v(-10, 5), v(10, 5)),
      Arc.circle(Vec3.ZERO, 5),
    );
    expect(hits).toHaveLength(1);
    expectPoint(hits[0]!.point, v(0, 5), 6);
  });

  it('respects the arc sweep when reporting circle hits', () => {
    // A first-quadrant arc is not crossed by a line through the third quadrant.
    const arc = new Arc(Vec3.ZERO, 5, 0, Math.PI / 2);
    const line = new LineSegment(v(-10, -1), v(10, -1));
    expect(intersectCurves(line, arc)).toHaveLength(0);
  });

  it('finds the two intersections of overlapping circles', () => {
    const hits = intersectArcArc(Arc.circle(Vec3.ZERO, 5), Arc.circle(v(8, 0), 5));
    expect(hits).toHaveLength(2);
    for (const h of hits) {
      expect(h.point.x).toBeCloseTo(4);
      expect(Math.abs(h.point.y)).toBeCloseTo(3);
    }
  });

  it('reports no hits for concentric, disjoint or nested circles', () => {
    expect(intersectArcArc(Arc.circle(Vec3.ZERO, 5), Arc.circle(Vec3.ZERO, 7))).toHaveLength(0);
    expect(intersectArcArc(Arc.circle(Vec3.ZERO, 1), Arc.circle(v(50, 0), 1))).toHaveLength(0);
    expect(intersectArcArc(Arc.circle(Vec3.ZERO, 10), Arc.circle(v(1, 0), 2))).toHaveLength(0);
  });

  it('handles externally tangent circles as one point', () => {
    const hits = intersectArcArc(Arc.circle(Vec3.ZERO, 5), Arc.circle(v(10, 0), 5));
    expect(hits).toHaveLength(1);
    expectPoint(hits[0]!.point, v(5, 0), 6);
  });

  it('intersects a polyline and remaps parameters onto the whole curve', () => {
    const square = Polyline.rectangle(v(0, 0), v(10, 10));
    const diagonal = new LineSegment(v(-5, 5), v(15, 5));
    const hits = intersectCurves(diagonal, square);
    expect(hits).toHaveLength(2);
    const ys = hits.map((h) => h.point.x).sort((p, q) => p - q);
    expect(ys[0]).toBeCloseTo(0);
    expect(ys[1]).toBeCloseTo(10);
    for (const h of hits) {
      expect(h.paramB).toBeGreaterThanOrEqual(0);
      expect(h.paramB).toBeLessThanOrEqual(1);
      // The remapped parameter must actually locate the hit on the polyline.
      expectPoint(square.pointAt(h.paramB), h.point, 6);
    }
  });

  it('intersects an ellipse numerically to high precision', () => {
    const ellipse = new EllipseArc(Vec3.ZERO, v(10, 0), 0.5);
    const line = new LineSegment(v(0, -10), v(0, 10));
    const hits = intersectCurves(line, ellipse);
    expect(hits).toHaveLength(2);
    const ys = hits.map((h) => h.point.y).sort((p, q) => p - q);
    expect(ys[0]).toBeCloseTo(-5, 6);
    expect(ys[1]).toBeCloseTo(5, 6);
  });

  it('detects polyline self-intersection but not adjacent-segment joints', () => {
    // A bow-tie crosses itself once in the middle.
    const bowtie = Polyline.fromPoints([v(0, 0), v(10, 10), v(10, 0), v(0, 10)]);
    const hits = selfIntersections(bowtie);
    expect(hits).toHaveLength(1);
    expectPoint(hits[0]!.point, v(5, 5), 6);

    // A simple rectangle has none, despite every corner being a segment meeting.
    expect(selfIntersections(Polyline.rectangle(v(0, 0), v(5, 5)))).toHaveLength(0);
  });
});

describe('Angle helpers', () => {
  it('normalizes into the documented ranges', () => {
    expect(normalizeAngle(-Math.PI / 2)).toBeCloseTo((3 * Math.PI) / 2);
    expect(normalizeAngle(TAU + 1)).toBeCloseTo(1);
  });
});
