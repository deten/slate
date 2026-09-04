#include "test_helpers.hpp"

#include "cad/math/Angle.hpp"
#include "cad/math/Box3.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/math/Tolerance.hpp"
#include "cad/math/Vec3.hpp"

using namespace cad;

TEST_SUITE("Vec3") {
    TEST_CASE("computes basic vector algebra") {
        const Vec3 a{3.0, 4.0};
        CHECK(a.length() == doctest::Approx(5.0));
        CHECK(a.dot(Vec3{1.0, 0.0}) == doctest::Approx(3.0));
        CHECK(a.cross2(Vec3{1.0, 0.0}) == doctest::Approx(-4.0));
        CHECK(a.normalized().length() == doctest::Approx(1.0));
    }

    TEST_CASE("refuses to normalize a zero vector but offers a fallible form") {
        CHECK_THROWS_AS((void)Vec3::zero().normalized(), std::domain_error);
        CHECK_FALSE(Vec3::zero().tryNormalize().has_value());
    }

    TEST_CASE("rotates about a centre point") {
        CHECK_POINT(Vec3(1.0, 0.0).rotatedAbout(Vec3::zero(), kPi / 2.0), Vec3(0.0, 1.0));
    }

    TEST_CASE("distinguishes parallel from codirectional") {
        const Vec3 a{1.0, 0.0};
        const Vec3 b{-1.0, 0.0};
        CHECK(a.isParallel(b));
        CHECK_FALSE(a.isCodirectional(b));
        CHECK(a.isPerpendicular(Vec3{0.0, 5.0}));
    }

    TEST_CASE("separates 2D and 3D distance") {
        const Vec3 a{0.0, 0.0, 0.0};
        const Vec3 b{3.0, 4.0, 12.0};
        CHECK(a.distance2To(b) == doctest::Approx(5.0));
        CHECK(a.distanceTo(b) == doctest::Approx(13.0));
    }
}

TEST_SUITE("Mat4") {
    TEST_CASE("composes transforms in the documented order") {
        // Scale, then rotate, then translate, applied to the unit X point.
        const Mat4 m = Mat4::blockTransform(Vec3{10.0, 0.0}, Vec3{2.0, 2.0, 1.0}, kPi / 2.0);
        CHECK_POINT(m.transformPoint(Vec3{1.0, 0.0}), Vec3(10.0, 2.0));
    }

    TEST_CASE("inverts and round-trips a point") {
        const Mat4 m = Mat4::translation(Vec3{5.0, -3.0})
                           .multiply(Mat4::rotationZ(0.7))
                           .multiply(Mat4::scaling(3.0));
        const auto inv = m.inverted();
        REQUIRE(inv.has_value());
        CHECK_POINT(inv->transformPoint(m.transformPoint(Vec3{2.0, 9.0})), Vec3(2.0, 9.0));
    }

    TEST_CASE("reports a singular matrix as non-invertible rather than throwing") {
        CHECK_FALSE(Mat4::scaling(0.0).inverted().has_value());
    }

    TEST_CASE("detects mirroring and conformality") {
        CHECK(Mat4::mirror2(Vec3::zero(), Vec3::unitX()).isMirrored());
        CHECK(Mat4::rotationZ(1.1).isConformal());
        CHECK_FALSE(Mat4::scaling(Vec3{2.0, 3.0, 1.0}).isConformal());
        CHECK(Mat4::scaling(2.0).isConformal());
    }

    TEST_CASE("mirrors across an arbitrary line") {
        // Mirroring across the 45-degree line through the origin swaps X and Y.
        const Mat4 m = Mat4::mirror2(Vec3::zero(), Vec3{1.0, 1.0});
        CHECK_POINT(m.transformPoint(Vec3{3.0, 0.0}), Vec3(0.0, 3.0));
    }

    TEST_CASE("rotates about an arbitrary axis") {
        // A 180-degree turn about X maps +Y to -Y.
        const Mat4 m = Mat4::rotationAboutAxis(Vec3::unitX(), kPi);
        CHECK_POINT(m.transformPoint(Vec3{0.0, 1.0, 0.0}), Vec3(0.0, -1.0, 0.0));
    }

    TEST_CASE("reports the XY scale and rotation of a composed transform") {
        const Mat4 m = Mat4::rotationZ(kPi / 6.0).multiply(Mat4::scaling(4.0));
        CHECK(m.scaleFactor2() == doctest::Approx(4.0));
        CHECK(m.rotation2() == doctest::Approx(kPi / 6.0));
    }
}

TEST_SUITE("Box3") {
    TEST_CASE("treats the empty box as a union identity") {
        const Box3 b = Box3::fromPoint(Vec3{1.0, 1.0});
        CHECK(Box3::empty().united(b).equals(b));
        CHECK(Box3::empty().isEmpty());
    }

    TEST_CASE("transforms by corners so rotation is handled correctly") {
        const Box3 box{Vec3{-1.0, -1.0}, Vec3{1.0, 1.0}};
        const Box3 rotated = box.transformed(Mat4::rotationZ(kPi / 4.0));
        // A rotated unit square has a half-diagonal extent of sqrt(2).
        CHECK(rotated.max.x == doctest::Approx(std::sqrt(2.0)));
    }

    TEST_CASE("separates containment from intersection for window vs crossing") {
        const Box3 outer{Vec3{0.0, 0.0}, Vec3{10.0, 10.0}};
        const Box3 inner{Vec3{2.0, 2.0}, Vec3{3.0, 3.0}};
        const Box3 straddling{Vec3{9.0, 9.0}, Vec3{20.0, 20.0}};
        CHECK(outer.containsBox(inner));
        CHECK_FALSE(outer.containsBox(straddling));
        CHECK(outer.intersects(straddling));
    }

    TEST_CASE("measures distance to an outside point and zero inside") {
        const Box3 box{Vec3{0.0, 0.0}, Vec3{10.0, 10.0}};
        CHECK(box.distanceToPoint(Vec3{5.0, 5.0}) == doctest::Approx(0.0));
        CHECK(box.distanceToPoint(Vec3{13.0, 14.0}) == doctest::Approx(5.0));
    }

    TEST_CASE("builds from a point span") {
        const std::array<Vec3, 3> points{{{1.0, 5.0}, {-2.0, 3.0}, {4.0, -1.0}}};
        const Box3 box = Box3::fromPoints(points);
        CHECK_POINT(box.min, Vec3(-2.0, -1.0, 0.0));
        CHECK_POINT(box.max, Vec3(4.0, 5.0, 0.0));
    }
}

TEST_SUITE("Angle") {
    TEST_CASE("normalizes into the documented ranges") {
        CHECK(normalizeAngle(-kPi / 2.0) == doctest::Approx(3.0 * kPi / 2.0));
        CHECK(normalizeAngle(kTau + 1.0) == doctest::Approx(1.0));
        CHECK(normalizeAngleSigned(3.0 * kPi / 2.0) == doctest::Approx(-kPi / 2.0));
    }

    TEST_CASE("keeps normalizeAngle strictly below tau") {
        // A value fractionally below zero must not round up to exactly tau,
        // which would break the half-open range every sweep calculation
        // assumes.
        CHECK(normalizeAngle(-1e-18) < kTau);
        CHECK(normalizeAngle(-1e-18) >= 0.0);
    }

    TEST_CASE("converts between degrees and radians at the I/O boundary") {
        CHECK(degToRad(180.0) == doctest::Approx(kPi));
        CHECK(radToDeg(kPi / 2.0) == doctest::Approx(90.0));
    }

    TEST_CASE("snaps angles for polar tracking") {
        const double increment = kPi / 4.0;
        CHECK(snapAngle(0.1, increment) == doctest::Approx(0.0));
        CHECK(snapAngle(0.7, increment) == doctest::Approx(increment));
    }
}

TEST_SUITE("Tolerance") {
    TEST_CASE("rejects a non-positive tolerance") {
        CHECK_THROWS_AS((void)makeTolerance(0.0, 1.0), std::invalid_argument);
        CHECK_THROWS_AS((void)makeTolerance(1.0, -1.0), std::invalid_argument);
    }

    TEST_CASE("compares with an absolute and a relative form") {
        CHECK(nearlyEqual(1.0, 1.0 + 1e-12));
        CHECK_FALSE(nearlyEqual(1.0, 1.0001));
        // The relative form tolerates proportional error on large magnitudes.
        CHECK(nearlyEqualRelative(1e9, 1e9 + 1.0, Tolerance{1e-6, 1e-6}));
    }
}
