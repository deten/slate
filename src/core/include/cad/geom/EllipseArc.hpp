#pragma once

#include "cad/geom/Arc.hpp"
#include "cad/geom/Curve.hpp"
#include "cad/math/Angle.hpp"

namespace cad {

/// An elliptical arc, parameterized by eccentric anomaly exactly as DXF stores
/// it (group codes 41/42), so import and export are lossless.
///
/// ```
///   P(u) = center + majorAxis * cos(u) + minorAxis * sin(u)
/// ```
///
/// `majorAxis` is a full vector carrying both direction and semi-major length;
/// `ratio` is semi-minor / semi-major in `(0, 1]`. The minor axis direction is
/// derived as `normal x majorAxis`, keeping a consistent counter-clockwise
/// sweep about the normal.
///
/// Ellipses are also the general home for circles under non-conformal
/// transforms: scaling a circle by different X and Y factors produces one of
/// these rather than a silently wrong circle.
class EllipseArc final : public Curve {
public:
    Vec3 center;
    /// Vector from centre to the end of the semi-major axis.
    Vec3 majorAxis = Vec3::unitX();
    /// Semi-minor / semi-major, in `(0, 1]`.
    double ratio = 1.0;
    /// Start eccentric anomaly, radians.
    double startParam = 0.0;
    /// End eccentric anomaly, radians.
    double endParam = kTau;
    Vec3 normal = Vec3::unitZ();

    /// @throws std::invalid_argument when the ratio is outside (0, 1] or the
    ///   major axis is degenerate.
    EllipseArc(const Vec3& centerPoint, const Vec3& major, double ratioValue,
               double start = 0.0, double end = kTau,
               const Vec3& extrusion = Vec3::unitZ());

    /// Widen a circular arc into the general conic form.
    [[nodiscard]] static EllipseArc fromArc(const Arc& arc);

    /// Vector from centre to the end of the semi-minor axis.
    [[nodiscard]] Vec3 minorAxis() const noexcept { return minorAxis_; }

    /// Total sweep in parameter space, in `(0, 2pi]`.
    [[nodiscard]] double sweep() const noexcept { return sweep_; }

    [[nodiscard]] double semiMajor() const { return majorAxis.length(); }
    [[nodiscard]] double semiMinor() const { return semiMajor() * ratio; }

    /// Rotation of the major axis within the XY plane.
    [[nodiscard]] double rotation() const { return majorAxis.angle2(); }

    [[nodiscard]] CurvePtr clone() const override {
        return std::make_unique<EllipseArc>(center, majorAxis, ratio, startParam, endParam,
                                            normal);
    }

    [[nodiscard]] bool isClosed() const override { return full_; }

    /// Position at an absolute eccentric anomaly.
    [[nodiscard]] Vec3 pointAtParam(double u) const {
        return center.addScaled(majorAxis, std::cos(u)).addScaled(minorAxis_, std::sin(u));
    }

    /// Derivative with respect to eccentric anomaly.
    [[nodiscard]] Vec3 derivativeAtParam(double u) const {
        return majorAxis * -std::sin(u) + minorAxis_ * std::cos(u);
    }

    /// Absolute eccentric anomaly at normalized parameter `t`.
    [[nodiscard]] double paramAt(double t) const { return startParam + sweep_ * t; }

    /// Normalized parameter for an absolute eccentric anomaly.
    [[nodiscard]] double normalizedAt(double u) const;

    [[nodiscard]] bool containsParam(double u,
                                     const Tolerance& tol = defaultTolerance()) const {
        if (full_) {
            return true;
        }
        const double offset = normalizeAngle(u - startParam);
        return offset <= sweep_ + tol.angle || offset >= kTau - tol.angle;
    }

    /// Eccentric anomaly of a point assumed to lie on the ellipse.
    [[nodiscard]] double paramAtPoint(const Vec3& p) const;

    [[nodiscard]] Vec3 startPoint() const override { return pointAtParam(startParam); }

    [[nodiscard]] Vec3 endPoint() const override {
        return full_ ? startPoint() : pointAtParam(endParam);
    }

    [[nodiscard]] Vec3 pointAt(double t) const override {
        return pointAtParam(paramAt(clampParam(t)));
    }

    [[nodiscard]] Vec3 tangentAt(double t) const override {
        return derivativeAtParam(paramAt(clampParam(t))) * sweep_;
    }

    /// Arc length by adaptive Gauss-Legendre quadrature.
    ///
    /// Elliptic arc length has no closed form, and Ramanujan-style
    /// approximations only cover the full perimeter. Quadrature over the actual
    /// sweep is both general and accurate to well below drafting tolerance.
    [[nodiscard]] double length() const override;

    [[nodiscard]] double signedArea() const override;
    [[nodiscard]] bool hasArea() const override { return true; }

    [[nodiscard]] Box3 bounds() const override;

    [[nodiscard]] ClosestPointResult closestPoint(const Vec3& p) const override;

    [[nodiscard]] std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const override;

    [[nodiscard]] CurvePtr transformed(const Mat4& m) const override;

    [[nodiscard]] CurvePtr reversed() const override;

    [[nodiscard]] std::vector<Vec3> tessellate(
        const TessellationOptions& options = {}) const override;

    /// Segment count sized by the *maximum* curvature, at the end of the major
    /// axis where the radius of curvature is `b^2 / a`. Using the tightest
    /// curvature keeps the chord error bounded everywhere on the ellipse.
    [[nodiscard]] int segmentCount(const TessellationOptions& options = {}) const;

    [[nodiscard]] CurvePtr trimmed(double t0, double t1) const override {
        return std::make_unique<EllipseArc>(center, majorAxis, ratio, paramAt(clampParam(t0)),
                                            paramAt(clampParam(t1)), normal);
    }

    /// True when the ellipse is circular within tolerance.
    [[nodiscard]] bool isCircular(const Tolerance& tol = defaultTolerance()) const {
        return std::abs(1.0 - ratio) <= tol.point;
    }

    /// Narrow to a circular arc when possible, for cleaner downstream handling.
    [[nodiscard]] std::optional<Arc> toArcIfCircular(
        const Tolerance& tol = defaultTolerance()) const;

private:
    Vec3 minorAxis_;
    double sweep_ = kTau;
    bool full_ = false;
    /// Lazily computed arc length; -1 means "not yet computed". Mutable so
    /// `length()` can stay const, which it must be to satisfy the Curve
    /// interface. The memo is not observable state.
    mutable double cachedLength_ = -1.0;
};

}  // namespace cad
