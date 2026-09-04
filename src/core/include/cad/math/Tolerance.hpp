#pragma once

#include <cmath>
#include <stdexcept>

namespace cad {

/// Tolerance model.
///
/// CAD comparisons are never exact. Every geometric predicate in this codebase
/// takes an explicit Tolerance (defaulting to `defaultTolerance()`) rather than
/// hard-coding an epsilon, so a document can later carry its own precision
/// settings — drawings authored in microns versus kilometres — without a
/// rewrite of the geometry layer.
///
/// Two separate tolerances are tracked, mirroring the distinction every serious
/// kernel makes:
///  - `point`: linear distance below which two positions are the same point.
///  - `angle`: angular difference (radians) below which two directions are
///    parallel, or two angles equal.
struct Tolerance {
    /// Linear tolerance, in drawing units.
    double point = 1e-10;
    /// Angular tolerance, in radians.
    double angle = 1e-10;

    constexpr Tolerance() noexcept = default;

    constexpr Tolerance(double linearTolerance, double angularTolerance) noexcept
        : point(linearTolerance), angle(angularTolerance) {}

    explicit constexpr Tolerance(double both) noexcept : point(both), angle(both) {}
};

/// Default working tolerance. Matches other CAD software's global tolerance
/// (`AcGeContext::gTol`) so imported geometry behaves familiarly.
[[nodiscard]] constexpr Tolerance defaultTolerance() noexcept {
    return Tolerance{1e-10, 1e-10};
}

/// A deliberately looser tolerance for user-facing pick operations, where the
/// input originates from screen coordinates rather than exact arithmetic.
[[nodiscard]] constexpr Tolerance pickTolerance() noexcept {
    return Tolerance{1e-6, 1e-6};
}

/// Construct a validated tolerance.
/// @throws std::invalid_argument when either component is not positive.
[[nodiscard]] inline Tolerance makeTolerance(double point, double angle) {
    if (!(point > 0.0) || !(angle > 0.0) || !std::isfinite(point) || !std::isfinite(angle)) {
        throw std::invalid_argument("Tolerance components must be positive and finite");
    }
    return Tolerance{point, angle};
}

/// Scale a tolerance, e.g. to relax comparisons over a large extent.
[[nodiscard]] inline Tolerance scaleTolerance(const Tolerance& tol, double factor) {
    return makeTolerance(tol.point * factor, tol.angle * factor);
}

/// True when `a` and `b` are equal within the linear tolerance.
[[nodiscard]] constexpr bool nearlyEqual(double a, double b,
                                         const Tolerance& tol = defaultTolerance()) noexcept {
    const double diff = a > b ? a - b : b - a;
    return diff <= tol.point;
}

/// True when `v` is zero within the linear tolerance.
[[nodiscard]] constexpr bool nearlyZero(double v,
                                        const Tolerance& tol = defaultTolerance()) noexcept {
    return (v < 0.0 ? -v : v) <= tol.point;
}

/// True when two angles (radians) are equal within the angular tolerance.
[[nodiscard]] constexpr bool angleNearlyEqual(double a, double b,
                                              const Tolerance& tol = defaultTolerance()) noexcept {
    const double diff = a > b ? a - b : b - a;
    return diff <= tol.angle;
}

/// Clamp `v` into `[lo, hi]`.
[[nodiscard]] constexpr double clamp(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Compare with a relative component, for magnitudes far from 1.0.
/// Used where values may be dot products of long vectors.
[[nodiscard]] constexpr bool nearlyEqualRelative(
    double a, double b, const Tolerance& tol = defaultTolerance()) noexcept {
    const double diff = a > b ? a - b : b - a;
    if (diff <= tol.point) {
        return true;
    }
    const double magA = a < 0.0 ? -a : a;
    const double magB = b < 0.0 ? -b : b;
    return diff <= tol.point * (magA > magB ? magA : magB);
}

}  // namespace cad
