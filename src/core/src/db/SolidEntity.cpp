#include "cad/db/SolidEntity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

/// The same downcast-or-throw every `restoreFrom` needs.
template <typename T>
[[nodiscard]] const T& expectSnapshot(const DbObject& snapshot, const char* what) {
    const auto* source = dynamic_cast<const T*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument(std::string{"Snapshot type mismatch restoring "} + what);
    }
    return *source;
}

/// Winding test, so a click in the middle of a filled shape hits it.
[[nodiscard]] bool containsPoint(const std::vector<Vec3>& ring, const Vec3& p) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const Vec3& a = ring[i];
        const Vec3& b = ring[j];
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

}  // namespace

std::vector<Vec3> SolidEntity::ring() const {
    // Traced first, second, fourth, third. See the class comment: this is the
    // swap that turns DXF's storage order into a shape rather than a bow tie.
    std::vector<Vec3> points{corners[0], corners[1], corners[3], corners[2]};
    // A repeated last corner means DXF is describing a triangle.
    if (points[2].equals(points[3])) {
        points.pop_back();
    }
    return points;
}

Box3 SolidEntity::bounds() const {
    Box3 box;
    for (const Vec3& corner : ring()) {
        box = box.united(Box3::fromPoint(corner));
    }
    return box;
}

std::vector<CurvePtr> SolidEntity::toCurves() const {
    const std::vector<Vec3> points = ring();
    std::vector<CurvePtr> curves;
    if (points.size() < 3) {
        return curves;
    }
    curves.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec3& from = points[i];
        const Vec3& to = points[(i + 1) % points.size()];
        if (!from.equals(to)) {
            curves.push_back(std::make_unique<LineSegment>(from, to));
        }
    }
    return curves;
}

double SolidEntity::distanceToPoint(const Vec3& p) const {
    const std::vector<Vec3> points = ring();
    if (points.size() >= 3 && containsPoint(points, p)) {
        return 0.0;
    }
    double best = std::numeric_limits<double>::infinity();
    for (const CurvePtr& curve : toCurves()) {
        best = std::min(best, curve->distanceTo(p));
    }
    return best;
}

void SolidEntity::transformBy(const Mat4& m) {
    for (Vec3& corner : corners) {
        corner = m.transformPoint(corner);
    }
}

std::vector<GripPoint> SolidEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    // Indexed by storage position, not ring position, so a grip moves the
    // corner it was drawn on rather than its neighbour.
    for (std::size_t i = 0; i < corners.size(); ++i) {
        // A triangle's repeated corner offers one grip, not two on top of
        // each other -- the second would be unreachable and would look like a
        // grip that does nothing.
        if (i == 3 && corners[3].equals(corners[2])) {
            continue;
        }
        grips.push_back({corners[i], GripKind::Vertex, i});
    }
    return grips;
}

void SolidEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index >= corners.size()) {
        return;
    }
    // On a triangle the third and fourth corners are the same point, and both
    // have to move or the shape becomes a quadrilateral with a hairline edge.
    const bool triangle = corners[3].equals(corners[2]);
    corners[grip.index] = to;
    if (triangle && grip.index == 2) {
        corners[3] = to;
    }
}

DbObjectPtr SolidEntity::clone() const {
    auto copy = std::make_unique<SolidEntity>();
    copyEntityInto(*copy);
    copy->corners = corners;
    copy->thickness = thickness;
    return copy;
}

void SolidEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<SolidEntity>(snapshot, "a solid");
    restoreEntityFrom(source);
    corners = source.corners;
    thickness = source.thickness;
}

}  // namespace cad
