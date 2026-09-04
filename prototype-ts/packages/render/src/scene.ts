import {
  Box3,
  Database,
  Entity,
  InsertEntity,
  Layer,
  Mat4,
  BlockTableRecord,
  Vec3,
  ViewportEntity,
  type Handle,
  type ResolvedProperties,
} from '@cad/core';
import { SpatialIndex } from './spatial-index.js';
import type { Viewport } from './viewport.js';

/**
 * One entity ready to draw: its geometry transform and resolved appearance.
 *
 * The renderer consumes these and never touches the database, which keeps the
 * document authoritative and means the same draw list can be produced once and
 * rendered to a canvas, an SVG export or a plot without re-resolving anything.
 */
export interface DrawItem {
  readonly entity: Entity;
  readonly handle: Handle;
  /** World transform, accumulated through any enclosing block references. */
  readonly transform: Mat4;
  readonly properties: ResolvedProperties;
  /** Nesting depth; 0 for entities directly in the space being drawn. */
  readonly depth: number;
  /** The block reference this came through, when nested. */
  readonly viaInsert: InsertEntity | null;
}

export interface SceneStats {
  readonly totalEntities: number;
  readonly visibleEntities: number;
  readonly culledEntities: number;
  readonly buildMs: number;
}

export interface SceneOptions {
  /** Skip entities smaller than this many pixels. */
  readonly minPixelSize?: number;
  /** Maximum block nesting depth to expand. */
  readonly maxDepth?: number;
  /** Viewport context, when rendering a paper-space sheet. */
  readonly paperViewport?: ViewportEntity;
  /** Cap on items produced, so a pathological drawing cannot stall a frame. */
  readonly maxItems?: number;
}

/**
 * Builds the list of things to draw for a given view.
 *
 * This is where the performance of large drawings is won or lost. Three
 * mechanisms, in the order they matter:
 *
 *  1. **Spatial culling.** The index is queried for the visible region rather
 *     than iterating the drawing. On a 500k-entity site plan zoomed into one
 *     room, this alone is the difference between 2 fps and 60.
 *  2. **Layer filtering.** Frozen and off layers are skipped before any
 *     geometry work happens, including per-viewport freezes in paper space.
 *  3. **Level of detail.** Entities smaller than a pixel are dropped. Nothing
 *     is lost visually, and on a drawing full of small blocks — bolts, tiles,
 *     hatching — it removes most of the work at low zoom.
 *
 * The scene is rebuilt per frame rather than cached and patched. That is a
 * deliberate simplicity/performance trade: rebuilding is O(visible) and the
 * visible set is bounded by the screen, whereas an incrementally maintained
 * cache must handle every kind of database change correctly and is a reliable
 * source of stale-render bugs.
 */
export class SceneBuilder {
  private readonly db: Database;
  private readonly index: SpatialIndex;

  /** Cached layer lookups, rebuilt when the database changes. */
  private layerCache = new Map<Handle, Layer>();

  private lastStats: SceneStats = {
    totalEntities: 0,
    visibleEntities: 0,
    culledEntities: 0,
    buildMs: 0,
  };

  constructor(db: Database, index: SpatialIndex) {
    this.db = db;
    this.index = index;
  }

  get stats(): SceneStats {
    return this.lastStats;
  }

  /** Drop cached lookups. Called when the database changes structurally. */
  invalidate(): void {
    this.layerCache = new Map();
  }

  /**
   * Rebuild the spatial index from a block record's contents.
   *
   * Only the top level is indexed; a block reference is indexed by its overall
   * extents rather than its contents. That keeps the index small — a drawing
   * with 10,000 references to a 500-entity block indexes 10,000 items, not
   * 5,000,000 — and the contents are expanded only for references that survive
   * culling.
   */
  reindex(space: BlockTableRecord): void {
    const items: Array<{ handle: Handle; bounds: Box3 }> = [];
    for (const entity of this.db.entitiesIn(space)) {
      if (entity instanceof InsertEntity) {
        const definition = this.db.getTyped(entity.blockHandle, BlockTableRecord);
        if (definition !== undefined) {
          entity.setBlockExtents(this.db.blockExtents(definition));
        }
      }
      const bounds = entity.bounds;
      if (!bounds.isEmpty) items.push({ handle: entity.handle, bounds });
    }
    this.index.rebuild(items);
  }

  /**
   * Build the draw list for a view of a space.
   */
  build(space: BlockTableRecord, viewport: Viewport, options: SceneOptions = {}): DrawItem[] {
    const started = now();
    const visible = viewport.visibleBounds;
    const minPixelSize = options.minPixelSize ?? 1;
    const minWorldSize = viewport.pixelsToWorld(minPixelSize);
    const maxItems = options.maxItems ?? 500_000;

    const items: DrawItem[] = [];
    let culled = 0;

    const candidates = this.index.search(visible);
    const total = this.index.size;

    for (const handle of candidates) {
      if (items.length >= maxItems) break;
      const entity = this.db.getEntity(handle);
      if (entity === undefined || entity.erased) continue;

      const layer = this.layerFor(entity.layerHandle);
      if (!this.isLayerVisible(layer, options.paperViewport)) {
        culled++;
        continue;
      }

      // Level of detail: an entity smaller than a pixel contributes nothing.
      const bounds = entity.bounds;
      if (!bounds.isEmpty && bounds.diagonal < minWorldSize && !(entity instanceof InsertEntity)) {
        culled++;
        continue;
      }

      if (entity instanceof InsertEntity) {
        this.expandInsert(entity, Mat4.IDENTITY, viewport, options, items, 0);
      } else {
        items.push({
          entity,
          handle: entity.handle,
          transform: Mat4.IDENTITY,
          properties: entity.resolveProperties(this.db.resolutionContextFor(entity)),
          depth: 0,
          viaInsert: null,
        });
      }
    }

    this.lastStats = {
      totalEntities: total,
      visibleEntities: items.length,
      culledEntities: culled,
      buildMs: now() - started,
    };
    return items;
  }

  /**
   * Expand a block reference into draw items for its contents.
   *
   * The reference's own extents are checked against the view before any
   * contents are touched, so an off-screen block costs one box test rather
   * than a full traversal.
   */
  private expandInsert(
    insert: InsertEntity,
    parentTransform: Mat4,
    viewport: Viewport,
    options: SceneOptions,
    output: DrawItem[],
    depth: number,
  ): void {
    const maxDepth = options.maxDepth ?? 16;
    if (depth > maxDepth) return;

    const definition = this.db.getTyped(insert.blockHandle, BlockTableRecord);
    if (definition === undefined) return;

    // A reference whose content could not be loaded still draws a marker, so
    // a missing xref is visible rather than silently absent.
    if (definition.isMissingContent) {
      output.push({
        entity: insert,
        handle: insert.handle,
        transform: parentTransform.multiply(insert.blockTransform),
        properties: insert.resolveProperties(this.db.resolutionContextFor(insert)),
        depth,
        viaInsert: null,
      });
      return;
    }

    const visible = viewport.visibleBounds;
    const minWorldSize = viewport.pixelsToWorld(options.minPixelSize ?? 1);

    for (const cell of insert.cellTransforms()) {
      const transform = parentTransform.multiply(cell);

      // Cull the whole cell if its extents fall outside the view.
      const cellBounds = insert.blockExtents?.transform(transform);
      if (cellBounds !== undefined && !cellBounds.isEmpty) {
        if (!cellBounds.intersects2(visible)) continue;
        // An entire block smaller than a pixel is dropped whole, which is the
        // single biggest saving on drawings full of small repeated symbols.
        if (cellBounds.diagonal < minWorldSize) continue;
      }

      for (const child of this.db.entitiesIn(definition)) {
        if (output.length >= (options.maxItems ?? 500_000)) return;

        const layer = this.layerFor(child.layerHandle);
        if (!this.isLayerVisible(layer, options.paperViewport)) continue;

        if (child instanceof InsertEntity) {
          this.expandInsert(child, transform, viewport, options, output, depth + 1);
          continue;
        }

        const childBounds = child.bounds.transform(transform);
        if (!childBounds.isEmpty && !childBounds.intersects2(visible)) continue;

        output.push({
          entity: child,
          handle: child.handle,
          transform,
          // Properties resolve against the *reference*, so a ByBlock entity
          // takes its colour from the insert rather than from the definition.
          properties: child.resolveProperties(this.db.resolutionContextFor(child, insert)),
          depth: depth + 1,
          viaInsert: insert,
        });
      }
    }
  }

  private layerFor(handle: Handle): Layer | undefined {
    const cached = this.layerCache.get(handle);
    if (cached !== undefined) return cached;
    const layer = this.db.getTyped(handle, Layer);
    if (layer !== undefined) this.layerCache.set(handle, layer);
    return layer;
  }

  /**
   * Layer visibility, including the per-viewport freeze that makes paper space
   * useful — the same model layer can show in one viewport and not another.
   */
  private isLayerVisible(layer: Layer | undefined, paperViewport?: ViewportEntity): boolean {
    if (layer === undefined) return true;
    if (!layer.isVisible) return false;
    if (paperViewport !== undefined && paperViewport.isLayerFrozen(layer.handle)) return false;
    return true;
  }
}

/** Monotonic clock, falling back where `performance` is unavailable. */
function now(): number {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

/**
 * Extents of a space, for ZOOM EXTENTS.
 *
 * Uses the index when it is populated, since that is already the union of
 * every visible entity's bounds and avoids a second traversal.
 */
export function spaceExtents(db: Database, space: BlockTableRecord, index?: SpatialIndex): Box3 {
  if (index !== undefined && index.size > 0) return index.bounds;
  return db.blockExtents(space);
}

/** A view centred on nothing in particular, for an empty drawing. */
export const DEFAULT_EXTENTS = new Box3(new Vec3(0, 0, 0), new Vec3(420, 297, 0));
