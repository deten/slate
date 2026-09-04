/**
 * DXF group code semantics.
 *
 * A DXF file is a flat stream of (code, value) pairs. The *code* determines
 * both the value's data type and its meaning, and the meaning is contextual —
 * code 10 is "primary point X" on most entities but "corner of the paper" in
 * the header. This module owns the type mapping, which is universal; meaning
 * is resolved by each entity reader.
 */

/** The data type a group code's value carries. */
export enum GroupValueType {
  String,
  Integer,
  Double,
  Boolean,
  /** A hexadecimal object handle. */
  Handle,
  /** Binary chunk, hex-encoded in ASCII DXF. */
  Binary,
}

/**
 * Determine a group code's value type from its numeric range.
 *
 * The ranges come from the DXF reference. Getting this wrong produces subtly
 * corrupt data rather than a parse error — a coordinate read as a string, or
 * an integer truncated from a double — so the ranges are enumerated
 * explicitly rather than approximated.
 */
export function groupValueType(code: number): GroupValueType {
  // Handles are strings of hex, but must not be number-parsed.
  if (code === 5 || code === 105) return GroupValueType.Handle;
  if (code >= 320 && code <= 369) return GroupValueType.Handle;
  if (code >= 390 && code <= 399) return GroupValueType.Handle;
  if (code >= 480 && code <= 481) return GroupValueType.Handle;
  if (code === 1005) return GroupValueType.Handle;

  if (code >= 0 && code <= 9) return GroupValueType.String;
  // 10-59: doubles (coordinates, distances, angles, scale factors).
  if (code >= 10 && code <= 59) return GroupValueType.Double;
  // 60-79: 16-bit integers.
  if (code >= 60 && code <= 79) return GroupValueType.Integer;
  // 90-99: 32-bit integers.
  if (code >= 90 && code <= 99) return GroupValueType.Integer;
  if (code === 100 || code === 102) return GroupValueType.String;
  if (code >= 110 && code <= 149) return GroupValueType.Double;
  if (code >= 160 && code <= 179) return GroupValueType.Integer;
  if (code >= 210 && code <= 239) return GroupValueType.Double;
  if (code >= 270 && code <= 289) return GroupValueType.Integer;
  if (code >= 290 && code <= 299) return GroupValueType.Boolean;
  if (code >= 300 && code <= 309) return GroupValueType.String;
  if (code >= 310 && code <= 319) return GroupValueType.Binary;
  if (code >= 370 && code <= 379) return GroupValueType.Integer;
  if (code >= 380 && code <= 389) return GroupValueType.Integer;
  if (code >= 400 && code <= 409) return GroupValueType.Integer;
  if (code >= 410 && code <= 419) return GroupValueType.String;
  if (code >= 420 && code <= 429) return GroupValueType.Integer;
  if (code >= 430 && code <= 439) return GroupValueType.String;
  if (code >= 440 && code <= 449) return GroupValueType.Integer;
  if (code >= 450 && code <= 459) return GroupValueType.Integer;
  if (code >= 460 && code <= 469) return GroupValueType.Double;
  if (code >= 470 && code <= 479) return GroupValueType.String;
  if (code >= 999 && code <= 1009) return GroupValueType.String;
  if (code >= 1010 && code <= 1059) return GroupValueType.Double;
  if (code >= 1060 && code <= 1070) return GroupValueType.Integer;
  if (code === 1071) return GroupValueType.Integer;

  // Unknown codes are kept as strings so nothing is lost.
  return GroupValueType.String;
}

/** `true` when the code carries application-defined extended data. */
export function isXDataCode(code: number): boolean {
  return code >= 1000 && code <= 1071;
}

/**
 * `true` for the X component of a point triple.
 *
 * DXF encodes points as three consecutive codes: `n`, `n + 10`, `n + 20` for
 * X, Y and Z. Recognizing the X code is what lets the reader reassemble them
 * into a {@link Vec3} rather than tracking three loose numbers.
 */
export function isPointXCode(code: number): boolean {
  return (code >= 10 && code <= 18) || (code >= 110 && code <= 112) || (code >= 210 && code <= 219);
}

/** Y and Z companions of a point X code. */
export function pointYCode(xCode: number): number {
  return xCode + 10;
}

export function pointZCode(xCode: number): number {
  return xCode + 20;
}

/** Common DXF section names. */
export const SECTION = {
  HEADER: 'HEADER',
  CLASSES: 'CLASSES',
  TABLES: 'TABLES',
  BLOCKS: 'BLOCKS',
  ENTITIES: 'ENTITIES',
  OBJECTS: 'OBJECTS',
  THUMBNAILIMAGE: 'THUMBNAILIMAGE',
  ACDSDATA: 'ACDSDATA',
} as const;

/** DXF version identifiers, oldest first, as written in `$ACADVER`. */
export const DXF_VERSIONS: readonly string[] = [
  'AC1006', // R10
  'AC1009', // R11/R12
  'AC1012', // R13
  'AC1014', // R14
  'AC1015', // 2000
  'AC1018', // 2004
  'AC1021', // 2007
  'AC1024', // 2010
  'AC1027', // 2013
  'AC1032', // 2018
];

/** Human-readable release name for a DXF version code. */
export function dxfVersionName(code: string): string {
  const names: Record<string, string> = {
    AC1006: 'R10',
    AC1009: 'R12',
    AC1012: 'R13',
    AC1014: 'R14',
    AC1015: 'other CAD software',
    AC1018: 'other CAD software',
    AC1021: 'other CAD software',
    AC1024: 'other CAD software',
    AC1027: 'other CAD software',
    AC1032: 'other CAD software',
  };
  return names[code] ?? code;
}
