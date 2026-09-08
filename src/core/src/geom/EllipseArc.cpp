#include "cad/geom/EllipseArc.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace cad {
namespace {

/// Five-point Gauss-Legendre quadrature on `[a, b]`.
template <typename F>
[[nodiscard]] double gaussLegendre5(F&& f, double a, double b) {
    static constexpr std::array<double, 5> kNodes{-0.906179845938664, -0.538469310105683, 0.0,
                                                  0.538469310105683, 0.906179845938664};
    static constexpr std::array<double, 5> kWeights{0.236926885056189, 0.478628670499366,
                                                    0.568888888888889, 0.478628670499366,
                                                    0.236926885056189};
    const double half = (b - a) / 2.0;
    const double mid = (a + b) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < kNodes.size(); ++i) {
        sum += kWeights[i] * f(mid + half * kNodes[i]);
    }
    return sum * half;
}

/// Rytz's construction: recover the principal axes of an ellipse from a pair of
/// conjugate semi-diameters.
///
/// Needed because an affine transform of an ellipse preserves conjugacy but not
/// perpendicularity — after a shear or non-uniform scale, the transformed
/// "axes" are no longer axes.
struct ConjugateAxes {
    Vec3 a;
    Vec3 b;
};

[[nodiscard]] ConjugateAxes conjugateToAxes(const Vec3& u, const Vec3& w) {
    // The extreme radii occur at the parameter extremizing |u cos t + w sin t|.
    // Differentiating gives tan(2t) = 2(u.w) / (|u|^2 - |w|^2).
    const double uu = u.lengthSquared();
    const double ww = w.lengthSquared();
    const double uw = u.dot(w);

    if (std::abs(uw) < 1e-15 && std::abs(uu - ww) < 1e-15) {
        // Already a circle: any orthogonal pair works.
        return {u, w};
    }

    const double t = 0.5 * std::atan2(2.0 * uw, uu - ww);
    return {u * std::cos(t) + w * std::sin(t), u * -std::sin(t) + w * std::cos(t)};
}

}  // namespace

EllipseArc::EllipseArc(const Vec3& centerPoint, const Vec3& major, double ratioValue,
                       double start, double end, const Vec3& extrusion)
    : center(centerPoint), majorAxis(major), normal(extrusion) {
    if (!(ratioValue > 0.0)) {
        throw std::invalid_argument("Ellipse ratio must be positive");
    }
    if (major.isZero()) {
        throw std::invalid_argument("Ellipse major axis must be non-zero");
    }
    // A ratio above one names the shorter axis as the major one. Files do
    // carry them -- a hatch edge at 1.0001 from rounding, or a genuine 2 --
    // and the ellipse is the same ellipse either way, so it is taken as the
    // one with the axes the other way round rather than refused. The
    // parameters move with the axis: a quarter turn earlier.
    if (ratioValue > 1.0 + 1e-12) {
        const Vec3 minorDirection =
            extrusion.cross(major).tryNormalize().value_or(Vec3::unitY());
        majorAxis = minorDirection * (major.length() * ratioValue);
        ratioValue = 1.0 / ratioValue;
        start -= kPi / 2.0;
        end -= kPi / 2.0;
    }
    ratio = std::min(ratioValue, 1.0);

    startParam = normalizeAngle(start);
    const double rawSweep = normalizeAngle(end - start);
    // DXF writes a full ellipse as 0 -> 2*pi, which normalizes to a zero sweep.
    full_ = rawSweep == 0.0;
    sweep_ = full_ ? kTau : rawSweep;
    endParam = normalizeAngle(startParam + sweep_);

    const Vec3 minorDirection =
        extrusion.cross(majorAxis).tryNormalize().value_or(Vec3::unitY());
    minorAxis_ = minorDirection * (majorAxis.length() * ratio);
}

EllipseArc EllipseArc::fromArc(const Arc& arc) {
    const Vec3 reference = arc.pointAtAngle(0.0) - arc.center;
    const Vec3 major = reference.isZero() ? Vec3::unitX() * arc.radius : reference;
    return EllipseArc{arc.center, major, 1.0, arc.startAngle,
                      arc.isFullCircle() ? arc.startAngle : arc.endAngle, arc.normal};
}

double EllipseArc::normalizedAt(double u) const {
    const double offset = normalizeAngle(u - startParam);
    if (offset <= sweep_) {
        return offset / sweep_;
    }
    return (offset - sweep_) <= (kTau - offset) ? 1.0 : 0.0;
}

double EllipseArc::paramAtPoint(const Vec3& p) const {
    const Vec3 d = p - center;
    const double cosine = d.dot(majorAxis) / majorAxis.lengthSquared();
    const double sine = d.dot(minorAxis_) / minorAxis_.lengthSquared();
    return normalizeAngle(std::atan2(sine, cosine));
}

double EllipseArc::length() const {
    if (cachedLength_ >= 0.0) {
        return cachedLength_;
    }
    constexpr int kPanels = 16;
    const double step = sweep_ / kPanels;
    double total = 0.0;
    for (int i = 0; i < kPanels; ++i) {
        total += gaussLegendre5([this](double u) { return derivativeAtParam(u).length(); },
                                startParam + i * step, startParam + (i + 1) * step);
    }
    cachedLength_ = total;
    return total;
}

double EllipseArc::signedArea() const {
    if (full_) {
        return kPi * semiMajor() * semiMinor();
    }
    // Area of the elliptical sector swept from start to end.
    return 0.5 * semiMajor() * semiMinor() * (sweep_ - std::sin(sweep_));
}

Box3 EllipseArc::bounds() const {
    Box3 box = Box3::fromCorners(startPoint(), endPoint());

    // Extremes along each world axis occur where the derivative's component
    // vanishes: -Mx*sin(u) + mx*cos(u) = 0, i.e. u = atan2(mx, Mx).
    const std::array<std::pair<double, double>, 3> axes{{
        {majorAxis.x, minorAxis_.x},
        {majorAxis.y, minorAxis_.y},
        {majorAxis.z, minorAxis_.z},
    }};

    for (const auto& [major, minor] : axes) {
        if (major == 0.0 && minor == 0.0) {
            continue;
        }
        const double u0 = std::atan2(minor, major);
        for (const double u : {u0, u0 + kPi}) {
            const double normalized = normalizeAngle(u);
            if (containsParam(normalized)) {
                box = box.expandedToPoint(pointAtParam(normalized));
            }
        }
    }
    return box;
}

namespace {

/// Newton iteration on `f(u) = (P(u) - p) . P'(u)`, whose roots are the
/// parameters where the curve is closest to (or furthest from) `p`.
[[nodiscard]] double newtonClosest(const EllipseArc& ellipse, const Vec3& p, double seed) {
    double u = seed;
    for (int i = 0; i < 24; ++i) {
        const Vec3 point = ellipse.pointAtParam(u);
        const Vec3 d1 = ellipse.derivativeAtParam(u);
        // Second derivative of the parameterization.
        const Vec3 d2 = ellipse.majorAxis * -std::cos(u) + ellipse.minorAxis() * -std::sin(u);
        const Vec3 diff = point - p;

        const double f = diff.dot(d1);
        const double df = d1.lengthSquared() + diff.dot(d2);
        if (std::abs(df) < 1e-18) {
            break;
        }
        const double stepSize = f / df;
        u -= stepSize;
        if (std::abs(stepSize) < 1e-14) {
            break;
        }
    }
    return u;
}

/// Eccentric anomaly of the closest point on the *full* ellipse.
///
/// Newton alone can converge to a local maximum on eccentric ellipses, so the
/// search is seeded from four candidates spread around the ellipse and the best
/// converged root wins.
[[nodiscard]] double solveClosestParam(const EllipseArc& ellipse, const Vec3& p) {
    const Vec3 d = p - ellipse.center;
    const double majorLength = ellipse.semiMajor();
    const double minorLength = ellipse.semiMinor();
    const double x = d.dot(ellipse.majorAxis) / (majorLength * majorLength);
    const double y = d.dot(ellipse.minorAxis()) / (minorLength * minorLength);
    const double seed = std::atan2(y * minorLength, x * majorLength);

    double best = seed;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int k = 0; k < 4; ++k) {
        const double u = newtonClosest(ellipse, p, seed + static_cast<double>(k) * kPi / 2.0);
        const double distance = p.distanceSquaredTo(ellipse.pointAtParam(u));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = u;
        }
    }
    return normalizeAngle(best);
}

}  // namespace

ClosestPointResult EllipseArc::closestPoint(const Vec3& p) const {
    const double u = solveClosestParam(*this, p);
    if (containsParam(u)) {
        const Vec3 point = pointAtParam(u);
        return {point, normalizedAt(u), p.distanceTo(point)};
    }

    const Vec3 s = startPoint();
    const Vec3 e = endPoint();
    const double ds = p.distanceTo(s);
    const double de = p.distanceTo(e);
    return ds <= de ? ClosestPointResult{s, 0.0, ds} : ClosestPointResult{e, 1.0, de};
}

std::optional<ClosestPointResult> EllipseArc::closestPointExtended(const Vec3& p) const {
    const double u = solveClosestParam(*this, p);
    const Vec3 point = pointAtParam(u);
    return ClosestPointResult{point, normalizedAt(u), p.distanceTo(point)};
}

CurvePtr EllipseArc::transformed(const Mat4& m) const {
    const Vec3 newCenter = m.transformPoint(center);
    const Vec3 major = m.transformVector(majorAxis);
    const Vec3 minor = m.transformVector(minorAxis_);
    const Vec3 newNormal = m.transformVector(normal).tryNormalize().value_or(normal);

    // A general affine map takes conjugate diameters to conjugate diameters,
    // not axes to axes. Rytz's construction recovers the true axes.
    const ConjugateAxes axes = conjugateToAxes(major, minor);
    const double lengthA = axes.a.length();
    const double lengthB = axes.b.length();
    const Vec3 newMajor = lengthA >= lengthB ? axes.a : axes.b;
    const Vec3 newMinor = lengthA >= lengthB ? axes.b : axes.a;

    const double newMajorLength = newMajor.length();
    const double newRatio =
        newMajorLength == 0.0 ? 1.0 : clamp(newMinor.length() / newMajorLength, 1e-12, 1.0);

    if (full_) {
        return std::make_unique<EllipseArc>(newCenter, newMajor, newRatio, 0.0, kTau, newNormal);
    }

    const EllipseArc reference{newCenter, newMajor, newRatio, 0.0, kTau, newNormal};
    const double s = reference.paramAtPoint(m.transformPoint(startPoint()));
    const double e = reference.paramAtPoint(m.transformPoint(endPoint()));

    return m.isMirrored()
               ? std::make_unique<EllipseArc>(newCenter, newMajor, newRatio, e, s, newNormal)
               : std::make_unique<EllipseArc>(newCenter, newMajor, newRatio, s, e, newNormal);
}

CurvePtr EllipseArc::reversed() const {
    if (full_) {
        return std::make_unique<EllipseArc>(center, majorAxis, ratio, 0.0, kTau, -normal);
    }
    return std::make_unique<EllipseArc>(center, majorAxis, ratio, endParam, startParam, normal);
}

int EllipseArc::segmentCount(const TessellationOptions& options) const {
    const int minimum = std::max(1, options.minSegments);
    const int maximum = std::max(minimum, options.maxSegments);

    const double a = semiMajor();
    const double b = semiMinor();
    if (a <= 0.0 || b <= 0.0) {
        return minimum;
    }

    // The semi-major axis, not the smallest radius of curvature.
    //
    // The circle formula below converts a tolerance into an angular step on the
    // assumption that a step of `dt` covers an arc of `R * dt`. On an ellipse
    // that is false: at the end of the major axis the curvature is tightest
    // (radius b^2/a) but the parametric speed is *b*, so a step there covers
    // `b * dt` and the sagitta works out at `a * dt^2 / 8` -- the deviation of
    // a circle of radius `a`. Feeding the curvature radius in instead
    // under-samples by (a/b)^2, which on a 200-by-80 ellipse is a factor of six
    // and plainly visible as a faceted outline.
    const double effectiveRadius = a;
    const double ratioValue = options.chordTolerance / effectiveRadius;
    if (ratioValue >= 1.0) {
        return minimum;
    }
    const double maxStep = 2.0 * std::acos(clamp(1.0 - ratioValue, -1.0, 1.0));
    if (!(maxStep > 0.0)) {
        return maximum;
    }
    const double needed = std::ceil(sweep_ / maxStep);
    return static_cast<int>(
        clamp(needed, static_cast<double>(minimum), static_cast<double>(maximum)));
}

std::vector<Vec3> EllipseArc::tessellate(const TessellationOptions& options) const {
    const int count = segmentCount(options);
    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(count) + 1);

    for (int i = 0; i <= count; ++i) {
        const double fraction = static_cast<double>(i) / static_cast<double>(count);
        points.push_back(pointAtParam(startParam + sweep_ * fraction));
    }
    points.front() = startPoint();
    points.back() = endPoint();
    return points;
}

std::optional<Arc> EllipseArc::toArcIfCircular(const Tolerance& tol) const {
    if (!isCircular(tol)) {
        return std::nullopt;
    }
    const double base = majorAxis.angle2();
    return Arc{center, semiMajor(), base + startParam, base + endParam, normal, full_};
}

}  // namespace cad
