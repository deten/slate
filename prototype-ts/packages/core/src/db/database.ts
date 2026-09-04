import { DbObject } from './db-object.js';
import { Entity, type ResolutionContext } from './entity.js';
import { HandleAllocator, NULL_HANDLE, type Handle } from './handle.js';
import { SymbolTable, type SymbolTableRecord } from './symbol-table.js';
import { Layer, LAYER_ZERO } from './layer.js';
import { Linetype, LINETYPE_CONTINUOUS, STANDARD_LINETYPES } from './linetype.js';
import { TextStyle, TEXT_STYLE_STANDARD } from './text-style.js';
import {
  BlockTableRecord,
  MODEL_SPACE,
  PAPER_SPACE,
  XrefStatus,
} from './block-table-record.js';
import { Layout } from './layout.js';
import { InsertEntity } from './entities/insert-entity.js';
import { Color, Lineweight } from './color.js';
import { Box3 } from '../math/box3.js';
import { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
import { createDefaultSettings, type DrawingSettings } from './settings.js';

/**
 * A change to the database, broadcast so that dependent systems — the spatial
 * index, the renderer's cache, the UI — can update incrementally rather than
 * rebuilding.
 */
export type DatabaseEvent =
  | { readonly kind: 'objectAdded'; readonly handle: Handle }
  | { readonly kind: 'objectModified'; readonly handle: Handle }
  | { readonly kind: 'objectErased'; readonly handle: Handle }
  | { readonly kind: 'objectUnerased'; readonly handle: Handle }
  | { readonly kind: 'reset' };

export type DatabaseListener = (event: DatabaseEvent) => void;

/**
 * The drawing database — the single source of truth for a document.
 *
 * Everything the application displays or edits lives here. The renderer reads
 * from it and never writes; commands write to it only inside a transaction.
 * That one-way flow is what the architecture rests on: if the renderer were
 * allowed to hold authoritative state, undo, save and multi-viewport display
 * would each need their own reconciliation logic.
 *
 * Objects are stored flat, keyed by handle, with ownership expressed through
 * `ownerHandle` rather than by nesting. A flat store means a handle lookup is
 * always O(1) regardless of nesting depth, and cross-references between
 * unrelated parts of the drawing — an xref layer referenced by a viewport
 * freeze list, say — need no special traversal.
 */
export class Database {
  /** Every object, live or erased, keyed by handle. */
  private readonly objects = new Map<Handle, DbObject>();

  private readonly allocator = new HandleAllocator(1);

  private readonly listeners = new Set<DatabaseListener>();

  /** Suppresses events during bulk load, then fires one reset. */
  private bulkLoading = false;

  readonly layers: SymbolTable<Layer>;
  readonly linetypes: SymbolTable<Linetype>;
  readonly textStyles: SymbolTable<TextStyle>;
  readonly blocks: SymbolTable<BlockTableRecord>;

  /** Layout objects keyed by handle, in tab order. */
  private readonly layoutHandles: Handle[] = [];

  /** Header variables and drafting settings. */
  settings: DrawingSettings = createDefaultSettings();

  /** Absolute path this drawing was loaded from, if any. */
  filePath: string | null = null;

  /** Original file format, so a save can round-trip to the same version. */
  sourceFormat: string | null = null;

  constructor() {
    const resolveAs = <T extends SymbolTableRecord>(ctor: abstract new () => T) =>
      (handle: Handle): T | undefined => {
        const obj = this.objects.get(handle);
        return obj instanceof ctor ? (obj as T) : undefined;
      };

    this.layers = new SymbolTable<Layer>('Layers', resolveAs(Layer));
    this.linetypes = new SymbolTable<Linetype>('Linetypes', resolveAs(Linetype));
    this.textStyles = new SymbolTable<TextStyle>('TextStyles', resolveAs(TextStyle));
    this.blocks = new SymbolTable<BlockTableRecord>('Blocks', resolveAs(BlockTableRecord));

    for (const table of [this.layers, this.linetypes, this.textStyles, this.blocks]) {
      this.addObject(table);
    }
  }

  /**
   * Build a database with the standard records every drawing must have.
   *
   * A drawing without layer "0", linetype "Continuous", style "Standard" and a
   * model-space block record is malformed, so these are created up front
   * rather than lazily — no code downstream should have to defend against
   * their absence.
   */
  static createEmpty(): Database {
    const db = new Database();

    for (const def of STANDARD_LINETYPES) {
      const lt = new Linetype();
      lt.name = def.name;
      lt.description = def.description;
      lt.elements = def.pattern.map((length) => ({ length }));
      db.addLinetype(lt);
    }

    const style = new TextStyle();
    style.name = TEXT_STYLE_STANDARD;
    db.addTextStyle(style);

    const layer0 = new Layer();
    layer0.name = LAYER_ZERO;
    layer0.color = Color.fromIndex(7);
    layer0.linetypeHandle = db.linetypes.getHandle(LINETYPE_CONTINUOUS) ?? NULL_HANDLE;
    layer0.lineweight = Lineweight.Default;
    db.addLayer(layer0);

    const model = new BlockTableRecord();
    model.name = MODEL_SPACE;
    db.addBlock(model);

    const paper = new BlockTableRecord();
    paper.name = PAPER_SPACE;
    db.addBlock(paper);

    const modelLayout = new Layout();
    modelLayout.name = 'Model';
    modelLayout.tabOrder = -1;
    modelLayout.blockTableRecordHandle = model.handle;
    db.addLayout(modelLayout);
    model.layoutHandle = modelLayout.handle;

    const sheetLayout = new Layout();
    sheetLayout.name = 'Layout1';
    sheetLayout.tabOrder = 0;
    sheetLayout.blockTableRecordHandle = paper.handle;
    db.addLayout(sheetLayout);
    paper.layoutHandle = sheetLayout.handle;

    return db;
  }

  // ---------------------------------------------------------------- objects

  /**
   * Add an object, assigning it a fresh handle if it does not already have one.
   *
   * Objects read from a file arrive with their original handle already set, and
   * that handle is kept — references between them depend on it.
   */
  addObject<T extends DbObject>(object: T): T {
    if (object.handle === NULL_HANDLE) {
      object.handle = this.allocator.allocate();
    } else {
      this.allocator.observe(object.handle);
      const existing = this.objects.get(object.handle);
      if (existing !== undefined && existing !== object) {
        throw new Error(
          `Handle collision: ${object.handle} is already held by a ${existing.objectType}`,
        );
      }
    }
    this.objects.set(object.handle, object);
    this.emit({ kind: 'objectAdded', handle: object.handle });
    return object;
  }

  /** Look up any object by handle. */
  getObject(handle: Handle | null | undefined): DbObject | undefined {
    if (handle === null || handle === undefined || handle === NULL_HANDLE) return undefined;
    return this.objects.get(handle);
  }

  /** Look up an object and assert its type. */
  getTyped<T extends DbObject>(
    handle: Handle | null | undefined,
    ctor: abstract new (...args: never[]) => T,
  ): T | undefined {
    const obj = this.getObject(handle);
    return obj instanceof ctor ? obj : undefined;
  }

  getEntity(handle: Handle | null | undefined): Entity | undefined {
    const obj = this.getObject(handle);
    return obj instanceof Entity ? obj : undefined;
  }

  has(handle: Handle): boolean {
    return this.objects.has(handle);
  }

  get objectCount(): number {
    return this.objects.size;
  }

  /** Every object in the database, including erased ones. */
  allObjects(): IterableIterator<DbObject> {
    return this.objects.values();
  }

  /**
   * Mark an object erased.
   *
   * Erased objects are retained so undo can restore them and so references to
   * them still resolve. Purging them for real is a separate, explicit
   * operation performed only when saving.
   */
  eraseObject(handle: Handle): boolean {
    const object = this.objects.get(handle);
    if (object === undefined || object.erased) return false;
    object.erased = true;
    // Detach from its owner's ordering so it stops being drawn and iterated.
    const owner = this.getTyped(object.ownerHandle, BlockTableRecord);
    owner?.removeEntity(handle);
    this.emit({ kind: 'objectErased', handle });
    return true;
  }

  /** Restore an erased object, re-attaching it to its owner. */
  uneraseObject(handle: Handle, restoreAtIndex?: number): boolean {
    const object = this.objects.get(handle);
    if (object === undefined || !object.erased) return false;
    object.erased = false;
    const owner = this.getTyped(object.ownerHandle, BlockTableRecord);
    if (owner !== undefined && !owner.entityHandles.includes(handle)) {
      if (restoreAtIndex === undefined) {
        owner.appendEntity(handle);
      } else {
        owner.entityHandles.splice(restoreAtIndex, 0, handle);
      }
    }
    this.emit({ kind: 'objectUnerased', handle });
    return true;
  }

  /** Announce that an object's contents changed. */
  notifyModified(handle: Handle): void {
    this.emit({ kind: 'objectModified', handle });
  }

  /**
   * Permanently remove erased objects that nothing references.
   *
   * Deliberately conservative: an object still referenced by anything —
   * including an unresolved reference we did not understand — is kept, because
   * dropping it would corrupt whatever holds the reference.
   */
  purgeErased(): number {
    const referenced = new Set<Handle>();
    for (const object of this.objects.values()) {
      if (object.erased) continue;
      for (const ref of object.referencedHandles()) referenced.add(ref);
    }
    let removed = 0;
    for (const [handle, object] of [...this.objects]) {
      if (object.erased && !referenced.has(handle)) {
        this.objects.delete(handle);
        removed++;
      }
    }
    return removed;
  }

  // ----------------------------------------------------------- symbol tables

  addLayer(layer: Layer): Layer {
    this.addObject(layer);
    this.layers.add(layer);
    return layer;
  }

  addLinetype(linetype: Linetype): Linetype {
    this.addObject(linetype);
    this.linetypes.add(linetype);
    return linetype;
  }

  addTextStyle(style: TextStyle): TextStyle {
    this.addObject(style);
    this.textStyles.add(style);
    return style;
  }

  addBlock(block: BlockTableRecord): BlockTableRecord {
    this.addObject(block);
    this.blocks.add(block);
    return block;
  }

  /** Layer "0", which is guaranteed to exist. */
  get defaultLayer(): Layer {
    const layer = this.layers.get(LAYER_ZERO);
    if (layer === undefined) {
      throw new Error('Drawing is missing layer 0; the database was not initialized correctly');
    }
    return layer;
  }

  get modelSpace(): BlockTableRecord {
    const block = this.blocks.get(MODEL_SPACE);
    if (block === undefined) {
      throw new Error('Drawing is missing model space; the database was not initialized correctly');
    }
    return block;
  }

  /**
   * Find or create a layer by name. Used by importers, which must not fail
   * because a drawing references a layer its own table omitted — a real and
   * surprisingly common defect in files produced by other software.
   */
  ensureLayer(name: string): Layer {
    const existing = this.layers.get(name);
    if (existing !== undefined) return existing;
    const layer = new Layer();
    layer.name = name;
    layer.linetypeHandle = this.linetypes.getHandle(LINETYPE_CONTINUOUS) ?? NULL_HANDLE;
    return this.addLayer(layer);
  }

  ensureLinetype(name: string): Linetype {
    const existing = this.linetypes.get(name);
    if (existing !== undefined) return existing;
    const lt = new Linetype();
    lt.name = name;
    return this.addLinetype(lt);
  }

  ensureTextStyle(name: string): TextStyle {
    const existing = this.textStyles.get(name);
    if (existing !== undefined) return existing;
    const style = new TextStyle();
    style.name = name;
    return this.addTextStyle(style);
  }

  // --------------------------------------------------------------- layouts

  addLayout(layout: Layout): Layout {
    this.addObject(layout);
    this.layoutHandles.push(layout.handle);
    return layout;
  }

  /** Layouts in tab order, model space first. */
  get layouts(): Layout[] {
    return this.layoutHandles
      .map((h) => this.getTyped(h, Layout))
      .filter((l): l is Layout => l !== undefined && !l.erased)
      .sort((a, b) => a.tabOrder - b.tabOrder);
  }

  getLayout(name: string): Layout | undefined {
    return this.layouts.find((l) => l.name.toUpperCase() === name.toUpperCase());
  }

  get modelLayout(): Layout | undefined {
    return this.layouts.find((l) => l.isModelSpace);
  }

  /** The block record holding a layout's geometry. */
  blockForLayout(layout: Layout): BlockTableRecord | undefined {
    return this.getTyped(layout.blockTableRecordHandle, BlockTableRecord);
  }

  // -------------------------------------------------------------- entities

  /**
   * Add an entity to a block record (which may be a layout space).
   *
   * Defaults the layer to "0" when unset, so that an importer or a plugin that
   * forgets to assign one produces a valid drawing rather than an entity that
   * cannot resolve its own colour.
   */
  addEntity(entity: Entity, owner: BlockTableRecord): Entity {
    this.addObject(entity);
    if (entity.layerHandle === NULL_HANDLE) {
      entity.layerHandle = this.defaultLayer.handle;
    }
    entity.ownerHandle = owner.handle;
    owner.appendEntity(entity.handle);
    this.invalidateBlockExtents(owner.handle);
    return entity;
  }

  /** Add an entity to model space. */
  addToModelSpace(entity: Entity): Entity {
    return this.addEntity(entity, this.modelSpace);
  }

  /** Live entities directly owned by a block record, in draw order. */
  *entitiesIn(block: BlockTableRecord): IterableIterator<Entity> {
    for (const handle of block.entityHandles) {
      const entity = this.getEntity(handle);
      if (entity !== undefined && !entity.erased) yield entity;
    }
  }

  /**
   * Walk a block's entities recursively, following block references and
   * accumulating their transforms.
   *
   * This single traversal serves rendering, extents, selection and explode.
   * The depth limit guards against a corrupt file whose blocks reference each
   * other in a cycle — which would otherwise hang the application on load.
   */
  *flatten(
    block: BlockTableRecord,
    transform: Mat4,
    maxDepth = 32,
    visiting: Set<Handle> = new Set(),
  ): IterableIterator<{ entity: Entity; transform: Mat4; depth: number }> {
    if (maxDepth <= 0 || visiting.has(block.handle)) return;
    visiting.add(block.handle);
    try {
      for (const entity of this.entitiesIn(block)) {
        if (entity instanceof InsertEntity) {
          const definition = this.getTyped(entity.blockHandle, BlockTableRecord);
          if (definition === undefined) continue;
          for (const cell of entity.cellTransforms()) {
            yield* this.flatten(
              definition,
              transform.multiply(cell),
              maxDepth - 1,
              visiting,
            );
          }
          // The insert itself is still yielded so callers can see the
          // reference (for selection, or to draw a missing-xref placeholder).
          yield { entity, transform, depth: maxDepth };
        } else {
          yield { entity, transform, depth: maxDepth };
        }
      }
    } finally {
      visiting.delete(block.handle);
    }
  }

  // ---------------------------------------------------------------- extents

  /**
   * Extents of a block's contents in its own coordinate system.
   *
   * Memoized per block on the inserts that need it, because a nested block
   * hierarchy would otherwise be re-walked for every reference on every frame.
   */
  blockExtents(block: BlockTableRecord, maxDepth = 32, visiting = new Set<Handle>()): Box3 {
    if (visiting.has(block.handle) || maxDepth <= 0) return Box3.EMPTY;
    visiting.add(block.handle);
    try {
      let result = Box3.EMPTY;
      for (const entity of this.entitiesIn(block)) {
        if (entity instanceof InsertEntity) {
          const definition = this.getTyped(entity.blockHandle, BlockTableRecord);
          if (definition !== undefined) {
            entity.setBlockExtents(this.blockExtents(definition, maxDepth - 1, visiting));
          }
        }
        const layer = this.getTyped(entity.layerHandle, Layer);
        // Frozen layers are excluded from extents, matching ZOOM EXTENTS.
        if (layer !== undefined && layer.isFrozen) continue;
        result = result.union(entity.bounds);
      }
      return result;
    } finally {
      visiting.delete(block.handle);
    }
  }

  /** Extents of model space — what ZOOM EXTENTS targets. */
  get modelExtents(): Box3 {
    return this.blockExtents(this.modelSpace);
  }

  /**
   * Drop cached block extents on every insert that references `blockHandle`,
   * and transitively on anything referencing those.
   */
  invalidateBlockExtents(blockHandle: Handle): void {
    const dirty = new Set<Handle>([blockHandle]);
    let changed = true;
    while (changed) {
      changed = false;
      for (const object of this.objects.values()) {
        if (!(object instanceof InsertEntity) || object.erased) continue;
        if (!dirty.has(object.blockHandle)) continue;
        object.setBlockExtents(null);
        if (!dirty.has(object.ownerHandle)) {
          dirty.add(object.ownerHandle);
          changed = true;
        }
      }
    }
  }

  // ------------------------------------------------------------- properties

  /**
   * Build the context needed to resolve an entity's ByLayer/ByBlock
   * properties. Centralized here because it needs the layer table, which
   * entities deliberately cannot reach.
   */
  resolutionContextFor(entity: Entity, insert?: InsertEntity): ResolutionContext {
    const layer = this.getTyped(entity.layerHandle, Layer) ?? this.defaultLayer;
    const base = {
      layerColor: layer.color,
      layerLinetypeHandle: layer.linetypeHandle,
      layerLineweight: layer.lineweight,
      layerTransparency: layer.transparency,
      layerVisible: layer.isVisible,
      globalLinetypeScale: this.settings.globalLinetypeScale,
    };
    if (insert === undefined) return base;
    return {
      ...base,
      blockColor: insert.color,
      blockLinetypeHandle: insert.linetypeHandle,
      blockLineweight: insert.lineweight,
    };
  }

  // ------------------------------------------------------------------ xrefs

  /** Every block record that is an external reference. */
  get externalReferences(): BlockTableRecord[] {
    return this.blocks.toArray().filter((b) => b.isXref);
  }

  /** Xrefs that failed to resolve, for the reference manager and status bar. */
  get unresolvedReferences(): BlockTableRecord[] {
    return this.externalReferences.filter(
      (b) => b.xrefStatus === XrefStatus.NotFound || b.xrefStatus === XrefStatus.Unresolved,
    );
  }

  // ----------------------------------------------------------------- events

  addListener(listener: DatabaseListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private emit(event: DatabaseEvent): void {
    if (this.bulkLoading) return;
    for (const listener of this.listeners) listener(event);
  }

  /**
   * Run `fn` without emitting per-object events, then emit a single reset.
   * Loading a large drawing would otherwise fire hundreds of thousands of
   * notifications that every listener immediately discards.
   */
  bulkLoad<T>(fn: () => T): T {
    const wasLoading = this.bulkLoading;
    this.bulkLoading = true;
    try {
      return fn();
    } finally {
      this.bulkLoading = wasLoading;
      if (!wasLoading) {
        for (const listener of this.listeners) listener({ kind: 'reset' });
      }
    }
  }

  /** Note a handle seen during load so future allocations avoid it. */
  observeHandle(handle: Handle): void {
    this.allocator.observe(handle);
  }

  /** The handle that would be allocated next, for the file header. */
  peekNextHandle(): Handle {
    return this.allocator.peek();
  }

  // ------------------------------------------------------------ diagnostics

  /**
   * Check referential integrity.
   *
   * Run after loading a file and in tests. Real-world DWGs do contain dangling
   * references, so this reports rather than throws — the drawing must still
   * open — but the report drives what the UI tells the user.
   */
  validate(): DatabaseIssue[] {
    const issues: DatabaseIssue[] = [];
    for (const object of this.objects.values()) {
      if (object.erased) continue;
      for (const ref of object.referencedHandles()) {
        if (ref === NULL_HANDLE) continue;
        if (!this.objects.has(ref)) {
          issues.push({
            severity: 'error',
            handle: object.handle,
            message: `${object.objectType} ${object.handle} references missing object ${ref}`,
          });
        }
      }
      if (object instanceof Entity && object.layerHandle === NULL_HANDLE) {
        issues.push({
          severity: 'warning',
          handle: object.handle,
          message: `${object.objectType} ${object.handle} has no layer assigned`,
        });
      }
    }
    return issues;
  }
}

export interface DatabaseIssue {
  readonly severity: 'error' | 'warning';
  readonly handle: Handle;
  readonly message: string;
}

// Re-exported so callers can construct a default model-space point without
// importing the math package separately.
export { Vec3 };
