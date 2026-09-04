#include "cad/render/SpatialIndex.hpp"

#include <algorithm>
#include <cmath>

namespace cad::render {
namespace {

struct Entry {
    Handle handle;
    Box3 bounds;
};

[[nodiscard]] double centerX(const Box3& box) noexcept {
    return (box.min.x + box.max.x) / 2.0;
}

[[nodiscard]] double centerY(const Box3& box) noexcept {
    return (box.min.y + box.max.y) / 2.0;
}

}  // namespace

/// A node in the R-tree. Leaves hold entries; branches hold child nodes.
struct SpatialIndex::Node {
    Box3 bounds;
    std::vector<Entry> entries;
    std::vector<NodePtr> children;
    int height = 1;

    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
};

namespace {

using Node = SpatialIndex::Node;
using NodePtr = std::unique_ptr<Node>;

[[nodiscard]] NodePtr makeLeaf(std::vector<Entry> entries) {
    auto node = std::make_unique<Node>();
    for (const Entry& entry : entries) {
        node->bounds = node->bounds.united(entry.bounds);
    }
    node->entries = std::move(entries);
    node->height = 1;
    return node;
}

[[nodiscard]] NodePtr makeBranch(std::vector<NodePtr> children) {
    auto node = std::make_unique<Node>();
    for (const NodePtr& child : children) {
        node->bounds = node->bounds.united(child->bounds);
    }
    node->height = children.empty() ? 1 : children.front()->height + 1;
    node->children = std::move(children);
    return node;
}

/// Sort-Tile-Recursive packing of leaves.
///
/// Entries are sorted by X and cut into vertical slices, then each slice is
/// sorted by Y and cut into leaves. The result has minimal node overlap, which
/// is what determines query performance — an R-tree with heavily overlapping
/// siblings degenerates toward a linear scan.
[[nodiscard]] std::vector<NodePtr> packLeaves(std::vector<Entry> entries,
                                              std::size_t capacity) {
    const std::size_t leafCount = (entries.size() + capacity - 1) / capacity;
    // A near-square grid minimizes the aspect ratio, and therefore the overlap,
    // of the resulting nodes.
    const auto sliceCount = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(leafCount))));
    const std::size_t perSlice = (entries.size() + sliceCount - 1) / sliceCount;

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return centerX(a.bounds) < centerX(b.bounds);
    });

    std::vector<NodePtr> leaves;
    for (std::size_t i = 0; i < entries.size(); i += perSlice) {
        const std::size_t sliceEnd = std::min(i + perSlice, entries.size());
        std::sort(entries.begin() + static_cast<std::ptrdiff_t>(i),
                  entries.begin() + static_cast<std::ptrdiff_t>(sliceEnd),
                  [](const Entry& a, const Entry& b) {
                      return centerY(a.bounds) < centerY(b.bounds);
                  });

        for (std::size_t j = i; j < sliceEnd; j += capacity) {
            const std::size_t leafEnd = std::min(j + capacity, sliceEnd);
            leaves.push_back(makeLeaf(std::vector<Entry>{
                entries.begin() + static_cast<std::ptrdiff_t>(j),
                entries.begin() + static_cast<std::ptrdiff_t>(leafEnd)}));
        }
    }
    return leaves;
}

/// The same packing, one level up.
[[nodiscard]] std::vector<NodePtr> packNodes(std::vector<NodePtr> nodes, std::size_t capacity) {
    const std::size_t parentCount = (nodes.size() + capacity - 1) / capacity;
    const auto sliceCount = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(parentCount))));
    const std::size_t perSlice = (nodes.size() + sliceCount - 1) / sliceCount;

    std::sort(nodes.begin(), nodes.end(), [](const NodePtr& a, const NodePtr& b) {
        return centerX(a->bounds) < centerX(b->bounds);
    });

    std::vector<NodePtr> parents;
    for (std::size_t i = 0; i < nodes.size(); i += perSlice) {
        const std::size_t sliceEnd = std::min(i + perSlice, nodes.size());
        std::sort(nodes.begin() + static_cast<std::ptrdiff_t>(i),
                  nodes.begin() + static_cast<std::ptrdiff_t>(sliceEnd),
                  [](const NodePtr& a, const NodePtr& b) {
                      return centerY(a->bounds) < centerY(b->bounds);
                  });

        for (std::size_t j = i; j < sliceEnd; j += capacity) {
            const std::size_t groupEnd = std::min(j + capacity, sliceEnd);
            std::vector<NodePtr> group;
            group.reserve(groupEnd - j);
            for (std::size_t k = j; k < groupEnd; ++k) {
                group.push_back(std::move(nodes[k]));
            }
            parents.push_back(makeBranch(std::move(group)));
        }
    }
    return parents;
}

[[nodiscard]] NodePtr buildTree(std::vector<Entry> entries, std::size_t capacity) {
    if (entries.empty()) {
        return nullptr;
    }
    if (entries.size() <= capacity) {
        return makeLeaf(std::move(entries));
    }

    std::vector<NodePtr> level = packLeaves(std::move(entries), capacity);
    while (level.size() > 1) {
        level = packNodes(std::move(level), capacity);
    }
    return std::move(level.front());
}

}  // namespace

SpatialIndex::SpatialIndex(std::size_t nodeCapacity)
    : nodeCapacity_(std::max<std::size_t>(4, nodeCapacity)) {}

SpatialIndex::~SpatialIndex() = default;
SpatialIndex::SpatialIndex(SpatialIndex&&) noexcept = default;
SpatialIndex& SpatialIndex::operator=(SpatialIndex&&) noexcept = default;

void SpatialIndex::insert(Handle handle, const Box3& bounds) {
    if (bounds.isEmpty()) {
        // An entity with no extent cannot be found by a spatial query, and
        // indexing an empty box would corrupt every ancestor's bounds.
        entries_.erase(handle);
    } else {
        entries_[handle] = bounds;
    }
    dirty_ = true;
}

bool SpatialIndex::remove(Handle handle) {
    const bool removed = entries_.erase(handle) > 0;
    if (removed) {
        dirty_ = true;
    }
    return removed;
}

void SpatialIndex::rebuild(const std::vector<Item>& items) {
    entries_.clear();
    for (const Item& item : items) {
        if (!item.bounds.isEmpty()) {
            entries_[item.handle] = item.bounds;
        }
    }
    dirty_ = true;
}

void SpatialIndex::clear() {
    entries_.clear();
    root_.reset();
    dirty_ = false;
}

void SpatialIndex::ensureBuilt() const {
    if (!dirty_ && root_ != nullptr) {
        return;
    }
    std::vector<Entry> all;
    all.reserve(entries_.size());
    for (const auto& [handle, bounds] : entries_) {
        all.push_back({handle, bounds});
    }
    root_ = buildTree(std::move(all), nodeCapacity_);
    dirty_ = false;
}

Box3 SpatialIndex::bounds() const {
    ensureBuilt();
    return root_ == nullptr ? Box3::empty() : root_->bounds;
}

std::vector<Handle> SpatialIndex::search(const Box3& region) const {
    ensureBuilt();
    std::vector<Handle> results;
    if (root_ == nullptr || region.isEmpty()) {
        return results;
    }

    std::vector<const Node*> stack{root_.get()};
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (!node->bounds.intersects2(region)) {
            continue;
        }
        if (node->isLeaf()) {
            for (const Entry& entry : node->entries) {
                if (entry.bounds.intersects2(region)) {
                    results.push_back(entry.handle);
                }
            }
        } else {
            for (const NodePtr& child : node->children) {
                stack.push_back(child.get());
            }
        }
    }
    return results;
}

std::vector<Handle> SpatialIndex::searchContained(const Box3& region) const {
    ensureBuilt();
    std::vector<Handle> results;
    if (root_ == nullptr || region.isEmpty()) {
        return results;
    }

    std::vector<const Node*> stack{root_.get()};
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (!node->bounds.intersects2(region)) {
            continue;
        }
        if (node->isLeaf()) {
            for (const Entry& entry : node->entries) {
                if (region.containsBox(entry.bounds)) {
                    results.push_back(entry.handle);
                }
            }
        } else {
            for (const NodePtr& child : node->children) {
                stack.push_back(child.get());
            }
        }
    }
    return results;
}

std::vector<Handle> SpatialIndex::searchNear(const Vec3& point, double radius) const {
    const Vec3 extent{radius, radius, radius};
    return search(Box3{point - extent, point + extent});
}

std::vector<Handle> SpatialIndex::nearest(const Vec3& point, std::size_t count,
                                          double maxDistance) const {
    ensureBuilt();
    if (root_ == nullptr) {
        return {};
    }

    struct Candidate {
        double distance;
        const Node* node;
        const Entry* entry;
    };

    std::vector<Candidate> queue{
        {root_->bounds.distanceToPoint(point), root_.get(), nullptr}};
    std::vector<Handle> results;

    while (!queue.empty() && results.size() < count) {
        // A linear scan for the minimum beats a heap at the queue lengths this
        // search produces, and avoids the heap's allocation churn.
        auto best = queue.begin();
        for (auto it = queue.begin() + 1; it != queue.end(); ++it) {
            if (it->distance < best->distance) {
                best = it;
            }
        }
        const Candidate item = *best;
        queue.erase(best);

        if (item.distance > maxDistance) {
            break;
        }
        if (item.entry != nullptr) {
            results.push_back(item.entry->handle);
            continue;
        }

        if (item.node->isLeaf()) {
            for (const Entry& entry : item.node->entries) {
                queue.push_back({entry.bounds.distanceToPoint(point), nullptr, &entry});
            }
        } else {
            for (const NodePtr& child : item.node->children) {
                queue.push_back({child->bounds.distanceToPoint(point), child.get(), nullptr});
            }
        }
    }
    return results;
}

Box3 SpatialIndex::boundsOf(Handle handle) const {
    const auto it = entries_.find(handle);
    return it == entries_.end() ? Box3::empty() : it->second;
}

int SpatialIndex::treeHeight() const {
    ensureBuilt();
    return root_ == nullptr ? 0 : root_->height;
}

}  // namespace cad::render
