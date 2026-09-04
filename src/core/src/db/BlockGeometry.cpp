#include "cad/db/BlockGeometry.hpp"

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"

namespace cad {
namespace {

void collect(const Database& db, const InsertEntity& insert, const Mat4& parent, int depth,
             std::vector<CurvePtr>& out) {
    if (depth < 0) {
        return;
    }
    const auto* definition = db.get<BlockTableRecord>(insert.blockHandle);
    // A missing or unloaded definition draws nothing, so there is nothing to
    // pick or outline either -- the marker shown in its place is not geometry.
    if (definition == nullptr || definition->isMissingContent()) {
        return;
    }

    for (const Mat4& cell : insert.cellTransforms()) {
        const Mat4 transform = parent.multiply(cell);
        for (const Entity* child : db.entitiesIn(*definition)) {
            if (child == nullptr || child->erased || !child->visible) {
                continue;
            }
            if (const auto* nested = dynamic_cast<const InsertEntity*>(child)) {
                collect(db, *nested, transform, depth - 1, out);
                continue;
            }
            for (const CurvePtr& curve : child->toCurves()) {
                if (curve != nullptr) {
                    out.push_back(curve->transformed(transform));
                }
            }
        }
    }
}

}  // namespace

std::vector<CurvePtr> blockReferenceCurves(const Database& db, const InsertEntity& insert,
                                           int maxDepth) {
    std::vector<CurvePtr> curves;
    collect(db, insert, Mat4::identity(), maxDepth, curves);
    return curves;
}

}  // namespace cad
