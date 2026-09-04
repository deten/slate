#include "cad/geom/Intersect.hpp"

#include <algorithm>

#include "cad/geom/EllipseArc.hpp"

namespace cad {
namespace {

/// Parametric slack equivalent to the linear tolerance on a given curve.
///
/// Without this, a hit exactly at an endpoint would be classified as being on
/// the extension purely through floating-point noise.
[[nodiscard]] double tolParam(const Curve& curve, const Tolerance& tol) {
    const double len = curve.length();
    return len > 0.0 ? tol.point / len : 0.0;
}

/// Reverse the roles of the two curves in a result set.
[[nodiscard]] std::vector<Intersection> swapRoles(std::vector<Intersection> hits) {
    for (Intersection& hit : hits) {
        std::swap(hit.paramA, hit.paramB);
        std::swap(hit.onExtensionA, hit.onExtensionB);
    }
    return hits;
}

/// Drop duplicate hits.
///
/// Tangency and shared endpoints routinely produce the same point twice, and
/// downstream commands must not see that as two crossings.
[[nodiscard]] std::vector<Intersection> dedupe(const std::vector<Intersection>& hits,
                                               const Tolerance& tol) {
    const double threshold = std::max(tol.point, 1e-9);
    std::vector<Intersection> out;
    out.reserve(hits.size());

    for (const Intersection& hit : hits) {
        const bool duplicate = std::any_of(
            out.begin(), out.end(), [&](const Intersection& existing) {
                return existing.point.distanceTo(hit.point) <= threshold;
            });
        if (!duplicate) {
            out.push_back(hit);
        }
    }
    return out;
}

/// Newton iteration on `A(u) - B(v) = 0`, projected onto the two tangent
/// directions to give a well-conditioned 2x2 system.
[[nodiscard]] std::optional<std::pair<double, double>> newtonRefine(const Curve& a, const Curve& b,
                                                                    double u0, double v0) {
    double u = u0;
    double v = v0;

    for (int i = 0; i < 32; ++i) {
        const Vec3 pa = a.pointAt(u);
        const Vec3 pb = b.pointAt(v);
        const Vec3 diff = pa - pb;
        if (diff.lengthSquared() < 1e-28) {
            return std::pair{u, v};
        }

        const Vec3 ta = a.tangentAt(u);
        const Vec3 tb = b.tangentAt(v);
        // Solve [ta, -tb] * [du, dv]^T = -diff in the XY plane.
        const double det = ta.cross2(-tb);
        if (std::abs(det) < 1e-18) {
            return std::nullopt;
        }
        const double du = (-diff).cross2(-tb) / det;
        const double dv = ta.cross2(-diff) / det;

        u = clamp(u + du, 0.0, 1.0);
        v = clamp(v + dv, 0.0, 1.0);
        if (std::abs(du) < 1e-15 && std::abs(dv) < 1e-15) {
            break;
        }
    }
    return std::pair{u, v};
}

/// Pick a tessellation fine enough that the coarse sweep cannot miss a
/// crossing: the chord tolerance is scaled to the smaller of the two curves'
/// extents rather than left at a fixed world-unit default, which would be far
/// too coarse for small geometry and far too fine for large.
[[nodiscard]] TessellationOptions refineTessellation(const Curve& a, const Curve& b,
                                                     const IntersectOptions& options) {
    if (options.tessellation) {
        return *options.tessellation;
    }
    const double scale = std::min(a.bounds().diagonal(), b.bounds().diagonal());
    if (!std::isfinite(scale) || scale <= 0.0) {
        return TessellationOptions{};
    }
    return TessellationOptions{scale * 1e-4, 2048, 16};
}

/// Generic intersection for curve pairs with no closed form (anything
/// involving an ellipse, and future splines).
///
/// Two stages: a coarse tessellated sweep finds bracketing segment pairs, then
/// Newton polishes each bracket to machine precision. Tessellation alone is not
/// accurate enough for CAD; Newton alone has no reliable seed.
[[nodiscard]] std::vector<Intersection> intersectNumeric(const Curve& a, const Curve& b,
                                                         const IntersectOptions& options) {
    const Tolerance tol = options.tolerance;
    const TessellationOptions tess = refineTessellation(a, b, options);

    const std::vector<Vec3> pointsA = a.tessellate(tess);
    const std::vector<Vec3> pointsB = b.tessellate(tess);
    if (pointsA.size() < 2 || pointsB.size() < 2) {
        return {};
    }

    const auto spanA = static_cast<double>(pointsA.size() - 1);
    const auto spanB = static_cast<double>(pointsB.size() - 1);

    std::vector<std::pair<double, double>> seeds;
    for (std::size_t i = 0; i + 1 < pointsA.size(); ++i) {
        const LineSegment segA{pointsA[i], pointsA[i + 1]};
        const Box3 boundsA = segA.bounds();

        for (std::size_t j = 0; j + 1 < pointsB.size(); ++j) {
            const LineSegment segB{pointsB[j], pointsB[j + 1]};
            if (!boundsA.intersects2(segB.bounds(), tol)) {
                continue;
            }
            for (const Intersection& hit : intersectLineLine(segA, segB, tol)) {
                if (hit.onExtensionA || hit.onExtensionB) {
                    continue;
                }
                seeds.emplace_back((static_cast<double>(i) + clamp(hit.paramA, 0.0, 1.0)) / spanA,
                                   (static_cast<double>(j) + clamp(hit.paramB, 0.0, 1.0)) / spanB);
            }
        }
    }

    std::vector<Intersection> results;
    for (const auto& [u0, v0] : seeds) {
        const auto refined = newtonRefine(a, b, u0, v0);
        if (!refined) {
            continue;
        }
        const Vec3 point = a.pointAt(refined->first);
        if (point.distanceTo(b.pointAt(refined->second)) > tol.point * 1e4) {
            continue;
        }
        results.push_back({point, refined->first, refined->second, false, false});
    }
    return results;
}

/// Split a composite curve, intersect each piece, and remap the parameters.
[[nodiscard]] std::vector<Intersection> intersectComposite(const Polyline& poly, const Curve& other,
                                                           const IntersectOptions& options,
                                                           bool swapped) {
    std::vector<Intersection> results;
    const auto& segs = poly.segments();

    for (std::size_t i = 0; i < segs.size(); ++i) {
        for (const Intersection& hit : intersectCurves(*segs[i], other, options)) {
            // Remap the segment-local parameter onto the whole polyline.
            const double polyParam = poly.makeParam(i, clamp(hit.paramA, 0.0, 1.0));
            if (swapped) {
                results.push_back({hit.point, hit.paramB, polyParam, hit.onExtensionB,
                                   hit.onExtensionA});
            } else {
                results.push_back({hit.point, polyParam, hit.paramB, hit.onExtensionA,
                                   hit.onExtensionB});
            }
        }
    }
    return dedupe(results, options.tolerance);
}

}  // namespace

std::vector<Intersection> intersectLineLine(const LineSegment& a, const LineSegment& b,
                                            const Tolerance& tol) {
    const Vec3 da = a.direction();
    const Vec3 db = b.direction();
    const double denominator = da.cross2(db);
    const Vec3 delta = b.start - a.start;

    // Parallel or collinear: no discrete crossing point.
    if (std::abs(denominator) <= tol.point * std::max(1.0, da.length2() * db.length2())) {
        return {};
    }

    const double ta = delta.cross2(db) / denominator;
    const double tb = delta.cross2(da) / denominator;
    const double slackA = tolParam(a, tol);
    const double slackB = tolParam(b, tol);

    return {{a.pointAtExtended(ta), ta, tb, ta < -slackA || ta > 1.0 + slackA,
             tb < -slackB || tb > 1.0 + slackB}};
}

std::vector<Intersection> intersectLineArc(const LineSegment& line, const Arc& arc,
                                           const Tolerance& tol) {
    const Vec3 d = line.direction();
    const double lengthSquared = d.lengthSquared();
    if (lengthSquared <= tol.point * tol.point) {
        return {};
    }

    // Solve |start + t*d - center|^2 = r^2 for t.
    const Vec3 f = line.start - arc.center;
    const double aCoefficient = lengthSquared;
    const double bCoefficient = 2.0 * f.dot(d);
    const double cCoefficient = f.lengthSquared() - arc.radius * arc.radius;
    const double discriminant = bCoefficient * bCoefficient - 4.0 * aCoefficient * cCoefficient;

    // Scale the tangency threshold by the geometry size so that large-radius
    // near-tangencies are still recognized as single hits.
    const double tangentEpsilon = tol.point * std::max(1.0, aCoefficient * arc.radius);
    if (discriminant < -tangentEpsilon) {
        return {};
    }

    std::vector<double> params;
    if (discriminant <= tangentEpsilon) {
        params.push_back(-bCoefficient / (2.0 * aCoefficient));
    } else {
        const double root = std::sqrt(discriminant);
        params.push_back((-bCoefficient - root) / (2.0 * aCoefficient));
        params.push_back((-bCoefficient + root) / (2.0 * aCoefficient));
    }

    const double slack = tolParam(line, tol);
    std::vector<Intersection> results;
    results.reserve(params.size());

    for (const double t : params) {
        const Vec3 point = line.pointAtExtended(t);
        const double angle = arc.angleAtPoint(point);
        results.push_back({point, t, arc.paramAtAngle(angle), t < -slack || t > 1.0 + slack,
                           !arc.containsAngle(angle, tol)});
    }
    return results;
}

std::vector<Intersection> intersectArcArc(const Arc& a, const Arc& b, const Tolerance& tol) {
    // Only coplanar arcs meet at discrete points in the general case; this
    // kernel's 2D scope means both normals are almost always +Z.
    if (!a.normal.isParallel(b.normal, tol)) {
        return {};
    }

    const Vec3 delta = b.center - a.center;
    const double distance = delta.length();
    const double ra = a.radius;
    const double rb = b.radius;

    if (distance <= tol.point) {
        return {};  // Concentric: coincident or disjoint.
    }
    if (distance > ra + rb + tol.point) {
        return {};  // Too far apart.
    }
    if (distance < std::abs(ra - rb) - tol.point) {
        return {};  // One inside the other.
    }

    // Distance from a.center to the radical line, along `delta`.
    const double along = (distance * distance + ra * ra - rb * rb) / (2.0 * distance);
    const double heightSquared = ra * ra - along * along;
    const double height = heightSquared <= 0.0 ? 0.0 : std::sqrt(heightSquared);

    const Vec3 unit = delta * (1.0 / distance);
    const Vec3 perpendicular = a.normal.cross(unit).tryNormalize().value_or(Vec3::unitY());
    const Vec3 base = a.center.addScaled(unit, along);

    std::vector<Vec3> points;
    if (height <= tol.point) {
        points.push_back(base);
    } else {
        points.push_back(base.addScaled(perpendicular, height));
        points.push_back(base.addScaled(perpendicular, -height));
    }

    std::vector<Intersection> results;
    results.reserve(points.size());
    for (const Vec3& point : points) {
        const double angleA = a.angleAtPoint(point);
        const double angleB = b.angleAtPoint(point);
        results.push_back({point, a.paramAtAngle(angleA), b.paramAtAngle(angleB),
                           !a.containsAngle(angleA, tol), !b.containsAngle(angleB, tol)});
    }
    return results;
}

std::vector<Intersection> intersectCurves(const Curve& a, const Curve& b,
                                          const IntersectOptions& options) {
    const Tolerance tol = options.tolerance;

    // Polylines decompose into their segments; this covers polyline/anything
    // and keeps the closed-form fast paths reachable for the pieces.
    if (const auto* polyA = dynamic_cast<const Polyline*>(&a)) {
        return intersectComposite(*polyA, b, options, false);
    }
    if (const auto* polyB = dynamic_cast<const Polyline*>(&b)) {
        return intersectComposite(*polyB, a, options, true);
    }

    const auto* lineA = dynamic_cast<const LineSegment*>(&a);
    const auto* lineB = dynamic_cast<const LineSegment*>(&b);
    const auto* arcA = dynamic_cast<const Arc*>(&a);
    const auto* arcB = dynamic_cast<const Arc*>(&b);

    std::vector<Intersection> raw;
    if (lineA != nullptr && lineB != nullptr) {
        raw = intersectLineLine(*lineA, *lineB, tol);
    } else if (lineA != nullptr && arcB != nullptr) {
        raw = intersectLineArc(*lineA, *arcB, tol);
    } else if (arcA != nullptr && lineB != nullptr) {
        raw = swapRoles(intersectLineArc(*lineB, *arcA, tol));
    } else if (arcA != nullptr && arcB != nullptr) {
        raw = intersectArcArc(*arcA, *arcB, tol);
    } else {
        raw = intersectNumeric(a, b, options);
    }

    if (!options.extend) {
        std::erase_if(raw, [](const Intersection& hit) {
            return hit.onExtensionA || hit.onExtensionB;
        });
    }
    return dedupe(raw, tol);
}

std::vector<Intersection> intersectCurvesExtended(const Curve& a, const Curve& b,
                                                  IntersectOptions options) {
    options.extend = true;
    return intersectCurves(a, b, options);
}

std::vector<Intersection> intersectAgainstAll(const Curve& curve,
                                              std::span<const Curve* const> others,
                                              const IntersectOptions& options) {
    std::vector<Intersection> all;
    for (const Curve* other : others) {
        if (other == nullptr || other == &curve) {
            continue;
        }
        const std::vector<Intersection> hits = intersectCurves(curve, *other, options);
        all.insert(all.end(), hits.begin(), hits.end());
    }
    std::sort(all.begin(), all.end(),
              [](const Intersection& x, const Intersection& y) { return x.paramA < y.paramA; });
    return dedupe(all, options.tolerance);
}

std::vector<Intersection> selfIntersections(const Polyline& poly,
                                            const IntersectOptions& options) {
    const auto& segs = poly.segments();
    std::vector<Intersection> results;

    for (std::size_t i = 0; i < segs.size(); ++i) {
        // Start at i + 2: adjacent segments always meet at their shared vertex,
        // which is not a self-intersection.
        for (std::size_t j = i + 2; j < segs.size(); ++j) {
            // On a closed polyline the first and last segments are likewise
            // adjacent, meeting at the closing vertex.
            if (poly.isClosed() && i == 0 && j == segs.size() - 1) {
                continue;
            }
            for (const Intersection& hit : intersectCurves(*segs[i], *segs[j], options)) {
                results.push_back({hit.point, poly.makeParam(i, hit.paramA),
                                   poly.makeParam(j, hit.paramB), false, false});
            }
        }
    }
    return dedupe(results, options.tolerance);
}

}  // namespace cad
