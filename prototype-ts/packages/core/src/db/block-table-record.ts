import { SymbolTableRecord } from './symbol-table.js';
import type { DbObject } from './db-object.js';
import { NULL_HANDLE, type Handle } from './handle.js';
import { Vec3 } from '../math/vec3.js';

/** Name of the block record holding model-space geometry. */
export const MODEL_SPACE = '*Model_Space';
/** Name of the block record holding the first paper-space layout. */
export const PAPER_SPACE = '*Paper_Space';

/**
 * How an external reference's path was recorded, which determines how it is
 * resolved on load.
 */
export enum XrefType {
  /** Not an external reference. */
  None = 'none',
  /** Contents are copied into the host on load and merged as one unit. */
  Overlay = 'overlay',
  /** Contents are linked; nested xrefs of an attachment are also loaded. */
  Attachment = 'attachment',
}

/**
 * The current resolution state of an external reference.
 *
 * Tracking this explicitly is what allows the reference manager to show a
 * useful status, and what allows a drawing with a broken reference to open
 * anyway — a missing xref must never prevent the host drawing from loading.
 */
export enum XrefStatus {
  /** Not an xref at all. */
  NotAnXref = 'notAnXref',
  /** Loaded successfully. */
  Resolved = 'resolved',
  /** The file could not be found on any search path. */
  NotFound = 'notFound',
  /** Found but failed to parse. */
  Unresolved = 'unresolved',
  /** Deliberately unloaded by the user; the reference is retained. */
  Unloaded = 'unloaded',
  /** Loading would create a cycle. */
  Circular = 'circular',
}

/**
 * A block definition — the container for a named, reusable set of entities.
 *
 * Block table records serve three distinct roles, which is why this class is
 * central to the whole document model:
 *
 *  1. **Ordinary blocks**: reusable symbol definitions instanced by INSERT.
 *  2. **Layout spaces**: model space and each paper-space layout are block
 *     records. Every entity in the drawing lives in one of them.
 *  3. **External references**: an xref is a block record whose content comes
 *     from another file.
 *
 * Treating all three uniformly means layouts, blocks and xrefs share the same
 * entity storage, traversal, and rendering paths rather than each needing
 * their own.
 */
export class BlockTableRecord extends SymbolTableRecord {
  /**
   * Handles of the entities in this block, in draw order.
   *
   * Order is significant: it determines what is drawn on top, and DWG
   * preserves it. Storing handles rather than objects keeps the record valid
   * across undo, when the objects themselves are replaced by restored clones.
   */
  entityHandles: Handle[] = [];

  /**
   * Base point of the block, in the block's own coordinate system. An INSERT
   * places this point at its insertion point.
   */
  origin: Vec3 = Vec3.ZERO;

  /** Description shown in the block manager. */
  description = '';

  /** Xref attachment mode. */
  xrefType: XrefType = XrefType.None;

  /** Current resolution state. */
  xrefStatus: XrefStatus = XrefStatus.NotAnXref;

  /**
   * Path exactly as stored in the file. Preserved verbatim — resolution
   * produces a separate resolved path and never overwrites this, so that
   * saving does not silently rewrite the user's relative paths as absolute.
   */
  xrefPath = '';

  /** Absolute path the reference actually resolved to, once loaded. */
  resolvedPath: string | null = null;

  /**
   * `true` when this record was brought in *by* an xref rather than defined
   * locally. Xref-dependent blocks are named `xrefname|blockname`.
   */
  isXrefDependent = false;

  /**
   * `true` for anonymous blocks — those other CAD software generates internally for
   * hatch associativity, dimensions and dynamic block states. Their names
   * begin with `*` and they are hidden from the block picker.
   */
  get isAnonymous(): boolean {
    return this.name.startsWith('*');
  }

  /** `true` when this record is model space or a paper-space layout. */
  get isLayoutSpace(): boolean {
    const upper = this.name.toUpperCase();
    return upper.startsWith('*MODEL_SPACE') || upper.startsWith('*PAPER_SPACE');
  }

  get isModelSpace(): boolean {
    return this.name.toUpperCase().startsWith('*MODEL_SPACE');
  }

  get isXref(): boolean {
    return this.xrefType !== XrefType.None;
  }

  /**
   * `true` when the reference is present but its content is unavailable.
   * The renderer draws a placeholder for these rather than nothing, so the
   * user can see that something is missing.
   */
  get isMissingContent(): boolean {
    return (
      this.isXref &&
      this.xrefStatus !== XrefStatus.Resolved &&
      this.xrefStatus !== XrefStatus.Unloaded
    );
  }

  /** Handle of the layout object, for records that are layout spaces. */
  layoutHandle: Handle | null = null;

  /** `true` when the block contains attribute definitions. */
  hasAttributeDefinitions = false;

  /**
   * Units the block was authored in, for automatic scaling on insertion.
   * Matches the DXF `$INSUNITS` enumeration.
   */
  units = 0;

  /** `true` when the block may be scaled non-uniformly. */
  allowsExploding = true;
  allowsNonUniformScaling = true;

  override get objectType(): string {
    return 'BlockTableRecord';
  }

  override get isReserved(): boolean {
    return this.isLayoutSpace;
  }

  get entityCount(): number {
    return this.entityHandles.length;
  }

  /** Append an entity handle to the end of the draw order. */
  appendEntity(handle: Handle): void {
    this.entityHandles.push(handle);
  }

  /** Remove an entity handle. Returns `false` when it was not present. */
  removeEntity(handle: Handle): boolean {
    const index = this.entityHandles.indexOf(handle);
    if (index < 0) return false;
    this.entityHandles.splice(index, 1);
    return true;
  }

  /** Move an entity within the draw order (DRAWORDER / BRING TO FRONT). */
  reorderEntity(handle: Handle, newIndex: number): boolean {
    const index = this.entityHandles.indexOf(handle);
    if (index < 0) return false;
    this.entityHandles.splice(index, 1);
    const clamped = Math.max(0, Math.min(this.entityHandles.length, newIndex));
    this.entityHandles.splice(clamped, 0, handle);
    return true;
  }

  override clone(): BlockTableRecord {
    const copy = new BlockTableRecord();
    this.copyRecordInto(copy);
    copy.entityHandles = [...this.entityHandles];
    copy.origin = this.origin;
    copy.description = this.description;
    copy.xrefType = this.xrefType;
    copy.xrefStatus = this.xrefStatus;
    copy.xrefPath = this.xrefPath;
    copy.resolvedPath = this.resolvedPath;
    copy.isXrefDependent = this.isXrefDependent;
    copy.layoutHandle = this.layoutHandle;
    copy.hasAttributeDefinitions = this.hasAttributeDefinitions;
    copy.units = this.units;
    copy.allowsExploding = this.allowsExploding;
    copy.allowsNonUniformScaling = this.allowsNonUniformScaling;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof BlockTableRecord)) {
      throw new TypeError('Snapshot type mismatch restoring a block record');
    }
    this.restoreRecordFrom(snapshot);
    this.entityHandles = [...snapshot.entityHandles];
    this.origin = snapshot.origin;
    this.description = snapshot.description;
    this.xrefType = snapshot.xrefType;
    this.xrefStatus = snapshot.xrefStatus;
    this.xrefPath = snapshot.xrefPath;
    this.resolvedPath = snapshot.resolvedPath;
    this.isXrefDependent = snapshot.isXrefDependent;
    this.layoutHandle = snapshot.layoutHandle;
    this.hasAttributeDefinitions = snapshot.hasAttributeDefinitions;
    this.units = snapshot.units;
    this.allowsExploding = snapshot.allowsExploding;
    this.allowsNonUniformScaling = snapshot.allowsNonUniformScaling;
  }

  override referencedHandles(): Handle[] {
    const refs = [...super.referencedHandles(), ...this.entityHandles];
    if (this.layoutHandle !== null && this.layoutHandle !== NULL_HANDLE) {
      refs.push(this.layoutHandle);
    }
    return refs;
  }
}
