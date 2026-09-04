#pragma once

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

#include "cad/math/Tolerance.hpp"

namespace cad {

/// An immutable 3D vector / point in world space.
///
/// Deliberately 3-component even though the initial product is 2D drafting:
/// DWG itself stores every point as a 3D position with an extrusion direction,
/// and collapsing to 2D here would be the single hardest decision to undo
/// later. 2D operations simply leave `z` at 0.
///
/// A trivially copyable value type — 24 bytes, passed by value, no allocation.
/// Hot paths that cannot tolerate per-point work operate on packed arrays of
/// doubles instead.
class Vec3 {
public:
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3() noexcept = default;

    constexpr Vec3(double xValue, double yValue, double zValue = 0.0) noexcept
        : x(xValue), y(yValue), z(zValue) {}

    [[nodiscard]] static constexpr Vec3 zero() noexcept { return {0.0, 0.0, 0.0}; }
    [[nodiscard]] static constexpr Vec3 unitX() noexcept { return {1.0, 0.0, 0.0}; }
    [[nodiscard]] static constexpr Vec3 unitY() noexcept { return {0.0, 1.0, 0.0}; }
    [[nodiscard]] static constexpr Vec3 unitZ() noexcept { return {0.0, 0.0, 1.0}; }

    /// Unit vector at `angle` radians in the XY plane, scaled by `radius`.
    [[nodiscard]] static Vec3 fromAngle(double angle, double radius = 1.0) noexcept {
        return {std::cos(angle) * radius, std::sin(angle) * radius, 0.0};
    }

    // ------------------------------------------------------------ arithmetic

    [[nodiscard]] constexpr Vec3 operator+(const Vec3& v) const noexcept {
        return {x + v.x, y + v.y, z + v.z};
    }

    [[nodiscard]] constexpr Vec3 operator-(const Vec3& v) const noexcept {
        return {x - v.x, y - v.y, z - v.z};
    }

    [[nodiscard]] constexpr Vec3 operator*(double s) const noexcept {
        return {x * s, y * s, z * s};
    }

    [[nodiscard]] constexpr Vec3 operator/(double s) const noexcept {
        return {x / s, y / s, z / s};
    }

    [[nodiscard]] constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }

    constexpr Vec3& operator+=(const Vec3& v) noexcept {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }

    constexpr Vec3& operator-=(const Vec3& v) noexcept {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        return *this;
    }

    constexpr Vec3& operator*=(double s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    /// Component-wise multiply — used for non-uniform scaling.
    [[nodiscard]] constexpr Vec3 componentMul(const Vec3& v) const noexcept {
        return {x * v.x, y * v.y, z * v.z};
    }

    /// `*this + v * s`, the fused form used throughout the geometry code.
    [[nodiscard]] constexpr Vec3 addScaled(const Vec3& v, double s) const noexcept {
        return {x + v.x * s, y + v.y * s, z + v.z * s};
    }

    // --------------------------------------------------------------- products

    [[nodiscard]] constexpr double dot(const Vec3& v) const noexcept {
        return x * v.x + y * v.y + z * v.z;
    }

    [[nodiscard]] constexpr Vec3 cross(const Vec3& v) const noexcept {
        return {y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x};
    }

    /// Z component of the 2D cross product; its sign gives orientation in XY.
    [[nodiscard]] constexpr double cross2(const Vec3& v) const noexcept {
        return x * v.y - y * v.x;
    }

    // ---------------------------------------------------------------- length

    [[nodiscard]] double length() const noexcept { return std::sqrt(x * x + y * y + z * z); }

    [[nodiscard]] constexpr double lengthSquared() const noexcept { return x * x + y * y + z * z; }

    /// Length ignoring Z.
    [[nodiscard]] double length2() const noexcept { return std::sqrt(x * x + y * y); }

    [[nodiscard]] double distanceTo(const Vec3& v) const noexcept { return (*this - v).length(); }

    [[nodiscard]] constexpr double distanceSquaredTo(const Vec3& v) const noexcept {
        return (*this - v).lengthSquared();
    }

    /// Distance ignoring Z — the common case for 2D drafting predicates.
    [[nodiscard]] double distance2To(const Vec3& v) const noexcept {
        const double dx = x - v.x;
        const double dy = y - v.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // ------------------------------------------------------------- direction

    /// Unit vector in the same direction, or nullopt when degenerate.
    ///
    /// Callers that can legitimately receive a zero vector use this rather than
    /// catching an exception, which keeps the degenerate case on the normal
    /// control-flow path where it belongs.
    [[nodiscard]] std::optional<Vec3> tryNormalize(
        const Tolerance& tol = defaultTolerance()) const noexcept {
        const double len = length();
        if (len <= tol.point) {
            return std::nullopt;
        }
        return Vec3{x / len, y / len, z / len};
    }

    /// Unit vector in the same direction.
    /// @throws std::domain_error when the vector is shorter than the tolerance.
    [[nodiscard]] Vec3 normalized(const Tolerance& tol = defaultTolerance()) const {
        const auto result = tryNormalize(tol);
        if (!result) {
            throw std::domain_error("Cannot normalize a zero-length vector");
        }
        return *result;
    }

    /// Rotate about the Z axis through the origin.
    [[nodiscard]] Vec3 rotatedZ(double angle) const noexcept {
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        return {x * c - y * s, x * s + y * c, z};
    }

    /// Rotate about the Z axis through `center`.
    [[nodiscard]] Vec3 rotatedAbout(const Vec3& center, double angle) const noexcept {
        return (*this - center).rotatedZ(angle) + center;
    }

    /// Rotate about an arbitrary axis through the origin (Rodrigues' formula).
    /// Present so that 3D work needs no change to this class.
    [[nodiscard]] Vec3 rotatedAboutAxis(const Vec3& axis, double angle) const {
        const Vec3 k = axis.normalized();
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        return *this * c + k.cross(*this) * s + k * (k.dot(*this) * (1.0 - c));
    }

    /// Perpendicular in the XY plane, rotated +90 degrees.
    [[nodiscard]] constexpr Vec3 perp2() const noexcept { return {-y, x, z}; }

    /// Angle of this vector in the XY plane, in `(-pi, pi]`.
    [[nodiscard]] double angle2() const noexcept { return std::atan2(y, x); }

    // ------------------------------------------------------------ predicates

    [[nodiscard]] Vec3 lerp(const Vec3& v, double t) const noexcept {
        return {x + (v.x - x) * t, y + (v.y - y) * t, z + (v.z - z) * t};
    }

    [[nodiscard]] constexpr Vec3 midpoint(const Vec3& v) const noexcept {
        return {(x + v.x) * 0.5, (y + v.y) * 0.5, (z + v.z) * 0.5};
    }

    [[nodiscard]] constexpr bool equals(const Vec3& v,
                                        const Tolerance& tol = defaultTolerance()) const noexcept {
        return distanceSquaredTo(v) <= tol.point * tol.point;
    }

    /// Equality ignoring Z.
    [[nodiscard]] constexpr bool equals2(const Vec3& v,
                                         const Tolerance& tol = defaultTolerance()) const noexcept {
        const double dx = x - v.x;
        const double dy = y - v.y;
        return dx * dx + dy * dy <= tol.point * tol.point;
    }

    [[nodiscard]] constexpr bool isZero(const Tolerance& tol = defaultTolerance()) const noexcept {
        return lengthSquared() <= tol.point * tol.point;
    }

    /// True when both vectors point the same way, not merely parallel.
    [[nodiscard]] bool isCodirectional(const Vec3& v,
                                       const Tolerance& tol = defaultTolerance()) const noexcept {
        const auto a = tryNormalize(tol);
        const auto b = v.tryNormalize(tol);
        if (!a || !b) {
            return false;
        }
        return a->dot(*b) >= std::cos(tol.angle);
    }

    /// True when the vectors are parallel, in either direction.
    [[nodiscard]] bool isParallel(const Vec3& v,
                                  const Tolerance& tol = defaultTolerance()) const noexcept {
        const auto a = tryNormalize(tol);
        const auto b = v.tryNormalize(tol);
        if (!a || !b) {
            return false;
        }
        return std::abs(a->dot(*b)) >= std::cos(tol.angle);
    }

    [[nodiscard]] bool isPerpendicular(const Vec3& v,
                                       const Tolerance& tol = defaultTolerance()) const noexcept {
        const auto a = tryNormalize(tol);
        const auto b = v.tryNormalize(tol);
        if (!a || !b) {
            return false;
        }
        return std::abs(a->dot(*b)) <= std::sin(tol.angle);
    }

    /// Unsigned angle between two vectors, in `[0, pi]`.
    [[nodiscard]] double angleTo(const Vec3& v,
                                 const Tolerance& tol = defaultTolerance()) const noexcept {
        const auto a = tryNormalize(tol);
        const auto b = v.tryNormalize(tol);
        if (!a || !b) {
            return 0.0;
        }
        // Clamped because floating point can push the dot product past +/-1.
        return std::acos(clamp(a->dot(*b), -1.0, 1.0));
    }

    /// Drop to the XY plane.
    [[nodiscard]] constexpr Vec3 flattened() const noexcept { return {x, y, 0.0}; }

    [[nodiscard]] constexpr Vec3 withZ(double newZ) const noexcept { return {x, y, newZ}; }

    [[nodiscard]] bool isFinite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    [[nodiscard]] std::string toString() const;

    /// Exact equality. Provided for container keys and tests; geometric code
    /// must use `equals` with a tolerance instead.
    [[nodiscard]] constexpr bool operator==(const Vec3& v) const noexcept {
        return x == v.x && y == v.y && z == v.z;
    }

    [[nodiscard]] constexpr bool operator!=(const Vec3& v) const noexcept { return !(*this == v); }
};

[[nodiscard]] constexpr Vec3 operator*(double s, const Vec3& v) noexcept {
    return v * s;
}

}  // namespace cad
