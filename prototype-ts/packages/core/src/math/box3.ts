import { Vec3 } from './vec3.js';
import type { Mat4 } from './mat4.js';
import { TOL, type Tolerance } from './tolerance.js';

/**
 * An axis-aligned bounding box (the CAD "extents").
 *
 * An empty box is represented by `min > max` on every axis, which makes
 * {@link Box3.EMPTY} a correct identity for union without special cases.
 */
export class Box3 {
  static readonly EMPTY = new Box3(
    new Vec3(Infinity, Infinity, Infinity),
    new Vec3(-Infinity, -Infinity, -Infinity),
  );

  readonly min: Vec3;
  readonly max: Vec3;

  constructor(min: Vec3, max: Vec3) {
    this.min = min;
    this.max = max;
    Object.freeze(this);
  }

  /** Box tightly containing all given points. */
  static fromPoints(points: Iterable<Vec3>): Box3 {
    let minX = Infinity, minY = Infinity, minZ = Infinity;
    let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
    for (const p of points) {
      if (p.x < minX) minX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.z < minZ) minZ = p.z;
      if (p.x > maxX) maxX = p.x;
      if (p.y > maxY) maxY = p.y;
      if (p.z > maxZ) maxZ = p.z;
    }
    return new Box3(new Vec3(minX, minY, minZ), new Vec3(maxX, maxY, maxZ));
  }

  static fromPoint(p: Vec3): Box3 {
    return new Box3(p, p);
  }

  /** Box from two opposite corners in any order. */
  static fromCorners(a: Vec3, b: Vec3): Box3 {
    return new Box3(
      new Vec3(Math.min(a.x, b.x), Math.min(a.y, b.y), Math.min(a.z, b.z)),
      new Vec3(Math.max(a.x, b.x), Math.max(a.y, b.y), Math.max(a.z, b.z)),
    );
  }

  static fromCenterSize(center: Vec3, size: Vec3): Box3 {
    const h = size.scale(0.5);
    return new Box3(center.sub(h), center.add(h));
  }

  get isEmpty(): boolean {
    return this.min.x > this.max.x || this.min.y > this.max.y || this.min.z > this.max.z;
  }

  get center(): Vec3 {
    return this.min.midpoint(this.max);
  }

  get size(): Vec3 {
    return this.isEmpty ? Vec3.ZERO : this.max.sub(this.min);
  }

  get width(): number {
    return this.isEmpty ? 0 : this.max.x - this.min.x;
  }

  get height(): number {
    return this.isEmpty ? 0 : this.max.y - this.min.y;
  }

  get depth(): number {
    return this.isEmpty ? 0 : this.max.z - this.min.z;
  }

  /** Length of the diagonal — a useful scale-independent size measure. */
  get diagonal(): number {
    return this.isEmpty ? 0 : this.min.distanceTo(this.max);
  }

  union(other: Box3): Box3 {
    if (other.isEmpty) return this;
    if (this.isEmpty) return other;
    return new Box3(
      new Vec3(
        Math.min(this.min.x, other.min.x),
        Math.min(this.min.y, other.min.y),
        Math.min(this.min.z, other.min.z),
      ),
      new Vec3(
        Math.max(this.max.x, other.max.x),
        Math.max(this.max.y, other.max.y),
        Math.max(this.max.z, other.max.z),
      ),
    );
  }

  static union(boxes: Iterable<Box3>): Box3 {
    let result = Box3.EMPTY;
    for (const b of boxes) result = result.union(b);
    return result;
  }

  expandToPoint(p: Vec3): Box3 {
    if (this.isEmpty) return Box3.fromPoint(p);
    return new Box3(
      new Vec3(Math.min(this.min.x, p.x), Math.min(this.min.y, p.y), Math.min(this.min.z, p.z)),
      new Vec3(Math.max(this.max.x, p.x), Math.max(this.max.y, p.y), Math.max(this.max.z, p.z)),
    );
  }

  /** Grow by `margin` on every side. */
  expand(margin: number): Box3 {
    if (this.isEmpty) return this;
    const m = new Vec3(margin, margin, margin);
    return new Box3(this.min.sub(m), this.max.add(m));
  }

  /** Intersection, or {@link Box3.EMPTY} when the boxes are disjoint. */
  intersection(other: Box3): Box3 {
    if (this.isEmpty || other.isEmpty) return Box3.EMPTY;
    const min = new Vec3(
      Math.max(this.min.x, other.min.x),
      Math.max(this.min.y, other.min.y),
      Math.max(this.min.z, other.min.z),
    );
    const max = new Vec3(
      Math.min(this.max.x, other.max.x),
      Math.min(this.max.y, other.max.y),
      Math.min(this.max.z, other.max.z),
    );
    const result = new Box3(min, max);
    return result.isEmpty ? Box3.EMPTY : result;
  }

  containsPoint(p: Vec3, tol: Tolerance = TOL): boolean {
    if (this.isEmpty) return false;
    const e = tol.point;
    return (
      p.x >= this.min.x - e && p.x <= this.max.x + e &&
      p.y >= this.min.y - e && p.y <= this.max.y + e &&
      p.z >= this.min.z - e && p.z <= this.max.z + e
    );
  }

  /** `true` when `other` lies entirely within this box (window selection). */
  containsBox(other: Box3, tol: Tolerance = TOL): boolean {
    if (this.isEmpty || other.isEmpty) return false;
    const e = tol.point;
    return (
      other.min.x >= this.min.x - e && other.max.x <= this.max.x + e &&
      other.min.y >= this.min.y - e && other.max.y <= this.max.y + e &&
      other.min.z >= this.min.z - e && other.max.z <= this.max.z + e
    );
  }

  /** `true` when the boxes overlap at all (crossing selection, culling). */
  intersects(other: Box3, tol: Tolerance = TOL): boolean {
    if (this.isEmpty || other.isEmpty) return false;
    const e = tol.point;
    return (
      this.min.x <= other.max.x + e && this.max.x >= other.min.x - e &&
      this.min.y <= other.max.y + e && this.max.y >= other.min.y - e &&
      this.min.z <= other.max.z + e && this.max.z >= other.min.z - e
    );
  }

  /** Overlap test ignoring Z — the common case for 2D drafting. */
  intersects2(other: Box3, tol: Tolerance = TOL): boolean {
    if (this.isEmpty || other.isEmpty) return false;
    const e = tol.point;
    return (
      this.min.x <= other.max.x + e && this.max.x >= other.min.x - e &&
      this.min.y <= other.max.y + e && this.max.y >= other.min.y - e
    );
  }

  /** Shortest distance from `p` to the box; 0 when inside. */
  distanceToPoint(p: Vec3): number {
    if (this.isEmpty) return Infinity;
    const dx = Math.max(this.min.x - p.x, 0, p.x - this.max.x);
    const dy = Math.max(this.min.y - p.y, 0, p.y - this.max.y);
    const dz = Math.max(this.min.z - p.z, 0, p.z - this.max.z);
    return Math.hypot(dx, dy, dz);
  }

  /**
   * Box containing this box after transformation.
   *
   * Transforms all eight corners rather than the two extreme points, which is
   * required for correctness under rotation.
   */
  transform(t: Mat4): Box3 {
    if (this.isEmpty) return this;
    const { min, max } = this;
    const corners = [
      new Vec3(min.x, min.y, min.z),
      new Vec3(max.x, min.y, min.z),
      new Vec3(min.x, max.y, min.z),
      new Vec3(max.x, max.y, min.z),
      new Vec3(min.x, min.y, max.z),
      new Vec3(max.x, min.y, max.z),
      new Vec3(min.x, max.y, max.z),
      new Vec3(max.x, max.y, max.z),
    ];
    return Box3.fromPoints(corners.map((c) => t.transformPoint(c)));
  }

  /** The four XY corners, counter-clockwise from the minimum. */
  get corners2(): [Vec3, Vec3, Vec3, Vec3] {
    const z = this.min.z;
    return [
      new Vec3(this.min.x, this.min.y, z),
      new Vec3(this.max.x, this.min.y, z),
      new Vec3(this.max.x, this.max.y, z),
      new Vec3(this.min.x, this.max.y, z),
    ];
  }

  equals(other: Box3, tol: Tolerance = TOL): boolean {
    if (this.isEmpty && other.isEmpty) return true;
    if (this.isEmpty !== other.isEmpty) return false;
    return this.min.equals(other.min, tol) && this.max.equals(other.max, tol);
  }

  toString(): string {
    return this.isEmpty ? 'Box3(empty)' : `Box3(${this.min} .. ${this.max})`;
  }

  toJSON(): { min: number[]; max: number[] } | null {
    return this.isEmpty ? null : { min: this.min.toJSON(), max: this.max.toJSON() };
  }
}
