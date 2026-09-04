#pragma once

#include <optional>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/render/SpatialIndex.hpp"

namespace cad::render {

struct PickResult {
    Handle handle;
    /// Distance from the pick point to the entity's geometry, in world units.
    double distance = 0.0;
};

/// Find the entity nearest a world point, within `aperture` world units.
///
/// Two stages, and both are necessary. The spatial index narrows a
/// million-entity drawing to a handful of candidates by bounding box; the
/// precise distance test then picks among them, because bounding boxes overlap
/// far more often than geometry does — clicking inside the box of a large
/// diagonal line must not select it when the cursor is nowhere near the line.
///
/// Locked and invisible layers are excluded: a lock the application ignores is
/// worse than no lock, and selecting something you cannot see is never useful.
[[nodiscard]] std::optional<PickResult> pickEntity(Database& db, const SpatialIndex& index,
                                                   const Vec3& world, double aperture);

/// Every entity near a point, nearest first.
///
/// Backs selection cycling: repeated clicks in the same place walk through
/// overlapping entities rather than always selecting the same one.
[[nodiscard]] std::vector<PickResult> pickAll(Database& db, const SpatialIndex& index,
                                              const Vec3& world, double aperture);

/// Entities selected by a rectangular drag.
///
/// `crossing` distinguishes other CAD software's two selection modes, and the distinction
/// is not cosmetic: dragging right-to-left (crossing) catches anything the
/// rectangle touches, left-to-right (window) only what it fully encloses.
/// Drafters rely on this constantly.
[[nodiscard]] std::vector<Handle> pickWindow(Database& db, const SpatialIndex& index,
                                             const Box3& region, bool crossing);

}  // namespace cad::render
