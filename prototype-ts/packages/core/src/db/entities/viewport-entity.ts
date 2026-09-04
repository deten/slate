import { Entity, type GripEditable, type GripPoint } from '../entity.js';
import type { DbObject } from '../db-object.js';
import { Vec3 } from '../../math/vec3.js';
import { Mat4 } from '../../math/mat4.js';
import { Box3 } from '../../math/box3.js';
import type { Curve } from '../../geom/curve.js';
import { LineSegment } from '../../geom/line-segment.js';
import type { Handle } from '../handle.js';

/**
 * A paper-space viewport — the VIEWPORT entity.
 *
 * A viewport is a window on a sheet showing a region of model space at a fixed
 * scale. It is the mechanism that makes paper space useful, and it carries
 * three things that the rest of the system must respect:
 *
 *  - a **transform** between model space and this window on the sheet,
 *  - a **scale**, which annotation and lineweight display depend on,
 *  - a **per-viewport layer freeze list**, so the same model geometry can show
 *    different layers in different views on the same sheet.
 *
 * The frozen-layer list in particular is why layer visibility cannot be a
 * simple boolean on the layer: it is contextual.
 */
export class ViewportEntity extends Entity implements GripEditable {
  /**
   * Centre of the viewport window, in paper space.
   */
  center: Vec3 = Vec3.ZERO;

  /** Size of the window on the sheet, in paper-space units. */
  width = 100;
  height = 75;

  /**
   * Viewport number. other CAD software reserves 1 for the paper-space "overall"
   * viewport, which is not a real window and is never rendered as one.
   */
  viewportId = 2;

  /** Centre of the model-space view shown in this window. */
  viewCenter: Vec3 = Vec3.ZERO;

  /** Height of the model-space region shown, in drawing units. */
  viewHeight = 100;

  /** View direction; the default looks straight down the Z axis. */
  viewDirection: Vec3 = Vec3.UNIT_Z;
  viewTarget: Vec3 = Vec3.ZERO;

  /** Twist angle of the view within the window, in radians. */
  twistAngle = 0;

  /** Lens length for perspective views. */
  lensLength = 50;

  /** `true` when the viewport's contents are displayed. */
  isOn = true;

  /** `true` when the view is locked against accidental zooming. */
  isLocked = false;

  /**
   * Handles of layers frozen *only* in this viewport.
   *
   * A Set rather than an array: the renderer tests membership once per entity
   * per frame, which is the hottest lookup in paper-space rendering.
   */
  frozenLayerHandles: Set<Handle> = new Set();

  /** Clipping boundary entity, for non-rectangular viewports. */
  clipEntityHandle: Handle | null = null;

  /** `true` when the UCS icon is displayed in this viewport. */
  showsUcsIcon = true;
  /** `true` when the grid is displayed in this viewport. */
  showsGrid = false;

  override get objectType(): string {
    return 'Viewport';
  }

  /**
   * `true` for the pseudo-viewport that represents the sheet itself.
   * It carries paper-space display settings but has no model view.
   */
  get isOverallViewport(): boolean {
    return this.viewportId === 1;
  }

  /**
   * Scale factor from model space to paper space.
   *
   * This is the number a drafter sets to "1:100" and the number annotation
   * scaling depends on, so it is derived from the authoritative view height
   * rather than stored separately and risking divergence.
   */
  get scaleFactor(): number {
    return this.viewHeight === 0 ? 1 : this.height / this.viewHeight;
  }

  /** Set the scale, adjusting the model view height to match. */
  setScaleFactor(scale: number): void {
    if (scale > 0) this.viewHeight = this.height / scale;
  }

  /** The window rectangle on the sheet. */
  get windowBounds(): Box3 {
    const half = new Vec3(this.width / 2, this.height / 2, 0);
    return new Box3(this.center.sub(half), this.center.add(half));
  }

  /**
   * Region of model space visible through this window.
   * Used to cull model geometry when rendering a sheet.
   */
  get visibleModelBounds(): Box3 {
    const halfHeight = this.viewHeight / 2;
    const halfWidth = (this.viewHeight * this.width) / (this.height * 2);
    const half = new Vec3(halfWidth, halfHeight, 0);
    // The twist rotates the visible region, so bound the rotated rectangle.
    const local = new Box3(half.negate(), half);
    return local
      .transform(Mat4.rotationZ(this.twistAngle))
      .transform(Mat4.translation(this.viewCenter));
  }

  /**
   * Transform taking model-space coordinates into this sheet's paper space.
   * Composed as: recentre on the view, apply twist, scale, then place at the
   * window centre.
   */
  get modelToPaper(): Mat4 {
    const s = this.scaleFactor;
    return Mat4.translation(this.center)
      .multiply(Mat4.scaling(new Vec3(s, s, s)))
      .multiply(Mat4.rotationZ(-this.twistAngle))
      .multiply(Mat4.translation(this.viewCenter.negate()));
  }

  /** Inverse of {@link modelToPaper}; `null` if the viewport is degenerate. */
  get paperToModel(): Mat4 | null {
    return this.modelToPaper.invert();
  }

  /** `true` when the given layer is frozen in this viewport specifically. */
  isLayerFrozen(layerHandle: Handle): boolean {
    return this.frozenLayerHandles.has(layerHandle);
  }

  freezeLayer(layerHandle: Handle): void {
    this.frozenLayerHandles.add(layerHandle);
  }

  thawLayer(layerHandle: Handle): void {
    this.frozenLayerHandles.delete(layerHandle);
  }

  override get bounds(): Box3 {
    return this.windowBounds;
  }

  /** The viewport border, so it can be selected by clicking its edge. */
  override toCurves(): Curve[] {
    const b = this.windowBounds;
    const [p0, p1, p2, p3] = b.corners2;
    return [
      new LineSegment(p0, p1),
      new LineSegment(p1, p2),
      new LineSegment(p2, p3),
      new LineSegment(p3, p0),
    ];
  }

  override transformBy(m: Mat4): void {
    this.center = m.transformPoint(this.center);
    const scale = m.scaleFactor2;
    this.width *= scale;
    this.height *= scale;
    // The model view is not rescaled: moving or resizing a viewport on the
    // sheet must not silently change the drawing scale it displays.
  }

  getGripPoints(): GripPoint[] {
    const corners = this.windowBounds.corners2;
    return [
      { position: this.center, kind: 'center', index: 0 },
      ...corners.map((position, i) => ({
        position,
        kind: 'vertex' as const,
        index: i + 1,
      })),
    ];
  }

  moveGrip(grip: GripPoint, to: Vec3): void {
    if (grip.index === 0) {
      this.center = to;
      return;
    }
    // Dragging a corner resizes the window about the opposite corner.
    const corners = this.windowBounds.corners2;
    const opposite = corners[(grip.index - 1 + 2) % 4]!;
    const box = Box3.fromCorners(opposite, to);
    this.center = box.center;
    this.width = Math.max(box.width, 1e-6);
    this.height = Math.max(box.height, 1e-6);
  }

  override clone(): ViewportEntity {
    const copy = new ViewportEntity();
    this.copyEntityInto(copy);
    copy.center = this.center;
    copy.width = this.width;
    copy.height = this.height;
    copy.viewportId = this.viewportId;
    copy.viewCenter = this.viewCenter;
    copy.viewHeight = this.viewHeight;
    copy.viewDirection = this.viewDirection;
    copy.viewTarget = this.viewTarget;
    copy.twistAngle = this.twistAngle;
    copy.lensLength = this.lensLength;
    copy.isOn = this.isOn;
    copy.isLocked = this.isLocked;
    copy.frozenLayerHandles = new Set(this.frozenLayerHandles);
    copy.clipEntityHandle = this.clipEntityHandle;
    copy.showsUcsIcon = this.showsUcsIcon;
    copy.showsGrid = this.showsGrid;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof ViewportEntity)) {
      throw new TypeError('Snapshot type mismatch restoring a viewport');
    }
    this.restoreEntityFrom(snapshot);
    this.center = snapshot.center;
    this.width = snapshot.width;
    this.height = snapshot.height;
    this.viewportId = snapshot.viewportId;
    this.viewCenter = snapshot.viewCenter;
    this.viewHeight = snapshot.viewHeight;
    this.viewDirection = snapshot.viewDirection;
    this.viewTarget = snapshot.viewTarget;
    this.twistAngle = snapshot.twistAngle;
    this.lensLength = snapshot.lensLength;
    this.isOn = snapshot.isOn;
    this.isLocked = snapshot.isLocked;
    this.frozenLayerHandles = new Set(snapshot.frozenLayerHandles);
    this.clipEntityHandle = snapshot.clipEntityHandle;
    this.showsUcsIcon = snapshot.showsUcsIcon;
    this.showsGrid = snapshot.showsGrid;
  }

  override referencedHandles(): Handle[] {
    const refs = [...super.referencedHandles(), ...this.frozenLayerHandles];
    if (this.clipEntityHandle !== null) refs.push(this.clipEntityHandle);
    return refs;
  }
}
