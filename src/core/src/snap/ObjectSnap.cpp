#include "cad/snap/ObjectSnap.hpp"

#include <algorithm>
#include <array>

#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/Intersect.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {
namespace {

constexpr std::array<SnapMode, 13> kAllModes{
    SnapMode::Endpoint,      SnapMode::Midpoint,  SnapMode::Center,
    SnapMode::Node,          SnapMode::Quadrant,  SnapMode::Intersection,
    SnapMode::Extension,     SnapMode::Insertion, SnapMode::Perpendicular,
    SnapMode::Tangent,       SnapMode::Nearest,   SnapMode::ApparentIntersection,
    SnapMode::Parallel,
};

/// Snap priority, highest first.
///
/// The ordering is not arbitrary: it reflects how *specific* a snap is. An
/// endpoint is an exact, meaningful location; "nearest" is a fallback that
/// should win only when nothing better is in range.
[[nodiscard]] int priorityOf(SnapMode mode) noexcept {
    switch (mode) {
        case SnapMode::Endpoint:             return 100;
        case SnapMode::Intersection:         return 95;
        case SnapMode::Midpoint:             return 90;
        case SnapMode::Center:               return 85;
        case SnapMode::Node:                 return 80;
        case SnapMode::Quadrant:             return 75;
        case SnapMode::Insertion:            return 70;
        case SnapMode::Perpendicular:        return 60;
        case SnapMode::Tangent:              return 55;
        case SnapMode::ApparentIntersection: return 50;
        case SnapMode::Extension:            return 40;
        case SnapMode::Parallel:             return 35;
        case SnapMode::Nearest:              return 10;
        case SnapMode::None:
        case SnapMode::Clear:                break;
    }
    return 0;
}

/// Centre of a curve that has one.
[[nodiscard]] std::optional<Vec3> centerOf(const Curve& curve) {
    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        return arc->center;
    }
    if (const auto* ellipse = dynamic_cast<const EllipseArc*>(&curve)) {
        return ellipse->center;
    }
    if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
        // A polyline's arc segments each have a centre worth snapping to.
        for (const CurvePtr& segment : poly->segments()) {
            if (const auto* arc = dynamic_cast<const Arc*>(segment.get())) {
                return arc->center;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<Vec3> quadrantsOf(const Curve& curve) {
    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        return arc->quadrantPoints();
    }
    if (const auto* ellipse = dynamic_cast<const EllipseArc*>(&curve)) {
        const Vec3 minor = ellipse->minorAxis();
        return {ellipse->center + ellipse->majorAxis, ellipse->center - ellipse->majorAxis,
                ellipse->center + minor, ellipse->center - minor};
    }
    return {};
}

/// Foot of the perpendicular from `from` onto the curve.
///
/// Uses the *extended* projection, because other CAD software allows a perpendicular snap
/// to land beyond a segment's end — the resulting line is still perpendicular
/// to that segment's direction, which is what the user asked for.
[[nodiscard]] std::optional<Vec3> perpendicularFrom(const Curve& curve, const Vec3& from) {
    if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
        return line->perpendicularFoot(from);
    }

    if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
        std::optional<Vec3> best;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (const CurvePtr& segment : poly->segments()) {
            const auto* line = dynamic_cast<const LineSegment*>(segment.get());
            if (line == nullptr) {
                continue;
            }
            const auto foot = line->perpendicularFoot(from);
            if (!foot) {
                continue;
            }
            const double distance = from.distanceTo(*foot);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = foot;
            }
        }
        return best;
    }

    // On a circle or arc the perpendicular from an external point passes
    // through the centre, so the foot is where that line meets the curve.
    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        const auto direction = (from - arc->center).tryNormalize();
        if (!direction) {
            return std::nullopt;
        }
        return arc->center.addScaled(*direction, arc->radius);
    }

    return curve.closestPoint(from).point;
}

/// Tangent points on a circle from an external point.
///
/// The tangent points lie where the circle meets the circle whose diameter is
/// the segment from the centre to the external point — Thales' construction.
[[nodiscard]] std::vector<Vec3> tangentPointsFrom(const Curve& curve, const Vec3& from,
                                                  const Tolerance& tol) {
    const auto* arc = dynamic_cast<const Arc*>(&curve);
    if (arc == nullptr) {
        return {};
    }

    const Vec3 toPoint = from - arc->center;
    const double distance = toPoint.length2();
    // A point inside the circle has no tangent to it.
    if (distance <= arc->radius + tol.point) {
        return {};
    }

    // Measured at the circle's centre: the tangent points sit at +/- acos(r/d)
    // either side of the direction toward the external point. Their radius is
    // perpendicular to the tangent line, which is what makes the construction
    // work.
    const double angleToPoint = std::atan2(toPoint.y, toPoint.x);
    const double offset = std::acos(arc->radius / distance);

    std::vector<Vec3> results;
    for (const double sign : {1.0, -1.0}) {
        const double angle = angleToPoint + sign * offset;
        if (arc->isFullCircle() || arc->containsAngle(angle, tol)) {
            results.push_back(arc->pointAtAngle(angle));
        }
    }
    return results;
}

}  // namespace

std::span<const SnapMode> allSnapModes() noexcept {
    return kAllModes;
}

std::string_view snapModeName(SnapMode mode) noexcept {
    switch (mode) {
        case SnapMode::Endpoint:             return "Endpoint";
        case SnapMode::Midpoint:             return "Midpoint";
        case SnapMode::Center:               return "Center";
        case SnapMode::Node:                 return "Node";
        case SnapMode::Quadrant:             return "Quadrant";
        case SnapMode::Intersection:         return "Intersection";
        case SnapMode::Insertion:            return "Insertion";
        case SnapMode::Perpendicular:        return "Perpendicular";
        case SnapMode::Tangent:              return "Tangent";
        case SnapMode::Nearest:              return "Nearest";
        case SnapMode::ApparentIntersection: return "Apparent Intersection";
        case SnapMode::Extension:            return "Extension";
        case SnapMode::Parallel:             return "Parallel";
        case SnapMode::None:
        case SnapMode::Clear:                break;
    }
    return "Snap";
}

std::optional<SnapPoint> SnapEngine::findBest(const SnapQuery& query) const {
    std::vector<SnapPoint> all = findAll(query);
    if (all.empty()) {
        return std::nullopt;
    }
    // Sort by priority first, then proximity. findAll already filtered to
    // within the aperture, so the highest-priority snap in range wins.
    std::sort(all.begin(), all.end(), [](const SnapPoint& a, const SnapPoint& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.distance < b.distance;
    });
    return all.front();
}

std::vector<SnapPoint> SnapEngine::findAll(const SnapQuery& query) const {
    std::vector<SnapPoint> results;

    if (hasMode(query.modes, SnapMode::Clear) || query.modes == 0) {
        return results;
    }

    const auto add = [&](const Vec3& position, SnapMode mode, Handle handle) {
        const double distance = query.cursor.distance2To(position);
        if (distance > query.aperture) {
            return;
        }
        results.push_back({position, mode, handle, distance, priorityOf(mode)});
    };

    for (const SnapCandidate& candidate : query.candidates) {
        // Insertion snaps to a block reference's or text's placement point.
        if (hasMode(query.modes, SnapMode::Insertion) && candidate.insertionPoint != nullptr) {
            add(*candidate.insertionPoint, SnapMode::Insertion, candidate.handle);
        }

        // Node snaps to POINT entities, used heavily in survey workflows.
        if (hasMode(query.modes, SnapMode::Node) && candidate.isNode &&
            candidate.insertionPoint != nullptr) {
            add(*candidate.insertionPoint, SnapMode::Node, candidate.handle);
        }

        for (const CurvePtr& curvePtr : candidate.curves) {
            const Curve& curve = *curvePtr;

            if (hasMode(query.modes, SnapMode::Endpoint)) {
                if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
                    // Every vertex is an endpoint, not just the two ends.
                    for (const PolylineVertex& vertex : poly->vertices) {
                        add(vertex.position, SnapMode::Endpoint, candidate.handle);
                    }
                } else if (!curve.isClosed()) {
                    add(curve.startPoint(), SnapMode::Endpoint, candidate.handle);
                    add(curve.endPoint(), SnapMode::Endpoint, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Midpoint)) {
                if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
                    // Each segment has its own midpoint; the whole polyline's
                    // is not useful and is not what other CAD software offers.
                    for (const CurvePtr& segment : poly->segments()) {
                        add(segment->midPoint(), SnapMode::Midpoint, candidate.handle);
                    }
                } else {
                    add(curve.midPoint(), SnapMode::Midpoint, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Center)) {
                if (const auto center = centerOf(curve)) {
                    add(*center, SnapMode::Center, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Quadrant)) {
                for (const Vec3& quadrant : quadrantsOf(curve)) {
                    add(quadrant, SnapMode::Quadrant, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Perpendicular) && query.basePoint != nullptr) {
                if (const auto foot = perpendicularFrom(curve, *query.basePoint)) {
                    add(*foot, SnapMode::Perpendicular, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Tangent) && query.basePoint != nullptr) {
                for (const Vec3& point :
                     tangentPointsFrom(curve, *query.basePoint, query.tolerance)) {
                    add(point, SnapMode::Tangent, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Extension)) {
                const auto extended = curve.closestPointExtended(query.cursor);
                // Only report it when the point is genuinely off the curve's
                // own extent; otherwise this duplicates Nearest at lower
                // priority.
                if (extended && (extended->param < 0.0 || extended->param > 1.0)) {
                    add(extended->point, SnapMode::Extension, candidate.handle);
                }
            }

            if (hasMode(query.modes, SnapMode::Nearest)) {
                add(curve.closestPoint(query.cursor).point, SnapMode::Nearest, candidate.handle);
            }
        }
    }

    const bool wantsReal = hasMode(query.modes, SnapMode::Intersection);
    const bool wantsApparent = hasMode(query.modes, SnapMode::ApparentIntersection);

    if (wantsReal || wantsApparent) {
        struct Entry {
            const Curve* curve;
            Handle handle;
        };
        std::vector<Entry> curves;
        for (const SnapCandidate& candidate : query.candidates) {
            for (const CurvePtr& curve : candidate.curves) {
                curves.push_back({curve.get(), candidate.handle});
            }
        }

        IntersectOptions options;
        options.tolerance = query.tolerance;
        options.extend = wantsApparent;

        int pairs = 0;
        for (std::size_t i = 0; i < curves.size() && pairs < maxIntersectionPairs; ++i) {
            for (std::size_t j = i + 1; j < curves.size() && pairs < maxIntersectionPairs; ++j) {
                ++pairs;
                for (const Intersection& hit :
                     intersectCurves(*curves[i].curve, *curves[j].curve, options)) {
                    const bool onBoth = !hit.onExtensionA && !hit.onExtensionB;
                    if (onBoth && wantsReal) {
                        add(hit.point, SnapMode::Intersection, curves[i].handle);
                    } else if (!onBoth && wantsApparent) {
                        add(hit.point, SnapMode::ApparentIntersection, curves[i].handle);
                    }
                }
            }
        }
    }

    return results;
}

}  // namespace cad
