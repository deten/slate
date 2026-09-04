import { Vec3 } from '@cad/core';
import type { XDataItem } from '@cad/core';
import type { DxfGroup } from './tokenizer.js';
import { isXDataCode, pointYCode, pointZCode } from './group-codes.js';

/**
 * The accumulated group pairs describing one DXF object.
 *
 * DXF gives no schema: an object is whatever groups appear between its type
 * marker and the next one. Reading is therefore "pull the codes I understand,
 * and keep the rest". This class provides the pulling; the *keeping* is what
 * makes structure preservation possible, and is why `consumed` is tracked.
 *
 * Repeated codes are common and meaningful — a polyline's vertex coordinates,
 * an MTEXT's continued contents, a hatch's boundary path edges — so every code
 * maps to a list, not a single value.
 */
export class DxfRecord {
  /** DXF type name, e.g. `LINE`, `LWPOLYLINE`, `LAYER`. */
  readonly type: string;

  /** All groups in file order, which some readers depend on. */
  readonly groups: readonly DxfGroup[];

  /** Code -> every value for that code, in order. */
  private readonly byCode = new Map<number, Array<string | number | boolean>>();

  /** Codes a reader has claimed, so the remainder can be preserved. */
  private readonly consumed = new Set<number>();

  /** Extended data, grouped by the registered application name. */
  readonly xdata = new Map<string, XDataItem[]>();

  constructor(type: string, groups: readonly DxfGroup[]) {
    this.type = type;
    this.groups = groups;

    let currentXDataApp: string | null = null;
    for (const group of groups) {
      // Code 1001 opens an XData block belonging to a named application; every
      // XData code after it belongs to that app until the next 1001.
      if (group.code === 1001) {
        currentXDataApp = String(group.value);
        if (!this.xdata.has(currentXDataApp)) this.xdata.set(currentXDataApp, []);
        continue;
      }
      if (isXDataCode(group.code)) {
        if (currentXDataApp !== null) {
          this.xdata.get(currentXDataApp)!.push({
            code: group.code,
            value: group.value as string | number,
          });
        }
        continue;
      }

      const existing = this.byCode.get(group.code);
      if (existing === undefined) {
        this.byCode.set(group.code, [group.value]);
      } else {
        existing.push(group.value);
      }
    }
  }

  has(code: number): boolean {
    return this.byCode.has(code);
  }

  /** First value for a code, marking it consumed. */
  private take(code: number): string | number | boolean | undefined {
    this.consumed.add(code);
    return this.byCode.get(code)?.[0];
  }

  /** Every value for a code, marking it consumed. */
  all(code: number): ReadonlyArray<string | number | boolean> {
    this.consumed.add(code);
    return this.byCode.get(code) ?? [];
  }

  string(code: number, fallback = ''): string {
    const value = this.take(code);
    return value === undefined ? fallback : String(value);
  }

  /** String value, or `undefined` when the code is absent. */
  optionalString(code: number): string | undefined {
    const value = this.take(code);
    return value === undefined ? undefined : String(value);
  }

  number(code: number, fallback = 0): number {
    const value = this.take(code);
    if (typeof value === 'number') return value;
    if (typeof value === 'string') {
      const parsed = Number.parseFloat(value);
      return Number.isFinite(parsed) ? parsed : fallback;
    }
    return fallback;
  }

  optionalNumber(code: number): number | undefined {
    const value = this.take(code);
    if (typeof value === 'number') return value;
    if (typeof value === 'string') {
      const parsed = Number.parseFloat(value);
      return Number.isFinite(parsed) ? parsed : undefined;
    }
    return undefined;
  }

  integer(code: number, fallback = 0): number {
    return Math.trunc(this.number(code, fallback));
  }

  boolean(code: number, fallback = false): boolean {
    const value = this.take(code);
    if (typeof value === 'boolean') return value;
    if (typeof value === 'number') return value !== 0;
    if (typeof value === 'string') return value.trim() !== '0' && value.trim() !== '';
    return fallback;
  }

  /**
   * Read a point from the `code`, `code+10`, `code+20` triple.
   *
   * A missing Z is 0 rather than an error: 2D DXF omits it routinely, and
   * treating that as a failure would reject most real files.
   */
  point(xCode: number, fallback: Vec3 = Vec3.ZERO): Vec3 {
    if (!this.byCode.has(xCode)) {
      this.consumed.add(xCode);
      return fallback;
    }
    const x = this.number(xCode, fallback.x);
    const y = this.number(pointYCode(xCode), fallback.y);
    const z = this.number(pointZCode(xCode), fallback.z);
    return new Vec3(x, y, z);
  }

  optionalPoint(xCode: number): Vec3 | undefined {
    if (!this.byCode.has(xCode)) return undefined;
    return this.point(xCode);
  }

  /**
   * Read every occurrence of a point triple, for entities with repeated
   * coordinates such as LWPOLYLINE vertices.
   *
   * The X, Y and Z lists are zipped positionally. That is correct for DXF's
   * actual layout, where the components of one vertex are adjacent, and it is
   * the only workable approach given that codes are collected per-code.
   */
  pointList(xCode: number): Vec3[] {
    const xs = this.all(xCode);
    const ys = this.all(pointYCode(xCode));
    const zs = this.all(pointZCode(xCode));
    const result: Vec3[] = [];
    for (let i = 0; i < xs.length; i++) {
      result.push(
        new Vec3(
          toNumber(xs[i]) ?? 0,
          toNumber(ys[i]) ?? 0,
          toNumber(zs[i]) ?? 0,
        ),
      );
    }
    return result;
  }

  /**
   * Extrusion direction (code 210), defaulting to world +Z.
   *
   * Normalized here because a file may store an unnormalized vector and every
   * consumer assumes a unit normal.
   */
  extrusion(): Vec3 {
    const n = this.point(210, Vec3.UNIT_Z);
    return n.normalizeOrNull() ?? Vec3.UNIT_Z;
  }

  /**
   * Group codes this reader did not claim.
   *
   * Everything here is written back out on save. Without it, opening and
   * saving a drawing would quietly strip every feature not yet implemented —
   * the single most damaging thing a CAD file reader can do.
   */
  unconsumed(): Array<{ code: number; value: unknown }> {
    const result: Array<{ code: number; value: unknown }> = [];
    for (const [code, values] of this.byCode) {
      if (this.consumed.has(code)) continue;
      // Structural codes are re-derived on write and would be duplicated.
      if (code === 0 || code === 5 || code === 100 || code === 330) continue;
      for (const value of values) result.push({ code, value });
    }
    return result;
  }

  /** Mark codes as consumed without reading them, for codes handled inline. */
  markConsumed(...codes: number[]): void {
    for (const code of codes) this.consumed.add(code);
  }
}

function toNumber(value: string | number | boolean | undefined): number | undefined {
  if (typeof value === 'number') return value;
  if (typeof value === 'string') {
    const parsed = Number.parseFloat(value);
    return Number.isFinite(parsed) ? parsed : undefined;
  }
  return undefined;
}
