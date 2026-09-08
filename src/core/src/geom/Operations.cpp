#include "cad/geom/Operations.hpp"

#include <algorithm>

#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/Intersect.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {
namespace {

[[nodiscard]] double bulgeOf(const Curve& curve) {
    const auto* arc = dynamic_cast<const Arc*>(&curve);
    return arc == nullptr ? 0.0 : arc->bulge();
}

/// Where two offset segments should meet.
///
/// Their true intersection when they have one — including on their extensions,
/// which is the convex-corner case — and otherwise the midpoint of the gap,
/// which keeps the outline continuous for near-tangent joints.
[[nodiscard]] Vec3 jointPoint(const Curve& a, const Curve& b, const Tolerance& tol) {
    IntersectOptions options;
    options.tolerance = tol;
    options.extend = true;

    const std::vector<Intersection> hits = intersectCurves(a, b, options);
    if (hits.empty()) {
        return a.endPoint().midpoint(b.startPoint());
    }
    // With several candidates, take the one nearest the original joint.
    const Vec3 target = a.endPoint();
    const Intersection* best = &hits.front();
    for (const Intersection& hit : hits) {
        if (hit.point.distanceTo(target) < best->point.distanceTo(target)) {
            best = &hit;
        }
    }
    return best->point;
}

/// Offset a polyline segment-by-segment, then repair the joints.
///
/// The joints are the whole problem. Offsetting each segment independently
/// leaves gaps at convex corners and crossings at concave ones. Walking
/// consecutive pairs and intersecting them is what produces a correct parallel
/// outline rather than a fringe of disconnected pieces.
[[nodiscard]] CurvePtr offsetPolyline(const Polyline& poly, double distance,
                                      const Tolerance& tol) {
    const auto& segments = poly.segments();
    if (segments.empty()) {
        return nullptr;
    }

    std::vector<CurvePtr> offsets;
    offsets.reserve(segments.size());
    for (const CurvePtr& segment : segments) {
        // A segment that collapses is dropped rather than aborting the whole
        // offset: a polyline with one tiny arc should still offset usefully.
        if (CurvePtr offset = offsetCurve(*segment, distance, tol)) {
            offsets.push_back(std::move(offset));
        }
    }
    if (offsets.empty()) {
        return nullptr;
    }

    std::vector<PolylineVertex> vertices;
    vertices.reserve(offsets.size() + 1);
    vertices.emplace_back(offsets.front()->startPoint(), bulgeOf(*offsets.front()));

    for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
        vertices.emplace_back(jointPoint(*offsets[i], *offsets[i + 1], tol),
                              bulgeOf(*offsets[i + 1]));
    }

    if (poly.isClosed()) {
        // Close the loop by intersecting the last with the first.
        const Vec3 joint = jointPoint(*offsets.back(), *offsets.front(), tol);
        vertices.front().position = joint;
    } else {
        vertices.emplace_back(offsets.back()->endPoint());
    }

    return std::make_unique<Polyline>(std::move(vertices), poly.isClosed(), poly.normal);
}

/// Unit direction along `segment` pointing from `corner` toward the picked side.
[[nodiscard]] std::optional<Vec3> directionAwayFromCorner(const LineSegment& segment,
                                                          const Vec3& corner, const Vec3& pick,
                                                          const Tolerance& tol) {
    const auto direction = segment.unitDirection(tol);
    if (!direction) {
        return std::nullopt;
    }
    const Vec3 pickOnSegment = segment.closestPoint(pick).point;
    const Vec3 toPick = pickOnSegment - corner;
    if (toPick.isZero(tol)) {
        // The pick landed on the corner itself; keep the segment's direction.
        return direction;
    }
    return direction->dot(toPick) >= 0.0 ? *direction : -*direction;
}

/// Shorten a segment so it ends at `newEnd`, keeping the picked side.
[[nodiscard]] LineSegment trimToCorner(const LineSegment& segment, const Vec3& newEnd,
                                       const Vec3& pick) {
    const double pickParam = segment.closestPoint(pick).param;
    const auto endProjection = segment.closestPointExtended(newEnd);
    const double endParam = endProjection ? endProjection->param : 0.5;

    // Keep the portion the user picked: if the pick lies beyond the new end,
    // the far end is the one retained.
    return pickParam > endParam ? LineSegment{newEnd, segment.end}
                                : LineSegment{segment.start, newEnd};
}

[[nodiscard]] std::optional<FilletResult> filletLines(const LineSegment& a, const LineSegment& b,
                                                      double radius, const Vec3& pickA,
                                                      const Vec3& pickB, const Tolerance& tol) {
    IntersectOptions options;
    options.tolerance = tol;
    options.extend = true;

    const std::vector<Intersection> hits = intersectCurves(a, b, options);
    if (hits.empty()) {
        return std::nullopt;  // Parallel lines cannot be filleted.
    }
    const Vec3 corner = hits.front().point;

    // Point each direction away from the corner, toward the side the user
    // picked. That choice is what selects the quadrant.
    const auto dirA = directionAwayFromCorner(a, corner, pickA, tol);
    const auto dirB = directionAwayFromCorner(b, corner, pickB, tol);
    if (!dirA || !dirB) {
        return std::nullopt;
    }

    const double angle = dirA->angleTo(*dirB, tol);
    // Collinear or antiparallel lines leave no corner to round.
    if (angle <= tol.angle || std::abs(angle - kPi) <= tol.angle) {
        return std::nullopt;
    }

    if (radius <= tol.point) {
        // Radius zero means "extend both to the corner".
        FilletResult result;
        result.arc = Arc{corner, 0.0, 0.0, 0.0, Vec3::unitZ(), true};
        result.trimmedA = trimToCorner(a, corner, pickA);
        result.trimmedB = trimToCorner(b, corner, pickB);
        return result;
    }

    // Distance from the corner to each tangent point.
    const double tangentDistance = radius / std::tan(angle / 2.0);
    const Vec3 tangentA = corner.addScaled(*dirA, tangentDistance);
    const Vec3 tangentB = corner.addScaled(*dirB, tangentDistance);

    // The arc centre lies along the angle bisector.
    const auto bisector = (*dirA + *dirB).tryNormalize(tol);
    if (!bisector) {
        return std::nullopt;
    }
    const Vec3 center = corner.addScaled(*bisector, radius / std::sin(angle / 2.0));

    // The requested radius must fit within at least one of the segments.
    if (tangentDistance > a.length() + tol.point && tangentDistance > b.length() + tol.point) {
        return std::nullopt;
    }

    const double startAngle = std::atan2(tangentA.y - center.y, tangentA.x - center.x);
    const double endAngle = std::atan2(tangentB.y - center.y, tangentB.x - center.x);

    // Choose the sweep that produces the minor arc; the major arc would loop
    // the long way round and is never what a fillet means.
    const double ccwSweep = normalizeAngle(endAngle - startAngle);

    FilletResult result;
    result.arc = ccwSweep <= kPi ? Arc{center, radius, startAngle, endAngle}
                                 : Arc{center, radius, endAngle, startAngle};
    result.trimmedA = trimToCorner(a, tangentA, pickA);
    result.trimmedB = trimToCorner(b, tangentB, pickB);
    return result;
}

/// Reverse the complement of a span on a closed curve.
[[nodiscard]] TrimResult trimClosed(const Curve& curve, const std::vector<double>& params,
                                    double pickParam) {
    if (params.size() < 2) {
        // A single intersection cannot bound a span on a closed curve, so there
        // is nothing well-defined to remove.
        TrimResult result;
        result.remaining.push_back(curve.clone());
        return result;
    }

    double lower = params.back();
    double upper = params.front();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const double a = params[i];
        const double b = params[(i + 1) % params.size()];
        const bool inSpan = (i == params.size() - 1) ? (pickParam >= a || pickParam < b)
                                                     : (pickParam >= a && pickParam < b);
        if (inSpan) {
            lower = a;
            upper = b;
            break;
        }
    }

    // What remains is the complement: from the upper bound round to the lower.
    TrimResult result;
    result.trimmed = true;
    if (upper <= lower) {
        result.remaining.push_back(curve.trimmed(upper, lower));
    } else {
        result.remaining.push_back(curve.trimmed(upper, 1.0));
        result.remaining.push_back(curve.trimmed(0.0, lower));
    }
    std::erase_if(result.remaining, [](const CurvePtr& c) { return c->length() <= 1e-9; });
    return result;
}

}  // namespace

CurvePtr offsetCurve(const Curve& curve, double distance, const Tolerance& tol) {
    if (std::abs(distance) <= tol.point) {
        return curve.clone();
    }

    if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
        const auto direction = line->unitDirection(tol);
        if (!direction) {
            return nullptr;
        }
        const Vec3 offset = direction->perp2() * distance;
        return std::make_unique<LineSegment>(line->start + offset, line->end + offset);
    }

    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        // Offsetting to the left of a counter-clockwise arc means moving
        // *inward*, toward the centre, because the centre is on the left. An
        // arc whose normal points down runs clockwise as seen from above, and
        // its left is outward.
        const double inward = arc->normal.z < 0.0 ? -distance : distance;
        const double newRadius = arc->radius - inward;
        if (newRadius <= tol.point) {
            return nullptr;
        }
        return std::make_unique<Arc>(arc->center, newRadius, arc->startAngle, arc->endAngle,
                                     arc->normal, arc->isFullCircle());
    }

    if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
        return offsetPolyline(*poly, distance, tol);
    }

    return nullptr;
}

TrimResult trimCurve(const Curve& curve, std::span<const Curve* const> cuttingEdges,
                     const Vec3& pickPoint, const TrimOptions& options) {
    IntersectOptions intersectOptions;
    intersectOptions.tolerance = options.tolerance;
    intersectOptions.extend = options.extend;

    const std::vector<Intersection> hits =
        intersectAgainstAll(curve, cuttingEdges, intersectOptions);

    std::vector<double> params;
    params.reserve(hits.size());
    for (const Intersection& hit : hits) {
        if (hit.paramA > 1e-9 && hit.paramA < 1.0 - 1e-9) {
            params.push_back(hit.paramA);
        }
    }
    std::sort(params.begin(), params.end());

    if (params.empty()) {
        TrimResult result;
        result.remaining.push_back(curve.clone());
        return result;
    }

    const double pickParam = curve.closestPoint(pickPoint).param;

    // A closed curve has no ends, so the removed span is between the two
    // intersections bracketing the pick, wrapping around if necessary.
    if (curve.isClosed()) {
        return trimClosed(curve, params, pickParam);
    }

    // Boundaries of the span containing the pick.
    double lower = 0.0;
    double upper = 1.0;
    for (const double t : params) {
        if (t <= pickParam) {
            lower = t;
        }
        if (t > pickParam) {
            upper = t;
            break;
        }
    }

    TrimResult result;
    result.trimmed = true;
    if (lower > 1e-9) {
        result.remaining.push_back(curve.trimmed(0.0, lower));
    }
    if (upper < 1.0 - 1e-9) {
        result.remaining.push_back(curve.trimmed(upper, 1.0));
    }
    return result;
}

CurvePtr extendCurve(const Curve& curve, std::span<const Curve* const> boundaries,
                     const Vec3& pickPoint, const Tolerance& tol) {
    const double pickParam = curve.closestPoint(pickPoint).param;
    const bool extendEnd = pickParam >= 0.5;

    IntersectOptions options;
    options.tolerance = tol;
    options.extend = true;
    const std::vector<Intersection> hits = intersectAgainstAll(curve, boundaries, options);

    std::vector<double> candidates;
    for (const Intersection& hit : hits) {
        // Only intersections beyond the relevant end can extend it.
        if (extendEnd ? hit.paramA > 1.0 + 1e-9 : hit.paramA < -1e-9) {
            candidates.push_back(hit.paramA);
        }
    }
    if (candidates.empty()) {
        return nullptr;
    }

    // The nearest boundary beyond the end is the one to stop at, so repeated
    // EXTEND walks outward one boundary at a time.
    const double target = extendEnd
                              ? *std::min_element(candidates.begin(), candidates.end())
                              : *std::max_element(candidates.begin(), candidates.end());

    if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
        const Vec3 point = line->pointAtExtended(target);
        return extendEnd ? std::make_unique<LineSegment>(line->start, point)
                         : std::make_unique<LineSegment>(point, line->end);
    }

    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        if (arc->isFullCircle()) {
            return nullptr;
        }
        // The parameter is outside [0,1], so convert it to an absolute angle.
        const double angle = arc->angleAt(target);
        return extendEnd ? std::make_unique<Arc>(arc->center, arc->radius, arc->startAngle,
                                                 angle, arc->normal)
                         : std::make_unique<Arc>(arc->center, arc->radius, angle,
                                                 arc->endAngle, arc->normal);
    }

    return nullptr;
}

std::optional<FilletResult> filletCurves(const Curve& a, const Curve& b, double radius,
                                         const Vec3& pickA, const Vec3& pickB,
                                         const Tolerance& tol) {
    if (radius < 0.0) {
        return std::nullopt;
    }

    const auto* lineA = dynamic_cast<const LineSegment*>(&a);
    const auto* lineB = dynamic_cast<const LineSegment*>(&b);
    if (lineA != nullptr && lineB != nullptr) {
        return filletLines(*lineA, *lineB, radius, pickA, pickB, tol);
    }

    // Arc and mixed fillets reduce to finding a circle tangent to both curves.
    // That general solver is not implemented yet; returning empty is honest,
    // and the command surfaces it as "cannot fillet these objects".
    return std::nullopt;
}

std::optional<ChamferResult> chamferLines(const LineSegment& a, const LineSegment& b,
                                          double distanceA, double distanceB, const Vec3& pickA,
                                          const Vec3& pickB, const Tolerance& tol) {
    IntersectOptions options;
    options.tolerance = tol;
    options.extend = true;

    const std::vector<Intersection> hits = intersectCurves(a, b, options);
    if (hits.empty()) {
        return std::nullopt;
    }
    const Vec3 corner = hits.front().point;

    const auto dirA = directionAwayFromCorner(a, corner, pickA, tol);
    const auto dirB = directionAwayFromCorner(b, corner, pickB, tol);
    if (!dirA || !dirB || dirA->isParallel(*dirB, tol)) {
        return std::nullopt;
    }

    const Vec3 pointA = corner.addScaled(*dirA, distanceA);
    const Vec3 pointB = corner.addScaled(*dirB, distanceB);

    ChamferResult result;
    result.line = LineSegment{pointA, pointB};
    result.trimmedA = trimToCorner(a, pointA, pickA);
    result.trimmedB = trimToCorner(b, pointB, pickB);
    return result;
}

std::vector<CurvePtr> breakCurve(const Curve& curve, const Vec3& first,
                                 const std::optional<Vec3>& second) {
    const double t1 = curve.closestPoint(first).param;
    if (!second) {
        const std::array<double, 1> params{t1};
        return curve.split(params);
    }

    const double t2 = curve.closestPoint(*second).param;
    const double lower = std::min(t1, t2);
    const double upper = std::max(t1, t2);

    std::vector<CurvePtr> pieces;
    if (lower > 1e-9) {
        pieces.push_back(curve.trimmed(0.0, lower));
    }
    if (upper < 1.0 - 1e-9) {
        pieces.push_back(curve.trimmed(upper, 1.0));
    }
    return pieces;
}

Vec3 pointAtDistance(const Curve& curve, double distance) {
    // Polylines expose an exact arc-length mapping; the other curve types in
    // this kernel have uniform parametric speed, so the normalized parameter is
    // proportional to arc length.
    if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
        return poly->pointAt(poly->paramAtLength(distance));
    }
    const double total = curve.length();
    if (total <= 0.0) {
        return curve.startPoint();
    }
    return curve.pointAt(clamp(distance / total, 0.0, 1.0));
}

std::vector<Vec3> dividePoints(const Curve& curve, int count) {
    if (count < 2) {
        return {};
    }
    const double total = curve.length();
    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(count) - 1);
    for (int i = 1; i < count; ++i) {
        points.push_back(pointAtDistance(curve, (total * i) / count));
    }
    return points;
}

std::vector<Vec3> measurePoints(const Curve& curve, double spacing) {
    if (spacing <= 0.0) {
        return {};
    }
    const double total = curve.length();
    std::vector<Vec3> points;
    for (double d = spacing; d < total - 1e-9; d += spacing) {
        points.push_back(pointAtDistance(curve, d));
    }
    return points;
}

}  // namespace cad
