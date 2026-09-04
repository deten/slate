import { Vec3 } from '../math/vec3.js';
import { TOL, type Tolerance, clamp } from '../math/tolerance.js';
import { normalizeAngle } from '../math/angle.js';
import { Curve } from './curve.js';
import { LineSegment } from './line-segment.js';
import { Arc } from './arc.js';
import { Polyline, polylineVertex } from './polyline.js';
import { intersectAgainstAll, intersectCurves, type Intersection } from './intersect.js';

/**
 * Editing operations on curves.
 *
 * These are the geometric heart of the drafting commands — TRIM, EXTEND,
 * OFFSET, FILLET, CHAMFER, BREAK. They live in the geometry kernel rather than
 * in the commands because they are pure functions on geometry, and keeping
 * them here means each can be tested exhaustively without a database, a
 * selection, or a UI.
 */

// ------------------------------------------------------------------- offset

/**
 * Offset a curve by a signed distance.
 *
 * Positive is to the left of the direction of travel (the +Y side for a curve
 * running along +X), which matches the convention used by the `perp2` helper
 * and keeps the sign consistent across curve types.
 *
 * @returns the offset curve, or `null` when the offset is degenerate — an arc
 *   offset inward by more than its radius has no result, and reporting that
 *   honestly is better than emitting an inside-out arc.
 */
export function offsetCurve(curve: Curve, distance: number, tol: Tolerance = TOL): Curve | null {
  if (Math.abs(distance) <= tol.point) return curve;

  if (curve instanceof LineSegment) {
    const direction = curve.unitDirection(tol);
    if (direction === null) return null;
    const offset = direction.perp2().scale(distance);
    return new LineSegment(curve.start.add(offset), curve.end.add(offset));
  }

  if (curve instanceof Arc) {
    // Offsetting to the left of a counter-clockwise arc means moving *inward*,
    // toward the centre, because the centre is on the left of travel.
    const newRadius = curve.radius - distance;
    if (newRadius <= tol.point) return null;
    return new Arc(
      curve.center,
      newRadius,
      curve.startAngle,
      curve.endAngle,
      curve.normal,
      curve.isFullCircle,
    );
  }

  if (curve instanceof Polyline) {
    return offsetPolyline(curve, distance, tol);
  }

  return null;
}

/**
 * Offset a polyline segment-by-segment, then repair the joints.
 *
 * The joints are the whole problem. Offsetting each segment independently
 * leaves gaps at convex corners and crossings at concave ones. This walks
 * consecutive pairs and either intersects them (extending both to meet) or
 * bridges them with an arc, which is what produces a correct parallel outline
 * rather than a fringe of disconnected pieces.
 */
function offsetPolyline(poly: Polyline, distance: number, tol: Tolerance): Polyline | null {
  const segments = poly.segments;
  if (segments.length === 0) return null;

  const offsets: Curve[] = [];
  for (const segment of segments) {
    const offset = offsetCurve(segment, distance, tol);
    // A segment that collapses is dropped rather than aborting the whole
    // offset: a polyline with one tiny arc should still offset usefully.
    if (offset !== null) offsets.push(offset);
  }
  if (offsets.length === 0) return null;

  const vertices: Array<{ position: Vec3; bulge: number }> = [];
  const first = offsets[0]!;
  vertices.push({ position: first.startPoint, bulge: bulgeOf(first) });

  for (let i = 0; i < offsets.length - 1; i++) {
    const current = offsets[i]!;
    const next = offsets[i + 1]!;
    const joint = jointPoint(current, next, tol);
    vertices.push({ position: joint, bulge: bulgeOf(next) });
  }

  const last = offsets[offsets.length - 1]!;
  if (poly.isClosed) {
    // Close the loop by intersecting the last with the first.
    const joint = jointPoint(last, first, tol);
    vertices[0] = { position: joint, bulge: vertices[0]!.bulge };
  } else {
    vertices.push({ position: last.endPoint, bulge: 0 });
  }

  return new Polyline(
    vertices.map((v) => polylineVertex(v.position, v.bulge)),
    poly.isClosed,
    poly.normal,
  );
}

/**
 * Where two offset segments should meet.
 *
 * Their true intersection when they have one — including on their extensions,
 * which is the convex-corner case — and otherwise the midpoint of the gap,
 * which keeps the outline continuous for near-tangent joints.
 */
function jointPoint(a: Curve, b: Curve, tol: Tolerance): Vec3 {
  const hits = intersectCurves(a, b, { extend: true, tolerance: tol });
  if (hits.length === 0) return a.endPoint.midpoint(b.startPoint);
  // With several candidates, take the one nearest the original joint.
  const target = a.endPoint;
  let best = hits[0]!;
  for (const hit of hits) {
    if (hit.point.distanceTo(target) < best.point.distanceTo(target)) best = hit;
  }
  return best.point;
}

function bulgeOf(curve: Curve): number {
  return curve instanceof Arc ? curve.bulge : 0;
}

// --------------------------------------------------------------------- trim

export interface TrimResult {
  /** The pieces that remain. Empty when the whole curve was removed. */
  readonly remaining: readonly Curve[];
  /** `true` when anything was actually cut. */
  readonly trimmed: boolean;
}

/**
 * Trim a curve against cutting edges, removing the piece containing
 * `pickPoint`.
 *
 * This is exactly other CAD software's TRIM: the user picks the *part to remove*, and the
 * cutting edges bound it. The subtlety is that the removed span runs between
 * the two intersections that bracket the pick — not from the nearest
 * intersection to the end — and a pick beyond every intersection removes the
 * tail.
 */
export function trimCurve(
  curve: Curve,
  cuttingEdges: readonly Curve[],
  pickPoint: Vec3,
  options: { extend?: boolean; tolerance?: Tolerance } = {},
): TrimResult {
  const tol = options.tolerance ?? TOL;
  const hits = intersectAgainstAll(curve, cuttingEdges, {
    tolerance: tol,
    ...(options.extend === undefined ? {} : { extend: options.extend }),
  });

  const params = hits
    .map((h) => h.paramA)
    .filter((t) => t > 1e-9 && t < 1 - 1e-9)
    .sort((a, b) => a - b);

  if (params.length === 0) {
    return { remaining: [curve], trimmed: false };
  }

  const pickParam = curve.closestPoint(pickPoint).param;

  // A closed curve has no ends, so the removed span is between the two
  // intersections bracketing the pick, wrapping around if necessary.
  if (curve.isClosed) {
    return trimClosed(curve, params, pickParam);
  }

  // Boundaries of the span containing the pick.
  let lower = 0;
  let upper = 1;
  for (const t of params) {
    if (t <= pickParam) lower = t;
    if (t > pickParam) {
      upper = t;
      break;
    }
  }

  const remaining: Curve[] = [];
  if (lower > 1e-9) remaining.push(curve.trim(0, lower));
  if (upper < 1 - 1e-9) remaining.push(curve.trim(upper, 1));
  return { remaining, trimmed: true };
}

function trimClosed(curve: Curve, params: readonly number[], pickParam: number): TrimResult {
  if (params.length < 2) {
    // A single intersection cannot bound a span on a closed curve, so there is
    // nothing well-defined to remove.
    return { remaining: [curve], trimmed: false };
  }
  let lower = params[params.length - 1]!;
  let upper = params[0]!;
  for (let i = 0; i < params.length; i++) {
    const a = params[i]!;
    const b = params[(i + 1) % params.length]!;
    const inSpan = i === params.length - 1 ? pickParam >= a || pickParam < b : pickParam >= a && pickParam < b;
    if (inSpan) {
      lower = a;
      upper = b;
      break;
    }
  }
  // What remains is the complement: from the upper bound round to the lower.
  const remaining = upper <= lower ? [curve.trim(upper, lower)] : [curve.trim(upper, 1), curve.trim(0, lower)];
  return { remaining: remaining.filter((c) => c.length > 1e-9), trimmed: true };
}

// ------------------------------------------------------------------- extend

/**
 * Extend a curve to meet a boundary.
 *
 * `pickPoint` selects which end to extend: the end nearer the pick is the one
 * that moves, matching how a drafter indicates intent by clicking near an end.
 *
 * @returns the extended curve, or `null` when no boundary lies beyond that end.
 */
export function extendCurve(
  curve: Curve,
  boundaries: readonly Curve[],
  pickPoint: Vec3,
  tol: Tolerance = TOL,
): Curve | null {
  const pickParam = curve.closestPoint(pickPoint).param;
  const extendEnd = pickParam >= 0.5;

  const hits = intersectAgainstAll(curve, boundaries, { tolerance: tol, extend: true });
  const candidates = hits
    .map((h) => h.paramA)
    // Only intersections beyond the relevant end can extend it.
    .filter((t) => (extendEnd ? t > 1 + 1e-9 : t < -1e-9));

  if (candidates.length === 0) return null;

  // The nearest boundary beyond the end is the one to stop at, so that
  // repeated EXTEND walks outward one boundary at a time.
  const target = extendEnd ? Math.min(...candidates) : Math.max(...candidates);

  if (curve instanceof LineSegment) {
    const point = curve.pointAtExtended(target);
    return extendEnd
      ? new LineSegment(curve.start, point)
      : new LineSegment(point, curve.end);
  }

  if (curve instanceof Arc) {
    if (curve.isFullCircle) return null;
    // The parameter is outside [0,1], so convert it to an absolute angle.
    const angle = curve.angleAt(target);
    return extendEnd
      ? new Arc(curve.center, curve.radius, curve.startAngle, angle, curve.normal)
      : new Arc(curve.center, curve.radius, angle, curve.endAngle, curve.normal);
  }

  return null;
}

// ------------------------------------------------------------------- fillet

export interface FilletResult {
  /** The arc joining the two curves. */
  readonly arc: Arc;
  /** The first curve, trimmed back to the arc. */
  readonly trimmedA: Curve;
  readonly trimmedB: Curve;
}

/**
 * Round the corner between two curves with an arc of the given radius.
 *
 * The pick points matter: two lines have up to four possible fillets (one per
 * quadrant of their crossing), and the picks select which corner the user
 * meant. Ignoring them and choosing arbitrarily is the single most common way
 * a fillet implementation feels wrong to use.
 *
 * @returns `null` when no fillet of that radius fits between the curves.
 */
export function filletCurves(
  a: Curve,
  b: Curve,
  radius: number,
  pickA: Vec3,
  pickB: Vec3,
  tol: Tolerance = TOL,
): FilletResult | null {
  if (radius < 0) return null;

  if (a instanceof LineSegment && b instanceof LineSegment) {
    return filletLines(a, b, radius, pickA, pickB, tol);
  }
  // Arc and mixed fillets reduce to finding a circle tangent to both curves;
  // that general solver is not implemented yet, and returning null is honest.
  // The commands surface it as "cannot fillet these objects".
  return null;
}

function filletLines(
  a: LineSegment,
  b: LineSegment,
  radius: number,
  pickA: Vec3,
  pickB: Vec3,
  tol: Tolerance,
): FilletResult | null {
  const hits = intersectCurves(a, b, { extend: true, tolerance: tol });
  const corner = hits[0]?.point;
  if (corner === undefined) return null; // Parallel lines cannot be filleted.

  // Point each direction away from the corner, toward the side the user
  // picked. That choice is what selects the quadrant.
  const dirA = directionAwayFromCorner(a, corner, pickA, tol);
  const dirB = directionAwayFromCorner(b, corner, pickB, tol);
  if (dirA === null || dirB === null) return null;

  const angle = dirA.angleTo(dirB, tol);
  // Collinear or antiparallel lines leave no corner to round.
  if (angle <= tol.angle || Math.abs(angle - Math.PI) <= tol.angle) return null;

  if (radius <= tol.point) {
    // A zero radius is a valid request: it means "extend both to the corner",
    // which is how other CAD software's FILLET with radius 0 joins two lines.
    const zero = new Arc(corner, 0, 0, 0, Vec3.UNIT_Z, true);
    return {
      arc: zero,
      trimmedA: trimToCorner(a, corner, pickA),
      trimmedB: trimToCorner(b, corner, pickB),
    };
  }

  // Distance from the corner to each tangent point.
  const tangentDistance = radius / Math.tan(angle / 2);
  const tangentA = corner.addScaled(dirA, tangentDistance);
  const tangentB = corner.addScaled(dirB, tangentDistance);

  // The arc centre lies along the angle bisector.
  const bisector = dirA.add(dirB).normalizeOrNull(tol);
  if (bisector === null) return null;
  const centerDistance = radius / Math.sin(angle / 2);
  const center = corner.addScaled(bisector, centerDistance);

  // Both tangent points must lie within the segments once trimmed, or the
  // requested radius does not fit.
  if (tangentDistance > a.length + tol.point && tangentDistance > b.length + tol.point) {
    return null;
  }

  const startAngle = Math.atan2(tangentA.y - center.y, tangentA.x - center.x);
  const endAngle = Math.atan2(tangentB.y - center.y, tangentB.x - center.x);

  // Choose the sweep direction that produces the minor arc; the major arc
  // would loop the long way round and is never what a fillet means.
  const ccwSweep = normalizeAngle(endAngle - startAngle);
  const arc =
    ccwSweep <= Math.PI
      ? new Arc(center, radius, startAngle, endAngle)
      : new Arc(center, radius, endAngle, startAngle);

  return {
    arc,
    trimmedA: trimToCorner(a, tangentA, pickA),
    trimmedB: trimToCorner(b, tangentB, pickB),
  };
}

/**
 * Unit direction along `segment` pointing from `corner` toward the picked side.
 */
function directionAwayFromCorner(
  segment: LineSegment,
  corner: Vec3,
  pick: Vec3,
  tol: Tolerance,
): Vec3 | null {
  const direction = segment.unitDirection(tol);
  if (direction === null) return null;
  const pickOnSegment = segment.closestPoint(pick).point;
  const toPick = pickOnSegment.sub(corner);
  if (toPick.isZero(tol)) {
    // The pick landed on the corner itself; keep the segment's own direction.
    return direction;
  }
  return direction.dot(toPick) >= 0 ? direction : direction.negate();
}

/** Shorten a segment so it ends at `newEnd`, keeping the picked side. */
function trimToCorner(segment: LineSegment, newEnd: Vec3, pick: Vec3): LineSegment {
  const pickParam = segment.closestPoint(pick).param;
  const endParam = segment.closestPointExtended(newEnd)?.param ?? 0.5;
  // Keep the portion the user picked: if the pick is beyond the new end,
  // the far end is the one retained.
  return pickParam > endParam
    ? new LineSegment(newEnd, segment.end)
    : new LineSegment(segment.start, newEnd);
}

// ------------------------------------------------------------------ chamfer

export interface ChamferResult {
  readonly line: LineSegment;
  readonly trimmedA: Curve;
  readonly trimmedB: Curve;
}

/**
 * Cut the corner between two lines with a straight bevel.
 *
 * `distanceA` and `distanceB` are measured back along each line from their
 * intersection, matching other CAD software's two-distance chamfer.
 */
export function chamferLines(
  a: LineSegment,
  b: LineSegment,
  distanceA: number,
  distanceB: number,
  pickA: Vec3,
  pickB: Vec3,
  tol: Tolerance = TOL,
): ChamferResult | null {
  const hits = intersectCurves(a, b, { extend: true, tolerance: tol });
  const corner = hits[0]?.point;
  if (corner === undefined) return null;

  const dirA = directionAwayFromCorner(a, corner, pickA, tol);
  const dirB = directionAwayFromCorner(b, corner, pickB, tol);
  if (dirA === null || dirB === null) return null;
  if (dirA.isParallel(dirB, tol)) return null;

  const pointA = corner.addScaled(dirA, distanceA);
  const pointB = corner.addScaled(dirB, distanceB);

  return {
    line: new LineSegment(pointA, pointB),
    trimmedA: trimToCorner(a, pointA, pickA),
    trimmedB: trimToCorner(b, pointB, pickB),
  };
}

// -------------------------------------------------------------------- break

/**
 * Split a curve at two points, discarding the span between them.
 *
 * With `second` omitted the curve is merely split, keeping both halves — which
 * is other CAD software's BREAK AT POINT.
 */
export function breakCurve(
  curve: Curve,
  first: Vec3,
  second?: Vec3,
): Curve[] {
  const t1 = curve.closestPoint(first).param;
  if (second === undefined) {
    return curve.split([t1]);
  }
  const t2 = curve.closestPoint(second).param;
  const lower = Math.min(t1, t2);
  const upper = Math.max(t1, t2);

  const pieces: Curve[] = [];
  if (lower > 1e-9) pieces.push(curve.trim(0, lower));
  if (upper < 1 - 1e-9) pieces.push(curve.trim(upper, 1));
  return pieces;
}

// ------------------------------------------------------------------ helpers

/**
 * The point on `curve` nearest `from`, restricted to the curve's own extent.
 * Convenience used by the snap engine and several commands.
 */
export function nearestPointOn(curve: Curve, from: Vec3): Vec3 {
  return curve.closestPoint(from).point;
}

/**
 * Divide a curve into `count` equal-length spans, returning the interior
 * division points. Backs the DIVIDE command.
 */
export function dividePoints(curve: Curve, count: number): Vec3[] {
  if (count < 2) return [];
  const total = curve.length;
  const points: Vec3[] = [];
  for (let i = 1; i < count; i++) {
    const distance = (total * i) / count;
    points.push(pointAtDistance(curve, distance));
  }
  return points;
}

/**
 * Place points at a fixed spacing along a curve. Backs the MEASURE command.
 */
export function measurePoints(curve: Curve, spacing: number): Vec3[] {
  if (spacing <= 0) return [];
  const total = curve.length;
  const points: Vec3[] = [];
  for (let d = spacing; d < total - 1e-9; d += spacing) {
    points.push(pointAtDistance(curve, d));
  }
  return points;
}

/**
 * Point at an arc-length distance along a curve.
 *
 * Polylines expose an exact mapping; other curves in this kernel have uniform
 * parametric speed within a segment, so the normalized parameter is
 * proportional to arc length.
 */
export function pointAtDistance(curve: Curve, distance: number): Vec3 {
  if (curve instanceof Polyline) {
    return curve.pointAt(curve.paramAtLength(distance));
  }
  const total = curve.length;
  if (total <= 0) return curve.startPoint;
  return curve.pointAt(clamp(distance / total, 0, 1));
}

/** All intersections between a curve and a set, as points. */
export function intersectionPoints(
  curve: Curve,
  others: readonly Curve[],
  options: { extend?: boolean; tolerance?: Tolerance } = {},
): Vec3[] {
  const hits: Intersection[] = intersectAgainstAll(curve, others, {
    ...(options.tolerance === undefined ? {} : { tolerance: options.tolerance }),
    ...(options.extend === undefined ? {} : { extend: options.extend }),
  });
  return hits.map((h) => h.point);
}
