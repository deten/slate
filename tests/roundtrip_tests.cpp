#include "test_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;
using namespace cad::io;

namespace {

/// Read text, write it back, and read it again.
///
/// This is the shape that matters. Comparing the two *files* would be wrong —
/// the writer legitimately normalizes formatting, omits defaults and reorders
/// sections. What must survive is the *content*, so the comparison is between
/// the two databases.
[[nodiscard]] std::unique_ptr<Database> roundTrip(const std::string& text) {
    DxfReadResult first = readDxf(text);
    REQUIRE(first.database != nullptr);

    const std::string written = writeDxf(*first.database);
    DxfReadResult second = readDxf(written);
    REQUIRE(second.database != nullptr);

    // A file we wrote ourselves must parse without complaint. Warnings here
    // would mean the writer emits something the reader cannot understand.
    const bool anyWarnings =
        std::any_of(second.diagnostics.begin(), second.diagnostics.end(),
                    [](const DxfDiagnostic& d) {
                        return d.severity != DxfDiagnostic::Severity::Info;
                    });
    CHECK_FALSE(anyWarnings);

    return std::move(second.database);
}

template <typename T>
[[nodiscard]] std::vector<T*> entitiesOfType(Database& db) {
    std::vector<T*> result;
    for (Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (auto* typed = dynamic_cast<T*>(entity)) {
            result.push_back(typed);
        }
    }
    return result;
}

/// A drawing built in memory, so the round-trip starts from known content
/// rather than from text that might itself be wrong.
[[nodiscard]] std::unique_ptr<Database> buildDrawing() {
    auto db = Database::createEmpty();

    Layer& walls = db->ensureLayer("Walls");
    walls.color = Color::fromIndex(1);
    walls.lineweight = 50;

    Layer& hidden = db->ensureLayer("Hidden");
    hidden.isOff = true;
    hidden.color = Color::fromIndex(3);
    hidden.linetypeHandle = db->ensureLinetype("Dashed").handle;

    Layer& locked = db->ensureLayer("Locked");
    locked.isLocked = true;
    locked.isFrozen = true;

    auto line = std::make_unique<LineEntity>(Vec3{0.0, 0.0}, Vec3{5000.0, 0.0});
    line->layerHandle = walls.handle;
    db->addToModelSpace(std::move(line));

    auto coloured = std::make_unique<LineEntity>(Vec3{5000.0, 0.0}, Vec3{5000.0, 3000.0});
    coloured->color = Color::fromRgb(0x12, 0x34, 0x56);
    coloured->linetypeScale = 2.5;
    db->addToModelSpace(std::move(coloured));

    db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{1000.0, 1000.0}, 250.0));
    db->addToModelSpace(
        std::make_unique<ArcEntity>(Vec3{2000.0, 1000.0}, 500.0, 0.0, kPi));
    db->addToModelSpace(std::make_unique<PointEntity>(Vec3{42.0, 43.0}));

    // A polyline with a bulged span, which is the fiddliest thing to write:
    // the bulge must land between the right pair of coordinate codes.
    auto poly = std::make_unique<PolylineEntity>();
    poly->vertices.emplace_back(Vec3{0.0, 2000.0}, 1.0);
    poly->vertices.emplace_back(Vec3{1000.0, 2000.0});
    poly->vertices.emplace_back(Vec3{2000.0, 2000.0}, -0.5);
    poly->closed = true;
    db->addToModelSpace(std::move(poly));

    auto ellipse = std::make_unique<EllipseEntity>();
    ellipse->center = Vec3{4000.0, 2000.0};
    ellipse->majorAxis = Vec3{500.0, 0.0};
    ellipse->ratio = 0.5;
    db->addToModelSpace(std::move(ellipse));

    auto text = std::make_unique<TextEntity>(Vec3{100.0, 100.0}, "ROOM 101", 200.0);
    text->rotation = degToRad(30.0);
    db->addToModelSpace(std::move(text));

    auto mtext = std::make_unique<MTextEntity>(Vec3{500.0, 500.0},
                                               "First line\\PSecond line", 150.0);
    mtext->width = 2000.0;
    db->addToModelSpace(std::move(mtext));

    // A block with a reference to it.
    auto block = std::make_unique<BlockTableRecord>();
    block->name = "DOOR";
    BlockTableRecord& door = db->addBlock(std::move(block));
    db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{900.0, 0.0}), door);

    auto insert = std::make_unique<InsertEntity>(door.handle, Vec3{3000.0, 0.0});
    insert->rotation = degToRad(90.0);
    insert->scale = Vec3{2.0, 2.0, 1.0};
    db->addToModelSpace(std::move(insert));

    return db;
}

}  // namespace

TEST_SUITE("DXF round trip") {
    TEST_CASE("a written drawing reads back with the same content") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::string text = writeDxf(*original);

        DxfReadResult result = readDxf(text);
        REQUIRE(result.database != nullptr);
        Database& db = *result.database;

        CHECK(db.entitiesIn(db.modelSpace()).size() ==
              original->entitiesIn(original->modelSpace()).size());

        const auto errors =
            std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const DxfDiagnostic& d) {
                              return d.severity == DxfDiagnostic::Severity::Error;
                          });
        CHECK(errors == 0);
        CHECK(db.validate().empty());
    }

    TEST_CASE("layers survive with their colour, state and linetype") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        const Layer* walls = db->findLayer("Walls");
        REQUIRE(walls != nullptr);
        CHECK(walls->color.toRgb() == 0xFF0000u);
        CHECK(walls->lineweight == 50);

        // "Off" is carried as a negative colour index, so the magnitude must
        // survive alongside the flag.
        const Layer* hidden = db->findLayer("Hidden");
        REQUIRE(hidden != nullptr);
        CHECK(hidden->isOff);
        CHECK(hidden->color.index() == 3);
        CHECK(db->get<Linetype>(hidden->linetypeHandle)->name == "Dashed");

        const Layer* locked = db->findLayer("Locked");
        REQUIRE(locked != nullptr);
        CHECK(locked->isLocked);
        CHECK(locked->isFrozen);
    }

    TEST_CASE("geometry survives to full precision") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        const auto lines = entitiesOfType<LineEntity>(*db);
        REQUIRE(lines.size() == 2);
        CHECK_POINT(lines[0]->start, Vec3::zero());
        CHECK_POINT(lines[0]->end, Vec3(5000, 0));

        const auto circles = entitiesOfType<CircleEntity>(*db);
        REQUIRE(circles.size() == 1);
        CHECK_POINT(circles[0]->center, Vec3(1000, 1000));
        CHECK(circles[0]->radius == doctest::Approx(250.0));

        const auto arcs = entitiesOfType<ArcEntity>(*db);
        REQUIRE(arcs.size() == 1);
        // Angles go out as degrees and come back as radians; a slip here would
        // silently rotate every arc in the drawing.
        CHECK(arcs[0]->startAngle == doctest::Approx(0.0));
        CHECK(arcs[0]->endAngle == doctest::Approx(kPi));

        const auto ellipses = entitiesOfType<EllipseEntity>(*db);
        REQUIRE(ellipses.size() == 1);
        CHECK(ellipses[0]->ratio == doctest::Approx(0.5));
        CHECK(ellipses[0]->geometry().semiMajor() == doctest::Approx(500.0));

        const auto points = entitiesOfType<PointEntity>(*db);
        REQUIRE(points.size() == 1);
        CHECK_POINT(points[0]->position, Vec3(42, 43));
    }

    TEST_CASE("polyline bulges stay attached to their own vertex") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        const auto polys = entitiesOfType<PolylineEntity>(*db);
        REQUIRE(polys.size() == 1);
        REQUIRE(polys[0]->vertices.size() == 3);
        CHECK(polys[0]->closed);

        // The optional codes sit between coordinate pairs, so an off-by-one in
        // the writer would shift every bulge to the wrong span.
        CHECK(polys[0]->vertices[0].bulge == doctest::Approx(1.0));
        CHECK(polys[0]->vertices[1].bulge == doctest::Approx(0.0));
        CHECK(polys[0]->vertices[2].bulge == doctest::Approx(-0.5));
    }

    TEST_CASE("entity colours and scales survive") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        const auto lines = entitiesOfType<LineEntity>(*db);
        REQUIRE(lines.size() == 2);
        // ByLayer is written implicitly, so it must come back as ByLayer and
        // not as a resolved concrete colour.
        CHECK(lines[0]->color.isByLayer());
        CHECK(lines[1]->color.toRgb() == 0x123456u);
        CHECK(lines[1]->linetypeScale == doctest::Approx(2.5));
    }

    TEST_CASE("text and mtext survive with their formatting intact") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        bool foundText = false;
        for (Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (auto* text = dynamic_cast<TextEntity*>(entity)) {
                CHECK(text->contents == "ROOM 101");
                CHECK(text->height == doctest::Approx(200.0));
                CHECK(radToDeg(text->rotation) == doctest::Approx(30.0));
                foundText = true;
            }
        }
        CHECK(foundText);

        const auto mtexts = entitiesOfType<MTextEntity>(*db);
        REQUIRE(mtexts.size() == 1);
        // The raw contents keep their inline codes; a lossy parse on write
        // would destroy formatting this application does not render.
        CHECK(mtexts[0]->contents == "First line\\PSecond line");
        CHECK(mtexts[0]->plainText() == "First line\nSecond line");
    }

    TEST_CASE("blocks and their references survive") {
        const std::unique_ptr<Database> original = buildDrawing();
        const std::unique_ptr<Database> db = roundTrip(writeDxf(*original));

        BlockTableRecord* door = db->findBlock("DOOR");
        REQUIRE(door != nullptr);
        CHECK(door->entityCount() == 1);

        bool foundInsert = false;
        for (Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
                CHECK(insert->blockHandle == door->handle);
                CHECK_POINT(insert->position, Vec3(3000, 0));
                CHECK(radToDeg(insert->rotation) == doctest::Approx(90.0));
                CHECK(insert->scale.x == doctest::Approx(2.0));
                foundInsert = true;
            }
        }
        CHECK(foundInsert);
    }

    TEST_CASE("unknown entity types and group codes survive untouched") {
        // The whole justification for preserving them on read: a drawing with
        // features this application does not implement must come through an
        // open-edit-save cycle intact rather than being silently stripped.
        const std::string source =
            "0\r\nSECTION\r\n2\r\nENTITIES\r\n"
            "0\r\nHELIX\r\n5\r\n500\r\n8\r\n0\r\n10\r\n1.0\r\n20\r\n2.0\r\n40\r\n5.0\r\n"
            "0\r\nLINE\r\n5\r\n501\r\n8\r\n0\r\n10\r\n0.0\r\n20\r\n0.0\r\n"
            "11\r\n10.0\r\n21\r\n0.0\r\n160\r\n12345\r\n"
            "1001\r\nACME_APP\r\n1000\r\ncustom payload\r\n1040\r\n3.5\r\n"
            "0\r\nENDSEC\r\n0\r\nEOF\r\n";

        const std::unique_ptr<Database> db = roundTrip(source);

        // The unsupported type is still present and still knows what it was.
        const auto unsupported = entitiesOfType<UnsupportedEntity>(*db);
        REQUIRE(unsupported.size() == 1);
        CHECK(unsupported[0]->dxfType == "HELIX");

        const auto lines = entitiesOfType<LineEntity>(*db);
        REQUIRE(lines.size() == 1);
        REQUIRE(lines[0]->preserved != nullptr);

        const auto& codes = lines[0]->preserved->unknownCodes;
        CHECK(std::any_of(codes.begin(), codes.end(),
                          [](const UnknownGroup& g) { return g.code == 160; }));

        const auto* xdata = lines[0]->getXData("ACME_APP");
        REQUIRE(xdata != nullptr);
        REQUIRE(xdata->size() == 2);
        CHECK(std::get<std::string>((*xdata)[0].value) == "custom payload");
        CHECK(std::get<double>((*xdata)[1].value) == doctest::Approx(3.5));
    }

    TEST_CASE("header settings survive, including ones not modelled") {
        const std::string source =
            "0\r\nSECTION\r\n2\r\nHEADER\r\n"
            "9\r\n$ACADVER\r\n1\r\nAC1032\r\n"
            "9\r\n$INSUNITS\r\n70\r\n1\r\n"
            "9\r\n$LTSCALE\r\n40\r\n2.5\r\n"
            "9\r\n$CLAYER\r\n8\r\nWalls\r\n"
            "9\r\n$EXTMIN\r\n10\r\n0.0\r\n20\r\n0.0\r\n30\r\n0.0\r\n"
            "0\r\nENDSEC\r\n"
            "0\r\nSECTION\r\n2\r\nENTITIES\r\n"
            "0\r\nLINE\r\n8\r\nWalls\r\n10\r\n0.0\r\n20\r\n0.0\r\n11\r\n1.0\r\n21\r\n1.0\r\n"
            "0\r\nENDSEC\r\n0\r\nEOF\r\n";

        const std::unique_ptr<Database> db = roundTrip(source);

        CHECK(db->settings.insertionUnits == DrawingUnits::Inches);
        CHECK(db->settings.globalLinetypeScale == doctest::Approx(2.5));
        CHECK(db->settings.currentLayerName == "Walls");
        // A variable read but not modelled must not be reset by saving.
        CHECK(db->settings.unknownHeaderVariables.contains("$EXTMIN"));
    }

    TEST_CASE("an xref block keeps its stored path, not the resolved one") {
        const std::string source =
            "0\r\nSECTION\r\n2\r\nBLOCKS\r\n"
            "0\r\nBLOCK\r\n5\r\n400\r\n2\r\nGRID\r\n70\r\n4\r\n1\r\n./refs/grid.dwg\r\n"
            "10\r\n0.0\r\n20\r\n0.0\r\n"
            "0\r\nENDBLK\r\n5\r\n401\r\n"
            "0\r\nENDSEC\r\n0\r\nEOF\r\n";

        const std::unique_ptr<Database> db = roundTrip(source);

        BlockTableRecord* grid = db->findBlock("GRID");
        REQUIRE(grid != nullptr);
        CHECK(grid->isXref());
        CHECK(grid->xrefType == XrefType::Attachment);
        // Rewriting a portable relative path as absolute would break the
        // project for everyone else who opens it.
        CHECK(grid->xrefPath == "./refs/grid.dwg");
    }

    TEST_CASE("survives repeated round trips without drift") {
        std::unique_ptr<Database> db = buildDrawing();
        const std::size_t originalCount = db->entitiesIn(db->modelSpace()).size();

        // Content must reach a fixed point rather than degrading a little on
        // each save, which is how a format bug becomes data loss over time.
        for (int i = 0; i < 5; ++i) {
            db = roundTrip(writeDxf(*db));
        }

        CHECK(db->entitiesIn(db->modelSpace()).size() == originalCount);

        const auto circles = entitiesOfType<CircleEntity>(*db);
        REQUIRE(circles.size() == 1);
        CHECK_POINT(circles[0]->center, Vec3(1000, 1000));
        CHECK(circles[0]->radius == doctest::Approx(250.0));

        const auto polys = entitiesOfType<PolylineEntity>(*db);
        REQUIRE(polys.size() == 1);
        CHECK(polys[0]->vertices[0].bulge == doctest::Approx(1.0));
        CHECK(polys[0]->vertices[2].bulge == doctest::Approx(-0.5));
    }

    TEST_CASE("never emits scientific notation") {
        // Legal in the format, but several real consumers choke on it.
        auto db = Database::createEmpty();
        db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{1e-7, 0.0}, Vec3{1.5e9, 0.0}));

        const std::string text = writeDxf(*db);

        // Looking for a bare 'e' would match "SECTION" and every other keyword.
        // Scientific notation is specifically a digit, an exponent marker, then
        // a sign or digit.
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            const char c = text[i];
            if (c != 'e' && c != 'E') {
                continue;
            }
            const bool digitBefore = std::isdigit(static_cast<unsigned char>(text[i - 1])) != 0;
            const char after = text[i + 1];
            const bool exponentAfter =
                after == '+' || after == '-' || std::isdigit(static_cast<unsigned char>(after));
            const bool isScientific = digitBefore && exponentAfter;
            CHECK_FALSE(isScientific);
        }

        // And the very large and very small values still survive the trip.
        const std::unique_ptr<Database> reloaded = roundTrip(text);
        const auto lines = entitiesOfType<LineEntity>(*reloaded);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0]->end.x == doctest::Approx(1.5e9));
    }

    TEST_CASE("writing an empty drawing produces a readable file") {
        const auto db = Database::createEmpty();
        const std::string text = writeDxf(*db);

        DxfReadResult result = readDxf(text);
        REQUIRE(result.database != nullptr);
        CHECK(result.database->findLayer("0") != nullptr);
        CHECK(result.database->entitiesIn(result.database->modelSpace()).empty());
    }
}

TEST_CASE("moving a table moves the bytes it is written back from") {
    // A table is read for its meaning and written back for its bytes, so those
    // bytes are what a save emits. Moving the entity without moving them wrote
    // the table back where it started: the screen and the file disagreed, and
    // the file won.
    TableEntity table;
    table.position = Vec3{100.0, 200.0, 0.0};
    table.direction = Vec3{1.0, 0.0, 0.0};
    table.preserved = std::make_unique<PreservedData>();
    table.preserved->originalType = "ACAD_TABLE";
    table.preserved->unknownCodes = {
        {100, std::string{"AcDbBlockReference"}},
        {10, 100.0}, {20, 200.0}, {30, 0.0},
        {11, 1.0},   {21, 0.0},   {31, 0.0},
    };

    table.transformBy(Mat4::translation(Vec3{50.0, -25.0, 0.0}));

    CHECK(table.position.x == doctest::Approx(150.0));
    const std::vector<UnknownGroup>& codes = table.preserved->unknownCodes;
    const auto valueOf = [&codes](int code) {
        for (const UnknownGroup& group : codes) {
            if (group.code == code) {
                return std::get<double>(group.value);
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    CHECK(valueOf(10) == doctest::Approx(150.0));
    CHECK(valueOf(20) == doctest::Approx(175.0));
    // The direction is a vector, not a point: a translation must leave it be.
    CHECK(valueOf(11) == doctest::Approx(1.0));
    CHECK(valueOf(21) == doctest::Approx(0.0));
}

TEST_CASE("a preserved point that is not there reports rather than half-updating") {
    PreservedData preserved;
    preserved.unknownCodes = {{10, 1.0}};  // No 20 to go with it.
    CHECK_FALSE(preserved.rewritePoint(10, Vec3{9.0, 9.0, 0.0}));
    // Left alone: a point half-updated moves the object diagonally by whatever
    // the file happened to hold.
    CHECK(std::get<double>(preserved.unknownCodes.front().value) == doctest::Approx(1.0));
}

TEST_CASE("a drawing made here carries the dictionary a strict reader needs") {
    // Without a named object dictionary the OBJECTS section is a bare LAYOUT
    // owned by nothing. This reader is lenient enough not to mind; the ODA
    // converter is not, and refused every file with "Object of class AcDbLayout
    // can't be cast to AcDbDictionary" -- which meant a drawing drawn here
    // could be saved as DXF and never as DWG.
    auto db = Database::createEmpty();
    db->addToModelSpace(std::make_unique<LineEntity>(Vec3{}, Vec3{10.0, 10.0, 0.0}));
    // The shell always has a Model tab, so this is the state a saved drawing is
    // actually in. A bare database with no layouts writes no OBJECTS section at
    // all, which is legal and is not what this is about.
    static_cast<void>(db->ensureModelLayout());

    const std::string text = io::writeDxf(*db);
    const auto after = [&text](std::string_view marker) {
        return text.find(marker) != std::string::npos;
    };
    REQUIRE(after("OBJECTS"));
    CHECK(after("DICTIONARY"));
    CHECK(after("ACAD_LAYOUT"));
    CHECK(after("AcDbDictionary"));

    // And the layout is owned by it: the entry naming the layout has to point
    // at the layout, and the layout back at the dictionary, or the pair is as
    // broken as having neither.
    const io::DxfReadResult reloaded = io::readDxf(text);
    REQUIRE(reloaded.database != nullptr);
    const std::vector<Layout*> layouts = reloaded.database->layouts();
    REQUIRE_FALSE(layouts.empty());
    CHECK_FALSE(layouts.front()->ownerHandle.isNull());
}

TEST_CASE("a drawing that brought its own dictionaries does not get a second set") {
    // The synthesised one is for drawings that have none. Adding it to a file
    // that arrived with a full OBJECTS section would give it two roots.
    auto db = Database::createEmpty();
    static_cast<void>(db->ensureModelLayout());
    auto dictionary = std::make_unique<RawObject>();
    dictionary->dxfType = "DICTIONARY";
    db->addObject(std::move(dictionary));

    const std::string text = io::writeDxf(*db);
    std::size_t count = 0;
    for (std::size_t at = text.find("ACAD_LAYOUT"); at != std::string::npos;
         at = text.find("ACAD_LAYOUT", at + 1)) {
        ++count;
    }
    CHECK(count == 0);
}
