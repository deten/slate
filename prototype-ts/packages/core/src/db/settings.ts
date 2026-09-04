import { Vec3 } from '../math/vec3.js';

/**
 * Drawing units, matching the DXF `$INSUNITS` enumeration.
 *
 * Units matter for more than display: inserting a block or attaching an xref
 * authored in different units requires an automatic scale factor, and getting
 * that wrong silently produces a drawing that is 25.4 times too large.
 */
export enum DrawingUnits {
  Unitless = 0,
  Inches = 1,
  Feet = 2,
  Miles = 3,
  Millimeters = 4,
  Centimeters = 5,
  Meters = 6,
  Kilometers = 7,
  Microinches = 8,
  Mils = 9,
  Yards = 10,
  Angstroms = 11,
  Nanometers = 12,
  Microns = 13,
  Decimeters = 14,
  Decameters = 15,
  Hectometers = 16,
  Gigameters = 17,
  AstronomicalUnits = 18,
  LightYears = 19,
  Parsecs = 20,
}

/** Metres per one unit, for converting between drawings. */
const UNIT_TO_METERS: Readonly<Record<DrawingUnits, number>> = {
  [DrawingUnits.Unitless]: 1,
  [DrawingUnits.Inches]: 0.0254,
  [DrawingUnits.Feet]: 0.3048,
  [DrawingUnits.Miles]: 1609.344,
  [DrawingUnits.Millimeters]: 0.001,
  [DrawingUnits.Centimeters]: 0.01,
  [DrawingUnits.Meters]: 1,
  [DrawingUnits.Kilometers]: 1000,
  [DrawingUnits.Microinches]: 2.54e-8,
  [DrawingUnits.Mils]: 2.54e-5,
  [DrawingUnits.Yards]: 0.9144,
  [DrawingUnits.Angstroms]: 1e-10,
  [DrawingUnits.Nanometers]: 1e-9,
  [DrawingUnits.Microns]: 1e-6,
  [DrawingUnits.Decimeters]: 0.1,
  [DrawingUnits.Decameters]: 10,
  [DrawingUnits.Hectometers]: 100,
  [DrawingUnits.Gigameters]: 1e9,
  [DrawingUnits.AstronomicalUnits]: 1.495978707e11,
  [DrawingUnits.LightYears]: 9.4607304725808e15,
  [DrawingUnits.Parsecs]: 3.0856775814913673e16,
};

/**
 * Scale factor converting a length in `from` units to `to` units.
 * Returns 1 when either side is unitless, matching other CAD software's behaviour of
 * declining to guess.
 */
export function unitConversionFactor(from: DrawingUnits, to: DrawingUnits): number {
  if (from === DrawingUnits.Unitless || to === DrawingUnits.Unitless) return 1;
  return UNIT_TO_METERS[from] / UNIT_TO_METERS[to];
}

/** How linear values are formatted, matching `$LUNITS`. */
export enum LinearUnitFormat {
  Scientific = 1,
  Decimal = 2,
  Engineering = 3,
  Architectural = 4,
  Fractional = 5,
}

/** How angular values are formatted, matching `$AUNITS`. */
export enum AngularUnitFormat {
  DecimalDegrees = 0,
  DegreesMinutesSeconds = 1,
  Gradians = 2,
  Radians = 3,
  Surveyor = 4,
}

/**
 * Header variables and drafting settings for a drawing.
 *
 * These correspond to other CAD software system variables. They are stored on the
 * database rather than in application preferences because they belong to the
 * *document*: two open drawings can legitimately have different units, snap
 * settings and linetype scales.
 */
export interface DrawingSettings {
  // --- Units and formatting
  insertionUnits: DrawingUnits;
  linearFormat: LinearUnitFormat;
  /** Decimal places shown for linear values. */
  linearPrecision: number;
  angularFormat: AngularUnitFormat;
  angularPrecision: number;
  /** Angle considered zero, in radians. Usually east. */
  angleBase: number;
  /** `true` when angles increase clockwise. */
  angleClockwise: boolean;

  // --- Display
  /** Global linetype scale (LTSCALE). */
  globalLinetypeScale: number;
  /** Linetype scale applied in paper space (PSLTSCALE). */
  paperSpaceLinetypeScale: number;
  /** Point display style (PDMODE) and size (PDSIZE). */
  pointDisplayMode: number;
  pointDisplaySize: number;
  /** Whether lineweights are shown on screen (LWDISPLAY). */
  displayLineweights: boolean;
  /** Default lineweight in hundredths of a millimetre (LWDEFAULT). */
  defaultLineweight: number;

  // --- Drawing limits and base
  limitsMin: Vec3;
  limitsMax: Vec3;
  insertionBase: Vec3;

  // --- Drafting aids
  gridSpacing: Vec3;
  gridEnabled: boolean;
  snapSpacing: Vec3;
  snapEnabled: boolean;
  /** Rotation of the snap grid, in radians. */
  snapAngle: number;
  snapBase: Vec3;
  orthoEnabled: boolean;
  /** Polar tracking increment, in radians. Zero disables polar tracking. */
  polarAngleIncrement: number;
  polarTrackingEnabled: boolean;

  // --- Object snap
  /** Bitmask of enabled running object snaps (OSMODE). */
  osnapModes: number;
  osnapEnabled: boolean;
  /** Pick aperture, in pixels. */
  apertureSize: number;
  /** Selection pick box, in pixels. */
  pickboxSize: number;

  // --- Current defaults for new entities
  currentLayerName: string;
  currentLinetypeName: string;
  currentTextStyleName: string;
  currentDimStyleName: string;

  /** Header variables read from file that this application does not model. */
  unknownHeaderVariables: Map<string, unknown>;
}

export function createDefaultSettings(): DrawingSettings {
  return {
    insertionUnits: DrawingUnits.Millimeters,
    linearFormat: LinearUnitFormat.Decimal,
    linearPrecision: 4,
    angularFormat: AngularUnitFormat.DecimalDegrees,
    angularPrecision: 2,
    angleBase: 0,
    angleClockwise: false,

    globalLinetypeScale: 1,
    paperSpaceLinetypeScale: 1,
    pointDisplayMode: 0,
    pointDisplaySize: 0,
    displayLineweights: false,
    defaultLineweight: 25,

    limitsMin: Vec3.ZERO,
    limitsMax: new Vec3(420, 297, 0),
    insertionBase: Vec3.ZERO,

    gridSpacing: new Vec3(10, 10, 0),
    gridEnabled: true,
    snapSpacing: new Vec3(10, 10, 0),
    snapEnabled: false,
    snapAngle: 0,
    snapBase: Vec3.ZERO,
    orthoEnabled: false,
    polarAngleIncrement: Math.PI / 4,
    polarTrackingEnabled: false,

    // Endpoint | Midpoint | Center | Node | Intersection by default, matching
    // a sensible professional starting configuration.
    osnapModes: 0b0000_0010_1111,
    osnapEnabled: true,
    apertureSize: 10,
    pickboxSize: 5,

    currentLayerName: '0',
    currentLinetypeName: 'ByLayer',
    currentTextStyleName: 'Standard',
    currentDimStyleName: 'Standard',

    unknownHeaderVariables: new Map(),
  };
}

export type { DrawingSettings as Settings };
