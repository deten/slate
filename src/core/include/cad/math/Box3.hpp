#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <span>

#include "cad/math/Mat4.hpp"
#include "cad/math/Tolerance.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// An axis-aligned bounding box — the CAD "extents".
///
/// An empty box is represented by `min > max` on every axis, which makes the
/// default-constructed box a correct identity for union with no special cases.
class Box3 {
public:
    Vec3 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity()};
    Vec3 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()};

    /// Constructs the empty box.
    constexpr Box3() noexcept = default;

    constexpr Box3(const Vec3& minimum, const Vec3& maximum) noexcept
        : min(minimum), max(maximum) {}

    [[nodiscard]] static constexpr Box3 empty() noexcept { return Box3{}; }

    [[nodiscard]] static constexpr Box3 fromPoint(const Vec3& p) noexcept { return Box3{p, p}; }

    /// Box from two opposite corners, in any order.
    [[nodiscard]] static constexpr Box3 fromCorners(const Vec3& a, const Vec3& b) noexcept {
        return Box3{Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)},
                    Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}};
    }

    /// Box tightly containing all given points.
    [[nodiscard]] static Box3 fromPoints(std::span<const Vec3> points) noexcept;

    [[nodiscard]] static Box3 fromPoints(std::initializer_list<Vec3> points) noexcept {
        return fromPoints(std::span<const Vec3>{points.begin(), points.size()});
    }

    [[nodiscard]] static constexpr Box3 fromCenterSize(const Vec3& center,
                                                       const Vec3& size) noexcept {
        const Vec3 half = size * 0.5;
        return Box3{center - half, center + half};
    }

    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    [[nodiscard]] constexpr Vec3 center() const noexcept { return min.midpoint(max); }

    [[nodiscard]] constexpr Vec3 size() const noexcept {
        return isEmpty() ? Vec3::zero() : max - min;
    }

    [[nodiscard]] constexpr double width() const noexcept { return isEmpty() ? 0.0 : max.x - min.x; }
    [[nodiscard]] constexpr double height() const noexcept {
        return isEmpty() ? 0.0 : max.y - min.y;
    }
    [[nodiscard]] constexpr double depth() const noexcept { return isEmpty() ? 0.0 : max.z - min.z; }

    /// Length of the diagonal — a scale-independent size measure.
    [[nodiscard]] double diagonal() const noexcept { return isEmpty() ? 0.0 : min.distanceTo(max); }

    [[nodiscard]] constexpr Box3 united(const Box3& other) const noexcept {
        if (other.isEmpty()) {
            return *this;
        }
        if (isEmpty()) {
            return other;
        }
        return Box3{Vec3{std::min(min.x, other.min.x), std::min(min.y, other.min.y),
                         std::min(min.z, other.min.z)},
                    Vec3{std::max(max.x, other.max.x), std::max(max.y, other.max.y),
                         std::max(max.z, other.max.z)}};
    }

    constexpr void unite(const Box3& other) noexcept { *this = united(other); }

    [[nodiscard]] constexpr Box3 expandedToPoint(const Vec3& p) const noexcept {
        if (isEmpty()) {
            return fromPoint(p);
        }
        return Box3{Vec3{std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)},
                    Vec3{std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)}};
    }

    constexpr void expandToPoint(const Vec3& p) noexcept { *this = expandedToPoint(p); }

    /// Grow by `margin` on every side.
    [[nodiscard]] constexpr Box3 expanded(double margin) const noexcept {
        if (isEmpty()) {
            return *this;
        }
        const Vec3 m{margin, margin, margin};
        return Box3{min - m, max + m};
    }

    /// Intersection, or the empty box when disjoint.
    [[nodiscard]] constexpr Box3 intersected(const Box3& other) const noexcept {
        if (isEmpty() || other.isEmpty()) {
            return Box3{};
        }
        const Box3 result{Vec3{std::max(min.x, other.min.x), std::max(min.y, other.min.y),
                               std::max(min.z, other.min.z)},
                          Vec3{std::min(max.x, other.max.x), std::min(max.y, other.max.y),
                               std::min(max.z, other.max.z)}};
        return result.isEmpty() ? Box3{} : result;
    }

    [[nodiscard]] constexpr bool containsPoint(
        const Vec3& p, const Tolerance& tol = defaultTolerance()) const noexcept {
        if (isEmpty()) {
            return false;
        }
        const double e = tol.point;
        return p.x >= min.x - e && p.x <= max.x + e && p.y >= min.y - e && p.y <= max.y + e &&
               p.z >= min.z - e && p.z <= max.z + e;
    }

    /// True when `other` lies entirely within this box — window selection.
    [[nodiscard]] constexpr bool containsBox(
        const Box3& other, const Tolerance& tol = defaultTolerance()) const noexcept {
        if (isEmpty() || other.isEmpty()) {
            return false;
        }
        const double e = tol.point;
        return other.min.x >= min.x - e && other.max.x <= max.x + e && other.min.y >= min.y - e &&
               other.max.y <= max.y + e && other.min.z >= min.z - e && other.max.z <= max.z + e;
    }

    /// True when the boxes overlap at all — crossing selection, culling.
    [[nodiscard]] constexpr bool intersects(
        const Box3& other, const Tolerance& tol = defaultTolerance()) const noexcept {
        if (isEmpty() || other.isEmpty()) {
            return false;
        }
        const double e = tol.point;
        return min.x <= other.max.x + e && max.x >= other.min.x - e && min.y <= other.max.y + e &&
               max.y >= other.min.y - e && min.z <= other.max.z + e && max.z >= other.min.z - e;
    }

    /// Overlap test ignoring Z — the common case for 2D drafting.
    [[nodiscard]] constexpr bool intersects2(
        const Box3& other, const Tolerance& tol = defaultTolerance()) const noexcept {
        if (isEmpty() || other.isEmpty()) {
            return false;
        }
        const double e = tol.point;
        return min.x <= other.max.x + e && max.x >= other.min.x - e && min.y <= other.max.y + e &&
               max.y >= other.min.y - e;
    }

    /// Shortest distance from `p` to the box; zero when inside.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const noexcept;

    /// Box containing this box after transformation.
    ///
    /// Transforms all eight corners rather than the two extreme points, which
    /// is required for correctness under rotation.
    [[nodiscard]] Box3 transformed(const Mat4& t) const noexcept;

    /// The four XY corners, counter-clockwise from the minimum.
    [[nodiscard]] std::array<Vec3, 4> corners2() const noexcept;

    [[nodiscard]] bool equals(const Box3& other,
                              const Tolerance& tol = defaultTolerance()) const noexcept {
        if (isEmpty() && other.isEmpty()) {
            return true;
        }
        if (isEmpty() != other.isEmpty()) {
            return false;
        }
        return min.equals(other.min, tol) && max.equals(other.max, tol);
    }
};

}  // namespace cad
