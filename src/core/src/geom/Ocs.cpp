#include "cad/geom/Ocs.hpp"

#include <cmath>

namespace cad {

CoordinateFrame arbitraryAxis(const Vec3& normal) noexcept {
    const Vec3 n = normal.tryNormalize().value_or(Vec3::unitZ());

    // When the normal is nearly vertical in the world XZ/YZ sense, world Z is a
    // poor reference because the cross product would be near-degenerate.
    const Vec3 reference =
        (std::abs(n.x) < kArbitraryAxisLimit && std::abs(n.y) < kArbitraryAxisLimit)
            ? Vec3::unitY()
            : Vec3::unitZ();

    const Vec3 xAxis = reference.cross(n).normalized();
    const Vec3 yAxis = n.cross(xAxis).normalized();
    return {xAxis, yAxis, n};
}

Mat4 ocsToWorld(const Vec3& normal) noexcept {
    if (isWorldNormal(normal)) {
        return Mat4::identity();
    }
    const CoordinateFrame frame = arbitraryAxis(normal);
    return Mat4::fromBasis(frame.xAxis, frame.yAxis, frame.zAxis);
}

Mat4 worldToOcs(const Vec3& normal) noexcept {
    if (isWorldNormal(normal)) {
        return Mat4::identity();
    }
    return ocsToWorld(normal).transposed();
}

Vec3 pointOcsToWorld(const Vec3& p, const Vec3& normal) noexcept {
    if (isWorldNormal(normal)) {
        return p;
    }
    const CoordinateFrame frame = arbitraryAxis(normal);
    return frame.xAxis * p.x + frame.yAxis * p.y + frame.zAxis * p.z;
}

Vec3 pointWorldToOcs(const Vec3& p, const Vec3& normal) noexcept {
    if (isWorldNormal(normal)) {
        return p;
    }
    const CoordinateFrame frame = arbitraryAxis(normal);
    return {p.dot(frame.xAxis), p.dot(frame.yAxis), p.dot(frame.zAxis)};
}

}  // namespace cad
