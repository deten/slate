import { TOL, type Tolerance, clamp } from './tolerance.js';

/**
 * An immutable 3D vector / point in world space.
 *
 * Deliberately 3-component even though the initial product is a 2D drafting
 * application: DWG itself stores every point as a 3D position with an extrusion
 * direction, and collapsing to 2D here would be the single hardest decision to
 * undo later. 2D operations simply leave `z` at 0.
 *
 * Instances are frozen and every operation returns a new vector. Hot paths that
 * cannot tolerate allocation (per-frame rendering) work on packed
 * `Float64Array` buffers instead of this type.
 */
export class Vec3 {
  static readonly ZERO = new Vec3(0, 0, 0);
  static readonly UNIT_X = new Vec3(1, 0, 0);
  static readonly UNIT_Y = new Vec3(0, 1, 0);
  static readonly UNIT_Z = new Vec3(0, 0, 1);

  readonly x: number;
  readonly y: number;
  readonly z: number;

  constructor(x: number, y: number, z = 0) {
    this.x = x;
    this.y = y;
    this.z = z;
    Object.freeze(this);
  }

  /** Construct from an XY pair, leaving Z at 0. */
  static xy(x: number, y: number): Vec3 {
    return new Vec3(x, y, 0);
  }

  /** Unit vector at `angle` radians in the XY plane. */
  static fromAngle(angle: number, radius = 1): Vec3 {
    return new Vec3(Math.cos(angle) * radius, Math.sin(angle) * radius, 0);
  }

  static from(v: { x: number; y: number; z?: number }): Vec3 {
    return new Vec3(v.x, v.y, v.z ?? 0);
  }

  add(v: Vec3): Vec3 {
    return new Vec3(this.x + v.x, this.y + v.y, this.z + v.z);
  }

  sub(v: Vec3): Vec3 {
    return new Vec3(this.x - v.x, this.y - v.y, this.z - v.z);
  }

  scale(s: number): Vec3 {
    return new Vec3(this.x * s, this.y * s, this.z * s);
  }

  /** Component-wise multiply — used for non-uniform scaling. */
  mul(v: Vec3): Vec3 {
    return new Vec3(this.x * v.x, this.y * v.y, this.z * v.z);
  }

  negate(): Vec3 {
    return new Vec3(-this.x, -this.y, -this.z);
  }

  /** `this + v * s`, the fused form used throughout the geometry code. */
  addScaled(v: Vec3, s: number): Vec3 {
    return new Vec3(this.x + v.x * s, this.y + v.y * s, this.z + v.z * s);
  }

  dot(v: Vec3): number {
    return this.x * v.x + this.y * v.y + this.z * v.z;
  }

  cross(v: Vec3): Vec3 {
    return new Vec3(
      this.y * v.z - this.z * v.y,
      this.z * v.x - this.x * v.z,
      this.x * v.y - this.y * v.x,
    );
  }

  /** Z component of the 2D cross product — sign gives orientation in XY. */
  cross2(v: Vec3): number {
    return this.x * v.y - this.y * v.x;
  }

  get length(): number {
    return Math.hypot(this.x, this.y, this.z);
  }

  get lengthSq(): number {
    return this.x * this.x + this.y * this.y + this.z * this.z;
  }

  /** Length ignoring Z. */
  get length2(): number {
    return Math.hypot(this.x, this.y);
  }

  distanceTo(v: Vec3): number {
    return Math.hypot(this.x - v.x, this.y - v.y, this.z - v.z);
  }

  distanceSqTo(v: Vec3): number {
    const dx = this.x - v.x;
    const dy = this.y - v.y;
    const dz = this.z - v.z;
    return dx * dx + dy * dy + dz * dz;
  }

  /** Distance ignoring Z — the common case for 2D drafting predicates. */
  distance2To(v: Vec3): number {
    return Math.hypot(this.x - v.x, this.y - v.y);
  }

  /**
   * Unit vector in the same direction.
   * @throws {RangeError} if the vector is shorter than the tolerance; callers
   *   that can legitimately receive a degenerate vector should use
   *   {@link normalizeOrNull} instead of catching.
   */
  normalize(tol: Tolerance = TOL): Vec3 {
    const n = this.normalizeOrNull(tol);
    if (n === null) {
      throw new RangeError('Cannot normalize a zero-length vector');
    }
    return n;
  }

  normalizeOrNull(tol: Tolerance = TOL): Vec3 | null {
    const len = this.length;
    if (len <= tol.point) return null;
    return new Vec3(this.x / len, this.y / len, this.z / len);
  }

  /** Rotate about the Z axis through the origin. */
  rotateZ(angle: number): Vec3 {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return new Vec3(this.x * c - this.y * s, this.x * s + this.y * c, this.z);
  }

  /** Rotate about the Z axis through `center`. */
  rotateAbout(center: Vec3, angle: number): Vec3 {
    return this.sub(center).rotateZ(angle).add(center);
  }

  /**
   * Rotate about an arbitrary axis through the origin (Rodrigues' formula).
   * Present so that 3D work does not require revisiting this class.
   */
  rotateAboutAxis(axis: Vec3, angle: number): Vec3 {
    const k = axis.normalize();
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return this.scale(c)
      .add(k.cross(this).scale(s))
      .add(k.scale(k.dot(this) * (1 - c)));
  }

  /** Perpendicular in the XY plane, rotated +90°. */
  perp2(): Vec3 {
    return new Vec3(-this.y, this.x, this.z);
  }

  /** Angle of this vector in the XY plane, in `(-PI, PI]`. */
  get angle2(): number {
    return Math.atan2(this.y, this.x);
  }

  lerp(v: Vec3, t: number): Vec3 {
    return new Vec3(
      this.x + (v.x - this.x) * t,
      this.y + (v.y - this.y) * t,
      this.z + (v.z - this.z) * t,
    );
  }

  midpoint(v: Vec3): Vec3 {
    return new Vec3((this.x + v.x) * 0.5, (this.y + v.y) * 0.5, (this.z + v.z) * 0.5);
  }

  equals(v: Vec3, tol: Tolerance = TOL): boolean {
    return this.distanceSqTo(v) <= tol.point * tol.point;
  }

  /** Equality ignoring Z. */
  equals2(v: Vec3, tol: Tolerance = TOL): boolean {
    const dx = this.x - v.x;
    const dy = this.y - v.y;
    return dx * dx + dy * dy <= tol.point * tol.point;
  }

  isZero(tol: Tolerance = TOL): boolean {
    return this.lengthSq <= tol.point * tol.point;
  }

  /** `true` when both vectors point the same way (not merely parallel). */
  isCodirectional(v: Vec3, tol: Tolerance = TOL): boolean {
    const a = this.normalizeOrNull(tol);
    const b = v.normalizeOrNull(tol);
    if (a === null || b === null) return false;
    return a.dot(b) >= Math.cos(tol.angle);
  }

  /** `true` when the vectors are parallel, in either direction. */
  isParallel(v: Vec3, tol: Tolerance = TOL): boolean {
    const a = this.normalizeOrNull(tol);
    const b = v.normalizeOrNull(tol);
    if (a === null || b === null) return false;
    return Math.abs(a.dot(b)) >= Math.cos(tol.angle);
  }

  isPerpendicular(v: Vec3, tol: Tolerance = TOL): boolean {
    const a = this.normalizeOrNull(tol);
    const b = v.normalizeOrNull(tol);
    if (a === null || b === null) return false;
    return Math.abs(a.dot(b)) <= Math.sin(tol.angle);
  }

  /** Unsigned angle between two vectors, in `[0, PI]`. */
  angleTo(v: Vec3, tol: Tolerance = TOL): number {
    const a = this.normalizeOrNull(tol);
    const b = v.normalizeOrNull(tol);
    if (a === null || b === null) return 0;
    // Clamped because floating point can push the dot product past +/-1.
    return Math.acos(clamp(a.dot(b), -1, 1));
  }

  /** Drop to the XY plane. */
  flatten(): Vec3 {
    return this.z === 0 ? this : new Vec3(this.x, this.y, 0);
  }

  withZ(z: number): Vec3 {
    return new Vec3(this.x, this.y, z);
  }

  get isFinite(): boolean {
    return Number.isFinite(this.x) && Number.isFinite(this.y) && Number.isFinite(this.z);
  }

  toArray(): [number, number, number] {
    return [this.x, this.y, this.z];
  }

  toString(): string {
    return `(${this.x}, ${this.y}, ${this.z})`;
  }

  toJSON(): [number, number, number] {
    return [this.x, this.y, this.z];
  }

  static fromJSON(a: readonly number[]): Vec3 {
    return new Vec3(a[0] ?? 0, a[1] ?? 0, a[2] ?? 0);
  }
}

/** Convenience constructor: `v(1, 2)` reads better than `new Vec3(1, 2)`. */
export function v(x: number, y: number, z = 0): Vec3 {
  return new Vec3(x, y, z);
}
