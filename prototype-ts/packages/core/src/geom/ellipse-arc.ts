import { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
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
import { Arc, registerEllipseFactory } from './arc.js';
import { sanitizeSplitParams } from './line-segment.js';

/**
 * An elliptical arc, parameterized by eccentric anomaly exactly as DXF
 * stores it (group codes 41/42), so import and export are lossless.
 *
 * ```
 *   P(u) = center + majorAxis * cos(u) + minorAxis * sin(u)
 * ```
 *
 * `majorAxis` is a full vector, carrying both the direction and the semi-major
 * length; `ratio` is the semi-minor / semi-major ratio in `(0, 1]`. The minor
 * axis direction is derived as `normal x majorAxis`, which keeps a consistent
 * counter-clockwise sweep about the normal.
 *
 * Ellipses also serve as the general home for circles under non-conformal
 * transforms — scaling a circle by different X and Y factors produces one of
 * these rather than a silently wrong circle.
 */
export class EllipseArc extends Curve implements AreaCurve {
  readonly center: Vec3;
  /** Vector from centre to the end of the semi-major axis. */
  readonly majorAxis: Vec3;
  /** Semi-minor / semi-major, in `(0, 1]`. */
  readonly ratio: number;
  /** Start eccentric anomaly, radians. */
  readonly startParam: number;
  /** End eccentric anomaly, radians. */
  readonly endParam: number;
  readonly normal: Vec3;

  /** Vector from centre to the end of the semi-minor axis. */
  readonly minorAxis: Vec3;
  /** Total sweep in parameter space, in `(0, 2PI]`. */
  readonly sweep: number;

  private readonly full: boolean;
  /**
   * Mutable memo holder. The instance itself is frozen, so lazily computed
   * values live in this nested object: the reference is immutable, only the
   * cached contents change, and no observable state is affected.
   */
  private readonly memo: { length: number | null } = { length: null };

  constructor(
    center: Vec3,
    majorAxis: Vec3,
    ratio: number,
    startParam = 0,
    endParam = TAU,
    normal: Vec3 = Vec3.UNIT_Z,
  ) {
    super();
    if (!(ratio > 0) || ratio > 1 + 1e-12) {
      throw new RangeError(`Ellipse ratio must lie in (0, 1], received ${ratio}`);
    }
    if (majorAxis.isZero()) {
      throw new RangeError('Ellipse major axis must be non-zero');
    }
    this.center = center;
    this.majorAxis = majorAxis;
    this.ratio = Math.min(ratio, 1);
    this.normal = normal;

    const sp = normalizeAngle(startParam);
    const rawSweep = normalizeAngle(endParam - startParam);
    // DXF writes a full ellipse as 0 -> 2*PI, which normalizes to a zero sweep.
    this.full = rawSweep === 0;
    this.startParam = sp;
    this.sweep = this.full ? TAU : rawSweep;
    this.endParam = normalizeAngle(sp + this.sweep);

    const minorDir = normal.cross(majorAxis).normalizeOrNull() ?? Vec3.UNIT_Y;
    this.minorAxis = minorDir.scale(majorAxis.length * this.ratio);
    Object.freeze(this);
  }

  /** Widen a circular arc into the general conic form. */
  static fromArc(arc: Arc): EllipseArc {
    const frame = arc.pointAtAngle(0).sub(arc.center);
    const majorAxis = frame.isZero() ? Vec3.UNIT_X.scale(arc.radius) : frame;
    return new EllipseArc(
      arc.center,
      majorAxis,
      1,
      arc.startAngle,
      arc.isFullCircle ? arc.startAngle : arc.endAngle,
      arc.normal,
    );
  }

  get semiMajor(): number {
    return this.majorAxis.length;
  }

  get semiMinor(): number {
    return this.semiMajor * this.ratio;
  }

  /** Rotation of the major axis within the XY plane. */
  get rotation(): number {
    return this.majorAxis.angle2;
  }

  override get isClosed(): boolean {
    return this.full;
  }

  /** Position at an absolute eccentric anomaly. */
  pointAtParam(u: number): Vec3 {
    return this.center.addScaled(this.majorAxis, Math.cos(u)).addScaled(this.minorAxis, Math.sin(u));
  }

  /** Derivative with respect to eccentric anomaly. */
  derivativeAtParam(u: number): Vec3 {
    return this.majorAxis.scale(-Math.sin(u)).addScaled(this.minorAxis, Math.cos(u));
  }

  /** Absolute eccentric anomaly at normalized parameter `t`. */
  paramAt(t: number): number {
    return this.startParam + this.sweep * t;
  }

  /** Normalized parameter for an absolute eccentric anomaly. */
  normalizedAt(u: number): number {
    const offset = normalizeAngle(u - this.startParam);
    if (offset <= this.sweep) return offset / this.sweep;
    return offset - this.sweep <= TAU - offset ? 1 : 0;
  }

  containsParam(u: number, tol: Tolerance = TOL): boolean {
    if (this.full) return true;
    const offset = normalizeAngle(u - this.startParam);
    return offset <= this.sweep + tol.angle || offset >= TAU - tol.angle;
  }

  override get startPoint(): Vec3 {
    return this.pointAtParam(this.startParam);
  }

  override get endPoint(): Vec3 {
    return this.full ? this.startPoint : this.pointAtParam(this.endParam);
  }

  override pointAt(t: number): Vec3 {
    return this.pointAtParam(this.paramAt(this.clampParam(t)));
  }

  override tangentAt(t: number): Vec3 {
    return this.derivativeAtParam(this.paramAt(this.clampParam(t))).scale(this.sweep);
  }

  /**
   * Arc length by adaptive Gauss-Legendre quadrature.
   *
   * Elliptic arc length has no closed form, and Ramanujan-style approximations
   * only cover the full perimeter. Quadrature over the actual sweep is both
   * general and accurate to well below drafting tolerance.
   */
  override get length(): number {
    if (this.memo.length !== null) return this.memo.length;
    const panels = 16;
    const step = this.sweep / panels;
    let total = 0;
    for (let i = 0; i < panels; i++) {
      total += gaussLegendre5(
        (u) => this.derivativeAtParam(u).length,
        this.startParam + i * step,
        this.startParam + (i + 1) * step,
      );
    }
    this.memo.length = total;
    return total;
  }

  get signedArea(): number {
    if (this.full) return Math.PI * this.semiMajor * this.semiMinor;
    // Area of the elliptical sector swept from start to end.
    return 0.5 * this.semiMajor * this.semiMinor * (this.sweep - Math.sin(this.sweep));
  }

  override get bounds(): Box3 {
    let box = Box3.fromCorners(this.startPoint, this.endPoint);
    // Extremes along each world axis occur where the derivative's component
    // vanishes: -Mx*sin(u) + mx*cos(u) = 0, i.e. u = atan2(mx, Mx).
    const axes: Array<[number, number]> = [
      [this.majorAxis.x, this.minorAxis.x],
      [this.majorAxis.y, this.minorAxis.y],
      [this.majorAxis.z, this.minorAxis.z],
    ];
    for (const [maj, min] of axes) {
      if (maj === 0 && min === 0) continue;
      const u0 = Math.atan2(min, maj);
      for (const u of [u0, u0 + Math.PI]) {
        const nu = normalizeAngle(u);
        if (this.containsParam(nu)) box = box.expandToPoint(this.pointAtParam(nu));
      }
    }
    return box;
  }

  override closestPoint(p: Vec3): ClosestPointResult {
    const u = this.solveClosestParam(p);
    if (this.containsParam(u)) {
      const point = this.pointAtParam(u);
      return { point, param: this.normalizedAt(u), distance: p.distanceTo(point) };
    }
    const s = this.startPoint;
    const e = this.endPoint;
    const ds = p.distanceTo(s);
    const de = p.distanceTo(e);
    return ds <= de
      ? { point: s, param: 0, distance: ds }
      : { point: e, param: 1, distance: de };
  }

  override closestPointExtended(p: Vec3): ClosestPointResult | null {
    const u = this.solveClosestParam(p);
    const point = this.pointAtParam(u);
    return { point, param: this.normalizedAt(u), distance: p.distanceTo(point) };
  }

  /**
   * Eccentric anomaly of the closest point on the *full* ellipse.
   *
   * Newton's method on `f(u) = (P(u) - p) . P'(u)`, seeded from the angle in
   * the ellipse's own frame. Newton alone can converge to a local maximum on
   * eccentric ellipses, so the search is seeded from four candidates spread
   * around the ellipse and the best converged root wins.
   */
  private solveClosestParam(p: Vec3): number {
    const d = p.sub(this.center);
    const majLen = this.semiMajor;
    const minLen = this.semiMinor;
    const x = d.dot(this.majorAxis) / (majLen * majLen);
    const y = d.dot(this.minorAxis) / (minLen * minLen);
    const seed = Math.atan2(y * minLen, x * majLen);

    let best = seed;
    let bestDist = Infinity;
    for (let k = 0; k < 4; k++) {
      const u = this.newtonClosest(p, seed + (k * Math.PI) / 2);
      const dist = p.distanceSqTo(this.pointAtParam(u));
      if (dist < bestDist) {
        bestDist = dist;
        best = u;
      }
    }
    return normalizeAngle(best);
  }

  private newtonClosest(p: Vec3, seed: number): number {
    let u = seed;
    for (let i = 0; i < 24; i++) {
      const pt = this.pointAtParam(u);
      const d1 = this.derivativeAtParam(u);
      // Second derivative of the parameterization.
      const d2 = this.majorAxis.scale(-Math.cos(u)).addScaled(this.minorAxis, -Math.sin(u));
      const diff = pt.sub(p);
      const f = diff.dot(d1);
      const df = d1.lengthSq + diff.dot(d2);
      if (Math.abs(df) < 1e-18) break;
      const step = f / df;
      u -= step;
      if (Math.abs(step) < 1e-14) break;
    }
    return u;
  }

  override transform(m: Mat4): EllipseArc {
    const center = m.transformPoint(this.center);
    const major = m.transformVector(this.majorAxis);
    const minor = m.transformVector(this.minorAxis);
    const normal = m.transformVector(this.normal).normalizeOrNull() ?? this.normal;

    // A general affine map takes conjugate diameters to conjugate diameters,
    // not axes to axes. Rytz's construction recovers the true axes from the
    // transformed conjugate pair.
    const { axisA, axisB } = conjugateToAxes(major, minor);
    const aLen = axisA.length;
    const bLen = axisB.length;
    const [newMajor, newMinor] = aLen >= bLen ? [axisA, axisB] : [axisB, axisA];
    const ratio = newMajor.length === 0 ? 1 : clamp(newMinor.length / newMajor.length, 1e-12, 1);

    const result = new EllipseArc(center, newMajor, ratio, 0, TAU, normal);
    if (this.full) return result;

    const s = result.paramAtPoint(m.transformPoint(this.startPoint));
    const e = result.paramAtPoint(m.transformPoint(this.endPoint));
    return m.isMirrored
      ? new EllipseArc(center, newMajor, ratio, e, s, normal)
      : new EllipseArc(center, newMajor, ratio, s, e, normal);
  }

  /** Eccentric anomaly of a point assumed to lie on the ellipse. */
  paramAtPoint(p: Vec3): number {
    const d = p.sub(this.center);
    const cos = d.dot(this.majorAxis) / this.majorAxis.lengthSq;
    const sin = d.dot(this.minorAxis) / this.minorAxis.lengthSq;
    return normalizeAngle(Math.atan2(sin, cos));
  }

  override reverse(): EllipseArc {
    if (this.full) {
      return new EllipseArc(this.center, this.majorAxis, this.ratio, 0, TAU, this.normal.negate());
    }
    return new EllipseArc(
      this.center,
      this.majorAxis,
      this.ratio,
      this.endParam,
      this.startParam,
      this.normal,
    );
  }

  override tessellate(options: TessellationOptions = DEFAULT_TESSELLATION): Vec3[] {
    const count = this.segmentCount(options);
    const points: Vec3[] = new Array(count + 1);
    for (let i = 0; i <= count; i++) {
      points[i] = this.pointAtParam(this.startParam + (this.sweep * i) / count);
    }
    points[0] = this.startPoint;
    points[count] = this.endPoint;
    return points;
  }

  /**
   * Segment count sized by the *maximum* curvature (at the end of the major
   * axis, where the radius of curvature is `b^2 / a`). Using the tightest
   * curvature keeps the chord error bounded everywhere on the ellipse.
   */
  segmentCount(options: TessellationOptions = DEFAULT_TESSELLATION): number {
    const min = options.minSegments ?? 4;
    const max = options.maxSegments ?? 4096;
    const a = this.semiMajor;
    const b = this.semiMinor;
    if (a <= 0 || b <= 0) return min;
    const minRadius = (b * b) / a;
    const ratio = options.chordTolerance / minRadius;
    if (ratio >= 1) return min;
    const maxStep = 2 * Math.acos(clamp(1 - ratio, -1, 1));
    if (!(maxStep > 0)) return max;
    return clamp(Math.ceil(this.sweep / maxStep), min, max);
  }

  override trim(t0: number, t1: number): EllipseArc {
    return new EllipseArc(
      this.center,
      this.majorAxis,
      this.ratio,
      this.paramAt(this.clampParam(t0)),
      this.paramAt(this.clampParam(t1)),
      this.normal,
    );
  }

  override split(params: readonly number[]): EllipseArc[] {
    const cuts = sanitizeSplitParams(params);
    if (cuts.length === 0) return [this];
    const result: EllipseArc[] = [];
    let prev = 0;
    for (const t of cuts) {
      result.push(this.trim(prev, t));
      prev = t;
    }
    result.push(this.trim(prev, 1));
    return result;
  }

  /** `true` when the ellipse is circular within tolerance. */
  isCircular(tol: Tolerance = TOL): boolean {
    return Math.abs(1 - this.ratio) <= tol.point;
  }

  /** Narrow to a circular arc when possible, for cleaner downstream handling. */
  toArcIfCircular(tol: Tolerance = TOL): Arc | null {
    if (!this.isCircular(tol)) return null;
    const radius = this.semiMajor;
    const base = this.majorAxis.angle2;
    return new Arc(
      this.center,
      radius,
      base + this.startParam,
      base + this.endParam,
      this.normal,
      this.full,
    );
  }

  override toString(): string {
    return `EllipseArc(c=${this.center}, a=${this.semiMajor}, b=${this.semiMinor})`;
  }
}

/**
 * Rytz's construction: recover the principal axes of an ellipse from a pair of
 * conjugate semi-diameters.
 *
 * Needed because an affine transform of an ellipse preserves conjugacy but not
 * perpendicularity — after a shear or non-uniform scale, the transformed
 * "axes" are no longer axes.
 */
function conjugateToAxes(u: Vec3, w: Vec3): { axisA: Vec3; axisB: Vec3 } {
  // The extreme radii occur at the parameter that extremizes |u cos t + w sin t|.
  // Differentiating gives tan(2t) = 2(u.w) / (|u|^2 - |w|^2).
  const uu = u.lengthSq;
  const ww = w.lengthSq;
  const uw = u.dot(w);
  if (Math.abs(uw) < 1e-15 && Math.abs(uu - ww) < 1e-15) {
    // Already a circle: any orthogonal pair works.
    return { axisA: u, axisB: w };
  }
  const t = 0.5 * Math.atan2(2 * uw, uu - ww);
  const axisA = u.scale(Math.cos(t)).addScaled(w, Math.sin(t));
  const axisB = u.scale(-Math.sin(t)).addScaled(w, Math.cos(t));
  return { axisA, axisB };
}

/** Five-point Gauss-Legendre quadrature on `[a, b]`. */
function gaussLegendre5(f: (x: number) => number, a: number, b: number): number {
  const nodes = [
    -0.906179845938664, -0.538469310105683, 0, 0.538469310105683, 0.906179845938664,
  ];
  const weights = [
    0.236926885056189, 0.478628670499366, 0.568888888888889, 0.478628670499366,
    0.236926885056189,
  ];
  const half = (b - a) / 2;
  const mid = (a + b) / 2;
  let sum = 0;
  for (let i = 0; i < nodes.length; i++) {
    sum += weights[i]! * f(mid + half * nodes[i]!);
  }
  return sum * half;
}

// Close the cycle declared in `arc.ts` so that `Arc.transform` can widen to an
// ellipse under a non-conformal transform without a static circular import.
registerEllipseFactory((arc) => EllipseArc.fromArc(arc));
