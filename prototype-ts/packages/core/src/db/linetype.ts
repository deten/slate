import { SymbolTableRecord } from './symbol-table.js';
import type { DbObject } from './db-object.js';
import { NULL_HANDLE, type Handle } from './handle.js';
import { Vec3 } from '../math/vec3.js';

/** The three linetypes every drawing contains. */
export const LINETYPE_CONTINUOUS = 'Continuous';
export const LINETYPE_BY_LAYER = 'ByLayer';
export const LINETYPE_BY_BLOCK = 'ByBlock';

/**
 * One element of a linetype pattern.
 *
 * A plain dash pattern uses only {@link length}. Complex linetypes — the ones
 * that draw text or shapes along the line, such as a gas line marked "GAS" or
 * a fence line with tick marks — additionally carry an embedded text or shape
 * element. Those are modelled here so that importing a professional drawing
 * does not silently flatten them to plain dashes.
 */
export interface LinetypeElement {
  /**
   * Dash length in drawing units. Positive draws a dash, negative leaves a
   * gap, and zero produces a dot.
   */
  readonly length: number;

  /** Embedded text, for complex linetypes with a shape or string element. */
  readonly text?: string;
  /** Handle of the text style used to draw {@link text}. */
  readonly styleHandle?: Handle;
  /** Shape index within an SHX shape file, when the element is a shape. */
  readonly shapeNumber?: number;
  /** Offset of the embedded element from the line. */
  readonly offset?: Vec3;
  /** Rotation of the embedded element, in radians. */
  readonly rotation?: number;
  /** `true` when the rotation is absolute rather than relative to the line. */
  readonly rotationIsAbsolute?: boolean;
  readonly scale?: number;
}

/**
 * A linetype definition: the repeating dash/gap pattern applied along a curve.
 */
export class Linetype extends SymbolTableRecord {
  /** Human-readable description, e.g. `Dashed __ __ __ __`. */
  description = '';

  /** The repeating pattern. An empty pattern means a solid line. */
  elements: LinetypeElement[] = [];

  override get objectType(): string {
    return 'Linetype';
  }

  override get isReserved(): boolean {
    const upper = this.name.toUpperCase();
    return (
      upper === LINETYPE_CONTINUOUS.toUpperCase() ||
      upper === LINETYPE_BY_LAYER.toUpperCase() ||
      upper === LINETYPE_BY_BLOCK.toUpperCase()
    );
  }

  /** `true` when this linetype draws an unbroken line. */
  get isContinuous(): boolean {
    return this.elements.length === 0;
  }

  /** Total length of one repetition of the pattern. */
  get patternLength(): number {
    let total = 0;
    for (const e of this.elements) total += Math.abs(e.length);
    return total;
  }

  /** `true` when any element embeds text or a shape. */
  get isComplex(): boolean {
    return this.elements.some((e) => e.text !== undefined || e.shapeNumber !== undefined);
  }

  /**
   * The dash pattern as alternating on/off lengths, suitable for a renderer's
   * dash array. Complex elements contribute their length only; their embedded
   * glyphs are drawn separately by the renderer.
   *
   * @param scale combined linetype scale (global x entity x block).
   * @returns alternating draw/skip lengths, or `null` for a solid line.
   */
  toDashArray(scale = 1): number[] | null {
    if (this.isContinuous || scale <= 0) return null;
    const dashes: number[] = [];
    for (const e of this.elements) {
      // A zero-length element is a dot. Rendering it as a true zero-length
      // dash makes it invisible in most rasterizers, so it is given a small
      // positive extent proportional to the pattern.
      const len = e.length === 0 ? Math.max(this.patternLength * 0.01, 1e-6) : Math.abs(e.length);
      dashes.push(len * scale);
    }
    // A dash array must have even length to alternate correctly; an odd
    // pattern is repeated so the on/off phases stay aligned.
    if (dashes.length % 2 !== 0) dashes.push(...dashes);
    return dashes;
  }

  override clone(): Linetype {
    const copy = new Linetype();
    this.copyRecordInto(copy);
    copy.description = this.description;
    copy.elements = this.elements.map((e) => ({ ...e }));
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof Linetype)) {
      throw new TypeError('Snapshot type mismatch restoring a linetype');
    }
    this.restoreRecordFrom(snapshot);
    this.description = snapshot.description;
    this.elements = snapshot.elements.map((e) => ({ ...e }));
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    for (const e of this.elements) {
      if (e.styleHandle !== undefined && e.styleHandle !== NULL_HANDLE) {
        refs.push(e.styleHandle);
      }
    }
    return refs;
  }
}

/**
 * Definitions for the linetypes other CAD software ships in `acad.lin`, so a new drawing
 * is usable without an external resource file. Lengths are in drawing units at
 * a scale of 1.
 */
export const STANDARD_LINETYPES: ReadonlyArray<{
  name: string;
  description: string;
  pattern: readonly number[];
}> = [
  { name: 'Continuous', description: 'Solid line', pattern: [] },
  { name: 'Center', description: 'Center ____ _ ____ _ ____', pattern: [31.75, -6.35, 6.35, -6.35] },
  { name: 'Dashed', description: 'Dashed __ __ __ __ __ __', pattern: [12.7, -6.35] },
  { name: 'Phantom', description: 'Phantom ___ _ _ ___ _ _', pattern: [31.75, -6.35, 6.35, -6.35, 6.35, -6.35] },
  { name: 'Dashdot', description: 'Dash dot __ . __ . __ .', pattern: [12.7, -6.35, 0, -6.35] },
  { name: 'Divide', description: 'Divide __ . . __ . . __', pattern: [12.7, -6.35, 0, -6.35, 0, -6.35] },
  { name: 'Dot', description: 'Dot . . . . . . . . . .', pattern: [0, -6.35] },
  { name: 'Hidden', description: 'Hidden __ __ __ __ __ __', pattern: [6.35, -3.175] },
];
