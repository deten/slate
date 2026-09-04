import { NULL_HANDLE, type Handle } from './handle.js';

/**
 * A single extended-data (XData) item, preserved verbatim.
 *
 * XData is how third-party applications and vertical products (structural,
 * MEP, survey) attach their own information to entities. It is opaque to this
 * application, but discarding it silently corrupts other people's data — so it
 * is carried through load, edit and save untouched.
 */
export interface XDataItem {
  /** DXF group code, in the 1000-1071 XData range. */
  readonly code: number;
  readonly value: string | number | readonly number[] | Handle;
}

/** XData grouped by the registered application name that owns it. */
export type XDataTable = ReadonlyMap<string, readonly XDataItem[]>;

/**
 * Data read from a file that this application does not yet interpret.
 *
 * Rather than dropping unknown group codes, the reader parks them here and the
 * writer emits them again. This is what makes it safe to open a professional
 * drawing containing features we have not implemented, edit an unrelated part
 * of it, and save without destroying anything.
 */
export interface PreservedData {
  /** Raw group-code pairs that no property mapped to. */
  readonly unknownCodes: ReadonlyArray<{ code: number; value: unknown }>;
  /** The object's DXF/DWG class name, for round-tripping unknown types. */
  readonly originalType?: string;
}

/**
 * Base class for everything stored in the drawing database — entities, symbol
 * table records, dictionaries, layouts and the objects yet to be implemented.
 *
 * Objects are *mutable*. This is a considered choice: a real drawing holds
 * hundreds of thousands of objects, and an immutable graph would make every
 * edit allocate proportionally to the drawing rather than to the change.
 * Undo correctness is instead provided by the transaction layer, which clones
 * an object's state the first time it is opened for write within a
 * transaction. That is the same model other CAD software itself uses.
 */
export abstract class DbObject {
  /** Permanent identity. Assigned when the object is added to a database. */
  handle: Handle = NULL_HANDLE;

  /**
   * The object that owns this one — a block record for an entity, the block
   * table for a block record, and so on. Ownership is a tree and drives
   * deletion; other references are non-owning.
   */
  ownerHandle: Handle = NULL_HANDLE;

  /**
   * Handle of this object's extension dictionary, if it has one. Extension
   * dictionaries hold arbitrary named object references and are how features
   * like multileader styles and layer states attach to objects.
   */
  extensionDictionaryHandle: Handle | null = null;

  /**
   * Handles of persistent reactors — objects that must be notified when this
   * one changes. Preserved for fidelity; the notification mechanism itself is
   * not yet implemented.
   */
  reactorHandles: Handle[] = [];

  /** Application-registered extended data, preserved verbatim. */
  xdata: Map<string, XDataItem[]> = new Map();

  /** Group codes read from file that no property claimed. */
  preserved: PreservedData | null = null;

  /**
   * Erased objects remain in the database so that undo can restore them and so
   * that dangling references resolve to something rather than nothing. They are
   * skipped by iteration, rendering and selection.
   */
  erased = false;

  /** Discriminator used for serialization and type dispatch. */
  abstract get objectType(): string;

  /**
   * Deep-copy this object's mutable state.
   *
   * Used by the transaction layer for undo snapshots and by COPY/array
   * commands. Subclasses must override, call `super.copyBaseInto`, and copy
   * their own fields; anything missed silently breaks undo, so each entity
   * type has a round-trip test covering exactly this.
   */
  abstract clone(): DbObject;

  /**
   * Copy the base-class state into `target`. Deliberately not copying the
   * handle: a clone used for undo restores onto the same handle, while a clone
   * used for COPY must receive a fresh one from the database.
   */
  protected copyBaseInto(target: DbObject): void {
    target.ownerHandle = this.ownerHandle;
    target.extensionDictionaryHandle = this.extensionDictionaryHandle;
    target.reactorHandles = [...this.reactorHandles];
    target.erased = this.erased;
    target.xdata = new Map(
      [...this.xdata].map(([app, items]) => [app, items.map((i) => ({ ...i }))]),
    );
    target.preserved = this.preserved === null
      ? null
      : {
          unknownCodes: this.preserved.unknownCodes.map((u) => ({ ...u })),
          ...(this.preserved.originalType !== undefined
            ? { originalType: this.preserved.originalType }
            : {}),
        };
  }

  /**
   * Restore this object's state from a snapshot taken by {@link clone}.
   * The handle is intentionally left alone, so the object keeps its identity
   * across an undo.
   */
  abstract restoreFrom(snapshot: DbObject): void;

  protected restoreBaseFrom(snapshot: DbObject): void {
    this.ownerHandle = snapshot.ownerHandle;
    this.extensionDictionaryHandle = snapshot.extensionDictionaryHandle;
    this.reactorHandles = [...snapshot.reactorHandles];
    this.erased = snapshot.erased;
    this.xdata = new Map(
      [...snapshot.xdata].map(([app, items]) => [app, items.map((i) => ({ ...i }))]),
    );
    this.preserved = snapshot.preserved;
  }

  /** Read XData registered by a named application. */
  getXData(appName: string): readonly XDataItem[] | undefined {
    return this.xdata.get(appName);
  }

  setXData(appName: string, items: readonly XDataItem[]): void {
    this.xdata.set(appName, [...items]);
  }

  /**
   * Handles this object references but does not own.
   *
   * Used to validate a loaded database, to detect what an operation would
   * break, and to remap references when objects are cloned between databases
   * (the mechanism behind block insertion and copy/paste between drawings).
   * Subclasses override to add their own references.
   */
  referencedHandles(): Handle[] {
    const refs: Handle[] = [...this.reactorHandles];
    if (this.extensionDictionaryHandle !== null) refs.push(this.extensionDictionaryHandle);
    return refs;
  }
}
