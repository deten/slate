/**
 * Angle utilities.
 *
 * Angles are radians everywhere inside the kernel. Degrees exist only at the
 * I/O boundary (DXF stores degrees) and in the UI layer; conversion happens
 * exactly once, at those boundaries, so no interior code has to guess a unit.
 */

export const TAU = Math.PI * 2;
export const DEG_TO_RAD = Math.PI / 180;
export const RAD_TO_DEG = 180 / Math.PI;

export function degToRad(deg: number): number {
  return deg * DEG_TO_RAD;
}

export function radToDeg(rad: number): number {
  return rad * RAD_TO_DEG;
}

/** Normalize into `[0, 2PI)`. */
export function normalizeAngle(a: number): number {
  const r = a % TAU;
  return r < 0 ? r + TAU : r;
}

/** Normalize into `(-PI, PI]`. */
export function normalizeAngleSigned(a: number): number {
  const r = normalizeAngle(a);
  return r > Math.PI ? r - TAU : r;
}

/**
 * Signed sweep from `from` to `to` in the given direction, always in
 * `[0, 2PI)`. A full circle is reported as 0, so callers that need to
 * distinguish "no sweep" from "full sweep" must track closure separately —
 * which arcs do, via their explicit start/end angles.
 */
export function sweepAngle(from: number, to: number, counterClockwise = true): number {
  return counterClockwise
    ? normalizeAngle(to - from)
    : normalizeAngle(from - to);
}

/**
 * `true` when `angle` lies on the counter-clockwise sweep from `start` to
 * `end`, inclusive of the endpoints within `tol`.
 */
export function angleInSweep(
  angle: number,
  start: number,
  end: number,
  tol = 1e-10,
): boolean {
  const sweep = normalizeAngle(end - start);
  const offset = normalizeAngle(angle - start);
  // A zero-length sweep means the angles coincide; treat as a full circle only
  // when the caller passed identical start/end, handled by the caller instead.
  return offset <= sweep + tol || offset >= TAU - tol;
}

/** Shortest absolute difference between two angles, in `[0, PI]`. */
export function angleDifference(a: number, b: number): number {
  return Math.abs(normalizeAngleSigned(a - b));
}

/**
 * Snap an angle to the nearest multiple of `increment`, offset by `base`.
 * Backs polar tracking and ortho mode.
 */
export function snapAngle(angle: number, increment: number, base = 0): number {
  if (increment <= 0) return angle;
  return base + Math.round((angle - base) / increment) * increment;
}
