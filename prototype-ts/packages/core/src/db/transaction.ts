import type { Database } from './database.js';
import type { DbObject } from './db-object.js';
import { Entity } from './entity.js';
import { BlockTableRecord } from './block-table-record.js';
import type { Handle } from './handle.js';

/**
 * One reversible change to a single object.
 *
 * Storing whole-object snapshots rather than field-level diffs is a deliberate
 * trade: snapshots cost more memory per change but make undo *provably*
 * correct for every entity type, including ones added later by plugins, with
 * no per-type undo code to get wrong. Memory is bounded instead by capping the
 * undo stack depth.
 */
type ObjectChange =
  | {
      readonly kind: 'added';
      readonly handle: Handle;
      /** Where in the owner's draw order it was inserted. */
      readonly ownerHandle: Handle;
    }
  | {
      readonly kind: 'modified';
      readonly handle: Handle;
      /** State before the change. */
      readonly before: DbObject;
    }
  | {
      readonly kind: 'erased';
      readonly handle: Handle;
      readonly ownerHandle: Handle;
      /** Position in the owner's draw order, so redo restores it in place. */
      readonly index: number;
    };

/**
 * A completed, reversible unit of work.
 *
 * One command produces exactly one of these no matter how many objects it
 * touched, which is what makes a complex operation like FILLET undo as a
 * single step rather than as a confusing sequence of partial states.
 */
export class UndoRecord {
  readonly name: string;
  readonly changes: readonly ObjectChange[];
  readonly timestamp: number;

  constructor(name: string, changes: readonly ObjectChange[]) {
    this.name = name;
    this.changes = changes;
    this.timestamp = Date.now();
  }

  get isEmpty(): boolean {
    return this.changes.length === 0;
  }

  get changeCount(): number {
    return this.changes.length;
  }
}

/**
 * A scope in which database modifications are recorded so they can be undone.
 *
 * Usage is always through {@link TransactionManager.run}, which guarantees
 * that a transaction is either committed in full or aborted with every change
 * rolled back. Direct construction is not exposed: a transaction that is
 * neither committed nor aborted would leave the database mutated but
 * unrecorded, which is exactly the corruption the design exists to prevent.
 */
export class Transaction {
  private readonly db: Database;
  private readonly changes: ObjectChange[] = [];
  /**
   * Handles already snapshotted in this transaction. An object modified ten
   * times still needs only its original state kept.
   */
  private readonly snapshotted = new Set<Handle>();
  private state: 'open' | 'committed' | 'aborted' = 'open';

  /** @internal Constructed only by {@link TransactionManager}. */
  constructor(db: Database, readonly name: string) {
    this.db = db;
  }

  get isOpen(): boolean {
    return this.state === 'open';
  }

  private assertOpen(): void {
    if (this.state !== 'open') {
      throw new Error(`Transaction "${this.name}" is already ${this.state}`);
    }
  }

  /**
   * Declare that an object is about to be modified.
   *
   * **Every** mutation of a database object must be preceded by this call.
   * Forgetting it does not corrupt the database immediately — it silently
   * makes that change un-undoable, which surfaces later as a confusing bug.
   * The helper methods below cover the common cases so most command code never
   * calls this directly.
   */
  openForWrite<T extends DbObject>(object: T): T {
    this.assertOpen();
    if (!this.snapshotted.has(object.handle)) {
      this.snapshotted.add(object.handle);
      this.changes.push({
        kind: 'modified',
        handle: object.handle,
        before: object.clone(),
      });
    }
    return object;
  }

  /** Add a new entity to a block record, recording it for undo. */
  addEntity(entity: Entity, owner: BlockTableRecord): Entity {
    this.assertOpen();
    // The owner's entity list changes, so it needs a snapshot too.
    this.openForWrite(owner);
    this.db.addEntity(entity, owner);
    this.changes.push({
      kind: 'added',
      handle: entity.handle,
      ownerHandle: owner.handle,
    });
    return entity;
  }

  /** Add an entity to model space. */
  addToModelSpace(entity: Entity): Entity {
    return this.addEntity(entity, this.db.modelSpace);
  }

  /** Add a non-entity object (a layer, style, layout) for undo. */
  addObject<T extends DbObject>(object: T): T {
    this.assertOpen();
    this.db.addObject(object);
    this.changes.push({
      kind: 'added',
      handle: object.handle,
      ownerHandle: object.ownerHandle,
    });
    return object;
  }

  /** Erase an object, recording enough to restore it in draw order. */
  erase(object: DbObject): boolean {
    this.assertOpen();
    if (object.erased) return false;
    const owner = this.db.getTyped(object.ownerHandle, BlockTableRecord);
    const index = owner?.entityHandles.indexOf(object.handle) ?? -1;
    if (owner !== undefined) this.openForWrite(owner);

    const erased = this.db.eraseObject(object.handle);
    if (erased) {
      this.changes.push({
        kind: 'erased',
        handle: object.handle,
        ownerHandle: object.ownerHandle,
        index: index < 0 ? 0 : index,
      });
    }
    return erased;
  }

  /**
   * Apply a mutation to an entity and notify the database.
   *
   * The convenience path most commands use: it snapshots, mutates, and fires
   * the change notification in the right order, so none of those can be
   * forgotten independently.
   */
  modifyEntity<T extends Entity>(entity: T, mutate: (e: T) => void): T {
    this.openForWrite(entity);
    mutate(entity);
    this.db.notifyModified(entity.handle);
    if (entity.ownerHandle !== undefined) {
      this.db.invalidateBlockExtents(entity.ownerHandle);
    }
    return entity;
  }

  /** @internal Finalize and hand the recorded changes to the manager. */
  commit(): UndoRecord {
    this.assertOpen();
    this.state = 'committed';
    return new UndoRecord(this.name, this.changes);
  }

  /**
   * Roll every change back, leaving the database exactly as it was.
   *
   * Called when a command is cancelled or throws. This is what makes
   * "cancellation must never corrupt the drawing" true by construction rather
   * than by each command remembering to clean up after itself.
   */
  abort(): void {
    if (this.state !== 'open') return;
    this.state = 'aborted';
    undoChanges(this.db, this.changes);
  }
}

/**
 * Reverse a list of changes, newest first.
 *
 * Order matters: a transaction that created an object and then modified it
 * must undo the modification before removing the object.
 */
function undoChanges(db: Database, changes: readonly ObjectChange[]): void {
  for (let i = changes.length - 1; i >= 0; i--) {
    const change = changes[i]!;
    switch (change.kind) {
      case 'added':
        // Erase rather than delete: a redo needs the object back, and other
        // objects may still reference it.
        db.eraseObject(change.handle);
        break;
      case 'modified': {
        const object = db.getObject(change.handle);
        object?.restoreFrom(change.before);
        db.notifyModified(change.handle);
        break;
      }
      case 'erased':
        db.uneraseObject(change.handle, change.index);
        break;
    }
  }
}

/**
 * Owns the undo and redo stacks and is the only supported way to modify the
 * database.
 */
export class TransactionManager {
  private readonly db: Database;
  private readonly undoStack: UndoRecord[] = [];
  private readonly redoStack: UndoRecord[] = [];
  private active: Transaction | null = null;

  /**
   * Maximum undo depth. Bounded because whole-object snapshots would otherwise
   * grow without limit over a long editing session.
   */
  maxUndoDepth = 200;

  private readonly listeners = new Set<() => void>();

  constructor(db: Database) {
    this.db = db;
  }

  get hasActiveTransaction(): boolean {
    return this.active !== null;
  }

  get canUndo(): boolean {
    return this.undoStack.length > 0;
  }

  get canRedo(): boolean {
    return this.redoStack.length > 0;
  }

  /** Name of the operation that would be undone, for the menu item. */
  get undoName(): string | null {
    return this.undoStack[this.undoStack.length - 1]?.name ?? null;
  }

  get redoName(): string | null {
    return this.redoStack[this.redoStack.length - 1]?.name ?? null;
  }

  /**
   * Open a transaction with an explicit lifetime.
   *
   * Needed because an interactive command's lifetime is *asynchronous*: LINE
   * opens a transaction, then waits — possibly for many seconds — while the
   * user picks points. A synchronous `run` scope cannot express that, so the
   * caller takes responsibility for calling {@link commit} or {@link abort}.
   *
   * Prefer {@link run} for synchronous work, which cannot leak a transaction.
   *
   * @throws {Error} when a transaction is already open. Concurrent
   *   transactions would interleave their undo records and make the history
   *   incoherent.
   */
  begin(name: string): Transaction {
    if (this.active !== null) {
      throw new Error(
        `Cannot begin transaction "${name}": "${this.active.name}" is still open`,
      );
    }
    const tx = new Transaction(this.db, name);
    this.active = tx;
    return tx;
  }

  /**
   * Commit an explicitly opened transaction, pushing it onto the undo stack.
   *
   * A transaction that changed nothing is discarded rather than recorded, so
   * that a cancelled or no-op command does not leave a confusing empty entry
   * in the undo history.
   *
   * @returns `true` when an undo entry was recorded.
   */
  commit(tx: Transaction): boolean {
    if (this.active !== tx) {
      throw new Error('Attempted to commit a transaction that is not the open one');
    }
    this.active = null;
    const record = tx.commit();
    if (record.isEmpty) {
      this.notify();
      return false;
    }
    this.push(record);
    return true;
  }

  /** Roll back an explicitly opened transaction. Safe to call twice. */
  abort(tx: Transaction): void {
    if (this.active === tx) this.active = null;
    tx.abort();
    this.notify();
  }

  /**
   * Run `fn` inside a transaction.
   *
   * Commits when `fn` returns normally and aborts when it throws, so a command
   * that fails partway cannot leave the drawing in a half-edited state. The
   * transaction is also aborted — not committed — when `fn` returns
   * {@link ABORT}, which is how a command signals user cancellation without
   * throwing.
   */
  run<T>(name: string, fn: (tx: Transaction) => T): T {
    if (this.active !== null) {
      // Nested transactions join the outer one rather than creating a second
      // undo entry. A command that calls another command should still undo as
      // a single step.
      return fn(this.active);
    }

    const tx = this.begin(name);
    try {
      const result = fn(tx);
      if (result === ABORT) {
        this.abort(tx);
        return result;
      }
      this.commit(tx);
      return result;
    } catch (error) {
      this.abort(tx);
      throw error;
    }
  }

  private push(record: UndoRecord): void {
    this.undoStack.push(record);
    // Any new edit invalidates the redo branch, matching every editor's
    // behaviour and avoiding an inconsistent history tree.
    this.redoStack.length = 0;
    while (this.undoStack.length > this.maxUndoDepth) {
      this.undoStack.shift();
    }
    this.notify();
  }

  /**
   * Undo the most recent operation.
   *
   * Before reversing, the current state of every modified object is captured
   * so that redo can restore it — which is why undo and redo are symmetric
   * rather than redo needing its own recorded forward diff.
   */
  undo(): boolean {
    if (this.active !== null) {
      throw new Error('Cannot undo while a transaction is open');
    }
    const record = this.undoStack.pop();
    if (record === undefined) return false;

    const inverted = this.captureInverse(record);
    undoChanges(this.db, record.changes);
    this.redoStack.push(inverted);
    this.notify();
    return true;
  }

  redo(): boolean {
    if (this.active !== null) {
      throw new Error('Cannot redo while a transaction is open');
    }
    const record = this.redoStack.pop();
    if (record === undefined) return false;

    const inverted = this.captureInverse(record);
    undoChanges(this.db, record.changes);
    this.undoStack.push(inverted);
    this.notify();
    return true;
  }

  /**
   * Build the record that reverses `record`.
   *
   * For modifications this means snapshotting the object's *current* state
   * before it is overwritten. For additions and erasures the inverse is just
   * the opposite operation, which {@link undoChanges} already performs, so the
   * change is re-recorded with its kind flipped.
   */
  private captureInverse(record: UndoRecord): UndoRecord {
    const inverse: ObjectChange[] = [];
    for (const change of record.changes) {
      switch (change.kind) {
        case 'modified': {
          const object = this.db.getObject(change.handle);
          if (object !== undefined) {
            inverse.push({
              kind: 'modified',
              handle: change.handle,
              before: object.clone(),
            });
          }
          break;
        }
        case 'added':
          // Undoing an 'added' erases it; the inverse must re-add it.
          inverse.push({
            kind: 'erased',
            handle: change.handle,
            ownerHandle: change.ownerHandle,
            index: this.indexOf(change.ownerHandle, change.handle),
          });
          break;
        case 'erased':
          inverse.push({
            kind: 'added',
            handle: change.handle,
            ownerHandle: change.ownerHandle,
          });
          break;
      }
    }
    return new UndoRecord(record.name, inverse);
  }

  private indexOf(ownerHandle: Handle, handle: Handle): number {
    const owner = this.db.getTyped(ownerHandle, BlockTableRecord);
    const index = owner?.entityHandles.indexOf(handle) ?? -1;
    return index < 0 ? 0 : index;
  }

  /** Discard all history, e.g. after opening a new file. */
  clear(): void {
    this.undoStack.length = 0;
    this.redoStack.length = 0;
    this.notify();
  }

  get undoDepth(): number {
    return this.undoStack.length;
  }

  get redoDepth(): number {
    return this.redoStack.length;
  }

  /** Names of the operations on the undo stack, newest last. */
  get history(): string[] {
    return this.undoStack.map((r) => r.name);
  }

  addListener(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) listener();
  }
}

/**
 * Returned from a transaction body to abort it without throwing.
 *
 * User cancellation is not an error, so making it an exception would mean
 * every caller distinguishing "the user pressed Escape" from "something broke".
 */
export const ABORT = Symbol('transaction.abort');
export type Abort = typeof ABORT;
