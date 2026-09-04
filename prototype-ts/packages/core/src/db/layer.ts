import { SymbolTableRecord } from './symbol-table.js';
import type { DbObject } from './db-object.js';
import { Color, Lineweight, type Transparency } from './color.js';
import { NULL_HANDLE, type Handle } from './handle.js';

/** The layer every drawing has and none may delete or rename. */
export const LAYER_ZERO = '0';
/** The layer other CAD software reserves for xref-dependent defpoints. */
export const LAYER_DEFPOINTS = 'Defpoints';

/**
 * A drawing layer.
 *
 * Layers carry both *visibility* state (on/off, frozen, viewport-frozen) and
 * *default property* state (colour, linetype, lineweight) that entities
 * inherit through ByLayer. Both are modelled here rather than resolved at load
 * time so that toggling a layer updates every entity on it immediately.
 *
 * The distinction between "off" and "frozen" is preserved because it is real:
 * frozen layers are skipped during regeneration and are not considered by
 * ZOOM EXTENTS, while layers that are merely off still participate.
 */
export class Layer extends SymbolTableRecord {
  /** Default colour for entities whose colour is ByLayer. */
  color: Color = Color.fromIndex(7);

  /** Handle of the default linetype record. */
  linetypeHandle: Handle = NULL_HANDLE;

  /** Default lineweight, in hundredths of a millimetre. */
  lineweight: number = Lineweight.Default;

  /** Default transparency; `null` means fully opaque. */
  transparency: Transparency = null;

  /**
   * Turned off: entities are hidden but still regenerated and still included
   * in extents.
   */
  isOff = false;

  /**
   * Frozen: entities are hidden *and* excluded from regeneration and extents.
   * Freezing is the performance tool; turning off is the visual one.
   */
  isFrozen = false;

  /** Locked layers render normally but reject selection and modification. */
  isLocked = false;

  /** Excluded from plotting while still visible on screen. */
  isPlottable = true;

  /** Frozen by default in newly created viewports. */
  isFrozenInNewViewports = false;

  /** Handle of the plot style record, for named plot style drawings. */
  plotStyleHandle: Handle | null = null;

  /** Optional description shown in the layer manager. */
  description = '';

  /**
   * `true` when this layer came from an external reference. Xref layers are
   * named `xrefname|layername` and may not be renamed or deleted directly.
   */
  isXrefDependent = false;

  override get objectType(): string {
    return 'Layer';
  }

  override get isReserved(): boolean {
    return this.name === LAYER_ZERO;
  }

  /**
   * `true` when entities on this layer should be drawn.
   *
   * Locked layers are still visible — the lock only restricts editing — which
   * is why locking is not part of this test.
   */
  get isVisible(): boolean {
    return !this.isOff && !this.isFrozen;
  }

  /** `true` when entities on this layer may be selected or edited. */
  get isEditable(): boolean {
    return this.isVisible && !this.isLocked;
  }

  override clone(): Layer {
    const copy = new Layer();
    this.copyRecordInto(copy);
    copy.color = this.color;
    copy.linetypeHandle = this.linetypeHandle;
    copy.lineweight = this.lineweight;
    copy.transparency = this.transparency;
    copy.isOff = this.isOff;
    copy.isFrozen = this.isFrozen;
    copy.isLocked = this.isLocked;
    copy.isPlottable = this.isPlottable;
    copy.isFrozenInNewViewports = this.isFrozenInNewViewports;
    copy.plotStyleHandle = this.plotStyleHandle;
    copy.description = this.description;
    copy.isXrefDependent = this.isXrefDependent;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof Layer)) {
      throw new TypeError('Snapshot type mismatch restoring a layer');
    }
    this.restoreRecordFrom(snapshot);
    this.color = snapshot.color;
    this.linetypeHandle = snapshot.linetypeHandle;
    this.lineweight = snapshot.lineweight;
    this.transparency = snapshot.transparency;
    this.isOff = snapshot.isOff;
    this.isFrozen = snapshot.isFrozen;
    this.isLocked = snapshot.isLocked;
    this.isPlottable = snapshot.isPlottable;
    this.isFrozenInNewViewports = snapshot.isFrozenInNewViewports;
    this.plotStyleHandle = snapshot.plotStyleHandle;
    this.description = snapshot.description;
    this.isXrefDependent = snapshot.isXrefDependent;
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    if (this.linetypeHandle !== NULL_HANDLE) refs.push(this.linetypeHandle);
    if (this.plotStyleHandle !== null) refs.push(this.plotStyleHandle);
    return refs;
  }
}
