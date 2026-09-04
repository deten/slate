#include "cad/math/Box3.hpp"

namespace cad {

Box3 Box3::fromPoints(std::span<const Vec3> points) noexcept {
    Box3 result;
    for (const Vec3& p : points) {
        result = result.expandedToPoint(p);
    }
    return result;
}

double Box3::distanceToPoint(const Vec3& p) const noexcept {
    if (isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    const double dx = std::max({min.x - p.x, 0.0, p.x - max.x});
    const double dy = std::max({min.y - p.y, 0.0, p.y - max.y});
    const double dz = std::max({min.z - p.z, 0.0, p.z - max.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Box3 Box3::transformed(const Mat4& t) const noexcept {
    if (isEmpty()) {
        return *this;
    }
    const std::array<Vec3, 8> corners{{
        {min.x, min.y, min.z}, {max.x, min.y, min.z}, {min.x, max.y, min.z}, {max.x, max.y, min.z},
        {min.x, min.y, max.z}, {max.x, min.y, max.z}, {min.x, max.y, max.z}, {max.x, max.y, max.z},
    }};

    Box3 result;
    for (const Vec3& corner : corners) {
        result = result.expandedToPoint(t.transformPoint(corner));
    }
    return result;
}

std::array<Vec3, 4> Box3::corners2() const noexcept {
    const double z = min.z;
    return {{{min.x, min.y, z}, {max.x, min.y, z}, {max.x, max.y, z}, {min.x, max.y, z}}};
}

}  // namespace cad
