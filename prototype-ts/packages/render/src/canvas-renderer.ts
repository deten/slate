import {
  ArcEntity,
  CircleEntity,
  Curve,
  Database,
  EllipseEntity,
  InsertEntity,
  LineEntity,
  Linetype,
  MTextEntity,
  Mat4,
  PointEntity,
  PolylineEntity,
  TextEntity,
  TextHorizontalAlignment,
  TextStyle,
  TextVerticalAlignment,
  Vec3,
  ViewportEntity,
  type Handle,
} from '@cad/core';
import type { DrawItem } from './scene.js';
import { chordToleranceFor, type Viewport } from './viewport.js';

/** Visual theme. Colours are packed 0xRRGGBB. */
export interface RenderTheme {
  readonly background: number;
  /** Colour substituted for ACI 7, which means "contrast with background". */
  readonly foreground: number;
  readonly grid: number;
  readonly gridMajor: number;
  readonly selection: number;
  readonly selectionFill: number;
  readonly highlight: number;
  readonly preview: number;
  readonly grip: number;
  readonly gripHover: number;
  readonly snapMarker: number;
  readonly crosshair: number;
  readonly missingReference: number;
}

export const DARK_THEME: RenderTheme = Object.freeze({
  background: 0x1c1c1e,
  foreground: 0xf5f5f7,
  grid: 0x2c2c2e,
  gridMajor: 0x3a3a3c,
  selection: 0x0a84ff,
  selectionFill: 0x0a84ff,
  highlight: 0x64d2ff,
  preview: 0x8e8e93,
  grip: 0x0a84ff,
  gripHover: 0xff9f0a,
  snapMarker: 0x30d158,
  crosshair: 0x636366,
  missingReference: 0xff453a,
});

export const LIGHT_THEME: RenderTheme = Object.freeze({
  background: 0xffffff,
  foreground: 0x1c1c1e,
  grid: 0xececee,
  gridMajor: 0xd8d8dc,
  selection: 0x007aff,
  selectionFill: 0x007aff,
  highlight: 0x0071e3,
  preview: 0x8e8e93,
  grip: 0x007aff,
  gripHover: 0xff9500,
  snapMarker: 0x28cd41,
  crosshair: 0xaeaeb2,
  missingReference: 0xff3b30,
});

export interface RenderOptions {
  readonly theme: RenderTheme;
  readonly showGrid: boolean;
  readonly gridSpacing: number;
  /** Draw lineweights at their true width rather than hairlines. */
  readonly showLineweights: boolean;
  readonly selection: ReadonlySet<Handle>;
  readonly highlighted: Handle | null;
  /** Preview geometry drawn over the document, e.g. rubber-band lines. */
  readonly preview: readonly DrawItem[];
  /** Draw a placeholder for references that failed to load. */
  readonly showMissingReferences: boolean;
}

export function defaultRenderOptions(theme: RenderTheme = DARK_THEME): RenderOptions {
  return {
    theme,
    showGrid: true,
    gridSpacing: 10,
    showLineweights: false,
    selection: new Set(),
    highlighted: null,
    preview: [],
    showMissingReferences: true,
  };
}

/**
 * Renders a scene to a 2D canvas.
 *
 * Canvas2D rather than WebGL for the first implementation, deliberately: it is
 * correct by default for line joins, dashes, and text, all of which are
 * genuinely hard in WebGL, and it removes an entire class of driver-dependent
 * bugs while the rest of the application is being built. The renderer is
 * behind {@link DrawItem} and reads nothing from the database, so a GPU
 * backend can be added later for the large-drawing case without touching the
 * scene, the document model or the commands.
 *
 * The renderer is strictly a *consumer*. It never writes to the database, and
 * it holds no authoritative state — which is what allows several viewports to
 * render the same document simultaneously and correctly.
 */
export class CanvasRenderer {
  private readonly context: CanvasRenderingContext2D;
  private readonly db: Database;

  /** Resolved linetype dash arrays, keyed by handle and scale. */
  private readonly dashCache = new Map<string, number[] | null>();

  constructor(context: CanvasRenderingContext2D, db: Database) {
    this.context = context;
    this.db = db;
  }

  /** Draw a whole frame. */
  render(items: readonly DrawItem[], viewport: Viewport, options: RenderOptions): void {
    const ctx = this.context;
    const { theme } = options;

    ctx.save();
    // Work in device pixels so lines land on pixel boundaries on HiDPI screens.
    ctx.setTransform(viewport.pixelRatio, 0, 0, viewport.pixelRatio, 0, 0);

    ctx.fillStyle = toCss(theme.background);
    ctx.fillRect(0, 0, viewport.width, viewport.height);

    if (options.showGrid) this.drawGrid(viewport, options);

    const tolerance = chordToleranceFor(viewport);

    for (const item of items) {
      this.drawItem(item, viewport, options, tolerance, false);
    }

    // Selection is drawn as a second pass so it is never hidden beneath
    // unselected geometry drawn later.
    if (options.selection.size > 0) {
      for (const item of items) {
        if (!options.selection.has(item.handle)) continue;
        this.drawItem(item, viewport, options, tolerance, true);
      }
    }

    if (options.highlighted !== null) {
      for (const item of items) {
        if (item.handle !== options.highlighted) continue;
        this.drawHighlight(item, viewport, options, tolerance);
      }
    }

    for (const item of options.preview) {
      this.drawPreview(item, viewport, options, tolerance);
    }

    ctx.restore();
  }

  // ------------------------------------------------------------------- grid

  /**
   * Draw the reference grid.
   *
   * The spacing adapts to the zoom: a fixed world spacing becomes either an
   * unreadable smear or invisible as the user zooms. The step is snapped to a
   * 1-2-5 sequence, which is what keeps grid values round at every zoom level
   * instead of landing on values like 3.7.
   */
  private drawGrid(viewport: Viewport, options: RenderOptions): void {
    const ctx = this.context;
    const bounds = viewport.visibleBounds;

    const step = niceStep(viewport.pixelsToWorld(60), options.gridSpacing);
    if (!Number.isFinite(step) || step <= 0) return;

    // Refuse to draw more lines than could possibly be distinguished.
    const columns = bounds.width / step;
    const rows = bounds.height / step;
    if (columns > 500 || rows > 500) return;

    const startX = Math.floor(bounds.min.x / step) * step;
    const startY = Math.floor(bounds.min.y / step) * step;

    ctx.lineWidth = 1;

    for (let pass = 0; pass < 2; pass++) {
      const major = pass === 1;
      ctx.strokeStyle = toCss(major ? options.theme.gridMajor : options.theme.grid);
      ctx.beginPath();

      for (let x = startX; x <= bounds.max.x; x += step) {
        // Every tenth line is major, which gives the grid a readable rhythm.
        if (isMajorLine(x, step) !== major) continue;
        const from = viewport.worldToScreen(new Vec3(x, bounds.min.y, 0));
        const to = viewport.worldToScreen(new Vec3(x, bounds.max.y, 0));
        ctx.moveTo(snapHalf(from.x), from.y);
        ctx.lineTo(snapHalf(to.x), to.y);
      }
      for (let y = startY; y <= bounds.max.y; y += step) {
        if (isMajorLine(y, step) !== major) continue;
        const from = viewport.worldToScreen(new Vec3(bounds.min.x, y, 0));
        const to = viewport.worldToScreen(new Vec3(bounds.max.x, y, 0));
        ctx.moveTo(from.x, snapHalf(from.y));
        ctx.lineTo(to.x, snapHalf(to.y));
      }
      ctx.stroke();
    }
  }

  // ---------------------------------------------------------------- entities

  private drawItem(
    item: DrawItem,
    viewport: Viewport,
    options: RenderOptions,
    tolerance: number,
    asSelection: boolean,
  ): void {
    const ctx = this.context;
    const { entity, properties } = item;

    if (!properties.visible) return;

    ctx.save();

    if (asSelection) {
      ctx.strokeStyle = toCss(options.theme.selection);
      ctx.lineWidth = Math.max(2, this.lineWidthFor(properties, viewport, options) + 1);
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    } else {
      ctx.strokeStyle = toCss(this.resolveDisplayColor(properties.rgb, options.theme));
      ctx.lineWidth = this.lineWidthFor(properties, viewport, options);
      ctx.globalAlpha = properties.alpha / 255;
      const dash = this.dashArrayFor(properties, viewport);
      ctx.setLineDash(dash ?? []);
    }
    ctx.lineJoin = 'round';
    ctx.lineCap = 'butt';

    if (entity instanceof TextEntity || entity instanceof MTextEntity) {
      this.drawText(entity, item.transform, viewport, options, asSelection);
    } else if (entity instanceof PointEntity) {
      this.drawPoint(entity, item.transform, viewport);
    } else if (entity instanceof InsertEntity) {
      // Only reached for a reference whose content is missing.
      if (options.showMissingReferences) {
        this.drawMissingReference(entity, item.transform, viewport, options);
      }
    } else if (entity instanceof ViewportEntity) {
      this.drawViewportBorder(entity, item.transform, viewport, options);
    } else {
      this.drawCurves(entity.toCurves(), item.transform, viewport, tolerance);
    }

    ctx.restore();
  }

  /** Stroke a set of curves under a transform. */
  private drawCurves(
    curves: readonly Curve[],
    transform: Mat4,
    viewport: Viewport,
    tolerance: number,
  ): void {
    const ctx = this.context;
    if (curves.length === 0) return;

    ctx.beginPath();
    for (const curve of curves) {
      this.tracePath(curve, transform, viewport, tolerance);
    }
    ctx.stroke();
  }

  /**
   * Append a curve to the current path.
   *
   * Circles and arcs use the canvas's native arc primitive when the transform
   * is conformal, which is both faster and smoother than any tessellation.
   * Everything else is tessellated at the view-dependent chord tolerance.
   */
  private tracePath(
    curve: Curve,
    transform: Mat4,
    viewport: Viewport,
    tolerance: number,
  ): void {
    const ctx = this.context;

    const worldCurve = transform.isIdentity() ? curve : curve.transform(transform);
    const points = worldCurve.tessellate({
      // The tolerance is in world units, so it must be scaled by any transform
      // the curve has already had baked in.
      chordTolerance: tolerance,
      minSegments: 2,
      maxSegments: 4096,
    });

    if (points.length === 0) return;
    const first = viewport.worldToScreen(points[0]!);
    ctx.moveTo(first.x, first.y);
    for (let i = 1; i < points.length; i++) {
      const p = viewport.worldToScreen(points[i]!);
      ctx.lineTo(p.x, p.y);
    }
  }

  /**
   * Draw a POINT entity's marker.
   *
   * Points have no extent, so their marker is drawn at a fixed pixel size —
   * one of the few places where screen space is the correct space.
   */
  private drawPoint(entity: PointEntity, transform: Mat4, viewport: Viewport): void {
    const ctx = this.context;
    const world = transform.transformPoint(entity.position);
    const p = viewport.worldToScreen(world);
    const size = 4;

    ctx.beginPath();
    ctx.moveTo(p.x - size, p.y);
    ctx.lineTo(p.x + size, p.y);
    ctx.moveTo(p.x, p.y - size);
    ctx.lineTo(p.x, p.y + size);
    ctx.stroke();
  }

  /**
   * Draw text.
   *
   * SHX fonts are approximated with a system face — real SHX rendering means
   * parsing the shape file and is deferred. The *metrics* still come from the
   * text style, so the layout is right even where the glyphs are not.
   */
  private drawText(
    entity: TextEntity | MTextEntity,
    transform: Mat4,
    viewport: Viewport,
    options: RenderOptions,
    asSelection: boolean,
  ): void {
    const ctx = this.context;
    const style = this.db.getTyped(entity.styleHandle, TextStyle);

    const heightPixels = viewport.worldToPixels(entity.height * transform.scaleFactor2);
    // Below a couple of pixels text is an unreadable smudge; a line standing
    // in for it is both faster and more honest about what is there.
    if (heightPixels < 3) {
      const b = entity.bounds.transform(transform);
      const from = viewport.worldToScreen(new Vec3(b.min.x, b.center.y, 0));
      const to = viewport.worldToScreen(new Vec3(b.max.x, b.center.y, 0));
      ctx.beginPath();
      ctx.moveTo(from.x, from.y);
      ctx.lineTo(to.x, to.y);
      ctx.stroke();
      return;
    }

    const anchorWorld = transform.transformPoint(
      entity instanceof TextEntity ? entity.effectivePosition : entity.position,
    );
    const anchor = viewport.worldToScreen(anchorWorld);
    const rotation = entity.rotation + transform.rotation2 - viewport.rotation;

    ctx.save();
    ctx.translate(anchor.x, anchor.y);
    // Negated because screen Y is inverted relative to world Y.
    ctx.rotate(-rotation);

    ctx.fillStyle = asSelection
      ? toCss(options.theme.selection)
      : ctx.strokeStyle as string;
    ctx.font = style?.toCssFont(heightPixels) ?? `${heightPixels}px sans-serif`;
    ctx.setLineDash([]);

    if (entity instanceof TextEntity) {
      ctx.textAlign = horizontalAlignToCanvas(entity.horizontalAlignment);
      ctx.textBaseline = verticalAlignToCanvas(entity.verticalAlignment);
      ctx.fillText(entity.contents, 0, 0);
    } else {
      const lines = entity.lines;
      const lineHeight = heightPixels * entity.lineSpacingFactor;
      const { align, baseline, offsetY } = mtextAlignment(entity.attachment, lines.length, lineHeight);
      ctx.textAlign = align;
      ctx.textBaseline = baseline;
      for (let i = 0; i < lines.length; i++) {
        ctx.fillText(lines[i]!, 0, offsetY + i * lineHeight);
      }
    }

    ctx.restore();
  }

  /**
   * Draw a placeholder for an unresolved external reference.
   *
   * A missing reference that renders as nothing is invisible to the user, who
   * then has no idea part of the drawing is absent. A marked box at the
   * insertion point makes the problem impossible to miss.
   */
  private drawMissingReference(
    entity: InsertEntity,
    transform: Mat4,
    viewport: Viewport,
    options: RenderOptions,
  ): void {
    const ctx = this.context;
    const world = transform.transformPoint(entity.position);
    const p = viewport.worldToScreen(world);
    const size = 12;

    ctx.save();
    ctx.strokeStyle = toCss(options.theme.missingReference);
    ctx.setLineDash([4, 3]);
    ctx.lineWidth = 1.5;
    ctx.strokeRect(p.x - size, p.y - size, size * 2, size * 2);
    ctx.beginPath();
    ctx.moveTo(p.x - size, p.y - size);
    ctx.lineTo(p.x + size, p.y + size);
    ctx.moveTo(p.x + size, p.y - size);
    ctx.lineTo(p.x - size, p.y + size);
    ctx.stroke();
    ctx.restore();
  }

  /** Draw a paper-space viewport's border. */
  private drawViewportBorder(
    entity: ViewportEntity,
    transform: Mat4,
    viewport: Viewport,
    options: RenderOptions,
  ): void {
    // The overall pseudo-viewport represents the sheet and has no border.
    if (entity.isOverallViewport) return;
    const ctx = this.context;
    ctx.save();
    ctx.setLineDash([]);
    ctx.strokeStyle = toCss(options.theme.crosshair);
    this.drawCurves(entity.toCurves(), transform, viewport, chordToleranceFor(viewport));
    ctx.restore();
  }

  private drawHighlight(
    item: DrawItem,
    viewport: Viewport,
    options: RenderOptions,
    tolerance: number,
  ): void {
    const ctx = this.context;
    ctx.save();
    ctx.strokeStyle = toCss(options.theme.highlight);
    ctx.lineWidth = 3;
    ctx.globalAlpha = 0.6;
    ctx.setLineDash([]);
    this.drawCurves(item.entity.toCurves(), item.transform, viewport, tolerance);
    ctx.restore();
  }

  private drawPreview(
    item: DrawItem,
    viewport: Viewport,
    options: RenderOptions,
    tolerance: number,
  ): void {
    const ctx = this.context;
    ctx.save();
    ctx.strokeStyle = toCss(options.theme.preview);
    ctx.lineWidth = 1;
    ctx.setLineDash([5, 4]);
    ctx.globalAlpha = 0.9;
    this.drawCurves(item.entity.toCurves(), item.transform, viewport, tolerance);
    ctx.restore();
  }

  // ------------------------------------------------------------- appearance

  /**
   * ACI 7 means "whatever contrasts with the background", so it renders white
   * on a dark theme and black on a light one. Substituting it here rather than
   * in the colour model keeps the document independent of the display theme.
   */
  private resolveDisplayColor(rgb: number, theme: RenderTheme): number {
    if (rgb === 0xffffff && theme.background > 0x808080) return theme.foreground;
    if (rgb === 0x000000 && theme.background <= 0x808080) return theme.foreground;
    return rgb;
  }

  /**
   * Line width in device pixels.
   *
   * Lineweights are off by default and render as hairlines, matching other CAD software:
   * true-width display makes a drawing unreadable when zoomed out. When on,
   * the width is converted from hundredths of a millimetre through the view
   * scale, and floored at one pixel so nothing vanishes.
   */
  private lineWidthFor(
    properties: { lineweight: number },
    viewport: Viewport,
    options: RenderOptions,
  ): number {
    if (!options.showLineweights) return 1;
    const millimetres = properties.lineweight / 100;
    // Lineweight is a plotted width, so it is a paper measurement rather than
    // a drawing one; it is displayed at an approximate on-screen size.
    const pixels = millimetres * 3.78;
    return Math.max(1, pixels);
  }

  /** Dash pattern for a resolved linetype, scaled to the view. */
  private dashArrayFor(
    properties: { linetypeHandle: Handle; linetypeScale: number },
    viewport: Viewport,
  ): number[] | null {
    const key = `${properties.linetypeHandle}:${properties.linetypeScale}:${viewport.scale}`;
    const cached = this.dashCache.get(key);
    if (cached !== undefined) return cached;

    const linetype = this.db.getTyped(properties.linetypeHandle, Linetype);
    let dashes: number[] | null = null;

    if (linetype !== undefined && !linetype.isContinuous) {
      const worldDashes = linetype.toDashArray(properties.linetypeScale);
      if (worldDashes !== null) {
        const pixels = worldDashes.map((d) => viewport.worldToPixels(d));
        // A pattern finer than a couple of pixels reads as a solid grey line
        // and costs more to rasterize than a solid one.
        const total = pixels.reduce((sum, d) => sum + d, 0);
        dashes = total >= 3 ? pixels : null;
      }
    }

    // Bounded so a long session cannot grow the cache without limit.
    if (this.dashCache.size > 512) this.dashCache.clear();
    this.dashCache.set(key, dashes);
    return dashes;
  }

  /** Drop cached appearance data after a database change. */
  invalidate(): void {
    this.dashCache.clear();
  }
}

// ---------------------------------------------------------------- utilities

export function toCss(rgb: number): string {
  return `#${(rgb & 0xffffff).toString(16).padStart(6, '0')}`;
}

/**
 * Round a world step to the nearest 1, 2 or 5 times a power of ten.
 *
 * Grid and ruler values must be round numbers at every zoom level; without
 * this the grid would land on arbitrary values and be useless for reading
 * positions.
 */
export function niceStep(target: number, minimum = 0): number {
  const base = Math.max(target, minimum > 0 ? minimum : target);
  if (!(base > 0)) return 1;
  const magnitude = Math.pow(10, Math.floor(Math.log10(base)));
  const normalized = base / magnitude;
  const stepped = normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10;
  return stepped * magnitude;
}

/** Every tenth grid line is major. */
function isMajorLine(value: number, step: number): boolean {
  const index = Math.round(value / step);
  return index % 10 === 0;
}

/**
 * Offset a coordinate to a half-pixel.
 *
 * A one-pixel line drawn on an integer coordinate straddles two pixel rows and
 * renders as a blurry two-pixel line. Half-pixel alignment is what makes the
 * grid and hairlines crisp.
 */
function snapHalf(value: number): number {
  return Math.round(value) + 0.5;
}

function horizontalAlignToCanvas(align: TextHorizontalAlignment): CanvasTextAlign {
  switch (align) {
    case TextHorizontalAlignment.Center:
    case TextHorizontalAlignment.Middle:
      return 'center';
    case TextHorizontalAlignment.Right:
      return 'right';
    default:
      return 'left';
  }
}

function verticalAlignToCanvas(align: TextVerticalAlignment): CanvasTextBaseline {
  switch (align) {
    case TextVerticalAlignment.Top:
      return 'top';
    case TextVerticalAlignment.Middle:
      return 'middle';
    case TextVerticalAlignment.Bottom:
      return 'bottom';
    default:
      return 'alphabetic';
  }
}

/** Canvas alignment and first-line offset for an MTEXT attachment point. */
function mtextAlignment(
  attachment: number,
  lineCount: number,
  lineHeight: number,
): { align: CanvasTextAlign; baseline: CanvasTextBaseline; offsetY: number } {
  const column = (attachment - 1) % 3;
  const row = Math.floor((attachment - 1) / 3);

  const align: CanvasTextAlign = column === 0 ? 'left' : column === 1 ? 'center' : 'right';
  const baseline: CanvasTextBaseline = 'top';

  // Row 0 is top-attached, 1 middle, 2 bottom. Screen Y grows downward, so a
  // bottom attachment shifts the first line up by the full block height.
  const totalHeight = lineCount * lineHeight;
  const offsetY = row === 0 ? 0 : row === 1 ? -totalHeight / 2 : -totalHeight;
  return { align, baseline, offsetY };
}

/** Curve types re-exported so callers need only this module. */
export type { DrawItem };
export { ArcEntity, CircleEntity, EllipseEntity, LineEntity, PolylineEntity };
