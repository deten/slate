#include "test_helpers.hpp"

#include <initializer_list>
#include <string>
#include <variant>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/Layer.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;
using namespace cad::io;

namespace {

/// One group pair, written as either an int or a string code.
struct Pair {
    int code;
    std::string value;

    Pair(int c, std::string v) : code(c), value(std::move(v)) {}
    Pair(int c, const char* v) : code(c), value(v) {}
    Pair(int c, double v) : code(c), value(std::to_string(v)) {}
    Pair(int c, int v) : code(c), value(std::to_string(v)) {}
};

/// Join group pairs into DXF text with CRLF endings, as other CAD software writes.
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

/// A minimal but structurally complete drawing: header, tables, a block
/// definition, and model-space geometry including a reference to that block.
[[nodiscard]] std::string minimalDrawing() {
    return dxf({
        {0, "SECTION"}, {2, "HEADER"},
        {9, "$ACADVER"}, {1, "AC1032"},
        {9, "$INSUNITS"}, {70, 4},
        {9, "$LTSCALE"}, {40, "2.5"},
        {9, "$CLAYER"}, {8, "Walls"},
        {9, "$EXTMIN"}, {10, "0"}, {20, "0"}, {30, "0"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "TABLES"},
        {0, "TABLE"}, {2, "LTYPE"},
        {0, "LTYPE"}, {5, "14"}, {2, "Continuous"}, {70, 0}, {3, "Solid line"},
        {0, "LTYPE"}, {5, "15"}, {2, "Dashed"}, {70, 0}, {3, "__ __ __"},
            {49, "12.7"}, {49, "-6.35"},
        {0, "ENDTAB"},

        {0, "TABLE"}, {2, "LAYER"},
        {0, "LAYER"}, {5, "10"}, {2, "0"}, {70, 0}, {62, 7}, {6, "Continuous"},
        {0, "LAYER"}, {5, "11"}, {2, "Walls"}, {70, 0}, {62, 1}, {6, "Continuous"}, {370, 50},
        // A negative colour index marks the layer as switched off.
        {0, "LAYER"}, {5, "12"}, {2, "Hidden"}, {70, 0}, {62, -3}, {6, "Dashed"},
        {0, "LAYER"}, {5, "13"}, {2, "Frozen"}, {70, 1}, {62, 5}, {6, "Continuous"},
        {0, "ENDTAB"},

        {0, "TABLE"}, {2, "STYLE"},
        {0, "STYLE"}, {5, "20"}, {2, "Standard"}, {70, 0}, {40, "0"}, {41, "1"}, {3, "txt.shx"},
        {0, "ENDTAB"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "BLOCKS"},
        {0, "BLOCK"}, {5, "30"}, {2, "DOOR"}, {70, 0}, {10, "0"}, {20, "0"}, {3, "DOOR"},
        {0, "LINE"}, {5, "31"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "900"}, {21, "0"},
        {0, "ARC"}, {5, "32"}, {8, "0"}, {10, "0"}, {20, "0"}, {40, "900"},
            {50, "0"}, {51, "90"},
        {0, "ENDBLK"}, {5, "33"},
        {0, "ENDSEC"},

        {0, "SECTION"}, {2, "ENTITIES"},

        {0, "LINE"}, {5, "100"}, {8, "Walls"},
            {10, "0"}, {20, "0"}, {30, "0"}, {11, "5000"}, {21, "0"}, {31, "0"},

        {0, "LINE"}, {5, "101"}, {8, "Walls"}, {62, 3},
            {10, "5000"}, {20, "0"}, {11, "5000"}, {21, "3000"},

        {0, "CIRCLE"}, {5, "102"}, {8, "0"}, {10, "1000"}, {20, "1000"}, {40, "250"},

        {0, "ARC"}, {5, "103"}, {8, "0"}, {10, "2000"}, {20, "1000"}, {40, "500"},
            {50, "0"}, {51, "180"},

        {0, "LWPOLYLINE"}, {5, "104"}, {8, "Walls"}, {90, 4}, {70, 1},
            {10, "0"}, {20, "0"},
            {10, "1000"}, {20, "0"},
            {10, "1000"}, {20, "1000"},
            {10, "0"}, {20, "1000"},

        // A polyline with a bulged segment: vertex 0 carries a half-circle.
        {0, "LWPOLYLINE"}, {5, "105"}, {8, "0"}, {90, 3}, {70, 0},
            {10, "0"}, {20, "2000"}, {42, "1"},
            {10, "1000"}, {20, "2000"},
            {10, "2000"}, {20, "2000"},

        {0, "TEXT"}, {5, "106"}, {8, "0"}, {10, "100"}, {20, "100"}, {40, "200"},
            {1, "ROOM 101"}, {7, "Standard"},

        {0, "MTEXT"}, {5, "107"}, {8, "0"}, {10, "500"}, {20, "500"}, {40, "150"},
            {41, "2000"}, {71, 1}, {1, "First line\\PSecond line"}, {7, "Standard"},

        {0, "INSERT"}, {5, "108"}, {8, "Walls"}, {2, "DOOR"},
            {10, "3000"}, {20, "0"}, {41, "1"}, {42, "1"}, {43, "1"}, {50, "90"},

        {0, "POINT"}, {5, "109"}, {8, "0"}, {10, "42"}, {20, "43"},

        {0, "ELLIPSE"}, {5, "10A"}, {8, "0"}, {10, "4000"}, {20, "2000"},
            {11, "500"}, {21, "0"}, {31, "0"}, {40, "0.5"},
            {41, "0"}, {42, "6.283185307179586"},

        {0, "ENDSEC"},
        {0, "EOF"},
    });
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

}  // namespace

TEST_SUITE("DXF tokenizer") {
    TEST_CASE("reads code/value pairs with typed values") {
        DxfTokenizer tokenizer{"0\r\nSECTION\r\n10\r\n1.5\r\n70\r\n42\r\n"};
        const std::vector<DxfGroup> groups = tokenizer.readAll();

        REQUIRE(groups.size() == 3);
        CHECK(groups[0].code == 0);
        CHECK(groups[0].asString() == "SECTION");
        // Code 10 is a double, 70 an integer.
        CHECK(std::holds_alternative<double>(groups[1].value));
        CHECK(groups[1].asDouble() == doctest::Approx(1.5));
        CHECK(std::holds_alternative<std::int64_t>(groups[2].value));
        CHECK(groups[2].asInteger() == 42);
    }

    TEST_CASE("keeps handles as text rather than parsing them as numbers") {
        DxfTokenizer tokenizer{"5\r\n1E\r\n"};
        const std::vector<DxfGroup> groups = tokenizer.readAll();
        REQUIRE(groups.size() == 1);
        // "1E" would become 1 or a float if number-parsed.
        CHECK(groups[0].asString() == "1E");
    }

    TEST_CASE("preserves leading and trailing spaces inside string values") {
        DxfTokenizer tokenizer{"1\r\n  spaced text  \r\n"};
        const std::vector<DxfGroup> groups = tokenizer.readAll();
        REQUIRE(groups.size() == 1);
        CHECK(groups[0].asString() == "  spaced text  ");
    }

    TEST_CASE("reports a malformed group code and stays in sync") {
        DxfTokenizer tokenizer{"0\nLINE\nbroken\nvalue\n10\n5\n"};
        const std::vector<DxfGroup> groups = tokenizer.readAll();

        CHECK_FALSE(tokenizer.diagnostics().empty());
        // The valid groups on either side are still read.
        REQUIRE(groups.size() == 2);
        CHECK(groups[0].code == 0);
        CHECK(groups[1].code == 10);
    }

    TEST_CASE("handles CRLF, LF, lone CR and a BOM in one file") {
        const std::string text =
            "\xEF\xBB\xBF"  // UTF-8 BOM
            "  0\r\nSECTION\n2  \r\nENTITIES\r"
            "0\rLINE\r5\r700\r8\r0\r10\r1.5\r20\r2.5\r11\r3.5\r21\r4.5\n"
            "0\nENDSEC\n0\nEOF\n";

        const DxfReadResult result = readDxf(text);
        const auto lines = entitiesOfType<LineEntity>(*result.database);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0]->start.equals(Vec3{1.5, 2.5}));
        CHECK(lines[0]->end.equals(Vec3{3.5, 4.5}));
    }
}

TEST_SUITE("Reading a complete drawing") {
    TEST_CASE("reports the DXF version") {
        const DxfReadResult result = readDxf(minimalDrawing());
        CHECK(result.version == "AC1032");
        CHECK(result.versionName == "other CAD software");
        CHECK(result.succeeded());
    }

    TEST_CASE("reads header variables into drawing settings") {
        const DxfReadResult result = readDxf(minimalDrawing());
        const DrawingSettings& settings = result.database->settings;
        CHECK(settings.insertionUnits == DrawingUnits::Millimeters);
        CHECK(settings.globalLinetypeScale == doctest::Approx(2.5));
        CHECK(settings.currentLayerName == "Walls");
    }

    TEST_CASE("preserves header variables it does not model") {
        const DxfReadResult result = readDxf(minimalDrawing());
        CHECK(result.database->settings.unknownHeaderVariables.contains("$EXTMIN"));
    }

    TEST_CASE("reads the layer table with colours and states") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        const Layer* walls = db.findLayer("Walls");
        REQUIRE(walls != nullptr);
        CHECK(walls->color.toRgb() == 0xFF0000u);
        CHECK(walls->lineweight == 50);

        // A negative colour index means the layer is off, and the magnitude
        // remains its colour.
        const Layer* hidden = db.findLayer("Hidden");
        REQUIRE(hidden != nullptr);
        CHECK(hidden->isOff);
        CHECK(hidden->color.index() == 3);
        CHECK_FALSE(hidden->isVisible());

        CHECK(db.findLayer("Frozen")->isFrozen);
    }

    TEST_CASE("reads linetype patterns and links them to layers") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        const Linetype* dashed = db.findLinetype("Dashed");
        REQUIRE(dashed != nullptr);
        REQUIRE(dashed->elements.size() == 2);
        CHECK(dashed->elements[0].length == doctest::Approx(12.7));
        CHECK(dashed->elements[1].length == doctest::Approx(-6.35));
        CHECK(db.findLinetype("Continuous")->isContinuous());

        CHECK(db.findLayer("Hidden")->linetypeHandle == dashed->handle);
    }

    TEST_CASE("reads lines with their layer and colour") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        const auto lines = entitiesOfType<LineEntity>(db);
        REQUIRE(lines.size() == 2);

        CHECK(lines[0]->start.equals(Vec3::zero()));
        CHECK(lines[0]->end.equals(Vec3{5000, 0}));
        CHECK(lines[0]->layerHandle == db.findLayer("Walls")->handle);
        CHECK(lines[0]->color.isByLayer());

        // The second carries an explicit ACI 3.
        CHECK(lines[1]->color.index() == 3);
    }

    TEST_CASE("converts arc angles from degrees to radians") {
        const DxfReadResult result = readDxf(minimalDrawing());
        const auto arcs = entitiesOfType<ArcEntity>(*result.database);
        REQUIRE(arcs.size() == 1);
        CHECK(arcs[0]->radius == doctest::Approx(500.0));
        CHECK(radToDeg(arcs[0]->startAngle) == doctest::Approx(0.0));
        CHECK(radToDeg(arcs[0]->endAngle) == doctest::Approx(180.0));
        CHECK(arcs[0]->geometry().length() == doctest::Approx(kPi * 500.0));
    }

    TEST_CASE("reads a closed lwpolyline") {
        const DxfReadResult result = readDxf(minimalDrawing());
        const auto polys = entitiesOfType<PolylineEntity>(*result.database);
        REQUIRE(polys.size() == 2);

        CHECK(polys[0]->closed);
        CHECK(polys[0]->vertices.size() == 4);
        CHECK(polys[0]->geometry().area() == doctest::Approx(1'000'000.0));
    }

    TEST_CASE("associates a bulge with the correct vertex") {
        const DxfReadResult result = readDxf(minimalDrawing());
        const auto polys = entitiesOfType<PolylineEntity>(*result.database);
        REQUIRE(polys.size() == 2);

        const PolylineEntity& bulged = *polys[1];
        REQUIRE(bulged.vertices.size() == 3);
        // The bulge belongs to vertex 0, not vertex 1. Optional codes cannot be
        // zipped positionally, which is why the reader walks file order.
        CHECK(bulged.vertices[0].bulge == doctest::Approx(1.0));
        CHECK(bulged.vertices[1].bulge == doctest::Approx(0.0));

        const Polyline geometry = bulged.geometry();
        CHECK(dynamic_cast<const Arc*>(geometry.segmentAt(0)) != nullptr);
        CHECK(dynamic_cast<const Arc*>(geometry.segmentAt(1)) == nullptr);
    }

    TEST_CASE("reads single-line text") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        for (Entity* entity : db.entitiesIn(db.modelSpace())) {
            if (auto* text = dynamic_cast<TextEntity*>(entity)) {
                CHECK(text->contents == "ROOM 101");
                CHECK(text->height == doctest::Approx(200.0));
                CHECK(text->styleHandle == db.findTextStyle("Standard")->handle);
                return;
            }
        }
        FAIL("no TEXT entity was read");
    }

    TEST_CASE("reads mtext and strips its formatting for display") {
        const DxfReadResult result = readDxf(minimalDrawing());
        const auto mtexts = entitiesOfType<MTextEntity>(*result.database);
        REQUIRE(mtexts.size() == 1);
        // The raw contents keep the formatting codes for round-tripping.
        CHECK(mtexts[0]->contents == "First line\\PSecond line");
        CHECK(mtexts[0]->plainText() == "First line\nSecond line");
        CHECK(mtexts[0]->width == doctest::Approx(2000.0));
    }

    TEST_CASE("reads a point and an ellipse") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        const auto points = entitiesOfType<PointEntity>(db);
        REQUIRE(points.size() == 1);
        CHECK(points[0]->position.equals(Vec3{42, 43}));

        const auto ellipses = entitiesOfType<EllipseEntity>(db);
        REQUIRE(ellipses.size() == 1);
        CHECK(ellipses[0]->ratio == doctest::Approx(0.5));
        CHECK(ellipses[0]->geometry().semiMajor() == doctest::Approx(500.0));
        CHECK(ellipses[0]->geometry().semiMinor() == doctest::Approx(250.0));
        CHECK(ellipses[0]->geometry().isClosed());
    }

    TEST_CASE("reads a block definition and resolves the reference to it") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        BlockTableRecord* door = db.findBlock("DOOR");
        REQUIRE(door != nullptr);
        CHECK(door->entityCount() == 2);

        const auto inserts = entitiesOfType<InsertEntity>(db);
        REQUIRE(inserts.size() == 1);
        // The block is named, not handle-referenced, and may be defined after
        // the entity; deferred resolution is what makes this work.
        CHECK(inserts[0]->blockHandle == door->handle);
        CHECK(inserts[0]->position.equals(Vec3{3000, 0}));
        CHECK(radToDeg(inserts[0]->rotation) == doctest::Approx(90.0));
    }

    TEST_CASE("produces a referentially valid database with sensible extents") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        const auto issues = db.validate();
        const bool anyErrors =
            std::any_of(issues.begin(), issues.end(), [](const DatabaseIssue& i) {
                return i.severity == DatabaseIssue::Severity::Error;
            });
        CHECK_FALSE(anyErrors);

        const Box3 extents = db.modelExtents();
        CHECK_FALSE(extents.isEmpty());
        CHECK(extents.max.x >= 5000.0);
    }

    TEST_CASE("keeps the handles the file assigned and allocates beyond them") {
        const DxfReadResult result = readDxf(minimalDrawing());
        Database& db = *result.database;

        CHECK(dynamic_cast<LineEntity*>(db.getObject(Handle{0x100})) != nullptr);
        CHECK(db.findLayer("Walls")->handle == Handle{0x11});

        // A newly allocated handle must be beyond every handle the file used.
        Entity& added =
            db.addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}));
        CHECK(added.handle.value() > 0x10A);
    }
}

TEST_SUITE("Legacy POLYLINE and attributes") {
    TEST_CASE("attaches VERTEX children and resumes reading after SEQEND") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "POLYLINE"}, {5, "200"}, {8, "0"}, {66, 1}, {70, 1},
                {10, "0"}, {20, "0"}, {30, "0"},
            {0, "VERTEX"}, {5, "201"}, {8, "0"}, {10, "0"}, {20, "0"},
            {0, "VERTEX"}, {5, "202"}, {8, "0"}, {10, "100"}, {20, "0"}, {42, "0.5"},
            {0, "VERTEX"}, {5, "203"}, {8, "0"}, {10, "100"}, {20, "100"},
            {0, "SEQEND"}, {5, "204"}, {8, "0"},
            {0, "LINE"}, {5, "205"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "1"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        const auto polys = entitiesOfType<PolylineEntity>(db);
        REQUIRE(polys.size() == 1);
        CHECK(polys[0]->isLegacyFormat);
        CHECK(polys[0]->closed);
        REQUIRE(polys[0]->vertices.size() == 3);
        CHECK(polys[0]->vertices[1].bulge == doctest::Approx(0.5));

        // The VERTEX records must not become entities of their own, and the
        // LINE after SEQEND must still be read.
        CHECK(db.entitiesIn(db.modelSpace()).size() == 2);
        CHECK(entitiesOfType<LineEntity>(db).size() == 1);
    }

    TEST_CASE("reads attribute definitions and values and links them") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "BLOCKS"},
            {0, "BLOCK"}, {5, "300"}, {2, "TITLEBLOCK"}, {70, 2}, {10, "0"}, {20, "0"},
            {0, "ATTDEF"}, {5, "301"}, {8, "0"}, {10, "0"}, {20, "0"}, {40, "5"},
                {1, "DEFAULT"}, {2, "DRAWING_NO"}, {3, "Enter drawing number"}, {70, 0},
            {0, "ENDBLK"}, {5, "302"},
            {0, "ENDSEC"},
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "INSERT"}, {5, "310"}, {8, "0"}, {66, 1}, {2, "TITLEBLOCK"},
                {10, "0"}, {20, "0"},
            {0, "ATTRIB"}, {5, "311"}, {8, "0"}, {10, "0"}, {20, "0"}, {40, "5"},
                {1, "A-101"}, {2, "DRAWING_NO"}, {70, 0},
            {0, "ATTRIB"}, {5, "312"}, {8, "0"}, {10, "0"}, {20, "10"}, {40, "5"},
                {1, "GROUND FLOOR PLAN"}, {2, "TITLE"}, {70, 0},
            {0, "SEQEND"}, {5, "313"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        BlockTableRecord* block = db.findBlock("TITLEBLOCK");
        REQUIRE(block != nullptr);
        bool foundDefinition = false;
        for (const Entity* entity : db.entitiesIn(*block)) {
            if (const auto* attdef = dynamic_cast<const AttributeDefinitionEntity*>(entity)) {
                CHECK(attdef->tag == "DRAWING_NO");
                CHECK(attdef->prompt == "Enter drawing number");
                foundDefinition = true;
            }
        }
        CHECK(foundDefinition);

        const auto attributes = entitiesOfType<AttributeEntity>(db);
        REQUIRE(attributes.size() == 2);

        // This is what carries the real data in a professional drawing.
        std::vector<std::string> tags;
        for (const AttributeEntity* attribute : attributes) {
            tags.push_back(attribute->tag + "=" + attribute->value());
        }
        std::sort(tags.begin(), tags.end());
        CHECK(tags[0] == "DRAWING_NO=A-101");
        CHECK(tags[1] == "TITLE=GROUND FLOOR PLAN");

        const auto inserts = entitiesOfType<InsertEntity>(db);
        REQUIRE(inserts.size() == 1);
        CHECK(inserts[0]->attributeHandles.size() == 2);
    }
}

TEST_SUITE("External references") {
    TEST_CASE("recognizes attachments and overlays without claiming they are missing") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "BLOCKS"},
            {0, "BLOCK"}, {5, "400"}, {2, "GRID"}, {70, 4}, {1, "./refs/grid.dwg"},
                {10, "0"}, {20, "0"},
            {0, "ENDBLK"}, {5, "401"},
            {0, "BLOCK"}, {5, "402"}, {2, "SITE"}, {70, 12}, {1, "../site/topo.dwg"},
                {10, "0"}, {20, "0"},
            {0, "ENDBLK"}, {5, "403"},
            {0, "ENDSEC"},
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "INSERT"}, {5, "410"}, {8, "0"}, {2, "GRID"}, {10, "0"}, {20, "0"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        BlockTableRecord* grid = db.findBlock("GRID");
        REQUIRE(grid != nullptr);
        CHECK(grid->isXref());
        CHECK(grid->xrefType == XrefType::Attachment);
        CHECK(grid->xrefPath == "./refs/grid.dwg");

        CHECK(db.findBlock("SITE")->xrefType == XrefType::Overlay);

        // "Not found" is a claim the reader has not earned yet — resolution has
        // not run.
        CHECK(grid->xrefStatus == XrefStatus::Unloaded);
        CHECK(db.unresolvedReferences().empty());
        CHECK(db.externalReferences().size() == 2);

        // The insert still resolves to the xref's block record.
        const auto inserts = entitiesOfType<InsertEntity>(db);
        REQUIRE(inserts.size() == 1);
        CHECK(inserts[0]->blockHandle == grid->handle);
    }
}

TEST_SUITE("Preserving unrecognized data") {
    TEST_CASE("keeps unsupported types, unknown codes and third-party XData") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "HELIX"}, {5, "500"}, {8, "0"}, {10, "1"}, {20, "2"}, {40, "5"}, {280, 1},
            {0, "LINE"}, {5, "501"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},
                // An unmodelled code on a known entity.
                {160, 12345},
                {1001, "ACME_APP"}, {1000, "custom payload"}, {1040, "3.5"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        // An unsupported type is kept rather than dropped: silently deleting
        // geometry on save is the most damaging thing a reader can do.
        const auto unsupported = entitiesOfType<UnsupportedEntity>(db);
        REQUIRE(unsupported.size() == 1);
        CHECK(unsupported[0]->dxfType == "HELIX");
        REQUIRE(unsupported[0]->preserved != nullptr);
        CHECK_FALSE(unsupported[0]->preserved->unknownCodes.empty());
        // But it contributes nothing to extents and cannot be picked.
        CHECK(unsupported[0]->bounds().isEmpty());

        const auto lines = entitiesOfType<LineEntity>(db);
        REQUIRE(lines.size() == 1);
        REQUIRE(lines[0]->preserved != nullptr);

        const auto& codes = lines[0]->preserved->unknownCodes;
        const bool has160 = std::any_of(codes.begin(), codes.end(),
                                        [](const UnknownGroup& g) { return g.code == 160; });
        CHECK(has160);

        const auto* xdata = lines[0]->getXData("ACME_APP");
        REQUIRE(xdata != nullptr);
        REQUIRE(xdata->size() == 2);
        CHECK(std::get<std::string>((*xdata)[0].value) == "custom payload");

        // And all of it survives a clone, which undo depends on.
        const DbObjectPtr copy = lines[0]->clone();
        REQUIRE(copy->preserved != nullptr);
        CHECK(copy->preserved->unknownCodes.size() == codes.size());
        CHECK(copy->getXData("ACME_APP") != nullptr);
    }

    /// A DICTIONARY is a run of alternating pairs: code 3 names an entry and
    /// the 350 after it is the object. Nothing but the order says which name
    /// goes with which handle.
    ///
    /// Preserved groups used to be written grouped by code, which kept both
    /// lists complete and paired every name with the wrong object. A saved
    /// drawing then had its layer states, groups, field definitions and plot
    /// settings all pointing somewhere else, while still appearing to contain
    /// them — the failure looks like corruption in the other application.
    TEST_CASE("preserved groups keep file order, so dictionaries survive") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "OBJECTS"},
            {0, "DICTIONARY"}, {5, "18E"},
                {3, "ACAD_LAYERFILTERS"}, {350, "9195"},
                {3, "ACAD_LAYERSTATES"}, {350, "18F"},
                {3, "ACLYDICTIONARY"}, {350, "9194"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        const auto* dictionary = result.database->get<RawObject>(Handle{0x18E});
        REQUIRE(dictionary != nullptr);
        REQUIRE(dictionary->preserved != nullptr);

        std::vector<std::pair<int, std::string>> pairs;
        for (const UnknownGroup& group : dictionary->preserved->unknownCodes) {
            if (group.code == 3 || group.code == 350) {
                pairs.emplace_back(group.code, std::get<std::string>(group.value));
            }
        }
        const std::vector<std::pair<int, std::string>> expected{
            {3, "ACAD_LAYERFILTERS"}, {350, "9195"},
            {3, "ACAD_LAYERSTATES"},  {350, "18F"},
            {3, "ACLYDICTIONARY"},    {350, "9194"},
        };
        CHECK(pairs == expected);
    }

    /// Group 101 starts a second object inside the record whose codes begin
    /// again from scratch. An annotative MTEXT is the common case, and its
    /// embedded object repeats the insertion point as code 11 — which is the
    /// MTEXT's own code for its X-axis direction vector.
    ///
    /// The pairs below are taken from a real other CAD software drawing: a room label
    /// reading "Bath" at (622.65, 426.40). Read past the marker, the label
    /// takes the angle of its own position vector and hangs at 34 degrees.
    TEST_CASE("does not read an embedded object's codes as the entity's own") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "MTEXT"}, {5, "600"}, {8, "0"},
                {10, "622.6496128062918"}, {20, "426.4007955679972"}, {30, "0"},
                {40, "9.3"}, {41, "55.98"}, {71, 1}, {1, "Bath"},
            {101, "Embedded Object"},
                {70, 1}, {10, "1.0"}, {20, "0"}, {30, "0"},
                {11, "622.6496128062918"}, {21, "426.4007955679972"}, {31, "0"},
                {40, "55.98"}, {42, "24.89"}, {43, "9.45"},
            {1001, "ACAD"}, {1000, "after the embedded object"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        const auto mtexts = entitiesOfType<MTextEntity>(*result.database);
        REQUIRE(mtexts.size() == 1);

        CHECK(mtexts[0]->contents == "Bath");
        CHECK(mtexts[0]->rotation == doctest::Approx(0.0));
        CHECK_FALSE(mtexts[0]->directionVector.has_value());
        // The MTEXT's own code 40 is the text height, not the embedded
        // object's reference width.
        CHECK(mtexts[0]->height == doctest::Approx(9.3));

        // The tail is kept in file order so it can be written back as it
        // arrived, rather than sorted by code among the unknown groups.
        REQUIRE(mtexts[0]->preserved != nullptr);
        const auto& embedded = mtexts[0]->preserved->embeddedObject;
        std::vector<int> codes;
        for (const UnknownGroup& group : embedded) {
            codes.push_back(group.code);
        }
        CHECK(codes == std::vector<int>{101, 70, 10, 20, 30, 11, 21, 31, 40, 42, 43});

        // XData is written after the embedded object, so a reader that stops
        // at the marker drops it — on exactly the entities that have one.
        const auto* xdata = mtexts[0]->getXData("ACAD");
        REQUIRE(xdata != nullptr);
        REQUIRE(xdata->size() == 1);
        CHECK(std::get<std::string>((*xdata)[0].value) == "after the embedded object");
    }

    /// The other half of the same drawing: an MTEXT with no embedded object,
    /// whose code 11 really is the X-axis direction vector. Ignoring code 11
    /// outright would fix the room labels by standing every genuinely vertical
    /// label back up, which is the same bug facing the other way.
    TEST_CASE("a direction vector still rotates text when there is no embedded object") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "MTEXT"}, {5, "601"}, {8, "0"},
                {10, "18.2"}, {20, "132.4"}, {40, "3.16"}, {1, "850.0"},
                // (cos 90, sin 90), as other CAD software writes a vertical label.
                {11, "6.123233995736766E-17"}, {21, "1.0"}, {31, "0"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        const auto mtexts = entitiesOfType<MTextEntity>(*result.database);
        REQUIRE(mtexts.size() == 1);
        CHECK(mtexts[0]->rotation == doctest::Approx(kPi / 2.0));
        CHECK(mtexts[0]->directionVector.has_value());
    }

    /// Code 330 names the owner *and* every reactor, and the reactors come
    /// first, wrapped in `102 {ACAD_REACTORS}` … `102 }`. Taking the first 330
    /// therefore reads a reactor handle as the owner on any drawing where
    /// other CAD software attached one.
    ///
    /// It hides well, because a reactor handle usually resolves to nothing and
    /// an entity with no owner falls back to model space — where most entities
    /// belong anyway. It stops hiding on a paper-space entity, whose sheet then
    /// empties onto the model and gets written back out as model geometry.
    TEST_CASE("a reactor group does not masquerade as the owner") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "BLOCKS"},
            {0, "BLOCK"}, {5, "20"}, {330, "1F"}, {8, "0"},
                {2, "*Paper_Space"}, {70, 0}, {10, "0"}, {20, "0"}, {30, "0"},
                {3, "*Paper_Space"}, {1, ""},
            {0, "ENDBLK"}, {5, "21"}, {8, "0"},
            {0, "ENDSEC"},

            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "LINE"}, {5, "600"},
                {102, "{ACAD_REACTORS"}, {330, "1F2"}, {102, "}"},
                {330, "1F"},
                {8, "0"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        const BlockTableRecord* paper = db.findBlock(kPaperSpace);
        REQUIRE(paper != nullptr);
        CHECK(db.entitiesIn(*paper).size() == 1);
        CHECK(db.entitiesIn(db.modelSpace()).empty());

        // And the owner recorded on the entity is the block record, not the
        // reactor — the sheet placement above would also come out right if the
        // owner were simply dropped.
        const std::vector<Entity*> filed = db.entitiesIn(*paper);
        REQUIRE(filed.size() == 1);
        CHECK(filed[0]->ownerHandle == paper->handle);
    }
}

TEST_SUITE("Resilience to malformed input") {
    TEST_CASE("reads what it can and reports the rest") {
        const std::string text =
            "0\nSECTION\n2\nENTITIES\n"
            "0\nLINE\n5\n600\n8\n0\n10\nnot-a-number\n20\n0\n11\n10\n21\n0\n"
            "garbage\nvalue\n"
            "0\nCIRCLE\n5\n601\n8\n0\n10\n5\n20\n5\n40\n3\n"
            "0\nENDSEC\n0\nEOF\n";

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        // The circle after the damaged region is still read.
        CHECK(entitiesOfType<CircleEntity>(db).size() == 1);
        CHECK_FALSE(result.diagnostics.empty());

        // An unparseable coordinate reads as 0 rather than shifting the triple
        // and misplacing the whole entity.
        const auto lines = entitiesOfType<LineEntity>(db);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0]->start.equals(Vec3::zero()));
        CHECK(lines[0]->end.equals(Vec3{10, 0}));
    }

    TEST_CASE("does not throw on empty or truncated input") {
        CHECK_NOTHROW((void)readDxf(""));
        CHECK_NOTHROW((void)readDxf("0\r\nSECTION\r\n2\r\nENTITIES\r\n0\r\nLINE\r\n"));
    }

    TEST_CASE("produces a usable database from a file with no tables at all") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "LINE"}, {8, "SomeLayer"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "1"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        Database& db = *result.database;

        // Layer 0 and the referenced layer both exist; real files do reference
        // layers their own table omits.
        CHECK(db.findLayer("0") != nullptr);
        CHECK(db.findLayer("SomeLayer") != nullptr);

        const auto issues = db.validate();
        const bool anyErrors =
            std::any_of(issues.begin(), issues.end(), [](const DatabaseIssue& i) {
                return i.severity == DatabaseIssue::Severity::Error;
            });
        CHECK_FALSE(anyErrors);
    }

    TEST_CASE("warns rather than failing when an insert names an undefined block") {
        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "INSERT"}, {8, "0"}, {2, "NO_SUCH_BLOCK"}, {10, "0"}, {20, "0"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult result = readDxf(text);
        const bool warned = std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(), [](const DxfDiagnostic& d) {
                return d.message.find("NO_SUCH_BLOCK") != std::string::npos;
            });
        CHECK(warned);
        // The drawing still opens.
        CHECK(result.database->entitiesIn(result.database->modelSpace()).size() == 1);
    }
}

TEST_SUITE("Entity reader registry") {
    TEST_CASE("lets a plugin add support for a new entity type") {
        EntityReaderRegistry registry = EntityReaderRegistry::standard();
        const std::size_t before = registry.size();

        // A plugin registers a reader without modifying the reader's control
        // flow — one of the extensibility requirements.
        registry.registerReader("HELIX", [](const DxfRecord& record, ReadContext&) {
            auto entity = std::make_unique<PointEntity>();
            entity->position = record.point(10);
            return entity;
        });
        CHECK(registry.size() == before + 1);

        const std::string text = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "HELIX"}, {8, "0"}, {10, "7"}, {20, "9"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        DxfReader reader{std::move(registry)};
        const DxfReadResult result = reader.read(text);

        const auto points = entitiesOfType<PointEntity>(*result.database);
        REQUIRE(points.size() == 1);
        CHECK(points[0]->position.equals(Vec3{7, 9}));
        // And it is no longer an unsupported placeholder.
        CHECK(entitiesOfType<UnsupportedEntity>(*result.database).empty());
    }
}

TEST_SUITE("Preserving whole structures") {
    /// A record's own handle and owner are structural *once*.
    ///
    /// A SORTENTSTABLE is a run of 331/5 pairs saying which entity draws on top
    /// of which. Skipping the code rather than the first occurrence deleted
    /// every code-5 half of it, and draw order is what keeps a solid fill under
    /// the text it would otherwise hide.
    TEST_CASE("a repeated code 5 is data, not the record's handle") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "OBJECTS"},
            {0, "SORTENTSTABLE"}, {5, "202D"},
            {102, "{ACAD_REACTORS"}, {330, "15D"}, {102, "}"},
            {330, "15D"},
            {100, "AcDbSortentsTable"},
            {330, "1F"},
            {331, "512A"}, {5, "513C"},
            {331, "512E"}, {5, "518A"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        DxfWriter writer;
        const std::string text = writer.write(*read.database);

        const auto occurrences = [&text](std::string_view needle) {
            std::size_t count = 0;
            for (std::size_t at = text.find(needle); at != std::string::npos;
                 at = text.find(needle, at + 1)) {
                ++count;
            }
            return count;
        };

        CHECK(occurrences("513C") == 1);
        CHECK(occurrences("518A") == 1);
        CHECK(occurrences("512A") == 1);
        // The owner, the reactor and the table's subject are three separate
        // 330s and all three have to survive.
        CHECK(occurrences("15D") == 2);
        // The record's own handle is written once by the writer, not twice.
        CHECK(occurrences("202D") == 1);
    }

    TEST_CASE("a reactor list survives a save") {
        // An empty reactor list is how other CAD software is told that nothing uses an
        // object any more, which invites the next PURGE to delete something
        // that is still in the drawing.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "OBJECTS"},
            {0, "IMAGEDEF"}, {5, "DD70"},
            {102, "{ACAD_REACTORS"}, {330, "DD6F"}, {330, "DD78"}, {102, "}"},
            {330, "DD6F"},
            {100, "AcDbRasterImageDef"}, {90, 0}, {1, "scan.png"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        DxfWriter writer;
        const std::string text = writer.write(*read.database);

        CHECK(text.find("{ACAD_REACTORS") != std::string::npos);
        CHECK(text.find("DD78") != std::string::npos);

        // And it still reads back as a reactor rather than as the owner.
        const DxfReadResult again = readDxf(text);
        const DbObject* definition = again.database->getObject(Handle{0xDD70});
        REQUIRE(definition != nullptr);
        CHECK(definition->ownerHandle == Handle{0xDD6F});
    }

    TEST_CASE("a section this reader does not parse is written back verbatim") {
        // CLASSES tells other CAD software how to construct the object types a drawing
        // uses that are not in its base set. Dropping it turns every one of
        // them into a proxy, in a file that opened without complaint.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "CLASSES"},
            {0, "CLASS"}, {1, "ACDBPLACEHOLDER"}, {2, "AcDbPlaceHolder"},
            {3, "ObjectDBX Classes"}, {90, 0}, {91, 1}, {280, 0}, {281, 0},
            {0, "CLASS"}, {1, "IMAGEDEF"}, {2, "AcDbRasterImageDef"},
            {3, "ISM"}, {90, 0}, {91, 1}, {280, 0}, {281, 0},
            {0, "ENDSEC"},
            {0, "SECTION"}, {2, "ENTITIES"}, {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database->preservedSections.size() == 1);
        CHECK(read.database->preservedSections.front().name == "CLASSES");

        DxfWriter writer;
        const std::string text = writer.write(*read.database);
        CHECK(text.find("ACDBPLACEHOLDER") != std::string::npos);
        CHECK(text.find("AcDbRasterImageDef") != std::string::npos);
        CHECK(text.find("ObjectDBX Classes") != std::string::npos);

        // And it comes round again, so a second save does not lose it either.
        const DxfReadResult again = readDxf(text);
        REQUIRE(again.database->preservedSections.size() == 1);
        CHECK(again.database->preservedSections.front().groups.size() ==
              read.database->preservedSections.front().groups.size());
    }

    TEST_CASE("CLASSES lands after the header, where the format puts it") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "HEADER"}, {9, "$ACADVER"}, {1, "AC1032"}, {0, "ENDSEC"},
            {0, "SECTION"}, {2, "CLASSES"},
            {0, "CLASS"}, {1, "IMAGEDEF"}, {2, "AcDbRasterImageDef"}, {90, 0},
            {0, "ENDSEC"},
            {0, "SECTION"}, {2, "ENTITIES"}, {0, "ENDSEC"}, {0, "EOF"},
        });

        DxfWriter writer;
        const std::string text = writer.write(*readDxf(source).database);

        const std::size_t classes = text.find("CLASSES");
        const std::size_t tables = text.find("TABLES");
        REQUIRE(classes != std::string::npos);
        REQUIRE(tables != std::string::npos);
        CHECK(classes < tables);
    }

    TEST_CASE("a symbol table this reader does not model keeps its own header") {
        // The VPORT table is how the model space view is configured. Its
        // records name the TABLE header as their owner, so the header's handle
        // has to come back as it was: a re-issued one would leave every record
        // owned by something that is not there.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "VPORT"}, {5, "8"}, {100, "AcDbSymbolTable"}, {70, 1},
            {0, "VPORT"}, {5, "31"}, {330, "8"},
            {100, "AcDbSymbolTableRecord"}, {100, "AcDbViewportTableRecord"},
            {2, "*Active"}, {70, 0}, {40, "1078.3"},
            {0, "ENDTAB"},
            {0, "TABLE"}, {2, "LAYER"}, {5, "2"}, {100, "AcDbSymbolTable"}, {70, 1},
            {0, "LAYER"}, {5, "10"}, {2, "0"}, {70, 0}, {62, 7}, {6, "Continuous"},
            {0, "ENDTAB"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database->preservedSections.size() == 1);
        CHECK(read.database->preservedSections.front().name == "VPORT");
        CHECK(read.database->preservedSections.front().isTable);

        DxfWriter writer;
        const std::string text = writer.write(*read.database);
        CHECK(text.find("*Active") != std::string::npos);
        CHECK(text.find("AcDbViewportTableRecord") != std::string::npos);
        CHECK(text.find("1078.3") != std::string::npos);
        // The preserved table sits inside TABLES, not loose in the file.
        const std::size_t tables = text.find("\r\nTABLES\r\n");
        const std::size_t vport = text.find("\r\nVPORT\r\n");
        REQUIRE(tables != std::string::npos);
        REQUIRE(vport != std::string::npos);
        CHECK(tables < vport);
    }

    TEST_CASE("extended data on a layer survives a save") {
        // XData used to be copied only in the entity path, so a layer's — a
        // layer-state manager's settings, an estimating add-in's codes — was
        // read and then dropped. It belongs to every kind of record.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "LAYER"}, {5, "2"}, {100, "AcDbSymbolTable"}, {70, 1},
            {0, "LAYER"}, {5, "10"}, {2, "Walls"}, {70, 0}, {62, 7}, {6, "Continuous"},
            {1001, "ACME_ESTIMATOR"}, {1000, "unit-cost"}, {1040, "12.5"},
            {0, "ENDTAB"}, {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        const Layer* layer = read.database->findLayer("Walls");
        REQUIRE(layer != nullptr);
        REQUIRE(layer->xdata.contains("ACME_ESTIMATOR"));

        DxfWriter writer;
        const std::string text = writer.write(*read.database);
        CHECK(text.find("ACME_ESTIMATOR") != std::string::npos);
        CHECK(text.find("unit-cost") != std::string::npos);

        // And the application is registered, or the XData names an APPID that
        // is not in the file.
        CHECK(text.find("\r\nAPPID\r\n") != std::string::npos);
    }

    TEST_CASE("a block record keeps the list of what references it") {
        // `102 {BLKREFS}` names every INSERT that uses the block. It is
        // other CAD software's answer to "is this block still in use", and an empty one is
        // an invitation to purge something that is.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "BLOCK_RECORD"}, {5, "1"}, {100, "AcDbSymbolTable"}, {70, 1},
            {0, "BLOCK_RECORD"}, {5, "1F"}, {330, "1"},
            {100, "AcDbSymbolTableRecord"}, {100, "AcDbBlockTableRecord"},
            {2, "Detail"},
            {102, "{BLKREFS"}, {331, "5A1"}, {331, "5A2"}, {102, "}"},
            {340, "0"}, {70, 0}, {280, 1}, {281, 1},
            {0, "ENDTAB"}, {0, "ENDSEC"}, {0, "EOF"},
        });

        const DxfReadResult read = readDxf(source);
        DxfWriter writer;
        const std::string text = writer.write(*read.database);

        CHECK(text.find("{BLKREFS") != std::string::npos);
        CHECK(text.find("5A1") != std::string::npos);
        CHECK(text.find("5A2") != std::string::npos);
    }

    TEST_CASE("an entity with no reader keeps its subclass marker") {
        // A SOLID that came back with no `100 AcDbTrace` has told the next
        // reader that its four corner points belong to no subclass at all.
        const std::string source = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "SOLID"}, {5, "A1"}, {100, "AcDbEntity"}, {8, "0"},
            {100, "AcDbTrace"},
            {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"},
            {12, "0"}, {22, "1"}, {13, "1"}, {23, "1"},
            {0, "ENDSEC"}, {0, "EOF"},
        });

        DxfWriter writer;
        const std::string whole = writer.write(*readDxf(source).database);

        // Narrowed to the SOLID's own record: the BLOCK and ENDBLK markers a
        // written file carries have `100 AcDbEntity` of their own, and counting
        // across the file would be counting those.
        const std::size_t start = whole.find("\r\nSOLID\r\n");
        REQUIRE(start != std::string::npos);
        // To the end of the section rather than to the next code 0: the SOLID
        // is on layer "0", so searching for a code-0 line finds the layer name.
        const std::size_t end = whole.find("ENDSEC", start);
        REQUIRE(end != std::string::npos);
        const std::string text = whole.substr(start, end - start);

        const auto occurrences = [&text](std::string_view needle) {
            std::size_t count = 0;
            for (std::size_t at = text.find(needle); at != std::string::npos;
                 at = text.find(needle, at + 1)) {
                ++count;
            }
            return count;
        };
        CHECK(occurrences("AcDbTrace") == 1);
        // And exactly one AcDbEntity: the writer emits it, so the preserved
        // copy must not be emitted alongside.
        CHECK(occurrences("AcDbEntity") == 1);
    }
}

TEST_SUITE("Writing a file other readers accept") {
    /// XData names an application; an unregistered name is a dangling reference.
    ///
    /// ODA's reader rejects one outright -- "Invalid RegApp" -- which stopped a
    /// DWG being produced at all. The table used to be rebuilt from the XData
    /// found on *entities*, so a registration used only by a block record, a
    /// layer or a dictionary object was dropped and the file it wrote referred
    /// to an application it had not registered.
    TEST_CASE("an APPID survives even when only a block record uses it") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "APPID"},
            {0, "APPID"}, {5, "A1"}, {2, "ACAD"}, {70, 0},
            {0, "APPID"}, {5, "A2"}, {2, "AcDbDynamicBlockGUID"}, {70, 0},
            {0, "APPID"}, {5, "A3"}, {2, "AcCmTransparency"}, {70, 0},
            {0, "ENDTAB"},
            {0, "ENDSEC"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database != nullptr);

        DxfWriter writer;
        const std::string written = writer.write(*read.database);

        // Every one of them, not merely the ones something was seen to use. A
        // registration this session found no data for is still the drawing's.
        CHECK(written.find("AcDbDynamicBlockGUID") != std::string::npos);
        CHECK(written.find("AcCmTransparency") != std::string::npos);
        CHECK(written.find("ACAD") != std::string::npos);
    }

    /// Extra layouts keep their entities inside their own block.
    ///
    /// Writing every layout's entities into the ENTITIES section round-tripped
    /// perfectly through this reader, which resolves each entity's owner
    /// handle. Other readers take containment instead, so a drawing saved that
    /// way opened elsewhere with every sheet emptied and its contents piled
    /// into model space. Only writing a DWG and reading it back could show it.
    TEST_CASE("a second paper space keeps its entities in its own block") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "BLOCK_RECORD"},
            {0, "BLOCK_RECORD"}, {5, "1F"}, {100, "AcDbBlockTableRecord"},
                {2, "*Model_Space"},
            {0, "BLOCK_RECORD"}, {5, "58"}, {100, "AcDbBlockTableRecord"},
                {2, "*Paper_Space"},
            {0, "BLOCK_RECORD"}, {5, "77"}, {100, "AcDbBlockTableRecord"},
                {2, "*Paper_Space0"},
            {0, "ENDTAB"},
            {0, "ENDSEC"},

            {0, "SECTION"}, {2, "BLOCKS"},
            {0, "BLOCK"}, {5, "60"}, {330, "1F"}, {2, "*Model_Space"}, {70, 0},
                {10, "0"}, {20, "0"}, {3, "*Model_Space"},
            {0, "ENDBLK"}, {5, "61"},
            {0, "BLOCK"}, {5, "62"}, {330, "58"}, {2, "*Paper_Space"}, {70, 0},
                {10, "0"}, {20, "0"}, {3, "*Paper_Space"},
            {0, "ENDBLK"}, {5, "63"},
            {0, "BLOCK"}, {5, "64"}, {330, "77"}, {2, "*Paper_Space0"}, {70, 0},
                {10, "0"}, {20, "0"}, {3, "*Paper_Space0"},
            {0, "LINE"}, {5, "900"}, {330, "77"}, {8, "0"},
                {10, "1"}, {20, "2"}, {11, "3"}, {21, "4"},
            {0, "ENDBLK"}, {5, "65"},
            {0, "ENDSEC"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database != nullptr);

        DxfWriter writer;
        const std::string written = writer.write(*read.database);

        // The line must appear between its own BLOCK header and that block's
        // ENDBLK, not loose in ENTITIES after them.
        const std::size_t block = written.find("*Paper_Space0");
        const std::size_t line = written.find("LINE", block);
        const std::size_t entities = written.find("ENTITIES", block);
        REQUIRE(block != std::string::npos);
        REQUIRE(line != std::string::npos);
        REQUIRE(entities != std::string::npos);
        CHECK(line < entities);

        // And it must still come back where it started.
        const DxfReadResult reread = readDxf(written);
        REQUIRE(reread.database != nullptr);
        const auto* sheet =
            reread.database->get<BlockTableRecord>(reread.database->blocks().find("*Paper_Space0"));
        REQUIRE(sheet != nullptr);
        CHECK(reread.database->entitiesIn(*sheet).size() == 1);
    }
}

TEST_SUITE("Reachability of owned objects") {
    /// An extension dictionary is found through a group in the record's header.
    ///
    /// Written after the record's data instead, the reference is ignored by
    /// readers that expect it where DXF puts it, and a dictionary nothing
    /// appears to reference is discarded together with everything it owns. On
    /// the sample drawing that silently cost 223 dictionaries and the 157
    /// fields, sort tables and dimension associations beneath them -- while
    /// every record count in the file still matched.
    TEST_CASE("an application group is written in the record header") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "ENTITIES"},
            {0, "LINE"}, {5, "2A0"},
            {102, "{ACAD_XDICTIONARY"}, {360, "2A4"}, {102, "}"},
            {330, "1F"},
            {100, "AcDbEntity"}, {8, "0"},
            {100, "AcDbLine"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "1"},
            {0, "ENDSEC"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database != nullptr);

        DxfWriter writer;
        const std::string written = writer.write(*read.database);

        // Anchored on the record itself: "AcDbEntity" also appears in the
        // BLOCK headers written before the ENTITIES section.
        const std::size_t record = written.find("2A0");
        REQUIRE(record != std::string::npos);
        const std::size_t group = written.find("ACAD_XDICTIONARY", record);
        const std::size_t subclass = written.find("AcDbEntity", record);
        REQUIRE(group != std::string::npos);
        REQUIRE(subclass != std::string::npos);
        // Before the subclass marker, which is what "in the header" means.
        CHECK(group < subclass);

        // And exactly once: lifting it into the header must not leave a copy
        // behind in the preserved data.
        std::size_t occurrences = 0;
        for (std::size_t at = written.find("ACAD_XDICTIONARY"); at != std::string::npos;
             at = written.find("ACAD_XDICTIONARY", at + 1)) {
            ++occurrences;
        }
        CHECK(occurrences == 1);
    }

    /// Records name their table as their owner, so the table keeps its handle.
    ///
    /// Allocating a fresh one left every layer, linetype and block record in
    /// the file owned by something that was not in it.
    TEST_CASE("a symbol table keeps the handle it arrived with") {
        const std::string source = dxf({
            {0, "SECTION"}, {2, "TABLES"},
            {0, "TABLE"}, {2, "LAYER"}, {5, "2"}, {100, "AcDbSymbolTable"}, {70, 1},
            {0, "LAYER"}, {5, "10"}, {330, "2"}, {100, "AcDbSymbolTableRecord"},
                {100, "AcDbLayerTableRecord"}, {2, "Walls"}, {70, 0}, {62, 7},
            {0, "ENDTAB"},
            {0, "ENDSEC"},
        });

        const DxfReadResult read = readDxf(source);
        REQUIRE(read.database != nullptr);

        DxfWriter writer;
        const std::string written = writer.write(*read.database);

        const DxfReadResult reread = readDxf(written);
        REQUIRE(reread.database != nullptr);
        const auto found = reread.database->symbolTableHeaders.find("LAYER");
        REQUIRE(found != reread.database->symbolTableHeaders.end());
        CHECK(found->second.handle == Handle{0x2});

        // And the record still names it as its owner in the file.
        const std::size_t at = written.find("Walls");
        REQUIRE(at != std::string::npos);
        const std::size_t owner = written.rfind("330", at);
        REQUIRE(owner != std::string::npos);
        CHECK(written.compare(owner, 8, "330\r\n2\r\n") == 0);
    }
}
