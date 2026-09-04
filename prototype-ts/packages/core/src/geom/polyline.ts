import { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
import { Box3 } from '../math/box3.js';
import { TOL, type Tolerance, clamp } from '../math/tolerance.js';
import {
  Curve,
  DEFAULT_TESSELLATION,
  type AreaCurve,
  type ClosestPointResult,
  type TessellationOptions,
} from './curve.js';
import { LineSegment, sanitizeSplitParams } from './line-segment.js';
import { Arc } from './arc.js';

/**
 * One vertex of a polyline.
 *
 * `bulge` describes the arc from *this* vertex to the next one, as
 * `tan(includedAngle / 4)` — the DWG/DXF representation. Storing it on the
 * vertex rather than deriving arcs eagerly keeps the polyline losslessly
 * round-trippable and keeps vertex editing cheap.
 *
 * `startWidth`/`endWidth` are carried for fidelity; the renderer honours them
 * and editing preserves them even where commands do not yet manipulate them.
 */
export interface PolylineVertex {
  readonly position: Vec3;
  readonly bulge: number;
  readonly startWidth: number;
  readonly endWidth: number;
}

export function polylineVertex(
  position: Vec3,
  bulge = 0,
  startWidth = 0,
  endWidth = startWidth,
): PolylineVertex {
  return Object.freeze({ position, bulge, startWidth, endWidth });
}

/**
 * A polyline: a connected chain of line and circular-arc segments.
 *
 * This is the workhorse of real drawings — most DWG geometry is polylines —
 * so it is a first-class curve rather than a container of separate entities.
 *
 * Normalized parameter `t` is *segment-indexed*: `t = i / segmentCount` lands
 * exactly on vertex `i`. That makes vertex-level operations exact and keeps
 * `split` cheap; arc-length queries are available separately via
 * {@link paramAtLength}.
 */
export class Polyline extends Curve implements AreaCurve {
  readonly vertices: readonly PolylineVertex[];
  readonly closed: boolean;
  readonly normal: Vec3;

  private cachedSegments: Curve[] | null = null;
  private cachedLengths: number[] | null = null;

  constructor(vertices: readonly PolylineVertex[], closed = false, normal: Vec3 = Vec3.UNIT_Z) {
    super();
    this.vertices = Object.freeze([...vertices]);
    // Two vertices are enough to close a polyline: with bulges, the pair forms
    // a lens or a full circle, which other CAD software both permits and produces. Only a
    // single vertex has no closing segment to speak of.
    this.closed = closed && vertices.length >= 2;
    this.normal = normal;
  }

  /** Build from bare points, with straight segments throughout. */
  static fromPoints(points: readonly Vec3[], closed = false): Polyline {
    return new Polyline(
      points.map((p) => polylineVertex(p)),
      closed,
    );
  }

  /** An axis-aligned rectangle, as produced by the RECTANG command. */
  static rectangle(corner1: Vec3, corner2: Vec3): Polyline {
    const z = corner1.z;
    return Polyline.fromPoints(
      [
        new Vec3(corner1.x, corner1.y, z),
        new Vec3(corner2.x, corner1.y, z),
        new Vec3(corner2.x, corner2.y, z),
        new Vec3(corner1.x, corner2.y, z),
      ],
      true,
    );
  }

  get vertexCount(): number {
    return this.vertices.length;
  }

  /**
   * Number of segments: one fewer than the vertex count when open, equal to it
   * when closed (the extra segment runs from the last vertex back to the first).
   */
  get segmentCount(): number {
    const n = this.vertices.length;
    if (n < 2) return 0;
    return this.closed ? n : n - 1;
  }

  get isEmpty(): boolean {
    return this.segmentCount === 0;
  }

  /**
   * The individual segments as concrete curves. Computed once and memoized —
   * every geometric query needs them and rebuilding per call is the single
   * easiest way to make a large drawing slow.
   */
  get segments(): readonly Curve[] {
    if (this.cachedSegments !== null) return this.cachedSegments;
    const result: Curve[] = [];
    const n = this.vertices.length;
    const count = this.segmentCount;
    for (let i = 0; i < count; i++) {
      const a = this.vertices[i]!;
      const b = this.vertices[(i + 1) % n]!;
      const arc = a.bulge === 0 ? null : Arc.fromBulge(a.position, b.position, a.bulge);
      result.push(arc ?? new LineSegment(a.position, b.position));
    }
    this.cachedSegments = result;
    return result;
  }

  /** Segment at `index`, or `undefined` when out of range. */
  segmentAt(index: number): Curve | undefined {
    return this.segments[index];
  }

  private get segmentLengths(): number[] {
    if (this.cachedLengths !== null) return this.cachedLengths;
    this.cachedLengths = this.segments.map((s) => s.length);
    return this.cachedLengths;
  }

  override get isClosed(): boolean {
    return this.closed;
  }

  override get startPoint(): Vec3 {
    return this.vertices[0]?.position ?? Vec3.ZERO;
  }

  override get endPoint(): Vec3 {
    if (this.closed) return this.startPoint;
    return this.vertices[this.vertices.length - 1]?.position ?? Vec3.ZERO;
  }

  override get length(): number {
    let total = 0;
    for (const l of this.segmentLengths) total += l;
    return total;
  }

  /** Decompose a normalized parameter into a segment index and local parameter. */
  resolveParam(t: number): { index: number; local: number } {
    const count = this.segmentCount;
    if (count === 0) return { index: 0, local: 0 };
    const scaled = this.clampParam(t) * count;
    let index = Math.floor(scaled);
    if (index >= count) index = count - 1;
    return { index, local: scaled - index };
  }

  /** Inverse of {@link resolveParam}. */
  makeParam(index: number, local: number): number {
    const count = this.segmentCount;
    if (count === 0) return 0;
    return clamp((index + local) / count, 0, 1);
  }

  override pointAt(t: number): Vec3 {
    if (this.isEmpty) return this.startPoint;
    const { index, local } = this.resolveParam(t);
    return this.segments[index]!.pointAt(local);
  }

  override tangentAt(t: number): Vec3 {
    if (this.isEmpty) return Vec3.UNIT_X;
    const { index, local } = this.resolveParam(t);
    return this.segments[index]!.tangentAt(local);
  }

  /** Normalized parameter at a given distance along the polyline. */
  paramAtLength(distance: number): number {
    const lengths = this.segmentLengths;
    const total = this.length;
    if (total <= 0) return 0;
    let remaining = clamp(distance, 0, total);
    for (let i = 0; i < lengths.length; i++) {
      const segLen = lengths[i]!;
      if (remaining <= segLen || i === lengths.length - 1) {
        const local = segLen > 0 ? remaining / segLen : 0;
        return this.makeParam(i, clamp(local, 0, 1));
      }
      remaining -= segLen;
    }
    return 1;
  }

  /** Distance along the polyline at a normalized parameter. */
  lengthAtParam(t: number): number {
    const { index, local } = this.resolveParam(t);
    const lengths = this.segmentLengths;
    let total = 0;
    for (let i = 0; i < index; i++) total += lengths[i]!;
    return total + (lengths[index] ?? 0) * local;
  }

  override get bounds(): Box3 {
    return Box3.union(this.segments.map((s) => s.bounds));
  }

  override closestPoint(p: Vec3): ClosestPointResult {
    if (this.isEmpty) {
      const point = this.startPoint;
      return { point, param: 0, distance: p.distanceTo(point) };
    }
    let best: ClosestPointResult | null = null;
    let bestIndex = 0;
    const segs = this.segments;
    for (let i = 0; i < segs.length; i++) {
      const r = segs[i]!.closestPoint(p);
      if (best === null || r.distance < best.distance) {
        best = r;
        bestIndex = i;
      }
    }
    return {
      point: best!.point,
      param: this.makeParam(bestIndex, best!.param),
      distance: best!.distance,
    };
  }

  /**
   * Polylines have no meaningful unbounded host geometry, so extension is
   * defined only against the first and last segments — which is exactly what
   * EXTEND and the extension snap need.
   */
  override closestPointExtended(p: Vec3): ClosestPointResult | null {
    if (this.closed || this.isEmpty) return null;
    const segs = this.segments;
    const first = segs[0]!.closestPointExtended(p);
    const last = segs[segs.length - 1]!.closestPointExtended(p);
    if (first === null) return last;
    if (last === null) return first;
    return first.distance <= last.distance ? first : last;
  }

  /**
   * Signed area in the XY plane, positive when counter-clockwise.
   *
   * The shoelace formula over the vertices plus the circular-segment
   * contribution of each bulged span — the standard exact result for a
   * line/arc polygon, and what the AREA command reports.
   */
  get signedArea(): number {
    const n = this.vertices.length;
    if (n < 3 && !this.closed) return 0;
    let area = 0;
    const count = this.closed ? n : n - 1;
    for (let i = 0; i < count; i++) {
      const a = this.vertices[i]!.position;
      const b = this.vertices[(i + 1) % n]!.position;
      area += a.x * b.y - b.x * a.y;
    }
    area *= 0.5;

    // Each arc adds (or removes) the circular segment between chord and arc.
    const segs = this.segments;
    for (let i = 0; i < segs.length; i++) {
      const seg = segs[i]!;
      if (!(seg instanceof Arc)) continue;
      const bulge = this.vertices[i]!.bulge;
      const segmentArea = 0.5 * seg.radius * seg.radius * (seg.sweep - Math.sin(seg.sweep));
      area += bulge > 0 ? segmentArea : -segmentArea;
    }
    return area;
  }

  get area(): number {
    return Math.abs(this.signedArea);
  }

  /** Counter-clockwise when the signed area is positive. */
  get isCounterClockwise(): boolean {
    return this.signedArea > 0;
  }

  /**
   * Point-in-polygon test for closed polylines, using the winding number of
   * the tessellated boundary. Arcs are tessellated finely enough that the
   * result matches the true curved boundary to within `chordTolerance`.
   */
  containsPointInside(p: Vec3, options: TessellationOptions = DEFAULT_TESSELLATION): boolean {
    if (!this.closed) return false;
    const pts = this.tessellate(options);
    let winding = 0;
    for (let i = 0; i < pts.length - 1; i++) {
      const a = pts[i]!;
      const b = pts[i + 1]!;
      if (a.y <= p.y) {
        if (b.y > p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) > 0) winding++;
      } else if (b.y <= p.y && (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y) < 0) {
        winding--;
      }
    }
    return winding !== 0;
  }

  override transform(m: Mat4): Polyline {
    // Bulge is invariant under conformal transforms but flips sign under a
    // mirror, since the sweep direction reverses.
    const flip = m.isMirrored ? -1 : 1;
    const scale = m.scaleFactor2;
    const vertices = this.vertices.map((v) =>
      polylineVertex(
        m.transformPoint(v.position),
        v.bulge * flip,
        v.startWidth * scale,
        v.endWidth * scale,
      ),
    );
    const normal = m.transformVector(this.normal).normalizeOrNull() ?? this.normal;
    return new Polyline(vertices, this.closed, normal);
  }

  override reverse(): Polyline {
    const n = this.vertices.length;
    if (n === 0) return this;
    const reversed: PolylineVertex[] = [];
    // Bulge and widths belong to the span *after* each vertex, so reversing
    // the order also shifts those attributes back by one and negates the bulge.
    for (let i = n - 1; i >= 0; i--) {
      const v = this.vertices[i]!;
      const source = this.closed
        ? this.vertices[(i - 1 + n) % n]!
        : this.vertices[Math.max(i - 1, 0)]!;
      const carry = i === 0 && !this.closed ? { bulge: 0, startWidth: 0, endWidth: 0 } : source;
      reversed.push(
        polylineVertex(v.position, -carry.bulge, carry.endWidth, carry.startWidth),
      );
    }
    return new Polyline(reversed, this.closed, this.normal);
  }

  override tessellate(options: TessellationOptions = DEFAULT_TESSELLATION): Vec3[] {
    const points: Vec3[] = [];
    const segs = this.segments;
    for (let i = 0; i < segs.length; i++) {
      const pts = segs[i]!.tessellate(options);
      // Skip the leading point of every segment after the first; it duplicates
      // the previous segment's endpoint.
      for (let j = i === 0 ? 0 : 1; j < pts.length; j++) points.push(pts[j]!);
    }
    if (points.length === 0 && this.vertices.length === 1) {
      points.push(this.vertices[0]!.position);
    }
    return points;
  }

  override trim(t0: number, t1: number): Polyline {
    const a = this.clampParam(Math.min(t0, t1));
    const b = this.clampParam(Math.max(t0, t1));
    const start = this.resolveParam(a);
    const end = this.resolveParam(b);

    const out: PolylineVertex[] = [];
    if (start.index === end.index) {
      const seg = this.segments[start.index];
      if (seg === undefined) return new Polyline([], false, this.normal);
      const piece = seg.trim(start.local, end.local);
      out.push(polylineVertex(piece.startPoint, piece instanceof Arc ? piece.bulge : 0));
      out.push(polylineVertex(piece.endPoint));
      return new Polyline(out, false, this.normal);
    }

    const firstSeg = this.segments[start.index]!.trim(start.local, 1);
    out.push(
      polylineVertex(firstSeg.startPoint, firstSeg instanceof Arc ? firstSeg.bulge : 0),
    );
    for (let i = start.index + 1; i < end.index; i++) {
      const v = this.vertices[i % this.vertices.length]!;
      out.push(v);
    }
    const lastSeg = this.segments[end.index]!.trim(0, end.local);
    out.push(
      polylineVertex(lastSeg.startPoint, lastSeg instanceof Arc ? lastSeg.bulge : 0),
    );
    out.push(polylineVertex(lastSeg.endPoint));
    return new Polyline(out, false, this.normal);
  }

  override split(params: readonly number[]): Polyline[] {
    const cuts = sanitizeSplitParams(params);
    if (cuts.length === 0) return [this];
    const result: Polyline[] = [];
    let prev = 0;
    for (const t of cuts) {
      result.push(this.trim(prev, t));
      prev = t;
    }
    result.push(this.trim(prev, 1));
    return result;
  }

  /** A copy with one vertex replaced — the primitive behind grip editing. */
  withVertex(index: number, vertex: PolylineVertex): Polyline {
    if (index < 0 || index >= this.vertices.length) {
      throw new RangeError(`Vertex index ${index} out of range`);
    }
    const next = [...this.vertices];
    next[index] = vertex;
    return new Polyline(next, this.closed, this.normal);
  }

  withVertexInserted(index: number, vertex: PolylineVertex): Polyline {
    const next = [...this.vertices];
    next.splice(clamp(index, 0, next.length), 0, vertex);
    return new Polyline(next, this.closed, this.normal);
  }

  withVertexRemoved(index: number): Polyline {
    const next = [...this.vertices];
    next.splice(index, 1);
    return new Polyline(next, this.closed, this.normal);
  }

  withClosed(closed: boolean): Polyline {
    return new Polyline(this.vertices, closed, this.normal);
  }

  /** `true` when every bulge is zero — a pure line-segment chain. */
  get hasArcs(): boolean {
    return this.vertices.some((v) => v.bulge !== 0);
  }

  /** `true` when any vertex carries a non-zero width. */
  get hasWidth(): boolean {
    return this.vertices.some((v) => v.startWidth !== 0 || v.endWidth !== 0);
  }

  equals(other: Polyline, tol: Tolerance = TOL): boolean {
    if (this.closed !== other.closed) return false;
    if (this.vertices.length !== other.vertices.length) return false;
    for (let i = 0; i < this.vertices.length; i++) {
      const a = this.vertices[i]!;
      const b = other.vertices[i]!;
      if (!a.position.equals(b.position, tol)) return false;
      if (Math.abs(a.bulge - b.bulge) > tol.point) return false;
    }
    return true;
  }

  override toString(): string {
    return `Polyline(${this.vertices.length} vertices, ${this.closed ? 'closed' : 'open'})`;
  }
}
