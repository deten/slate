/// Tests for the entities whose content lives in another file: raster images,
/// PDF and DGN underlays, and embedded OLE objects.
///
/// The values here come from the sample drawing's own IMAGE and OLE2FRAME
/// records, because the point of these entities is agreeing with what other CAD software
/// put in the file. Where a number is invented it is because the sample has no
/// case for it — it has no underlay placement, only an orphaned definition.
#include "test_helpers.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/OleFrameEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/UnderlayEntity.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/io/XrefLoader.hpp"
#include "cad/render/Scene.hpp"

using namespace cad;
using namespace cad::io;

namespace {

struct Pair {
    int code;
    std::string value;

    Pair(int c, std::string v) : code(c), value(std::move(v)) {}
    Pair(int c, const char* v) : code(c), value(v) {}
    Pair(int c, double v) : code(c), value(std::to_string(v)) {}
    Pair(int c, int v) : code(c), value(std::to_string(v)) {}
};

[[nodiscard]] std::string dxf(std::initializer_list<Pair> pairs) {
    std::string out;
    for (const Pair& pair : pairs) {
        out += std::to_string(pair.code);
        out += "\r\n";
        out += pair.value;
        out += "\r\n";
    }
    return out;
}

/// The sample drawing's IMAGE, in a drawing small enough to reason about.
///
/// 7177 x 4833 pixels at 0.005 units per pixel, unrotated, unclipped, with the
/// clip boundary other CAD software writes for an unclipped image: the two corners half a
/// pixel outside the outermost pixel centres.
[[nodiscard]] std::string drawingWithImage() {
    return dxf({
        {0, "SECTION"}, {2, "ENTITIES"},
        {0, "IMAGE"}, {5, "DD77"}, {330, "1F"},
        {100, "AcDbEntity"}, {8, "PDF5_Images"},
        {100, "AcDbRasterImage"}, {90, 0},
        {10, "0.0"}, {20, "0.0"}, {30, "0.0"},
        {11, "0.005000541854380505"}, {21, "0.0"}, {31, "0.0"},
        {12, "0.0"}, {22, "0.005000344851369059"}, {32, "0.0"},
        {13, "7177.0"}, {23, "4833.0"},
        {340, "DD70"}, {70, 15}, {280, 0}, {281, 50}, {282, 50}, {283, 0},
        {360, "DD78"}, {71, 1}, {91, 2},
        {14, "-0.5"}, {24, "-0.5"}, {14, "7176.5"}, {24, "4832.5"}, {290, 0},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "OBJECTS"},
        {0, "IMAGEDEF"}, {5, "DD70"}, {330, "DD6F"},
        {100, "AcDbRasterImageDef"}, {90, 0},
        {1, "scans\\site-survey.png"},
        {10, "7177.0"}, {20, "4833.0"},
        {11, "0.0001393339835585899"}, {21, "0.0001393339835585899"},
        {280, 1}, {281, 0},
        {0, "ENDSEC"},
    });
}

/// The same image, but sitting in a block that nothing inserts.
///
/// Taken from a real drawing: a PDF page had been imported into a block called
/// `Binder2` and the INSERT later erased, leaving the definition behind. The
/// file is still attached, the IMAGE entity still exists, and nothing is drawn
/// anywhere -- which is exactly the state that makes repairing the path look
/// like it failed.
[[nodiscard]] std::string drawingWithImageInAnUninsertedBlock() {
    return dxf({
        {0, "SECTION"}, {2, "BLOCKS"},
        {0, "BLOCK"}, {5, "B100"}, {2, "Binder2"}, {70, 0}, {10, "0"}, {20, "0"}, {3, "Binder2"},
        {0, "IMAGE"}, {5, "DD77"}, {330, "B100"},
        {100, "AcDbEntity"}, {8, "PDF5_Images"},
        {100, "AcDbRasterImage"}, {90, 0},
        {10, "0.0"}, {20, "0.0"}, {30, "0.0"},
        {11, "0.005"}, {21, "0.0"}, {31, "0.0"},
        {12, "0.0"}, {22, "0.005"}, {32, "0.0"},
        {13, "7177.0"}, {23, "4833.0"},
        {340, "DD70"}, {70, 15}, {280, 0}, {281, 50}, {282, 50}, {283, 0},
        {0, "ENDBLK"}, {5, "B101"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "OBJECTS"},
        {0, "IMAGEDEF"}, {5, "DD70"}, {330, "DD6F"},
        {100, "AcDbRasterImageDef"}, {90, 0},
        {1, R"(scans\site-survey.png)"},
        {10, "7177.0"}, {20, "4833.0"},
        {280, 1}, {281, 0},
        {0, "ENDSEC"},
    });
}

[[nodiscard]] RasterImageEntity* firstImage(Database& db) {
    for (Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (auto* image = dynamic_cast<RasterImageEntity*>(entity)) {
            return image;
        }
    }
    return nullptr;
}

}  // namespace

TEST_SUITE("Raster images") {

TEST_CASE("an IMAGE is read as a placement, not a placeholder") {
    const DxfReadResult result = readDxf(drawingWithImage());
    REQUIRE(result.database != nullptr);

    RasterImageEntity* image = firstImage(*result.database);
    REQUIRE(image != nullptr);

    CHECK(image->widthPixels == doctest::Approx(7177.0));
    CHECK(image->heightPixels == doctest::Approx(4833.0));
    CHECK(image->uVector.x == doctest::Approx(0.005000541854380505));
    CHECK(image->vVector.y == doctest::Approx(0.005000344851369059));
    CHECK(image->definitionHandle == Handle{0xDD70});
    CHECK(image->reactorHandle == Handle{0xDD78});

    // Flags 15 is all four bits: shown, shown unaligned, clipping honoured,
    // transparent.
    CHECK(image->showImage);
    CHECK(image->showWhenNotAligned);
    CHECK(image->useClippingBoundary);
    CHECK(image->transparent);
    CHECK_FALSE(image->clippingEnabled);
    CHECK(image->brightness == 50);
    CHECK(image->contrast == 50);
    CHECK(image->fade == 0);
    CHECK(image->clipVertices.size() == 2);
}

TEST_CASE("the definition carries the path the file stores, unchanged") {
    const DxfReadResult result = readDxf(drawingWithImage());
    const auto* definition = result.database->get<ImageDefinition>(Handle{0xDD70});
    REQUIRE(definition != nullptr);

    // Backslashes and all. Rewriting a stored path on read is how a project
    // folder stops being portable for everybody else who opens it.
    CHECK(definition->fileName == "scans\\site-survey.png");
    CHECK(definition->sizeInPixels.x == doctest::Approx(7177.0));
    CHECK(definition->loaded);
}

TEST_CASE("the corners follow the pixel vectors, not an assumed rectangle") {
    RasterImageEntity image;
    image.origin = Vec3{10.0, 20.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 50.0;
    // A 30-degree rotation expressed the way DXF expresses it: in the vectors.
    image.uVector = Vec3{0.86602540378, 0.5, 0.0} * 0.1;
    image.vVector = Vec3{-0.5, 0.86602540378, 0.0} * 0.1;

    const std::vector<Vec3> corners = image.corners();
    REQUIRE(corners.size() == 4);
    CHECK_POINT(corners[0], image.origin);
    CHECK_POINT(corners[1], (Vec3{10.0 + 8.6602540378, 20.0 + 5.0, 0.0}));
    CHECK_POINT(corners[3], (Vec3{10.0 - 2.5, 20.0 + 4.3301270189, 0.0}));

    // Opposite corners, so the parallelogram closes.
    CHECK_POINT(corners[2], corners[1] + (corners[3] - corners[0]));
}

TEST_CASE("a transform moves the origin and rotates the pixel vectors") {
    RasterImageEntity image;
    image.origin = Vec3{1.0, 2.0, 0.0};
    image.uVector = Vec3{0.5, 0.0, 0.0};
    image.vVector = Vec3{0.0, 0.5, 0.0};
    image.widthPixels = 4.0;
    image.heightPixels = 4.0;

    image.transformBy(Mat4::translation(Vec3{10.0, 0.0, 0.0}));

    CHECK_POINT(image.origin, (Vec3{11.0, 2.0, 0.0}));
    // The vectors are directions: a translation must not touch them. Running
    // them through a point transform would drag the far corner onto the origin.
    CHECK_POINT(image.uVector, (Vec3{0.5, 0.0, 0.0}));
    CHECK_POINT(image.vVector, (Vec3{0.0, 0.5, 0.0}));
}

TEST_CASE("an unclipped image is bounded by its frame") {
    const DxfReadResult result = readDxf(drawingWithImage());
    const RasterImageEntity* image = firstImage(*result.database);
    REQUIRE(image != nullptr);

    const Box3 bounds = image->bounds();
    CHECK(bounds.min.x == doctest::Approx(0.0));
    CHECK(bounds.min.y == doctest::Approx(0.0));
    CHECK(bounds.max.x == doctest::Approx(7177.0 * 0.005000541854380505));
    CHECK(bounds.max.y == doctest::Approx(4833.0 * 0.005000344851369059));
}

TEST_CASE("a clipped image is bounded by its clip, not its frame") {
    RasterImageEntity image;
    image.origin = Vec3::zero();
    image.uVector = Vec3{1.0, 0.0, 0.0};
    image.vVector = Vec3{0.0, 1.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 100.0;
    image.clippingEnabled = true;
    image.clipBoundaryType = 1;
    image.clipVertices = {Vec3{9.5, 19.5, 0.0}, Vec3{39.5, 49.5, 0.0}};

    // Pixel coordinates are offset half a pixel, so 9.5 is the left edge of
    // pixel 10 and lands at 10.0 in world units.
    const Box3 bounds = image.bounds();
    CHECK(bounds.min.x == doctest::Approx(10.0));
    CHECK(bounds.min.y == doctest::Approx(20.0));
    CHECK(bounds.max.x == doctest::Approx(40.0));
    CHECK(bounds.max.y == doctest::Approx(50.0));

    // A rectangular boundary is two corners in the file and four everywhere
    // else, so nothing downstream has to know which form it was stored in.
    CHECK(image.worldClipBoundary().size() == 4);
}

TEST_CASE("clipping that is stored but switched off does not bind") {
    RasterImageEntity image;
    image.uVector = Vec3{1.0, 0.0, 0.0};
    image.vVector = Vec3{0.0, 1.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 100.0;
    image.clipBoundaryType = 1;
    image.clipVertices = {Vec3{9.5, 19.5, 0.0}, Vec3{39.5, 49.5, 0.0}};
    image.clippingEnabled = false;

    CHECK(image.worldClipBoundary().empty());
    CHECK(image.bounds().max.x == doctest::Approx(100.0));
}

TEST_CASE("an image is picked by its area, like a hatch") {
    RasterImageEntity image;
    image.uVector = Vec3{1.0, 0.0, 0.0};
    image.vVector = Vec3{0.0, 1.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 100.0;

    // Clicking the middle of a photograph must select the photograph.
    CHECK(image.distanceToPoint(Vec3{50.0, 50.0, 0.0}) == doctest::Approx(0.0));
    CHECK(image.distanceToPoint(Vec3{110.0, 50.0, 0.0}) == doctest::Approx(10.0));
}

TEST_CASE("dragging a corner scales about the opposite one") {
    RasterImageEntity image;
    image.origin = Vec3::zero();
    image.uVector = Vec3{1.0, 0.0, 0.0};
    image.vVector = Vec3{0.0, 1.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 100.0;

    const std::vector<GripPoint> grips = image.gripPoints();
    REQUIRE(grips.size() == 4);

    // Grip 2 is the far corner; the origin is the anchor and must not move.
    image.moveGrip(grips[2], Vec3{200.0, 50.0, 0.0});
    CHECK_POINT(image.origin, Vec3::zero());
    CHECK(image.uVector.x == doctest::Approx(2.0));
    CHECK(image.vVector.y == doctest::Approx(0.5));

    // Grip 0 is the origin itself; now the *far* corner is the anchor.
    const Vec3 far = image.corners()[2];
    image.moveGrip(image.gripPoints()[0], Vec3{-100.0, -100.0, 0.0});
    CHECK_POINT(image.corners()[2], far);
}

TEST_CASE("a degenerate drag is refused rather than flattening the image") {
    RasterImageEntity image;
    image.uVector = Vec3{1.0, 0.0, 0.0};
    image.vVector = Vec3{0.0, 1.0, 0.0};
    image.widthPixels = 100.0;
    image.heightPixels = 100.0;

    // Dropping a corner onto its anchor would give the image no area at all,
    // which is a state a user cannot select their way back out of.
    image.moveGrip(image.gripPoints()[2], Vec3::zero());
    CHECK(image.uVector.x == doctest::Approx(1.0));
    CHECK(image.vVector.y == doctest::Approx(1.0));
}

namespace {

/// How many lines of a DXF text are exactly `value`, whatever the line ending.
[[nodiscard]] std::size_t linesEqual(const std::string& text, std::string_view value) {
    std::size_t total = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string_view line{text.data() + start, end - start};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line == value) {
            ++total;
        }
        start = end + 1;
    }
    return total;
}

}  // namespace

TEST_CASE("definitions made here are written with their dictionaries and read back") {
    std::unique_ptr<Database> db = Database::createEmpty();

    auto definition = std::make_unique<ImageDefinition>();
    definition->fileName = "scans/site.png";
    definition->sizeInPixels = Vec3{800.0, 600.0, 0.0};
    definition->pixelSize = Vec3{1.0, 1.0, 0.0};
    const Handle imageDefinition = db->addObject(std::move(definition)).handle;
    auto image = std::make_unique<RasterImageEntity>();
    image->definitionHandle = imageDefinition;
    image->origin = Vec3{10.0, 20.0, 0.0};
    image->uVector = Vec3{0.5, 0.0, 0.0};
    image->vVector = Vec3{0.0, 0.5, 0.0};
    image->widthPixels = 800.0;
    image->heightPixels = 600.0;
    db->addToModelSpace(std::move(image));

    auto pdf = std::make_unique<UnderlayDefinition>();
    pdf->kind = UnderlayKind::Pdf;
    pdf->fileName = "plans/sketch.pdf";
    pdf->itemName = "3";
    const Handle pdfDefinition = db->addObject(std::move(pdf)).handle;
    auto underlay = std::make_unique<UnderlayEntity>();
    underlay->definitionHandle = pdfDefinition;
    underlay->pageSize = Vec3{210.0, 297.0, 0.0};
    db->addToModelSpace(std::move(underlay));

    DxfWriter writer;
    const std::string text = writer.write(*db);
    // The named object dictionary names both, and each definition is written
    // from its fields under its own subclass.
    CHECK(text.find("ACAD_IMAGE_DICT") != std::string::npos);
    CHECK(text.find("ACAD_PDFDEFINITIONS") != std::string::npos);
    CHECK(text.find("AcDbRasterImageDef") != std::string::npos);
    CHECK(text.find("AcDbUnderlayDefinition") != std::string::npos);
    CHECK(text.find("sketch - 3") != std::string::npos);

    const DxfReadResult again = readDxf(text);
    REQUIRE(again.database != nullptr);
    const auto* readImage = again.database->get<ImageDefinition>(imageDefinition);
    REQUIRE(readImage != nullptr);
    CHECK(readImage->fileName == "scans/site.png");
    CHECK(readImage->sizeInPixels.x == doctest::Approx(800.0));
    CHECK_FALSE(readImage->ownerHandle.isNull());
    const auto* readPdf = again.database->get<UnderlayDefinition>(pdfDefinition);
    REQUIRE(readPdf != nullptr);
    CHECK(readPdf->itemName == "3");
    CHECK(readPdf->fileName == "plans/sketch.pdf");
    const RasterImageEntity* placed = firstImage(*again.database);
    REQUIRE(placed != nullptr);
    CHECK(placed->definitionHandle == imageDefinition);

    // Saved again, the definitions now come from the file and are written
    // back as read: still one of each, still one entry each.
    DxfWriter second;
    const std::string once = second.write(*again.database);
    // IMAGEDEF twice: its class declaration, and the object itself.
    CHECK_EQ(linesEqual(once, "IMAGEDEF"), 2u);
    CHECK_EQ(linesEqual(once, "AcDbUnderlayDefinition"), 1u);
    CHECK_EQ(linesEqual(once, "ACAD_IMAGE_DICT"), 1u);
    CHECK_EQ(linesEqual(once, "ACAD_PDFDEFINITIONS"), 1u);
}

TEST_CASE("a PDF underlay is read under either name it is written with") {
    // The converter's name for the entity, as a DWG arrives here.
    const std::string text =
        "0\nSECTION\n2\nENTITIES\n"
        "0\nPDFREFERENCE\n5\n2A\n330\n1F\n100\nAcDbEntity\n8\n0\n"
        "100\nAcDbUnderlayReference\n340\n2B\n10\n5.0\n20\n6.0\n30\n0.0\n"
        "41\n2.0\n42\n2.0\n43\n1.0\n50\n0.0\n280\n2\n281\n100\n282\n0\n"
        "0\nENDSEC\n0\nEOF\n";
    const DxfReadResult read = readDxf(text);
    REQUIRE(read.database != nullptr);
    const UnderlayEntity* underlay = nullptr;
    for (const Entity* entity : read.database->entitiesIn(read.database->modelSpace())) {
        underlay = dynamic_cast<const UnderlayEntity*>(entity);
        if (underlay != nullptr) {
            break;
        }
    }
    REQUIRE(underlay != nullptr);
    CHECK(underlay->kind == UnderlayKind::Pdf);
    CHECK(underlay->position.x == doctest::Approx(5.0));
    CHECK(underlay->scale.x == doctest::Approx(2.0));
    CHECK(underlay->definitionHandle == Handle{0x2B});
}

TEST_CASE("a drawing with an image or a PDF declares their classes when it is written") {
    std::unique_ptr<Database> db = Database::createEmpty();
    DxfWriter bare;
    CHECK(bare.write(*db).find("AcDbRasterImage") == std::string::npos);

    auto pdf = std::make_unique<UnderlayDefinition>();
    pdf->kind = UnderlayKind::Pdf;
    pdf->fileName = "sketch.pdf";
    pdf->itemName = "1";
    const Handle pdfHandle = db->addObject(std::move(pdf)).handle;
    auto underlay = std::make_unique<UnderlayEntity>();
    underlay->definitionHandle = pdfHandle;
    underlay->pageSize = Vec3{210.0, 297.0, 0.0};
    db->addToModelSpace(std::move(underlay));

    DxfWriter writer;
    const std::string text = writer.write(*db);
    CHECK_EQ(linesEqual(text, "CLASSES"), 1u);
    CHECK_EQ(linesEqual(text, "AcDbPdfReference"), 1u);
    CHECK_EQ(linesEqual(text, "AcDbPdfDefinition"), 1u);
    // Only what the drawing holds: no image classes for a drawing without one.
    CHECK_EQ(linesEqual(text, "AcDbRasterImage"), 0u);

    // Read back, the classes come from the file; adding an image then declares
    // the image classes beside them, and declares nothing twice.
    const DxfReadResult again = readDxf(text);
    REQUIRE(again.database != nullptr);
    auto definition = std::make_unique<ImageDefinition>();
    definition->fileName = "site.png";
    definition->sizeInPixels = Vec3{10.0, 10.0, 0.0};
    const Handle imageDefinition = again.database->addObject(std::move(definition)).handle;
    auto image = std::make_unique<RasterImageEntity>();
    image->definitionHandle = imageDefinition;
    image->widthPixels = 10.0;
    image->heightPixels = 10.0;
    again.database->addToModelSpace(std::move(image));
    DxfWriter second;
    const std::string more = second.write(*again.database);
    CHECK_EQ(linesEqual(more, "AcDbPdfReference"), 1u);
    CHECK_EQ(linesEqual(more, "AcDbRasterImageDefReactor"), 1u);
    CHECK_EQ(linesEqual(more, "CLASSES"), 1u);
}

TEST_CASE("an image survives a round-trip through the writer") {
    const DxfReadResult read = readDxf(drawingWithImage());
    REQUIRE(read.database != nullptr);

    DxfWriter writer;
    const std::string text = writer.write(*read.database);

    const DxfReadResult again = readDxf(text);
    const RasterImageEntity* image = firstImage(*again.database);
    REQUIRE(image != nullptr);

    CHECK(image->widthPixels == doctest::Approx(7177.0));
    CHECK(image->uVector.x == doctest::Approx(0.005000541854380505));
    CHECK(image->definitionHandle == Handle{0xDD70});
    CHECK(image->reactorHandle == Handle{0xDD78});
    CHECK(image->clipVertices.size() == 2);
    CHECK(image->clipVertices.back().x == doctest::Approx(7176.5));

    const auto* definition = again.database->get<ImageDefinition>(Handle{0xDD70});
    REQUIRE(definition != nullptr);
    CHECK(definition->fileName == "scans\\site-survey.png");
}

TEST_CASE("the scene hands the backend a top-left corner and two edge vectors") {
    // The entity stores its origin at the *bottom* left and its v vector points
    // up; a decoded raster's first row is at the top. The scene resolves that
    // flip once so no backend has to rediscover it, and this is the assertion
    // that says which way round it came out.
    const std::unique_ptr<Database> owned = Database::createEmpty();
    Database& db = *owned;

    auto definition = std::make_unique<ImageDefinition>();
    definition->fileName = "scan.png";
    definition->resolvedPath = "C:\\scans\\scan.png";
    const Handle definitionHandle = db.add(std::move(definition)).handle;

    auto image = std::make_unique<RasterImageEntity>();
    image->definitionHandle = definitionHandle;
    image->origin = Vec3{10.0, 20.0, 0.0};
    image->uVector = Vec3{0.5, 0.0, 0.0};
    image->vVector = Vec3{0.0, 0.5, 0.0};
    image->widthPixels = 40.0;
    image->heightPixels = 20.0;
    image->fade = 30;
    db.addToModelSpace(std::move(image));

    render::SpatialIndex index;
    render::SceneBuilder builder{db, index};
    builder.reindex(db.modelSpace());
    const render::Scene scene =
        builder.build(db.modelSpace(), render::Viewport{Vec3{20.0, 25.0}, 8.0, 800.0, 800.0});

    REQUIRE(scene.images.size() == 1);
    const render::SceneImage& payload = scene.images.front();
    CHECK(payload.path == "C:\\scans\\scan.png");
    // Origin (10, 20) plus a full height of v: 20 pixels at 0.5 is 10 units up.
    CHECK_POINT(payload.topLeft, (Vec3{10.0, 30.0, 0.0}));
    CHECK_POINT(payload.across, (Vec3{20.0, 0.0, 0.0}));
    CHECK_POINT(payload.down, (Vec3{0.0, -10.0, 0.0}));
    CHECK(payload.fade == 30);
}

TEST_CASE("an image inside a block reference moves with the reference") {
    const std::unique_ptr<Database> owned = Database::createEmpty();
    Database& db = *owned;

    auto definition = std::make_unique<ImageDefinition>();
    definition->fileName = "scan.png";
    definition->resolvedPath = "C:\\scans\\scan.png";
    const Handle definitionHandle = db.add(std::move(definition)).handle;

    auto definitionBlock = std::make_unique<BlockTableRecord>();
    definitionBlock->name = "Detail";
    BlockTableRecord& block = db.addBlock(std::move(definitionBlock));
    auto image = std::make_unique<RasterImageEntity>();
    image->definitionHandle = definitionHandle;
    image->origin = Vec3::zero();
    image->uVector = Vec3{1.0, 0.0, 0.0};
    image->vVector = Vec3{0.0, 1.0, 0.0};
    image->widthPixels = 10.0;
    image->heightPixels = 10.0;
    db.addEntity(std::move(image), block);

    db.addToModelSpace(std::make_unique<InsertEntity>(block.handle, Vec3{100.0, 100.0, 0.0}));

    render::SpatialIndex index;
    render::SceneBuilder builder{db, index};
    builder.reindex(db.modelSpace());
    const render::Scene scene =
        builder.build(db.modelSpace(), render::Viewport{Vec3{105.0, 105.0}, 8.0, 800.0, 800.0});

    REQUIRE(scene.images.size() == 1);
    // The whole placement travels with the block, not just the anchor: an image
    // whose corner moved and whose edges did not would come out sheared.
    CHECK_POINT(scene.images.front().topLeft, (Vec3{100.0, 110.0, 0.0}));
    CHECK_POINT(scene.images.front().across, (Vec3{10.0, 0.0, 0.0}));
}

}  // TEST_SUITE

TEST_SUITE("Underlays and OLE frames") {

TEST_CASE("a PDF underlay is read with its placement") {
    const DxfReadResult result = readDxf(dxf({
        {0, "SECTION"}, {2, "ENTITIES"},
        {0, "PDFUNDERLAY"}, {5, "A1"}, {330, "1F"},
        {100, "AcDbEntity"}, {8, "0"},
        {100, "AcDbUnderlayReference"},
        {340, "4B40"},
        {10, "12.0"}, {20, "34.0"}, {30, "0.0"},
        {41, "2.0"}, {42, "2.0"}, {43, "1.0"},
        {50, "90.0"}, {280, 3}, {281, 80}, {282, 20},
        {11, "0.0"}, {21, "0.0"}, {11, "100.0"}, {21, "60.0"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "OBJECTS"},
        {0, "PDFDEFINITION"}, {5, "4B40"}, {330, "C"},
        {100, "AcDbUnderlayDefinition"},
        {1, ".\\2nd Floor Remo Sketch.pdf"}, {2, "1"},
        {0, "ENDSEC"},
    }));
    REQUIRE(result.database != nullptr);

    UnderlayEntity* underlay = nullptr;
    for (Entity* entity : result.database->entitiesIn(result.database->modelSpace())) {
        underlay = dynamic_cast<UnderlayEntity*>(entity);
        if (underlay != nullptr) {
            break;
        }
    }
    REQUIRE(underlay != nullptr);

    CHECK(underlay->kind == UnderlayKind::Pdf);
    CHECK(underlay->objectType() == "PdfUnderlay");
    CHECK_POINT(underlay->position, (Vec3{12.0, 34.0, 0.0}));
    CHECK(underlay->scale.x == doctest::Approx(2.0));
    CHECK(underlay->rotation == doctest::Approx(kPi / 2.0));
    // Flags 3 is clipping on and the underlay switched on.
    CHECK(underlay->clippingEnabled);
    CHECK(underlay->underlayOn);
    CHECK_FALSE(underlay->monochrome);
    CHECK(underlay->contrast == 80);
    CHECK(underlay->fade == 20);
    CHECK(underlay->clipVertices.size() == 2);

    const auto* definition = result.database->get<UnderlayDefinition>(Handle{0x4B40});
    REQUIRE(definition != nullptr);
    CHECK(definition->fileName == ".\\2nd Floor Remo Sketch.pdf");
    CHECK(definition->itemName == "1");
    CHECK(definition->kind == UnderlayKind::Pdf);
}

TEST_CASE("an underlay with no clip and no known page size still has a place") {
    UnderlayEntity underlay;
    underlay.position = Vec3{5.0, 7.0, 0.0};

    // Nothing here can know how big a PDF page is without opening the PDF. The
    // entity must still report a location, or ZOOM EXTENTS leaves the user with
    // no way to find something they cannot see.
    CHECK(underlay.displayOutline().empty());
    CHECK_FALSE(underlay.bounds().isEmpty());
    CHECK_POINT(underlay.bounds().min, (Vec3{5.0, 7.0, 0.0}));
    CHECK(underlay.distanceToPoint(Vec3{5.0, 10.0, 0.0}) == doctest::Approx(3.0));
}

TEST_CASE("a clipped underlay is outlined by its clip, through its placement") {
    UnderlayEntity underlay;
    underlay.position = Vec3{100.0, 100.0, 0.0};
    underlay.scale = Vec3{2.0, 2.0, 1.0};
    underlay.clippingEnabled = true;
    underlay.clipVertices = {Vec3::zero(), Vec3{10.0, 5.0, 0.0}};

    const std::vector<Vec3> outline = underlay.displayOutline();
    REQUIRE(outline.size() == 4);
    CHECK_POINT(outline[0], (Vec3{100.0, 100.0, 0.0}));
    CHECK_POINT(outline[2], (Vec3{120.0, 110.0, 0.0}));
}

TEST_CASE("an OLE frame has corners even when written upside down") {
    OleFrameEntity frame;
    // The sample drawing's frame: stored upper-left first, so the y values
    // arrive in descending order.
    frame.upperLeft = Vec3{317.4312649832154, 1710.770939684494, 0.0};
    frame.lowerRight = Vec3{537.647315710216, 1597.006421813055, 0.0};

    const std::vector<Vec3> corners = frame.corners();
    REQUIRE(corners.size() == 4);
    CHECK(corners[0].x == doctest::Approx(317.4312649832154));
    CHECK(corners[0].y == doctest::Approx(1597.006421813055));
    CHECK(corners[2].x == doctest::Approx(537.647315710216));
    CHECK(corners[2].y == doctest::Approx(1710.770939684494));

    // Selectable by its area: before this the frame was invisible *and*
    // unpickable, so a user could see nothing and click nothing.
    CHECK(frame.distanceToPoint(Vec3{400.0, 1650.0, 0.0}) == doctest::Approx(0.0));
    CHECK_FALSE(frame.bounds().isEmpty());
}

TEST_CASE("an OLE frame keeps its payload across a round-trip") {
    // The compound document is megabytes in a real file. Its content is not
    // interpreted, so the requirement is only that every byte comes back.
    const std::string source = dxf({
        {0, "SECTION"}, {2, "ENTITIES"},
        {0, "OLE2FRAME"}, {5, "8A0B"}, {330, "1F"},
        {100, "AcDbEntity"}, {8, "A-PDFS"},
        {100, "AcDbOle2Frame"}, {70, 2},
        {10, "317.4312649832154"}, {20, "1710.770939684494"}, {30, "0.0"},
        {11, "537.647315710216"}, {21, "1597.006421813055"}, {31, "0.0"},
        {72, 0}, {73, 2}, {90, 64},
        {310, "D0CF11E0A1B11AE1000000000000000000000000000000003E000300FEFF0900"},
        {1, "OLE"},
        {0, "ENDSEC"},
    });

    const DxfReadResult read = readDxf(source);
    DxfWriter writer;
    const std::string text = writer.write(*read.database);

    CHECK(text.find("D0CF11E0A1B11AE1000000000000000000000000000000003E000300FEFF0900") !=
          std::string::npos);
    // The codes this application does not interpret come back too, or an OLE
    // object that reopened would no longer know what kind of object it is.
    CHECK(text.find("\r\n73\r\n") != std::string::npos);
    CHECK(text.find("\r\n90\r\n") != std::string::npos);

    const DxfReadResult again = readDxf(text);
    const OleFrameEntity* frame = nullptr;
    for (Entity* entity : again.database->entitiesIn(again.database->modelSpace())) {
        frame = dynamic_cast<const OleFrameEntity*>(entity);
        if (frame != nullptr) {
            break;
        }
    }
    REQUIRE(frame != nullptr);
    CHECK(frame->upperLeft.x == doctest::Approx(317.4312649832154));
    CHECK(frame->lowerRight.y == doctest::Approx(1597.006421813055));
}

}  // TEST_SUITE

TEST_SUITE("Tables") {

/// The shape of the sample drawing's EXISTING AREA CHART, cut to three rows.
///
/// A title spanning both columns, one row whose value comes from a field, and a
/// literal total — which is every kind of cell a schedule has.
[[nodiscard]] std::string drawingWithTable() {
    return dxf({
        {0, "SECTION"}, {2, "ENTITIES"},
        {0, "ACAD_TABLE"}, {5, "7081"}, {330, "1F"},
        {100, "AcDbEntity"}, {8, "A-NOTES"},
        {100, "AcDbBlockReference"}, {2, "*T19"},
        {10, "100.0"}, {20, "200.0"}, {30, "0.0"},
        {100, "AcDbTable"}, {342, "87"}, {343, "7082"},
        {11, "1.0"}, {21, "0.0"}, {31, "0.0"},
        {91, 3}, {92, 2},
        {141, "20.0"}, {141, "10.0"}, {141, "10.0"},
        {142, "80.0"}, {142, "40.0"},

        // Row 0: a title merged across both columns.
        {171, 1}, {172, 0}, {173, 0}, {174, 0}, {175, 2}, {176, 1},
        {140, "8.0"}, {301, "CELL_VALUE"}, {93, 6}, {90, 4},
        {1, "EXISTING AREA CHART"}, {302, "EXISTING AREA CHART"}, {304, "ACVALUE_END"},
        {171, 1}, {172, 0}, {173, 1}, {174, 0}, {175, 1}, {176, 1},
        {140, "8.0"}, {301, "CELL_VALUE"}, {93, 3}, {302, ""}, {304, "ACVALUE_END"},

        // Row 1: a name and an area, both from fields.
        {171, 1}, {172, 0}, {173, 0}, {174, 0}, {175, 1}, {176, 1},
        {344, "7097"}, {140, "5.0"},
        {301, "CELL_VALUE"}, {93, 3}, {302, ""}, {304, "ACVALUE_END"},
        {171, 1}, {172, 0}, {173, 0}, {174, 0}, {175, 1}, {176, 1},
        {344, "7129"}, {140, "5.0"},
        {301, "CELL_VALUE"}, {93, 3}, {302, ""}, {304, "ACVALUE_END"},

        // Row 2: a literal total.
        {171, 1}, {172, 0}, {173, 0}, {174, 0}, {175, 1}, {176, 1},
        {140, "5.0"}, {301, "CELL_VALUE"}, {93, 6}, {90, 4},
        {1, "TOTAL"}, {302, "TOTAL"}, {304, "ACVALUE_END"},
        {171, 1}, {172, 0}, {173, 0}, {174, 0}, {175, 1}, {176, 1},
        {140, "5.0"}, {301, "CELL_VALUE"}, {93, 6}, {90, 4},
        {1, "1142.30"}, {302, "1142.30"}, {304, "ACVALUE_END"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "OBJECTS"},
        // The name cell's field: a template over a child that reads a label.
        {0, "FIELD"}, {5, "7097"}, {330, "E3FB"},
        {100, "AcDbField"}, {1, "_text"}, {2, "2nd %<\\_FldIdx 0>%"},
        {360, "7098"}, {301, ""},
        {0, "FIELD"}, {5, "7098"}, {330, "7097"},
        {100, "AcDbField"}, {1, "AcObjProp"},
        {2, "\\AcObjProp Object(%<\\_ObjIdx 0>%).TextString \\f \"%tc1\""},
        {301, "MBATH"},
        // The area cell's field, cached the same way.
        {0, "FIELD"}, {5, "7129"}, {330, "E3FC"},
        {100, "AcDbField"}, {1, "_text"}, {2, "%<\\_FldIdx 0>%"},
        {360, "712A"}, {301, ""},
        {0, "FIELD"}, {5, "712A"}, {330, "7129"},
        {100, "AcDbField"}, {1, "AcObjProp"}, {2, "\\AcObjProp Object.Area"},
        {301, "71.81"},
        {0, "ENDSEC"}, {0, "EOF"},
    });
}

[[nodiscard]] TableEntity* firstTable(Database& db) {
    for (Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (auto* table = dynamic_cast<TableEntity*>(entity)) {
            return table;
        }
    }
    return nullptr;
}

TEST_CASE("an ACAD_TABLE is read as a grid, not a placeholder") {
    const DxfReadResult result = readDxf(drawingWithTable());
    REQUIRE(result.database != nullptr);

    TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    CHECK(table->rowCount() == 3);
    CHECK(table->columnCount() == 2);
    CHECK(table->rowHeights[0] == doctest::Approx(20.0));
    CHECK(table->columnWidths[0] == doctest::Approx(80.0));
    CHECK(table->styleHandle == Handle{0x87});
    // The anonymous block other CAD software drew the table into is kept, never drawn.
    CHECK(table->blockHandle == Handle{0x7082});
    CHECK(table->cells.size() == 6);
}

TEST_CASE("a cell holds either words or the field that produces them") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    const TableCell* title = table->cellAt(0, 0);
    REQUIRE(title != nullptr);
    CHECK(title->text == "EXISTING AREA CHART");
    CHECK(title->fieldHandle.isNull());

    const TableCell* name = table->cellAt(1, 0);
    REQUIRE(name != nullptr);
    CHECK(name->text.empty());
    CHECK(name->fieldHandle == Handle{0x7097});

    const TableCell* total = table->cellAt(2, 0);
    REQUIRE(total != nullptr);
    CHECK(total->text == "TOTAL");
}

TEST_CASE("a table grows down from its insertion point") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    // Insertion point is the *upper* left. A table read as growing upwards
    // lands on top of whatever it was placed below.
    const Box3 bounds = table->bounds();
    CHECK(bounds.min.x == doctest::Approx(100.0));
    CHECK(bounds.max.y == doctest::Approx(200.0));
    CHECK(bounds.max.x == doctest::Approx(220.0));
    CHECK(bounds.min.y == doctest::Approx(160.0));

    const Box3 second = table->cellBounds(1, 1);
    CHECK(second.min.x == doctest::Approx(180.0));
    CHECK(second.max.x == doctest::Approx(220.0));
    CHECK(second.max.y == doctest::Approx(180.0));
    CHECK(second.min.y == doctest::Approx(170.0));
}

TEST_CASE("a merged title spans its columns, not its rows") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    // Codes 175 and 176 are merged *width* and *height*, which read like a
    // row/column pair and are the other way round. Swapped, a title row merges
    // downwards and its text lands in the middle of the schedule.
    const TableCell* title = table->cellAt(0, 0);
    REQUIRE(title != nullptr);
    CHECK(title->columnSpan == 2);
    CHECK(title->rowSpan == 1);

    const Box3 box = table->cellBounds(0, 0);
    CHECK(box.min.x == doctest::Approx(100.0));
    CHECK(box.max.x == doctest::Approx(220.0));
    CHECK(box.max.y == doctest::Approx(200.0));
    CHECK(box.min.y == doctest::Approx(180.0));
}

TEST_CASE("a derived cell says what its field works out to") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    // Composed, not cached: the cell's own field is a template and only its
    // child holds a value. Reading the cache alone gives an empty grid, which
    // is what a schedule looked like before this.
    const TableCell* name = table->cellAt(1, 0);
    REQUIRE(name != nullptr);
    CHECK(name->displayText() == "2nd MBATH");

    const TableCell* area = table->cellAt(1, 1);
    REQUIRE(area != nullptr);
    CHECK(area->displayText() == "71.81");

    // A literal cell is untouched by the pass.
    CHECK(table->cellAt(2, 0)->displayText() == "TOTAL");
}

TEST_CASE("a table is picked by its area") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    // Clicking the middle of a schedule must select the schedule rather than
    // miss between two rules.
    CHECK(table->distanceToPoint(Vec3{160.0, 180.0, 0.0}) == doctest::Approx(0.0));
    CHECK(table->distanceToPoint(Vec3{230.0, 180.0, 0.0}) == doctest::Approx(10.0));
}

TEST_CASE("the grid is generated from the row and column sizes") {
    const DxfReadResult result = readDxf(drawingWithTable());
    const TableEntity* table = firstTable(*result.database);
    REQUIRE(table != nullptr);

    // The border, two interior rules across and one down. Drawn from the sizes
    // rather than replayed from the anonymous block, for the reason ADR 0005
    // gives for dimensions.
    CHECK(table->toCurves().size() == 4);
}

TEST_CASE("a table round-trips, cells and all") {
    const DxfReadResult read = readDxf(drawingWithTable());
    DxfWriter writer;
    const std::string text = writer.write(*read.database);

    // Nothing in the record is claimed, so every code comes back — including
    // the per-cell blocks, whose repeated codes are exactly why it is scanned
    // rather than consumed.
    CHECK(text.find("EXISTING AREA CHART") != std::string::npos);
    CHECK(text.find("AcDbTable") != std::string::npos);
    CHECK(text.find("\r\n344\r\n") != std::string::npos);

    const DxfReadResult again = readDxf(text);
    const TableEntity* table = firstTable(*again.database);
    REQUIRE(table != nullptr);
    CHECK(table->rowCount() == 3);
    CHECK(table->columnCount() == 2);
    CHECK(table->cells.size() == 6);
    CHECK(table->cellAt(1, 0)->fieldHandle == Handle{0x7097});
}

}  // TEST_SUITE

TEST_SUITE("Reference repair") {

TEST_CASE("an attached file can be listed without touching the disk") {
    const DxfReadResult result = readDxf(drawingWithImage());
    REQUIRE(result.database != nullptr);

    const std::vector<AttachedFileNode> files = attachedFiles(*result.database);
    REQUIRE(files.size() == 1);
    CHECK(files.front().kind == "Image");
    CHECK(files.front().storedPath == R"(scans\site-survey.png)");
    // Nothing has been resolved, so nothing claims to be found. A manager has
    // to be able to show the state before acting on it — and must not stall on
    // a network path that is slow to fail.
    CHECK_FALSE(files.front().found);
    CHECK(files.front().placements == 1);
}

TEST_CASE("a placement inside a block nothing inserts is counted as not drawn") {
    const DxfReadResult result = readDxf(drawingWithImageInAnUninsertedBlock());
    REQUIRE(result.database != nullptr);

    const std::vector<AttachedFileNode> files = attachedFiles(*result.database);
    REQUIRE(files.size() == 1);

    // The entity exists, so the file is placed once. It is drawn nowhere,
    // because a block definition is not a drawing of anything -- it is drawn
    // where an INSERT puts it, and nothing inserts this one.
    CHECK(files.front().placements == 1);
    CHECK(files.front().drawnPlacements == 0);

    // Named, because "0 drawn" tells a user their repair did not help without
    // telling them why, and the block name is the thing they can go and look
    // for.
    REQUIRE(files.front().unplacedBlocks.size() == 1);
    CHECK(files.front().unplacedBlocks.front() == "Binder2");
}

TEST_CASE("a placement in model space is counted as drawn") {
    const DxfReadResult result = readDxf(drawingWithImage());
    REQUIRE(result.database != nullptr);

    const std::vector<AttachedFileNode> files = attachedFiles(*result.database);
    REQUIRE(files.size() == 1);
    CHECK(files.front().placements == 1);
    CHECK(files.front().drawnPlacements == 1);
    CHECK(files.front().unplacedBlocks.empty());
}

TEST_CASE("repathing writes the stored path, not the one we resolved") {
    const DxfReadResult result = readDxf(drawingWithImage());
    Database& db = *result.database;
    db.filePath = R"(C:\projects\house\plan.dxf)";

    CHECK(repathAttachedFile(db, Handle{0xDD70}, R"(C:\projects\house\scans\survey.png)"));

    const auto* definition = db.get<ImageDefinition>(Handle{0xDD70});
    REQUIRE(definition != nullptr);
    // Inside the drawing's own folder, so relative — which is what keeps the
    // project working when the folder is copied to another machine. Writing the
    // absolute path would fix it here and break it everywhere else, which is
    // the failure the whole resolution mechanism exists to avoid.
    const std::string stored = definition->fileName;
    CHECK(stored.rfind(".", 0) == 0);
    CHECK(stored.find("survey.png") != std::string::npos);
    CHECK(stored.find("C:") == std::string::npos);
}

TEST_CASE("a file outside the drawing's folder keeps its absolute path") {
    const DxfReadResult result = readDxf(drawingWithImage());
    Database& db = *result.database;
    db.filePath = R"(C:\projects\house\plan.dxf)";

    CHECK(repathAttachedFile(db, Handle{0xDD70}, R"(D:\library\textures\brick.png)"));

    // A relative path to another drive is impossible, and to a distant folder
    // it is longer and more fragile than simply saying where the file is.
    const std::string stored = db.get<ImageDefinition>(Handle{0xDD70})->fileName;
    CHECK(stored.find("brick.png") != std::string::npos);
    CHECK(stored.rfind("..", 0) != 0);
}

TEST_CASE("repathing clears the stale resolution") {
    const DxfReadResult result = readDxf(drawingWithImage());
    Database& db = *result.database;
    auto* definition = db.get<ImageDefinition>(Handle{0xDD70});
    REQUIRE(definition != nullptr);
    definition->resolvedPath = R"(C:\old\wrong.png)";
    definition->resolutionProblem = "Not found";

    CHECK(repathAttachedFile(db, Handle{0xDD70}, R"(C:\new\right.png)"));

    // Otherwise the manager would go on reporting the old file as found, and
    // the renderer would go on drawing it.
    CHECK(definition->resolvedPath.empty());
    CHECK(definition->resolutionProblem.empty());
}

TEST_CASE("repathing something that is not a reference is refused") {
    const DxfReadResult result = readDxf(drawingWithImage());
    // DD77 is the IMAGE entity, not its definition. The path lives on the
    // definition, and quietly doing nothing would look like it had worked.
    CHECK_FALSE(repathAttachedFile(*result.database, Handle{0xDD77}, R"(C:\x.png)"));
}

}  // TEST_SUITE
