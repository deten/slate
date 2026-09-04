#include "test_helpers.hpp"

#include <cmath>
#include <vector>

#include "cad/geom/NurbsCurve.hpp"

using namespace cad;

namespace {

/// A cubic Bezier from the Bernstein form, written out longhand.
///
/// The point of this is to be a *different* implementation from the one under
/// test. A clamped cubic B-spline with four control points and the knot vector
/// {0,0,0,0,1,1,1,1} is exactly a cubic Bezier, so de Boor has to agree with
/// this everywhere -- and if both were derived the same way, agreeing would
/// prove nothing.
[[nodiscard]] Vec3 bezier(const std::vector<Vec3>& p, double t) {
    const double s = 1.0 - t;
    const double b0 = s * s * s;
    const double b1 = 3.0 * s * s * t;
    const double b2 = 3.0 * s * t * t;
    const double b3 = t * t * t;
    return p[0] * b0 + p[1] * b1 + p[2] * b2 + p[3] * b3;
}

}  // namespace

TEST_SUITE("NURBS") {

TEST_CASE("a clamped cubic agrees with the Bezier it is") {
    const std::vector<Vec3> control{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 40.0, 0.0},
                                    Vec3{60.0, 40.0, 0.0}, Vec3{80.0, 0.0, 0.0}};
    NurbsCurve curve{control, {0, 0, 0, 0, 1, 1, 1, 1}, 3};
    REQUIRE(curve.isValid());

    for (int i = 0; i <= 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        const Vec3 expected = bezier(control, t);
        const Vec3 actual = curve.evaluate(t);
        INFO("t = ", t);
        CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-12));
        CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-12));
    }
}

TEST_CASE("a clamped curve passes through its first and last control points") {
    // The defining property of a clamped knot vector, and the one a drafter
    // sees: the curve starts where the spline starts.
    NurbsCurve curve{{Vec3{5.0, 5.0, 0.0}, Vec3{10.0, 40.0, 0.0}, Vec3{60.0, 40.0, 0.0},
                      Vec3{80.0, 7.0, 0.0}},
                     {0, 0, 0, 0, 1, 1, 1, 1},
                     3};
    CHECK(curve.startPoint().equals(Vec3{5.0, 5.0, 0.0}));
    CHECK(curve.endPoint().equals(Vec3{80.0, 7.0, 0.0}));
}

TEST_CASE("a multi-span curve is continuous across its interior knots") {
    // Where de Boor picks the wrong span, the curve jumps at a knot. Nothing
    // else about it looks wrong, which is what makes it worth checking
    // directly: sampling either side of every interior knot and insisting the
    // two agree.
    NurbsCurve curve{{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 30.0, 0.0}, Vec3{30.0, -20.0, 0.0},
                      Vec3{50.0, 30.0, 0.0}, Vec3{70.0, -10.0, 0.0}, Vec3{90.0, 0.0, 0.0}},
                     {0, 0, 0, 0, 1, 2, 3, 3, 3, 3},
                     3};
    REQUIRE(curve.isValid());

    for (const double knot : {1.0, 2.0}) {
        constexpr double kNudge = 1e-7;
        const Vec3 before = curve.evaluate(knot - kNudge);
        const Vec3 after = curve.evaluate(knot + kNudge);
        INFO("knot ", knot);
        CHECK(before.distanceTo(after) < 1e-4);
    }
}

TEST_CASE("the curve stays inside the hull of its control points") {
    // A B-spline is a convex combination of its control points, so it cannot
    // leave their bounding box. A span-indexing error usually throws a point
    // far outside, which this catches without knowing where the curve should
    // actually be.
    const std::vector<Vec3> control{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 30.0, 0.0},
                                    Vec3{30.0, -20.0, 0.0}, Vec3{50.0, 30.0, 0.0},
                                    Vec3{70.0, 0.0, 0.0}};
    NurbsCurve curve{control, {0, 0, 0, 0, 1, 2, 2, 2, 2}, 3};
    REQUIRE(curve.isValid());

    Box3 hull;
    for (const Vec3& point : control) {
        hull = hull.united(Box3::fromPoint(point));
    }
    for (const Vec3& point : curve.tessellate()) {
        INFO("point ", point.x, ", ", point.y);
        CHECK(point.x >= hull.min.x - 1e-9);
        CHECK(point.x <= hull.max.x + 1e-9);
        CHECK(point.y >= hull.min.y - 1e-9);
        CHECK(point.y <= hull.max.y + 1e-9);
    }
}

TEST_CASE("weights pull the curve toward their control point") {
    // The rational half of NURBS. Raising the middle weight of a three-point
    // quadratic has to move the curve toward that point and nowhere else.
    const std::vector<Vec3> control{Vec3{0.0, 0.0, 0.0}, Vec3{50.0, 50.0, 0.0},
                                    Vec3{100.0, 0.0, 0.0}};
    NurbsCurve plain{control, {0, 0, 0, 1, 1, 1}, 2};
    NurbsCurve heavy{control, {0, 0, 0, 1, 1, 1}, 2};
    heavy.weights = {1.0, 8.0, 1.0};
    REQUIRE(plain.isValid());
    REQUIRE(heavy.isValid());

    // Both are symmetric, so the midpoint stays on the axis and only rises.
    CHECK(plain.evaluate(0.5).x == doctest::Approx(50.0));
    CHECK(heavy.evaluate(0.5).x == doctest::Approx(50.0));
    CHECK(heavy.evaluate(0.5).y > plain.evaluate(0.5).y);
    // ...and not past the control point it is being pulled toward.
    CHECK(heavy.evaluate(0.5).y < 50.0);

    // The ends are pinned whatever the weights do.
    CHECK(heavy.startPoint().equals(Vec3{0.0, 0.0, 0.0}));
    CHECK(heavy.endPoint().equals(Vec3{100.0, 0.0, 0.0}));
}

TEST_CASE("nonsense in the file is rejected rather than evaluated") {
    // A knot vector of the wrong length is not a curve, and guessing what was
    // meant would put geometry somewhere nobody drew it.
    NurbsCurve wrongKnots{{Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 1.0, 0.0}, Vec3{2.0, 0.0, 0.0},
                           Vec3{3.0, 1.0, 0.0}},
                          {0, 0, 1, 1},
                          3};
    CHECK_FALSE(wrongKnots.isValid());
    CHECK(wrongKnots.tessellate().empty());
    CHECK(wrongKnots.length() == doctest::Approx(0.0));

    NurbsCurve tooFewPoints{{Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 1.0, 0.0}}, {0, 0, 0, 1, 1, 1}, 3};
    CHECK_FALSE(tooFewPoints.isValid());

    NurbsCurve partialWeights{{Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 1.0, 0.0}, Vec3{2.0, 0.0, 0.0}},
                              {0, 0, 0, 1, 1, 1},
                              2};
    partialWeights.weights = {1.0, 2.0};
    CHECK_FALSE(partialWeights.isValid());
}

TEST_CASE("transforming the control points transforms the curve") {
    // True because a B-spline is an affine combination of its control points,
    // which is why the format stores control points and not samples.
    NurbsCurve curve{{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 40.0, 0.0}, Vec3{60.0, 40.0, 0.0},
                      Vec3{80.0, 0.0, 0.0}},
                     {0, 0, 0, 0, 1, 1, 1, 1},
                     3};
    const Mat4 move = Mat4::translation(Vec3{100.0, 25.0, 0.0});
    const CurvePtr moved = curve.transformed(move);

    for (int i = 0; i <= 8; ++i) {
        const double t = static_cast<double>(i) / 8.0;
        INFO("t = ", t);
        CHECK(moved->pointAt(t).equals(move.transformPoint(curve.pointAt(t))));
    }
}

TEST_CASE("reversing keeps the geometry and flips the direction") {
    NurbsCurve curve{{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 40.0, 0.0}, Vec3{60.0, 40.0, 0.0},
                      Vec3{80.0, 0.0, 0.0}},
                     {0, 0, 0, 0, 1, 1, 1, 1},
                     3};
    const CurvePtr back = curve.reversed();
    CHECK(back->startPoint().equals(curve.endPoint()));
    CHECK(back->endPoint().equals(curve.startPoint()));
    for (int i = 0; i <= 8; ++i) {
        const double t = static_cast<double>(i) / 8.0;
        INFO("t = ", t);
        CHECK(back->pointAt(t).equals(curve.pointAt(1.0 - t)));
    }
}

TEST_CASE("the closest point lands on the curve, not on a chord across it") {
    // What object snapping rests on. A polyline approximation alone answers
    // this only to within its sampling, which at a snap aperture shows.
    NurbsCurve curve{{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 40.0, 0.0}, Vec3{60.0, 40.0, 0.0},
                      Vec3{80.0, 0.0, 0.0}},
                     {0, 0, 0, 0, 1, 1, 1, 1},
                     3};

    const Vec3 onCurve = curve.pointAt(0.37);
    const ClosestPointResult hit = curve.closestPoint(onCurve);
    CHECK(hit.distance < 1e-6);
    CHECK(hit.param == doctest::Approx(0.37).epsilon(1e-4));
}

}  // TEST_SUITE
