// The layer tools: LAYISO, LAYUNISO, LAYOFF, LAYON, LAYFRZ, LAYTHW, LAYLCK, LAYULK.
#include "test_helpers.hpp"

#include <memory>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct LayerHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};
    Handle onWalls;
    Handle onDoors;
    Handle onZero;

    LayerHarness() {
        db->ensureLayer("WALLS");
        db->ensureLayer("DOORS");
        db->ensureLayer("NOTES");
        onWalls = add("WALLS", 0.0);
        onDoors = add("DOORS", 10.0);
        onZero = add("0", 20.0);
    }

    Handle add(const char* layer, double y) {
        auto line = std::make_unique<LineEntity>(Vec3{0.0, y, 0.0}, Vec3{10.0, y, 0.0});
        line->layerHandle = db->findLayer(layer)->handle;
        return db->addToModelSpace(std::move(line)).handle;
    }

    [[nodiscard]] Layer& layer(const char* name) { return *db->findLayer(name); }

    CommandOutcome run(const char* command, std::vector<Handle> picked) {
        REQUIRE(executor.execute(command).status == CommandOutcome::Status::Running);
        return executor.supply(InputValue::makeSelection(std::move(picked)));
    }
};

}  // namespace

TEST_CASE("LAYOFF turns off the selected objects' layers, but never the current one") {
    LayerHarness h;
    CHECK(h.run("LAYOFF", {h.onWalls, h.onZero}).status == CommandOutcome::Status::Completed);
    CHECK(h.layer("WALLS").isOff);
    CHECK_FALSE(h.layer("0").isOff);   // current
    CHECK_FALSE(h.layer("DOORS").isOff);
    REQUIRE(h.tm.undo());
    CHECK_FALSE(h.layer("WALLS").isOff);
}

TEST_CASE("LAYFRZ freezes, LAYTHW thaws everything") {
    LayerHarness h;
    CHECK(h.run("LAYFRZ", {h.onWalls, h.onDoors}).status == CommandOutcome::Status::Completed);
    CHECK(h.layer("WALLS").isFrozen);
    CHECK(h.layer("DOORS").isFrozen);
    CHECK_FALSE(h.layer("0").isFrozen);
    CHECK(h.executor.execute("LAYTHW").status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.layer("WALLS").isFrozen);
    CHECK_FALSE(h.layer("DOORS").isFrozen);
}

TEST_CASE("LAYISO leaves only the chosen layers on and makes the first current; LAYUNISO puts the rest back") {
    LayerHarness h;
    h.layer("NOTES").isOff = true;  // off before: must stay off after unisolate
    CHECK(h.run("LAYISO", {h.onDoors}).status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.layer("DOORS").isOff);
    CHECK(h.layer("WALLS").isOff);
    CHECK(h.layer("0").isOff);
    CHECK(h.layer("NOTES").isOff);
    CHECK(h.db->settings.currentLayerName == "DOORS");

    CHECK(h.executor.execute("LAYUNISO").status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.layer("WALLS").isOff);
    CHECK_FALSE(h.layer("0").isOff);
    CHECK(h.layer("NOTES").isOff);
    CHECK(h.db->settings.isolatedLayers.empty());
}

TEST_CASE("LAYON turns every layer on") {
    LayerHarness h;
    h.layer("NOTES").isOff = true;
    h.layer("WALLS").isOff = true;
    CHECK(h.executor.execute("LAYON").status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.layer("NOTES").isOff);
    CHECK_FALSE(h.layer("WALLS").isOff);
}

TEST_CASE("LAYLCK and LAYULK lock and unlock by pointing") {
    LayerHarness h;
    CHECK(h.run("LAYLCK", {h.onWalls}).status == CommandOutcome::Status::Completed);
    CHECK(h.layer("WALLS").isLocked);
    CHECK(h.run("LAYULK", {h.onWalls}).status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.layer("WALLS").isLocked);
}

TEST_CASE("a layer tool takes the selection already made") {
    LayerHarness h;
    h.selection.setSelection({h.onWalls});
    CHECK(h.executor.execute("LAYOFF").status == CommandOutcome::Status::Completed);
    CHECK(h.layer("WALLS").isOff);
}
