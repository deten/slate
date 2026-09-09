#include "cad/db/ConstructionLineEntity.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

#include "cad/db/DbObject.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

/// How far the drawn stretch reaches from the base, in drawing units. Far
/// enough to cross any drawing many times; not so far that a double loses
/// precision along it.
constexpr double kReach = 1.0e6;

/// The downcast-or-throw every `restoreFrom` needs, worded as the others.
template <typename T>
[[nodiscard]] const T& expectSnapshot(const DbObject& snapshot, const char* what) {
    const auto* source = dynamic_cast<const T*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument(std::string{"Snapshot type mismatch restoring "} + what);
    }
    return *source;
}

}  // namespace

Vec3 ConstructionLineEntity::drawnStart() const {
    return bothWays ? base - direction * kReach : base;
}

Vec3 ConstructionLineEntity::drawnEnd() const { return base + direction * kReach; }

Box3 ConstructionLineEntity::bounds() const {
    return Box3::fromCorners(drawnStart(), drawnEnd());
}

std::vector<CurvePtr> ConstructionLineEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<LineSegment>(drawnStart(), drawnEnd()));
    return curves;
}

double ConstructionLineEntity::distanceToPoint(const Vec3& p) const {
    const Vec3 d = p - base;
    double along = d.dot(direction);
    if (!bothWays && along < 0.0) {
        along = 0.0;
    }
    return (base + direction * along).distanceTo(p);
}

void ConstructionLineEntity::transformBy(const Mat4& m) {
    base = m.transformPoint(base);
    const Vec3 moved = m.transformVector(direction);
    if (const auto unit = moved.tryNormalize(); unit.has_value()) {
        direction = *unit;
    }
}

std::vector<GripPoint> ConstructionLineEntity::gripPoints() const {
    // The base, and a second grip a way along to turn it by. A ray's base
    // is its end, so its grip is an endpoint; a line's base is a point on
    // it, so it is a midpoint, which is what the grip's snap will say.
    return {{base, bothWays ? GripKind::Midpoint : GripKind::Endpoint, 0},
            {base + direction * 100.0, GripKind::Endpoint, 1}};
}

void ConstructionLineEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index == 0) {
        base = to;
        return;
    }
    if (const auto unit = (to - base).tryNormalize(); unit.has_value()) {
        direction = *unit;
    }
}

DbObjectPtr ConstructionLineEntity::clone() const {
    auto copy = std::make_unique<ConstructionLineEntity>();
    copyEntityInto(*copy);
    copy->base = base;
    copy->direction = direction;
    copy->bothWays = bothWays;
    return copy;
}

void ConstructionLineEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<ConstructionLineEntity>(snapshot, "a construction line");
    restoreEntityFrom(source);
    base = source.base;
    direction = source.direction;
    bothWays = source.bothWays;
}

}  // namespace cad
