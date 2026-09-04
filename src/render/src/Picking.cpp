#include "cad/render/Picking.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "cad/db/BlockGeometry.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/geom/Intersect.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad::render {
namespace {

/// True when the entity may be selected: present, visible, and not on a locked
/// layer.
[[nodiscard]] bool isSelectable(Database& db, const Entity& entity) {
    if (entity.erased || !entity.visible) {
        return false;
    }
    const auto* layer = db.get<Layer>(entity.layerHandle);
    return layer == nullptr || layer->isEditable();
}

}  // namespace

std::vector<PickResult> pickAll(Database& db, const SpatialIndex& index, const Vec3& world,
                                double aperture) {
    std::vector<PickResult> results;

    // Stage one: the index narrows the drawing to candidates by bounding box.
    for (const Handle handle : index.searchNear(world, aperture)) {
        Entity* entity = db.getEntity(handle);
        if (entity == nullptr || !isSelectable(db, *entity)) {
            continue;
        }
        // Stage two: the precise test. Bounding boxes overlap far more often
        // than geometry does, so this is what stops a click inside the box of a
        // large diagonal line selecting it.
        //
        // A block reference cannot answer this for itself -- it knows its
        // extents and not its contents -- so its own answer is "anywhere in the
        // box", and a click in the empty middle of a room would take whichever
        // piece of furniture had the room inside its box. Expanding it here,
        // where the database is in hand, is what makes clicking a block mean
        // clicking the lines it draws, as it does in other CAD software.
        double distance = entity->distanceToPoint(world);
        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
            if (distance > aperture) {
                continue;
            }
            distance = std::numeric_limits<double>::infinity();
            for (const CurvePtr& curve : blockReferenceCurves(db, *insert)) {
                distance = std::min(distance, curve->distanceTo(world));
            }
        }
        if (distance <= aperture) {
            results.push_back({handle, distance});
        }
    }

    std::sort(results.begin(), results.end(),
              [](const PickResult& a, const PickResult& b) { return a.distance < b.distance; });
    return results;
}

std::optional<PickResult> pickEntity(Database& db, const SpatialIndex& index, const Vec3& world,
                                     double aperture) {
    const std::vector<PickResult> all = pickAll(db, index, world, aperture);
    if (all.empty()) {
        return std::nullopt;
    }
    return all.front();
}

std::vector<Handle> pickWindow(Database& db, const SpatialIndex& index, const Box3& region,
                               bool crossing) {
    std::vector<Handle> results;
    if (region.isEmpty()) {
        return results;
    }

    // Window selection can be answered from bounding boxes alone: an entity
    // fully inside the region by box is fully inside by geometry. Crossing
    // cannot, so its box hits are candidates needing a precise test.
    const std::vector<Handle> candidates =
        crossing ? index.search(region) : index.searchContained(region);

    for (const Handle handle : candidates) {
        Entity* entity = db.getEntity(handle);
        if (entity == nullptr || !isSelectable(db, *entity)) {
            continue;
        }

        if (!crossing) {
            results.push_back(handle);
            continue;
        }

        // An entity whose box straddles the region may still not touch it —
        // a long diagonal line passing by a corner, for instance.
        if (region.containsBox(entity->bounds())) {
            results.push_back(handle);
            continue;
        }

        bool touches = false;
        const std::array<Vec3, 4> corners = region.corners2();
        for (const CurvePtr& curve : entity->toCurves()) {
            // Cheap accept: any tessellated vertex inside the region.
            for (const Vec3& point : curve->tessellate()) {
                if (region.containsPoint(point)) {
                    touches = true;
                    break;
                }
            }
            if (touches) {
                break;
            }
            // Otherwise the curve may still cross an edge without any vertex
            // landing inside — a line passing straight through the rectangle.
            for (std::size_t i = 0; i < corners.size() && !touches; ++i) {
                const LineSegment edge{corners[i], corners[(i + 1) % corners.size()]};
                if (!intersectCurves(*curve, edge).empty()) {
                    touches = true;
                }
            }
            if (touches) {
                break;
            }
        }

        // Entities with no curve geometry (text, block references) are judged
        // by their box, which is also how they are drawn and picked.
        if (touches || entity->toCurves().empty()) {
            results.push_back(handle);
        }
    }
    return results;
}

}  // namespace cad::render
