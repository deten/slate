#pragma once

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "cad/math/Box3.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// Print a Vec3 legibly when an assertion fails. Without this doctest reports
/// the raw bytes, which makes a geometry failure nearly undiagnosable.
inline std::ostream& operator<<(std::ostream& os, const Vec3& v) {
    os << '(' << v.x << ", " << v.y << ", " << v.z << ')';
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const Box3& b) {
    if (b.isEmpty()) {
        os << "Box3(empty)";
    } else {
        os << "Box3(" << b.min << " .. " << b.max << ')';
    }
    return os;
}

}  // namespace cad

/// Assert two points are equal to within a drafting tolerance.
///
/// A macro rather than a function so a failure reports the call site in the
/// test, not a line inside a helper.
#define CHECK_POINT(actual, expected)                        \
    do {                                                     \
        const ::cad::Vec3 a_ = (actual);                     \
        const ::cad::Vec3 e_ = (expected);                   \
        CHECK(a_.x == doctest::Approx(e_.x).epsilon(1e-9));  \
        CHECK(a_.y == doctest::Approx(e_.y).epsilon(1e-9));  \
        CHECK(a_.z == doctest::Approx(e_.z).epsilon(1e-9));  \
    } while (false)

/// Point comparison with an explicit tolerance, for results that legitimately
/// carry more accumulated error (numeric solvers, long transform chains).
#define CHECK_POINT_EPS(actual, expected, eps)              \
    do {                                                    \
        const ::cad::Vec3 a_ = (actual);                    \
        const ::cad::Vec3 e_ = (expected);                  \
        CHECK(a_.x == doctest::Approx(e_.x).epsilon(eps));  \
        CHECK(a_.y == doctest::Approx(e_.y).epsilon(eps));  \
        CHECK(a_.z == doctest::Approx(e_.z).epsilon(eps));  \
    } while (false)
