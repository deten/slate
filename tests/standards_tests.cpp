// What the assistant is told: the drawing digest, and the house standards
// learned from a folder of drawings.
#include "test_helpers.hpp"

#include <memory>
#include <string>

#include "cad/agent/AgentProvider.hpp"
#include "cad/agent/DrawingStandards.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"

using namespace cad;

namespace {

[[nodiscard]] bool has(const std::string& text, const char* piece) {
    return text.find(piece) != std::string::npos;
}

/// A small office drawing: walls on A-WALL in red, a door block with a tag.
[[nodiscard]] std::unique_ptr<Database> officeDrawing(int walls, bool withDoor) {
    std::unique_ptr<Database> db = Database::createEmpty();
    Layer& wallLayer = db->ensureLayer("A-WALL");
    wallLayer.color = Color::fromIndex(1);
    wallLayer.description = "Walls, plan";
    db->ensureLayer("A-DOOR");
    db->ensureLayer("UNUSED");

    for (int i = 0; i < walls; ++i) {
        auto line = std::make_unique<LineEntity>(Vec3{0.0, 100.0 * i, 0.0},
                                                 Vec3{500.0, 100.0 * i, 0.0});
        line->layerHandle = wallLayer.handle;
        db->addToModelSpace(std::move(line));
    }

    if (withDoor) {
        auto block = std::make_unique<BlockTableRecord>();
        block->name = "DOOR-900";
        BlockTableRecord& record = db->addBlock(std::move(block));
        auto tag = std::make_unique<AttributeDefinitionEntity>();
        tag->tag = "MARK";
        db->addEntity(std::move(tag), record);
        auto insert = std::make_unique<InsertEntity>(record.handle, Vec3{50.0, 0.0, 0.0});
        insert->layerHandle = db->findLayer("A-DOOR")->handle;
        db->addToModelSpace(std::move(insert));
    }
    return db;
}

}  // namespace

TEST_CASE("The drawing digest says what each layer carries and what is selected") {
    const std::unique_ptr<Database> db = officeDrawing(3, true);
    const Handle wall = db->entitiesIn(db->modelSpace()).front()->handle;
    const std::string text = describeDrawing(*db, std::vector<Handle>{wall});

    CHECK(has(text, "Units: millimeters"));
    CHECK(has(text, "Current space: model space"));
    CHECK(has(text, "A-WALL: colour 1 (red), Continuous, 3 objects - Walls, plan"));
    CHECK(has(text, "A-DOOR: colour 7, Continuous, 1 object"));
    CHECK(has(text, "DOOR-900: placed 1 time, attributes MARK"));
    CHECK(has(text, "Selected: 1 object"));
    CHECK(has(text, "Line from 0,0 to 500,0 (length 500) on layer A-WALL"));
}

TEST_CASE("Objects are described the way a person would say them") {
    std::unique_ptr<Database> db = Database::createEmpty();
    const Entity& circle =
        db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{10.0, 20.0, 0.0}, 5.0));
    CHECK(describeEntity(*db, circle) == "Circle centre 10,20 radius 5 on layer 0");

    const Entity& text = db->addToModelSpace(
        std::make_unique<TextEntity>(Vec3{1.0, 2.0, 0.0}, std::string{"NORTH"}, 3.5));
    CHECK(describeEntity(*db, text) == "Text \"NORTH\" at 1,2 height 3.5 on layer 0");
}

TEST_CASE("House standards are what the drawings agree on, most used first") {
    DrawingStandards standards;
    CHECK(standards.empty());
    CHECK(standards.describe().empty());

    standards.learnFrom(*officeDrawing(4, true), "plan-a.dwg");
    standards.learnFrom(*officeDrawing(2, false), "plan-b.dwg");
    standards.learnFrom(*officeDrawing(1, true), "plan-c.dwg");

    CHECK(standards.drawingCount == 3);
    CHECK(standards.layers.at("A-WALL").drawings == 3);
    CHECK(standards.layers.at("A-WALL").objects == 7);
    CHECK(standards.blocks.at("DOOR-900").drawings == 2);
    CHECK(standards.blocks.at("DOOR-900").inserts == 2);
    CHECK(standards.blocks.at("DOOR-900").attributeTags == std::vector<std::string>{"MARK"});

    const std::string text = standards.describe();
    CHECK(has(text, "House standards, learned from 3 drawings"));
    CHECK(has(text, "Units: millimeters (3)"));
    CHECK(has(text, "A-WALL: colour 1 (red), Continuous, in 3 drawings, 7 objects (7 Line) - Walls, plan"));
    CHECK(has(text, "A-DOOR: colour 7, Continuous, in 3 drawings, 2 objects (2 Insert)"));
    CHECK(has(text, "DOOR-900: placed 2 times in 2 drawings, attributes MARK"));
    // The busiest layer comes first; the model reads the top of a list most
    // carefully.
    CHECK(text.find("A-WALL:") < text.find("A-DOOR:"));
    // A layer defined in every drawing and never drawn on is still a
    // convention, and stays.
    CHECK(has(text, "UNUSED: colour 7"));
}

TEST_CASE("The standards go into the request after the drawing") {
    class Recorder final : public AgentProvider {
    public:
        [[nodiscard]] std::string name() const override { return "recorder"; }
        [[nodiscard]] AgentReply send(const AgentRequest& request) override {
            seen = request;
            return AgentReply{"```\nLINE 0,0 1,1\n```\n", {}};
        }
        AgentRequest seen;
    };

    const std::unique_ptr<Database> db = officeDrawing(1, false);
    const std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    Recorder recorder;
    std::string error;
    static_cast<void>(requestAgentPlan(recorder, "add a wall", *db, *registry, {}, &error,
                                       "House standards, learned from 2 drawings:\n  A-WALL"));
    CHECK(error.empty());
    CHECK(has(recorder.seen.context, "Model space holds 1 objects"));
    CHECK(has(recorder.seen.context, "House standards, learned from 2 drawings"));
    CHECK(recorder.seen.context.find("Model space") < recorder.seen.context.find("House standards"));
    CHECK(has(recorder.seen.system, "house standards"));
}
