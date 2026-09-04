import { Vec3 } from '../math/vec3.js';
import { Mat4 } from '../math/mat4.js';
import { Box3 } from '../math/box3.js';
import { TOL, type Tolerance, clamp } from '../math/tolerance.js';
import { TAU, normalizeAngle } from '../math/angle.js';
import {
  Curve,
  DEFAULT_TESSELLATION,
  type AreaCurve,
  type ClosestPointResult,
  type TessellationOptions,
} from './curve.js';
import { arbitraryAxis, isWorldNormal, type CoordinateFrame } from './ocs.js';
import { sanitizeSplitParams } from './line-segment.js';

const WORLD_FRAME: CoordinateFrame = Object.freeze({
  xAxis: Vec3.UNIT_X,
  yAxis: Vec3.UNIT_Y,
  zAxis: Vec3.UNIT_Z,
});

/**
 * A circular arc, always swept counter-clockwise from `startAngle` to
 * `endAngle` about `normal` — the same convention DWG uses, which avoids a
 * class of sign errors at the import boundary.
 *
 * A full circle is represented by `startAngle === endAngle` together with
 * {@link isClosed} being true; {@link Arc.circle} builds one.
 *
 * The arc is planar but not restricted to the world XY plane: `normal` gives
 * the plane's orientation and the in-plane axes come from the arbitrary axis
 * algorithm, matching OCS semantics exactly.
 */
export class Arc extends Curve implements AreaCurve {
  readonly center: Vec3;
  readonly radius: number;
  /** Start angle in radians, measured in the arc's own plane. */
  readonly startAngle: number;
  /** End angle in radians. Equal to `startAngle` for a full circle. */
  readonly endAngle: number;
  readonly normal: Vec3;
  /** Total counter-clockwise sweep in radians, in `(0, 2PI]`. */
  readonly sweep: number;

  private readonly frame: CoordinateFrame;
  private readonly full: boolean;

  constructor(
    center: Vec3,
    radius: number,
    startAngle: number,
    endAngle: number,
    normal: Vec3 = Vec3.UNIT_Z,
    /** Force a full circle even though the angles coincide. */
    isFullCircle = false,
  ) {
    super();
    if (!(radius >= 0) || !Number.isFinite(radius)) {
      throw new RangeError(`Arc radius must be finite and non-negative, received ${radius}`);
    }
    this.center = center;
    this.radius = radius;
    this.startAngle = normalizeAngle(startAngle);
    this.endAngle = normalizeAngle(endAngle);
    this.normal = normal;
    this.frame = isWorldNormal(normal) ? WORLD_FRAME : arbitraryAxis(normal);

    const rawSweep = normalizeAngle(this.endAngle - this.startAngle);
    // A zero sweep is ambiguous: it means either a degenerate arc or a full
    // circle. Callers disambiguate with `isFullCircle`; DXF disambiguates by
    // entity type (CIRCLE vs ARC), which the importer maps onto this flag.
    this.full = isFullCircle || rawSweep === 0;
    this.sweep = this.full ? TAU : rawSweep;
    Object.freeze(this);
  }

  /** A complete circle. */
  static circle(center: Vec3, radius: number, normal: Vec3 = Vec3.UNIT_Z): Arc {
    return new Arc(center, radius, 0, 0, normal, true);
  }

  /**
   * Arc through three points, in the XY plane.
   * @returns `null` when the points are collinear or coincident.
   */
  static fromThreePoints(a: Vec3, b: Vec3, c: Vec3, tol: Tolerance = TOL): Arc | null {
    const ax = a.x, ay = a.y;
    const bx = b.x, by = b.y;
    const cx = c.x, cy = c.y;
    const d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (Math.abs(d) <= tol.point) return null;

    const aSq = ax * ax + ay * ay;
    const bSq = bx * bx + by * by;
    const cSq = cx * cx + cy * cy;
    const ux = (aSq * (by - cy) + bSq * (cy - ay) + cSq * (ay - by)) / d;
    const uy = (aSq * (cx - bx) + bSq * (ax - cx) + cSq * (bx - ax)) / d;
    const center = new Vec3(ux, uy, a.z);
    const radius = center.distance2To(a);
    if (radius <= tol.point) return null;

    const angA = Math.atan2(ay - uy, ax - ux);
    const angB = Math.atan2(by - uy, bx - ux);
    const angC = Math.atan2(cy - uy, cx - ux);

    // Choose the sweep direction that actually passes through the middle point.
    const ccwHasB = normalizeAngle(angB - angA) <= normalizeAngle(angC - angA);
    return ccwHasB
      ? new Arc(center, radius, angA, angC)
      : new Arc(center, radius, angC, angA);
  }

  /**
   * Arc from two endpoints and a bulge factor, the representation used by
   * DWG/DXF lightweight polylines.
   *
   * Bulge is `tan(includedAngle / 4)`; positive is counter-clockwise, and 0
   * means a straight segment (for which this returns `null`).
   */
  static fromBulge(start: Vec3, end: Vec3, bulge: number, tol: Tolerance = TOL): Arc | null {
    if (Math.abs(bulge) <= 1e-12) return null;
    const chord = end.sub(start).flatten();
    const chordLen = chord.length2;
    if (chordLen <= tol.point) return null;

    const included = 4 * Math.atan(bulge);
    const radius = Math.abs(chordLen / (2 * Math.sin(included / 2)));
    // Distance from the chord midpoint to the centre, signed by direction.
    const apothem = (chordLen / 2) / Math.tan(included / 2);
    const mid = start.midpoint(end);
    const perp = new Vec3(-chord.y, chord.x, 0).scale(1 / chordLen);
    const center = mid.addScaled(perp, apothem);

    const startAngle = Math.atan2(start.y - center.y, start.x - center.x);
    const endAngle = Math.atan2(end.y - center.y, end.x - center.x);
    // A negative bulge sweeps clockwise, which this class represents by
    // swapping the endpoints and keeping the counter-clockwise convention.
    return bulge > 0
      ? new Arc(center, radius, startAngle, endAngle)
      : new Arc(center, radius, endAngle, startAngle);
  }

  /**
   * The bulge factor for this arc traversed from start to end, for writing
   * back out to a polyline.
   */
  get bulge(): number {
    return Math.tan(this.sweep / 4);
  }

  override get isClosed(): boolean {
    return this.full;
  }

  get isFullCircle(): boolean {
    return this.full;
  }

  /** Point on the arc's plane at the given absolute angle. */
  pointAtAngle(angle: number): Vec3 {
    const { xAxis, yAxis } = this.frame;
    const c = Math.cos(angle) * this.radius;
    const s = Math.sin(angle) * this.radius;
    return this.center.addScaled(xAxis, c).addScaled(yAxis, s);
  }

  /** Angle in the arc's plane of the direction from the centre to `p`. */
  angleAtPoint(p: Vec3): number {
    const d = p.sub(this.center);
    return normalizeAngle(Math.atan2(d.dot(this.frame.yAxis), d.dot(this.frame.xAxis)));
  }

  /** Absolute angle at normalized parameter `t`. */
  angleAt(t: number): number {
    return this.startAngle + this.sweep * t;
  }

  /**
   * Normalized parameter for an absolute angle. Angles outside the sweep are
   * reported as the nearest of 0 or 1 by proximity, mirroring how other CAD software
   * resolves picks slightly off the end of an arc.
   */
  paramAtAngle(angle: number): number {
    const offset = normalizeAngle(angle - this.startAngle);
    if (offset <= this.sweep) return offset / this.sweep;
    // Outside the sweep — decide which end of the gap it is closer to.
    const gapFromEnd = offset - this.sweep;
    const gapToStart = TAU - offset;
    return gapFromEnd <= gapToStart ? 1 : 0;
  }

  /** `true` when the absolute angle lies within the swept region. */
  containsAngle(angle: number, tol: Tolerance = TOL): boolean {
    if (this.full) return true;
    const offset = normalizeAngle(angle - this.startAngle);
    return offset <= this.sweep + tol.angle || offset >= TAU - tol.angle;
  }

  override get startPoint(): Vec3 {
    return this.pointAtAngle(this.startAngle);
  }

  override get endPoint(): Vec3 {
    return this.full ? this.startPoint : this.pointAtAngle(this.endAngle);
  }

  override pointAt(t: number): Vec3 {
    return this.pointAtAngle(this.angleAt(this.clampParam(t)));
  }

  override tangentAt(t: number): Vec3 {
    const angle = this.angleAt(this.clampParam(t));
    const { xAxis, yAxis } = this.frame;
    // d/dt of the position, i.e. the CCW tangent scaled by radius * sweep.
    return xAxis
      .scale(-Math.sin(angle))
      .add(yAxis.scale(Math.cos(angle)))
      .scale(this.radius * this.sweep);
  }

  override get length(): number {
    return this.radius * this.sweep;
  }

  get signedArea(): number {
    // Circular segment area for an open arc, full disc area when closed.
    if (this.full) return Math.PI * this.radius * this.radius;
    return 0.5 * this.radius * this.radius * (this.sweep - Math.sin(this.sweep));
  }

  override get bounds(): Box3 {
    if (this.radius === 0) return Box3.fromPoint(this.center);
    if (this.full) {
      // Exact bounds of a circle in an arbitrary plane: the extent along each
      // world axis is r * |projection of that axis onto the circle's plane|.
      const { xAxis, yAxis } = this.frame;
      const ex = Math.hypot(xAxis.x, yAxis.x) * this.radius;
      const ey = Math.hypot(xAxis.y, yAxis.y) * this.radius;
      const ez = Math.hypot(xAxis.z, yAxis.z) * this.radius;
      return new Box3(
        new Vec3(this.center.x - ex, this.center.y - ey, this.center.z - ez),
        new Vec3(this.center.x + ex, this.center.y + ey, this.center.z + ez),
      );
    }

    // Open arc: the endpoints plus any axis-extreme angle inside the sweep.
    // For a plane-aligned arc these are the quadrant points; for a tilted
    // plane the extremes along each world axis occur at atan2 of the basis
    // components, which is the general form used here.
    let box = Box3.fromCorners(this.startPoint, this.endPoint);
    const { xAxis, yAxis } = this.frame;
    const axes: Array<[number, number]> = [
      [xAxis.x, yAxis.x],
      [xAxis.y, yAxis.y],
      [xAxis.z, yAxis.z],
    ];
    for (const [cx, cy] of axes) {
      if (cx === 0 && cy === 0) continue;
      const extreme = Math.atan2(cy, cx);
      for (const angle of [extreme, extreme + Math.PI]) {
        if (this.containsAngle(normalizeAngle(angle))) {
          box = box.expandToPoint(this.pointAtAngle(angle));
        }
      }
    }
    return box;
  }

  override closestPoint(p: Vec3): ClosestPointResult {
    const projected = this.projectToPlane(p);
    if (projected === null) {
      // Query point is on the axis through the centre: every point on the arc
      // is equidistant, so the start point is as good an answer as any.
      const point = this.startPoint;
      return { point, param: 0, distance: p.distanceTo(point) };
    }
    const angle = projected.angle;
    if (this.containsAngle(angle)) {
      const point = this.pointAtAngle(angle);
      return { point, param: this.paramAtAngle(angle), distance: p.distanceTo(point) };
    }
    // Outside the sweep: the nearest point is whichever endpoint is closer.
    const s = this.startPoint;
    const e = this.endPoint;
    const ds = p.distanceTo(s);
    const de = p.distanceTo(e);
    return ds <= de
      ? { point: s, param: 0, distance: ds }
      : { point: e, param: 1, distance: de };
  }

  override closestPointExtended(p: Vec3): ClosestPointResult | null {
    const projected = this.projectToPlane(p);
    if (projected === null) return null;
    const point = this.pointAtAngle(projected.angle);
    return {
      point,
      param: this.paramAtAngle(projected.angle),
      distance: p.distanceTo(point),
    };
  }

  /** Angle of `p` projected onto the arc's plane, or `null` when on the axis. */
  private projectToPlane(p: Vec3): { angle: number } | null {
    const d = p.sub(this.center);
    const u = d.dot(this.frame.xAxis);
    const w = d.dot(this.frame.yAxis);
    if (u === 0 && w === 0) return null;
    return { angle: normalizeAngle(Math.atan2(w, u)) };
  }

  /** The four quadrant points of the containing circle — the QUADRANT snap. */
  get quadrantPoints(): Vec3[] {
    const result: Vec3[] = [];
    for (let i = 0; i < 4; i++) {
      const angle = (i * Math.PI) / 2;
      if (this.containsAngle(angle)) result.push(this.pointAtAngle(angle));
    }
    return result;
  }

  override transform(m: Mat4): Curve {
    // A non-conformal transform turns a circle into an ellipse. Rather than
    // silently distorting the radius, hand off to the ellipse type.
    if (!m.isConformal()) {
      return this.toEllipseArc().transform(m);
    }
    const center = m.transformPoint(this.center);
    const radius = this.radius * m.scaleFactor;
    const normal = m.transformVector(this.normal).normalizeOrNull() ?? this.normal;

    // Re-derive the angles from transformed endpoints so that rotation and
    // mirroring are both handled without special-casing.
    const startWorld = m.transformPoint(this.startPoint);
    const endWorld = m.transformPoint(this.endPoint);
    const result = new Arc(center, radius, 0, 0, normal, true);
    if (this.full) return result;

    const s = result.angleAtPoint(startWorld);
    const e = result.angleAtPoint(endWorld);
    // Mirroring reverses the sweep direction; swapping the angles restores the
    // counter-clockwise invariant while preserving the visible geometry.
    return m.isMirrored
      ? new Arc(center, radius, e, s, normal)
      : new Arc(center, radius, s, e, normal);
  }

  override reverse(): Arc {
    if (this.full) {
      return new Arc(this.center, this.radius, this.startAngle, this.endAngle, this.normal.negate(), true);
    }
    return new Arc(this.center, this.radius, this.endAngle, this.startAngle, this.normal);
  }

  override tessellate(options: TessellationOptions = DEFAULT_TESSELLATION): Vec3[] {
    const count = this.segmentCount(options);
    const points: Vec3[] = new Array(count + 1);
    for (let i = 0; i <= count; i++) {
      points[i] = this.pointAtAngle(this.startAngle + (this.sweep * i) / count);
    }
    // Snap the ends to the exact endpoints so that adjacent tessellated curves
    // meet without visible gaps.
    points[0] = this.startPoint;
    points[count] = this.endPoint;
    return points;
  }

  /**
   * Number of chords needed to stay within the chord tolerance.
   *
   * From the sagitta relation `s = r * (1 - cos(theta / 2))`, the maximum
   * angular step for a given deviation is `2 * acos(1 - tol / r)`.
   */
  segmentCount(options: TessellationOptions = DEFAULT_TESSELLATION): number {
    const min = options.minSegments ?? 4;
    const max = options.maxSegments ?? 4096;
    if (this.radius <= 0) return min;
    const ratio = options.chordTolerance / this.radius;
    if (ratio >= 1) return min;
    const maxStep = 2 * Math.acos(clamp(1 - ratio, -1, 1));
    if (!(maxStep > 0)) return max;
    return clamp(Math.ceil(this.sweep / maxStep), min, max);
  }

  override trim(t0: number, t1: number): Arc {
    const a0 = this.angleAt(this.clampParam(t0));
    const a1 = this.angleAt(this.clampParam(t1));
    return new Arc(this.center, this.radius, a0, a1, this.normal);
  }

  override split(params: readonly number[]): Arc[] {
    const cuts = sanitizeSplitParams(params);
    if (cuts.length === 0) return [this];
    const result: Arc[] = [];
    let prev = 0;
    for (const t of cuts) {
      result.push(this.trim(prev, t));
      prev = t;
    }
    result.push(this.trim(prev, 1));
    return result;
  }

  /** Widen to the general conic representation. */
  toEllipseArc(): EllipseArcLike {
    // Imported lazily through an interface to keep the module graph acyclic;
    // `EllipseArc` constructs itself from these parameters.
    return ellipseFactory(this);
  }

  equals(other: Arc, tol: Tolerance = TOL): boolean {
    return (
      this.center.equals(other.center, tol) &&
      Math.abs(this.radius - other.radius) <= tol.point &&
      this.full === other.full &&
      (this.full ||
        (Math.abs(this.startAngle - other.startAngle) <= tol.angle &&
          Math.abs(this.endAngle - other.endAngle) <= tol.angle))
    );
  }

  override toString(): string {
    return this.full
      ? `Circle(c=${this.center}, r=${this.radius})`
      : `Arc(c=${this.center}, r=${this.radius}, ${this.startAngle}..${this.endAngle})`;
  }
}

/**
 * Indirection that lets {@link Arc.toEllipseArc} widen an arc without this
 * module importing the ellipse module, which imports this one for its own
 * degenerate cases. The ellipse module installs the factory at load time.
 */
export type EllipseArcLike = Curve;
type EllipseFactory = (arc: Arc) => EllipseArcLike;

let ellipseFactoryImpl: EllipseFactory | null = null;

export function registerEllipseFactory(factory: EllipseFactory): void {
  ellipseFactoryImpl = factory;
}

function ellipseFactory(arc: Arc): EllipseArcLike {
  if (ellipseFactoryImpl === null) {
    throw new Error(
      'Ellipse support has not been loaded; import the ellipse module before ' +
        'applying a non-conformal transform to an arc.',
    );
  }
  return ellipseFactoryImpl(arc);
}
