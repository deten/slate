#pragma once

#include <optional>
#include <vector>

#include "cad/geom/Arc.hpp"
#include "cad/geom/Curve.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {

/// Editing operations on curves.
///
/// These are the geometric heart of the drafting commands — TRIM, EXTEND,
/// OFFSET, FILLET, CHAMFER, BREAK. They live in the geometry kernel rather than
/// in the commands because they are pure functions on geometry, so each can be
/// tested exhaustively without a database, a selection, or a UI.

// -------------------------------------------------------------------- offset

/// Offset a curve by a signed distance.
///
/// Positive is to the left of the direction of travel (the +Y side for a curve
/// running along +X), matching the `perp2` convention so the sign stays
/// consistent across curve types.
///
/// @returns the offset curve, or null when the offset is degenerate — an arc
///   offset inward by more than its radius has no result, and reporting that
///   honestly beats emitting an inside-out arc.
[[nodiscard]] CurvePtr offsetCurve(const Curve& curve, double distance,
                                   const Tolerance& tol = defaultTolerance());

// ---------------------------------------------------------------------- trim

struct TrimResult {
    /// The pieces that remain. Empty when the whole curve was removed.
    std::vector<CurvePtr> remaining;
    /// True when anything was actually cut.
    bool trimmed = false;
};

struct TrimOptions {
    Tolerance tolerance = defaultTolerance();
    /// Allow cutting edges to bound the curve on their extensions.
    bool extend = false;
};

/// Trim a curve against cutting edges, removing the piece containing
/// `pickPoint`.
///
/// This is exactly other CAD software's TRIM: the user picks the *part to remove*, and the
/// cutting edges bound it. The subtlety is that the removed span runs between
/// the two intersections that bracket the pick — not from the nearest
/// intersection to an end — and a pick beyond every intersection removes the
/// tail.
[[nodiscard]] TrimResult trimCurve(const Curve& curve,
                                   std::span<const Curve* const> cuttingEdges,
                                   const Vec3& pickPoint, const TrimOptions& options = {});

// -------------------------------------------------------------------- extend

/// Extend a curve to meet a boundary.
///
/// `pickPoint` selects which end moves: the end nearer the pick, matching how a
/// drafter indicates intent by clicking near an end.
///
/// @returns the extended curve, or null when no boundary lies beyond that end.
[[nodiscard]] CurvePtr extendCurve(const Curve& curve,
                                   std::span<const Curve* const> boundaries,
                                   const Vec3& pickPoint,
                                   const Tolerance& tol = defaultTolerance());

// -------------------------------------------------------------------- fillet

struct FilletResult {
    /// The arc joining the two curves. Zero-radius for a corner join.
    Arc arc{Vec3::zero(), 0.0, 0.0, 0.0, Vec3::unitZ(), true};
    /// The first curve, trimmed back to the arc.
    LineSegment trimmedA{Vec3::zero(), Vec3::zero()};
    LineSegment trimmedB{Vec3::zero(), Vec3::zero()};
};

/// Round the corner between two curves with an arc of the given radius.
///
/// The pick points matter: two lines have up to four possible fillets, one per
/// quadrant of their crossing, and the picks select which corner the user
/// meant. Ignoring them and choosing arbitrarily is the most common way a
/// fillet implementation feels wrong to use.
///
/// A radius of zero is a valid request meaning "extend both to the corner",
/// which is how other CAD software's FILLET joins two lines.
///
/// @returns empty when no fillet of that radius fits, or when the curves are
///   not both line segments — arc and mixed fillets are not yet implemented,
///   and returning empty is honest about that.
[[nodiscard]] std::optional<FilletResult> filletCurves(
    const Curve& a, const Curve& b, double radius, const Vec3& pickA, const Vec3& pickB,
    const Tolerance& tol = defaultTolerance());

// ------------------------------------------------------------------- chamfer

struct ChamferResult {
    LineSegment line{Vec3::zero(), Vec3::zero()};
    LineSegment trimmedA{Vec3::zero(), Vec3::zero()};
    LineSegment trimmedB{Vec3::zero(), Vec3::zero()};
};

/// Cut the corner between two lines with a straight bevel.
///
/// The distances are measured back along each line from their intersection,
/// matching other CAD software's two-distance chamfer.
[[nodiscard]] std::optional<ChamferResult> chamferLines(
    const LineSegment& a, const LineSegment& b, double distanceA, double distanceB,
    const Vec3& pickA, const Vec3& pickB, const Tolerance& tol = defaultTolerance());

// --------------------------------------------------------------------- break

/// Split a curve at two points, discarding the span between them.
///
/// With `second` omitted the curve is merely split, keeping both halves — which
/// is other CAD software's BREAK AT POINT.
[[nodiscard]] std::vector<CurvePtr> breakCurve(const Curve& curve, const Vec3& first,
                                               const std::optional<Vec3>& second = std::nullopt);

// ------------------------------------------------------------------- measure

/// Divide a curve into `count` equal-length spans, returning the interior
/// division points. Backs the DIVIDE command.
[[nodiscard]] std::vector<Vec3> dividePoints(const Curve& curve, int count);

/// Place points at a fixed spacing along a curve. Backs the MEASURE command.
[[nodiscard]] std::vector<Vec3> measurePoints(const Curve& curve, double spacing);

/// Point at an arc-length distance along a curve.
[[nodiscard]] Vec3 pointAtDistance(const Curve& curve, double distance);

}  // namespace cad
