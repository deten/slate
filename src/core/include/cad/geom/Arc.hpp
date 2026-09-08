#pragma once

#include "cad/geom/Curve.hpp"
#include "cad/geom/Ocs.hpp"
#include "cad/math/Angle.hpp"

namespace cad {

/// A circular arc, always swept counter-clockwise from `startAngle` to
/// `endAngle` about `normal` — the same convention DWG uses, which avoids a
/// class of sign errors at the import boundary.
///
/// A full circle is represented by `startAngle == endAngle` together with the
/// explicit full-circle flag; `Arc::circle` builds one. The flag exists because
/// a zero sweep is otherwise ambiguous, and DXF disambiguates by entity type
/// (CIRCLE versus ARC) rather than by angle.
///
/// The arc is planar but not restricted to the world XY plane: `normal` gives
/// the plane's orientation and the in-plane axes come from the arbitrary axis
/// algorithm, matching OCS semantics exactly.
class Arc final : public Curve {
public:
    Vec3 center;
    double radius = 1.0;
    /// Start angle in radians, measured in the arc's own plane.
    double startAngle = 0.0;
    /// End angle in radians. Equal to `startAngle` for a full circle.
    double endAngle = 0.0;
    Vec3 normal = Vec3::unitZ();

    /// @throws std::invalid_argument when the radius is negative or non-finite.
    Arc(const Vec3& centerPoint, double radiusValue, double start, double end,
        const Vec3& extrusion = Vec3::unitZ(), bool forceFullCircle = false);

    /// A complete circle.
    [[nodiscard]] static Arc circle(const Vec3& centerPoint, double radiusValue,
                                    const Vec3& extrusion = Vec3::unitZ()) {
        return Arc{centerPoint, radiusValue, 0.0, 0.0, extrusion, true};
    }

    /// Arc through three points, in the XY plane.
    /// Empty when the points are collinear or coincident.
    [[nodiscard]] static std::optional<Arc> fromThreePoints(
        const Vec3& a, const Vec3& b, const Vec3& c,
        const Tolerance& tol = defaultTolerance());

    /// Arc from two endpoints and a bulge factor — the representation used by
    /// DWG/DXF lightweight polylines.
    ///
    /// Bulge is `tan(includedAngle / 4)`; positive is counter-clockwise, and 0
    /// means a straight segment (for which this returns empty).
    [[nodiscard]] static std::optional<Arc> fromBulge(
        const Vec3& start, const Vec3& end, double bulge,
        const Tolerance& tol = defaultTolerance());

    /// The bulge factor for this arc traversed start to end, for writing back
    /// out to a polyline.
    [[nodiscard]] double bulge() const {
        // Signed for a polyline in the world XY plane: an arc whose normal
        // points down is walked clockwise as seen from above, which is what
        // a negative bulge means. Reversing an arc flips its normal, so this
        // is how a reversed arc keeps its shape when it becomes a vertex.
        return normal.z < 0.0 ? -std::tan(sweep_ / 4.0) : std::tan(sweep_ / 4.0);
    }

    /// Total counter-clockwise sweep in radians, in `(0, 2pi]`.
    [[nodiscard]] double sweep() const noexcept { return sweep_; }

    [[nodiscard]] bool isFullCircle() const noexcept { return full_; }

    [[nodiscard]] CurvePtr clone() const override {
        return std::make_unique<Arc>(center, radius, startAngle, endAngle, normal, full_);
    }

    [[nodiscard]] bool isClosed() const override { return full_; }

    /// Point on the arc's plane at the given absolute angle.
    [[nodiscard]] Vec3 pointAtAngle(double angle) const {
        return center.addScaled(frame_.xAxis, std::cos(angle) * radius)
            .addScaled(frame_.yAxis, std::sin(angle) * radius);
    }

    /// Angle in the arc's plane of the direction from the centre to `p`.
    [[nodiscard]] double angleAtPoint(const Vec3& p) const {
        const Vec3 d = p - center;
        return normalizeAngle(std::atan2(d.dot(frame_.yAxis), d.dot(frame_.xAxis)));
    }

    /// Absolute angle at normalized parameter `t`.
    [[nodiscard]] double angleAt(double t) const { return startAngle + sweep_ * t; }

    /// Normalized parameter for an absolute angle. Angles outside the sweep are
    /// reported as the nearer of 0 or 1, mirroring how other CAD software resolves picks
    /// slightly off the end of an arc.
    [[nodiscard]] double paramAtAngle(double angle) const;

    /// True when the absolute angle lies within the swept region.
    [[nodiscard]] bool containsAngle(double angle,
                                     const Tolerance& tol = defaultTolerance()) const {
        if (full_) {
            return true;
        }
        const double offset = normalizeAngle(angle - startAngle);
        return offset <= sweep_ + tol.angle || offset >= kTau - tol.angle;
    }

    [[nodiscard]] Vec3 startPoint() const override { return pointAtAngle(startAngle); }

    [[nodiscard]] Vec3 endPoint() const override {
        return full_ ? startPoint() : pointAtAngle(endAngle);
    }

    [[nodiscard]] Vec3 pointAt(double t) const override {
        return pointAtAngle(angleAt(clampParam(t)));
    }

    [[nodiscard]] Vec3 tangentAt(double t) const override;

    [[nodiscard]] double length() const override { return radius * sweep_; }

    [[nodiscard]] double signedArea() const override;
    [[nodiscard]] bool hasArea() const override { return true; }

    [[nodiscard]] Box3 bounds() const override;

    [[nodiscard]] ClosestPointResult closestPoint(const Vec3& p) const override;

    [[nodiscard]] std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const override;

    /// The quadrant points of the containing circle that lie on this arc —
    /// the QUADRANT object snap.
    [[nodiscard]] std::vector<Vec3> quadrantPoints() const;

    [[nodiscard]] CurvePtr transformed(const Mat4& m) const override;

    [[nodiscard]] CurvePtr reversed() const override;

    [[nodiscard]] std::vector<Vec3> tessellate(
        const TessellationOptions& options = {}) const override;

    /// Number of chords needed to stay within the chord tolerance.
    [[nodiscard]] int segmentCount(const TessellationOptions& options = {}) const;

    [[nodiscard]] CurvePtr trimmed(double t0, double t1) const override {
        return std::make_unique<Arc>(center, radius, angleAt(clampParam(t0)),
                                     angleAt(clampParam(t1)), normal);
    }

    [[nodiscard]] bool equals(const Arc& other,
                              const Tolerance& tol = defaultTolerance()) const;

private:
    CoordinateFrame frame_;
    double sweep_ = kTau;
    bool full_ = false;
};

}  // namespace cad
