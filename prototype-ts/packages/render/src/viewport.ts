import { Vec3, Box3, Mat4, clamp } from '@cad/core';

/** A position in device (CSS) pixels, origin at the canvas top-left. */
export interface ScreenPoint {
  readonly x: number;
  readonly y: number;
}

/**
 * The mapping between world coordinates and the screen.
 *
 * Immutable: every navigation operation returns a new viewport. That makes
 * view state trivially comparable (so React re-renders only on real change),
 * trivially restorable (named views, undo of a zoom), and impossible to mutate
 * from two places at once during an animation.
 *
 * Two invariants the whole renderer depends on:
 *
 *  - **Y is flipped.** World Y increases upward, as every drafter expects;
 *    screen Y increases downward. The flip lives here and nowhere else.
 *  - **Zoom is `scale`, in pixels per world unit.** Storing pixels-per-unit
 *    rather than a "zoom percentage" keeps every derived quantity — line
 *    widths, snap apertures, tessellation tolerance, text sizes — a single
 *    multiplication away, with no scale-factor conventions to get wrong.
 */
export class Viewport {
  /** World point displayed at the centre of the canvas. */
  readonly center: Vec3;

  /** Pixels per world unit. Always positive. */
  readonly scale: number;

  /** Canvas size in CSS pixels. */
  readonly width: number;
  readonly height: number;

  /** View rotation about the screen centre, in radians. */
  readonly rotation: number;

  /**
   * Device pixel ratio. Kept on the viewport so that hit testing, which works
   * in CSS pixels, and rendering, which works in device pixels, cannot
   * disagree about the conversion.
   */
  readonly pixelRatio: number;

  constructor(
    center: Vec3 = Vec3.ZERO,
    scale = 1,
    width = 800,
    height = 600,
    rotation = 0,
    pixelRatio = 1,
  ) {
    this.center = center;
    // A non-positive scale would invert or collapse the view and make the
    // inverse transform undefined, so it is clamped rather than trusted.
    this.scale = scale > 0 ? scale : 1;
    this.width = Math.max(1, width);
    this.height = Math.max(1, height);
    this.rotation = rotation;
    this.pixelRatio = pixelRatio > 0 ? pixelRatio : 1;
    Object.freeze(this);
  }

  /** World units per screen pixel — the reciprocal of {@link scale}. */
  get unitsPerPixel(): number {
    return 1 / this.scale;
  }

  /** Convert a world point to screen coordinates. */
  worldToScreen(world: Vec3): ScreenPoint {
    const dx = world.x - this.center.x;
    const dy = world.y - this.center.y;

    let rx = dx;
    let ry = dy;
    if (this.rotation !== 0) {
      const c = Math.cos(-this.rotation);
      const s = Math.sin(-this.rotation);
      rx = dx * c - dy * s;
      ry = dx * s + dy * c;
    }

    return {
      x: this.width / 2 + rx * this.scale,
      // Negated: screen Y grows downward while world Y grows upward.
      y: this.height / 2 - ry * this.scale,
    };
  }

  /** Convert a screen point to world coordinates on the Z = 0 plane. */
  screenToWorld(screen: ScreenPoint): Vec3 {
    const sx = (screen.x - this.width / 2) * this.unitsPerPixel;
    const sy = -(screen.y - this.height / 2) * this.unitsPerPixel;

    let wx = sx;
    let wy = sy;
    if (this.rotation !== 0) {
      const c = Math.cos(this.rotation);
      const s = Math.sin(this.rotation);
      wx = sx * c - sy * s;
      wy = sx * s + sy * c;
    }

    return new Vec3(this.center.x + wx, this.center.y + wy, 0);
  }

  /**
   * The full world-to-screen transform, for handing to a GPU or to a Canvas
   * `setTransform` call.
   */
  get worldToScreenMatrix(): Mat4 {
    return Mat4.translation(new Vec3(this.width / 2, this.height / 2, 0))
      .multiply(Mat4.scaling(new Vec3(this.scale, -this.scale, 1)))
      .multiply(Mat4.rotationZ(-this.rotation))
      .multiply(Mat4.translation(this.center.negate()));
  }

  /** World-space region currently visible. */
  get visibleBounds(): Box3 {
    // With rotation, the visible region is a rotated rectangle; its
    // axis-aligned bound is what culling needs.
    const corners: ScreenPoint[] = [
      { x: 0, y: 0 },
      { x: this.width, y: 0 },
      { x: this.width, y: this.height },
      { x: 0, y: this.height },
    ];
    return Box3.fromPoints(corners.map((c) => this.screenToWorld(c)));
  }

  /** Convert a length in pixels to world units. */
  pixelsToWorld(pixels: number): number {
    return pixels * this.unitsPerPixel;
  }

  /** Convert a length in world units to pixels. */
  worldToPixels(units: number): number {
    return units * this.scale;
  }

  // ------------------------------------------------------------ navigation

  /** Pan by a screen-space delta, as during a drag. */
  panByPixels(dx: number, dy: number): Viewport {
    // The delta is converted through the inverse rotation so that panning
    // follows the cursor even in a rotated view.
    const worldDx = dx * this.unitsPerPixel;
    const worldDy = -dy * this.unitsPerPixel;
    const c = Math.cos(this.rotation);
    const s = Math.sin(this.rotation);
    const offset = new Vec3(worldDx * c - worldDy * s, worldDx * s + worldDy * c, 0);
    return this.withCenter(this.center.sub(offset));
  }

  withCenter(center: Vec3): Viewport {
    return new Viewport(
      center,
      this.scale,
      this.width,
      this.height,
      this.rotation,
      this.pixelRatio,
    );
  }

  withScale(scale: number): Viewport {
    return new Viewport(
      this.center,
      clampScale(scale),
      this.width,
      this.height,
      this.rotation,
      this.pixelRatio,
    );
  }

  withRotation(rotation: number): Viewport {
    return new Viewport(
      this.center,
      this.scale,
      this.width,
      this.height,
      rotation,
      this.pixelRatio,
    );
  }

  withSize(width: number, height: number, pixelRatio = this.pixelRatio): Viewport {
    return new Viewport(this.center, this.scale, width, height, this.rotation, pixelRatio);
  }

  /**
   * Zoom by a factor while keeping one screen point anchored.
   *
   * This is what makes wheel and trackpad zoom feel right: the world point
   * under the cursor stays under the cursor. Zooming about the view centre
   * instead — the naive implementation — forces the user to pan after every
   * zoom, and is immediately noticeable as wrong.
   */
  zoomAt(anchor: ScreenPoint, factor: number): Viewport {
    const worldAnchor = this.screenToWorld(anchor);
    const zoomed = this.withScale(this.scale * factor);
    const afterAnchor = zoomed.screenToWorld(anchor);
    // Shift the centre by however far the anchor drifted.
    return zoomed.withCenter(zoomed.center.add(worldAnchor.sub(afterAnchor)));
  }

  /** Zoom about the view centre. */
  zoomBy(factor: number): Viewport {
    return this.withScale(this.scale * factor);
  }

  /**
   * Fit a world region to the canvas with a margin.
   *
   * Backs ZOOM EXTENTS and ZOOM WINDOW. An empty or degenerate box — a drawing
   * containing a single point, or nothing at all — would otherwise produce an
   * infinite scale, so it falls back to a sensible default view.
   */
  zoomToFit(bounds: Box3, marginFraction = 0.05): Viewport {
    if (bounds.isEmpty) return this;

    const width = bounds.width;
    const height = bounds.height;
    if (width <= 0 && height <= 0) {
      // A single point: centre on it and keep the current zoom.
      return this.withCenter(bounds.center);
    }

    const margin = 1 + marginFraction * 2;
    const scaleX = width > 0 ? this.width / (width * margin) : Infinity;
    const scaleY = height > 0 ? this.height / (height * margin) : Infinity;
    const scale = Math.min(scaleX, scaleY);

    return this.withCenter(bounds.center).withScale(
      Number.isFinite(scale) ? scale : this.scale,
    );
  }

  /** Zoom to a screen-space rectangle, as dragged by the user. */
  zoomToScreenRect(a: ScreenPoint, b: ScreenPoint): Viewport {
    const worldA = this.screenToWorld(a);
    const worldB = this.screenToWorld(b);
    return this.zoomToFit(Box3.fromCorners(worldA, worldB), 0);
  }

  /**
   * Interpolate toward another viewport, for animated navigation.
   *
   * Scale is interpolated *geometrically* rather than linearly. Zoom is
   * multiplicative, so a linear interpolation from 1x to 100x spends most of
   * the animation already zoomed in and lurches at the start; the geometric
   * path is what reads as smooth.
   */
  lerpTo(target: Viewport, t: number): Viewport {
    const clamped = clamp(t, 0, 1);
    const scale = this.scale * Math.pow(target.scale / this.scale, clamped);
    return new Viewport(
      this.center.lerp(target.center, clamped),
      scale,
      target.width,
      target.height,
      this.rotation + (target.rotation - this.rotation) * clamped,
      target.pixelRatio,
    );
  }

  equals(other: Viewport): boolean {
    return (
      this.center.equals(other.center) &&
      this.scale === other.scale &&
      this.width === other.width &&
      this.height === other.height &&
      this.rotation === other.rotation &&
      this.pixelRatio === other.pixelRatio
    );
  }

  toJSON(): {
    center: number[];
    scale: number;
    rotation: number;
  } {
    return { center: this.center.toJSON(), scale: this.scale, rotation: this.rotation };
  }
}

/**
 * Bounds on zoom.
 *
 * A CAD drawing can legitimately span from millimetres to kilometres, so the
 * range is wide — but not unbounded: beyond roughly these limits, `float64`
 * coordinate arithmetic in screen space starts losing precision and geometry
 * visibly jitters as it is panned.
 */
export const MIN_SCALE = 1e-9;
export const MAX_SCALE = 1e9;

function clampScale(scale: number): number {
  if (!Number.isFinite(scale) || scale <= 0) return 1;
  return clamp(scale, MIN_SCALE, MAX_SCALE);
}

/**
 * Chord tolerance for tessellating curves at the current zoom.
 *
 * Tying tessellation to the viewport is what makes curves smooth when zoomed
 * in without generating tens of thousands of segments for a circle that is
 * three pixels across. Half a pixel is below the threshold of visibility on
 * any display.
 */
export function chordToleranceFor(viewport: Viewport, subPixel = 0.5): number {
  return viewport.pixelsToWorld(subPixel);
}
