// Finding the region around a point, the way picking inside a room for a
// hatch needs it.
#include "test_helpers.hpp"

#include <memory>
#include <vector>

#include "cad/geom/Arc.hpp"
#include "cad/geom/BoundaryTrace.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"
#include "cad/math/Angle.hpp"

using namespace cad;

namespace {

struct Curves {
    std::vector<CurvePtr> owned;

    void line(double x0, double y0, double x1, double y1) {
        owned.push_back(std::make_unique<LineSegment>(Vec3{x0, y0, 0.0}, Vec3{x1, y1, 0.0}));
    }
    void circle(double cx, double cy, double r) {
        owned.push_back(std::make_unique<Arc>(Vec3{cx, cy, 0.0}, r, 0.0, kTau, Vec3::unitZ(), true));
    }
    [[nodiscard]] std::vector<const Curve*> view() const {
        std::vector<const Curve*> pointers;
        for (const CurvePtr& curve : owned) {
            pointers.push_back(curve.get());
        }
        return pointers;
    }
    [[nodiscard]] BoundaryTraceResult trace(double x, double y) const {
        const std::vector<const Curve*> pointers = view();
        return traceBoundary(pointers, Vec3{x, y, 0.0});
    }
};

[[nodiscard]] double loopLength(const BoundaryLoop& loop) {
    double total = 0.0;
    for (const CurvePtr& curve : loop.curves) {
        total += curve->length();
    }
    return total;
}

[[nodiscard]] bool loopIsContinuous(const BoundaryLoop& loop) {
    for (std::size_t i = 0; i < loop.curves.size(); ++i) {
        const Curve& a = *loop.curves[i];
        const Curve& b = *loop.curves[(i + 1) % loop.curves.size()];
        if (a.endPoint().distanceTo(b.startPoint()) > 1e-6) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Four lines around a point trace into one ring") {
    Curves c;
    c.line(0.0, 0.0, 100.0, 0.0);
    c.line(100.0, 0.0, 100.0, 50.0);
    c.line(100.0, 50.0, 0.0, 50.0);
    c.line(0.0, 50.0, 0.0, 0.0);
    const BoundaryTraceResult result = c.trace(30.0, 20.0);
    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1);
    CHECK(result.loops[0].curves.size() == 4);
    CHECK(loopIsContinuous(result.loops[0]));
    CHECK(loopLength(result.loops[0]) == doctest::Approx(300.0));
}

TEST_CASE("Lines that run past the corners are cut where they cross") {
    // A tic-tac-toe grid: nine cells, no line ends at a corner of the middle one.
    Curves c;
    c.line(-50.0, 10.0, 150.0, 10.0);
    c.line(-50.0, 40.0, 150.0, 40.0);
    c.line(20.0, -50.0, 20.0, 100.0);
    c.line(70.0, -50.0, 70.0, 100.0);
    const BoundaryTraceResult result = c.trace(45.0, 25.0);
    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1);
    CHECK(result.loops[0].curves.size() == 4);
    CHECK(loopLength(result.loops[0]) == doctest::Approx(2.0 * (50.0 + 30.0)));
    // Every piece came from one of the four inputs.
    for (const std::size_t source : result.loops[0].sources) {
        CHECK(source < 4);
    }
}

TEST_CASE("Two rooms sharing a wall each trace to their own side") {
    Curves c;
    c.line(0.0, 0.0, 200.0, 0.0);
    c.line(200.0, 0.0, 200.0, 100.0);
    c.line(200.0, 100.0, 0.0, 100.0);
    c.line(0.0, 100.0, 0.0, 0.0);
    c.line(100.0, 0.0, 100.0, 100.0);   // the party wall
    const BoundaryTraceResult left = c.trace(50.0, 50.0);
    REQUIRE(left.ok());
    CHECK(loopLength(left.loops[0]) == doctest::Approx(400.0));
    const BoundaryTraceResult right = c.trace(150.0, 50.0);
    REQUIRE(right.ok());
    CHECK(loopLength(right.loops[0]) == doctest::Approx(400.0));
}

TEST_CASE("A line dangling into the room is not part of its boundary") {
    Curves c;
    c.line(0.0, 0.0, 100.0, 0.0);
    c.line(100.0, 0.0, 100.0, 100.0);
    c.line(100.0, 100.0, 0.0, 100.0);
    c.line(0.0, 100.0, 0.0, 0.0);
    c.line(0.0, 50.0, 40.0, 50.0);   // a stub off the left wall
    const BoundaryTraceResult result = c.trace(70.0, 50.0);
    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1);
    CHECK(loopLength(result.loops[0]) == doctest::Approx(400.0));
    CHECK(loopIsContinuous(result.loops[0]));
}

TEST_CASE("A circle inside the room is reported as an island") {
    Curves c;
    c.line(0.0, 0.0, 100.0, 0.0);
    c.line(100.0, 0.0, 100.0, 100.0);
    c.line(100.0, 100.0, 0.0, 100.0);
    c.line(0.0, 100.0, 0.0, 0.0);
    c.circle(50.0, 50.0, 10.0);
    const BoundaryTraceResult result = c.trace(10.0, 10.0);
    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 2);
    CHECK(loopLength(result.loops[0]) == doctest::Approx(400.0));
    CHECK(loopLength(result.loops[1]) == doctest::Approx(kTau * 10.0));

    // Picking inside the circle hatches the circle, with no island.
    const BoundaryTraceResult inner = c.trace(50.0, 50.0);
    REQUIRE(inner.ok());
    REQUIRE(inner.loops.size() == 1);
    CHECK(loopLength(inner.loops[0]) == doctest::Approx(kTau * 10.0));
}

TEST_CASE("A closed polyline is a boundary on its own, and is cut when crossed") {
    Curves c;
    std::vector<PolylineVertex> vertices;
    for (const Vec3 corner : {Vec3{0.0, 0.0, 0.0}, Vec3{100.0, 0.0, 0.0}, Vec3{100.0, 60.0, 0.0},
                              Vec3{0.0, 60.0, 0.0}}) {
        PolylineVertex vertex;
        vertex.position = corner;
        vertices.push_back(vertex);
    }
    c.owned.push_back(std::make_unique<Polyline>(std::move(vertices), true));
    c.line(50.0, -10.0, 50.0, 70.0);
    const BoundaryTraceResult result = c.trace(25.0, 30.0);
    REQUIRE(result.ok());
    REQUIRE(result.loops.size() == 1);
    CHECK(loopLength(result.loops[0]) == doctest::Approx(2.0 * (50.0 + 60.0)));
}

TEST_CASE("An open shape and an empty spot are reported, not guessed at") {
    Curves c;
    c.line(0.0, 0.0, 100.0, 0.0);
    c.line(100.0, 0.0, 100.0, 100.0);
    c.line(100.0, 100.0, 0.0, 100.0);   // no left wall
    const BoundaryTraceResult open = c.trace(50.0, 50.0);
    CHECK_FALSE(open.ok());
    CHECK_FALSE(open.error.empty());

    c.line(0.0, 100.0, 0.0, 0.0);
    const BoundaryTraceResult outside = c.trace(150.0, 50.0);
    CHECK_FALSE(outside.ok());
    CHECK_FALSE(outside.error.empty());
}
