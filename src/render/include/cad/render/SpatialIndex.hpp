#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "cad/db/Handle.hpp"
#include "cad/math/Box3.hpp"

namespace cad::render {

/// A bulk-loaded R-tree over entity bounding boxes.
///
/// Why this exists: with a flat list, every frame and every pick is O(n) over
/// the whole drawing. A real architectural DWG is 100k–1M entities, so a flat
/// scan is a hard ceiling of a few frames per second no matter how fast the
/// drawing code is. The index makes culling and hit testing roughly
/// O(log n + k) in the number of results.
///
/// Why an R-tree rather than a quadtree or uniform grid: CAD geometry is
/// extremely non-uniform. A site plan has a dense building in one corner and
/// empty hectares elsewhere, and entity sizes vary wildly — a hatch covering
/// the sheet next to a 1 mm dimension tick. A grid degenerates badly on both
/// counts; an R-tree adapts to the actual distribution.
///
/// Bulk loading uses Sort-Tile-Recursive packing, which builds a near-optimally
/// packed tree in O(n log n) — far better than n individual insertions, and
/// fast enough that incremental edits can mark the tree dirty and rebuild
/// rather than needing a correct-but-complex delete.
class SpatialIndex {
public:
    explicit SpatialIndex(std::size_t nodeCapacity = 16);
    ~SpatialIndex();

    SpatialIndex(const SpatialIndex&) = delete;
    SpatialIndex& operator=(const SpatialIndex&) = delete;
    SpatialIndex(SpatialIndex&&) noexcept;
    SpatialIndex& operator=(SpatialIndex&&) noexcept;

    struct Item {
        Handle handle;
        Box3 bounds;
    };

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Add or replace an item. The tree is rebuilt lazily on the next query.
    void insert(Handle handle, const Box3& bounds);

    bool remove(Handle handle);

    /// Replace the whole contents.
    void rebuild(const std::vector<Item>& items);

    void clear();

    /// Overall extents of everything indexed.
    [[nodiscard]] Box3 bounds() const;

    /// Handles whose bounds intersect `region`.
    ///
    /// This is the query the renderer runs every frame to cull, and the first
    /// stage of every pick. It reports *candidates*: bounding boxes overlap
    /// more often than geometry does, so callers must still test precisely.
    [[nodiscard]] std::vector<Handle> search(const Box3& region) const;

    /// Handles whose bounds lie entirely within `region` — window selection.
    [[nodiscard]] std::vector<Handle> searchContained(const Box3& region) const;

    /// Handles whose bounds come within `radius` of a point.
    [[nodiscard]] std::vector<Handle> searchNear(const Vec3& point, double radius) const;

    /// The `count` nearest items to a point, nearest first.
    ///
    /// A best-first branch-and-bound walk: the queue always expands the closest
    /// unexplored node, so the search stops as soon as `count` results are
    /// closer than every remaining branch. That is what keeps it independent of
    /// drawing size rather than degenerating into a full scan.
    [[nodiscard]] std::vector<Handle> nearest(
        const Vec3& point, std::size_t count = 1,
        double maxDistance = std::numeric_limits<double>::infinity()) const;

    /// Bounds recorded for a handle, or an empty box when absent.
    [[nodiscard]] Box3 boundsOf(Handle handle) const;

    /// Depth of the built tree, for diagnostics and tests.
    [[nodiscard]] int treeHeight() const;

    /// The tree node. Public only so the packing helpers in the
    /// implementation file can name it; it is not part of the interface.
    struct Node;
    using NodePtr = std::unique_ptr<Node>;

private:
    void ensureBuilt() const;

    std::unordered_map<Handle, Box3> entries_;
    std::size_t nodeCapacity_;

    /// Mutable so const queries can rebuild lazily; the tree is a derived
    /// index, not observable state.
    mutable NodePtr root_;
    mutable bool dirty_ = false;
};

}  // namespace cad::render
