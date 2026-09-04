import { DbObject } from './db-object.js';
import type { Handle } from './handle.js';

/**
 * Base class for named records in a symbol table (layers, linetypes, text
 * styles, dimension styles, block definitions, views, UCSs).
 */
export abstract class SymbolTableRecord extends DbObject {
  /**
   * Record name. Symbol names are case-insensitive for lookup but their
   * original casing is preserved for display and for writing back out.
   */
  name = '';

  /**
   * `true` for records the application depends on and that must not be
   * deleted — layer "0", linetype "Continuous", text style "Standard",
   * and the model/paper space block records.
   */
  get isReserved(): boolean {
    return false;
  }

  protected copyRecordInto(target: SymbolTableRecord): void {
    this.copyBaseInto(target);
    target.name = this.name;
  }

  protected restoreRecordFrom(snapshot: SymbolTableRecord): void {
    this.restoreBaseFrom(snapshot);
    this.name = snapshot.name;
  }
}

/**
 * A name-keyed collection of symbol table records.
 *
 * Lookup is case-insensitive, matching other CAD software: a drawing cannot contain both
 * "Walls" and "WALLS". The table keeps a normalized-name index alongside the
 * handle list so that the very common "find layer by name" path is O(1)
 * without forcing callers to normalize.
 */
export class SymbolTable<T extends SymbolTableRecord> extends DbObject {
  readonly tableName: string;

  /** Normalized name -> handle. */
  private readonly byName = new Map<string, Handle>();
  /** Insertion order, which is the order records are written back out. */
  private readonly order: Handle[] = [];

  /**
   * Resolves a handle to its record. Supplied by the database so the table
   * does not need a back-reference to the whole document.
   */
  private readonly resolve: (handle: Handle) => T | undefined;

  constructor(tableName: string, resolve: (handle: Handle) => T | undefined) {
    super();
    this.tableName = tableName;
    this.resolve = resolve;
  }

  override get objectType(): string {
    return `SymbolTable:${this.tableName}`;
  }

  /** Canonical form used for case-insensitive comparison. */
  static normalize(name: string): string {
    return name.trim().toUpperCase();
  }

  get size(): number {
    return this.order.length;
  }

  has(name: string): boolean {
    return this.byName.has(SymbolTable.normalize(name));
  }

  /** Look up by name, case-insensitively. */
  get(name: string): T | undefined {
    const handle = this.byName.get(SymbolTable.normalize(name));
    return handle === undefined ? undefined : this.resolve(handle);
  }

  getHandle(name: string): Handle | undefined {
    return this.byName.get(SymbolTable.normalize(name));
  }

  /**
   * Register a record. The record must already have a handle assigned by the
   * database.
   * @throws {Error} when the name is already taken by a different record.
   */
  add(record: T): void {
    const key = SymbolTable.normalize(record.name);
    const existing = this.byName.get(key);
    if (existing !== undefined && existing !== record.handle) {
      throw new Error(
        `${this.tableName} already contains a record named ${JSON.stringify(record.name)}`,
      );
    }
    if (existing === undefined) {
      this.order.push(record.handle);
    }
    this.byName.set(key, record.handle);
    record.ownerHandle = this.handle;
  }

  /**
   * Remove a record from the table. Does not erase the object itself; the
   * caller (the database) decides whether the object stays for undo.
   */
  remove(name: string): boolean {
    const key = SymbolTable.normalize(name);
    const handle = this.byName.get(key);
    if (handle === undefined) return false;
    this.byName.delete(key);
    const index = this.order.indexOf(handle);
    if (index >= 0) this.order.splice(index, 1);
    return true;
  }

  /**
   * Rename a record, keeping the index consistent.
   * @returns `false` when the new name is already in use.
   */
  rename(oldName: string, newName: string): boolean {
    const oldKey = SymbolTable.normalize(oldName);
    const newKey = SymbolTable.normalize(newName);
    const handle = this.byName.get(oldKey);
    if (handle === undefined) return false;
    if (oldKey !== newKey && this.byName.has(newKey)) return false;

    this.byName.delete(oldKey);
    this.byName.set(newKey, handle);
    const record = this.resolve(handle);
    if (record !== undefined) record.name = newName;
    return true;
  }

  /** Records in insertion order, skipping any that fail to resolve. */
  *records(): IterableIterator<T> {
    for (const handle of this.order) {
      const record = this.resolve(handle);
      if (record !== undefined) yield record;
    }
  }

  toArray(): T[] {
    return [...this.records()];
  }

  names(): string[] {
    return this.toArray().map((r) => r.name);
  }

  handles(): readonly Handle[] {
    return this.order;
  }

  /**
   * Generate a name not already present, e.g. `Layer1`, `Layer2`.
   * Used when importing a drawing whose names collide with the host's.
   */
  uniqueName(base: string): string {
    if (!this.has(base)) return base;
    for (let i = 1; ; i++) {
      const candidate = `${base}${i}`;
      if (!this.has(candidate)) return candidate;
    }
  }

  override clone(): SymbolTable<T> {
    const copy = new SymbolTable<T>(this.tableName, this.resolve);
    this.copyBaseInto(copy);
    copy.order.push(...this.order);
    for (const [key, handle] of this.byName) copy.byName.set(key, handle);
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof SymbolTable)) {
      throw new TypeError('Snapshot type mismatch restoring a symbol table');
    }
    this.restoreBaseFrom(snapshot);
    this.order.length = 0;
    this.order.push(...(snapshot.order as Handle[]));
    this.byName.clear();
    for (const [key, handle] of snapshot.byName as Map<string, Handle>) {
      this.byName.set(key, handle);
    }
  }

  override referencedHandles(): Handle[] {
    return [...super.referencedHandles(), ...this.order];
  }
}
