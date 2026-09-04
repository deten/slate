import { Entity, type GripEditable, type GripPoint } from '../entity.js';
import type { DbObject } from '../db-object.js';
import { Vec3 } from '../../math/vec3.js';
import { Mat4 } from '../../math/mat4.js';
import { Box3 } from '../../math/box3.js';
import type { Curve } from '../../geom/curve.js';
import { NULL_HANDLE, type Handle } from '../handle.js';

/** Horizontal justification of a single-line TEXT entity. */
export enum TextHorizontalAlignment {
  Left = 0,
  Center = 1,
  Right = 2,
  Aligned = 3,
  Middle = 4,
  Fit = 5,
}

/** Vertical justification of a single-line TEXT entity. */
export enum TextVerticalAlignment {
  Baseline = 0,
  Bottom = 1,
  Middle = 2,
  Top = 3,
}

/**
 * A single-line text entity — TEXT.
 *
 * Text bounds cannot be computed exactly without measuring the font, which the
 * core has no access to (it must stay platform-free). The entity therefore
 * exposes a *metrics provider* hook: the renderer installs a real measurement
 * function, and until then a proportional estimate is used. This keeps text
 * selectable and roughly correct in a headless context while being exact in
 * the application.
 */
export class TextEntity extends Entity implements GripEditable {
  /** Insertion point. Its meaning depends on the alignment settings. */
  position: Vec3 = Vec3.ZERO;

  /**
   * Secondary alignment point, used when the text is centred, aligned or fit.
   * DWG stores both and which is authoritative depends on the alignment.
   */
  alignmentPoint: Vec3 | null = null;

  contents = '';
  height = 2.5;
  /** Rotation about the normal, in radians. */
  rotation = 0;
  widthFactor = 1;
  obliqueAngle = 0;
  styleHandle: Handle = NULL_HANDLE;
  horizontalAlignment: TextHorizontalAlignment = TextHorizontalAlignment.Left;
  verticalAlignment: TextVerticalAlignment = TextVerticalAlignment.Baseline;
  isBackwards = false;
  isUpsideDown = false;
  thickness = 0;

  static create(position: Vec3, contents: string, height: number): TextEntity {
    const e = new TextEntity();
    e.position = position;
    e.contents = contents;
    e.height = height;
    return e;
  }

  override get objectType(): string {
    return 'Text';
  }

  /**
   * The point the text actually hangs from. When the alignment is anything but
   * left/baseline, DWG uses the alignment point instead of the insertion point
   * — a detail that misplaces text throughout a drawing if ignored.
   */
  get effectivePosition(): Vec3 {
    const usesAlignmentPoint =
      this.horizontalAlignment !== TextHorizontalAlignment.Left ||
      this.verticalAlignment !== TextVerticalAlignment.Baseline;
    return usesAlignmentPoint && this.alignmentPoint !== null
      ? this.alignmentPoint
      : this.position;
  }

  /** Estimated advance width of the string, in drawing units. */
  get estimatedWidth(): number {
    return measureText(this.contents, this.height, this.widthFactor);
  }

  override get bounds(): Box3 {
    const width = this.estimatedWidth;
    const anchor = this.effectivePosition;

    // Local-space box relative to the anchor, before rotation.
    let left = 0;
    switch (this.horizontalAlignment) {
      case TextHorizontalAlignment.Center:
      case TextHorizontalAlignment.Middle:
        left = -width / 2;
        break;
      case TextHorizontalAlignment.Right:
        left = -width;
        break;
      default:
        left = 0;
    }
    let bottom = 0;
    switch (this.verticalAlignment) {
      case TextVerticalAlignment.Top:
        bottom = -this.height;
        break;
      case TextVerticalAlignment.Middle:
        bottom = -this.height / 2;
        break;
      case TextVerticalAlignment.Bottom:
        bottom = 0;
        break;
      default:
        // Baseline: descenders fall below the insertion point.
        bottom = -this.height * 0.25;
    }

    const local = new Box3(
      new Vec3(left, bottom, 0),
      new Vec3(left + width, bottom + this.height, 0),
    );
    return local.transform(Mat4.rotationZ(this.rotation)).transform(Mat4.translation(anchor));
  }

  override toCurves(): Curve[] {
    // Text is not curve geometry. Returning nothing is correct and keeps trim,
    // fillet and the curve-based snaps from doing something nonsensical to it.
    return [];
  }

  /** Text is picked by its bounding box rather than by an outline. */
  override distanceToPoint(p: Vec3): number {
    const b = this.bounds;
    return b.containsPoint(p) ? 0 : b.distanceToPoint(p);
  }

  override transformBy(m: Mat4): void {
    this.position = m.transformPoint(this.position);
    if (this.alignmentPoint !== null) {
      this.alignmentPoint = m.transformPoint(this.alignmentPoint);
    }
    // Height and rotation follow the transform's scale and rotation. Text is
    // never sheared by other CAD software, so the conformal components are sufficient.
    this.height *= m.scaleFactor2;
    this.rotation += m.rotation2;
    if (m.isMirrored) this.isBackwards = !this.isBackwards;
  }

  getGripPoints(): GripPoint[] {
    return [{ position: this.effectivePosition, kind: 'insertion', index: 0 }];
  }

  moveGrip(_grip: GripPoint, to: Vec3): void {
    const delta = to.sub(this.effectivePosition);
    this.position = this.position.add(delta);
    if (this.alignmentPoint !== null) this.alignmentPoint = this.alignmentPoint.add(delta);
  }

  /**
   * Copy the text-specific fields into `target`.
   *
   * Subclasses (ATTDEF and ATTRIB) call this from their own `clone` so the
   * shared state is copied in exactly one place without any of them going back
   * through `clone` — which would recurse.
   */
  protected copyTextInto(target: TextEntity): void {
    this.copyEntityInto(target);
    target.position = this.position;
    target.alignmentPoint = this.alignmentPoint;
    target.contents = this.contents;
    target.height = this.height;
    target.rotation = this.rotation;
    target.widthFactor = this.widthFactor;
    target.obliqueAngle = this.obliqueAngle;
    target.styleHandle = this.styleHandle;
    target.horizontalAlignment = this.horizontalAlignment;
    target.verticalAlignment = this.verticalAlignment;
    target.isBackwards = this.isBackwards;
    target.isUpsideDown = this.isUpsideDown;
    target.thickness = this.thickness;
  }

  protected restoreTextFrom(snapshot: TextEntity): void {
    this.restoreEntityFrom(snapshot);
    this.position = snapshot.position;
    this.alignmentPoint = snapshot.alignmentPoint;
    this.contents = snapshot.contents;
    this.height = snapshot.height;
    this.rotation = snapshot.rotation;
    this.widthFactor = snapshot.widthFactor;
    this.obliqueAngle = snapshot.obliqueAngle;
    this.styleHandle = snapshot.styleHandle;
    this.horizontalAlignment = snapshot.horizontalAlignment;
    this.verticalAlignment = snapshot.verticalAlignment;
    this.isBackwards = snapshot.isBackwards;
    this.isUpsideDown = snapshot.isUpsideDown;
    this.thickness = snapshot.thickness;
  }

  override clone(): TextEntity {
    const copy = new TextEntity();
    this.copyTextInto(copy);
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof TextEntity)) {
      throw new TypeError('Snapshot type mismatch restoring text');
    }
    this.restoreTextFrom(snapshot);
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    if (this.styleHandle !== NULL_HANDLE) refs.push(this.styleHandle);
    return refs;
  }
}

/** Attachment point of an MTEXT block, matching DXF group code 71. */
export enum MTextAttachment {
  TopLeft = 1,
  TopCenter = 2,
  TopRight = 3,
  MiddleLeft = 4,
  MiddleCenter = 5,
  MiddleRight = 6,
  BottomLeft = 7,
  BottomCenter = 8,
  BottomRight = 9,
}

/**
 * Multi-line formatted text — MTEXT.
 *
 * The contents carry other CAD software's inline formatting codes (`\P` for a paragraph
 * break, `{\fArial|b1;...}` for font runs, and so on). Those codes are
 * preserved verbatim rather than parsed into a rich model: a lossy parse would
 * destroy formatting we do not yet render, and {@link plainText} provides the
 * stripped form wherever a plain string is needed.
 */
export class MTextEntity extends Entity implements GripEditable {
  position: Vec3 = Vec3.ZERO;
  /** Raw contents including inline formatting codes. */
  contents = '';
  height = 2.5;
  /** Column width; 0 means the text does not wrap. */
  width = 0;
  rotation = 0;
  attachment: MTextAttachment = MTextAttachment.TopLeft;
  styleHandle: Handle = NULL_HANDLE;
  /** Line spacing multiplier. */
  lineSpacingFactor = 1;
  /** Direction vector, an alternative to `rotation` in DXF. */
  directionVector: Vec3 | null = null;

  static create(position: Vec3, contents: string, height: number): MTextEntity {
    const e = new MTextEntity();
    e.position = position;
    e.contents = contents;
    e.height = height;
    return e;
  }

  override get objectType(): string {
    return 'MText';
  }

  /** Contents with other CAD software's inline formatting codes removed. */
  get plainText(): string {
    return stripMTextFormatting(this.contents);
  }

  get lines(): string[] {
    return this.plainText.split('\n');
  }

  override get bounds(): Box3 {
    const lines = this.lines;
    const lineHeight = this.height * this.lineSpacingFactor;
    const totalHeight = Math.max(lines.length, 1) * lineHeight;
    const widest =
      this.width > 0
        ? this.width
        : Math.max(
            0,
            ...lines.map((line) => measureText(line, this.height, 1)),
          );

    // Offset from the attachment point to the box's lower-left corner.
    const col = (this.attachment - 1) % 3;
    const row = Math.floor((this.attachment - 1) / 3);
    const left = col === 0 ? 0 : col === 1 ? -widest / 2 : -widest;
    const bottom = row === 0 ? -totalHeight : row === 1 ? -totalHeight / 2 : 0;

    const local = new Box3(
      new Vec3(left, bottom, 0),
      new Vec3(left + widest, bottom + totalHeight, 0),
    );
    return local
      .transform(Mat4.rotationZ(this.rotation))
      .transform(Mat4.translation(this.position));
  }

  override toCurves(): Curve[] {
    return [];
  }

  override distanceToPoint(p: Vec3): number {
    const b = this.bounds;
    return b.containsPoint(p) ? 0 : b.distanceToPoint(p);
  }

  override transformBy(m: Mat4): void {
    this.position = m.transformPoint(this.position);
    const scale = m.scaleFactor2;
    this.height *= scale;
    this.width *= scale;
    this.rotation += m.rotation2;
    if (this.directionVector !== null) {
      this.directionVector = m.transformVector(this.directionVector);
    }
  }

  getGripPoints(): GripPoint[] {
    return [{ position: this.position, kind: 'insertion', index: 0 }];
  }

  moveGrip(_grip: GripPoint, to: Vec3): void {
    this.position = to;
  }

  override clone(): MTextEntity {
    const copy = new MTextEntity();
    this.copyEntityInto(copy);
    copy.position = this.position;
    copy.contents = this.contents;
    copy.height = this.height;
    copy.width = this.width;
    copy.rotation = this.rotation;
    copy.attachment = this.attachment;
    copy.styleHandle = this.styleHandle;
    copy.lineSpacingFactor = this.lineSpacingFactor;
    copy.directionVector = this.directionVector;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof MTextEntity)) {
      throw new TypeError('Snapshot type mismatch restoring mtext');
    }
    this.restoreEntityFrom(snapshot);
    this.position = snapshot.position;
    this.contents = snapshot.contents;
    this.height = snapshot.height;
    this.width = snapshot.width;
    this.rotation = snapshot.rotation;
    this.attachment = snapshot.attachment;
    this.styleHandle = snapshot.styleHandle;
    this.lineSpacingFactor = snapshot.lineSpacingFactor;
    this.directionVector = snapshot.directionVector;
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    if (this.styleHandle !== NULL_HANDLE) refs.push(this.styleHandle);
    return refs;
  }
}

/**
 * Strip other CAD software's MTEXT inline formatting codes, leaving readable text.
 *
 * Handles the codes that appear in practically every real drawing: paragraph
 * breaks, font and colour runs, stacked fractions, and the brace grouping that
 * scopes them. Unrecognized `\x` codes are dropped rather than shown, matching
 * what other CAD software displays.
 */
export function stripMTextFormatting(raw: string): string {
  let out = '';
  for (let i = 0; i < raw.length; i++) {
    const ch = raw[i]!;

    if (ch === '\\') {
      const next = raw[i + 1];
      if (next === undefined) break;

      // Escaped literal brace or backslash.
      if (next === '\\' || next === '{' || next === '}') {
        out += next;
        i++;
        continue;
      }
      // Paragraph break.
      if (next === 'P') {
        out += '\n';
        i++;
        continue;
      }
      // Non-breaking space.
      if (next === '~') {
        out += ' ';
        i++;
        continue;
      }
      // Stacked text: \S numerator ^ denominator ;
      if (next === 'S') {
        const end = raw.indexOf(';', i + 2);
        const body = end < 0 ? raw.slice(i + 2) : raw.slice(i + 2, end);
        out += body.replace(/[\^#]/, '/');
        i = end < 0 ? raw.length : end;
        continue;
      }
      // Codes terminated by a semicolon: font, colour, height, width, tracking.
      if ('fFcCHWTAQpL'.includes(next)) {
        const end = raw.indexOf(';', i + 2);
        i = end < 0 ? raw.length : end;
        continue;
      }
      // Any other escape: drop the code character.
      i++;
      continue;
    }

    // Braces group formatting scopes and are not literal content.
    if (ch === '{' || ch === '}') continue;
    out += ch;
  }
  return out;
}

/**
 * Estimated text advance width.
 *
 * The 0.6 factor is the average glyph aspect ratio of the `txt.shx` setting in other CAD software and
 * of common sans-serif faces. It is a placeholder for real font metrics: the
 * renderer overrides {@link setTextMeasurer} with a canvas-based measurement,
 * and this estimate is used only in headless contexts.
 */
const DEFAULT_ASPECT = 0.6;

type TextMeasurer = (text: string, height: number, widthFactor: number) => number;

let measurer: TextMeasurer = (text, height, widthFactor) =>
  text.length * height * DEFAULT_ASPECT * widthFactor;

/**
 * Install a real font-metrics function. Called once by the rendering layer at
 * start-up so that entity bounds — and therefore selection and zoom extents —
 * match what is actually drawn.
 */
export function setTextMeasurer(fn: TextMeasurer): void {
  measurer = fn;
}

export function measureText(text: string, height: number, widthFactor: number): number {
  return measurer(text, height, widthFactor);
}
