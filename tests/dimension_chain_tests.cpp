// DIMCONTINUE and DIMBASELINE: runs of dimensions off a linear one.
#include "test_helpers.hpp"

#include <memory>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct ChainHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    [[nodiscard]] std::vector<DimensionEntity*> dimensions() {
        std::vector<DimensionEntity*> found;
        for (Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (auto* dimension = dynamic_cast<DimensionEntity*>(entity);
                dimension != nullptr && !dimension->erased) {
                found.push_back(dimension);
            }
        }
        return found;
    }

    /// A horizontal dimension from (0,0) to (40,0) with its line at y = 10.
    void linear() {
        REQUIRE(executor.execute("DIMLINEAR").status == CommandOutcome::Status::Running);
        executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        executor.supply(InputValue::makePoint(Vec3{40.0, 0.0, 0.0}));
        REQUIRE(executor.supply(InputValue::makePoint(Vec3{20.0, 10.0, 0.0})).status ==
                CommandOutcome::Status::Completed);
    }
};

[[nodiscard]] InputValue at(double x, double y) {
    return InputValue::makePoint(Vec3{x, y, 0.0});
}

}  // namespace

TEST_CASE("DIMCONTINUE chains dimensions end to end on the same line") {
    ChainHarness h;
    h.linear();
    REQUIRE(h.executor.execute("DIMCONTINUE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(70.0, 3.0));   // a little off the line: measured along it
    h.executor.supply(at(100.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);

    const auto dims = h.dimensions();
    REQUIRE(dims.size() == 3);
    const DimensionEntity& second = *dims[1];
    CHECK(second.type == DimensionType::Rotated);
    CHECK(second.point13.x == doctest::Approx(40.0));
    CHECK(second.point14.x == doctest::Approx(70.0));
    CHECK(second.definitionPoint.y == doctest::Approx(10.0));  // the same line
    CHECK(second.measure() == doctest::Approx(30.0));
    const DimensionEntity& third = *dims[2];
    CHECK(third.point13.x == doctest::Approx(70.0));
    CHECK(third.point14.x == doctest::Approx(100.0));
    CHECK(third.definitionPoint.y == doctest::Approx(10.0));
    CHECK(third.measure() == doctest::Approx(30.0));
}

TEST_CASE("DIMBASELINE fans dimensions out from the first origin, a spacing apart") {
    ChainHarness h;
    h.linear();
    const double spacing = h.db->dimStyleByHandle(h.dimensions().front()->styleHandle).baselineSpacing;
    REQUIRE(h.executor.execute("DIMBASELINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(70.0, 0.0));
    h.executor.supply(at(100.0, 0.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);

    const auto dims = h.dimensions();
    REQUIRE(dims.size() == 3);
    CHECK(dims[1]->point13.x == doctest::Approx(0.0));
    CHECK(dims[1]->point14.x == doctest::Approx(70.0));
    CHECK(dims[1]->measure() == doctest::Approx(70.0));
    // Away from the points: the base line is at y = 10 above them, so the
    // next lines are further up.
    CHECK(dims[1]->definitionPoint.y == doctest::Approx(10.0 + spacing));
    CHECK(dims[2]->point13.x == doctest::Approx(0.0));
    CHECK(dims[2]->point14.x == doctest::Approx(100.0));
    CHECK(dims[2]->definitionPoint.y == doctest::Approx(10.0 + 2.0 * spacing));
}

TEST_CASE("with no dimension to continue, the base is asked for, and must be linear") {
    ChainHarness h;
    REQUIRE(h.executor.execute("DIMCONTINUE").status == CommandOutcome::Status::Running);
    CHECK(h.executor.currentRequest() != nullptr);
    CHECK(h.executor.supply(InputValue::makeEntity(Handle{0xFFFF})).status ==
          CommandOutcome::Status::Failed);
}

TEST_CASE("Select picks a different dimension to continue from") {
    ChainHarness h;
    h.linear();
    // A second, vertical dimension is now the newest; Select goes back to the first.
    REQUIRE(h.executor.execute("DIMLINEAR").status == CommandOutcome::Status::Running);
    h.executor.supply(at(200.0, 0.0));
    h.executor.supply(at(200.0, 50.0));
    h.executor.supply(at(210.0, 25.0));
    const Handle first = h.dimensions().front()->handle;

    REQUIRE(h.executor.execute("DIMCONTINUE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeEntity(first));
    h.executor.supply(at(60.0, 0.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    const auto dims = h.dimensions();
    REQUIRE(dims.size() == 3);
    CHECK(dims.back()->point13.x == doctest::Approx(40.0));
    CHECK(dims.back()->point14.x == doctest::Approx(60.0));
    CHECK(dims.back()->definitionPoint.y == doctest::Approx(10.0));
}

TEST_CASE("Enter with nothing placed cancels rather than reporting a run") {
    ChainHarness h;
    h.linear();
    REQUIRE(h.executor.execute("DIMCONTINUE").status == CommandOutcome::Status::Running);
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Cancelled);
    CHECK(h.dimensions().size() == 1);
}
