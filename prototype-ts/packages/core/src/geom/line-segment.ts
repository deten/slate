import { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
import { Box3 } from '../math/box3.js';
import { TOL, type Tolerance, clamp } from '../math/tolerance.js';
import { Curve, type ClosestPointResult } from './curve.js';

/**
 * A bounded straight segment between two points.
 *
 * Parameter `t` runs linearly from 0 at {@link start} to 1 at {@link end},
 * so the normalized parameter is also the fraction of arc length.
 */
export class LineSegment extends Curve {
  readonly start: Vec3;
  readonly end: Vec3;

  constructor(start: Vec3, end: Vec3) {
    super();
    this.start = start;
    this.end = end;
    Object.freeze(this);
  }

  override get startPoint(): Vec3 {
    return this.start;
  }

  override get endPoint(): Vec3 {
    return this.end;
  }

  override get isClosed(): boolean {
    return false;
  }

  /** End minus start. Zero-length segments are legal and handled throughout. */
  get direction(): Vec3 {
    return this.end.sub(this.start);
  }

  /** Unit direction, or `null` for a degenerate (zero-length) segment. */
  unitDirection(tol: Tolerance = TOL): Vec3 | null {
    return this.direction.normalizeOrNull(tol);
  }

  isDegenerate(tol: Tolerance = TOL): boolean {
    return this.direction.isZero(tol);
  }

  override get length(): number {
    return this.start.distanceTo(this.end);
  }

  override pointAt(t: number): Vec3 {
    return this.start.lerp(this.end, this.clampParam(t));
  }

  /** Position at an unclamped parameter — used by extension logic. */
  pointAtExtended(t: number): Vec3 {
    return this.start.lerp(this.end, t);
  }

  override tangentAt(_t: number): Vec3 {
    return this.direction;
  }

  override get bounds(): Box3 {
    return Box3.fromCorners(this.start, this.end);
  }

  override closestPoint(p: Vec3): ClosestPointResult {
    const d = this.direction;
    const lenSq = d.lengthSq;
    if (lenSq === 0) {
      return { point: this.start, param: 0, distance: p.distanceTo(this.start) };
    }
    const t = clamp(p.sub(this.start).dot(d) / lenSq, 0, 1);
    const point = this.start.addScaled(d, t);
    return { point, param: t, distance: p.distanceTo(point) };
  }

  override closestPointExtended(p: Vec3): ClosestPointResult | null {
    const d = this.direction;
    const lenSq = d.lengthSq;
    if (lenSq === 0) return null;
    const t = p.sub(this.start).dot(d) / lenSq;
    const point = this.start.addScaled(d, t);
    return { point, param: t, distance: p.distanceTo(point) };
  }

  /**
   * Perpendicular foot from `p`, unrestricted by the segment's extent.
   * Backs the PERPENDICULAR object snap, which is allowed to land on the
   * extension of a segment.
   */
  perpendicularFoot(p: Vec3): Vec3 | null {
    return this.closestPointExtended(p)?.point ?? null;
  }

  /**
   * Signed perpendicular distance from `p` to the infinite line in the XY
   * plane. Positive when `p` is to the left of the direction of travel.
   */
  signedDistance2(p: Vec3): number {
    const d = this.direction.flatten();
    const len = d.length2;
    if (len === 0) return p.distance2To(this.start);
    return d.cross2(p.sub(this.start)) / len;
  }

  /** `true` when `p` lies on the infinite line through this segment. */
  isPointOnLine(p: Vec3, tol: Tolerance = TOL): boolean {
    const r = this.closestPointExtended(p);
    return r === null ? p.equals(this.start, tol) : r.distance <= tol.point;
  }

  override transform(m: Mat4): LineSegment {
    return new LineSegment(m.transformPoint(this.start), m.transformPoint(this.end));
  }

  override reverse(): LineSegment {
    return new LineSegment(this.end, this.start);
  }

  override tessellate(): Vec3[] {
    return [this.start, this.end];
  }

  override trim(t0: number, t1: number): LineSegment {
    return new LineSegment(this.pointAt(t0), this.pointAt(t1));
  }

  override split(params: readonly number[]): LineSegment[] {
    const cuts = sanitizeSplitParams(params);
    if (cuts.length === 0) return [this];
    const result: LineSegment[] = [];
    let prev = 0;
    for (const t of cuts) {
      result.push(this.trim(prev, t));
      prev = t;
    }
    result.push(this.trim(prev, 1));
    return result;
  }

  /**
   * Extend or shorten the segment so that it terminates at `p`, keeping
   * whichever endpoint is further from `p`. Returns `null` when `p` is not on
   * the infinite line within tolerance.
   */
  extendTo(p: Vec3, tol: Tolerance = TOL): LineSegment | null {
    if (!this.isPointOnLine(p, tol)) return null;
    const r = this.closestPointExtended(p);
    if (r === null) return null;
    // Below the start: the new segment runs from p to the current end.
    // Beyond the end: it runs from the current start to p.
    return r.param < 0.5 ? new LineSegment(p, this.end) : new LineSegment(this.start, p);
  }

  equals(other: LineSegment, tol: Tolerance = TOL): boolean {
    return this.start.equals(other.start, tol) && this.end.equals(other.end, tol);
  }

  override toString(): string {
    return `LineSegment(${this.start} -> ${this.end})`;
  }
}

/**
 * Sort split parameters, drop those outside the open interval `(0, 1)`, and
 * collapse duplicates. Shared by every curve's `split` implementation.
 */
export function sanitizeSplitParams(params: readonly number[], eps = 1e-12): number[] {
  const sorted = params
    .filter((t) => Number.isFinite(t) && t > eps && t < 1 - eps)
    .sort((a, b) => a - b);
  const out: number[] = [];
  for (const t of sorted) {
    const last = out[out.length - 1];
    if (last === undefined || t - last > eps) out.push(t);
  }
  return out;
}
