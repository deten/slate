// PLINE's arc, width and length options, and PEDIT.
#include "test_helpers.hpp"

#include <cmath>
#include <memory>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/geom/Arc.hpp"

using namespace cad;

namespace {

struct PolyHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    [[nodiscard]] std::vector<PolylineEntity*> polylines() {
        std::vector<PolylineEntity*> found;
        for (Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (auto* poly = dynamic_cast<PolylineEntity*>(entity); poly != nullptr && !poly->erased) {
                found.push_back(poly);
            }
        }
        return found;
    }

    void begin(const char* command) {
        REQUIRE(executor.execute(command).status == CommandOutcome::Status::Running);
    }
    void point(double x, double y) { executor.supply(InputValue::makePoint(Vec3{x, y, 0.0})); }
    void key(const char* k) { executor.supply(InputValue::makeKeyword(k)); }
    void number(double n) { executor.supply(InputValue::makeNumber(n)); }
    CommandOutcome enter() { return executor.supply(InputValue::makeEmpty()); }
};

}  // namespace

TEST_CASE("PLINE CEnter makes an arc about the given centre") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.point(10.0, 0.0);
    h.key("A");
    h.key("CE");
    h.point(10.0, 5.0);   // centre above the last point: a half turn up and back
    h.point(10.0, 10.0);
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    REQUIRE(polys.front()->vertices.size() == 3);
    // A semicircle counter-clockwise: bulge tan(pi/4) = 1.
    CHECK(polys.front()->vertices[1].bulge == doctest::Approx(1.0));
}

TEST_CASE("PLINE Radius makes the minor arc of that radius") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.key("A");
    h.key("R");
    h.number(10.0);
    h.point(10.0, 10.0);   // chord 14.14 on a radius 10: a quarter turn
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    CHECK(polys.front()->vertices[0].bulge == doctest::Approx(std::tan(kPi / 8.0)));
}

TEST_CASE("PLINE Second point makes the arc through it, turning the way the points turn") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.key("A");
    h.key("S");
    h.point(10.0, 10.0);   // above the chord, left to right: clockwise
    h.point(20.0, 0.0);
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    CHECK(polys.front()->vertices[0].bulge == doctest::Approx(-1.0));
}

TEST_CASE("PLINE Direction and Angle set the arc from a tangent or a sweep") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.key("A");
    h.key("D");
    h.point(0.0, 1.0);      // leaving straight up
    h.point(10.0, 0.0);     // to the right: a clockwise half circle... no, a quarter? It is a semicircle.
    h.key("AN");
    h.number(kPi / 2.0);
    h.point(20.0, 0.0);
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    REQUIRE(polys.front()->vertices.size() == 3);
    // Leaving upward and landing to the right is a clockwise semicircle.
    CHECK(polys.front()->vertices[0].bulge == doctest::Approx(-1.0));
    CHECK(polys.front()->vertices[1].bulge == doctest::Approx(std::tan(kPi / 8.0)));
}

TEST_CASE("PLINE Width applies to the segments that follow, and Length goes straight on") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.point(10.0, 0.0);
    h.key("W");
    h.number(2.0);
    h.number(4.0);
    h.point(20.0, 0.0);
    h.key("LE");
    h.number(5.0);
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    const auto& v = polys.front()->vertices;
    REQUIRE(v.size() == 4);
    CHECK(v[0].startWidth == doctest::Approx(0.0));
    CHECK(v[1].startWidth == doctest::Approx(2.0));
    CHECK(v[1].endWidth == doctest::Approx(4.0));
    CHECK(v[2].startWidth == doctest::Approx(4.0));
    CHECK(v[3].position.x == doctest::Approx(25.0));
    CHECK(v[3].position.y == doctest::Approx(0.0));
}

TEST_CASE("PEDIT closes, opens, widens, reverses and straightens") {
    PolyHarness h;
    h.begin("PLINE");
    h.point(0.0, 0.0);
    h.point(10.0, 0.0);
    h.key("A");
    h.point(20.0, 10.0);
    h.enter();
    PolylineEntity& poly = *h.polylines().front();
    const double bulge = poly.vertices[1].bulge;
    REQUIRE(bulge != 0.0);

    h.begin("PEDIT");
    h.executor.supply(InputValue::makeEntity(poly.handle));
    h.key("C");
    CHECK(poly.closed);
    h.key("O");
    CHECK_FALSE(poly.closed);
    h.key("W");
    h.number(1.5);
    CHECK(poly.constantWidth == doctest::Approx(1.5));
    h.key("R");
    CHECK(poly.vertices.front().position.x == doctest::Approx(20.0));
    CHECK(poly.vertices.front().bulge == doctest::Approx(-bulge));
    h.key("D");
    CHECK(poly.vertices.front().bulge == doctest::Approx(0.0));
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
}

TEST_CASE("PEDIT turns a line into a polyline and joins what meets its ends") {
    PolyHarness h;
    const Handle line =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0})).handle;
    const Handle next =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{10.0, 0.0, 0.0}, Vec3{10.0, 10.0, 0.0})).handle;
    const Handle apart =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{50.0, 0.0, 0.0}, Vec3{60.0, 0.0, 0.0})).handle;

    h.begin("PEDIT");
    h.executor.supply(InputValue::makeEntity(line));
    REQUIRE(h.polylines().size() == 1);   // the line became one
    h.key("J");
    h.executor.supply(InputValue::makeSelection({next, apart}));
    CHECK(h.enter().status == CommandOutcome::Status::Completed);

    const auto polys = h.polylines();
    REQUIRE(polys.size() == 1);
    CHECK(polys.front()->vertices.size() == 3);
    CHECK(h.db->getEntity(next)->erased);
    CHECK_FALSE(h.db->getEntity(apart)->erased);   // did not meet: left alone
}
