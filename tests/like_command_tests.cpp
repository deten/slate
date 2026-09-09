// LIKE: draw another one like an example, in its style.
#include "test_helpers.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/command/TypedInput.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
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

    template <typename T>
    [[nodiscard]] std::vector<const T*> all() const {
        std::vector<const T*> out;
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (const auto* typed = dynamic_cast<const T*>(entity); typed != nullptr && !typed->erased) {
                out.push_back(typed);
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

/// An office's supply run: a wide polyline on HSDUCT in cyan with its size
/// written above it in the office's text style.
[[nodiscard]] Handle supplyRun(Harness& h) {
    Layer& layer = h.db->ensureLayer("HSDUCT");
    layer.color = Color::fromIndex(4);
    const Vec3 route[] = {{0, 0, 0}, {600, 0, 0}};
    auto run = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(route));
    run->constantWidth = 24.0;
    run->layerHandle = layer.handle;
    run->color = Color::fromIndex(2);
    const Handle handle = h.db->addToModelSpace(std::move(run)).handle;
    auto tag = std::make_unique<TextEntity>(Vec3{270, 20, 0}, "24x12", 9.0);
    tag->layerHandle = h.db->ensureLayer("HSTEXT").handle;
    tag->widthFactor = 0.8;
    h.db->addToModelSpace(std::move(tag));
    return handle;
}

}  // namespace

TEST_SUITE("LIKE") {

TEST_CASE("a run like a wide run takes its layer, colour, width and tag placing") {
    Harness h;
    const Handle example = supplyRun(h);

    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(example));
    h.executor.supply(InputValue::makeText("12x10"));
    h.executor.supply(at(0.0, 300.0));
    h.executor.supply(at(400.0, 300.0));
    h.executor.supply(at(400.0, 600.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message.find("2 legs, tagged 12x10") != std::string::npos);

    const auto runs = h.all<PolylineEntity>();
    REQUIRE(runs.size() == 2);
    const PolylineEntity& copy = *runs.back();
    CHECK(copy.constantWidth == doctest::Approx(24.0));
    CHECK(copy.vertices.size() == 3);
    CHECK(h.layerOf(copy) == "HSDUCT");
    CHECK(copy.color.index() == 2);

    const auto texts = h.all<TextEntity>();
    REQUIRE(texts.size() == 2);
    const TextEntity& tag = *texts.back();
    CHECK(tag.contents == "12x10");
    CHECK(tag.height == doctest::Approx(9.0));
    CHECK(tag.widthFactor == doctest::Approx(0.8));
    CHECK(h.layerOf(tag) == "HSTEXT");
    // Above the longest leg (y = 300, x from 0 to 400), the same 20 off it
    // as the example's tag, and the same shift along.
    CHECK(tag.position.y == doctest::Approx(320.0));
    CHECK(tag.position.x == doctest::Approx(170.0));
    CHECK(tag.rotation == doctest::Approx(0.0));

    CHECK(h.transactions.undo());
    CHECK(h.all<PolylineEntity>().size() == 1);
}

TEST_CASE("a run like a double-line run is two lines the same distance apart") {
    Harness h;
    Layer& layer = h.db->ensureLayer("M-DUCT-RETN");
    const Vec3 top[] = {{0, 10, 0}, {500, 10, 0}};
    const Vec3 bottom[] = {{0, -10, 0}, {500, -10, 0}};
    auto a = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(top));
    a->layerHandle = layer.handle;
    const Handle example = h.db->addToModelSpace(std::move(a)).handle;
    auto b = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(bottom));
    b->layerHandle = layer.handle;
    h.db->addToModelSpace(std::move(b));

    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(example));
    h.executor.supply(InputValue::makeEmpty());
    h.executor.supply(at(0.0, 200.0));
    h.executor.supply(at(300.0, 200.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);

    const auto runs = h.all<PolylineEntity>();
    REQUIRE(runs.size() == 4);
    std::vector<double> ys;
    for (std::size_t i = 2; i < 4; ++i) {
        CHECK(runs[i]->constantWidth == doctest::Approx(0.0));
        CHECK(h.layerOf(*runs[i]) == "M-DUCT-RETN");
        ys.push_back(runs[i]->vertices.front().position.y);
    }
    CHECK(std::abs(ys[0] - ys[1]) == doctest::Approx(20.0));
    CHECK((ys[0] + ys[1]) / 2.0 == doctest::Approx(200.0));
}

TEST_CASE("a symbol like a letter in a circle keeps the circle and changes the letter") {
    Harness h;
    Layer& layer = h.db->ensureLayer("M-CTRL");
    auto circle = std::make_unique<CircleEntity>(Vec3{100, 100, 0}, 8.0);
    circle->layerHandle = layer.handle;
    const Handle example = h.db->addToModelSpace(std::move(circle)).handle;
    auto letter = std::make_unique<TextEntity>(Vec3{96, 95, 0}, "T", 9.0);
    letter->layerHandle = layer.handle;
    h.db->addToModelSpace(std::move(letter));

    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(example));
    h.executor.supply(InputValue::makeText("H"));
    const CommandOutcome outcome = h.executor.supply(at(500.0, 500.0));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message.find("Placed a H in a circle") != std::string::npos);

    const auto circles = h.all<CircleEntity>();
    REQUIRE(circles.size() == 2);
    CHECK(circles.back()->center.x == doctest::Approx(500.0));
    CHECK(circles.back()->radius == doctest::Approx(8.0));
    CHECK(h.layerOf(*circles.back()) == "M-CTRL");
    const auto texts = h.all<TextEntity>();
    REQUIRE(texts.size() == 2);
    CHECK(texts.back()->contents == "H");
    CHECK(texts.back()->position.x == doctest::Approx(496.0));
    CHECK(texts.back()->position.y == doctest::Approx(495.0));
    CHECK(texts.back()->height == doctest::Approx(9.0));
}

TEST_CASE("a block like a placed block keeps its scale, rotation and attributes") {
    Harness h;
    auto definition = std::make_unique<BlockTableRecord>();
    definition->name = "DIFF";
    BlockTableRecord& block = h.db->addBlock(std::move(definition));
    h.db->addEntity(std::make_unique<CircleEntity>(Vec3{}, 6.0), block);
    auto insert = std::make_unique<InsertEntity>(block.handle, Vec3{50, 50, 0});
    insert->scale = Vec3{2.0, 2.0, 2.0};
    insert->rotation = kPi / 4.0;
    insert->layerHandle = h.db->ensureLayer("M-DIFF").handle;
    InsertEntity& placed = static_cast<InsertEntity&>(h.db->addToModelSpace(std::move(insert)));
    auto attribute = std::make_unique<AttributeEntity>();
    attribute->tag = "CFM";
    attribute->contents = "150";
    attribute->position = Vec3{60, 40, 0};
    placed.attributeHandles.push_back(h.db->addToModelSpace(std::move(attribute)).handle);

    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(placed.handle));
    h.executor.supply(InputValue::makeText("200"));
    CHECK(h.executor.supply(at(300.0, 300.0)).status == CommandOutcome::Status::Completed);

    const auto inserts = h.all<InsertEntity>();
    REQUIRE(inserts.size() == 2);
    const InsertEntity& copy = *inserts.back();
    CHECK(copy.blockHandle == block.handle);
    CHECK(copy.scale.x == doctest::Approx(2.0));
    CHECK(copy.rotation == doctest::Approx(kPi / 4.0));
    CHECK(h.layerOf(copy) == "M-DIFF");
    REQUIRE(copy.attributeHandles.size() == 1);
    const auto* value = h.db->get<AttributeEntity>(copy.attributeHandles.front());
    REQUIRE(value != nullptr);
    CHECK(value->tag == "CFM");
    CHECK(value->contents == "200");
    CHECK(value->position.x == doctest::Approx(310.0));
    CHECK(value->position.y == doctest::Approx(290.0));
}

TEST_CASE("an example named by its handle is accepted as typed input") {
    Harness h;
    const Handle example = supplyRun(h);
    InputRequest request;
    request.kind = InputRequest::Kind::Entity;
    const std::optional<InputValue> typed = parseTypedInput(request, example.toString(), {});
    REQUIRE(typed.has_value());
    CHECK(typed->kind == InputValue::Kind::Entity);
    CHECK(typed->entity == example);
    CHECK_FALSE(parseTypedInput(request, "not a handle", {}).has_value());

    // And a handle that is no object is refused by the command, not run.
    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEntity(Handle{0xFFFFFF}));
    CHECK(outcome.status == CommandOutcome::Status::Failed);
}

TEST_CASE("what LIKE cannot copy is refused with the reason") {
    Harness h;
    auto point = std::make_unique<PointEntity>();
    point->position = Vec3{10, 10, 0};
    const Handle hatchless = h.db->addToModelSpace(std::move(point)).handle;
    REQUIRE(h.executor.execute("LIKE").status == CommandOutcome::Status::Running);
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEntity(hatchless));
    CHECK(outcome.status == CommandOutcome::Status::Failed);
    CHECK(outcome.message.find("not a Point") != std::string::npos);
}

}  // TEST_SUITE
