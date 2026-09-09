// DUCT: a run of duct in one command, drawn the office way.
#include "test_helpers.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registryOwner = createStandardRegistry();
    CommandRegistry& registry = *registryOwner;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};

    [[nodiscard]] std::vector<const PolylineEntity*> polylines() const {
        std::vector<const PolylineEntity*> out;
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (const auto* polyline = dynamic_cast<const PolylineEntity*>(entity);
                polyline != nullptr && !polyline->erased) {
                out.push_back(polyline);
            }
        }
        return out;
    }
    [[nodiscard]] std::vector<const TextEntity*> texts() const {
        std::vector<const TextEntity*> out;
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (const auto* text = dynamic_cast<const TextEntity*>(entity);
                text != nullptr && !text->erased) {
                out.push_back(text);
            }
        }
        return out;
    }
    [[nodiscard]] std::string layerOf(const Entity& entity) const {
        const auto* layer = db->get<Layer>(entity.layerHandle);
        return layer == nullptr ? std::string{} : layer->name;
    }
};

[[nodiscard]] InputValue at(double x, double y) { return InputValue::makePoint(Vec3{x, y, 0.0}); }

}  // namespace

TEST_SUITE("DUCT") {

TEST_CASE("a supply run is one wide polyline on the supply layer with its size along it") {
    Harness h;
    REQUIRE(h.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeText("24x12"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(400.0, 0.0));
    h.executor.supply(at(400.0, 300.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message.find("supply duct 24x12 drawn, 2 legs") != std::string::npos);

    const auto runs = h.polylines();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0]->constantWidth == doctest::Approx(24.0));
    CHECK(runs[0]->vertices.size() == 3);
    CHECK(h.layerOf(*runs[0]) == "M-DUCT-SUPP");
    // The layer was made with the usual colour.
    CHECK(h.db->findLayer("M-DUCT-SUPP")->color.index() == 4);

    const auto tags = h.texts();
    REQUIRE(tags.size() == 1);
    CHECK(tags[0]->contents == "24x12");
    CHECK(h.layerOf(*tags[0]) == "M-DUCT-SUPP");
    // Along the longest leg, which runs along +X, just above it.
    CHECK(tags[0]->rotation == doctest::Approx(0.0));
    CHECK(tags[0]->position.y > 12.0);
    CHECK(tags[0]->position.x > 100.0);
    CHECK(tags[0]->position.x < 300.0);
    CHECK(tags[0]->height == doctest::Approx(24.0 * 0.35));

    // One command, one undo.
    CHECK(h.transactions.undo());
    CHECK(h.polylines().empty());
    CHECK(h.texts().empty());
}

TEST_CASE("a double-line run is two polylines the duct's width apart") {
    Harness h;
    REQUIRE(h.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("D"));
    h.executor.supply(InputValue::makeKeyword("R"));
    h.executor.supply(InputValue::makeText("20x10"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(500.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message.find("Double-line return duct 20x10") != std::string::npos);

    const auto runs = h.polylines();
    REQUIRE(runs.size() == 2);
    std::vector<double> ys;
    for (const PolylineEntity* run : runs) {
        CHECK(run->constantWidth == doctest::Approx(0.0));
        CHECK(h.layerOf(*run) == "M-DUCT-RETN");
        ys.push_back(run->vertices.front().position.y);
    }
    CHECK(std::abs(ys[0] - ys[1]) == doctest::Approx(20.0));
}

TEST_CASE("the drawing's own system layer is used when it has one") {
    Harness h;
    Layer& office = h.db->ensureLayer("HSDUCT");
    static_cast<void>(office);
    REQUIRE(h.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEmpty());  // supply, the default
    h.executor.supply(InputValue::makeText("12"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(0.0, 200.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    const auto runs = h.polylines();
    REQUIRE(runs.size() == 1);
    CHECK(h.layerOf(*runs[0]) == "HSDUCT");
    CHECK(runs[0]->constantWidth == doctest::Approx(12.0));
    // A leg running up reads bottom to top.
    CHECK(h.texts().front()->rotation == doctest::Approx(kPi / 2.0));
}

TEST_CASE("the size may come first, and a bad size is refused") {
    Harness h;
    REQUIRE(h.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeText("18x18"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(100.0, 0.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    CHECK(h.polylines().size() == 1);

    Harness bad;
    REQUIRE(bad.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    bad.executor.supply(InputValue::makeKeyword("S"));
    const CommandOutcome outcome = bad.executor.supply(InputValue::makeText("wide"));
    CHECK(outcome.status == CommandOutcome::Status::Failed);
    CHECK(bad.polylines().empty());
}

TEST_CASE("a run needs two points") {
    Harness h;
    REQUIRE(h.executor.execute("DUCT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    h.executor.supply(InputValue::makeText("10"));
    h.executor.supply(at(0.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Cancelled);
    CHECK(h.polylines().empty());
}

}  // TEST_SUITE
