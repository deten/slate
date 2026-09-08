// What other CAD software itself reported after round-tripping a real drawing through
// Slate , each difference pinned here so it stays fixed.
#include "test_helpers.hpp"

#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

/// The group codes of the first record of `type` in DXF text, each code with
/// every value it takes, in order.
std::map<int, std::vector<std::string>> firstRecord(const std::string& dxf, const std::string& type) {
    std::istringstream lines{dxf};
    std::vector<std::string> all;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        all.push_back(line);
    }
    std::map<int, std::vector<std::string>> groups;
    bool inside = false;
    for (std::size_t i = 0; i + 1 < all.size(); i += 2) {
        const std::string code = all[i].substr(all[i].find_first_not_of(' '));
        const std::string& value = all[i + 1];
        if (code == "0") {
            if (inside) {
                break;
            }
            inside = value == type;
            continue;
        }
        if (inside) {
            groups[std::stoi(code)].push_back(value);
        }
    }
    return groups;
}

template <typename T>
const T* firstOf(Database& db) {
    for (const Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (const auto* found = dynamic_cast<const T*>(entity)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("an ellipse keeps its extrusion direction, so a mirrored arc stays the same arc") {
    // Read without its normal, an elliptical arc drawn with -Z came back as the
    // other three quarters of the same ellipse.
    auto db = Database::createEmpty();
    auto ellipse = std::make_unique<EllipseEntity>();
    ellipse->center = {38.0, 75.0, 0.0};
    ellipse->majorAxis = {-618.0, 618.0, 0.0};
    ellipse->ratio = 1.0;
    ellipse->startParam = 0.8357;
    ellipse->endParam = 2.3562;
    ellipse->normal = {0.0, 0.0, -1.0};
    ellipse->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(ellipse));

    const std::string text = io::writeDxf(*db);
    // After the subclass marker, where the DWG converter looks for it; among
    // the common properties it was dropped, and the arc came back mirrored.
    const std::size_t marker = text.find("AcDbEllipse");
    const std::size_t extrusion = text.find("230", marker);
    REQUIRE(marker != std::string::npos);
    CHECK(extrusion != std::string::npos);
    CHECK(text.find("230") >= marker);

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const auto* reloaded = firstOf<EllipseEntity>(*back.database);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->normal.z == doctest::Approx(-1.0));
    CHECK(reloaded->startParam == doctest::Approx(0.8357));
}

TEST_CASE("a polyline keeps its linetype-generation flag") {
    // Bit 128 of group 70 was dropped on write; 118 polylines in one drawing
    // lost it in a single save.
    auto db = Database::createEmpty();
    auto poly = std::make_unique<PolylineEntity>();
    for (const Vec3 corner : {Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}, Vec3{10.0, 5.0, 0.0}}) {
        PolylineVertex vertex;
        vertex.position = corner;
        poly->vertices.push_back(vertex);
    }
    poly->closed = true;
    poly->linetypeGeneration = true;
    poly->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(poly));

    const std::string text = io::writeDxf(*db);
    const auto groups = firstRecord(text, "LWPOLYLINE");
    REQUIRE(groups.count(70) == 1);
    CHECK(std::stoi(groups.at(70).front()) == (1 | 128));

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const auto* reloaded = firstOf<PolylineEntity>(*back.database);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->linetypeGeneration);
    CHECK(reloaded->closed);
}

TEST_CASE("a dimension goes out the way other CAD software writes its own") {
    // Three things other CAD software reported about a Slate-made dimension: bit 32 set
    // for a block the file did not carry; the text midpoint left at the
    // origin, where a trusting reader puts the text; and group 12 dropped.
    auto db = Database::createEmpty();
    auto dimension = std::make_unique<DimensionEntity>();
    dimension->type = DimensionType::Rotated;
    dimension->point13 = {0.0, 0.0, 0.0};
    dimension->point14 = {40.0, 0.0, 0.0};
    dimension->definitionPoint = {40.0, 10.0, 0.0};
    dimension->insertionPoint = {-120.0, -257.0, 0.0};
    dimension->styleHandle = db->ensureDimStyle("Standard").handle;
    dimension->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(dimension));

    const std::string text = io::writeDxf(*db);
    const auto groups = firstRecord(text, "DIMENSION");
    REQUIRE(groups.count(70) == 1);
    CHECK((std::stoi(groups.at(70).front()) & 32) == 0);
    REQUIRE(groups.count(11) == 1);
    REQUIRE(groups.count(21) == 1);
    // On the dimension line, between the extension lines -- not at 0,0.
    CHECK(std::stod(groups.at(11).front()) == doctest::Approx(20.0));
    CHECK(std::stod(groups.at(21).front()) > 9.0);
    REQUIRE(groups.count(12) == 1);
    CHECK(std::stod(groups.at(12).front()) == doctest::Approx(-120.0));

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const auto* reloaded = firstOf<DimensionEntity>(*back.database);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->insertionPoint.y == doctest::Approx(-257.0));
}

TEST_CASE("a plain hatch writes the gradient groups other CAD software expects to find") {
    // Left out, the DWG converter invented a single-colour gradient flag for
    // every solid and pattern fill.
    auto db = Database::createEmpty();
    auto hatch = std::make_unique<HatchEntity>();
    hatch->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(hatch));

    const auto groups = firstRecord(io::writeDxf(*db), "HATCH");
    for (const int code : {450, 451, 452, 453}) {
        REQUIRE(groups.count(code) == 1);
        CHECK(std::stoi(groups.at(code).front()) == 0);
    }
    REQUIRE(groups.count(470) == 1);
    CHECK(groups.at(470).front().empty());
}

TEST_CASE("a hatch's pattern type is read once and written once") {
    // The reader never consumed group 76, so it came back as an unknown group
    // and was echoed after the record, which already said it: 36 hatches in
    // one drawing went out with the code twice.
    auto db = Database::createEmpty();
    auto hatch = std::make_unique<HatchEntity>();
    hatch->patternType = 1;
    hatch->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(hatch));

    const io::DxfReadResult back = io::readDxf(io::writeDxf(*db));
    REQUIRE(back.database != nullptr);
    const auto groups = firstRecord(io::writeDxf(*back.database), "HATCH");
    REQUIRE(groups.count(76) == 1);
    CHECK(groups.at(76).size() == 1);
    CHECK(groups.at(76).front() == "1");
}

TEST_CASE("an entity in paper space says so with group 67") {
    // The owner handle says it too, and this reader and the DWG converter go
    // by the owner. A reader that goes by the code -- as other CAD software's own DXF
    // invites -- put every sheet's contents into model space without it.
    auto db = Database::createEmpty();
    db->ensureModelLayout();
    BlockTableRecord* paper = db->findBlock(kPaperSpace);
    REQUIRE(paper != nullptr);
    auto line = std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0});
    line->layerHandle = db->defaultLayer().handle;
    db->addEntity(std::move(line), *paper);
    auto model = std::make_unique<LineEntity>(Vec3{0.0, 5.0, 0.0}, Vec3{10.0, 5.0, 0.0});
    model->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(model));

    const std::string text = io::writeDxf(*db);
    int flagged = 0, plain = 0;
    std::istringstream lines{text};
    std::vector<std::string> all;
    for (std::string row; std::getline(lines, row);) {
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        all.push_back(row);
    }
    for (std::size_t i = 0; i + 1 < all.size(); i += 2) {
        if (all[i].find_first_not_of(' ') != std::string::npos &&
            all[i].substr(all[i].find_first_not_of(' ')) == "0" && all[i + 1] == "LINE") {
            bool paperFlag = false;
            for (std::size_t j = i + 2; j + 1 < all.size(); j += 2) {
                const std::string code = all[j].substr(all[j].find_first_not_of(' '));
                if (code == "0") {
                    break;
                }
                if (code == "67" && all[j + 1].find('1') != std::string::npos) {
                    paperFlag = true;
                }
            }
            if (paperFlag) {
                ++flagged;
            } else {
                ++plain;
            }
        }
    }
    CHECK(flagged == 1);
    CHECK(plain == 1);

    // And once only, after a read and a second write: a 67 the reader did
    // not consume came back as an unknown group beside the one written.
    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    std::string again = io::writeDxf(*back.database);
    std::erase(again, '\r');
    std::size_t count = 0;
    // The builder may pad a two-digit code; count both spellings.
    for (const char* needle : {"\n 67\n", "\n67\n"}) {
        for (std::size_t at = again.find(needle); at != std::string::npos; at = again.find(needle, at + 1)) {
            ++count;
        }
    }
    CHECK(count == 1);
}

TEST_CASE("a sheet block spelled in capitals still writes its contents as paper space") {
    // A file from elsewhere named its sheet *PAPER_SPACE. Matched by exact
    // spelling it counted as an ordinary block, its contents went inside the
    // block definition, and other readers put them into model space.
    auto db = Database::createEmpty();
    db->ensureModelLayout();
    BlockTableRecord* paper = db->findBlock(kPaperSpace);
    REQUIRE(paper != nullptr);
    paper->name = "*PAPER_SPACE";
    auto line = std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0});
    line->layerHandle = db->defaultLayer().handle;
    db->addEntity(std::move(line), *paper);

    std::string text = io::writeDxf(*db);
    std::erase(text, '\r');
    const std::size_t entities = text.find("\nENTITIES\n");
    const std::size_t lineAt = text.find("\nLINE\n");
    REQUIRE(entities != std::string::npos);
    REQUIRE(lineAt != std::string::npos);
    // In the ENTITIES section, after the BLOCKS section, flagged as paper.
    CHECK(lineAt > entities);
    const std::string record = text.substr(lineAt, text.find("\n  0\n", lineAt + 1) - lineAt);
    CHECK((record.find("\n 67\n1\n") != std::string::npos || record.find("\n67\n1\n") != std::string::npos));
}

TEST_CASE("text symbols and accented letters go out as the escapes other readers expect") {
    // The file declares a legacy code page. Raw UTF-8 in it came back as
    // "Â°"; the degree, diameter and plus-minus signs go out as the %% codes
    // older files use, and anything else outside ASCII as \U+XXXX.
    auto db = Database::createEmpty();
    auto text = std::make_unique<TextEntity>(Vec3{0.0, 0.0, 0.0},
                                             std::string{"45\xC2\xB0 \xE2\x8C\x80" "20 \xC2\xB1" "1 Gr\xC3\xB6\xC3\x9F" "e"}, 2.5);
    db->addToModelSpace(std::move(text));
    auto note = std::make_unique<MTextEntity>(Vec3{0.0, 10.0, 0.0}, std::string{"Temp 78\xC2\xB0 F"}, 2.5);
    db->addToModelSpace(std::move(note));
    Layer& layer = db->ensureLayer("Gr\xC3\xB6\xC3\x9F" "e");
    static_cast<void>(layer);

    std::string written = io::writeDxf(*db);
    std::erase(written, '\r');
    CHECK(written.find("45%%d %%c20 %%p1 Gr\\U+00F6\\U+00DFe") != std::string::npos);
    CHECK(written.find("Temp 78%%d F") != std::string::npos);
    CHECK(written.find("\nGr\\U+00F6\\U+00DFe\n") != std::string::npos);
    // Nothing outside ASCII is left in the file.
    for (const char c : written) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }

    // And it all comes back as it was.
    const io::DxfReadResult back = io::readDxf(written);
    REQUIRE(back.database != nullptr);
    bool sawText = false;
    bool sawNote = false;
    for (const Entity* entity : back.database->entitiesIn(back.database->modelSpace())) {
        if (const auto* line = dynamic_cast<const TextEntity*>(entity)) {
            sawText = line->contents == "45\xC2\xB0 \xE2\x8C\x80" "20 \xC2\xB1" "1 Gr\xC3\xB6\xC3\x9F" "e";
        }
        if (const auto* paragraph = dynamic_cast<const MTextEntity*>(entity)) {
            sawNote = paragraph->contents == "Temp 78\xC2\xB0 F";
        }
    }
    CHECK(sawText);
    CHECK(sawNote);
    CHECK(back.database->findLayer("Gr\xC3\xB6\xC3\x9F" "e") != nullptr);
}

TEST_CASE("a polyline with a wide first segment says every other vertex is thin") {
    // A reader that meets a vertex with no width carries the previous one
    // forward, so leaving zeros out made the whole line wide.
    auto db = Database::createEmpty();
    const Vec3 corners[] = {Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}, Vec3{10.0, 10.0, 0.0},
                            Vec3{0.0, 10.0, 0.0}};
    auto polyline = std::make_unique<PolylineEntity>(
        PolylineEntity::fromPoints(std::span<const Vec3>{corners}, false));
    polyline->vertices[0].startWidth = 0.5;
    polyline->vertices[0].endWidth = 0.5;
    db->addToModelSpace(std::move(polyline));

    const auto groups = firstRecord(io::writeDxf(*db), "LWPOLYLINE");
    REQUIRE(groups.contains(40));
    CHECK(groups.at(40).size() == 4);
    CHECK(groups.at(41).size() == 4);
}

TEST_CASE("an attribute keeps its field length") {
    auto db = Database::createEmpty();
    auto block = std::make_unique<BlockTableRecord>();
    block->name = "TAG";
    BlockTableRecord& record = db->addBlock(std::move(block));
    auto definition = std::make_unique<AttributeDefinitionEntity>();
    definition->tag = "ROOM";
    definition->flags.fieldLength = 25;
    db->addEntity(std::move(definition), record);

    const auto groups = firstRecord(io::writeDxf(*db), "ATTDEF");
    REQUIRE(groups.contains(73));
    CHECK(groups.at(73).front().find("25") != std::string::npos);
}

TEST_CASE("a boundary names the associative hatch it feeds") {
    // other CAD software keeps a hatch's association only when the boundary entity
    // carries a reactor back to it; without one, every associative hatch
    // Slate made opened static (71 read back as 0).
    auto db = Database::createEmpty();
    auto circle = std::make_unique<CircleEntity>(Vec3{200.0, 50.0, 0.0}, 25.0);
    circle->layerHandle = db->defaultLayer().handle;
    const Handle boundary = db->addToModelSpace(std::move(circle)).handle;

    auto hatch = std::make_unique<HatchEntity>();
    hatch->isAssociative = true;
    HatchBoundaryPath path;
    path.sourceHandles.push_back(boundary);
    hatch->paths.push_back(path);
    hatch->layerHandle = db->defaultLayer().handle;
    const Handle fill = db->addToModelSpace(std::move(hatch)).handle;

    const auto groups = firstRecord(io::writeDxf(*db), "CIRCLE");
    REQUIRE(groups.count(102) == 1);
    CHECK(groups.at(102).front() == "{ACAD_REACTORS");
    REQUIRE(groups.count(330) == 1);
    bool named = false;
    for (const std::string& ref : groups.at(330)) {
        named = named || ref == fill.toString();
    }
    CHECK(named);
}

TEST_CASE("a viewport's view direction keeps the length the file gave it") {
    // other CAD software keeps the distance to the target in the vector's length. Read
    // normalised and written back at unit length, eight viewports in one
    // drawing came back with a target distance of 1.
    auto db = Database::createEmpty();
    db->ensureModelLayout();
    BlockTableRecord* paper = db->findBlock(kPaperSpace);
    REQUIRE(paper != nullptr);
    auto viewport = std::make_unique<ViewportEntity>();
    viewport->viewportId = 2;
    viewport->center = {200.0, 150.0, 0.0};
    viewport->width = 200.0;
    viewport->height = 100.0;
    viewport->viewDirection = {0.0, 0.0, 1.0};
    viewport->viewDirectionLength = 1690.78;
    viewport->layerHandle = db->defaultLayer().handle;
    db->addEntity(std::move(viewport), *paper);

    const std::string text = io::writeDxf(*db);
    const auto groups = firstRecord(text, "VIEWPORT");
    REQUIRE(groups.count(36) == 1);
    CHECK(std::stod(groups.at(36).front()) == doctest::Approx(1690.78));

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const BlockTableRecord* paperBack = back.database->findBlock(kPaperSpace);
    REQUIRE(paperBack != nullptr);
    const ViewportEntity* reloaded = nullptr;
    for (const Entity* entity : back.database->entitiesIn(*paperBack)) {
        if (const auto* found = dynamic_cast<const ViewportEntity*>(entity)) {
            reloaded = found;
        }
    }
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->viewDirection.z == doctest::Approx(1.0));
    CHECK(reloaded->viewDirectionLength == doctest::Approx(1690.78));
}
