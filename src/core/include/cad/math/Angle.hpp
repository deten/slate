#pragma once

#include <cmath>
#include <numbers>

namespace cad {

/// Angle utilities.
///
/// Angles are radians everywhere inside the kernel. Degrees exist only at the
/// I/O boundary (DXF stores degrees) and in the UI layer; conversion happens
/// exactly once, at those boundaries, so no interior code has to guess a unit.

inline constexpr double kPi = std::numbers::pi_v<double>;
inline constexpr double kTau = 2.0 * kPi;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

[[nodiscard]] constexpr double degToRad(double degrees) noexcept {
    return degrees * kDegToRad;
}

[[nodiscard]] constexpr double radToDeg(double radians) noexcept {
    return radians * kRadToDeg;
}

/// Normalize into `[0, 2pi)`.
[[nodiscard]] inline double normalizeAngle(double a) noexcept {
    double r = std::fmod(a, kTau);
    if (r < 0.0) {
        r += kTau;
    }
    // fmod of a value very slightly below zero can round to exactly kTau after
    // the addition, which would break the half-open range invariant.
    return r >= kTau ? 0.0 : r;
}

/// Normalize into `(-pi, pi]`.
[[nodiscard]] inline double normalizeAngleSigned(double a) noexcept {
    const double r = normalizeAngle(a);
    return r > kPi ? r - kTau : r;
}

/// Sweep from `from` to `to` in the given direction, always in `[0, 2pi)`.
///
/// A full circle is reported as 0, so callers needing to distinguish "no
/// sweep" from "full sweep" must track closure separately — which arcs do,
/// via their explicit start and end angles.
[[nodiscard]] inline double sweepAngle(double from, double to,
                                       bool counterClockwise = true) noexcept {
    return counterClockwise ? normalizeAngle(to - from) : normalizeAngle(from - to);
}

/// True when `angle` lies on the counter-clockwise sweep from `start` to
/// `end`, inclusive of the endpoints within `tol`.
[[nodiscard]] inline bool angleInSweep(double angle, double start, double end,
                                       double tol = 1e-10) noexcept {
    const double sweep = normalizeAngle(end - start);
    const double offset = normalizeAngle(angle - start);
    return offset <= sweep + tol || offset >= kTau - tol;
}

/// Shortest absolute difference between two angles, in `[0, pi]`.
[[nodiscard]] inline double angleDifference(double a, double b) noexcept {
    return std::abs(normalizeAngleSigned(a - b));
}

/// Snap an angle to the nearest multiple of `increment`, offset by `base`.
/// Backs polar tracking and ortho mode.
[[nodiscard]] inline double snapAngle(double angle, double increment, double base = 0.0) noexcept {
    if (increment <= 0.0) {
        return angle;
    }
    return base + std::round((angle - base) / increment) * increment;
}

}  // namespace cad
