#include "test_helpers.hpp"

#include <cmath>

#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/Intersect.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Ocs.hpp"
#include "cad/geom/Polyline.hpp"

using namespace cad;

// Ported from the TypeScript prototype's suite, which is the behavioural
// specification for this port (ADR 0002). Where an expectation encodes a real
// CAD convention rather than an implementation detail, the comment says so —
// those are the assertions that must not be "corrected" to match a regression.

TEST_SUITE("LineSegment") {
    TEST_CASE("parameterizes linearly over [0, 1]") {
        const LineSegment seg{Vec3{0, 0}, Vec3{10, 0}};
        CHECK_POINT(seg.pointAt(0.25), Vec3(2.5, 0));
        CHECK(seg.length() == doctest::Approx(10.0));
    }

    TEST_CASE("clamps the closest point to the segment extent") {
        const LineSegment seg{Vec3{0, 0}, Vec3{10, 0}};
        const ClosestPointResult r = seg.closestPoint(Vec3{-5, 3});
        CHECK_POINT(r.point, Vec3(0, 0));
        CHECK(r.param == doctest::Approx(0.0));
        CHECK(r.distance == doctest::Approx(std::hypot(5.0, 3.0)));
    }

    TEST_CASE("reports unclamped parameters on the extension") {
        const LineSegment seg{Vec3{0, 0}, Vec3{10, 0}};
        const auto r = seg.closestPointExtended(Vec3{-5, 3});
        REQUIRE(r.has_value());
        CHECK(r->param == doctest::Approx(-0.5));
        CHECK_POINT(r->point, Vec3(-5, 0));
    }

    TEST_CASE("extends to a point on its own line and rejects points off it") {
        const LineSegment seg{Vec3{0, 0}, Vec3{10, 0}};
        const auto extended = seg.extendedTo(Vec3{15, 0});
        REQUIRE(extended.has_value());
        CHECK_POINT(extended->end, Vec3(15, 0));
        CHECK_POINT(extended->start, Vec3(0, 0));
        CHECK_FALSE(seg.extendedTo(Vec3{15, 4}).has_value());
    }

    TEST_CASE("splits at interior parameters and ignores out-of-range ones") {
        const LineSegment seg{Vec3{0, 0}, Vec3{10, 0}};
        const std::array<double, 3> params{0.5, 1.5, -0.2};
        const std::vector<CurvePtr> parts = seg.split(params);
        REQUIRE(parts.size() == 2);
        CHECK_POINT(parts[0]->endPoint(), Vec3(5, 0));
    }

    TEST_CASE("handles zero-length segments without dividing by zero") {
        const LineSegment degenerate{Vec3{1, 1}, Vec3{1, 1}};
        CHECK(degenerate.isDegenerate());
        CHECK(degenerate.closestPoint(Vec3{4, 5}).distance == doctest::Approx(5.0));
        CHECK_FALSE(degenerate.closestPointExtended(Vec3{4, 5}).has_value());
    }
}

TEST_SUITE("Arc") {
    TEST_CASE("sweeps counter-clockwise and reports arc length") {
        const Arc arc{Vec3::zero(), 2.0, 0.0, kPi / 2.0};
        CHECK(arc.sweep() == doctest::Approx(kPi / 2.0));
        CHECK(arc.length() == doctest::Approx(kPi));
        CHECK_POINT(arc.startPoint(), Vec3(2, 0));
        CHECK_POINT(arc.endPoint(), Vec3(0, 2));
        CHECK_POINT(arc.midPoint(), Vec3(std::sqrt(2.0), std::sqrt(2.0)));
    }

    TEST_CASE("represents a full circle distinctly from a zero sweep") {
        const Arc circle = Arc::circle(Vec3::zero(), 3.0);
        CHECK(circle.isClosed());
        CHECK(circle.sweep() == doctest::Approx(kTau));
        CHECK(circle.length() == doctest::Approx(kTau * 3.0));
        CHECK(circle.signedArea() == doctest::Approx(kPi * 9.0));
    }

    TEST_CASE("computes exact bounds for a partial arc, not the full circle") {
        // First-quadrant arc: bounds are the unit square.
        const Arc arc{Vec3::zero(), 1.0, 0.0, kPi / 2.0};
        const Box3 b = arc.bounds();
        CHECK(b.min.x == doctest::Approx(0.0));
        CHECK(b.min.y == doctest::Approx(0.0));
        CHECK(b.max.x == doctest::Approx(1.0));
        CHECK(b.max.y == doctest::Approx(1.0));
    }

    TEST_CASE("builds from three points and passes through the middle one") {
        const auto arc = Arc::fromThreePoints(Vec3{1, 0}, Vec3{0, 1}, Vec3{-1, 0});
        REQUIRE(arc.has_value());
        CHECK_POINT(arc->center, Vec3::zero());
        CHECK(arc->radius == doctest::Approx(1.0));
        CHECK(arc->containsAngle(kPi / 2.0));
    }

    TEST_CASE("returns nothing for collinear three-point input") {
        CHECK_FALSE(Arc::fromThreePoints(Vec3{0, 0}, Vec3{1, 0}, Vec3{2, 0}).has_value());
    }

    TEST_CASE("follows the DXF bulge sign convention") {
        // Positive bulge sweeps counter-clockwise from start to end. For a
        // chord running left to right that puts the centre above and bows the
        // arc below. This is a DWG convention, not an implementation choice.
        const auto positive = Arc::fromBulge(Vec3{0, 0}, Vec3{10, 0}, 0.5);
        REQUIRE(positive.has_value());
        CHECK(positive->center.y > 0.0);
        CHECK(positive->midPoint().y < 0.0);
        CHECK_POINT(positive->startPoint(), Vec3(0, 0));
        CHECK_POINT(positive->endPoint(), Vec3(10, 0));
        // Included angle is 4*atan(bulge).
        CHECK(positive->sweep() == doctest::Approx(4.0 * std::atan(0.5)));

        // Negative bulge is the exact mirror.
        const auto negative = Arc::fromBulge(Vec3{0, 0}, Vec3{10, 0}, -0.5);
        REQUIRE(negative.has_value());
        CHECK(negative->center.y == doctest::Approx(-positive->center.y));
        CHECK(negative->midPoint().y > 0.0);
        CHECK(negative->radius == doctest::Approx(positive->radius));

        // A bulge of 1 is exactly a semicircle.
        const auto semi = Arc::fromBulge(Vec3{0, 0}, Vec3{10, 0}, 1.0);
        REQUIRE(semi.has_value());
        CHECK(semi->sweep() == doctest::Approx(kPi));
        CHECK(semi->radius == doctest::Approx(5.0));
        CHECK_POINT(semi->center, Vec3(5, 0));
    }

    TEST_CASE("round-trips through the DXF bulge representation") {
        const Arc original{Vec3{0, 0}, 5.0, degToRad(30.0), degToRad(150.0)};
        const auto rebuilt =
            Arc::fromBulge(original.startPoint(), original.endPoint(), original.bulge());
        REQUIRE(rebuilt.has_value());
        CHECK_POINT_EPS(rebuilt->center, original.center, 1e-6);
        CHECK(rebuilt->radius == doctest::Approx(original.radius));
        CHECK(rebuilt->sweep() == doctest::Approx(original.sweep()));
    }

    TEST_CASE("tessellates to within the requested chord tolerance") {
        const Arc arc = Arc::circle(Vec3::zero(), 100.0);
        TessellationOptions options;
        options.chordTolerance = 0.01;
        const std::vector<Vec3> points = arc.tessellate(options);

        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            const Vec3 mid = points[i].midpoint(points[i + 1]);
            // Sagitta = r - |chord midpoint|, and must not exceed the tolerance.
            CHECK(100.0 - mid.length() <= 0.0101);
        }
    }

    TEST_CASE("keeps a circle circular under a uniform scale") {
        const CurvePtr scaled = Arc::circle(Vec3::zero(), 2.0).transformed(Mat4::scaling(3.0));
        const auto* arc = dynamic_cast<const Arc*>(scaled.get());
        REQUIRE(arc != nullptr);
        CHECK(arc->radius == doctest::Approx(6.0));
    }

    TEST_CASE("widens to an ellipse under a non-uniform scale instead of distorting") {
        const CurvePtr scaled =
            Arc::circle(Vec3::zero(), 2.0).transformed(Mat4::scaling(Vec3{3, 1, 1}));
        const auto* ellipse = dynamic_cast<const EllipseArc*>(scaled.get());
        REQUIRE(ellipse != nullptr);
        CHECK(ellipse->semiMajor() == doctest::Approx(6.0));
        CHECK(ellipse->semiMinor() == doctest::Approx(2.0));
    }

    TEST_CASE("preserves visible geometry when mirrored") {
        const Arc arc{Vec3::zero(), 1.0, 0.0, kPi / 2.0};
        const CurvePtr mirroredCurve =
            arc.transformed(Mat4::mirror2(Vec3::zero(), Vec3::unitX()));
        const auto* mirrored = dynamic_cast<const Arc*>(mirroredCurve.get());
        REQUIRE(mirrored != nullptr);
        // Mirroring across X maps the endpoints; the class keeps CCW by
        // swapping them, so the sweep magnitude is unchanged.
        CHECK_POINT(mirrored->startPoint(), Vec3(0, -1));
        CHECK_POINT(mirrored->endPoint(), Vec3(1, 0));
        CHECK(mirrored->sweep() == doctest::Approx(kPi / 2.0));
    }

    TEST_CASE("reports only the quadrant points actually on the arc") {
        CHECK(Arc(Vec3::zero(), 1.0, 0.0, kPi / 2.0).quadrantPoints().size() == 2);
        CHECK(Arc::circle(Vec3::zero(), 1.0).quadrantPoints().size() == 4);
    }

    TEST_CASE("rejects a negative radius") {
        CHECK_THROWS_AS((void)Arc(Vec3::zero(), -1.0, 0.0, kPi), std::invalid_argument);
    }
}

TEST_SUITE("EllipseArc") {
    TEST_CASE("reports axes, area and length for a full ellipse") {
        const EllipseArc e{Vec3::zero(), Vec3{4, 0}, 0.5};
        CHECK(e.semiMajor() == doctest::Approx(4.0));
        CHECK(e.semiMinor() == doctest::Approx(2.0));
        CHECK(e.isClosed());
        CHECK(e.signedArea() == doctest::Approx(kPi * 8.0));

        // Ramanujan's perimeter approximation, as an independent check.
        const double a = 4.0;
        const double b = 2.0;
        const double h = ((a - b) * (a - b)) / ((a + b) * (a + b));
        const double approx = kPi * (a + b) * (1.0 + (3.0 * h) / (10.0 + std::sqrt(4.0 - 3.0 * h)));
        CHECK(e.length() == doctest::Approx(approx).epsilon(1e-3));
    }

    TEST_CASE("matches a circle when the ratio is 1") {
        const EllipseArc e{Vec3::zero(), Vec3{5, 0}, 1.0};
        CHECK(e.length() == doctest::Approx(kTau * 5.0));
        CHECK(e.isCircular());
        const auto arc = e.toArcIfCircular();
        REQUIRE(arc.has_value());
        CHECK(arc->radius == doctest::Approx(5.0));
    }

    TEST_CASE("finds the closest point on an eccentric ellipse") {
        const EllipseArc e{Vec3::zero(), Vec3{10, 0}, 0.1};
        // Directly above the centre, the nearest point is the minor-axis top.
        const ClosestPointResult r = e.closestPoint(Vec3{0, 5});
        CHECK_POINT_EPS(r.point, Vec3(0, 1), 1e-6);
        CHECK(r.distance == doctest::Approx(4.0));
    }

    TEST_CASE("recovers true axes after a shear via Rytz construction") {
        const EllipseArc e{Vec3::zero(), Vec3{3, 0}, 1.0};
        // Shearing a circle must still yield a valid ellipse whose principal
        // axes are perpendicular; an affine map preserves conjugacy, not
        // perpendicularity, so this is exactly what Rytz's construction fixes.
        const Mat4 shear{{1, 0, 0, 0, 0.5, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        const CurvePtr shearedCurve = e.transformed(shear);
        const auto* sheared = dynamic_cast<const EllipseArc*>(shearedCurve.get());
        REQUIRE(sheared != nullptr);

        CHECK(sheared->majorAxis.isPerpendicular(sheared->minorAxis()));
        CHECK(sheared->semiMajor() >= sheared->semiMinor());
        // A shear preserves area: pi*r^2 for the original circle.
        CHECK(std::abs(sheared->signedArea()) == doctest::Approx(kPi * 9.0));
    }

    TEST_CASE("bounds a rotated ellipse tightly") {
        const EllipseArc e{Vec3::zero(), Vec3{4, 0}, 0.5};
        const CurvePtr rotated = e.transformed(Mat4::rotationZ(kPi / 2.0));
        const Box3 b = rotated->bounds();
        CHECK(b.max.x == doctest::Approx(2.0));
        CHECK(b.max.y == doctest::Approx(4.0));
    }

    TEST_CASE("rejects a ratio that is not positive, and takes one above one") {
        CHECK_THROWS_AS((void)EllipseArc(Vec3::zero(), Vec3{1, 0}, 0.0), std::invalid_argument);
        CHECK_THROWS_AS((void)EllipseArc(Vec3::zero(), Vec3{1, 0}, -1.0), std::invalid_argument);
        CHECK_NOTHROW((void)EllipseArc(Vec3::zero(), Vec3{1, 0}, 2.0));
    }
}

TEST_SUITE("Polyline") {
    TEST_CASE("closes and measures a rectangle") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        CHECK(square.isClosed());
        CHECK(square.segmentCount() == 4);
        CHECK(square.length() == doctest::Approx(40.0));
        CHECK(square.area() == doctest::Approx(100.0));
        CHECK(square.isCounterClockwise());
    }

    TEST_CASE("indexes parameters by segment so vertices land exactly") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        CHECK_POINT(square.pointAt(0.25), Vec3(10, 0));
        CHECK_POINT(square.pointAt(0.5), Vec3(10, 10));
    }

    TEST_CASE("maps arc-length queries independently of the parameterization") {
        const Polyline poly = Polyline::fromPoints({Vec3{0, 0}, Vec3{1, 0}, Vec3{11, 0}});
        // Half the total length (5.5) falls inside the long second segment.
        CHECK_POINT(poly.pointAt(poly.paramAtLength(5.5)), Vec3(5.5, 0));
        CHECK(poly.lengthAtParam(poly.paramAtLength(5.5)) == doctest::Approx(5.5));
    }

    TEST_CASE("builds arc segments from bulges") {
        std::vector<PolylineVertex> vertices{PolylineVertex{Vec3{0, 0}, 1.0},
                                             PolylineVertex{Vec3{10, 0}}};
        const Polyline poly{std::move(vertices), false};
        CHECK(poly.hasArcs());
        CHECK(dynamic_cast<const Arc*>(poly.segmentAt(0)) != nullptr);
        // A bulge of 1 is a half circle: length is pi * r with r = 5.
        CHECK(poly.length() == doctest::Approx(kPi * 5.0));
    }

    TEST_CASE("includes circular segment area for bulged spans") {
        // A closed shape: straight base plus a semicircular cap.
        std::vector<PolylineVertex> vertices{PolylineVertex{Vec3{0, 0}, 1.0},
                                             PolylineVertex{Vec3{10, 0}}};
        const Polyline poly{std::move(vertices), true};
        CHECK(poly.area() == doctest::Approx((kPi * 25.0) / 2.0));
    }

    TEST_CASE("closes a full circle built from two opposing arc vertices") {
        // Two semicircles: the representation other CAD software emits for a circular
        // PLINE, and the reason a 2-vertex polyline may legitimately be closed.
        std::vector<PolylineVertex> vertices{PolylineVertex{Vec3{0, 0}, 1.0},
                                             PolylineVertex{Vec3{10, 0}, 1.0}};
        const Polyline circle{std::move(vertices), true};
        CHECK(circle.segmentCount() == 2);
        CHECK(circle.length() == doctest::Approx(kTau * 5.0));
        CHECK(circle.area() == doctest::Approx(kPi * 25.0));
    }

    TEST_CASE("reverses while preserving geometry") {
        std::vector<PolylineVertex> vertices{PolylineVertex{Vec3{0, 0}, 0.5},
                                             PolylineVertex{Vec3{10, 0}},
                                             PolylineVertex{Vec3{10, 10}}};
        const Polyline poly{std::move(vertices), false};
        const CurvePtr reversed = poly.reversed();
        CHECK_POINT(reversed->startPoint(), poly.endPoint());
        CHECK_POINT(reversed->endPoint(), poly.startPoint());
        CHECK(reversed->length() == doctest::Approx(poly.length()));
    }

    TEST_CASE("performs point-in-polygon tests on closed shapes") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        CHECK(square.containsPointInside(Vec3{5, 5}));
        CHECK_FALSE(square.containsPointInside(Vec3{15, 5}));
    }

    TEST_CASE("flips bulge sign under a mirror so arcs bow the right way") {
        std::vector<PolylineVertex> vertices{PolylineVertex{Vec3{0, 0}, 1.0},
                                             PolylineVertex{Vec3{10, 0}}};
        const Polyline poly{std::move(vertices), false};
        const CurvePtr mirroredCurve =
            poly.transformed(Mat4::mirror2(Vec3::zero(), Vec3::unitX()));
        const auto* mirrored = dynamic_cast<const Polyline*>(mirroredCurve.get());
        REQUIRE(mirrored != nullptr);
        CHECK(mirrored->vertices[0].bulge == doctest::Approx(-1.0));
        CHECK(mirrored->length() == doctest::Approx(poly.length()));
    }

    TEST_CASE("trims across multiple segments") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        const CurvePtr trimmed = square.trimmed(0.125, 0.625);
        CHECK_FALSE(trimmed->isClosed());
        CHECK_POINT(trimmed->startPoint(), Vec3(5, 0));
        CHECK_POINT(trimmed->endPoint(), Vec3(5, 10));
        CHECK(trimmed->length() == doctest::Approx(20.0));
    }

    TEST_CASE("finds the closest point across all segments") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        const ClosestPointResult r = square.closestPoint(Vec3{5, 12});
        CHECK_POINT(r.point, Vec3(5, 10));
        CHECK(r.distance == doctest::Approx(2.0));
    }

    TEST_CASE("tolerates degenerate vertex counts") {
        CHECK(Polyline{}.isEmpty());

        // A lone vertex has no closing segment, so the closed flag is dropped.
        const Polyline single{{PolylineVertex{Vec3{1, 1}}}, true};
        CHECK(single.length() == doctest::Approx(0.0));
        CHECK_FALSE(single.isClosed());

        // Two vertices may close: with no bulge it degenerates to zero area,
        // but the flag is preserved because DWG round-tripping depends on it.
        const Polyline flat{{PolylineVertex{Vec3{0, 0}}, PolylineVertex{Vec3{1, 0}}}, true};
        CHECK(flat.isClosed());
        CHECK(flat.area() == doctest::Approx(0.0));
    }
}

TEST_SUITE("OCS") {
    TEST_CASE("is the identity for a world Z extrusion") {
        const CoordinateFrame frame = arbitraryAxis(Vec3::unitZ());
        CHECK_POINT(frame.xAxis, Vec3::unitX());
        CHECK_POINT(frame.yAxis, Vec3::unitY());
    }

    TEST_CASE("produces an orthonormal right-handed frame for a tilted normal") {
        const Vec3 n = Vec3{0.3, -0.7, 0.5}.normalized();
        const CoordinateFrame frame = arbitraryAxis(n);
        CHECK(frame.xAxis.length() == doctest::Approx(1.0));
        CHECK(frame.yAxis.length() == doctest::Approx(1.0));
        CHECK(frame.xAxis.dot(frame.yAxis) == doctest::Approx(0.0));
        CHECK_POINT(frame.xAxis.cross(frame.yAxis), frame.zAxis);
    }

    TEST_CASE("switches reference axis below the 1/64 threshold") {
        // A near-vertical normal must use world Y as the reference, or the
        // cross product would be near-degenerate. The 1/64 value is from the
        // other CAD software specification.
        const CoordinateFrame frame = arbitraryAxis(Vec3{0.001, 0.001, 1.0}.normalized());
        CHECK(std::abs(frame.xAxis.dot(Vec3::unitY())) < 0.01);
    }

    TEST_CASE("round-trips points through OCS and back") {
        const Vec3 n = Vec3{1, 2, 3}.normalized();
        const Vec3 world{5, -2, 7};
        CHECK_POINT(pointOcsToWorld(pointWorldToOcs(world, n), n), world);
    }

    TEST_CASE("places a mirrored-UCS circle at the correct world location") {
        // A -Z extrusion mirrors the X axis. This is the classic DWG gotcha:
        // treating stored coordinates as world coordinates puts the geometry
        // in the wrong place with the wrong handedness.
        const Vec3 n{0, 0, -1};
        CHECK_POINT(pointOcsToWorld(Vec3{10, 0, 0}, n), Vec3(-10, 0, 0));
    }
}

TEST_SUITE("Intersections") {
    TEST_CASE("finds a crossing between two segments") {
        const LineSegment a{Vec3{0, 0}, Vec3{10, 0}};
        const LineSegment b{Vec3{5, -5}, Vec3{5, 5}};
        const std::vector<Intersection> hits = intersectLineLine(a, b);
        REQUIRE(hits.size() == 1);
        CHECK_POINT(hits[0].point, Vec3(5, 0));
        CHECK_FALSE(hits[0].onExtensionA);
    }

    TEST_CASE("flags hits that lie beyond the segment extents") {
        const LineSegment a{Vec3{0, 0}, Vec3{1, 0}};
        const LineSegment b{Vec3{5, -5}, Vec3{5, 5}};
        CHECK(intersectCurves(a, b).empty());

        const std::vector<Intersection> extended = intersectCurvesExtended(a, b);
        REQUIRE(extended.size() == 1);
        CHECK(extended[0].onExtensionA);
        CHECK_FALSE(extended[0].onExtensionB);
        CHECK_POINT(extended[0].point, Vec3(5, 0));
    }

    TEST_CASE("reports no discrete intersection for parallel or collinear lines") {
        CHECK(intersectLineLine(LineSegment{Vec3{0, 0}, Vec3{10, 0}},
                                LineSegment{Vec3{0, 1}, Vec3{10, 1}})
                  .empty());
        CHECK(intersectLineLine(LineSegment{Vec3{0, 0}, Vec3{10, 0}},
                                LineSegment{Vec3{5, 0}, Vec3{15, 0}})
                  .empty());
    }

    TEST_CASE("finds both line/circle crossings") {
        const std::vector<Intersection> hits =
            intersectLineArc(LineSegment{Vec3{-10, 0}, Vec3{10, 0}}, Arc::circle(Vec3::zero(), 5));
        REQUIRE(hits.size() == 2);
        std::vector<double> xs{hits[0].point.x, hits[1].point.x};
        std::sort(xs.begin(), xs.end());
        CHECK(xs[0] == doctest::Approx(-5.0));
        CHECK(xs[1] == doctest::Approx(5.0));
    }

    TEST_CASE("collapses a tangent line/circle hit to a single point") {
        const std::vector<Intersection> hits = intersectCurves(
            LineSegment{Vec3{-10, 5}, Vec3{10, 5}}, Arc::circle(Vec3::zero(), 5));
        REQUIRE(hits.size() == 1);
        CHECK_POINT_EPS(hits[0].point, Vec3(0, 5), 1e-6);
    }

    TEST_CASE("respects the arc sweep when reporting circle hits") {
        // A first-quadrant arc is not crossed by a line through y = -1.
        const Arc arc{Vec3::zero(), 5.0, 0.0, kPi / 2.0};
        CHECK(intersectCurves(LineSegment{Vec3{-10, -1}, Vec3{10, -1}}, arc).empty());
    }

    TEST_CASE("finds the two intersections of overlapping circles") {
        const std::vector<Intersection> hits =
            intersectArcArc(Arc::circle(Vec3::zero(), 5), Arc::circle(Vec3{8, 0}, 5));
        REQUIRE(hits.size() == 2);
        for (const Intersection& hit : hits) {
            CHECK(hit.point.x == doctest::Approx(4.0));
            CHECK(std::abs(hit.point.y) == doctest::Approx(3.0));
        }
    }

    TEST_CASE("reports no hits for concentric, disjoint or nested circles") {
        CHECK(intersectArcArc(Arc::circle(Vec3::zero(), 5), Arc::circle(Vec3::zero(), 7)).empty());
        CHECK(intersectArcArc(Arc::circle(Vec3::zero(), 1), Arc::circle(Vec3{50, 0}, 1)).empty());
        CHECK(intersectArcArc(Arc::circle(Vec3::zero(), 10), Arc::circle(Vec3{1, 0}, 2)).empty());
    }

    TEST_CASE("handles externally tangent circles as one point") {
        const std::vector<Intersection> hits =
            intersectArcArc(Arc::circle(Vec3::zero(), 5), Arc::circle(Vec3{10, 0}, 5));
        REQUIRE(hits.size() == 1);
        CHECK_POINT_EPS(hits[0].point, Vec3(5, 0), 1e-6);
    }

    TEST_CASE("intersects a polyline and remaps parameters onto the whole curve") {
        const Polyline square = Polyline::rectangle(Vec3{0, 0}, Vec3{10, 10});
        const LineSegment crossing{Vec3{-5, 5}, Vec3{15, 5}};
        const std::vector<Intersection> hits = intersectCurves(crossing, square);
        REQUIRE(hits.size() == 2);

        std::vector<double> xs{hits[0].point.x, hits[1].point.x};
        std::sort(xs.begin(), xs.end());
        CHECK(xs[0] == doctest::Approx(0.0));
        CHECK(xs[1] == doctest::Approx(10.0));

        for (const Intersection& hit : hits) {
            CHECK(hit.paramB >= 0.0);
            CHECK(hit.paramB <= 1.0);
            // The remapped parameter must actually locate the hit.
            CHECK_POINT_EPS(square.pointAt(hit.paramB), hit.point, 1e-6);
        }
    }

    TEST_CASE("intersects an ellipse numerically to high precision") {
        const EllipseArc ellipse{Vec3::zero(), Vec3{10, 0}, 0.5};
        const LineSegment line{Vec3{0, -10}, Vec3{0, 10}};
        const std::vector<Intersection> hits = intersectCurves(line, ellipse);
        REQUIRE(hits.size() == 2);

        std::vector<double> ys{hits[0].point.y, hits[1].point.y};
        std::sort(ys.begin(), ys.end());
        CHECK(ys[0] == doctest::Approx(-5.0).epsilon(1e-6));
        CHECK(ys[1] == doctest::Approx(5.0).epsilon(1e-6));
    }

    TEST_CASE("detects polyline self-intersection but not adjacent joints") {
        // A bow-tie crosses itself once in the middle.
        const Polyline bowtie =
            Polyline::fromPoints({Vec3{0, 0}, Vec3{10, 10}, Vec3{10, 0}, Vec3{0, 10}});
        const std::vector<Intersection> hits = selfIntersections(bowtie);
        REQUIRE(hits.size() == 1);
        CHECK_POINT_EPS(hits[0].point, Vec3(5, 5), 1e-6);

        // A rectangle has none, despite every corner being a segment meeting.
        CHECK(selfIntersections(Polyline::rectangle(Vec3{0, 0}, Vec3{5, 5})).empty());
    }

    TEST_CASE("collects and sorts intersections against a set of curves") {
        const LineSegment target{Vec3{0, 0}, Vec3{10, 0}};
        const LineSegment cutterA{Vec3{7, -1}, Vec3{7, 1}};
        const LineSegment cutterB{Vec3{3, -1}, Vec3{3, 1}};
        const std::array<const Curve*, 2> others{&cutterA, &cutterB};

        const std::vector<Intersection> hits = intersectAgainstAll(target, others);
        REQUIRE(hits.size() == 2);
        // Sorted along the target curve, so 3 comes before 7.
        CHECK(hits[0].point.x == doctest::Approx(3.0));
        CHECK(hits[1].point.x == doctest::Approx(7.0));
    }
}

TEST_CASE("an ellipse whose ratio exceeds one is the same ellipse with its axes the other way round") {
    // Files carry such ratios: a hatch edge at 1.0001 from rounding, or a
    // genuine 2. Refusing them threw away whole drawings.
    const EllipseArc nearly{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}, 1.0001, 0.0, 0.0};
    CHECK(nearly.ratio == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(nearly.majorAxis.length() == doctest::Approx(10.0).epsilon(1e-3));

    const EllipseArc tall{Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}, 2.0, 0.0, 0.0};
    CHECK(tall.ratio == doctest::Approx(0.5));
    CHECK(tall.majorAxis.x == doctest::Approx(0.0));
    CHECK(std::abs(tall.majorAxis.y) == doctest::Approx(20.0));
    // The rim still passes through the points the file describes: 10 along
    // the stated axis and 20 along the other.
    CHECK(tall.closestPoint(Vec3{10.0, 0.0, 0.0}).distance == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(tall.closestPoint(Vec3{0.0, 20.0, 0.0}).distance == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(tall.closestPoint(Vec3{20.0, 0.0, 0.0}).distance > 5.0);
}

TEST_CASE("a reversed arc is the same arc walked backwards, not the other arc") {
    // A quarter circle in the first quadrant. Reversed, it must start where
    // this one ends, pass through the same midpoint, and stay in the first
    // quadrant -- the complementary three-quarter arc would dip below.
    const Arc quarter{Vec3{0.0, 0.0, 0.0}, 10.0, 0.0, kPi / 2.0};
    const CurvePtr back = quarter.reversed();
    REQUIRE(back != nullptr);
    CHECK(back->startPoint().equals(quarter.endPoint(), Tolerance{1e-9}));
    CHECK(back->endPoint().equals(quarter.startPoint(), Tolerance{1e-9}));
    CHECK(back->midPoint().equals(quarter.midPoint(), Tolerance{1e-9}));
    CHECK(back->length() == doctest::Approx(quarter.length()));
    CHECK(back->bounds().min.y == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(back->bounds().min.x == doctest::Approx(0.0).epsilon(1e-9));

    // Reversing twice gives the original back.
    const CurvePtr again = back->reversed();
    CHECK(again->startPoint().equals(quarter.startPoint(), Tolerance{1e-9}));
    CHECK(again->midPoint().equals(quarter.midPoint(), Tolerance{1e-9}));

    // As a polyline vertex the reversed arc bulges the other way.
    const auto* reversedArc = dynamic_cast<const Arc*>(back.get());
    REQUIRE(reversedArc != nullptr);
    CHECK(reversedArc->bulge() == doctest::Approx(-quarter.bulge()));
    CHECK(quarter.bulge() > 0.0);
}
