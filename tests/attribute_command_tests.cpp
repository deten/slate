// Attributes: ATTDEF, INSERT asking for values, ATTEDIT, and a reference and
// its attributes behaving as one thing.
#include "test_helpers.hpp"

#include <memory>
#include <string>
#include <tuple>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

struct AttributeHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    /// A block "TAG": a line, and a ROOM attribute whose text sits at (10, 0).
    BlockTableRecord& makeTagBlock() {
        auto block = std::make_unique<BlockTableRecord>();
        block->name = "TAG";
        BlockTableRecord& record = db->addBlock(std::move(block));
        db->addEntity(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{20.0, 0.0, 0.0}), record);
        auto definition = std::make_unique<AttributeDefinitionEntity>();
        definition->tag = "ROOM";
        definition->prompt = "Room name";
        definition->defaultValue = "OFFICE";
        definition->contents = "OFFICE";
        definition->position = {10.0, 0.0, 0.0};
        definition->height = 5.0;
        db->addEntity(std::move(definition), record);
        return record;
    }

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

    /// INSERT the TAG block at `where`, answering the ROOM prompt with `value`.
    InsertEntity& insertTag(const Vec3& where, const std::string& value) {
        REQUIRE(executor.execute("INSERT").status == CommandOutcome::Status::Running);
        executor.supply(InputValue::makeText("TAG"));
        executor.supply(InputValue::makePoint(where));
        executor.supply(InputValue::makeEmpty());
        executor.supply(InputValue::makeEmpty());
        executor.supply(InputValue::makeEmpty());
        const CommandOutcome outcome = executor.supply(InputValue::makeText(value));
        REQUIRE(outcome.status == CommandOutcome::Status::Completed);
        const auto inserts = inModel<InsertEntity>();
        REQUIRE(!inserts.empty());
        return *inserts.back();
    }
};

[[nodiscard]] InputValue at(double x, double y) {
    return InputValue::makePoint(Vec3{x, y, 0.0});
}

}  // namespace

namespace {

[[nodiscard]] std::size_t liveEntitiesIn(Database& db, const BlockTableRecord& block) {
    std::size_t count = 0;
    for (const Entity* entity : db.entitiesIn(block)) {
        if (entity != nullptr && !entity->erased) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] const AttributeEntity* attributeOf(Database& db, const InsertEntity& insert,
                                                 const char* tag) {
    for (const Handle handle : insert.attributeHandles) {
        const auto* attribute = db.get<AttributeEntity>(handle);
        if (attribute != nullptr && !attribute->erased && attribute->tag == tag) {
            return attribute;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("BLOCK asks before redefining a block, and No keeps it as it was") {
    AttributeHarness h;
    h.makeTagBlock();
    const Handle circle =
        h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{0.0, 0.0, 0.0}, 3.0)).handle;
    h.selection.setSelection({circle});
    REQUIRE(h.executor.execute("BLOCK").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(InputValue::makeText("TAG"));
    REQUIRE(h.executor.currentRequest() != nullptr);
    CHECK(h.executor.currentRequest()->kind == InputRequest::Kind::Keyword);
    CHECK(h.executor.supply(InputValue::makeKeyword("N")).status ==
          CommandOutcome::Status::Cancelled);
    CHECK(liveEntitiesIn(*h.db, *h.db->findBlock("TAG")) == 2);
    CHECK(h.inModel<CircleEntity>().size() == 1);
}

TEST_CASE("BLOCK redefined with Yes replaces the contents and updates every reference") {
    AttributeHarness h;
    h.makeTagBlock();
    const Handle first = h.insertTag(Vec3{100.0, 0.0, 0.0}, "KITCHEN").handle;
    const Handle second = h.insertTag(Vec3{200.0, 0.0, 0.0}, "BATH").handle;

    // The revised block: a circle instead of the line, ROOM moved up, and a
    // NUMBER tag that did not exist before.
    const Handle circle =
        h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{0.0, 0.0, 0.0}, 3.0)).handle;
    auto room = std::make_unique<AttributeDefinitionEntity>();
    room->tag = "ROOM";
    room->defaultValue = "OFFICE";
    room->position = {0.0, 10.0, 0.0};
    room->height = 5.0;
    const Handle roomDefinition = h.db->addToModelSpace(std::move(room)).handle;
    auto number = std::make_unique<AttributeDefinitionEntity>();
    number->tag = "NUMBER";
    number->defaultValue = "000";
    number->position = {0.0, 20.0, 0.0};
    number->height = 5.0;
    const Handle numberDefinition = h.db->addToModelSpace(std::move(number)).handle;

    h.selection.setSelection({circle, roomDefinition, numberDefinition});
    REQUIRE(h.executor.execute("BLOCK").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(InputValue::makeText("TAG"));
    CHECK(h.executor.supply(InputValue::makeKeyword("Y")).status ==
          CommandOutcome::Status::Completed);

    const BlockTableRecord* block = h.db->findBlock("TAG");
    REQUIRE(block != nullptr);
    CHECK(liveEntitiesIn(*h.db, *block) == 3);
    for (const Entity* entity : h.db->entitiesIn(*block)) {
        CHECK((entity->erased || dynamic_cast<const LineEntity*>(entity) == nullptr));
    }

    // Both earlier references keep their words, get the new tag at its
    // default, and have every attribute where the new definitions put it.
    for (const auto [handle, value, x] : {std::tuple{first, "KITCHEN", 100.0},
                                          std::tuple{second, "BATH", 200.0}}) {
        const auto* insert = h.db->get<InsertEntity>(handle);
        REQUIRE(insert != nullptr);
        CHECK(insert->attributeHandles.size() == 2);
        const AttributeEntity* roomValue = attributeOf(*h.db, *insert, "ROOM");
        REQUIRE(roomValue != nullptr);
        CHECK(roomValue->contents == value);
        CHECK(roomValue->position.equals(Vec3{x, 10.0, 0.0}, Tolerance{1e-9}));
        const AttributeEntity* numberValue = attributeOf(*h.db, *insert, "NUMBER");
        REQUIRE(numberValue != nullptr);
        CHECK(numberValue->contents == "000");
        CHECK(numberValue->position.equals(Vec3{x, 20.0, 0.0}, Tolerance{1e-9}));
    }
    // And BLOCK left a reference in place of the sources, as it always does.
    CHECK(h.inModel<InsertEntity>().size() == 3);

    // The whole revision undoes as one step.
    h.tm.undo();
    CHECK(liveEntitiesIn(*h.db, *h.db->findBlock("TAG")) == 2);
    CHECK(h.db->get<InsertEntity>(first)->attributeHandles.size() == 1);
}

TEST_CASE("ATTSYNC brings references up to date after their block's definitions change") {
    AttributeHarness h;
    BlockTableRecord& block = h.makeTagBlock();
    const Handle placed = h.insertTag(Vec3{50.0, 50.0, 0.0}, "HALL").handle;

    // The block is revised behind the references' backs: ROOM moves, NUMBER
    // appears.
    for (Entity* entity : h.db->entitiesIn(block)) {
        if (auto* definition = dynamic_cast<AttributeDefinitionEntity*>(entity);
            definition != nullptr && definition->tag == "ROOM") {
            definition->position = {10.0, 30.0, 0.0};
        }
    }
    auto number = std::make_unique<AttributeDefinitionEntity>();
    number->tag = "NUMBER";
    number->defaultValue = "101";
    number->position = {0.0, -10.0, 0.0};
    number->height = 5.0;
    const Handle numberDefinition = h.db->addEntity(std::move(number), block).handle;

    REQUIRE(h.executor.execute("ATTSYNC").status == CommandOutcome::Status::Running);
    CHECK(h.executor.currentRequest()->kind == InputRequest::Kind::Text);
    CHECK(h.executor.supply(InputValue::makeText("TAG")).status ==
          CommandOutcome::Status::Completed);

    const auto* insert = h.db->get<InsertEntity>(placed);
    REQUIRE(insert != nullptr);
    CHECK(insert->attributeHandles.size() == 2);
    const AttributeEntity* room = attributeOf(*h.db, *insert, "ROOM");
    REQUIRE(room != nullptr);
    CHECK(room->contents == "HALL");
    CHECK(room->position.equals(Vec3{60.0, 80.0, 0.0}, Tolerance{1e-9}));
    const AttributeEntity* numberValue = attributeOf(*h.db, *insert, "NUMBER");
    REQUIRE(numberValue != nullptr);
    CHECK(numberValue->contents == "101");
    CHECK(numberValue->position.equals(Vec3{50.0, 40.0, 0.0}, Tolerance{1e-9}));

    // A definition taken away takes its attribute with it, and picking a
    // reference names the block as well as typing does.
    h.db->get<AttributeDefinitionEntity>(numberDefinition)->erased = true;
    REQUIRE(h.executor.execute("ATTSYNC").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("S"));
    CHECK(h.executor.supply(InputValue::makeEntity(placed)).status ==
          CommandOutcome::Status::Completed);
    insert = h.db->get<InsertEntity>(placed);
    CHECK(insert->attributeHandles.size() == 1);
    CHECK(attributeOf(*h.db, *insert, "NUMBER") == nullptr);
    CHECK(attributeOf(*h.db, *insert, "ROOM")->contents == "HALL");
}

TEST_CASE("ATTDEF makes a definition with its tag, prompt, default and modes") {
    AttributeHarness h;
    REQUIRE(h.executor.execute("ATTDEF").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("I"));   // invisible on
    h.executor.supply(InputValue::makeEmpty());        // modes done
    h.executor.supply(InputValue::makeText("room name"));
    h.executor.supply(InputValue::makeText("Room name"));
    h.executor.supply(InputValue::makeText("OFFICE"));
    h.executor.supply(at(3.0, 4.0));
    h.executor.supply(InputValue::makeNumber(5.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeNumber(0.0));
    CHECK(outcome.status == CommandOutcome::Status::Completed);

    const auto definitions = h.inModel<AttributeDefinitionEntity>();
    REQUIRE(definitions.size() == 1);
    const AttributeDefinitionEntity& definition = *definitions.front();
    CHECK(definition.tag == "ROOM_NAME");
    CHECK(definition.prompt == "Room name");
    CHECK(definition.defaultValue == "OFFICE");
    CHECK(definition.contents == "OFFICE");
    CHECK(definition.flags.isInvisible);
    CHECK_FALSE(definition.flags.isConstant);
    CHECK(definition.position.x == doctest::Approx(3.0));
    CHECK(definition.height == doctest::Approx(5.0));
}

TEST_CASE("BLOCK leaves a reference that shows the attributes at their defaults") {
    // The reference standing in for the sources used to carry no values, so
    // the tag just made showed nothing until it was inserted again.
    AttributeHarness h;
    const Handle line =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{20.0, 0.0, 0.0})).handle;
    auto definition = std::make_unique<AttributeDefinitionEntity>();
    definition->tag = "ROOM";
    definition->defaultValue = "OFFICE";
    definition->contents = "OFFICE";
    definition->position = {10.0, 5.0, 0.0};
    const Handle tag = h.db->addToModelSpace(std::move(definition)).handle;

    REQUIRE(h.executor.execute("BLOCK").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeSelection({line, tag}));
    h.executor.supply(at(0.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeText("TAG"));
    CHECK(outcome.status == CommandOutcome::Status::Completed);

    const auto inserts = h.inModel<InsertEntity>();
    REQUIRE(inserts.size() == 1);
    REQUIRE(inserts.front()->attributeHandles.size() == 1);
    const auto* value = h.db->get<AttributeEntity>(inserts.front()->attributeHandles.front());
    REQUIRE(value != nullptr);
    CHECK(value->value() == "OFFICE");
    CHECK(value->position.x == doctest::Approx(10.0));
    CHECK(value->position.y == doctest::Approx(5.0));
}

TEST_CASE("INSERT asks for each attribute and places the value with the reference") {
    AttributeHarness h;
    h.makeTagBlock();
    InsertEntity& insert = h.insertTag(Vec3{100.0, 100.0, 0.0}, "STORE");

    REQUIRE(insert.attributeHandles.size() == 1);
    const auto* attribute = h.db->get<AttributeEntity>(insert.attributeHandles.front());
    REQUIRE(attribute != nullptr);
    CHECK(attribute->tag == "ROOM");
    CHECK(attribute->value() == "STORE");
    // Where the definition's text sits once the block is placed.
    CHECK(attribute->position.x == doctest::Approx(110.0));
    CHECK(attribute->position.y == doctest::Approx(100.0));
    CHECK(attribute->height == doctest::Approx(5.0));
    // Beside the reference, in the same space -- not inside the block.
    CHECK(h.inModel<AttributeEntity>().size() == 1);
}

TEST_CASE("INSERT takes the default when Enter answers the prompt") {
    AttributeHarness h;
    h.makeTagBlock();
    REQUIRE(h.executor.execute("INSERT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeText("TAG"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(InputValue::makeEmpty());
    h.executor.supply(InputValue::makeEmpty());
    h.executor.supply(InputValue::makeEmpty());
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    const auto attributes = h.inModel<AttributeEntity>();
    REQUIRE(attributes.size() == 1);
    CHECK(attributes.front()->value() == "OFFICE");
}

TEST_CASE("ATTEDIT changes a value on a picked reference, and undoes") {
    AttributeHarness h;
    h.makeTagBlock();
    InsertEntity& insert = h.insertTag(Vec3{0.0, 0.0, 0.0}, "STORE");

    REQUIRE(h.executor.execute("ATTEDIT").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeText("STORE 2"));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    const auto* attribute = h.db->get<AttributeEntity>(insert.attributeHandles.front());
    REQUIRE(attribute != nullptr);
    CHECK(attribute->value() == "STORE 2");

    REQUIRE(h.tm.undo());
    CHECK(h.db->get<AttributeEntity>(insert.attributeHandles.front())->value() == "STORE");
}

TEST_CASE("moving a reference moves its attributes with it") {
    AttributeHarness h;
    h.makeTagBlock();
    InsertEntity& insert = h.insertTag(Vec3{0.0, 0.0, 0.0}, "STORE");
    const Handle attributeHandle = insert.attributeHandles.front();

    REQUIRE(h.executor.execute("MOVE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeSelection({insert.handle}));
    h.executor.supply(at(0.0, 0.0));
    const CommandOutcome outcome = h.executor.supply(at(5.0, 7.0));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(insert.position.x == doctest::Approx(5.0));
    const auto* attribute = h.db->get<AttributeEntity>(attributeHandle);
    REQUIRE(attribute != nullptr);
    CHECK(attribute->position.x == doctest::Approx(15.0));
    CHECK(attribute->position.y == doctest::Approx(7.0));
}

TEST_CASE("copying a reference copies its attributes and links the copy to them") {
    AttributeHarness h;
    h.makeTagBlock();
    InsertEntity& original = h.insertTag(Vec3{0.0, 0.0, 0.0}, "STORE");
    const Handle originalAttribute = original.attributeHandles.front();

    REQUIRE(h.executor.execute("COPY").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeSelection({original.handle}));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(50.0, 0.0));
    h.executor.supply(InputValue::makeEmpty());

    const auto inserts = h.inModel<InsertEntity>();
    REQUIRE(inserts.size() == 2);
    const InsertEntity& copy = *inserts.back();
    REQUIRE(copy.attributeHandles.size() == 1);
    CHECK(copy.attributeHandles.front() != originalAttribute);
    const auto* copied = h.db->get<AttributeEntity>(copy.attributeHandles.front());
    REQUIRE(copied != nullptr);
    CHECK(copied->value() == "STORE");
    CHECK(copied->position.x == doctest::Approx(60.0));
    CHECK(h.inModel<AttributeEntity>().size() == 2);
}

TEST_CASE("erasing a reference erases its attributes") {
    AttributeHarness h;
    h.makeTagBlock();
    InsertEntity& insert = h.insertTag(Vec3{0.0, 0.0, 0.0}, "STORE");
    REQUIRE(h.executor.execute("ERASE").status == CommandOutcome::Status::Running);
    const CommandOutcome outcome = h.executor.supply(InputValue::makeSelection({insert.handle}));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(h.inModel<InsertEntity>().empty());
    CHECK(h.inModel<AttributeEntity>().empty());
}

TEST_CASE("attributes are written after their reference and closed by SEQEND") {
    AttributeHarness h;
    h.makeTagBlock();
    h.insertTag(Vec3{0.0, 0.0, 0.0}, "STORE");

    std::string text = io::writeDxf(*h.db);
    std::erase(text, '\r');
    const std::size_t insertAt = text.find("AcDbBlockReference");
    const std::size_t attribAt = text.find("\nATTRIB\n");
    const std::size_t seqendAt = text.find("\nSEQEND\n");
    REQUIRE(insertAt != std::string::npos);
    REQUIRE(attribAt != std::string::npos);
    REQUIRE(seqendAt != std::string::npos);
    CHECK(insertAt < attribAt);
    CHECK(attribAt < seqendAt);
    // Once, following the reference -- not again as a record of its own.
    CHECK(text.find("\nATTRIB\n", attribAt + 1) == std::string::npos);

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    InsertEntity* reloaded = nullptr;
    std::size_t attributes = 0;
    for (Entity* entity : back.database->entitiesIn(back.database->modelSpace())) {
        if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
            reloaded = insert;
        }
        if (dynamic_cast<AttributeEntity*>(entity) != nullptr) {
            ++attributes;
        }
    }
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded->attributeHandles.size() == 1);
    CHECK(attributes == 1);
    const auto* value = back.database->get<AttributeEntity>(reloaded->attributeHandles.front());
    REQUIRE(value != nullptr);
    CHECK(value->value() == "STORE");
}
