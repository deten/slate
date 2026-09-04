import { DbObject } from './db-object.js';
import { NULL_HANDLE, type Handle } from './handle.js';
import { Vec3 } from '../math/vec3.js';
import { Box3 } from '../math/box3.js';

/** Standard plot rotation, matching the DXF enumeration. */
export enum PlotRotation {
  None = 0,
  Ninety = 1,
  OneEighty = 2,
  TwoSeventy = 3,
}

/** How the plot area is chosen. */
export enum PlotType {
  Display = 0,
  Extents = 1,
  Limits = 2,
  View = 3,
  Window = 4,
  LayoutInformation = 5,
}

/**
 * Plot configuration attached to a layout.
 *
 * Stored in full even though plotting is not yet implemented: these settings
 * are authored deliberately by the drawing's owner, and silently resetting a
 * sheet's paper size or plot style on save would be a serious data loss.
 */
export interface PlotSettings {
  /** Named page setup, if any. */
  pageSetupName: string;
  /** Output device name. */
  plotConfigurationName: string;
  /** Paper size name, e.g. `ISO_A1_(841.00_x_594.00_MM)`. */
  paperSizeName: string;
  /** Physical paper size in millimetres. */
  paperWidth: number;
  paperHeight: number;
  /** Unprintable margins in millimetres. */
  marginLeft: number;
  marginBottom: number;
  marginRight: number;
  marginTop: number;
  /** Plot scale as a numerator/denominator pair, e.g. 1:100. */
  scaleNumerator: number;
  scaleDenominator: number;
  /** `true` when the drawing is scaled to fit the paper. */
  scaleToFit: boolean;
  rotation: PlotRotation;
  plotType: PlotType;
  /** Window corners, used when `plotType` is `Window`. */
  windowMin: Vec3;
  windowMax: Vec3;
  /** Origin offset in millimetres. */
  originX: number;
  originY: number;
  /** Name of the plot style table (`.ctb` or `.stb`). */
  plotStyleSheet: string;
  plotCentered: boolean;
  plotHidden: boolean;
  /** `true` when paper-space geometry plots last. */
  plotPaperSpaceLast: boolean;
}

export function defaultPlotSettings(): PlotSettings {
  return {
    pageSetupName: '',
    plotConfigurationName: '',
    paperSizeName: 'ISO_A4_(297.00_x_210.00_MM)',
    paperWidth: 297,
    paperHeight: 210,
    marginLeft: 7.5,
    marginBottom: 20,
    marginRight: 7.5,
    marginTop: 20,
    scaleNumerator: 1,
    scaleDenominator: 1,
    scaleToFit: true,
    rotation: PlotRotation.None,
    plotType: PlotType.Extents,
    windowMin: Vec3.ZERO,
    windowMax: Vec3.ZERO,
    originX: 0,
    originY: 0,
    plotStyleSheet: '',
    plotCentered: true,
    plotHidden: false,
    plotPaperSpaceLast: true,
  };
}

/**
 * A layout — one sheet of the drawing, or model space.
 *
 * A layout pairs a *name* and *plot configuration* with the block record that
 * holds its geometry. Model space is a layout too, which keeps the tab bar,
 * the rendering path and the space-switching logic uniform instead of
 * special-casing model space everywhere.
 *
 * Supporting layouts from the start — rather than bolting paper space on
 * later — is a stated architectural requirement: retrofitting per-viewport
 * layer visibility and viewport scaling into a model-space-only renderer means
 * rewriting it.
 */
export class Layout extends DbObject {
  /** Tab name shown to the user. */
  name = '';

  /** Block record holding this layout's entities. */
  blockTableRecordHandle: Handle = NULL_HANDLE;

  /** Position in the tab order. Model space is conventionally -1. */
  tabOrder = 0;

  plotSettings: PlotSettings = defaultPlotSettings();

  /** Handles of the VIEWPORT entities on this sheet. */
  viewportHandles: Handle[] = [];

  /** Limits of the paper-space area. */
  limitsMin: Vec3 = Vec3.ZERO;
  limitsMax: Vec3 = new Vec3(297, 210, 0);

  /** Extents of the geometry on this layout, cached from the last regen. */
  extentsMin: Vec3 = Vec3.ZERO;
  extentsMax: Vec3 = Vec3.ZERO;

  /** Insertion base point for this layout. */
  insertionBase: Vec3 = Vec3.ZERO;

  /** UCS origin and axes for this layout. */
  ucsOrigin: Vec3 = Vec3.ZERO;
  ucsXAxis: Vec3 = Vec3.UNIT_X;
  ucsYAxis: Vec3 = Vec3.UNIT_Y;

  override get objectType(): string {
    return 'Layout';
  }

  get isModelSpace(): boolean {
    return this.name.toUpperCase() === 'MODEL';
  }

  /** Printable area of the sheet, in millimetres. */
  get printableArea(): Box3 {
    const s = this.plotSettings;
    return new Box3(
      new Vec3(s.marginLeft, s.marginBottom, 0),
      new Vec3(s.paperWidth - s.marginRight, s.paperHeight - s.marginTop, 0),
    );
  }

  /** Effective plot scale as a single multiplier. */
  get plotScale(): number {
    const s = this.plotSettings;
    if (s.scaleToFit || s.scaleDenominator === 0) return 1;
    return s.scaleNumerator / s.scaleDenominator;
  }

  override clone(): Layout {
    const copy = new Layout();
    this.copyBaseInto(copy);
    copy.name = this.name;
    copy.blockTableRecordHandle = this.blockTableRecordHandle;
    copy.tabOrder = this.tabOrder;
    copy.plotSettings = { ...this.plotSettings };
    copy.viewportHandles = [...this.viewportHandles];
    copy.limitsMin = this.limitsMin;
    copy.limitsMax = this.limitsMax;
    copy.extentsMin = this.extentsMin;
    copy.extentsMax = this.extentsMax;
    copy.insertionBase = this.insertionBase;
    copy.ucsOrigin = this.ucsOrigin;
    copy.ucsXAxis = this.ucsXAxis;
    copy.ucsYAxis = this.ucsYAxis;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof Layout)) {
      throw new TypeError('Snapshot type mismatch restoring a layout');
    }
    this.restoreBaseFrom(snapshot);
    this.name = snapshot.name;
    this.blockTableRecordHandle = snapshot.blockTableRecordHandle;
    this.tabOrder = snapshot.tabOrder;
    this.plotSettings = { ...snapshot.plotSettings };
    this.viewportHandles = [...snapshot.viewportHandles];
    this.limitsMin = snapshot.limitsMin;
    this.limitsMax = snapshot.limitsMax;
    this.extentsMin = snapshot.extentsMin;
    this.extentsMax = snapshot.extentsMax;
    this.insertionBase = snapshot.insertionBase;
    this.ucsOrigin = snapshot.ucsOrigin;
    this.ucsXAxis = snapshot.ucsXAxis;
    this.ucsYAxis = snapshot.ucsYAxis;
  }

  override referencedHandles(): Handle[] {
    const refs = [...super.referencedHandles(), ...this.viewportHandles];
    if (this.blockTableRecordHandle !== NULL_HANDLE) refs.push(this.blockTableRecordHandle);
    return refs;
  }
}
