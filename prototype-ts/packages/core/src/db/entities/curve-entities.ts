import { Entity, type GripEditable, type GripPoint } from '../entity.js';
import type { DbObject } from '../db-object.js';
import { Vec3 } from '../../math/vec3.js';
import { Mat4 } from '../../math/mat4.js';
import { Box3 } from '../../math/box3.js';
import type { Curve } from '../../geom/curve.js';
import { LineSegment } from '../../geom/line-segment.js';
import { Arc } from '../../geom/arc.js';
import { EllipseArc } from '../../geom/ellipse-arc.js';
import { Polyline, polylineVertex, type PolylineVertex } from '../../geom/polyline.js';
import { TAU } from '../../math/angle.js';

/**
 * A straight line between two points — the LINE entity.
 */
export class LineEntity extends Entity implements GripEditable {
  start: Vec3 = Vec3.ZERO;
  end: Vec3 = Vec3.UNIT_X;
  /** Thickness extrudes the line into a plane; preserved, rarely used in 2D. */
  thickness = 0;

  static create(start: Vec3, end: Vec3): LineEntity {
    const e = new LineEntity();
    e.start = start;
    e.end = end;
    return e;
  }

  override get objectType(): string {
    return 'Line';
  }

  get geometry(): LineSegment {
    return new LineSegment(this.start, this.end);
  }

  override get bounds(): Box3 {
    return Box3.fromCorners(this.start, this.end);
  }

  override toCurves(): Curve[] {
    return [this.geometry];
  }

  override transformBy(m: Mat4): void {
    this.start = m.transformPoint(this.start);
    this.end = m.transformPoint(this.end);
  }

  getGripPoints(): GripPoint[] {
    return [
      { position: this.start, kind: 'endpoint', index: 0 },
      { position: this.start.midpoint(this.end), kind: 'midpoint', index: 1 },
      { position: this.end, kind: 'endpoint', index: 2 },
    ];
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    if (grip.index === 0) {
      this.start = to;
    } else if (grip.index === 2) {
      this.end = to;
    } else {
      // Dragging the midpoint translates the whole line.
      const delta = to.sub(this.start.midpoint(this.end));
      this.start = this.start.add(delta);
      this.end = this.end.add(delta);
    }
  }

  override clone(): LineEntity {
    const copy = new LineEntity();
    this.copyEntityInto(copy);
    copy.start = this.start;
    copy.end = this.end;
    copy.thickness = this.thickness;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof LineEntity)) {
      throw new TypeError('Snapshot type mismatch restoring a line');
    }
    this.restoreEntityFrom(snapshot);
    this.start = snapshot.start;
    this.end = snapshot.end;
    this.thickness = snapshot.thickness;
  }
}

/**
 * A full circle — the CIRCLE entity.
 *
 * Kept distinct from {@link ArcEntity} because DWG does, and because collapsing
 * them would lose the distinction on save. The shared behaviour lives in the
 * {@link Arc} geometry class rather than in a common entity base.
 */
export class CircleEntity extends Entity implements GripEditable {
  center: Vec3 = Vec3.ZERO;
  radius = 1;
  thickness = 0;

  static create(center: Vec3, radius: number): CircleEntity {
    const e = new CircleEntity();
    e.center = center;
    e.radius = radius;
    return e;
  }

  override get objectType(): string {
    return 'Circle';
  }

  get geometry(): Arc {
    return Arc.circle(this.center, this.radius, this.normal);
  }

  override get bounds(): Box3 {
    return this.geometry.bounds;
  }

  override toCurves(): Curve[] {
    return [this.geometry];
  }

  override transformBy(m: Mat4): void {
    const transformed = this.geometry.transform(m);
    if (transformed instanceof Arc) {
      this.center = transformed.center;
      this.radius = transformed.radius;
      this.normal = transformed.normal;
      return;
    }
    // A non-uniform scale has made this an ellipse. The entity cannot
    // represent that, so it keeps a circular approximation; the caller is
    // responsible for substituting an EllipseEntity when exactness matters.
    // `Database.transformEntity` does exactly that.
    this.center = m.transformPoint(this.center);
    this.radius = this.radius * m.scaleFactor2;
  }

  /** `true` when `m` would turn this circle into a genuine ellipse. */
  requiresEllipseAfter(m: Mat4): boolean {
    return !m.isConformal();
  }

  getGripPoints(): GripPoint[] {
    const grips: GripPoint[] = [{ position: this.center, kind: 'center', index: 0 }];
    const g = this.geometry;
    for (let i = 0; i < 4; i++) {
      grips.push({ position: g.pointAtAngle((i * Math.PI) / 2), kind: 'quadrant', index: i + 1 });
    }
    return grips;
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    if (grip.kind === 'center') {
      this.center = to;
    } else {
      this.radius = this.center.distanceTo(to);
    }
  }

  override clone(): CircleEntity {
    const copy = new CircleEntity();
    this.copyEntityInto(copy);
    copy.center = this.center;
    copy.radius = this.radius;
    copy.thickness = this.thickness;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof CircleEntity)) {
      throw new TypeError('Snapshot type mismatch restoring a circle');
    }
    this.restoreEntityFrom(snapshot);
    this.center = snapshot.center;
    this.radius = snapshot.radius;
    this.thickness = snapshot.thickness;
  }
}

/**
 * A circular arc — the ARC entity. Angles are stored in radians and always
 * describe a counter-clockwise sweep, matching DWG.
 */
export class ArcEntity extends Entity implements GripEditable {
  center: Vec3 = Vec3.ZERO;
  radius = 1;
  startAngle = 0;
  endAngle = Math.PI;
  thickness = 0;

  static create(center: Vec3, radius: number, startAngle: number, endAngle: number): ArcEntity {
    const e = new ArcEntity();
    e.center = center;
    e.radius = radius;
    e.startAngle = startAngle;
    e.endAngle = endAngle;
    return e;
  }

  static fromArc(arc: Arc): ArcEntity {
    const e = new ArcEntity();
    e.center = arc.center;
    e.radius = arc.radius;
    e.startAngle = arc.startAngle;
    e.endAngle = arc.endAngle;
    e.normal = arc.normal;
    return e;
  }

  override get objectType(): string {
    return 'Arc';
  }

  get geometry(): Arc {
    return new Arc(this.center, this.radius, this.startAngle, this.endAngle, this.normal);
  }

  override get bounds(): Box3 {
    return this.geometry.bounds;
  }

  override toCurves(): Curve[] {
    return [this.geometry];
  }

  override transformBy(m: Mat4): void {
    const transformed = this.geometry.transform(m);
    if (transformed instanceof Arc) {
      this.center = transformed.center;
      this.radius = transformed.radius;
      this.startAngle = transformed.startAngle;
      this.endAngle = transformed.endAngle;
      this.normal = transformed.normal;
    }
    // As with CircleEntity, a non-conformal transform needs an EllipseEntity;
    // the database-level helper performs the substitution.
  }

  requiresEllipseAfter(m: Mat4): boolean {
    return !m.isConformal();
  }

  getGripPoints(): GripPoint[] {
    const g = this.geometry;
    return [
      { position: this.center, kind: 'center', index: 0 },
      { position: g.startPoint, kind: 'endpoint', index: 1 },
      { position: g.midPoint, kind: 'midpoint', index: 2 },
      { position: g.endPoint, kind: 'endpoint', index: 3 },
    ];
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    const g = this.geometry;
    switch (grip.index) {
      case 0:
        this.center = to;
        break;
      case 1:
        this.startAngle = g.angleAtPoint(to);
        this.radius = this.center.distanceTo(to);
        break;
      case 3:
        this.endAngle = g.angleAtPoint(to);
        this.radius = this.center.distanceTo(to);
        break;
      default: {
        // Dragging the midpoint reshapes the arc through the three points.
        const rebuilt = Arc.fromThreePoints(g.startPoint, to, g.endPoint);
        if (rebuilt !== null) {
          this.center = rebuilt.center;
          this.radius = rebuilt.radius;
          this.startAngle = rebuilt.startAngle;
          this.endAngle = rebuilt.endAngle;
        }
        break;
      }
    }
  }

  override clone(): ArcEntity {
    const copy = new ArcEntity();
    this.copyEntityInto(copy);
    copy.center = this.center;
    copy.radius = this.radius;
    copy.startAngle = this.startAngle;
    copy.endAngle = this.endAngle;
    copy.thickness = this.thickness;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof ArcEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an arc');
    }
    this.restoreEntityFrom(snapshot);
    this.center = snapshot.center;
    this.radius = snapshot.radius;
    this.startAngle = snapshot.startAngle;
    this.endAngle = snapshot.endAngle;
    this.thickness = snapshot.thickness;
  }
}

/**
 * An elliptical arc — the ELLIPSE entity.
 */
export class EllipseEntity extends Entity implements GripEditable {
  center: Vec3 = Vec3.ZERO;
  /** Vector from centre to the major-axis endpoint. */
  majorAxis: Vec3 = Vec3.UNIT_X;
  ratio = 1;
  startParam = 0;
  endParam = TAU;

  static fromEllipseArc(e: EllipseArc): EllipseEntity {
    const entity = new EllipseEntity();
    entity.center = e.center;
    entity.majorAxis = e.majorAxis;
    entity.ratio = e.ratio;
    entity.startParam = e.startParam;
    entity.endParam = e.isClosed ? e.startParam + TAU : e.endParam;
    entity.normal = e.normal;
    return entity;
  }

  override get objectType(): string {
    return 'Ellipse';
  }

  get geometry(): EllipseArc {
    return new EllipseArc(
      this.center,
      this.majorAxis,
      this.ratio,
      this.startParam,
      this.endParam,
      this.normal,
    );
  }

  override get bounds(): Box3 {
    return this.geometry.bounds;
  }

  override toCurves(): Curve[] {
    return [this.geometry];
  }

  override transformBy(m: Mat4): void {
    const t = this.geometry.transform(m);
    this.center = t.center;
    this.majorAxis = t.majorAxis;
    this.ratio = t.ratio;
    this.startParam = t.startParam;
    this.endParam = t.isClosed ? t.startParam + TAU : t.endParam;
    this.normal = t.normal;
  }

  getGripPoints(): GripPoint[] {
    const g = this.geometry;
    return [
      { position: this.center, kind: 'center', index: 0 },
      { position: this.center.add(this.majorAxis), kind: 'quadrant', index: 1 },
      { position: this.center.sub(this.majorAxis), kind: 'quadrant', index: 2 },
      { position: this.center.add(g.minorAxis), kind: 'quadrant', index: 3 },
      { position: this.center.sub(g.minorAxis), kind: 'quadrant', index: 4 },
    ];
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    switch (grip.index) {
      case 0:
        this.center = to;
        break;
      case 1:
      case 2: {
        const newMajor = grip.index === 1 ? to.sub(this.center) : this.center.sub(to);
        const minorLength = this.geometry.semiMinor;
        const majorLength = newMajor.length;
        if (majorLength > 0) {
          this.majorAxis = newMajor;
          this.ratio = Math.min(1, Math.max(1e-6, minorLength / majorLength));
        }
        break;
      }
      default: {
        const minorLength = this.center.distanceTo(to);
        const majorLength = this.majorAxis.length;
        if (majorLength > 0) {
          this.ratio = Math.min(1, Math.max(1e-6, minorLength / majorLength));
        }
        break;
      }
    }
  }

  override clone(): EllipseEntity {
    const copy = new EllipseEntity();
    this.copyEntityInto(copy);
    copy.center = this.center;
    copy.majorAxis = this.majorAxis;
    copy.ratio = this.ratio;
    copy.startParam = this.startParam;
    copy.endParam = this.endParam;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof EllipseEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an ellipse');
    }
    this.restoreEntityFrom(snapshot);
    this.center = snapshot.center;
    this.majorAxis = snapshot.majorAxis;
    this.ratio = snapshot.ratio;
    this.startParam = snapshot.startParam;
    this.endParam = snapshot.endParam;
  }
}

/**
 * A polyline — the LWPOLYLINE and POLYLINE entities.
 *
 * Both DXF forms map to this single entity. The `isLegacyFormat` flag records
 * which one the file used so that a round-trip writes back the same type;
 * everything else about them is equivalent for 2D drafting.
 */
export class PolylineEntity extends Entity implements GripEditable {
  vertices: PolylineVertex[] = [];
  closed = false;
  /** Uniform width applied when no per-vertex width is set. */
  constantWidth = 0;
  thickness = 0;
  /** `true` when the source was the heavyweight POLYLINE entity. */
  isLegacyFormat = false;

  static fromPolyline(poly: Polyline): PolylineEntity {
    const e = new PolylineEntity();
    e.vertices = [...poly.vertices];
    e.closed = poly.isClosed;
    e.normal = poly.normal;
    return e;
  }

  static fromPoints(points: readonly Vec3[], closed = false): PolylineEntity {
    const e = new PolylineEntity();
    e.vertices = points.map((p) => polylineVertex(p));
    e.closed = closed;
    return e;
  }

  override get objectType(): string {
    return 'Polyline';
  }

  get geometry(): Polyline {
    return new Polyline(this.vertices, this.closed, this.normal);
  }

  override get bounds(): Box3 {
    return this.geometry.bounds;
  }

  override toCurves(): Curve[] {
    return [this.geometry];
  }

  /** The individual segments, for commands that operate segment-wise. */
  get segments(): readonly Curve[] {
    return this.geometry.segments;
  }

  override transformBy(m: Mat4): void {
    const t = this.geometry.transform(m);
    this.vertices = [...t.vertices];
    this.normal = t.normal;
    this.constantWidth *= m.scaleFactor2;
  }

  getGripPoints(): GripPoint[] {
    return this.vertices.map((v, i) => ({
      position: v.position,
      kind: 'vertex' as const,
      index: i,
    }));
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    const existing = this.vertices[grip.index];
    if (existing === undefined) return;
    this.vertices[grip.index] = polylineVertex(
      to,
      existing.bulge,
      existing.startWidth,
      existing.endWidth,
    );
  }

  override clone(): PolylineEntity {
    const copy = new PolylineEntity();
    this.copyEntityInto(copy);
    copy.vertices = this.vertices.map((v) => ({ ...v }));
    copy.closed = this.closed;
    copy.constantWidth = this.constantWidth;
    copy.thickness = this.thickness;
    copy.isLegacyFormat = this.isLegacyFormat;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof PolylineEntity)) {
      throw new TypeError('Snapshot type mismatch restoring a polyline');
    }
    this.restoreEntityFrom(snapshot);
    this.vertices = snapshot.vertices.map((v) => ({ ...v }));
    this.closed = snapshot.closed;
    this.constantWidth = snapshot.constantWidth;
    this.thickness = snapshot.thickness;
    this.isLegacyFormat = snapshot.isLegacyFormat;
  }
}

/**
 * A single point marker — the POINT entity. Used as a node snap target and by
 * survey/setting-out workflows.
 */
export class PointEntity extends Entity implements GripEditable {
  position: Vec3 = Vec3.ZERO;
  /** Rotation of the point's display marker, in radians. */
  angle = 0;

  static create(position: Vec3): PointEntity {
    const e = new PointEntity();
    e.position = position;
    return e;
  }

  override get objectType(): string {
    return 'Point';
  }

  override get bounds(): Box3 {
    return Box3.fromPoint(this.position);
  }

  override toCurves(): Curve[] {
    return [];
  }

  override distanceToPoint(p: Vec3): number {
    return this.position.distanceTo(p);
  }

  override transformBy(m: Mat4): void {
    this.position = m.transformPoint(this.position);
  }

  getGripPoints(): GripPoint[] {
    return [{ position: this.position, kind: 'endpoint', index: 0 }];
  }

  moveGrip(_grip: GripPoint, to: Vec3): void {
    this.position = to;
  }

  override clone(): PointEntity {
    const copy = new PointEntity();
    this.copyEntityInto(copy);
    copy.position = this.position;
    copy.angle = this.angle;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof PointEntity)) {
      throw new TypeError('Snapshot type mismatch restoring a point');
    }
    this.restoreEntityFrom(snapshot);
    this.position = snapshot.position;
    this.angle = snapshot.angle;
  }
}
