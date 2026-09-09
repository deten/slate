// The drawing read as rooms, systems, shell and title block: what DESCRIBE
// prints and what the assistant is told before it is asked for anything.
#include "test_helpers.hpp"

#include <memory>
#include <string>

#include "cad/agent/AgentProvider.hpp"
#include "cad/agent/DrawingReading.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/math/Angle.hpp"

using namespace cad;

namespace {

[[nodiscard]] bool has(const std::string& text, const char* piece) {
    return text.find(piece) != std::string::npos;
}

/// Two rooms of 400 x 300 inches sharing a wall, a door in the first, a
/// supply duct through both, a pipe, notes that are not rooms, and a sheet
/// with a title block.
struct PlanHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    Layer* walls = nullptr;

    PlanHarness() {
        db->settings.insertionUnits = DrawingUnits::Inches;
        walls = &db->ensureLayer("A-WALL");
        db->ensureLayer("A-DOOR");
        db->ensureLayer("A-ANNO-TEXT");
        db->ensureLayer("M-DUCT-SUPP");
        db->ensureLayer("M-PIPE-CHW");

        // Outer walls, with a 36" gap in the bottom wall of the first room.
        wall({0, 0}, {150, 0});
        wall({186, 0}, {800, 0});
        wall({800, 0}, {800, 300});
        wall({800, 300}, {0, 300});
        wall({0, 300}, {0, 0});
        wall({400, 0}, {400, 300});

        // A door: leaf and swing, which together close the gap.
        auto definition = std::make_unique<BlockTableRecord>();
        definition->name = "DR";
        BlockTableRecord& door = db->addBlock(std::move(definition));
        db->addEntity(std::make_unique<LineEntity>(Vec3{0, 0, 0}, Vec3{0, 36, 0}), door);
        db->addEntity(std::make_unique<ArcEntity>(Vec3{0, 0, 0}, 36.0, 0.0, kPi / 2.0), door);
        auto placed = std::make_unique<InsertEntity>(door.handle, Vec3{150, 0, 0});
        placed->layerHandle = db->findLayer("A-DOOR")->handle;
        db->addToModelSpace(std::move(placed));

        // Room labels: a two-line note with its number, and a stacked name.
        text("OFFICE\\P394 SF", {200, 170}, "A-ANNO-TEXT", true);
        text("101", {200, 140}, "A-ANNO-TEXT");
        text("STAFF", {600, 160}, "A-ANNO-TEXT");
        text("WORK", {600, 146}, "A-ANNO-TEXT");
        text("102", {600, 126}, "A-ANNO-TEXT");
        // A label with no walls around it.
        text("LOBBY", {1000, 150}, "A-ANNO-TEXT");

        // What is not a room: units, tags, a sentence.
        for (int i = 0; i < 6; ++i) {
            text("CFM", {60.0 + 100.0 * i, 40}, "M-DUCT-SUPP");
        }
        for (int i = 0; i < 3; ++i) {
            text("T", {60.0 + 100.0 * i, 20}, "M-DUCT-SUPP");
        }
        text("Louver 92\"h x 94\"w", {500, 60}, "A-ANNO-TEXT");

        // A supply run through both rooms, two diffusers, a pipe.
        const Vec3 run[] = {{50, 250, 0}, {750, 250, 0}};
        auto duct = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(run));
        duct->constantWidth = 12.0;
        duct->layerHandle = db->findLayer("M-DUCT-SUPP")->handle;
        db->addToModelSpace(std::move(duct));
        auto diffuser = std::make_unique<BlockTableRecord>();
        diffuser->name = "DIFF";
        BlockTableRecord& grille = db->addBlock(std::move(diffuser));
        db->addEntity(std::make_unique<CircleEntity>(Vec3{}, 6.0), grille);
        for (const double x : {200.0, 600.0}) {
            auto insert = std::make_unique<InsertEntity>(grille.handle, Vec3{x, 250, 0});
            insert->layerHandle = db->findLayer("M-DUCT-SUPP")->handle;
            db->addToModelSpace(std::move(insert));
        }
        for (const double y : {20.0, 24.0}) {
            auto pipe = std::make_unique<LineEntity>(Vec3{420, y, 0}, Vec3{780, y, 0});
            pipe->layerHandle = db->findLayer("M-PIPE-CHW")->handle;
            db->addToModelSpace(std::move(pipe));
        }

        // A sheet with a title block.
        db->ensureModelLayout();
        BlockTableRecord* paper = db->findBlock(kPaperSpace);
        REQUIRE(paper != nullptr);
        auto layout = std::make_unique<Layout>();
        layout->name = "M-1";
        layout->blockRecordHandle = paper->handle;
        layout->tabOrder = 1;
        db->add(std::move(layout));
        auto frame = std::make_unique<BlockTableRecord>();
        frame->name = "TB";
        BlockTableRecord& titleBlock = db->addBlock(std::move(frame));
        auto reference = std::make_unique<InsertEntity>(titleBlock.handle, Vec3{});
        InsertEntity& title = static_cast<InsertEntity&>(db->addEntity(std::move(reference), *paper));
        for (const auto& [tag, value] : {std::pair{"SHEET", "M-1"}, std::pair{"TITLE", "MECHANICAL PLAN"},
                                         std::pair{"REV", ""}}) {
            auto attribute = std::make_unique<AttributeEntity>();
            attribute->tag = tag;
            attribute->contents = value;
            title.attributeHandles.push_back(db->addEntity(std::move(attribute), *paper).handle);
        }
    }

    void wall(const Vec3& from, const Vec3& to) {
        auto line = std::make_unique<LineEntity>(from, to);
        line->layerHandle = walls->handle;
        db->addToModelSpace(std::move(line));
    }

    void text(const std::string& what, const Vec3& where, const char* layer, bool multi = false) {
        std::unique_ptr<Entity> entity;
        if (multi) {
            entity = std::make_unique<MTextEntity>(where, what, 10.0);
        } else {
            entity = std::make_unique<TextEntity>(where, what, 10.0);
        }
        entity->layerHandle = db->findLayer(layer)->handle;
        db->addToModelSpace(std::move(entity));
    }
};

}  // namespace

TEST_CASE("layer names say which system a layer holds") {
    CHECK(systemKindOf("HSDUCT") == "supply duct");
    CHECK(systemKindOf("HRDUCT") == "return duct");
    CHECK(systemKindOf("M-DUCT-SUPP") == "supply duct");
    CHECK(systemKindOf("M-DUCT-RETN") == "return duct");
    CHECK(systemKindOf("MDUCT-D") == "duct");
    CHECK(systemKindOf("M-PIPE-CHW") == "pipe");
    CHECK(systemKindOf("MEQUIP-D") == "equipment");
    CHECK(systemKindOf("HSFITT") == "fittings");
    CHECK(systemKindOf("A-WALL").empty());
    CHECK(systemKindOf("mech-exist").empty());

    CHECK(systemStatusOf("MDUCT-D") == "demolition");
    CHECK(systemStatusOf("HSDUCT-OLD") == "existing");
    CHECK(systemStatusOf("mech-exist") == "existing");
    CHECK(systemStatusOf("A-WALL-STUD-N") == "new");
    CHECK(systemStatusOf("HSDUCT").empty());
}

TEST_CASE("a room name is capitals, not a tag, a unit or a sentence") {
    CHECK(looksLikeRoomName("OFFICE"));
    CHECK(looksLikeRoomName("CONF. #1"));
    CHECK(looksLikeRoomName("STAFF WORK AREA"));
    CHECK(looksLikeRoomName("MECH. EQT. RM"));
    CHECK_FALSE(looksLikeRoomName("T"));
    CHECK_FALSE(looksLikeRoomName("CFM"));
    CHECK_FALSE(looksLikeRoomName("150"));
    CHECK_FALSE(looksLikeRoomName("AHU-1"));
    CHECK_FALSE(looksLikeRoomName("24x12"));
    CHECK_FALSE(looksLikeRoomName("Louver 92\"h x 94\"w"));
}

TEST_CASE("a plan reads as rooms with walls, systems, shell and title block") {
    PlanHarness plan;
    const DrawingReading reading = readDrawing(*plan.db);

    REQUIRE(reading.rooms.size() == 3);
    const RoomReading& office = reading.rooms[0];
    CHECK(office.name == "OFFICE");
    CHECK(office.number == "101");
    CHECK(office.bounded);
    // The room less the door swing, which is what the drawn lines enclose.
    CHECK(office.area == doctest::Approx(120000.0 - kPi * 36.0 * 36.0 / 4.0).epsilon(1e-3));
    CHECK(office.bounds.height() == doctest::Approx(300.0));
    CHECK(office.bounds.width() == doctest::Approx(400.0));
    CHECK(office.bounds.center().x == doctest::Approx(200.0));
    // The door closed the gap, so the walls and the door bound the room.
    CHECK(office.boundaryHandles.size() >= 5);

    const RoomReading& staff = reading.rooms[1];
    CHECK(staff.name == "STAFF WORK");
    CHECK(staff.number == "102");
    CHECK(staff.bounded);
    CHECK(staff.bounds.center().x == doctest::Approx(600.0));

    const RoomReading& lobby = reading.rooms[2];
    CHECK(lobby.name == "LOBBY");
    CHECK_FALSE(lobby.bounded);

    REQUIRE(reading.systems.size() == 2);
    const auto systemOn = [&](const char* layer) -> const SystemReading& {
        for (const SystemReading& system : reading.systems) {
            if (system.layer == layer) {
                return system;
            }
        }
        FAIL("no system on " << layer);
        return reading.systems.front();
    };
    const SystemReading& duct = systemOn("M-DUCT-SUPP");
    CHECK(duct.kind == "supply duct");
    REQUIRE(duct.runs.size() == 1);
    CHECK(duct.runs[0].width == doctest::Approx(12.0));
    CHECK(duct.runs[0].length == doctest::Approx(700.0));
    CHECK(duct.blocks.at("DIFF") == 2);
    CHECK(duct.labels.at("CFM") == 6);
    CHECK(systemOn("M-PIPE-CHW").lines == 2);

    CHECK(reading.architecture.wallLayers.at("A-WALL") == 6);
    CHECK(reading.architecture.doors == 1);

    REQUIRE(reading.titleBlocks.size() == 1);
    CHECK(reading.titleBlocks[0].sheet == "M-1");
    CHECK(reading.titleBlocks[0].block == "TB");
    // The empty revision is left out.
    CHECK(reading.titleBlocks[0].fields.size() == 2);

    const std::string text = reading.describe();
    CHECK(has(text, "Rooms (3, 2 with walls traced):"));
    CHECK(has(text, "OFFICE 101: 826 sq ft, 400 x 300, centre 200,150"));
    CHECK(has(text, "STAFF WORK 102: 833 sq ft, 400 x 300, centre 600,150"));
    CHECK(has(text, "LOBBY: label at"));
    CHECK(has(text, "not enclosed"));
    CHECK_FALSE(has(text, "  CFM"));
    CHECK_FALSE(has(text, "Louver"));
    CHECK(has(text, "Shell: walls on A-WALL (6 lines); 1 door (DR x1)"));
    CHECK(has(text, "M-DUCT-SUPP (supply duct): 1 run of width 12, total length 700; blocks DIFF x2; labels CFM x6, T x3"));
    CHECK(has(text, "M-PIPE-CHW (pipe): 2 lines"));
    CHECK(has(text, "Title block on sheet \"M-1\" (block TB): SHEET = M-1; TITLE = MECHANICAL PLAN"));
}

TEST_CASE("with no time to trace, rooms are reported by their labels") {
    PlanHarness plan;
    DrawingReadingOptions options;
    options.budget = std::chrono::milliseconds{0};
    const DrawingReading reading = readDrawing(*plan.db, options);

    CHECK_FALSE(reading.boundariesTraced);
    REQUIRE(reading.rooms.size() == 3);
    for (const RoomReading& room : reading.rooms) {
        CHECK_FALSE(room.bounded);
    }
    CHECK(has(reading.describe(), "Rooms (3, walls not traced):"));
}

TEST_CASE("an empty drawing has nothing to describe") {
    const std::unique_ptr<Database> db = Database::createEmpty();
    const DrawingReading reading = readDrawing(*db);
    CHECK(reading.empty());
    CHECK(reading.describe().empty());
}

TEST_CASE("the assistant's digest carries the reading") {
    PlanHarness plan;
    const std::string text = describeDrawing(*plan.db);
    CHECK(has(text, "What the drawing shows:"));
    CHECK(has(text, "OFFICE 101: 826 sq ft"));
}

TEST_CASE("a zone tag is a word and a number in the usual shapes") {
    std::string name;
    CHECK(looksLikeZoneName("ZONE 3", &name));
    CHECK(name == "ZONE 3");
    CHECK(looksLikeZoneName("Zone-12", &name));
    CHECK(name == "ZONE 12");
    CHECK(looksLikeZoneName("Z-3", &name));
    CHECK(name == "ZONE 3");
    CHECK(looksLikeZoneName("VAV-2", &name));
    CHECK(name == "VAV-2");
    CHECK(looksLikeZoneName("VAV 2A", &name));
    CHECK(name == "VAV-2A");
    CHECK_FALSE(looksLikeZoneName("ZONE"));
    CHECK_FALSE(looksLikeZoneName("3"));
    CHECK_FALSE(looksLikeZoneName("OFFICE"));
    CHECK_FALSE(looksLikeZoneName("ZONING NOTES"));
}

TEST_CASE("symbols, zones and wall spots are read from a plan") {
    PlanHarness plan;
    Database& db = *plan.db;
    // A thermostat drawn as a T in a circle in the office, and a device
    // block in the staff room; a room-number bubble that is not a symbol.
    db.addToModelSpace(std::make_unique<CircleEntity>(Vec3{40, 150, 0}, 8.0));
    plan.text("T", {36, 146}, "M-DUCT-SUPP");
    auto device = std::make_unique<BlockTableRecord>();
    device->name = "T-STAT";
    BlockTableRecord& stat = db.addBlock(std::move(device));
    db.addEntity(std::make_unique<CircleEntity>(Vec3{}, 6.0), stat);
    db.addToModelSpace(std::make_unique<InsertEntity>(stat.handle, Vec3{760, 150, 0}));
    db.addToModelSpace(std::make_unique<CircleEntity>(Vec3{200, 140, 0}, 30.0));
    // Zone 1 is the office, outlined on a zone layer; zone 2 is tagged in
    // the staff room and takes its walls.
    Layer& zones = db.ensureLayer("M-ZONE");
    const Vec3 outline[] = {{-10, -10, 0}, {410, -10, 0}, {410, 310, 0}, {-10, 310, 0}};
    auto loop = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(outline, true));
    loop->layerHandle = zones.handle;
    db.addToModelSpace(std::move(loop));
    plan.text("ZONE 1", {300, 40}, "M-ZONE");
    plan.text("VAV-2", {700, 40}, "M-ZONE");

    const DrawingReading reading = readDrawing(db);

    REQUIRE(reading.symbols.size() == 2);
    CHECK(reading.symbols[0].tag == "T-STAT");
    CHECK(reading.symbols[0].kind == "block");
    CHECK(reading.symbols[1].tag == "T");
    CHECK(reading.symbols[1].kind == "circle");
    CHECK(reading.symbols[1].position.x == doctest::Approx(40.0));

    REQUIRE(reading.zones.size() == 2);
    const ZoneReading& one = reading.zones[0];
    CHECK(one.name == "VAV-2");
    CHECK(one.bounded);
    REQUIRE(one.rooms.size() == 1);
    CHECK(one.rooms[0] == "STAFF WORK 102");
    CHECK(one.symbols.at("T-STAT") == 1);
    const ZoneReading& two = reading.zones[1];
    CHECK(two.name == "ZONE 1");
    CHECK(two.bounded);
    CHECK(two.bounds.width() == doctest::Approx(420.0));
    REQUIRE(two.rooms.size() == 1);
    CHECK(two.rooms[0] == "OFFICE 101");
    CHECK(two.symbols.at("T") == 1);

    // The room-number bubble did not become a symbol, and the number still
    // belongs to its room.
    CHECK(reading.rooms[0].number == "101");

    const std::string text = reading.describe();
    CHECK(has(text, "Symbols (letters in circles, device blocks): T x1, T-STAT x1"));
    CHECK(has(text, "ZONE 1: 420 x 320, centre 200,150, rooms OFFICE 101, symbols T x1"));
    CHECK(has(text, "VAV-2:"));

    // Wall spots in the office: on a wall, clear of the door, best nearest
    // the door.
    const std::vector<SpotReading> spots = candidateSpots(db, reading, reading.rooms[0]);
    REQUIRE(!spots.empty());
    CHECK(spots.size() <= 5);
    for (const SpotReading& spot : spots) {
        const bool onWall = std::abs(spot.position.x) < 1e-6 || std::abs(spot.position.x - 400.0) < 1e-6 ||
                            std::abs(spot.position.y) < 1e-6 || std::abs(spot.position.y - 300.0) < 1e-6;
        CHECK(onWall);
        // Not in the door opening, x 150 to 186 on the bottom wall.
        const bool inTheDoor = std::abs(spot.position.y) < 1e-6 && spot.position.x > 140.0 &&
                               spot.position.x < 196.0;
        CHECK_FALSE(inTheDoor);
        CHECK(has(spot.reason, "wall"));
    }
    CHECK(spots[0].cost <= spots.back().cost);
}

TEST_CASE("rooms and walls inside a resolved reference are read where it is placed") {
    // The host holds only a duct; the building comes in as an xref placed
    // 1000 units up, with its own prefixed layers.
    std::unique_ptr<Database> db = Database::createEmpty();
    db->settings.insertionUnits = DrawingUnits::Inches;
    Layer& wallLayer = db->ensureLayer("XM|A-WALL-EXT");
    Layer& textLayer = db->ensureLayer("XM|A-ANNO-TEXT");

    auto definition = std::make_unique<BlockTableRecord>();
    definition->name = "XM";
    definition->xrefType = XrefType::Attachment;
    definition->xrefStatus = XrefStatus::Resolved;
    BlockTableRecord& xref = db->addBlock(std::move(definition));
    const Vec3 corners[] = {{0, 0, 0}, {400, 0, 0}, {400, 300, 0}, {0, 300, 0}};
    for (int i = 0; i < 4; ++i) {
        auto line = std::make_unique<LineEntity>(corners[i], corners[(i + 1) % 4]);
        line->layerHandle = wallLayer.handle;
        db->addEntity(std::move(line), xref);
    }
    auto name = std::make_unique<TextEntity>(Vec3{200, 160, 0}, "LAB", 10.0);
    name->layerHandle = textLayer.handle;
    db->addEntity(std::move(name), xref);
    auto number = std::make_unique<TextEntity>(Vec3{200, 140, 0}, "201", 10.0);
    number->layerHandle = textLayer.handle;
    db->addEntity(std::move(number), xref);

    db->addToModelSpace(std::make_unique<InsertEntity>(xref.handle, Vec3{0, 1000, 0}));

    const DrawingReading reading = readDrawing(*db);
    REQUIRE(reading.rooms.size() == 1);
    CHECK(reading.rooms[0].name == "LAB");
    CHECK(reading.rooms[0].number == "201");
    CHECK(reading.rooms[0].bounded);
    CHECK(reading.rooms[0].area == doctest::Approx(120000.0));
    CHECK(reading.rooms[0].bounds.center().y == doctest::Approx(1150.0));
    CHECK(reading.architecture.wallLayers.at("A-WALL-EXT") == 4);
    CHECK(has(reading.describe(), "LAB 201: 833 sq ft, 400 x 300, centre 200,1150"));
    // Where the reference sits is recorded, so a sibling sheet can place
    // the same background the same way.
    REQUIRE(reading.references.size() == 1);
    CHECK(reading.references[0].name == "XM");
    CHECK(reading.references[0].resolved);
    CHECK(reading.references[0].position.y == doctest::Approx(1000.0));
    CHECK(has(reading.describe(), "References: XM at 0,1000 scale 1 rotation 0"));

    // An unresolved reference brings nothing: there is nothing to read.
    xref.xrefStatus = XrefStatus::NotFound;
    CHECK(readDrawing(*db).rooms.empty());
}
