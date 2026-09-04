#include "test_helpers.hpp"

#include <algorithm>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/db/Transfer.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

/// A layer with a recognisable colour, so a paste that lands on the wrong one
/// is visible in the assertion rather than merely in the handle.
Layer& makeLayer(Database& db, const std::string& name, int colorIndex) {
    auto layer = std::make_unique<Layer>();
    layer->name = name;
    layer->color = Color::fromIndex(colorIndex);
    layer->linetypeHandle = db.linetypes().find("Continuous");
    return db.addLayer(std::move(layer));
}

LineEntity& makeLine(Database& db, const Vec3& from, const Vec3& to, Handle layer) {
    auto line = std::make_unique<LineEntity>();
    line->start = from;
    line->end = to;
    line->layerHandle = layer;
    return static_cast<LineEntity&>(db.addToModelSpace(std::move(line)));
}

/// Every live entity in the destination's model space, for counting.
std::vector<Entity*> modelEntities(Database& db) {
    std::vector<Entity*> live;
    for (Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (entity != nullptr && !entity->erased) {
            live.push_back(entity);
        }
    }
    return live;
}

/// Paste content into a database's model space, through a real transaction, so
/// the tests exercise the same path the PASTECLIP command does.
TransferReport pasteInto(Database& target, const Database& content, const Vec3& offset) {
    TransactionManager transactions{target};
    return transactions.run("Paste", [&](Transaction& transaction) {
        return pasteContent(content, target.modelSpace(), Mat4::translation(offset),
                            transaction);
    });
}

}  // namespace

TEST_CASE("extracting entities produces a database that stands on its own") {
    auto source = Database::createEmpty();
    const Handle walls = makeLayer(*source, "Walls", 3).handle;
    const Handle line = makeLine(*source, Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 5.0, 0.0}, walls).handle;

    const ExtractedContent content = extractEntities(*source, {line});

    REQUIRE(content.database != nullptr);
    CHECK(content.report.entities.size() == 1);

    // The copy names a layer in its *own* database, not the source's.
    const Entity* copy = content.database->getEntity(content.report.entities.front());
    REQUIRE(copy != nullptr);
    const auto* copiedLayer = content.database->get<Layer>(copy->layerHandle);
    REQUIRE(copiedLayer != nullptr);
    CHECK(copiedLayer->name == "Walls");
    CHECK(copiedLayer->color.index() == 3);

    // Base point is the lower-left of what came across, which is what a paste
    // measures its insertion point from.
    CHECK_POINT(content.basePoint, (Vec3{0.0, 0.0, 0.0}));
}

TEST_CASE("extraction skips handles that name nothing and erased entities") {
    auto source = Database::createEmpty();
    const Handle live = makeLine(*source, Vec3{}, Vec3{1.0, 0.0, 0.0},
                                 source->defaultLayer().handle)
                            .handle;
    const Handle gone = makeLine(*source, Vec3{}, Vec3{2.0, 0.0, 0.0},
                                 source->defaultLayer().handle)
                            .handle;
    source->eraseObject(gone);

    const ExtractedContent content =
        extractEntities(*source, {live, gone, Handle{0xDEAD}, Handle::null()});

    CHECK(content.report.entities.size() == 1);
}

TEST_CASE("a copied entity keeps its layer's appearance when the target lacks it") {
    auto source = Database::createEmpty();
    const Handle walls = makeLayer(*source, "Walls", 3).handle;
    const Handle line = makeLine(*source, Vec3{}, Vec3{10.0, 0.0, 0.0}, walls).handle;

    const ExtractedContent content = extractEntities(*source, {line});

    auto target = Database::createEmpty();
    REQUIRE(target->findLayer("Walls") == nullptr);

    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    REQUIRE(report.entities.size() == 1);
    CHECK(report.layersAdded == std::vector<std::string>{"Walls"});

    const Layer* pasted = target->findLayer("Walls");
    REQUIRE(pasted != nullptr);
    CHECK(pasted->color.index() == 3);
    CHECK(target->getEntity(report.entities.front())->layerHandle == pasted->handle);
}

TEST_CASE("pasting onto a drawing that already has the layer uses it, unchanged") {
    auto source = Database::createEmpty();
    const Handle sourceWalls = makeLayer(*source, "Walls", 3).handle;
    const Handle line = makeLine(*source, Vec3{}, Vec3{10.0, 0.0, 0.0}, sourceWalls).handle;
    const ExtractedContent content = extractEntities(*source, {line});

    auto target = Database::createEmpty();
    // Same name, deliberately different colour. The host drawing's standard
    // wins: a paste must not restyle the sheet it lands on.
    const Handle targetWalls = makeLayer(*target, "WALLS", 1).handle;

    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    REQUIRE(report.entities.size() == 1);
    CHECK(report.layersAdded.empty());
    CHECK(target->getEntity(report.entities.front())->layerHandle == targetWalls);
    CHECK(target->get<Layer>(targetWalls)->color.index() == 1);
    CHECK(target->layers().size() == 2);  // "0" and "Walls", not three.
}

TEST_CASE("paste places geometry at the offset it is given") {
    auto source = Database::createEmpty();
    const Handle line =
        makeLine(*source, Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}, source->defaultLayer().handle)
            .handle;
    const ExtractedContent content = extractEntities(*source, {line});

    auto target = Database::createEmpty();
    const TransferReport report = pasteInto(*target, *content.database, Vec3{100.0, 50.0, 0.0});

    REQUIRE(report.entities.size() == 1);
    const auto* pasted = target->get<LineEntity>(report.entities.front());
    REQUIRE(pasted != nullptr);
    CHECK_POINT(pasted->start, (Vec3{100.0, 50.0, 0.0}));
    CHECK_POINT(pasted->end, (Vec3{110.0, 50.0, 0.0}));
}

TEST_CASE("a paste is one undo step, layers and all") {
    auto source = Database::createEmpty();
    const Handle walls = makeLayer(*source, "Walls", 3).handle;
    makeLine(*source, Vec3{}, Vec3{1.0, 0.0, 0.0}, walls);
    makeLine(*source, Vec3{}, Vec3{2.0, 0.0, 0.0}, walls);
    std::vector<Handle> all;
    for (const Entity* entity : source->entitiesIn(source->modelSpace())) {
        all.push_back(entity->handle);
    }
    const ExtractedContent content = extractEntities(*source, all);

    auto target = Database::createEmpty();
    TransactionManager transactions{*target};
    transactions.run("Paste", [&](Transaction& transaction) {
        pasteContent(*content.database, target->modelSpace(), Mat4::translation(Vec3{}),
                     transaction);
    });

    CHECK(modelEntities(*target).size() == 2);
    REQUIRE(target->findLayer("Walls") != nullptr);
    CHECK(transactions.undoDepth() == 1);

    REQUIRE(transactions.undo());
    CHECK(modelEntities(*target).empty());
    // The layer the paste had to invent goes away with it. Leaving it behind
    // would make undo leave litter in the layer list.
    CHECK(target->findLayer("Walls") == nullptr);
}

TEST_CASE("a block reference brings its definition with it") {
    auto source = Database::createEmpty();
    auto definition = std::make_unique<BlockTableRecord>();
    definition->name = "DOOR";
    BlockTableRecord& door = source->addBlock(std::move(definition));

    auto leaf = std::make_unique<LineEntity>();
    leaf->start = Vec3{0.0, 0.0, 0.0};
    leaf->end = Vec3{0.0, 900.0, 0.0};
    leaf->layerHandle = source->defaultLayer().handle;
    source->addEntity(std::move(leaf), door);

    auto insert = std::make_unique<InsertEntity>(door.handle, Vec3{100.0, 0.0, 0.0});
    insert->layerHandle = source->defaultLayer().handle;
    const Handle reference = source->addToModelSpace(std::move(insert)).handle;

    const ExtractedContent content = extractEntities(*source, {reference});

    auto target = Database::createEmpty();
    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    REQUIRE(report.entities.size() == 1);
    CHECK(report.blocksAdded == std::vector<std::string>{"DOOR"});

    const BlockTableRecord* pastedDefinition = target->findBlock("DOOR");
    REQUIRE(pastedDefinition != nullptr);
    CHECK(target->entitiesIn(*pastedDefinition).size() == 1);

    const auto* pastedInsert = target->get<InsertEntity>(report.entities.front());
    REQUIRE(pastedInsert != nullptr);
    CHECK(pastedInsert->blockHandle == pastedDefinition->handle);
}

TEST_CASE("a block name the target already uses keeps the target's definition") {
    auto source = Database::createEmpty();
    auto sourceDefinition = std::make_unique<BlockTableRecord>();
    sourceDefinition->name = "TB";
    BlockTableRecord& sourceBlock = source->addBlock(std::move(sourceDefinition));
    auto line = std::make_unique<LineEntity>();
    line->end = Vec3{10.0, 0.0, 0.0};
    line->layerHandle = source->defaultLayer().handle;
    source->addEntity(std::move(line), sourceBlock);

    auto insert = std::make_unique<InsertEntity>(sourceBlock.handle, Vec3{});
    insert->layerHandle = source->defaultLayer().handle;
    const ExtractedContent content =
        extractEntities(*source, {source->addToModelSpace(std::move(insert)).handle});

    auto target = Database::createEmpty();
    auto targetDefinition = std::make_unique<BlockTableRecord>();
    targetDefinition->name = "TB";
    const Handle existing = target->addBlock(std::move(targetDefinition)).handle;
    const std::size_t blocksBefore = target->blocks().size();

    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    CHECK(report.blocksAdded.empty());
    CHECK(report.blocksReused == std::vector<std::string>{"TB"});
    // No second definition under a made-up name.
    CHECK(target->blocks().size() == blocksBefore);
    CHECK(target->get<InsertEntity>(report.entities.front())->blockHandle == existing);
    // The host's definition is untouched: still empty, not redefined by the
    // paste. Redefining it would change every other TB on the sheet.
    CHECK(target->entitiesIn(*target->get<BlockTableRecord>(existing)).empty());
}

TEST_CASE("an attribute comes across with the block reference that owns it") {
    auto source = Database::createEmpty();
    auto definition = std::make_unique<BlockTableRecord>();
    definition->name = "DOORTAG";
    BlockTableRecord& tag = source->addBlock(std::move(definition));

    auto insert = std::make_unique<InsertEntity>(tag.handle, Vec3{});
    insert->layerHandle = source->defaultLayer().handle;
    InsertEntity& reference = static_cast<InsertEntity&>(source->addToModelSpace(std::move(insert)));

    auto attribute = std::make_unique<AttributeEntity>();
    attribute->tag = "NUMBER";
    attribute->contents = "104A";
    attribute->layerHandle = source->defaultLayer().handle;
    const Handle attributeHandle = source->addToModelSpace(std::move(attribute)).handle;
    reference.attributeHandles.push_back(attributeHandle);

    // Only the INSERT is selected, which is how a drafter selects a tagged
    // block: the attribute is not separately pickable.
    const ExtractedContent content = extractEntities(*source, {reference.handle});

    CHECK(content.report.entities.size() == 2);

    auto target = Database::createEmpty();
    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    const auto* pasted = target->get<InsertEntity>(report.entities.front());
    REQUIRE(pasted != nullptr);
    REQUIRE(pasted->attributeHandles.size() == 1);
    const auto* value = target->get<AttributeEntity>(pasted->attributeHandles.front());
    REQUIRE(value != nullptr);
    CHECK(value->value() == "104A");
}

TEST_CASE("a hatch loses an association to a boundary that stayed behind") {
    auto source = Database::createEmpty();
    const Handle boundary =
        makeLine(*source, Vec3{}, Vec3{10.0, 0.0, 0.0}, source->defaultLayer().handle).handle;

    auto hatch = std::make_unique<HatchEntity>();
    hatch->layerHandle = source->defaultLayer().handle;
    hatch->isAssociative = true;
    HatchBoundaryPath path;
    path.sourceHandles.push_back(boundary);
    hatch->paths.push_back(std::move(path));
    const Handle fill = source->addToModelSpace(std::move(hatch)).handle;

    // The hatch alone: the line it was drawn against is not selected.
    const ExtractedContent content = extractEntities(*source, {fill});

    REQUIRE(content.report.entities.size() == 1);
    CHECK(content.report.droppedReferences == 1);

    const auto* copy = content.database->get<HatchEntity>(content.report.entities.front());
    REQUIRE(copy != nullptr);
    REQUIRE(copy->paths.size() == 1);
    // The shape survives; only the link to geometry that did not come does not.
    CHECK(copy->paths.front().sourceHandles.empty());
}

TEST_CASE("a hatch keeps an association to a boundary copied with it") {
    auto source = Database::createEmpty();
    const Handle boundary =
        makeLine(*source, Vec3{}, Vec3{10.0, 0.0, 0.0}, source->defaultLayer().handle).handle;

    auto hatch = std::make_unique<HatchEntity>();
    hatch->layerHandle = source->defaultLayer().handle;
    hatch->isAssociative = true;
    HatchBoundaryPath path;
    path.sourceHandles.push_back(boundary);
    hatch->paths.push_back(std::move(path));
    const Handle fill = source->addToModelSpace(std::move(hatch)).handle;

    const ExtractedContent content = extractEntities(*source, {fill, boundary});

    CHECK(content.report.droppedReferences == 0);
    const auto* copy = content.database->get<HatchEntity>(content.report.entities.front());
    REQUIRE(copy != nullptr);
    REQUIRE(copy->paths.size() == 1);
    REQUIRE(copy->paths.front().sourceHandles.size() == 1);

    // Points at the *copy* of the boundary, not at the source drawing's handle.
    const Handle linked = copy->paths.front().sourceHandles.front();
    CHECK(content.database->get<LineEntity>(linked) != nullptr);
    CHECK(linked != boundary);
}

TEST_CASE("text brings its style, and a dimension its dimension style") {
    auto source = Database::createEmpty();
    auto style = std::make_unique<TextStyle>();
    style->name = "ROMANS";
    style->height = 2.5;
    const Handle romans = source->addTextStyle(std::move(style)).handle;

    auto text = std::make_unique<TextEntity>();
    text->contents = "NOTE";
    text->styleHandle = romans;
    text->layerHandle = source->defaultLayer().handle;
    const Handle label = source->addToModelSpace(std::move(text)).handle;

    auto dimStyle = std::make_unique<DimStyle>();
    dimStyle->name = "ARCH";
    dimStyle->textStyleHandle = romans;
    const Handle arch = source->addDimStyle(std::move(dimStyle)).handle;

    auto dimension = std::make_unique<DimensionEntity>();
    dimension->type = DimensionType::Aligned;
    dimension->styleHandle = arch;
    dimension->layerHandle = source->defaultLayer().handle;
    dimension->point13 = Vec3{0.0, 0.0, 0.0};
    dimension->point14 = Vec3{10.0, 0.0, 0.0};
    dimension->blockHandle = Handle{0x1234};  // The source file's drawn block.
    const Handle dim = source->addToModelSpace(std::move(dimension)).handle;

    const ExtractedContent content = extractEntities(*source, {label, dim});

    auto target = Database::createEmpty();
    const TransferReport report = pasteInto(*target, *content.database, Vec3{});

    REQUIRE(report.entities.size() == 2);
    const TextStyle* pastedStyle = target->findTextStyle("ROMANS");
    REQUIRE(pastedStyle != nullptr);
    CHECK(pastedStyle->height == doctest::Approx(2.5));

    const auto* pastedText = target->get<TextEntity>(report.entities[0]);
    REQUIRE(pastedText != nullptr);
    CHECK(pastedText->styleHandle == pastedStyle->handle);

    const DimStyle* pastedDimStyle = target->findDimStyle("ARCH");
    REQUIRE(pastedDimStyle != nullptr);
    CHECK(pastedDimStyle->textStyleHandle == pastedStyle->handle);

    const auto* pastedDimension = target->get<DimensionEntity>(report.entities[1]);
    REQUIRE(pastedDimension != nullptr);
    CHECK(pastedDimension->styleHandle == pastedDimStyle->handle);
    // The drawn block belongs to the source file's handle space and is
    // regenerated wherever the dimension lands, so it must not follow.
    CHECK(pastedDimension->blockHandle.isNull());
}

TEST_CASE("extracted content survives a trip through DXF") {
    // The clipboard carries DXF text, so what extraction produces has to be
    // writable and readable — otherwise copy and paste work in the tests and
    // fail between two running copies of the application.
    auto source = Database::createEmpty();
    const Handle walls = makeLayer(*source, "Walls", 3).handle;
    const Handle line = makeLine(*source, Vec3{1.0, 2.0, 0.0}, Vec3{11.0, 2.0, 0.0}, walls).handle;

    const ExtractedContent content = extractEntities(*source, {line});
    const std::string text = io::writeDxf(*content.database);
    const io::DxfReadResult reloaded = io::readDxf(text);

    REQUIRE(reloaded.database != nullptr);
    REQUIRE(reloaded.succeeded());

    auto target = Database::createEmpty();
    const TransferReport report = pasteInto(*target, *reloaded.database, Vec3{0.0, 5.0, 0.0});

    REQUIRE(report.entities.size() == 1);
    const auto* pasted = target->get<LineEntity>(report.entities.front());
    REQUIRE(pasted != nullptr);
    CHECK_POINT(pasted->start, (Vec3{1.0, 7.0, 0.0}));

    const Layer* pastedLayer = target->findLayer("Walls");
    REQUIRE(pastedLayer != nullptr);
    CHECK(pastedLayer->color.index() == 3);
}

TEST_CASE("pasting nothing changes nothing") {
    auto source = Database::createEmpty();
    const ExtractedContent content = extractEntities(*source, {});
    CHECK(content.isEmpty());

    auto target = Database::createEmpty();
    const std::size_t before = target->objectCount();
    const TransferReport report = pasteInto(*target, *content.database, Vec3{});
    CHECK(report.entities.empty());
    CHECK(target->objectCount() == before);
}
