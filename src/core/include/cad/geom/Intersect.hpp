#pragma once

#include <optional>
#include <vector>

#include "cad/geom/Arc.hpp"
#include "cad/geom/Curve.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

/// A single intersection between two curves.
///
/// Parameters are normalized to each curve's own `[0, 1]` domain. When a hit
/// lies on a curve's *extension* rather than its bounded extent, the parameter
/// falls outside `[0, 1]` and the corresponding flag is set. TRIM and EXTEND
/// both depend on this distinction, as does other CAD software's apparent-intersection
/// object snap.
struct Intersection {
    Vec3 point;
    double paramA = 0.0;
    double paramB = 0.0;
    bool onExtensionA = false;
    bool onExtensionB = false;
};

struct IntersectOptions {
    Tolerance tolerance = defaultTolerance();
    /// Include hits on the infinite or complete host geometry beyond a curve's
    /// own extent. Off by default: ordinary queries report real crossings only.
    bool extend = false;
    /// Tessellation quality for curve types with no closed-form solution.
    std::optional<TessellationOptions> tessellation;
};

/// Intersect any two curves.
///
/// Closed-form solutions are used for the pairs that dominate real drawings
/// (line/line, line/arc, arc/arc); everything else falls back to a tessellated
/// search refined by Newton iteration, which converges to full double precision
/// for the smooth curve types in this kernel.
[[nodiscard]] std::vector<Intersection> intersectCurves(const Curve& a, const Curve& b,
                                                        const IntersectOptions& options = {});

/// Intersections including those on curve extensions — the apparent
/// intersection used by TRIM and EXTEND with implied edges.
[[nodiscard]] std::vector<Intersection> intersectCurvesExtended(
    const Curve& a, const Curve& b, IntersectOptions options = {});

/// Line/line intersection in the XY plane.
///
/// Collinear overlapping segments report no intersection: there is no single
/// crossing point, and every consumer (TRIM, FILLET, the intersection snap)
/// needs a discrete point rather than an interval.
[[nodiscard]] std::vector<Intersection> intersectLineLine(
    const LineSegment& a, const LineSegment& b, const Tolerance& tol = defaultTolerance());

/// Line/circle intersection, reporting sweep membership through the flags.
[[nodiscard]] std::vector<Intersection> intersectLineArc(
    const LineSegment& line, const Arc& arc, const Tolerance& tol = defaultTolerance());

/// Circle/circle intersection. Concentric and non-coplanar cases report no
/// hits — both are genuinely "no discrete intersection" rather than errors.
[[nodiscard]] std::vector<Intersection> intersectArcArc(
    const Arc& a, const Arc& b, const Tolerance& tol = defaultTolerance());

/// Every intersection between one curve and a set of others, sorted along the
/// first curve. This is the exact query TRIM performs against its cutting
/// edges, so it lives here rather than being re-derived in the command.
[[nodiscard]] std::vector<Intersection> intersectAgainstAll(
    const Curve& curve, std::span<const Curve* const> others,
    const IntersectOptions& options = {});

/// Self-intersections of a polyline — needed to validate hatch boundaries and
/// to warn on malformed imported geometry.
[[nodiscard]] std::vector<Intersection> selfIntersections(const Polyline& poly,
                                                          const IntersectOptions& options = {});

}  // namespace cad
