import { Vec3 } from './vec3.js';
import { TOL, type Tolerance } from './tolerance.js';

/**
 * An immutable 4x4 affine transformation matrix, stored column-major
 * (the same layout WebGL expects, so render code can upload it directly).
 *
 * Index layout — `m[column * 4 + row]`:
 * ```
 *   m0  m4  m8  m12      Xx Yx Zx Tx
 *   m1  m5  m9  m13  =   Xy Yy Zy Ty
 *   m2  m6  m10 m14      Xz Yz Zz Tz
 *   m3  m7  m11 m15      0  0  0  1
 * ```
 *
 * 4x4 rather than a 2D 3x3: block insert transforms, UCS/ECS conversion and
 * later 3D all need the full form, and DWG's OCS extrusion handling is a 3D
 * rotation. Using 3x3 here would be a structural dead end.
 */
export class Mat4 {
  static readonly IDENTITY = new Mat4(
    new Float64Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  );

  /** Column-major elements. Never mutated after construction. */
  readonly m: Float64Array;

  private constructor(m: Float64Array) {
    this.m = m;
    Object.freeze(this);
  }

  /** Wrap 16 column-major elements. The array is copied. */
  static fromColumnMajor(values: ArrayLike<number>): Mat4 {
    if (values.length !== 16) {
      throw new RangeError(`Expected 16 elements, received ${values.length}`);
    }
    return new Mat4(Float64Array.from(values));
  }

  /** Build from three basis vectors and an origin. */
  static fromBasis(xAxis: Vec3, yAxis: Vec3, zAxis: Vec3, origin: Vec3 = Vec3.ZERO): Mat4 {
    return new Mat4(
      new Float64Array([
        xAxis.x, xAxis.y, xAxis.z, 0,
        yAxis.x, yAxis.y, yAxis.z, 0,
        zAxis.x, zAxis.y, zAxis.z, 0,
        origin.x, origin.y, origin.z, 1,
      ]),
    );
  }

  static translation(t: Vec3): Mat4 {
    return new Mat4(
      new Float64Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t.x, t.y, t.z, 1]),
    );
  }

  /** Uniform or non-uniform scale about `center`. */
  static scaling(factor: Vec3 | number, center: Vec3 = Vec3.ZERO): Mat4 {
    const s = typeof factor === 'number' ? new Vec3(factor, factor, factor) : factor;
    return new Mat4(
      new Float64Array([
        s.x, 0, 0, 0,
        0, s.y, 0, 0,
        0, 0, s.z, 0,
        center.x * (1 - s.x), center.y * (1 - s.y), center.z * (1 - s.z), 1,
      ]),
    );
  }

  /** Rotation about the Z axis through `center` — the 2D drafting rotation. */
  static rotationZ(angle: number, center: Vec3 = Vec3.ZERO): Mat4 {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return new Mat4(
      new Float64Array([
        c, s, 0, 0,
        -s, c, 0, 0,
        0, 0, 1, 0,
        center.x - center.x * c + center.y * s,
        center.y - center.x * s - center.y * c,
        0, 1,
      ]),
    );
  }

  /** Rotation of `angle` radians about an arbitrary axis through `origin`. */
  static rotationAboutAxis(axis: Vec3, angle: number, origin: Vec3 = Vec3.ZERO): Mat4 {
    const a = axis.normalize();
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    const t = 1 - c;
    const { x, y, z } = a;
    const r = new Mat4(
      new Float64Array([
        t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
        t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
        t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,
        0, 0, 0, 1,
      ]),
    );
    if (origin.isZero()) return r;
    return Mat4.translation(origin).multiply(r).multiply(Mat4.translation(origin.negate()));
  }

  /**
   * Mirror across the line through `point` with direction `direction`,
   * in the XY plane.
   */
  static mirror2(point: Vec3, direction: Vec3): Mat4 {
    const d = direction.flatten().normalize();
    const { x: dx, y: dy } = d;
    // Householder reflection about the line, then re-anchored at `point`.
    const a = dx * dx - dy * dy;
    const b = 2 * dx * dy;
    const reflect = new Mat4(
      new Float64Array([a, b, 0, 0, b, -a, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
    );
    return Mat4.translation(point).multiply(reflect).multiply(Mat4.translation(point.negate()));
  }

  /** Mirror across an arbitrary plane defined by a point and a normal. */
  static mirrorPlane(point: Vec3, normal: Vec3): Mat4 {
    const n = normal.normalize();
    const { x, y, z } = n;
    const reflect = new Mat4(
      new Float64Array([
        1 - 2 * x * x, -2 * x * y, -2 * x * z, 0,
        -2 * x * y, 1 - 2 * y * y, -2 * y * z, 0,
        -2 * x * z, -2 * y * z, 1 - 2 * z * z, 0,
        0, 0, 0, 1,
      ]),
    );
    return Mat4.translation(point).multiply(reflect).multiply(Mat4.translation(point.negate()));
  }

  /**
   * The composite transform used by block references: scale, then rotate about
   * Z, then translate. Order matters and is fixed by the DWG INSERT semantics.
   */
  static blockTransform(insertion: Vec3, scale: Vec3, rotation: number): Mat4 {
    return Mat4.translation(insertion)
      .multiply(Mat4.rotationZ(rotation))
      .multiply(Mat4.scaling(scale));
  }

  /** `this * other` — applies `other` first, then `this`. */
  multiply(other: Mat4): Mat4 {
    const a = this.m;
    const b = other.m;
    const out = new Float64Array(16);
    for (let col = 0; col < 4; col++) {
      const b0 = b[col * 4]!;
      const b1 = b[col * 4 + 1]!;
      const b2 = b[col * 4 + 2]!;
      const b3 = b[col * 4 + 3]!;
      for (let row = 0; row < 4; row++) {
        out[col * 4 + row] =
          a[row]! * b0 + a[4 + row]! * b1 + a[8 + row]! * b2 + a[12 + row]! * b3;
      }
    }
    return new Mat4(out);
  }

  /** Transform a position (translation applies). */
  transformPoint(p: Vec3): Vec3 {
    const m = this.m;
    return new Vec3(
      m[0]! * p.x + m[4]! * p.y + m[8]! * p.z + m[12]!,
      m[1]! * p.x + m[5]! * p.y + m[9]! * p.z + m[13]!,
      m[2]! * p.x + m[6]! * p.y + m[10]! * p.z + m[14]!,
    );
  }

  /** Transform a direction (translation ignored). */
  transformVector(v: Vec3): Vec3 {
    const m = this.m;
    return new Vec3(
      m[0]! * v.x + m[4]! * v.y + m[8]! * v.z,
      m[1]! * v.x + m[5]! * v.y + m[9]! * v.z,
      m[2]! * v.x + m[6]! * v.y + m[10]! * v.z,
    );
  }

  get translation(): Vec3 {
    return new Vec3(this.m[12]!, this.m[13]!, this.m[14]!);
  }

  /** Determinant of the upper-left 3x3 (the linear part). */
  get determinant3(): number {
    const m = this.m;
    return (
      m[0]! * (m[5]! * m[10]! - m[9]! * m[6]!) -
      m[4]! * (m[1]! * m[10]! - m[9]! * m[2]!) +
      m[8]! * (m[1]! * m[6]! - m[5]! * m[2]!)
    );
  }

  /** `true` when the transform flips handedness (an odd number of mirrors). */
  get isMirrored(): boolean {
    return this.determinant3 < 0;
  }

  /**
   * `true` when the linear part is a uniform scale times a rotation, i.e. the
   * transform maps circles to circles. Non-conformal transforms must convert
   * circles and arcs to ellipses, so this predicate gates that path.
   */
  isConformal(tol: Tolerance = TOL): boolean {
    const m = this.m;
    const cx = new Vec3(m[0]!, m[1]!, m[2]!);
    const cy = new Vec3(m[4]!, m[5]!, m[6]!);
    const cz = new Vec3(m[8]!, m[9]!, m[10]!);
    const lx = cx.length;
    const ly = cy.length;
    const lz = cz.length;
    if (Math.abs(lx - ly) > tol.point * Math.max(1, lx)) return false;
    if (Math.abs(lx - lz) > tol.point * Math.max(1, lx)) return false;
    // Columns must remain mutually orthogonal.
    return (
      Math.abs(cx.dot(cy)) <= tol.point * lx * ly + tol.point &&
      Math.abs(cx.dot(cz)) <= tol.point * lx * lz + tol.point &&
      Math.abs(cy.dot(cz)) <= tol.point * ly * lz + tol.point
    );
  }

  /**
   * Uniform scale factor of a conformal transform. For non-conformal
   * transforms this returns the geometric mean, which is the right value for
   * scaling scalar properties such as linetype scale.
   */
  get scaleFactor(): number {
    return Math.cbrt(Math.abs(this.determinant3));
  }

  /** Scale factor within the XY plane. */
  get scaleFactor2(): number {
    const m = this.m;
    const det2 = m[0]! * m[5]! - m[4]! * m[1]!;
    return Math.sqrt(Math.abs(det2));
  }

  /** Rotation of the transformed X axis within the XY plane, in radians. */
  get rotation2(): number {
    return Math.atan2(this.m[1]!, this.m[0]!);
  }

  transpose(): Mat4 {
    const m = this.m;
    const out = new Float64Array(16);
    for (let c = 0; c < 4; c++) {
      for (let r = 0; r < 4; r++) {
        out[c * 4 + r] = m[r * 4 + c]!;
      }
    }
    return new Mat4(out);
  }

  /**
   * General 4x4 inverse.
   * @returns the inverse, or `null` when the matrix is singular. Callers
   *   working with user geometry must handle `null` — a zero scale factor is
   *   an ordinary input error, not an exceptional condition.
   */
  invert(): Mat4 | null {
    const m = this.m;
    const inv = new Float64Array(16);

    inv[0] = m[5]! * m[10]! * m[15]! - m[5]! * m[11]! * m[14]! - m[9]! * m[6]! * m[15]! +
      m[9]! * m[7]! * m[14]! + m[13]! * m[6]! * m[11]! - m[13]! * m[7]! * m[10]!;
    inv[4] = -m[4]! * m[10]! * m[15]! + m[4]! * m[11]! * m[14]! + m[8]! * m[6]! * m[15]! -
      m[8]! * m[7]! * m[14]! - m[12]! * m[6]! * m[11]! + m[12]! * m[7]! * m[10]!;
    inv[8] = m[4]! * m[9]! * m[15]! - m[4]! * m[11]! * m[13]! - m[8]! * m[5]! * m[15]! +
      m[8]! * m[7]! * m[13]! + m[12]! * m[5]! * m[11]! - m[12]! * m[7]! * m[9]!;
    inv[12] = -m[4]! * m[9]! * m[14]! + m[4]! * m[10]! * m[13]! + m[8]! * m[5]! * m[14]! -
      m[8]! * m[6]! * m[13]! - m[12]! * m[5]! * m[10]! + m[12]! * m[6]! * m[9]!;
    inv[1] = -m[1]! * m[10]! * m[15]! + m[1]! * m[11]! * m[14]! + m[9]! * m[2]! * m[15]! -
      m[9]! * m[3]! * m[14]! - m[13]! * m[2]! * m[11]! + m[13]! * m[3]! * m[10]!;
    inv[5] = m[0]! * m[10]! * m[15]! - m[0]! * m[11]! * m[14]! - m[8]! * m[2]! * m[15]! +
      m[8]! * m[3]! * m[14]! + m[12]! * m[2]! * m[11]! - m[12]! * m[3]! * m[10]!;
    inv[9] = -m[0]! * m[9]! * m[15]! + m[0]! * m[11]! * m[13]! + m[8]! * m[1]! * m[15]! -
      m[8]! * m[3]! * m[13]! - m[12]! * m[1]! * m[11]! + m[12]! * m[3]! * m[9]!;
    inv[13] = m[0]! * m[9]! * m[14]! - m[0]! * m[10]! * m[13]! - m[8]! * m[1]! * m[14]! +
      m[8]! * m[2]! * m[13]! + m[12]! * m[1]! * m[10]! - m[12]! * m[2]! * m[9]!;
    inv[2] = m[1]! * m[6]! * m[15]! - m[1]! * m[7]! * m[14]! - m[5]! * m[2]! * m[15]! +
      m[5]! * m[3]! * m[14]! + m[13]! * m[2]! * m[7]! - m[13]! * m[3]! * m[6]!;
    inv[6] = -m[0]! * m[6]! * m[15]! + m[0]! * m[7]! * m[14]! + m[4]! * m[2]! * m[15]! -
      m[4]! * m[3]! * m[14]! - m[12]! * m[2]! * m[7]! + m[12]! * m[3]! * m[6]!;
    inv[10] = m[0]! * m[5]! * m[15]! - m[0]! * m[7]! * m[13]! - m[4]! * m[1]! * m[15]! +
      m[4]! * m[3]! * m[13]! + m[12]! * m[1]! * m[7]! - m[12]! * m[3]! * m[5]!;
    inv[14] = -m[0]! * m[5]! * m[14]! + m[0]! * m[6]! * m[13]! + m[4]! * m[1]! * m[14]! -
      m[4]! * m[2]! * m[13]! - m[12]! * m[1]! * m[6]! + m[12]! * m[2]! * m[5]!;
    inv[3] = -m[1]! * m[6]! * m[11]! + m[1]! * m[7]! * m[10]! + m[5]! * m[2]! * m[11]! -
      m[5]! * m[3]! * m[10]! - m[9]! * m[2]! * m[7]! + m[9]! * m[3]! * m[6]!;
    inv[7] = m[0]! * m[6]! * m[11]! - m[0]! * m[7]! * m[10]! - m[4]! * m[2]! * m[11]! +
      m[4]! * m[3]! * m[10]! + m[8]! * m[2]! * m[7]! - m[8]! * m[3]! * m[6]!;
    inv[11] = -m[0]! * m[5]! * m[11]! + m[0]! * m[7]! * m[9]! + m[4]! * m[1]! * m[11]! -
      m[4]! * m[3]! * m[9]! - m[8]! * m[1]! * m[7]! + m[8]! * m[3]! * m[5]!;
    inv[15] = m[0]! * m[5]! * m[10]! - m[0]! * m[6]! * m[9]! - m[4]! * m[1]! * m[10]! +
      m[4]! * m[2]! * m[9]! + m[8]! * m[1]! * m[6]! - m[8]! * m[2]! * m[5]!;

    const det = m[0]! * inv[0]! + m[1]! * inv[4]! + m[2]! * inv[8]! + m[3]! * inv[12]!;
    if (det === 0 || !Number.isFinite(det)) return null;

    const invDet = 1 / det;
    for (let i = 0; i < 16; i++) inv[i] = inv[i]! * invDet;
    return new Mat4(inv);
  }

  isIdentity(tol: Tolerance = TOL): boolean {
    const id = Mat4.IDENTITY.m;
    for (let i = 0; i < 16; i++) {
      if (Math.abs(this.m[i]! - id[i]!) > tol.point) return false;
    }
    return true;
  }

  equals(other: Mat4, tol: Tolerance = TOL): boolean {
    for (let i = 0; i < 16; i++) {
      if (Math.abs(this.m[i]! - other.m[i]!) > tol.point) return false;
    }
    return true;
  }

  toArray(): number[] {
    return Array.from(this.m);
  }

  toJSON(): number[] {
    return this.toArray();
  }

  static fromJSON(a: readonly number[]): Mat4 {
    return Mat4.fromColumnMajor(a);
  }
}
