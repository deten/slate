#include "cad/geom/Arc.hpp"

// Included in the implementation only. A non-conformal transform widens an arc
// into an ellipse, so Arc depends on EllipseArc here — but EllipseArc's header
// depends on Arc's, so keeping this include out of Arc.hpp is what breaks the
// cycle. The TypeScript prototype needed a runtime factory registration for
// the same problem; C++ separate compilation solves it directly.
#include "cad/geom/EllipseArc.hpp"

#include <algorithm>
#include <stdexcept>

namespace cad {
namespace {

const CoordinateFrame kWorldFrame{Vec3::unitX(), Vec3::unitY(), Vec3::unitZ()};

}  // namespace

Arc::Arc(const Vec3& centerPoint, double radiusValue, double start, double end,
         const Vec3& extrusion, bool forceFullCircle)
    : center(centerPoint),
      radius(radiusValue),
      startAngle(normalizeAngle(start)),
      endAngle(normalizeAngle(end)),
      normal(extrusion),
      frame_(isWorldNormal(extrusion) ? kWorldFrame : arbitraryAxis(extrusion)) {
    if (!(radiusValue >= 0.0) || !std::isfinite(radiusValue)) {
        throw std::invalid_argument("Arc radius must be finite and non-negative");
    }

    const double rawSweep = normalizeAngle(endAngle - startAngle);
    // A zero sweep is ambiguous: it means either a degenerate arc or a full
    // circle. Callers disambiguate with the flag; DXF disambiguates by entity
    // type, which the importer maps onto it.
    full_ = forceFullCircle || rawSweep == 0.0;
    sweep_ = full_ ? kTau : rawSweep;
}

std::optional<Arc> Arc::fromThreePoints(const Vec3& a, const Vec3& b, const Vec3& c,
                                        const Tolerance& tol) {
    const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) <= tol.point) {
        return std::nullopt;
    }

    const double aSq = a.x * a.x + a.y * a.y;
    const double bSq = b.x * b.x + b.y * b.y;
    const double cSq = c.x * c.x + c.y * c.y;
    const double ux = (aSq * (b.y - c.y) + bSq * (c.y - a.y) + cSq * (a.y - b.y)) / d;
    const double uy = (aSq * (c.x - b.x) + bSq * (a.x - c.x) + cSq * (b.x - a.x)) / d;

    const Vec3 center{ux, uy, a.z};
    const double radius = center.distance2To(a);
    if (radius <= tol.point) {
        return std::nullopt;
    }

    const double angleA = std::atan2(a.y - uy, a.x - ux);
    const double angleB = std::atan2(b.y - uy, b.x - ux);
    const double angleC = std::atan2(c.y - uy, c.x - ux);

    // Choose the sweep direction that actually passes through the middle point.
    const bool ccwPassesThroughB =
        normalizeAngle(angleB - angleA) <= normalizeAngle(angleC - angleA);
    return ccwPassesThroughB ? Arc{center, radius, angleA, angleC}
                             : Arc{center, radius, angleC, angleA};
}

std::optional<Arc> Arc::fromBulge(const Vec3& start, const Vec3& end, double bulgeValue,
                                  const Tolerance& tol) {
    if (std::abs(bulgeValue) <= 1e-12) {
        return std::nullopt;
    }
    const Vec3 chord = (end - start).flattened();
    const double chordLength = chord.length2();
    if (chordLength <= tol.point) {
        return std::nullopt;
    }

    const double included = 4.0 * std::atan(bulgeValue);
    const double radius = std::abs(chordLength / (2.0 * std::sin(included / 2.0)));
    // Distance from the chord midpoint to the centre, signed by direction.
    const double apothem = (chordLength / 2.0) / std::tan(included / 2.0);

    const Vec3 mid = start.midpoint(end);
    const Vec3 perpendicular = Vec3{-chord.y, chord.x, 0.0} * (1.0 / chordLength);
    const Vec3 center = mid.addScaled(perpendicular, apothem);

    const double start_ = std::atan2(start.y - center.y, start.x - center.x);
    const double end_ = std::atan2(end.y - center.y, end.x - center.x);

    // A negative bulge sweeps clockwise, which this class represents by
    // swapping the endpoints and keeping the counter-clockwise invariant.
    return bulgeValue > 0.0 ? Arc{center, radius, start_, end_}
                            : Arc{center, radius, end_, start_};
}

double Arc::paramAtAngle(double angle) const {
    const double offset = normalizeAngle(angle - startAngle);
    if (offset <= sweep_) {
        return offset / sweep_;
    }
    // Outside the sweep — decide which end of the gap it is closer to.
    const double gapFromEnd = offset - sweep_;
    const double gapToStart = kTau - offset;
    return gapFromEnd <= gapToStart ? 1.0 : 0.0;
}

Vec3 Arc::tangentAt(double t) const {
    const double angle = angleAt(clampParam(t));
    // d/dt of the position: the CCW tangent scaled by radius * sweep.
    return (frame_.xAxis * -std::sin(angle) + frame_.yAxis * std::cos(angle)) * (radius * sweep_);
}

double Arc::signedArea() const {
    // Full disc when closed, circular segment otherwise.
    if (full_) {
        return kPi * radius * radius;
    }
    return 0.5 * radius * radius * (sweep_ - std::sin(sweep_));
}

Box3 Arc::bounds() const {
    if (radius == 0.0) {
        return Box3::fromPoint(center);
    }

    if (full_) {
        // Exact bounds of a circle in an arbitrary plane: the extent along each
        // world axis is r * |projection of that axis onto the circle's plane|.
        const double ex = std::hypot(frame_.xAxis.x, frame_.yAxis.x) * radius;
        const double ey = std::hypot(frame_.xAxis.y, frame_.yAxis.y) * radius;
        const double ez = std::hypot(frame_.xAxis.z, frame_.yAxis.z) * radius;
        return Box3{Vec3{center.x - ex, center.y - ey, center.z - ez},
                    Vec3{center.x + ex, center.y + ey, center.z + ez}};
    }

    // Open arc: the endpoints plus any axis-extreme angle inside the sweep. For
    // a plane-aligned arc these are the quadrant points; for a tilted plane the
    // extremes occur at atan2 of the basis components, which is the general
    // form used here.
    Box3 box = Box3::fromCorners(startPoint(), endPoint());

    const std::array<std::pair<double, double>, 3> axes{{
        {frame_.xAxis.x, frame_.yAxis.x},
        {frame_.xAxis.y, frame_.yAxis.y},
        {frame_.xAxis.z, frame_.yAxis.z},
    }};

    for (const auto& [cx, cy] : axes) {
        if (cx == 0.0 && cy == 0.0) {
            continue;
        }
        const double extreme = std::atan2(cy, cx);
        for (const double angle : {extreme, extreme + kPi}) {
            if (containsAngle(normalizeAngle(angle))) {
                box = box.expandedToPoint(pointAtAngle(angle));
            }
        }
    }
    return box;
}

ClosestPointResult Arc::closestPoint(const Vec3& p) const {
    const Vec3 d = p - center;
    const double u = d.dot(frame_.xAxis);
    const double w = d.dot(frame_.yAxis);

    if (u == 0.0 && w == 0.0) {
        // The query point is on the axis through the centre: every point on the
        // arc is equidistant, so the start point is as good an answer as any.
        const Vec3 point = startPoint();
        return {point, 0.0, p.distanceTo(point)};
    }

    const double angle = normalizeAngle(std::atan2(w, u));
    if (containsAngle(angle)) {
        const Vec3 point = pointAtAngle(angle);
        return {point, paramAtAngle(angle), p.distanceTo(point)};
    }

    // Outside the sweep: the nearest point is whichever endpoint is closer.
    const Vec3 s = startPoint();
    const Vec3 e = endPoint();
    const double ds = p.distanceTo(s);
    const double de = p.distanceTo(e);
    return ds <= de ? ClosestPointResult{s, 0.0, ds} : ClosestPointResult{e, 1.0, de};
}

std::optional<ClosestPointResult> Arc::closestPointExtended(const Vec3& p) const {
    const Vec3 d = p - center;
    const double u = d.dot(frame_.xAxis);
    const double w = d.dot(frame_.yAxis);
    if (u == 0.0 && w == 0.0) {
        return std::nullopt;
    }
    const double angle = normalizeAngle(std::atan2(w, u));
    const Vec3 point = pointAtAngle(angle);
    return ClosestPointResult{point, paramAtAngle(angle), p.distanceTo(point)};
}

std::vector<Vec3> Arc::quadrantPoints() const {
    std::vector<Vec3> result;
    for (int i = 0; i < 4; ++i) {
        const double angle = static_cast<double>(i) * kPi / 2.0;
        if (containsAngle(angle)) {
            result.push_back(pointAtAngle(angle));
        }
    }
    return result;
}

CurvePtr Arc::transformed(const Mat4& m) const {
    // A non-conformal transform turns a circle into an ellipse. Rather than
    // silently distorting the radius, hand off to the ellipse type.
    if (!m.isConformal()) {
        return EllipseArc::fromArc(*this).transformed(m);
    }

    const Vec3 newCenter = m.transformPoint(center);
    const double newRadius = radius * m.scaleFactor();
    const Vec3 newNormal = m.transformVector(normal).tryNormalize().value_or(normal);

    if (full_) {
        return std::make_unique<Arc>(newCenter, newRadius, 0.0, 0.0, newNormal, true);
    }

    // Re-derive the angles from transformed endpoints so rotation and mirroring
    // are both handled without special-casing.
    const Arc reference{newCenter, newRadius, 0.0, 0.0, newNormal, true};
    const double s = reference.angleAtPoint(m.transformPoint(startPoint()));
    const double e = reference.angleAtPoint(m.transformPoint(endPoint()));

    // Mirroring reverses the sweep direction; swapping the angles restores the
    // counter-clockwise invariant while preserving the visible geometry.
    return m.isMirrored()
               ? std::make_unique<Arc>(newCenter, newRadius, e, s, newNormal)
               : std::make_unique<Arc>(newCenter, newRadius, s, e, newNormal);
}

CurvePtr Arc::reversed() const {
    if (full_) {
        return std::make_unique<Arc>(center, radius, startAngle, endAngle, -normal, true);
    }
    // The same points walked the other way. An arc always sweeps
    // counter-clockwise about its normal, so the reverse walk is the
    // counter-clockwise arc about the opposite normal. Flipping the normal
    // negates the frame's x axis and keeps its y axis, so an angle θ in this
    // frame is π - θ in the flipped one. Swapping the angles in the same
    // frame would give the *other* arc around the circle: the wrong shape,
    // not the wrong direction.
    return std::make_unique<Arc>(center, radius, kPi - endAngle, kPi - startAngle, -normal);
}

int Arc::segmentCount(const TessellationOptions& options) const {
    const int minimum = std::max(1, options.minSegments);
    const int maximum = std::max(minimum, options.maxSegments);
    if (radius <= 0.0) {
        return minimum;
    }

    // From the sagitta relation `s = r * (1 - cos(theta / 2))`, the maximum
    // angular step for a given deviation is `2 * acos(1 - tol / r)`.
    const double ratio = options.chordTolerance / radius;
    if (ratio >= 1.0) {
        return minimum;
    }
    const double maxStep = 2.0 * std::acos(clamp(1.0 - ratio, -1.0, 1.0));
    if (!(maxStep > 0.0)) {
        return maximum;
    }
    const double needed = std::ceil(sweep_ / maxStep);
    return static_cast<int>(clamp(needed, static_cast<double>(minimum),
                                  static_cast<double>(maximum)));
}

std::vector<Vec3> Arc::tessellate(const TessellationOptions& options) const {
    const int count = segmentCount(options);
    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(count) + 1);

    for (int i = 0; i <= count; ++i) {
        const double fraction = static_cast<double>(i) / static_cast<double>(count);
        points.push_back(pointAtAngle(startAngle + sweep_ * fraction));
    }

    // Snap the ends to the exact endpoints so adjacent tessellated curves meet
    // without visible gaps.
    points.front() = startPoint();
    points.back() = endPoint();
    return points;
}

bool Arc::equals(const Arc& other, const Tolerance& tol) const {
    if (!center.equals(other.center, tol)) {
        return false;
    }
    if (std::abs(radius - other.radius) > tol.point) {
        return false;
    }
    if (full_ != other.full_) {
        return false;
    }
    return full_ || (std::abs(startAngle - other.startAngle) <= tol.angle &&
                     std::abs(endAngle - other.endAngle) <= tol.angle);
}

}  // namespace cad
