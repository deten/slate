// Attributes: ATTDEF, INSERT asking for values, ATTEDIT, and a reference and
// its attributes behaving as one thing.
#include "test_helpers.hpp"

#include <memory>
#include <string>

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
