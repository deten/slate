/**
 * Colour model.
 *
 * DWG colour is not a single RGB value. An entity's colour may be:
 *  - an index into the 256-entry other CAD software Colour Index (ACI) palette,
 *  - a 24-bit true colour,
 *  - or one of the two *inherited* pseudo-colours, ByLayer and ByBlock.
 *
 * Modelling inheritance explicitly — rather than resolving to RGB at load
 * time — is essential: changing a layer's colour must visibly change every
 * ByLayer entity on it, and a block inserted on a different layer must
 * re-resolve its ByBlock entities. Flattening to RGB early destroys both.
 */

export enum ColorMethod {
  ByLayer = 'byLayer',
  ByBlock = 'byBlock',
  Index = 'index',
  TrueColor = 'trueColor',
}

/** ACI index reserved for ByBlock. */
export const ACI_BY_BLOCK = 0;
/** ACI index reserved for ByLayer. */
export const ACI_BY_LAYER = 256;
/** ACI index meaning "no colour"; used by layers that are turned off. */
export const ACI_NONE = 257;

/**
 * An immutable colour value.
 */
export class Color {
  static readonly BY_LAYER = new Color(ColorMethod.ByLayer, 0);
  static readonly BY_BLOCK = new Color(ColorMethod.ByBlock, 0);

  readonly method: ColorMethod;
  /** ACI index when `method` is {@link ColorMethod.Index}. */
  readonly index: number;
  /** Packed 0xRRGGBB when `method` is {@link ColorMethod.TrueColor}. */
  readonly rgb: number;
  /** Optional colour-book name, preserved verbatim for round-tripping. */
  readonly bookName: string | undefined;
  readonly colorName: string | undefined;

  private constructor(
    method: ColorMethod,
    index: number,
    rgb = 0,
    bookName?: string,
    colorName?: string,
  ) {
    this.method = method;
    this.index = index;
    this.rgb = rgb;
    this.bookName = bookName;
    this.colorName = colorName;
    Object.freeze(this);
  }

  /** Colour from an ACI index, mapping the reserved indices to their meaning. */
  static fromIndex(index: number): Color {
    if (index === ACI_BY_BLOCK) return Color.BY_BLOCK;
    if (index === ACI_BY_LAYER) return Color.BY_LAYER;
    const clamped = Math.max(0, Math.min(257, Math.round(index)));
    return new Color(ColorMethod.Index, clamped);
  }

  static fromTrueColor(rgb: number, bookName?: string, colorName?: string): Color {
    return new Color(ColorMethod.TrueColor, 0, rgb & 0xffffff, bookName, colorName);
  }

  static fromRgb(r: number, g: number, b: number): Color {
    return Color.fromTrueColor(((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff));
  }

  get isByLayer(): boolean {
    return this.method === ColorMethod.ByLayer;
  }

  get isByBlock(): boolean {
    return this.method === ColorMethod.ByBlock;
  }

  /** `true` when this colour needs no context to resolve to RGB. */
  get isConcrete(): boolean {
    return this.method === ColorMethod.Index || this.method === ColorMethod.TrueColor;
  }

  /**
   * Resolve to a packed 0xRRGGBB value.
   * Only meaningful for concrete colours; inherited colours must be resolved
   * against their layer or block first (see `resolveColor` in `entity.ts`).
   */
  toRgb(): number {
    if (this.method === ColorMethod.TrueColor) return this.rgb;
    if (this.method === ColorMethod.Index) return aciToRgb(this.index);
    // An unresolved inherited colour renders as the ACI 7 default rather than
    // throwing: a drawing must always be displayable.
    return aciToRgb(7);
  }

  toCssHex(): string {
    return `#${this.toRgb().toString(16).padStart(6, '0')}`;
  }

  equals(other: Color): boolean {
    return (
      this.method === other.method && this.index === other.index && this.rgb === other.rgb
    );
  }

  toString(): string {
    switch (this.method) {
      case ColorMethod.ByLayer:
        return 'ByLayer';
      case ColorMethod.ByBlock:
        return 'ByBlock';
      case ColorMethod.Index:
        return `ACI ${this.index}`;
      case ColorMethod.TrueColor:
        return this.colorName ?? this.toCssHex();
    }
  }
}

/**
 * The standard other CAD software Colour Index palette.
 *
 * Indices 1-9 are the named primaries; 10-249 follow other CAD software's hue/saturation
 * ramp; 250-255 are the greyscale ramp. The ramp is generated rather than
 * table-stamped so the values stay auditable, and it matches other CAD software's
 * published palette.
 */
const ACI_PRIMARIES: readonly number[] = [
  0x000000, // 0 - ByBlock placeholder
  0xff0000, // 1 red
  0xffff00, // 2 yellow
  0x00ff00, // 3 green
  0x00ffff, // 4 cyan
  0x0000ff, // 5 blue
  0xff00ff, // 6 magenta
  0xffffff, // 7 white/black (rendered against the background)
  0x808080, // 8 dark grey
  0xc0c0c0, // 9 light grey
];

/** Hue sequence for the 10-249 ramp, in degrees. */
const RAMP_HUES = [
  0, 15, 30, 45, 60, 75, 90, 105, 120, 135, 150, 165, 180, 195, 210, 225, 240, 255, 270,
  285, 300, 315, 330, 345,
];

/** Value/saturation pairs for the five shades within each hue. */
const RAMP_SHADES: ReadonlyArray<readonly [number, number]> = [
  [1.0, 1.0],
  [1.0, 0.5],
  [0.76, 1.0],
  [0.76, 0.5],
  [0.5, 1.0],
];

let aciPalette: Int32Array | null = null;

function buildPalette(): Int32Array {
  const palette = new Int32Array(258);
  for (let i = 0; i < ACI_PRIMARIES.length; i++) {
    palette[i] = ACI_PRIMARIES[i]!;
  }

  // 10-249: 24 hues x 10 entries. Each hue block alternates a bright and a
  // half-luminance variant across five shade steps.
  let index = 10;
  for (const hue of RAMP_HUES) {
    for (const [value, sat] of RAMP_SHADES) {
      if (index > 249) break;
      palette[index++] = hsvToRgb(hue, sat, value);
      if (index > 249) break;
      palette[index++] = hsvToRgb(hue, sat, value * 0.55);
    }
  }

  // 250-255: greyscale ramp from near-black to near-white.
  const greys = [0x333333, 0x505050, 0x696969, 0x828282, 0xbebebe, 0xffffff];
  for (let i = 0; i < greys.length; i++) {
    palette[250 + i] = greys[i]!;
  }
  palette[256] = 0xffffff; // ByLayer placeholder
  palette[257] = 0x000000; // None
  return palette;
}

/** RGB for an ACI index. Out-of-range indices fall back to white. */
export function aciToRgb(index: number): number {
  aciPalette ??= buildPalette();
  if (index < 0 || index > 257) return 0xffffff;
  return aciPalette[index]!;
}

/** Nearest ACI index to a true colour, for export to index-only contexts. */
export function rgbToAci(rgb: number): number {
  aciPalette ??= buildPalette();
  const r = (rgb >> 16) & 0xff;
  const g = (rgb >> 8) & 0xff;
  const b = rgb & 0xff;
  let best = 7;
  let bestDist = Infinity;
  // Skip the reserved 0, 256 and 257 slots.
  for (let i = 1; i <= 255; i++) {
    const c = aciPalette[i]!;
    const dr = ((c >> 16) & 0xff) - r;
    const dg = ((c >> 8) & 0xff) - g;
    const db = (c & 0xff) - b;
    const dist = dr * dr + dg * dg + db * db;
    if (dist < bestDist) {
      bestDist = dist;
      best = i;
    }
  }
  return best;
}

function hsvToRgb(h: number, s: number, v: number): number {
  const c = v * s;
  const hp = (((h % 360) + 360) % 360) / 60;
  const x = c * (1 - Math.abs((hp % 2) - 1));
  let r = 0;
  let g = 0;
  let b = 0;
  if (hp < 1) [r, g, b] = [c, x, 0];
  else if (hp < 2) [r, g, b] = [x, c, 0];
  else if (hp < 3) [r, g, b] = [0, c, x];
  else if (hp < 4) [r, g, b] = [0, x, c];
  else if (hp < 5) [r, g, b] = [x, 0, c];
  else [r, g, b] = [c, 0, x];
  const m = v - c;
  const to8 = (n: number): number => Math.round((n + m) * 255) & 0xff;
  return (to8(r) << 16) | (to8(g) << 8) | to8(b);
}

/**
 * Lineweight, in hundredths of a millimetre — the DWG unit.
 *
 * The negative values are sentinels with the same inheritance role that
 * ByLayer/ByBlock play for colour.
 */
export enum Lineweight {
  ByLayer = -1,
  ByBlock = -2,
  Default = -3,
}

/** The fixed set of lineweights other CAD software permits, in hundredths of a mm. */
export const STANDARD_LINEWEIGHTS: readonly number[] = [
  0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158,
  200, 211,
];

/** Snap an arbitrary width to the nearest permitted lineweight. */
export function normalizeLineweight(value: number): number {
  if (value < 0) {
    return value === Lineweight.ByBlock || value === Lineweight.Default
      ? value
      : Lineweight.ByLayer;
  }
  let best = STANDARD_LINEWEIGHTS[0]!;
  let bestDist = Infinity;
  for (const w of STANDARD_LINEWEIGHTS) {
    const dist = Math.abs(w - value);
    if (dist < bestDist) {
      bestDist = dist;
      best = w;
    }
  }
  return best;
}

/**
 * Transparency, stored as DWG's 0-255 alpha where 255 is fully opaque.
 * `null` means ByLayer.
 */
export type Transparency = number | null;

export const TRANSPARENCY_OPAQUE = 255;
