/**
 * Tolerance model.
 *
 * CAD comparisons are never exact. Every geometric predicate in this codebase
 * takes an explicit {@link Tolerance} (defaulting to {@link TOL}) rather than
 * hard-coding an epsilon, so that a document can later carry its own precision
 * settings (e.g. drawings authored in microns vs. kilometres) without a
 * rewrite of the geometry layer.
 *
 * Two separate tolerances are tracked, mirroring the distinction every serious
 * kernel makes:
 *  - `point`   linear distance below which two positions are the same point.
 *  - `angle`   angular difference (radians) below which two directions are
 *              parallel / two angles are equal.
 */
export interface Tolerance {
  /** Linear tolerance, in drawing units. */
  readonly point: number;
  /** Angular tolerance, in radians. */
  readonly angle: number;
}

/**
 * Default working tolerance. Matches other CAD software's global tolerance
 * (`AcGeContext::gTol`) so that imported geometry behaves familiarly.
 */
export const TOL: Tolerance = Object.freeze({
  point: 1e-10,
  angle: 1e-10,
});

/**
 * A deliberately looser tolerance for user-facing / pick operations, where
 * inputs come from screen coordinates rather than exact arithmetic.
 */
export const PICK_TOL: Tolerance = Object.freeze({
  point: 1e-6,
  angle: 1e-6,
});

export function makeTolerance(point: number, angle: number = point): Tolerance {
  if (!(point > 0) || !(angle > 0)) {
    throw new RangeError('Tolerance components must be positive and finite');
  }
  return Object.freeze({ point, angle });
}

/** Scale a tolerance, e.g. to relax comparisons over a large extent. */
export function scaleTolerance(tol: Tolerance, factor: number): Tolerance {
  return makeTolerance(tol.point * factor, tol.angle * factor);
}

/** `true` when `a` and `b` are equal within the linear tolerance. */
export function eq(a: number, b: number, tol: Tolerance = TOL): boolean {
  return Math.abs(a - b) <= tol.point;
}

/** `true` when `v` is zero within the linear tolerance. */
export function isZero(v: number, tol: Tolerance = TOL): boolean {
  return Math.abs(v) <= tol.point;
}

/** `true` when two angles (radians) are equal within the angular tolerance. */
export function angleEq(a: number, b: number, tol: Tolerance = TOL): boolean {
  return Math.abs(a - b) <= tol.angle;
}

/** Clamp `v` into `[lo, hi]`. */
export function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

/**
 * Compare with a relative component, for magnitudes far from 1.0.
 * Used where values may be dot products of long vectors.
 */
export function eqRelative(a: number, b: number, tol: Tolerance = TOL): boolean {
  const diff = Math.abs(a - b);
  if (diff <= tol.point) return true;
  return diff <= tol.point * Math.max(Math.abs(a), Math.abs(b));
}
