#include "cad/db/OleFrameEntity.hpp"

#include <algorithm>

#include "cad/geom/Polyline.hpp"

namespace cad {

std::vector<Vec3> OleFrameEntity::corners() const {
    // Stored as upper-left and lower-right, which are not sorted: an OLE frame
    // written by an application working in a flipped UCS has them the other way
    // round, and a rectangle with negative width picks and plots as nothing.
    const double left = std::min(upperLeft.x, lowerRight.x);
    const double right = std::max(upperLeft.x, lowerRight.x);
    const double bottom = std::min(upperLeft.y, lowerRight.y);
    const double top = std::max(upperLeft.y, lowerRight.y);
    const double z = upperLeft.z;
    return {Vec3{left, bottom, z}, Vec3{right, bottom, z}, Vec3{right, top, z},
            Vec3{left, top, z}};
}

Box3 OleFrameEntity::bounds() const {
    return Box3::fromPoints({upperLeft, lowerRight});
}

void OleFrameEntity::transformBy(const Mat4& m) {
    upperLeft = m.transformPoint(upperLeft);
    lowerRight = m.transformPoint(lowerRight);
}

std::vector<CurvePtr> OleFrameEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Polyline>(Polyline::fromPoints(corners(), /*closed=*/true)));
    return curves;
}

double OleFrameEntity::distanceToPoint(const Vec3& p) const {
    const Polyline ring = Polyline::fromPoints(corners(), /*closed=*/true);
    if (ring.containsPointInside(p)) {
        return 0.0;
    }
    return ring.distanceTo(p);
}

std::vector<GripPoint> OleFrameEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    const std::vector<Vec3> frame = corners();
    grips.reserve(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        grips.push_back(GripPoint{frame[i], GripKind::Vertex, i});
    }
    return grips;
}

void OleFrameEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index > 3) {
        return;
    }
    // Corner order is lower-left, lower-right, upper-right, upper-left, and the
    // stored form is upper-left plus lower-right, so each grip touches one
    // coordinate of each.
    const bool movesRight = grip.index == 1 || grip.index == 2;
    const bool movesTop = grip.index == 2 || grip.index == 3;
    (movesRight ? lowerRight.x : upperLeft.x) = to.x;
    (movesTop ? upperLeft.y : lowerRight.y) = to.y;
}

DbObjectPtr OleFrameEntity::clone() const {
    auto copy = std::make_unique<OleFrameEntity>();
    copyEntityInto(*copy);
    copy->upperLeft = upperLeft;
    copy->lowerRight = lowerRight;
    return copy;
}

void OleFrameEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const OleFrameEntity*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreEntityFrom(*source);
    upperLeft = source->upperLeft;
    lowerRight = source->lowerRight;
}

}  // namespace cad
