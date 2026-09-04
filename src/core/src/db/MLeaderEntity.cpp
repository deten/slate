#include "cad/db/MLeaderEntity.hpp"

#include <stdexcept>

#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

/// The same proportions a dimension's arrow uses, for the same reason: an
/// arrowhead of a different shape from the ones beside it looks like a mistake.
constexpr double kArrowHalfWidthRatio = 1.0 / 6.0;
constexpr double kDegenerate = 1e-12;

[[nodiscard]] Vec3 perpendicular(const Vec3& direction, const Vec3& normal) {
    const Vec3 perp = normal.cross(direction);
    return perp.lengthSquared() < kDegenerate ? direction.perp2().normalized()
                                              : perp.normalized();
}

}  // namespace

std::vector<std::pair<Vec3, Vec3>> MLeaderEntity::segments() const {
    std::vector<std::pair<Vec3, Vec3>> out;
    for (const LeaderRun& leader : leaders) {
        for (std::size_t i = 0; i + 1 < leader.vertices.size(); ++i) {
            out.emplace_back(leader.vertices[i], leader.vertices[i + 1]);
        }
        if (leader.hasLanding && !leader.vertices.empty()) {
            out.emplace_back(leader.vertices.back(), leader.landingEnd);
        }
    }
    return out;
}

std::vector<std::array<Vec3, 3>> MLeaderEntity::arrowheads() const {
    std::vector<std::array<Vec3, 3>> heads;
    if (!hasArrowheads || arrowSize <= 0.0) {
        return heads;
    }
    for (const LeaderRun& leader : leaders) {
        if (leader.vertices.size() < 2) {
            continue;
        }
        const Vec3 tip = leader.vertices.front();
        const Vec3 shaft = leader.vertices[1] - tip;
        if (shaft.lengthSquared() < kDegenerate) {
            continue;
        }
        const Vec3 direction = shaft.normalized();
        const Vec3 back = tip + direction * arrowSize;
        const Vec3 side = perpendicular(direction, normal) * (arrowSize * kArrowHalfWidthRatio);
        heads.push_back({tip, back + side, back - side});
    }
    return heads;
}

Box3 MLeaderEntity::bounds() const {
    Box3 box = Box3::empty();
    for (const LeaderRun& leader : leaders) {
        for (const Vec3& vertex : leader.vertices) {
            box.expandToPoint(vertex);
        }
        if (leader.hasLanding) {
            box.expandToPoint(leader.landingEnd);
        }
    }
    if (!text.empty()) {
        box.expandToPoint(textLocation);
    }
    return box;
}

std::vector<CurvePtr> MLeaderEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    for (const auto& [from, to] : segments()) {
        curves.push_back(std::make_unique<LineSegment>(from, to));
    }
    return curves;
}

void MLeaderEntity::transformBy(const Mat4& /*m*/) {
    // Nothing, deliberately. This entity is written back from preserved bytes
    // that a transform cannot reach -- its points sit at three nesting depths
    // under repeated group codes -- so moving what is on the screen would leave
    // the file saying something else. `canTransform` is false, and the commands
    // that would move it say what they skipped instead.
}

DbObjectPtr MLeaderEntity::clone() const {
    auto copy = std::make_unique<MLeaderEntity>();
    copyEntityInto(*copy);
    copy->leaders = leaders;
    copy->text = text;
    copy->textLocation = textLocation;
    copy->textHeight = textHeight;
    copy->textRotation = textRotation;
    copy->attachment = attachment;
    copy->arrowSize = arrowSize;
    copy->hasArrowheads = hasArrowheads;
    return copy;
}

void MLeaderEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const MLeaderEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a multileader");
    }
    restoreEntityFrom(*source);
    leaders = source->leaders;
    text = source->text;
    textLocation = source->textLocation;
    textHeight = source->textHeight;
    textRotation = source->textRotation;
    attachment = source->attachment;
    arrowSize = source->arrowSize;
    hasArrowheads = source->hasArrowheads;
}

}  // namespace cad
