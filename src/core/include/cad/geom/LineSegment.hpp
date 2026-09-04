#pragma once

#include "cad/geom/Curve.hpp"

namespace cad {

/// A bounded straight segment between two points.
///
/// Parameter `t` runs linearly from 0 at `start` to 1 at `end`, so the
/// normalized parameter is also the fraction of arc length.
class LineSegment final : public Curve {
public:
    Vec3 start;
    Vec3 end;

    LineSegment(const Vec3& from, const Vec3& to) : start(from), end(to) {}

    [[nodiscard]] CurvePtr clone() const override {
        return std::make_unique<LineSegment>(start, end);
    }

    [[nodiscard]] Vec3 startPoint() const override { return start; }
    [[nodiscard]] Vec3 endPoint() const override { return end; }
    [[nodiscard]] bool isClosed() const override { return false; }

    /// End minus start. Zero-length segments are legal and handled throughout.
    [[nodiscard]] Vec3 direction() const { return end - start; }

    /// Unit direction, or empty for a degenerate segment.
    [[nodiscard]] std::optional<Vec3> unitDirection(
        const Tolerance& tol = defaultTolerance()) const {
        return direction().tryNormalize(tol);
    }

    [[nodiscard]] bool isDegenerate(const Tolerance& tol = defaultTolerance()) const {
        return direction().isZero(tol);
    }

    [[nodiscard]] double length() const override { return start.distanceTo(end); }

    [[nodiscard]] Vec3 pointAt(double t) const override {
        return start.lerp(end, clampParam(t));
    }

    /// Position at an unclamped parameter — used by extension logic.
    [[nodiscard]] Vec3 pointAtExtended(double t) const { return start.lerp(end, t); }

    [[nodiscard]] Vec3 tangentAt(double /*t*/) const override { return direction(); }

    [[nodiscard]] Box3 bounds() const override { return Box3::fromCorners(start, end); }

    [[nodiscard]] ClosestPointResult closestPoint(const Vec3& p) const override;

    [[nodiscard]] std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const override;

    /// Perpendicular foot from `p`, unrestricted by the segment's extent.
    ///
    /// Backs the PERPENDICULAR object snap, which is allowed to land on the
    /// extension of a segment.
    [[nodiscard]] std::optional<Vec3> perpendicularFoot(const Vec3& p) const {
        const auto result = closestPointExtended(p);
        if (!result) {
            return std::nullopt;
        }
        return result->point;
    }

    /// Signed perpendicular distance from `p` to the infinite line in the XY
    /// plane. Positive when `p` is to the left of the direction of travel.
    [[nodiscard]] double signedDistance2(const Vec3& p) const;

    /// True when `p` lies on the infinite line through this segment.
    [[nodiscard]] bool isPointOnLine(const Vec3& p,
                                     const Tolerance& tol = defaultTolerance()) const {
        const auto result = closestPointExtended(p);
        return result ? result->distance <= tol.point : p.equals(start, tol);
    }

    [[nodiscard]] CurvePtr transformed(const Mat4& m) const override {
        return std::make_unique<LineSegment>(m.transformPoint(start), m.transformPoint(end));
    }

    [[nodiscard]] CurvePtr reversed() const override {
        return std::make_unique<LineSegment>(end, start);
    }

    [[nodiscard]] std::vector<Vec3> tessellate(
        const TessellationOptions& /*options*/ = {}) const override {
        return {start, end};
    }

    [[nodiscard]] CurvePtr trimmed(double t0, double t1) const override {
        return std::make_unique<LineSegment>(pointAt(t0), pointAt(t1));
    }

    /// Extend or shorten so the segment terminates at `p`, keeping whichever
    /// endpoint is further from `p`. Empty when `p` is not on the infinite line.
    [[nodiscard]] std::optional<LineSegment> extendedTo(
        const Vec3& p, const Tolerance& tol = defaultTolerance()) const;

    [[nodiscard]] bool equals(const LineSegment& other,
                              const Tolerance& tol = defaultTolerance()) const {
        return start.equals(other.start, tol) && end.equals(other.end, tol);
    }
};

}  // namespace cad
