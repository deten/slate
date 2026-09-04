import { Vec3 } from '../math/vec3.js';
import { TOL, type Tolerance, clamp } from '../math/tolerance.js';
import { Curve, DEFAULT_TESSELLATION, type TessellationOptions } from './curve.js';
import { LineSegment } from './line-segment.js';
import { Arc } from './arc.js';
import { EllipseArc } from './ellipse-arc.js';
import { Polyline } from './polyline.js';

/**
 * A single intersection between two curves.
 *
 * Parameters are normalized to each curve's own `[0, 1]` domain. When the hit
 * lies on a curve's *extension* rather than its bounded extent, the parameter
 * is outside `[0, 1]` and the corresponding `onExtension` flag is set. TRIM and
 * EXTEND both depend on this distinction, and so does other CAD software's "apparent
 * intersection" object snap.
 */
export interface Intersection {
  readonly point: Vec3;
  readonly paramA: number;
  readonly paramB: number;
  readonly onExtensionA: boolean;
  readonly onExtensionB: boolean;
}

export interface IntersectOptions {
  readonly tolerance?: Tolerance;
  /**
   * Include hits that fall on the infinite/complete host geometry beyond a
   * curve's own extent. Off by default: ordinary intersection queries should
   * only report real crossings.
   */
  readonly extend?: boolean;
  /** Tessellation quality for curve types with no closed-form solution. */
  readonly tessellation?: TessellationOptions;
}

/**
 * Intersect any two curves.
 *
 * Closed-form solutions are used for the pairs that dominate real drawings
 * (line/line, line/arc, arc/arc); everything else falls back to a tessellated
 * search refined by Newton iteration, which converges to full double precision
 * for the smooth curve types in this kernel.
 */
export function intersectCurves(
  a: Curve,
  b: Curve,
  options: IntersectOptions = {},
): Intersection[] {
  const tol = options.tolerance ?? TOL;
  const extend = options.extend ?? false;

  // Polylines decompose into their segments; this also covers polyline/anything
  // and keeps the closed-form fast paths reachable for the pieces.
  if (a instanceof Polyline) return intersectComposite(a, b, options, false);
  if (b instanceof Polyline) return intersectComposite(b, a, options, true);

  let raw: Intersection[];
  if (a instanceof LineSegment && b instanceof LineSegment) {
    raw = intersectLineLine(a, b, tol);
  } else if (a instanceof LineSegment && b instanceof Arc) {
    raw = intersectLineArc(a, b, tol);
  } else if (a instanceof Arc && b instanceof LineSegment) {
    raw = swap(intersectLineArc(b, a, tol));
  } else if (a instanceof Arc && b instanceof Arc) {
    raw = intersectArcArc(a, b, tol);
  } else {
    raw = intersectNumeric(a, b, options);
  }

  const filtered = extend ? raw : raw.filter((i) => !i.onExtensionA && !i.onExtensionB);
  return dedupe(filtered, tol);
}

/**
 * Intersections including those on curve extensions — the "apparent
 * intersection" used by TRIM/EXTEND with implied edges.
 */
export function intersectCurvesExtended(
  a: Curve,
  b: Curve,
  options: IntersectOptions = {},
): Intersection[] {
  return intersectCurves(a, b, { ...options, extend: true });
}

/** Split a composite curve, intersect each piece, and remap the parameters. */
function intersectComposite(
  poly: Polyline,
  other: Curve,
  options: IntersectOptions,
  swapped: boolean,
): Intersection[] {
  const results: Intersection[] = [];
  const segs = poly.segments;
  for (let i = 0; i < segs.length; i++) {
    for (const hit of intersectCurves(segs[i]!, other, options)) {
      // Remap the segment-local parameter onto the whole polyline.
      const polyParam = poly.makeParam(i, clamp(hit.paramA, 0, 1));
      results.push(
        swapped
          ? {
              point: hit.point,
              paramA: hit.paramB,
              paramB: polyParam,
              onExtensionA: hit.onExtensionB,
              onExtensionB: hit.onExtensionA,
            }
          : {
              point: hit.point,
              paramA: polyParam,
              paramB: hit.paramB,
              onExtensionA: hit.onExtensionA,
              onExtensionB: hit.onExtensionB,
            },
      );
    }
  }
  return dedupe(results, options.tolerance ?? TOL);
}

/**
 * Line/line intersection in the XY plane.
 *
 * Collinear overlapping segments are reported as no intersection: there is no
 * single crossing point, and every command that consumes this (TRIM, FILLET,
 * the intersection snap) needs a discrete point rather than an interval.
 */
export function intersectLineLine(
  a: LineSegment,
  b: LineSegment,
  tol: Tolerance = TOL,
): Intersection[] {
  const da = a.direction;
  const db = b.direction;
  const denom = da.cross2(db);
  const delta = b.start.sub(a.start);

  if (Math.abs(denom) <= tol.point * Math.max(1, da.length2 * db.length2)) {
    return [];
  }

  const ta = delta.cross2(db) / denom;
  const tb = delta.cross2(da) / denom;
  const point = a.pointAtExtended(ta);
  return [
    {
      point,
      paramA: ta,
      paramB: tb,
      onExtensionA: ta < -tolParam(a, tol) || ta > 1 + tolParam(a, tol),
      onExtensionB: tb < -tolParam(b, tol) || tb > 1 + tolParam(b, tol),
    },
  ];
}

/**
 * Line/circle intersection, restricted to the arc's sweep unless extension is
 * requested by the caller (the caller filters; this reports both flags).
 */
export function intersectLineArc(
  line: LineSegment,
  arc: Arc,
  tol: Tolerance = TOL,
): Intersection[] {
  const d = line.direction;
  const lenSq = d.lengthSq;
  if (lenSq <= tol.point * tol.point) return [];

  const f = line.start.sub(arc.center);
  // Solve |start + t*d - center|^2 = r^2 for t.
  const aCoef = lenSq;
  const bCoef = 2 * f.dot(d);
  const cCoef = f.lengthSq - arc.radius * arc.radius;
  const disc = bCoef * bCoef - 4 * aCoef * cCoef;

  // Scale the tangency threshold by the geometry size so that large-radius
  // near-tangencies are still recognized as single hits.
  const tangentEps = tol.point * Math.max(1, aCoef * arc.radius);
  if (disc < -tangentEps) return [];

  const params: number[] =
    disc <= tangentEps
      ? [-bCoef / (2 * aCoef)]
      : [
          (-bCoef - Math.sqrt(disc)) / (2 * aCoef),
          (-bCoef + Math.sqrt(disc)) / (2 * aCoef),
        ];

  const results: Intersection[] = [];
  for (const t of params) {
    const point = line.pointAtExtended(t);
    const angle = arc.angleAtPoint(point);
    const onArc = arc.containsAngle(angle, tol);
    const lineSlack = tolParam(line, tol);
    results.push({
      point,
      paramA: t,
      paramB: arc.paramAtAngle(angle),
      onExtensionA: t < -lineSlack || t > 1 + lineSlack,
      onExtensionB: !onArc,
    });
  }
  return results;
}

/**
 * Circle/circle intersection.
 *
 * Concentric and non-coplanar cases return no hits — both are genuinely
 * "no discrete intersection" rather than error conditions.
 */
export function intersectArcArc(a: Arc, b: Arc, tol: Tolerance = TOL): Intersection[] {
  // Only coplanar arcs can meet at discrete points in the general case; this
  // kernel's 2D drafting scope means both normals are almost always +Z.
  if (!a.normal.isParallel(b.normal, tol)) return [];

  const delta = b.center.sub(a.center);
  const dist = delta.length;
  const ra = a.radius;
  const rb = b.radius;

  if (dist <= tol.point) return []; // Concentric: coincident or disjoint.
  if (dist > ra + rb + tol.point) return []; // Too far apart.
  if (dist < Math.abs(ra - rb) - tol.point) return []; // One inside the other.

  // Distance from a.center to the radical line, along `delta`.
  const along = (dist * dist + ra * ra - rb * rb) / (2 * dist);
  const heightSq = ra * ra - along * along;
  const height = heightSq <= 0 ? 0 : Math.sqrt(heightSq);

  const unit = delta.scale(1 / dist);
  // Perpendicular within the arcs' shared plane.
  const perp = a.normal.cross(unit).normalizeOrNull() ?? Vec3.UNIT_Y;
  const base = a.center.addScaled(unit, along);

  const points = height <= tol.point
    ? [base]
    : [base.addScaled(perp, height), base.addScaled(perp, -height)];

  const results: Intersection[] = [];
  for (const point of points) {
    const angleA = a.angleAtPoint(point);
    const angleB = b.angleAtPoint(point);
    results.push({
      point,
      paramA: a.paramAtAngle(angleA),
      paramB: b.paramAtAngle(angleB),
      onExtensionA: !a.containsAngle(angleA, tol),
      onExtensionB: !b.containsAngle(angleB, tol),
    });
  }
  return results;
}

/**
 * Generic intersection for curve pairs with no closed form (anything involving
 * an ellipse, and future splines).
 *
 * Two stages: a coarse tessellated sweep finds bracketing segment pairs, then
 * Newton's method on the 2D system `A(u) - B(v) = 0` polishes each bracket to
 * machine precision. Tessellation alone is not accurate enough for CAD; Newton
 * alone has no reliable seed. Together they are both robust and exact.
 */
function intersectNumeric(a: Curve, b: Curve, options: IntersectOptions): Intersection[] {
  const tol = options.tolerance ?? TOL;
  const tess = options.tessellation ?? refineTessellation(a, b, options.tessellation);

  const ptsA = a.tessellate(tess);
  const ptsB = b.tessellate(tess);
  const seeds: Array<{ u: number; v: number }> = [];

  for (let i = 0; i < ptsA.length - 1; i++) {
    const segA = new LineSegment(ptsA[i]!, ptsA[i + 1]!);
    const boundsA = segA.bounds;
    for (let j = 0; j < ptsB.length - 1; j++) {
      const segB = new LineSegment(ptsB[j]!, ptsB[j + 1]!);
      if (!boundsA.intersects2(segB.bounds, tol)) continue;
      const hits = intersectLineLine(segA, segB, tol);
      for (const hit of hits) {
        if (hit.onExtensionA || hit.onExtensionB) continue;
        seeds.push({
          u: (i + clamp(hit.paramA, 0, 1)) / (ptsA.length - 1),
          v: (j + clamp(hit.paramB, 0, 1)) / (ptsB.length - 1),
        });
      }
    }
  }

  const results: Intersection[] = [];
  for (const seed of seeds) {
    const refined = newtonRefine(a, b, seed.u, seed.v);
    if (refined === null) continue;
    const point = a.pointAt(refined.u);
    if (point.distanceTo(b.pointAt(refined.v)) > tol.point * 1e4) continue;
    results.push({
      point,
      paramA: refined.u,
      paramB: refined.v,
      onExtensionA: false,
      onExtensionB: false,
    });
  }
  return results;
}

/**
 * Newton iteration on the vector equation `A(u) - B(v) = 0`, projected onto
 * the two tangent directions to give a well-conditioned 2x2 system.
 */
function newtonRefine(
  a: Curve,
  b: Curve,
  u0: number,
  v0: number,
): { u: number; v: number } | null {
  let u = u0;
  let v = v0;
  for (let i = 0; i < 32; i++) {
    const pa = a.pointAt(u);
    const pb = b.pointAt(v);
    const diff = pa.sub(pb);
    if (diff.lengthSq < 1e-28) return { u, v };

    const ta = a.tangentAt(u);
    const tb = b.tangentAt(v);
    // Solve [ta, -tb] * [du, dv]^T = -diff in the XY plane.
    const det = ta.cross2(tb.negate());
    if (Math.abs(det) < 1e-18) return null;
    const du = diff.negate().cross2(tb.negate()) / det;
    const dv = ta.cross2(diff.negate()) / det;

    u = clamp(u + du, 0, 1);
    v = clamp(v + dv, 0, 1);
    if (Math.abs(du) < 1e-15 && Math.abs(dv) < 1e-15) break;
  }
  return { u, v };
}

/**
 * Pick a tessellation fine enough that the coarse sweep cannot miss a
 * crossing: the chord tolerance is scaled to the smaller of the two curves'
 * extents rather than left at a fixed world-unit default, which would be
 * far too coarse for small geometry and far too fine for large.
 */
function refineTessellation(
  a: Curve,
  b: Curve,
  provided: TessellationOptions | undefined,
): TessellationOptions {
  if (provided !== undefined) return provided;
  const scale = Math.min(a.bounds.diagonal, b.bounds.diagonal);
  if (!Number.isFinite(scale) || scale <= 0) return DEFAULT_TESSELLATION;
  return {
    chordTolerance: scale * 1e-4,
    minSegments: 16,
    maxSegments: 2048,
  };
}

/** Reverse the roles of the two curves in a result set. */
function swap(hits: Intersection[]): Intersection[] {
  return hits.map((h) => ({
    point: h.point,
    paramA: h.paramB,
    paramB: h.paramA,
    onExtensionA: h.onExtensionB,
    onExtensionB: h.onExtensionA,
  }));
}

/**
 * Drop duplicate hits. Tangency and shared endpoints routinely produce the
 * same point twice, and downstream commands must not see it as two crossings.
 */
function dedupe(hits: Intersection[], tol: Tolerance): Intersection[] {
  const out: Intersection[] = [];
  const threshold = Math.max(tol.point, 1e-9);
  for (const hit of hits) {
    if (out.some((existing) => existing.point.distanceTo(hit.point) <= threshold)) continue;
    out.push(hit);
  }
  return out;
}

/**
 * Parametric slack equivalent to the linear tolerance on a given curve.
 * Without this, a hit exactly at an endpoint would be classified as being on
 * the extension purely through floating-point noise.
 */
function tolParam(curve: Curve, tol: Tolerance): number {
  const len = curve.length;
  return len > 0 ? tol.point / len : 0;
}

/**
 * Self-intersections of a polyline — needed to validate hatch boundaries and
 * to warn on malformed imported geometry.
 */
export function selfIntersections(
  poly: Polyline,
  options: IntersectOptions = {},
): Intersection[] {
  const tol = options.tolerance ?? TOL;
  const segs = poly.segments;
  const results: Intersection[] = [];
  for (let i = 0; i < segs.length; i++) {
    // Start at i + 2: adjacent segments always meet at their shared vertex,
    // which is not a self-intersection.
    for (let j = i + 2; j < segs.length; j++) {
      if (poly.isClosed && i === 0 && j === segs.length - 1) continue;
      for (const hit of intersectCurves(segs[i]!, segs[j]!, options)) {
        results.push({
          point: hit.point,
          paramA: poly.makeParam(i, hit.paramA),
          paramB: poly.makeParam(j, hit.paramB),
          onExtensionA: false,
          onExtensionB: false,
        });
      }
    }
  }
  return dedupe(results, tol);
}

/**
 * Every intersection between one curve and a set of others, sorted along the
 * first curve. This is the exact query TRIM performs against its cutting
 * edges, so it lives here rather than being re-derived in the command.
 */
export function intersectAgainstAll(
  curve: Curve,
  others: Iterable<Curve>,
  options: IntersectOptions = {},
): Intersection[] {
  const all: Intersection[] = [];
  for (const other of others) {
    if (other === curve) continue;
    all.push(...intersectCurves(curve, other, options));
  }
  all.sort((x, y) => x.paramA - y.paramA);
  return dedupe(all, options.tolerance ?? TOL);
}

// Referenced so that the ellipse module is loaded whenever intersection is
// used; `Arc.transform` needs its factory registered before any non-conformal
// transform occurs.
void EllipseArc;
