// Clipping references and pictures: the clip on the entity, in the file,
// on the screen, and by command.
#include "test_helpers.hpp"

#include <memory>
#include <string>
#include <vector>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/ClipFilter.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Dictionary.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/SpatialIndex.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registryOwner = createStandardRegistry();
    CommandRegistry& registry = *registryOwner;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};

    /// A block of one square, placed at 100,100 scaled by 2.
    InsertEntity& placeSquare() {
        auto definition = std::make_unique<BlockTableRecord>();
        definition->name = "SQ";
        BlockTableRecord& block = db->addBlock(std::move(definition));
        const Vec3 corners[] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
        for (int i = 0; i < 4; ++i) {
            db->addEntity(std::make_unique<LineEntity>(corners[i], corners[(i + 1) % 4]), block);
        }
        auto insert = std::make_unique<InsertEntity>(block.handle, Vec3{100, 100, 0});
        insert->scale = Vec3{2, 2, 2};
        return static_cast<InsertEntity&>(db->addToModelSpace(std::move(insert)));
    }
};

[[nodiscard]] InputValue at(double x, double y) { return InputValue::makePoint(Vec3{x, y, 0.0}); }

[[nodiscard]] const RawObject* spatialFilterOf(const Database& db, const InsertEntity& insert) {
    const DbObject* xdictionary = db.getObject(insert.extensionDictionaryHandle);
    if (xdictionary == nullptr) {
        return nullptr;
    }
    const DbObject* filters = db.getObject(dictionaryEntry(*xdictionary, "ACAD_FILTER"));
    if (filters == nullptr) {
        return nullptr;
    }
    return db.get<RawObject>(dictionaryEntry(*filters, "SPATIAL"));
}

}  // namespace

TEST_SUITE("Clipping") {

TEST_CASE("a clip on a reference is written as a spatial filter and read back") {
    Harness h;
    InsertEntity& insert = h.placeSquare();
    h.transactions.run("clip", [&](Transaction& transaction) {
        transaction.openForWrite(insert);
        insert.clipBoundary = {{2, 2, 0}, {8, 2, 0}, {8, 8, 0}, {2, 8, 0}};
        insert.clipEnabled = true;
        writeClipFilter(*h.db, insert, transaction);
    });
    const RawObject* filter = spatialFilterOf(*h.db, insert);
    REQUIRE(filter != nullptr);
    CHECK(filter->dxfType == "SPATIAL_FILTER");

    io::DxfWriter writer;
    const std::string text = writer.write(*h.db);
    CHECK(text.find("SPATIAL_FILTER") != std::string::npos);
    CHECK(text.find("ACAD_FILTER") != std::string::npos);

    io::DxfReadResult result = io::readDxf(text);
    REQUIRE(result.database != nullptr);
    const InsertEntity* back = nullptr;
    for (const Entity* entity : result.database->entitiesIn(result.database->modelSpace())) {
        back = dynamic_cast<const InsertEntity*>(entity);
        if (back != nullptr) {
            break;
        }
    }
    REQUIRE(back != nullptr);
    REQUIRE(back->clipBoundary.size() == 4);
    CHECK(back->clipEnabled);
    CHECK(back->clipBoundary[2].x == doctest::Approx(8.0));
    CHECK(back->isClipped());

    // Undo takes the filter and the dictionaries away with the clip.
    CHECK(h.transactions.undo());
    CHECK_FALSE(insert.isClipped());
}

TEST_CASE("a spatial filter from elsewhere is read with its placing matrix") {
    // A reference at 50,50; the filter's points are given in a frame the
    // second matrix moves by 50,50, so the boundary comes back in block
    // coordinates.
    const std::string dxf =
        "0\nSECTION\n2\nBLOCKS\n"
        "0\nBLOCK\n5\n20\n8\n0\n2\nBX\n70\n0\n10\n0\n20\n0\n30\n0\n3\nBX\n"
        "0\nLINE\n5\n21\n8\n0\n10\n0\n20\n0\n30\n0\n11\n10\n21\n10\n31\n0\n"
        "0\nENDBLK\n5\n22\n8\n0\n"
        "0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        "0\nINSERT\n5\n30\n102\n{ACAD_XDICTIONARY\n360\n40\n102\n}\n8\n0\n2\nBX\n"
        "10\n50\n20\n50\n30\n0\n"
        "0\nENDSEC\n"
        "0\nSECTION\n2\nOBJECTS\n"
        "0\nDICTIONARY\n5\n40\n330\n30\n100\nAcDbDictionary\n281\n1\n3\nACAD_FILTER\n350\n41\n"
        "0\nDICTIONARY\n5\n41\n330\n40\n100\nAcDbDictionary\n281\n1\n3\nSPATIAL\n350\n42\n"
        "0\nSPATIAL_FILTER\n5\n42\n330\n41\n100\nAcDbFilter\n100\nAcDbSpatialFilter\n"
        "70\n2\n10\n51\n20\n51\n10\n59\n20\n59\n"
        "210\n0\n220\n0\n230\n1\n11\n0\n21\n0\n31\n0\n71\n1\n72\n0\n73\n0\n"
        "40\n1\n40\n0\n40\n0\n40\n0\n40\n0\n40\n1\n40\n0\n40\n0\n40\n0\n40\n0\n40\n1\n40\n0\n"
        "40\n1\n40\n0\n40\n0\n40\n-50\n40\n0\n40\n1\n40\n0\n40\n-50\n40\n0\n40\n0\n40\n1\n40\n0\n"
        "0\nENDSEC\n0\nEOF\n";
    io::DxfReadResult result = io::readDxf(dxf);
    REQUIRE(result.database != nullptr);
    const InsertEntity* insert = nullptr;
    for (const Entity* entity : result.database->entitiesIn(result.database->modelSpace())) {
        insert = dynamic_cast<const InsertEntity*>(entity);
        if (insert != nullptr) {
            break;
        }
    }
    REQUIRE(insert != nullptr);
    // Two points made a rectangle, placed into block coordinates.
    REQUIRE(insert->clipBoundary.size() == 4);
    CHECK(insert->clipBoundary[0].x == doctest::Approx(1.0));
    CHECK(insert->clipBoundary[0].y == doctest::Approx(1.0));
    CHECK(insert->clipBoundary[2].x == doctest::Approx(9.0));
    CHECK(insert->clipEnabled);
}

TEST_CASE("a clipped reference's items carry the boundary in world coordinates") {
    Harness h;
    InsertEntity& insert = h.placeSquare();
    insert.clipBoundary = {{2, 2, 0}, {8, 2, 0}, {8, 8, 0}, {2, 8, 0}};
    insert.clipEnabled = true;

    render::SpatialIndex index;
    render::SceneBuilder builder{*h.db, index};
    builder.reindex(h.db->modelSpace());
    const render::Viewport viewport{Vec3{100.0, 100.0}, 4.0, 800.0, 800.0};
    const render::Scene scene = builder.build(h.db->modelSpace(), viewport);
    int clipped = 0;
    for (const render::DrawItem& item : scene.items) {
        if (item.clipPolygon.size() == 4) {
            ++clipped;
            // Scaled by 2 and moved to 100,100: 2,2 becomes 104,104.
            CHECK(item.clipPolygon[0].x == doctest::Approx(104.0));
            CHECK(item.clipPolygon[2].x == doctest::Approx(116.0));
        }
    }
    CHECK(clipped == 4);

    insert.clipEnabled = false;
    builder.reindex(h.db->modelSpace());
    const render::Scene unclipped = builder.build(h.db->modelSpace(), viewport);
    for (const render::DrawItem& item : unclipped.items) {
        CHECK(item.clipPolygon.empty());
    }
}

TEST_CASE("XCLIP draws a boundary, turns it off and on, and deletes it") {
    Harness h;
    InsertEntity& insert = h.placeSquare();

    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    h.executor.supply(InputValue::makeKeyword("N"));
    h.executor.supply(InputValue::makeKeyword("R"));
    h.executor.supply(at(104.0, 104.0));
    const CommandOutcome outcome = h.executor.supply(at(116.0, 116.0));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    REQUIRE(insert.clipBoundary.size() == 4);
    // Picked in the world, kept in the block: 104 is 2 in the block.
    CHECK(insert.clipBoundary[0].x == doctest::Approx(2.0));
    CHECK(insert.clipBoundary[2].x == doctest::Approx(8.0));
    CHECK(spatialFilterOf(*h.db, insert) != nullptr);

    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    CHECK(h.executor.supply(InputValue::makeKeyword("OFF")).status == CommandOutcome::Status::Completed);
    CHECK_FALSE(insert.clipEnabled);
    CHECK(insert.clipBoundary.size() == 4);

    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    CHECK(h.executor.supply(InputValue::makeKeyword("ON")).status == CommandOutcome::Status::Completed);
    CHECK(insert.clipEnabled);

    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    CHECK(h.executor.supply(InputValue::makeKeyword("D")).status == CommandOutcome::Status::Completed);
    CHECK(insert.clipBoundary.empty());
    CHECK(spatialFilterOf(*h.db, insert) == nullptr);

    // A polygonal boundary, points until Enter.
    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(insert.handle));
    h.executor.supply(InputValue::makeKeyword("N"));
    h.executor.supply(InputValue::makeKeyword("P"));
    h.executor.supply(at(100.0, 100.0));
    h.executor.supply(at(120.0, 100.0));
    h.executor.supply(at(100.0, 120.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    CHECK(insert.clipBoundary.size() == 3);

    // Not a reference: refused.
    const Handle line =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{}, Vec3{1, 1, 0})).handle;
    REQUIRE(h.executor.execute("XCLIP").status == CommandOutcome::Status::Running);
    CHECK(h.executor.supply(InputValue::makeEntity(line)).status == CommandOutcome::Status::Failed);
}

TEST_CASE("IMAGECLIP keeps the boundary in the image's own pixels") {
    Harness h;
    auto picture = std::make_unique<RasterImageEntity>();
    picture->origin = Vec3{100, 100, 0};
    // Each pixel is 0.5 wide and 0.5 high.
    picture->uVector = Vec3{0.5, 0, 0};
    picture->vVector = Vec3{0, 0.5, 0};
    picture->widthPixels = 400;
    picture->heightPixels = 200;
    RasterImageEntity& image =
        static_cast<RasterImageEntity&>(h.db->addToModelSpace(std::move(picture)));

    REQUIRE(h.executor.execute("IMAGECLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(image.handle));
    h.executor.supply(InputValue::makeKeyword("N"));
    h.executor.supply(InputValue::makeEmpty());  // rectangular, the default
    h.executor.supply(at(110.0, 110.0));
    CHECK(h.executor.supply(at(150.0, 130.0)).status == CommandOutcome::Status::Completed);
    CHECK(image.clippingEnabled);
    CHECK(image.clipBoundaryType == 1);
    REQUIRE(image.clipVertices.size() == 2);
    CHECK(image.clipVertices[0].x == doctest::Approx(20.0));
    CHECK(image.clipVertices[0].y == doctest::Approx(20.0));
    CHECK(image.clipVertices[1].x == doctest::Approx(100.0));
    CHECK(image.clipVertices[1].y == doctest::Approx(60.0));

    REQUIRE(h.executor.execute("IMAGECLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(image.handle));
    CHECK(h.executor.supply(InputValue::makeKeyword("OFF")).status == CommandOutcome::Status::Completed);
    CHECK_FALSE(image.clippingEnabled);

    REQUIRE(h.executor.execute("IMAGECLIP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEntity(image.handle));
    CHECK(h.executor.supply(InputValue::makeKeyword("D")).status == CommandOutcome::Status::Completed);
    CHECK(image.clipVertices.empty());
}

}  // TEST_SUITE
