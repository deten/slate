import type { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
import type { Box3 } from '../math/box3.js';
import { TOL, type Tolerance } from '../math/tolerance.js';

/**
 * Result of projecting a point onto a curve.
 */
export interface ClosestPointResult {
  /** The closest position on the curve. */
  readonly point: Vec3;
  /** Its normalized parameter, in `[0, 1]`. */
  readonly param: number;
  /** Distance from the query point to {@link point}. */
  readonly distance: number;
}

/**
 * Controls how a curve is converted to a polyline for display or for
 * algorithms that only work on line segments.
 */
export interface TessellationOptions {
  /**
   * Maximum allowed distance between the true curve and its chord, in world
   * units. This is the *only* quality knob that produces resolution-independent
   * results: the renderer sets it from the current zoom so that curves are
   * smooth at any scale without over-tessellating when zoomed out.
   */
  readonly chordTolerance: number;
  /** Hard cap on segment count, to bound work on pathological input. */
  readonly maxSegments?: number;
  /** Lower bound on segment count, so tiny arcs still look round. */
  readonly minSegments?: number;
}

export const DEFAULT_TESSELLATION: TessellationOptions = Object.freeze({
  chordTolerance: 1e-3,
  maxSegments: 4096,
  minSegments: 4,
});

/**
 * Base class for all parametric curves in the geometry kernel.
 *
 * Every curve is parameterized over a normalized `[0, 1]` range regardless of
 * its natural parameterization. This is what lets generic algorithms — trim,
 * split, extend, offset, intersection bookkeeping — operate on any curve type
 * without a type switch. Each subclass maps `[0, 1]` onto its own natural
 * domain internally.
 *
 * Curves are immutable value objects; they carry no identity, no layer, and no
 * database presence. Entities in the document model *own* curves and expose
 * them, which keeps the geometry kernel independently testable and reusable
 * (an offset operation should not need a database).
 */
export abstract class Curve {
  abstract get startPoint(): Vec3;
  abstract get endPoint(): Vec3;

  /** `true` when start and end coincide and the curve encloses an area. */
  abstract get isClosed(): boolean;

  /** Position at normalized parameter `t`, clamped to `[0, 1]`. */
  abstract pointAt(t: number): Vec3;

  /**
   * Non-normalized tangent at `t`, pointing in the direction of increasing
   * parameter. Magnitude is the parametric speed and carries no meaning to
   * callers; use `tangentAt(t).normalize()` for a direction.
   */
  abstract tangentAt(t: number): Vec3;

  /** Arc length of the whole curve. */
  abstract get length(): number;

  /** Tight axis-aligned bounds. */
  abstract get bounds(): Box3;

  /** Project a point onto the curve, restricted to the curve's own extent. */
  abstract closestPoint(p: Vec3): ClosestPointResult;

  /**
   * Project onto the curve's *unbounded* host geometry — the infinite line for
   * a segment, the full circle for an arc. Returns `null` for curves with no
   * meaningful extension (e.g. a closed polyline).
   *
   * This is what apparent-intersection, EXTEND and the "extension" object snap
   * are built on, and it is why extension is a first-class concept in the
   * kernel rather than special-cased per command.
   */
  abstract closestPointExtended(p: Vec3): ClosestPointResult | null;

  /** Apply an affine transform, returning the transformed curve. */
  abstract transform(m: Mat4): Curve;

  /** Reverse the parameterization; geometry is unchanged. */
  abstract reverse(): Curve;

  /**
   * Convert to a polyline approximation within `chordTolerance`.
   * Always includes the exact start and end points.
   */
  abstract tessellate(options?: TessellationOptions): Vec3[];

  /**
   * Split at the given normalized parameters, returning the pieces in
   * parameter order. Parameters at or beyond the ends are ignored, so
   * `split([])` returns `[this]`.
   */
  abstract split(params: readonly number[]): Curve[];

  /** Sub-curve between two normalized parameters. */
  abstract trim(t0: number, t1: number): Curve;

  /** Distance from a point to the curve. */
  distanceTo(p: Vec3): number {
    return this.closestPoint(p).distance;
  }

  /** `true` when `p` lies on the curve within tolerance. */
  containsPoint(p: Vec3, tol: Tolerance = TOL): boolean {
    return this.closestPoint(p).distance <= tol.point;
  }

  /** Midpoint by arc length — the MIDPOINT object snap. */
  get midPoint(): Vec3 {
    return this.pointAt(0.5);
  }

  /**
   * Normalized parameters at which to sample for a smooth tessellation.
   * Subclasses with curvature override this; straight curves need only the
   * endpoints.
   */
  protected clampParam(t: number): number {
    return t < 0 ? 0 : t > 1 ? 1 : t;
  }
}

/**
 * A curve that bounds an area and can report it. Implemented by closed curves
 * (circles, closed polylines, ellipses) and consumed by hatch boundary
 * processing and the AREA command.
 */
export interface AreaCurve {
  /** Signed area in the XY plane; positive when counter-clockwise. */
  readonly signedArea: number;
}

export function hasArea(c: Curve): c is Curve & AreaCurve {
  return 'signedArea' in c;
}
