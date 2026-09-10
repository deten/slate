// MLINE: two parallel lines drawn as one, for walls.
#include "test_helpers.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/MlineEntity.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registryOwner = createStandardRegistry();
    CommandRegistry& registry = *registryOwner;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};

    [[nodiscard]] const MlineEntity* mline() const {
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (const auto* found = dynamic_cast<const MlineEntity*>(entity);
                found != nullptr && !found->erased) {
                return found;
            }
        }
        return nullptr;
    }
};

[[nodiscard]] InputValue at(double x, double y) { return InputValue::makePoint(Vec3{x, y, 0.0}); }

}  // namespace

TEST_SUITE("MLINE") {

TEST_CASE("a straight multiline is two lines the scale apart about the picked points") {
    Harness h;
    REQUIRE(h.executor.execute("MLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeNumber(10.0));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(200.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message == "Multiline drawn, 1 segment");

    const MlineEntity* mline = h.mline();
    REQUIRE(mline != nullptr);
    CHECK(mline->scale == doctest::Approx(10.0));
    REQUIRE(mline->vertices.size() == 2);
    const std::vector<std::vector<Vec3>> lines = mline->elementLines();
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].size() == 2);
    // Centred: one line 5 above, one 5 below.
    CHECK(lines[0][0].y == doctest::Approx(5.0));
    CHECK(lines[1][0].y == doctest::Approx(-5.0));
    CHECK(lines[0][1].x == doctest::Approx(200.0));
    CHECK(h.transactions.undo());
    CHECK(h.mline() == nullptr);
}

TEST_CASE("a corner is mitred so the two lines meet") {
    Harness h;
    REQUIRE(h.executor.execute("MLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeNumber(10.0));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(100.0, 0.0));
    h.executor.supply(at(100.0, 100.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    const MlineEntity* mline = h.mline();
    REQUIRE(mline != nullptr);
    const std::vector<std::vector<Vec3>> lines = mline->elementLines();
    REQUIRE(lines[0].size() == 3);
    // At the corner the outer line's mitre point is 5 out on both sides:
    // the corner of the outer offset lines, at 105,-5; the inner at 95,5.
    const Vec3 outer = lines[1][1];
    const Vec3 inner = lines[0][1];
    CHECK(outer.x == doctest::Approx(105.0));
    CHECK(outer.y == doctest::Approx(-5.0));
    CHECK(inner.x == doctest::Approx(95.0));
    CHECK(inner.y == doctest::Approx(5.0));
}

TEST_CASE("justification puts the picked points on the top or the bottom line") {
    Harness h;
    REQUIRE(h.executor.execute("MLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("J"));
    h.executor.supply(InputValue::makeKeyword("T"));
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeNumber(8.0));
    h.executor.supply(at(0.0, 50.0));
    h.executor.supply(at(100.0, 50.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    const std::vector<std::vector<Vec3>> lines = h.mline()->elementLines();
    CHECK(lines[0][0].y == doctest::Approx(50.0));
    CHECK(lines[1][0].y == doctest::Approx(42.0));
    CHECK(h.mline()->justification == 0);
}

TEST_CASE("Close joins the multiline back to its start, and it survives a file") {
    Harness h;
    REQUIRE(h.executor.execute("MLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(100.0, 0.0));
    h.executor.supply(at(100.0, 100.0));
    h.executor.supply(at(0.0, 100.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeKeyword("C"));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message == "Multiline drawn, 4 segments");
    const MlineEntity* mline = h.mline();
    REQUIRE(mline != nullptr);
    CHECK(mline->closed);
    const std::vector<std::vector<Vec3>> lines = mline->elementLines();
    REQUIRE(lines[0].size() == 5);
    CHECK(lines[0].front().equals(lines[0].back(), Tolerance{1e-9}));

    io::DxfWriter writer;
    const std::string text = writer.write(*h.db);
    io::DxfReadResult result = io::readDxf(text);
    REQUIRE(result.database != nullptr);
    const MlineEntity* back = nullptr;
    for (const Entity* entity : result.database->entitiesIn(result.database->modelSpace())) {
        back = dynamic_cast<const MlineEntity*>(entity);
        if (back != nullptr) {
            break;
        }
    }
    REQUIRE(back != nullptr);
    CHECK(back->closed);
    CHECK(back->vertices.size() == 4);
    CHECK(back->elementLines()[1][2].x == doctest::Approx(lines[1][2].x));
}

TEST_CASE("a multiline needs two points") {
    Harness h;
    REQUIRE(h.executor.execute("MLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Cancelled);
    CHECK(h.mline() == nullptr);
}

}  // TEST_SUITE
