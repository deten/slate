import { SymbolTableRecord } from './symbol-table.js';
import type { DbObject } from './db-object.js';

export const TEXT_STYLE_STANDARD = 'Standard';

/**
 * A text style: the named font-and-formatting record that TEXT, MTEXT,
 * dimensions and attributes reference.
 *
 * DWG text styles may name either an SHX shape font (other CAD software's own vector
 * format) or a TrueType font. Both are recorded; which one the renderer can
 * actually draw is a separate concern handled by font resolution, and a style
 * naming a font we cannot load still keeps its metrics so the layout stays
 * correct.
 */
export class TextStyle extends SymbolTableRecord {
  /**
   * Primary font file, e.g. `txt.shx` or `arial.ttf`. Kept as written in the
   * file so that resource resolution can search for it.
   */
  fontFile = 'txt.shx';

  /** Secondary font used for Asian glyphs ("big font"). */
  bigFontFile = '';

  /** TrueType family name, when the style names a system font. */
  typeface = '';

  /**
   * Fixed text height. Zero means the height is supplied per text object,
   * which is the common case and why it cannot be defaulted to something
   * non-zero.
   */
  height = 0;

  /** Width scale factor applied to each glyph. */
  widthFactor = 1;

  /** Slant angle in radians; positive leans right. */
  obliqueAngle = 0;

  /** Text is drawn mirrored across its baseline. */
  isBackwards = false;

  /** Text is drawn upside down. */
  isUpsideDown = false;

  /** Glyphs are rotated 90 degrees and stacked vertically. */
  isVertical = false;

  /** TrueType weight, 400 for regular and 700 for bold. */
  fontWeight = 400;

  /** `true` when the TrueType face is italic. */
  isItalic = false;

  /**
   * `true` for styles that exist only to hold dimension text formatting and
   * are not offered in the style picker.
   */
  isShapeFile = false;

  override get objectType(): string {
    return 'TextStyle';
  }

  override get isReserved(): boolean {
    return this.name.toUpperCase() === TEXT_STYLE_STANDARD.toUpperCase();
  }

  /** `true` when text objects must supply their own height. */
  get isHeightVariable(): boolean {
    return this.height === 0;
  }

  /** `true` when the style names a TrueType rather than an SHX font. */
  get isTrueType(): boolean {
    return this.typeface !== '' || /\.ttf$/i.test(this.fontFile);
  }

  /**
   * A CSS font shorthand approximating this style, for the Canvas renderer.
   *
   * SHX fonts have no web equivalent, so they fall back to a monospace face
   * with similar metrics. This is an acknowledged approximation: true SHX
   * rendering requires parsing the shape file and is deferred, but the
   * fallback keeps text legible and correctly positioned in the meantime.
   */
  toCssFont(height: number): string {
    const size = height > 0 ? height : this.height > 0 ? this.height : 1;
    const style = this.isItalic || this.obliqueAngle !== 0 ? 'italic ' : '';
    const weight = this.fontWeight >= 700 ? 'bold ' : '';
    const family = this.isTrueType
      ? `${JSON.stringify(this.typeface || this.fontFile.replace(/\.[^.]+$/, ''))}, sans-serif`
      : 'monospace';
    return `${style}${weight}${size}px ${family}`;
  }

  override clone(): TextStyle {
    const copy = new TextStyle();
    this.copyRecordInto(copy);
    copy.fontFile = this.fontFile;
    copy.bigFontFile = this.bigFontFile;
    copy.typeface = this.typeface;
    copy.height = this.height;
    copy.widthFactor = this.widthFactor;
    copy.obliqueAngle = this.obliqueAngle;
    copy.isBackwards = this.isBackwards;
    copy.isUpsideDown = this.isUpsideDown;
    copy.isVertical = this.isVertical;
    copy.fontWeight = this.fontWeight;
    copy.isItalic = this.isItalic;
    copy.isShapeFile = this.isShapeFile;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof TextStyle)) {
      throw new TypeError('Snapshot type mismatch restoring a text style');
    }
    this.restoreRecordFrom(snapshot);
    this.fontFile = snapshot.fontFile;
    this.bigFontFile = snapshot.bigFontFile;
    this.typeface = snapshot.typeface;
    this.height = snapshot.height;
    this.widthFactor = snapshot.widthFactor;
    this.obliqueAngle = snapshot.obliqueAngle;
    this.isBackwards = snapshot.isBackwards;
    this.isUpsideDown = snapshot.isUpsideDown;
    this.isVertical = snapshot.isVertical;
    this.fontWeight = snapshot.fontWeight;
    this.isItalic = snapshot.isItalic;
    this.isShapeFile = snapshot.isShapeFile;
  }
}
