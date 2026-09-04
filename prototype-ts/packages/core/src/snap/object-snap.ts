import { Vec3 } from '../math/vec3.js';
import { TOL, type Tolerance } from '../math/tolerance.js';
import { Curve } from '../geom/curve.js';
import { LineSegment } from '../geom/line-segment.js';
import { Arc } from '../geom/arc.js';
import { EllipseArc } from '../geom/ellipse-arc.js';
import { Polyline } from '../geom/polyline.js';
import { intersectCurves } from '../geom/intersect.js';
import type { Entity } from '../db/entity.js';
import { PointEntity } from '../db/entities/curve-entities.js';
import { InsertEntity } from '../db/entities/insert-entity.js';
import type { Handle } from '../db/handle.js';

/**
 * Object snap modes.
 *
 * The bit values match the `OSMODE` setting in other CAD software system variable, so a drawing's
 * stored snap configuration transfers exactly rather than needing a mapping
 * that would drift.
 */
export enum SnapMode {
  None = 0,
  Endpoint = 1 << 0,
  Midpoint = 1 << 1,
  Center = 1 << 2,
  Node = 1 << 3,
  Quadrant = 1 << 4,
  Intersection = 1 << 5,
  Insertion = 1 << 6,
  Perpendicular = 1 << 7,
  Tangent = 1 << 8,
  Nearest = 1 << 9,
  /** Explicitly suppress snapping. */
  Clear = 1 << 10,
  ApparentIntersection = 1 << 11,
  Extension = 1 << 12,
  Parallel = 1 << 13,
}

/** Every snap that can be enabled, in the order they appear in the UI. */
export const ALL_SNAP_MODES: readonly SnapMode[] = [
  SnapMode.Endpoint,
  SnapMode.Midpoint,
  SnapMode.Center,
  SnapMode.Node,
  SnapMode.Quadrant,
  SnapMode.Intersection,
  SnapMode.Extension,
  SnapMode.Insertion,
  SnapMode.Perpendicular,
  SnapMode.Tangent,
  SnapMode.Nearest,
  SnapMode.ApparentIntersection,
  SnapMode.Parallel,
];

export const SNAP_MODE_NAMES: Readonly<Record<number, string>> = {
  [SnapMode.Endpoint]: 'Endpoint',
  [SnapMode.Midpoint]: 'Midpoint',
  [SnapMode.Center]: 'Center',
  [SnapMode.Node]: 'Node',
  [SnapMode.Quadrant]: 'Quadrant',
  [SnapMode.Intersection]: 'Intersection',
  [SnapMode.Insertion]: 'Insertion',
  [SnapMode.Perpendicular]: 'Perpendicular',
  [SnapMode.Tangent]: 'Tangent',
  [SnapMode.Nearest]: 'Nearest',
  [SnapMode.ApparentIntersection]: 'Apparent Intersection',
  [SnapMode.Extension]: 'Extension',
  [SnapMode.Parallel]: 'Parallel',
};

/**
 * A candidate snap position.
 */
export interface SnapPoint {
  readonly position: Vec3;
  readonly mode: SnapMode;
  /** Entity the snap belongs to. */
  readonly entityHandle: Handle | null;
  /** Distance from the cursor, in world units. */
  readonly distance: number;
  /**
   * Priority for resolving ties. Higher wins. Endpoint beats midpoint beats
   * nearest, regardless of which happens to be a fraction closer — otherwise
   * snapping feels random near a vertex.
   */
  readonly priority: number;
}

/**
 * Snap priority, highest first.
 *
 * The ordering is not arbitrary: it reflects how specific a snap is. An
 * endpoint is an exact, meaningful location; "nearest" is a fallback that
 * should only win when nothing better is in range. Without this, moving the
 * cursor near a line's end would flicker between endpoint and nearest.
 */
const SNAP_PRIORITY: Readonly<Record<number, number>> = {
  [SnapMode.Endpoint]: 100,
  [SnapMode.Intersection]: 95,
  [SnapMode.Midpoint]: 90,
  [SnapMode.Center]: 85,
  [SnapMode.Node]: 80,
  [SnapMode.Quadrant]: 75,
  [SnapMode.Insertion]: 70,
  [SnapMode.Perpendicular]: 60,
  [SnapMode.Tangent]: 55,
  [SnapMode.ApparentIntersection]: 50,
  [SnapMode.Extension]: 40,
  [SnapMode.Parallel]: 35,
  [SnapMode.Nearest]: 10,
};

/** An entity paired with its world-space curves, ready for snapping. */
export interface SnapCandidate {
  readonly handle: Handle | null;
  readonly entity: Entity;
  readonly curves: readonly Curve[];
}

export interface SnapQuery {
  /** Cursor position in world coordinates. */
  readonly cursor: Vec3;
  /**
   * Search radius in world units, derived from the pixel aperture and the
   * current zoom. Passing world units rather than pixels keeps the snap engine
   * independent of the view.
   */
  readonly aperture: number;
  /** Bitmask of enabled modes. */
  readonly modes: number;
  /** Entities within range, supplied by the spatial index. */
  readonly candidates: readonly SnapCandidate[];
  /**
   * Reference point for snaps that need one: PERPENDICULAR and TANGENT are
   * meaningless without knowing where the line is being drawn *from*.
   */
  readonly basePoint?: Vec3;
  readonly tolerance?: Tolerance;
}

/**
 * Computes object snap positions.
 *
 * Object snaps are the single largest contributor to whether a CAD program
 * feels precise or approximate. The requirements that shape this
 * implementation:
 *
 *  - **Deterministic priority.** Ties resolve by snap specificity, never by
 *    floating-point luck, so the same cursor position always gives the same
 *    snap.
 *  - **Bounded work.** Intersection snapping is O(n²) in candidate curves, so
 *    the candidate set comes pre-filtered from the spatial index and pairwise
 *    intersection is capped.
 *  - **No database access.** Candidates are passed in, which keeps this pure
 *    and testable and lets the same code snap to preview geometry that is not
 *    in the database at all.
 */
export class SnapEngine {
  /**
   * Find the best snap for a cursor position, or `null` when nothing is in
   * range.
   */
  findBest(query: SnapQuery): SnapPoint | null {
    const all = this.findAll(query);
    if (all.length === 0) return null;
    // Sort by priority first, then proximity. `findAll` already filtered to
    // within the aperture, so the highest-priority snap in range wins.
    all.sort((a, b) => b.priority - a.priority || a.distance - b.distance);
    return all[0]!;
  }

  /** Every snap within the aperture, unsorted. */
  findAll(query: SnapQuery): SnapPoint[] {
    const { cursor, aperture, modes, candidates } = query;
    const tol = query.tolerance ?? TOL;

    if ((modes & SnapMode.Clear) !== 0 || modes === SnapMode.None) return [];

    const results: SnapPoint[] = [];
    const add = (position: Vec3, mode: SnapMode, handle: Handle | null): void => {
      const distance = cursor.distance2To(position);
      if (distance > aperture) return;
      results.push({
        position,
        mode,
        entityHandle: handle,
        distance,
        priority: SNAP_PRIORITY[mode] ?? 0,
      });
    };

    for (const candidate of candidates) {
      // Insertion snaps to a block reference's or text's placement point.
      if ((modes & SnapMode.Insertion) !== 0) {
        const insertion = insertionPointOf(candidate.entity);
        if (insertion !== null) add(insertion, SnapMode.Insertion, candidate.handle);
      }

      // Node snaps to POINT entities, used heavily in survey workflows.
      if ((modes & SnapMode.Node) !== 0 && candidate.entity instanceof PointEntity) {
        add(candidate.entity.position, SnapMode.Node, candidate.handle);
      }

      for (const curve of candidate.curves) {
        this.snapCurve(curve, query, candidate.handle, add, tol);
      }
    }

    if ((modes & (SnapMode.Intersection | SnapMode.ApparentIntersection)) !== 0) {
      this.snapIntersections(query, add, tol);
    }

    return results;
  }

  /** Snaps derived from a single curve. */
  private snapCurve(
    curve: Curve,
    query: SnapQuery,
    handle: Handle | null,
    add: (p: Vec3, mode: SnapMode, handle: Handle | null) => void,
    tol: Tolerance,
  ): void {
    const { cursor, modes, basePoint } = query;

    if ((modes & SnapMode.Endpoint) !== 0) {
      if (curve instanceof Polyline) {
        // Every vertex of a polyline is an endpoint, not just the two ends.
        for (const vertex of curve.vertices) add(vertex.position, SnapMode.Endpoint, handle);
      } else if (!curve.isClosed) {
        add(curve.startPoint, SnapMode.Endpoint, handle);
        add(curve.endPoint, SnapMode.Endpoint, handle);
      }
    }

    if ((modes & SnapMode.Midpoint) !== 0) {
      if (curve instanceof Polyline) {
        // Each segment has its own midpoint; the whole polyline's is not
        // useful and is not what other CAD software offers.
        for (const segment of curve.segments) add(segment.midPoint, SnapMode.Midpoint, handle);
      } else {
        add(curve.midPoint, SnapMode.Midpoint, handle);
      }
    }

    if ((modes & SnapMode.Center) !== 0) {
      const center = centerOf(curve);
      if (center !== null) add(center, SnapMode.Center, handle);
    }

    if ((modes & SnapMode.Quadrant) !== 0) {
      for (const q of quadrantsOf(curve)) add(q, SnapMode.Quadrant, handle);
    }

    if ((modes & SnapMode.Perpendicular) !== 0 && basePoint !== undefined) {
      const foot = perpendicularFrom(curve, basePoint);
      if (foot !== null) add(foot, SnapMode.Perpendicular, handle);
    }

    if ((modes & SnapMode.Tangent) !== 0 && basePoint !== undefined) {
      for (const point of tangentPointsFrom(curve, basePoint, tol)) {
        add(point, SnapMode.Tangent, handle);
      }
    }

    if ((modes & SnapMode.Extension) !== 0) {
      const extended = curve.closestPointExtended(cursor);
      // Only report it when the point is genuinely off the curve's own extent;
      // otherwise this duplicates NEAREST at lower priority.
      if (extended !== null && (extended.param < 0 || extended.param > 1)) {
        add(extended.point, SnapMode.Extension, handle);
      }
    }

    if ((modes & SnapMode.Nearest) !== 0) {
      add(curve.closestPoint(cursor).point, SnapMode.Nearest, handle);
    }
  }

  /**
   * Intersections between candidate curves.
   *
   * Quadratic in the number of curves, so the pair count is capped. The cap is
   * generous relative to what fits in a snap aperture but prevents a
   * pathological pick in dense geometry from stalling the cursor.
   */
  private snapIntersections(
    query: SnapQuery,
    add: (p: Vec3, mode: SnapMode, handle: Handle | null) => void,
    tol: Tolerance,
  ): void {
    const apparent = (query.modes & SnapMode.ApparentIntersection) !== 0;
    const real = (query.modes & SnapMode.Intersection) !== 0;

    const curves: Array<{ curve: Curve; handle: Handle | null }> = [];
    for (const candidate of query.candidates) {
      for (const curve of candidate.curves) {
        curves.push({ curve, handle: candidate.handle });
      }
    }

    const MAX_PAIRS = 400;
    let pairs = 0;

    for (let i = 0; i < curves.length && pairs < MAX_PAIRS; i++) {
      for (let j = i + 1; j < curves.length && pairs < MAX_PAIRS; j++) {
        pairs++;
        const a = curves[i]!;
        const b = curves[j]!;
        const hits = intersectCurves(a.curve, b.curve, {
          tolerance: tol,
          extend: apparent,
        });
        for (const hit of hits) {
          const onBoth = !hit.onExtensionA && !hit.onExtensionB;
          if (onBoth && real) {
            add(hit.point, SnapMode.Intersection, a.handle);
          } else if (!onBoth && apparent) {
            add(hit.point, SnapMode.ApparentIntersection, a.handle);
          }
        }
      }
    }
  }
}

/** Centre of a curve that has one. */
function centerOf(curve: Curve): Vec3 | null {
  if (curve instanceof Arc) return curve.center;
  if (curve instanceof EllipseArc) return curve.center;
  if (curve instanceof Polyline) {
    // A polyline's arc segments each have a centre worth snapping to.
    for (const segment of curve.segments) {
      if (segment instanceof Arc) return segment.center;
    }
  }
  return null;
}

function quadrantsOf(curve: Curve): Vec3[] {
  if (curve instanceof Arc) return curve.quadrantPoints;
  if (curve instanceof EllipseArc) {
    return [
      curve.center.add(curve.majorAxis),
      curve.center.sub(curve.majorAxis),
      curve.center.add(curve.minorAxis),
      curve.center.sub(curve.minorAxis),
    ];
  }
  return [];
}

/**
 * Foot of the perpendicular from `from` onto the curve.
 *
 * Uses the *extended* projection, because other CAD software allows a perpendicular snap
 * to land beyond a segment's end — the resulting line is still perpendicular
 * to that segment's direction, which is what the user asked for.
 */
function perpendicularFrom(curve: Curve, from: Vec3): Vec3 | null {
  if (curve instanceof LineSegment) return curve.perpendicularFoot(from);
  if (curve instanceof Polyline) {
    let best: Vec3 | null = null;
    let bestDistance = Infinity;
    for (const segment of curve.segments) {
      if (!(segment instanceof LineSegment)) continue;
      const foot = segment.perpendicularFoot(from);
      if (foot === null) continue;
      const distance = from.distanceTo(foot);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = foot;
      }
    }
    return best;
  }
  // On a circle or arc the perpendicular from an external point passes
  // through the centre, so the foot is where that line meets the curve.
  if (curve instanceof Arc) {
    const direction = from.sub(curve.center).normalizeOrNull();
    if (direction === null) return null;
    return curve.center.addScaled(direction, curve.radius);
  }
  return curve.closestPoint(from).point;
}

/**
 * Tangent points on a circle from an external point.
 *
 * The two tangent points lie where the circle meets the circle whose diameter
 * is the segment from the centre to the external point — Thales' construction.
 */
function tangentPointsFrom(curve: Curve, from: Vec3, tol: Tolerance): Vec3[] {
  if (!(curve instanceof Arc)) return [];
  const toCenter = curve.center.sub(from);
  const distance = toCenter.length2;
  // A point inside the circle has no tangent to it.
  if (distance <= curve.radius + tol.point) return [];

  const angleToCenter = Math.atan2(toCenter.y, toCenter.x);
  const offset = Math.acos(curve.radius / distance);
  const results: Vec3[] = [];
  for (const sign of [1, -1]) {
    // Angle, measured at the circle's centre, of each tangent point.
    const angle = angleToCenter + Math.PI + sign * (Math.PI - offset);
    if (curve.containsAngle(angle, tol) || curve.isFullCircle) {
      results.push(curve.pointAtAngle(angle));
    }
  }
  return results;
}

/** Placement point of entities that have one. */
function insertionPointOf(entity: Entity): Vec3 | null {
  if (entity instanceof InsertEntity) return entity.position;
  const withPosition = entity as { position?: Vec3 };
  return withPosition.position instanceof Vec3 ? withPosition.position : null;
}

/** Convert a pixel aperture to world units at a given zoom. */
export function apertureInWorld(pixels: number, unitsPerPixel: number): number {
  return pixels * unitsPerPixel;
}

/** Human-readable name for a snap mode, for the tooltip and status bar. */
export function snapModeName(mode: SnapMode): string {
  return SNAP_MODE_NAMES[mode] ?? 'Snap';
}
