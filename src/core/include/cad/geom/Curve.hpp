#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "cad/math/Box3.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/math/Tolerance.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

class Curve;
using CurvePtr = std::unique_ptr<Curve>;

/// Result of projecting a point onto a curve.
struct ClosestPointResult {
    /// The closest position on the curve.
    Vec3 point;
    /// Its normalized parameter, in `[0, 1]`.
    double param = 0.0;
    /// Distance from the query point to `point`.
    double distance = 0.0;
};

/// Controls how a curve is converted to a polyline.
struct TessellationOptions {
    /// Maximum allowed distance between the true curve and its chord, in world
    /// units. This is the only quality knob that produces resolution-independent
    /// results: the renderer sets it from the current zoom so curves are smooth
    /// at any scale without over-tessellating when zoomed out.
    double chordTolerance = 1e-3;
    /// Hard cap on segment count, to bound work on pathological input.
    int maxSegments = 4096;
    /// Lower bound on segment count, so tiny arcs still look round.
    int minSegments = 4;
};

/// Base class for all parametric curves in the geometry kernel.
///
/// Every curve is parameterized over a normalized `[0, 1]` range regardless of
/// its natural parameterization. That is what lets generic algorithms — trim,
/// split, extend, offset, intersection bookkeeping — operate on any curve type
/// without a type switch, including types added later.
///
/// Curves are immutable value objects. They carry no identity, no layer, and no
/// database presence: entities *own* curves and expose them. This keeps the
/// geometry kernel independently testable and reusable — an offset operation
/// should not need a database.
///
/// Polymorphic rather than a closed `std::variant` because the brief requires
/// plugin extensibility, and a variant would fix the curve set at compile time.
/// The cost is an allocation per curve; hot paths that cannot pay it (the
/// renderer) tessellate into shared point buffers instead of materialising
/// curve objects per frame.
class Curve {
public:
    Curve() = default;
    virtual ~Curve() = default;

    /// Polymorphic copy. Used wherever a curve of unknown type must be
    /// duplicated; concrete types are additionally copyable as plain values.
    [[nodiscard]] virtual CurvePtr clone() const = 0;

    [[nodiscard]] virtual Vec3 startPoint() const = 0;
    [[nodiscard]] virtual Vec3 endPoint() const = 0;

    /// True when start and end coincide and the curve encloses an area.
    [[nodiscard]] virtual bool isClosed() const = 0;

    /// Position at normalized parameter `t`, clamped to `[0, 1]`.
    [[nodiscard]] virtual Vec3 pointAt(double t) const = 0;

    /// Non-normalized tangent at `t`, pointing in the direction of increasing
    /// parameter. Its magnitude is the parametric speed and carries no meaning
    /// to callers; normalize it for a direction.
    [[nodiscard]] virtual Vec3 tangentAt(double t) const = 0;

    /// Arc length of the whole curve.
    [[nodiscard]] virtual double length() const = 0;

    /// Tight axis-aligned bounds.
    [[nodiscard]] virtual Box3 bounds() const = 0;

    /// Project a point onto the curve, restricted to the curve's own extent.
    [[nodiscard]] virtual ClosestPointResult closestPoint(const Vec3& p) const = 0;

    /// Project onto the curve's *unbounded* host geometry — the infinite line
    /// for a segment, the full circle for an arc. Empty for curves with no
    /// meaningful extension.
    ///
    /// This is what apparent-intersection, EXTEND and the extension object snap
    /// are built on, and why extension is a first-class kernel concept rather
    /// than special-cased per command.
    [[nodiscard]] virtual std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const = 0;

    /// Apply an affine transform, returning the transformed curve.
    ///
    /// Returns a new curve rather than mutating, and may return a *different
    /// type* than the receiver: a circle under a non-uniform scale becomes an
    /// ellipse. Silently keeping it a circle would distort the drawing.
    [[nodiscard]] virtual CurvePtr transformed(const Mat4& m) const = 0;

    /// Reverse the parameterization; geometry is unchanged.
    [[nodiscard]] virtual CurvePtr reversed() const = 0;

    /// Convert to a polyline approximation within `chordTolerance`.
    /// Always includes the exact start and end points.
    [[nodiscard]] virtual std::vector<Vec3> tessellate(
        const TessellationOptions& options = {}) const = 0;

    /// Sub-curve between two normalized parameters.
    [[nodiscard]] virtual CurvePtr trimmed(double t0, double t1) const = 0;

    /// Split at the given normalized parameters, in parameter order.
    /// Parameters at or beyond the ends are ignored, so an empty list returns a
    /// single clone of the whole curve.
    [[nodiscard]] virtual std::vector<CurvePtr> split(std::span<const double> params) const;

    /// Signed area in the XY plane for curves that bound one; zero otherwise.
    /// Positive when counter-clockwise.
    [[nodiscard]] virtual double signedArea() const { return 0.0; }

    /// True for curves that enclose an area — consumed by hatch boundary
    /// processing and the AREA command.
    [[nodiscard]] virtual bool hasArea() const { return false; }

    // ------------------------------------------------------------ convenience

    /// Distance from a point to the curve.
    [[nodiscard]] double distanceTo(const Vec3& p) const { return closestPoint(p).distance; }

    /// True when `p` lies on the curve within tolerance.
    [[nodiscard]] bool containsPoint(const Vec3& p,
                                     const Tolerance& tol = defaultTolerance()) const {
        return closestPoint(p).distance <= tol.point;
    }

    /// Midpoint by parameter — the MIDPOINT object snap.
    [[nodiscard]] Vec3 midPoint() const { return pointAt(0.5); }

protected:
    /// Copy and move are protected, not deleted.
    ///
    /// Protected keeps concrete curve types ordinary copyable values — which
    /// they must be, since factories such as `Arc::fromThreePoints` return
    /// `std::optional<Arc>` — while making it impossible to slice a derived
    /// curve through a `Curve&`. Deleting them instead would forbid both.
    Curve(const Curve&) = default;
    Curve& operator=(const Curve&) = default;
    Curve(Curve&&) = default;
    Curve& operator=(Curve&&) = default;

    [[nodiscard]] static constexpr double clampParam(double t) noexcept {
        return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    }
};

/// Sort split parameters, drop those outside the open interval `(0, 1)`, and
/// collapse duplicates. Shared by every curve's `split` implementation.
[[nodiscard]] std::vector<double> sanitizeSplitParams(std::span<const double> params,
                                                      double eps = 1e-12);

}  // namespace cad
