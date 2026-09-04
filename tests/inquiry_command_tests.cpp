// SPLINE, and the small tools: LIST, MEASURE, DIVIDE.
#include "test_helpers.hpp"

#include <memory>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/SplineEntity.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct ToolHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    template <typename T>
    [[nodiscard]] std::vector<T*> inModel() {
        std::vector<T*> found;
        for (Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (auto* typed = dynamic_cast<T*>(entity); typed != nullptr && !typed->erased) {
                found.push_back(typed);
            }
        }
        return found;
    }

    void begin(const char* command) {
        REQUIRE(executor.execute(command).status == CommandOutcome::Status::Running);
    }
    void point(double x, double y) { executor.supply(InputValue::makePoint(Vec3{x, y, 0.0})); }
};

}  // namespace

TEST_CASE("SPLINE passes through every point picked, smoothly, and remembers them") {
    ToolHarness h;
    h.begin("SPLINE");
    h.point(0.0, 0.0);
    h.point(10.0, 10.0);
    h.point(20.0, 0.0);
    h.point(30.0, 10.0);
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);

    const auto splines = h.inModel<SplineEntity>();
    REQUIRE(splines.size() == 1);
    const SplineEntity& spline = *splines.front();
    // By control vertices, not fit points, so other CAD software draws exactly this
    // curve rather than re-fitting one of its own through the picks.
    CHECK(spline.fitPoints.empty());
    CHECK(spline.curve.degree == 3);
    CHECK(spline.curve.controlPoints.size() == 10);
    CHECK(spline.curve.knots.size() == 14);
    // The ends are the first and last picks, and the curve runs through the
    // middle ones: the closest point on the curve to each is the point itself.
    CHECK(spline.curve.startPoint().distanceTo(Vec3{0.0, 0.0, 0.0}) < 1e-9);
    CHECK(spline.curve.endPoint().distanceTo(Vec3{30.0, 10.0, 0.0}) < 1e-9);
    for (const Vec3 fit : {Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 10.0, 0.0}, Vec3{20.0, 0.0, 0.0}, Vec3{30.0, 10.0, 0.0}}) {
        CHECK(spline.curve.closestPoint(fit).point.distanceTo(fit) < 1e-6);
    }
    CHECK(spline.curve.length() > 40.0);   // longer than the zigzag's straight legs would be short
}

TEST_CASE("SPLINE Close joins the last point back to the first") {
    ToolHarness h;
    h.begin("SPLINE");
    h.point(0.0, 0.0);
    h.point(10.0, 0.0);
    h.point(10.0, 10.0);
    CHECK(h.executor.supply(InputValue::makeKeyword("C")).status == CommandOutcome::Status::Completed);
    const auto splines = h.inModel<SplineEntity>();
    REQUIRE(splines.size() == 1);
    CHECK((splines.front()->flags & 1) != 0);
    CHECK(splines.front()->curve.startPoint().distanceTo(splines.front()->curve.endPoint()) < 1e-9);
}

TEST_CASE("DIVIDE places points that split an object into equal parts") {
    ToolHarness h;
    const Handle line =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{100.0, 0.0, 0.0})).handle;
    h.begin("DIVIDE");
    h.executor.supply(InputValue::makeEntity(line));
    CHECK(h.executor.supply(InputValue::makeNumber(4.0)).status == CommandOutcome::Status::Completed);
    const auto points = h.inModel<PointEntity>();
    REQUIRE(points.size() == 3);
    CHECK(points[0]->position.x == doctest::Approx(25.0));
    CHECK(points[1]->position.x == doctest::Approx(50.0));
    CHECK(points[2]->position.x == doctest::Approx(75.0));
}

TEST_CASE("MEASURE places points every given distance, from the start") {
    ToolHarness h;
    auto circle = std::make_unique<CircleEntity>(Vec3{0.0, 0.0, 0.0}, 10.0);
    const Handle handle = h.db->addToModelSpace(std::move(circle)).handle;
    h.selection.setSelection({handle});
    h.begin("MEASURE");
    // Circumference 62.83: at 20 and 40 and 60, three points.
    CHECK(h.executor.supply(InputValue::makeNumber(20.0)).status == CommandOutcome::Status::Completed);
    const auto points = h.inModel<PointEntity>();
    REQUIRE(points.size() == 3);
    for (const PointEntity* point : points) {
        CHECK(point->position.length2() == doctest::Approx(10.0).epsilon(1e-3));
    }
}

TEST_CASE("LIST reports what an object is without changing anything") {
    ToolHarness h;
    const Handle line =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{30.0, 40.0, 0.0})).handle;
    const std::size_t before = h.executor.history().size();
    h.begin("LIST");
    const CommandOutcome outcome = h.executor.supply(InputValue::makeSelection({line}));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    bool sawLength = false;
    for (std::size_t i = before; i < h.executor.history().size(); ++i) {
        sawLength = sawLength || h.executor.history()[i].text.find("Length = 50") != std::string::npos;
    }
    CHECK(sawLength);
    CHECK(h.inModel<LineEntity>().size() == 1);
}
