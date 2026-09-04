import { Box3, Vec3, type Handle } from '@cad/core';

/** One indexed item. */
interface IndexEntry {
  readonly handle: Handle;
  readonly bounds: Box3;
}

/** A node in the R-tree. Leaves hold entries; branches hold child nodes. */
interface RTreeNode {
  bounds: Box3;
  /** Entries, when this is a leaf. */
  entries: IndexEntry[] | null;
  /** Children, when this is a branch. */
  children: RTreeNode[] | null;
  height: number;
}

/**
 * A bulk-loaded R-tree over entity bounding boxes.
 *
 * Why this exists: with a flat list, every frame and every pick is O(n) over
 * the whole drawing. A real architectural DWG has 100k–1M entities, so a flat
 * scan is a hard ceiling of a few frames per second no matter how fast the
 * drawing code is. The index turns culling and hit testing into roughly
 * O(log n + k) in the number of results.
 *
 * Why an R-tree rather than a quadtree or a uniform grid: CAD geometry is
 * extremely non-uniform. A site plan has a dense building in one corner and
 * empty hectares elsewhere, and entities span wildly different sizes — a
 * hatch covering the whole sheet next to a 1 mm dimension tick. A grid
 * degenerates badly on both counts; an R-tree adapts to the actual
 * distribution.
 *
 * Bulk loading uses Sort-Tile-Recursive packing, which builds a
 * near-optimally packed tree in O(n log n) — far better than n individual
 * insertions, and rebuilding is fast enough that incremental edits can simply
 * mark the tree dirty rather than needing a correct-but-complex delete.
 */
export class SpatialIndex {
  private root: RTreeNode | null = null;

  /** Live entries by handle, so updates and removals are O(1) to record. */
  private readonly entries = new Map<Handle, IndexEntry>();

  /** `true` when the tree no longer reflects {@link entries}. */
  private dirty = false;

  /** Maximum entries per leaf. */
  private readonly nodeCapacity: number;

  constructor(nodeCapacity = 16) {
    this.nodeCapacity = Math.max(4, nodeCapacity);
  }

  get size(): number {
    return this.entries.size;
  }

  get isDirty(): boolean {
    return this.dirty;
  }

  /** Add or replace an item. The tree is rebuilt lazily on the next query. */
  insert(handle: Handle, bounds: Box3): void {
    if (bounds.isEmpty) {
      // An entity with no extent cannot be found by a spatial query, and
      // indexing an empty box would corrupt every ancestor's bounds.
      this.entries.delete(handle);
      this.dirty = true;
      return;
    }
    this.entries.set(handle, { handle, bounds });
    this.dirty = true;
  }

  remove(handle: Handle): boolean {
    const removed = this.entries.delete(handle);
    if (removed) this.dirty = true;
    return removed;
  }

  /** Replace the whole contents. */
  rebuild(items: Iterable<{ handle: Handle; bounds: Box3 }>): void {
    this.entries.clear();
    for (const item of items) {
      if (item.bounds.isEmpty) continue;
      this.entries.set(item.handle, { handle: item.handle, bounds: item.bounds });
    }
    this.dirty = true;
  }

  clear(): void {
    this.entries.clear();
    this.root = null;
    this.dirty = false;
  }

  /** Rebuild the tree if needed. Called automatically by every query. */
  private ensureBuilt(): void {
    if (!this.dirty && this.root !== null) return;
    const all = [...this.entries.values()];
    this.root = all.length === 0 ? null : buildTree(all, this.nodeCapacity);
    this.dirty = false;
  }

  /** Overall extents of everything indexed. */
  get bounds(): Box3 {
    this.ensureBuilt();
    return this.root?.bounds ?? Box3.EMPTY;
  }

  /**
   * Handles whose bounds intersect `region`.
   *
   * This is the query the renderer runs every frame to cull, and the first
   * stage of every pick. It reports *candidates*: bounding boxes overlap more
   * often than geometry does, so callers must still test precisely.
   */
  search(region: Box3): Handle[] {
    this.ensureBuilt();
    const results: Handle[] = [];
    if (this.root === null || region.isEmpty) return results;

    const stack: RTreeNode[] = [this.root];
    while (stack.length > 0) {
      const node = stack.pop()!;
      if (!node.bounds.intersects2(region)) continue;

      if (node.entries !== null) {
        for (const entry of node.entries) {
          if (entry.bounds.intersects2(region)) results.push(entry.handle);
        }
      } else if (node.children !== null) {
        for (const child of node.children) stack.push(child);
      }
    }
    return results;
  }

  /** Handles whose bounds lie entirely within `region` — window selection. */
  searchContained(region: Box3): Handle[] {
    this.ensureBuilt();
    const results: Handle[] = [];
    if (this.root === null || region.isEmpty) return results;

    const stack: RTreeNode[] = [this.root];
    while (stack.length > 0) {
      const node = stack.pop()!;
      if (!node.bounds.intersects2(region)) continue;

      if (node.entries !== null) {
        for (const entry of node.entries) {
          if (region.containsBox(entry.bounds)) results.push(entry.handle);
        }
      } else if (node.children !== null) {
        for (const child of node.children) stack.push(child);
      }
    }
    return results;
  }

  /** Handles whose bounds contain or come within `radius` of a point. */
  searchNear(point: Vec3, radius: number): Handle[] {
    const region = new Box3(
      new Vec3(point.x - radius, point.y - radius, point.z - radius),
      new Vec3(point.x + radius, point.y + radius, point.z + radius),
    );
    return this.search(region);
  }

  /**
   * The `count` nearest items to a point, nearest first.
   *
   * A best-first branch-and-bound walk: the priority queue always expands the
   * closest unexplored node, so the search stops as soon as `count` results
   * are closer than every remaining branch. That is what keeps it independent
   * of the drawing size rather than degenerating into a full scan.
   */
  nearest(point: Vec3, count = 1, maxDistance = Infinity): Handle[] {
    this.ensureBuilt();
    if (this.root === null) return [];

    const queue: Array<{ distance: number; node: RTreeNode | null; entry: IndexEntry | null }> =
      [{ distance: this.root.bounds.distanceToPoint(point), node: this.root, entry: null }];
    const results: Handle[] = [];

    while (queue.length > 0 && results.length < count) {
      // A linear scan for the minimum is faster than a heap at the queue
      // lengths this search produces, and avoids the heap's allocation churn.
      let bestIndex = 0;
      for (let i = 1; i < queue.length; i++) {
        if (queue[i]!.distance < queue[bestIndex]!.distance) bestIndex = i;
      }
      const item = queue.splice(bestIndex, 1)[0]!;
      if (item.distance > maxDistance) break;

      if (item.entry !== null) {
        results.push(item.entry.handle);
        continue;
      }

      const node = item.node!;
      if (node.entries !== null) {
        for (const entry of node.entries) {
          queue.push({
            distance: entry.bounds.distanceToPoint(point),
            node: null,
            entry,
          });
        }
      } else if (node.children !== null) {
        for (const child of node.children) {
          queue.push({
            distance: child.bounds.distanceToPoint(point),
            node: child,
            entry: null,
          });
        }
      }
    }
    return results;
  }

  /** Every indexed handle. */
  handles(): IterableIterator<Handle> {
    return this.entries.keys();
  }

  /** Bounds recorded for a handle. */
  boundsOf(handle: Handle): Box3 | undefined {
    return this.entries.get(handle)?.bounds;
  }

  /** Depth of the built tree, for diagnostics and tests. */
  get treeHeight(): number {
    this.ensureBuilt();
    return this.root?.height ?? 0;
  }
}

/**
 * Build a packed R-tree by Sort-Tile-Recursive bulk loading.
 *
 * The entries are sorted by X and cut into vertical slices, then each slice is
 * sorted by Y and cut into leaves. The result is a tree whose nodes have
 * minimal overlap, which is what determines query performance — an R-tree with
 * heavily overlapping siblings degenerates toward a linear scan.
 */
function buildTree(entries: IndexEntry[], capacity: number): RTreeNode {
  if (entries.length <= capacity) {
    return makeLeaf(entries);
  }

  const leaves = packLeaves(entries, capacity);
  let level: RTreeNode[] = leaves;
  let height = 1;

  // Repeatedly pack the previous level into parents until one root remains.
  while (level.length > 1) {
    level = packNodes(level, capacity);
    height++;
  }
  const root = level[0]!;
  root.height = height;
  return root;
}

function packLeaves(entries: IndexEntry[], capacity: number): RTreeNode[] {
  const leafCount = Math.ceil(entries.length / capacity);
  // Number of vertical slices that makes the grid as square as possible,
  // which minimizes the aspect ratio — and therefore the overlap — of nodes.
  const sliceCount = Math.ceil(Math.sqrt(leafCount));
  const perSlice = Math.ceil(entries.length / sliceCount);

  const byX = [...entries].sort((a, b) => centerX(a.bounds) - centerX(b.bounds));
  const leaves: RTreeNode[] = [];

  for (let i = 0; i < byX.length; i += perSlice) {
    const slice = byX.slice(i, i + perSlice);
    slice.sort((a, b) => centerY(a.bounds) - centerY(b.bounds));
    for (let j = 0; j < slice.length; j += capacity) {
      leaves.push(makeLeaf(slice.slice(j, j + capacity)));
    }
  }
  return leaves;
}

function packNodes(nodes: RTreeNode[], capacity: number): RTreeNode[] {
  const parentCount = Math.ceil(nodes.length / capacity);
  const sliceCount = Math.ceil(Math.sqrt(parentCount));
  const perSlice = Math.ceil(nodes.length / sliceCount);

  const byX = [...nodes].sort((a, b) => centerX(a.bounds) - centerX(b.bounds));
  const parents: RTreeNode[] = [];

  for (let i = 0; i < byX.length; i += perSlice) {
    const slice = byX.slice(i, i + perSlice);
    slice.sort((a, b) => centerY(a.bounds) - centerY(b.bounds));
    for (let j = 0; j < slice.length; j += capacity) {
      parents.push(makeBranch(slice.slice(j, j + capacity)));
    }
  }
  return parents;
}

function makeLeaf(entries: IndexEntry[]): RTreeNode {
  return {
    bounds: Box3.union(entries.map((e) => e.bounds)),
    entries,
    children: null,
    height: 1,
  };
}

function makeBranch(children: RTreeNode[]): RTreeNode {
  return {
    bounds: Box3.union(children.map((c) => c.bounds)),
    entries: null,
    children,
    height: (children[0]?.height ?? 0) + 1,
  };
}

function centerX(box: Box3): number {
  return (box.min.x + box.max.x) / 2;
}

function centerY(box: Box3): number {
  return (box.min.y + box.max.y) / 2;
}
