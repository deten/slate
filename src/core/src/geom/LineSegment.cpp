#include "cad/geom/LineSegment.hpp"

namespace cad {

ClosestPointResult LineSegment::closestPoint(const Vec3& p) const {
    const Vec3 d = direction();
    const double lengthSquared = d.lengthSquared();
    if (lengthSquared == 0.0) {
        return {start, 0.0, p.distanceTo(start)};
    }
    const double t = clamp((p - start).dot(d) / lengthSquared, 0.0, 1.0);
    const Vec3 point = start.addScaled(d, t);
    return {point, t, p.distanceTo(point)};
}

std::optional<ClosestPointResult> LineSegment::closestPointExtended(const Vec3& p) const {
    const Vec3 d = direction();
    const double lengthSquared = d.lengthSquared();
    if (lengthSquared == 0.0) {
        return std::nullopt;
    }
    const double t = (p - start).dot(d) / lengthSquared;
    const Vec3 point = start.addScaled(d, t);
    return ClosestPointResult{point, t, p.distanceTo(point)};
}

double LineSegment::signedDistance2(const Vec3& p) const {
    const Vec3 d = direction().flattened();
    const double len = d.length2();
    if (len == 0.0) {
        return p.distance2To(start);
    }
    return d.cross2(p - start) / len;
}

std::optional<LineSegment> LineSegment::extendedTo(const Vec3& p, const Tolerance& tol) const {
    if (!isPointOnLine(p, tol)) {
        return std::nullopt;
    }
    const auto projection = closestPointExtended(p);
    if (!projection) {
        return std::nullopt;
    }
    // Below the start: the new segment runs from p to the current end.
    // Beyond the end: it runs from the current start to p.
    return projection->param < 0.5 ? LineSegment{p, end} : LineSegment{start, p};
}

}  // namespace cad
