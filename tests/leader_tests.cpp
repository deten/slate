#include "test_helpers.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/Transfer.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/Viewport.hpp"

using namespace cad;

namespace {

/// A style with round numbers, so the assertions below are readable.
DimStyle plainStyle() {
    DimStyle style;
    style.name = "Test";
    style.arrowSize = 3.0;
    style.textGap = 1.0;
    style.textHeight = 2.5;
    return style;
}

LeaderEntity leaderThrough(std::vector<Vec3> path) {
    LeaderEntity leader;
    leader.vertices = std::move(path);
    return leader;
}

}  // namespace

TEST_CASE("a leader draws a segment between each pair of vertices") {
    const LeaderEntity leader = leaderThrough({{0, 0, 0}, {10, 10, 0}, {20, 10, 0}});
    const LeaderGeometry geometry = leader.geometry(plainStyle());

    // Two segments plus the landing.
    REQUIRE(geometry.lines.size() == 3);
    CHECK_POINT(geometry.lines[0].start, (Vec3{0, 0, 0}));
    CHECK_POINT(geometry.lines[1].end, (Vec3{20, 10, 0}));
}

TEST_CASE("the arrowhead sits at the first vertex and points along the first segment") {
    const LeaderEntity leader = leaderThrough({{0, 0, 0}, {10, 0, 0}});
    const LeaderGeometry geometry = leader.geometry(plainStyle());

    REQUIRE(geometry.arrowheads.size() == 1);
    const std::array<Vec3, 3>& arrow = geometry.arrowheads.front();

    // The tip is the arrow point -- the thing being called out.
    CHECK_POINT(arrow[0], (Vec3{0, 0, 0}));
    // The back is one arrow length along the shaft, so the arrow points at the
    // target rather than away from it.
    CHECK(arrow[1].x == doctest::Approx(3.0));
    CHECK(arrow[2].x == doctest::Approx(3.0));
    // And it has width: a degenerate triangle draws as nothing.
    CHECK(arrow[1].y != doctest::Approx(arrow[2].y));
}

TEST_CASE("a leader with no arrowhead produces none") {
    LeaderEntity leader = leaderThrough({{0, 0, 0}, {10, 0, 0}});
    leader.hasArrowhead = false;
    CHECK(leader.geometry(plainStyle()).arrowheads.empty());
}

TEST_CASE("the landing runs the way the leader arrived from") {
    // Arriving from the left, the note hangs on the right.
    const LeaderEntity rightward = leaderThrough({{0, 0, 0}, {10, 10, 0}, {20, 10, 0}});
    const LeaderGeometry right = rightward.geometry(plainStyle());
    CHECK(right.landingToTheRight);
    CHECK(right.landingEnd.x == doctest::Approx(24.0));  // 20 + arrow 3 + gap 1
    CHECK(right.landingEnd.y == doctest::Approx(10.0));

    // Arriving from the right, on the left. Getting this backwards runs every
    // note on a plan back through the geometry it labels.
    const LeaderEntity leftward = leaderThrough({{100, 0, 0}, {90, 10, 0}, {80, 10, 0}});
    const LeaderGeometry left = leftward.geometry(plainStyle());
    CHECK_FALSE(left.landingToTheRight);
    CHECK(left.landingEnd.x == doctest::Approx(76.0));
}

TEST_CASE("a leader with the landing turned off ends at its last vertex") {
    LeaderEntity leader = leaderThrough({{0, 0, 0}, {10, 10, 0}, {20, 10, 0}});
    leader.hasLanding = false;
    const LeaderGeometry geometry = leader.geometry(plainStyle());
    CHECK(geometry.lines.size() == 2);
    CHECK_POINT(geometry.landingEnd, (Vec3{20, 10, 0}));
}

TEST_CASE("a half-drawn leader draws nothing rather than misbehaving") {
    CHECK(leaderThrough({}).geometry(plainStyle()).lines.empty());
    CHECK(leaderThrough({{5, 5, 0}}).geometry(plainStyle()).lines.empty());
    CHECK_POINT(leaderThrough({{5, 5, 0}}).geometry(plainStyle()).landingEnd, (Vec3{5, 5, 0}));
}

TEST_CASE("a leader moves and picks like the geometry it is") {
    LeaderEntity leader = leaderThrough({{0, 0, 0}, {10, 10, 0}});
    CHECK(leader.bounds().equals(Box3::fromCorners(Vec3{0, 0, 0}, Vec3{10, 10, 0})));
    CHECK(leader.distanceToPoint(Vec3{5, 5, 0}) == doctest::Approx(0.0));
    CHECK(leader.gripPoints().size() == 2);

    leader.transformBy(Mat4::translation(Vec3{100, 0, 0}));
    CHECK_POINT(leader.arrowPoint(), (Vec3{100, 0, 0}));

    leader.moveGrip(leader.gripPoints().front(), Vec3{50, 50, 0});
    CHECK_POINT(leader.arrowPoint(), (Vec3{50, 50, 0}));
}

TEST_CASE("a leader Slate made goes out unassociated and re-links from its own data") {
    // other CAD software lays out an associated note by rules Slate does not model: it
    // underlined every note for the width of its text, on the side it believed
    // the text was -- the wrong side for a leader pointing left. So a leader
    // this application made carries no group 340. The link goes in XData, and
    // has to come back from it, or the note is orphaned the first time the
    // file is reopened.
    auto db = Database::createEmpty();
    auto note = std::make_unique<MTextEntity>(Vec3{24, 10, 0}, "TYP.", 2.5);
    note->layerHandle = db->defaultLayer().handle;
    const Handle noteHandle = db->addToModelSpace(std::move(note)).handle;

    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}, {20, 10, 0}};
    leader->layerHandle = db->defaultLayer().handle;
    leader->styleHandle = db->ensureDimStyle("Standard").handle;
    leader->setAnnotation(noteHandle);
    REQUIRE(leader->isSlateManaged());
    db->addToModelSpace(std::move(leader));

    std::string text = io::writeDxf(*db);
    // DXF lines end in CRLF; the substring searches below are line-based.
    std::erase(text, '\r');
    // No association for other CAD software to lay out; a registered application for the
    // XData, or a strict reader rejects the file.
    const std::size_t at = text.find("AcDbLeader");
    REQUIRE(at != std::string::npos);
    const std::string record = text.substr(at, text.find("\n  0\n", at) - at);
    CHECK(record.find("\n340\n") == std::string::npos);
    CHECK(record.find("SLATE") != std::string::npos);
    CHECK(text.find("AcDbRegAppTableRecord") != std::string::npos);

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const LeaderEntity* reloaded = nullptr;
    for (const Entity* entity : back.database->entitiesIn(back.database->modelSpace())) {
        if (const auto* found = dynamic_cast<const LeaderEntity*>(entity)) {
            reloaded = found;
        }
    }
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->isSlateManaged());
    CHECK(reloaded->annotationType == 0);
    const auto* linked = back.database->get<MTextEntity>(reloaded->annotationHandle);
    REQUIRE(linked != nullptr);
    CHECK(linked->contents == "TYP.");
}

TEST_CASE("a leader from somebody else's file keeps its association as it came") {
    // The inert form is for leaders this application made. One read with a
    // group 340 must go back out with it: quietly unlinking a drafter's leader
    // from its note is not a fix for anything.
    auto db = Database::createEmpty();
    auto note = std::make_unique<MTextEntity>(Vec3{24, 10, 0}, "TYP.", 2.5);
    note->layerHandle = db->defaultLayer().handle;
    const Handle noteHandle = db->addToModelSpace(std::move(note)).handle;
    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}};
    leader->layerHandle = db->defaultLayer().handle;
    leader->annotationHandle = noteHandle;   // As a reader sets it: no marker.
    leader->annotationType = 0;
    db->addToModelSpace(std::move(leader));

    std::string text = io::writeDxf(*db);
    std::erase(text, '\r');
    const std::size_t at = text.find("AcDbLeader");
    REQUIRE(at != std::string::npos);
    const std::string record = text.substr(at, text.find("\n  0\n", at) - at);
    CHECK(record.find("\n340\n") != std::string::npos);
    CHECK(record.find("SLATE") == std::string::npos);
}

TEST_CASE("a leader whose file leaves out the hookline flag has no hookline") {
    // The DWG converter omits a group at its default, and 75's default is 0.
    // Read as "on", every leader back from a DWG grew a second landing beyond
    // its last vertex, straight through the note placed just past it.
    auto db = Database::createEmpty();
    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}, {20, 10, 0}, {27.5, 10, 0}};
    leader->layerHandle = db->defaultLayer().handle;
    leader->hasLanding = false;
    db->addToModelSpace(std::move(leader));

    std::string written = io::writeDxf(*db);
    std::erase(written, '\r');
    // Drop the "75 / 0" pair, as the converter does.
    std::string text;
    bool dropped = false;
    std::istringstream lines{written};
    for (std::string line; std::getline(lines, line);) {
        if (line.find_first_not_of(' ') != std::string::npos &&
            line.substr(line.find_first_not_of(' ')) == "75") {
            std::getline(lines, line);   // its value
            dropped = true;
            continue;
        }
        text += line + '\n';
    }
    REQUIRE(dropped);

    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);
    const LeaderEntity* reloaded = nullptr;
    for (const Entity* entity : back.database->entitiesIn(back.database->modelSpace())) {
        if (const auto* found = dynamic_cast<const LeaderEntity*>(entity)) {
            reloaded = found;
        }
    }
    REQUIRE(reloaded != nullptr);
    CHECK_FALSE(reloaded->hasLanding);
    const LeaderGeometry geometry = reloaded->geometry(back.database->dimStyleByHandle(reloaded->styleHandle));
    CHECK(geometry.landingEnd.x == doctest::Approx(27.5));
}

TEST_CASE("a leader always writes which way its landing runs") {
    // Group 211 is how another application knows which side of the landing the
    // note belongs on. Left out, other CAD software assumes +X and lays the note out to
    // the right of the hookline on every leader -- so a leader pointing
    // leftwards had its own note lying back across its landing. Slate drew it
    // correctly and the file did not say so, which is the worst of both.
    auto db = Database::createEmpty();

    const auto add = [&db](const std::vector<Vec3>& path) {
        auto leader = std::make_unique<LeaderEntity>();
        leader->vertices = path;
        leader->layerHandle = db->defaultLayer().handle;
        leader->styleHandle = db->ensureDimStyle("Standard").handle;
        db->addToModelSpace(std::move(leader));
    };
    // Arriving from the left, then from the right.
    add({{0, 0, 0}, {40, 40, 0}, {80, 40, 0}});
    add({{0, 0, 0}, {-40, 40, 0}, {-80, 40, 0}});

    const std::string text = io::writeDxf(*db);
    const io::DxfReadResult back = io::readDxf(text);
    REQUIRE(back.database != nullptr);

    std::vector<double> written;
    for (const Entity* entity : back.database->entitiesIn(back.database->modelSpace())) {
        if (const auto* leader = dynamic_cast<const LeaderEntity*>(entity)) {
            REQUIRE(leader->hasHorizontalDirection);
            written.push_back(leader->horizontalDirection.x);
        }
    }
    REQUIRE(written.size() == 2);
    CHECK(written[0] > 0.0);
    CHECK(written[1] < 0.0);
}

TEST_CASE("the landing direction is known even for a leader that never stated one") {
    LeaderEntity leader;
    leader.vertices = {{0, 0, 0}, {-40, 40, 0}, {-80, 40, 0}};
    CHECK(leader.landingDirection().x < 0.0);

    // A file that did state one is believed over the geometry.
    leader.hasHorizontalDirection = true;
    leader.horizontalDirection = Vec3{1.0, 0.0, 0.0};
    CHECK(leader.landingDirection().x > 0.0);
}

TEST_CASE("a leader and its note survive a DXF round-trip") {
    auto db = Database::createEmpty();

    auto note = std::make_unique<MTextEntity>(Vec3{24, 10, 0}, "TYP.", 2.5);
    note->layerHandle = db->defaultLayer().handle;
    const Handle noteHandle = db->addToModelSpace(std::move(note)).handle;

    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}, {20, 10, 0}};
    leader->layerHandle = db->defaultLayer().handle;
    leader->annotationHandle = noteHandle;
    leader->annotationType = 0;
    leader->styleHandle = db->ensureDimStyle("Standard").handle;
    db->addToModelSpace(std::move(leader));

    const std::string text = io::writeDxf(*db);
    const io::DxfReadResult result = io::readDxf(text);
    REQUIRE(result.database != nullptr);
    REQUIRE(result.succeeded());

    const LeaderEntity* reloaded = nullptr;
    for (const Entity* entity : result.database->entitiesIn(result.database->modelSpace())) {
        if (const auto* found = dynamic_cast<const LeaderEntity*>(entity)) {
            reloaded = found;
        }
    }
    REQUIRE(reloaded != nullptr);
    REQUIRE(reloaded->vertices.size() == 3);
    CHECK_POINT(reloaded->vertices[1], (Vec3{10, 10, 0}));
    CHECK(reloaded->hasArrowhead);
    CHECK(reloaded->annotationType == 0);

    // The note came back, and the leader still names it. A handle that
    // survives writing but not reading leaves a leader pointing at nothing.
    const auto* linked = result.database->get<MTextEntity>(reloaded->annotationHandle);
    REQUIRE(linked != nullptr);
    CHECK(linked->contents == "TYP.");
}

TEST_CASE("a leader reaches the renderer as strokes and a filled arrowhead") {
    // The arrowhead is a filled triangle, not a stroked one, and it is easy for
    // a new entity type to be drawn by the generic curve path instead of by its
    // own -- which loses the arrow and the landing silently.
    auto db = Database::createEmpty();
    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}, {20, 10, 0}};
    leader->layerHandle = db->defaultLayer().handle;
    leader->styleHandle = db->ensureDimStyle("Standard").handle;
    const Handle handle = db->addToModelSpace(std::move(leader)).handle;

    render::SpatialIndex index;
    render::SceneBuilder builder{*db, index};
    builder.reindex(db->modelSpace());

    const render::Viewport viewport{Vec3{10.0, 5.0}, 4.0, 800.0, 800.0};
    const render::Scene scene = builder.build(db->modelSpace(), viewport, {});

    bool hasStrokes = false;
    bool hasFill = false;
    for (const render::DrawItem& item : scene.items) {
        if (item.handle != handle) {
            continue;
        }
        if (item.kind == render::DrawItem::Kind::Geometry) {
            hasStrokes = true;
            // Two segments and the landing.
            CHECK(item.geometry.size() == 3);
        }
        if (item.kind == render::DrawItem::Kind::Filled) {
            hasFill = true;
        }
    }
    CHECK(hasStrokes);
    CHECK(hasFill);
}

TEST_CASE("copying a leader brings its note and its dimension style") {
    auto source = Database::createEmpty();
    DimStyle& arch = source->ensureDimStyle("ARCH");
    arch.arrowSize = 4.0;

    auto note = std::make_unique<MTextEntity>(Vec3{24, 10, 0}, "SEE 4/A5.1", 2.5);
    note->layerHandle = source->defaultLayer().handle;
    const Handle noteHandle = source->addToModelSpace(std::move(note)).handle;

    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}, {20, 10, 0}};
    leader->layerHandle = source->defaultLayer().handle;
    leader->styleHandle = arch.handle;
    leader->annotationHandle = noteHandle;
    leader->annotationType = 0;
    const Handle leaderHandle = source->addToModelSpace(std::move(leader)).handle;

    // Only the leader is selected. Selecting a callout is how a drafter selects
    // the words on it too.
    const ExtractedContent content = extractEntities(*source, {leaderHandle});
    CHECK(content.report.entities.size() == 2);

    auto target = Database::createEmpty();
    TransactionManager transactions{*target};
    const TransferReport report =
        transactions.run("Paste", [&](Transaction& transaction) {
            return pasteContent(*content.database, target->modelSpace(),
                                Mat4::translation(Vec3{100, 0, 0}), transaction);
        });

    const LeaderEntity* pasted = nullptr;
    for (const Handle handle : report.entities) {
        if (const auto* found = target->get<LeaderEntity>(handle)) {
            pasted = found;
        }
    }
    REQUIRE(pasted != nullptr);
    CHECK_POINT(pasted->arrowPoint(), (Vec3{100, 0, 0}));

    // The style came across, under its own name and with its own arrow size.
    const DimStyle* style = target->findDimStyle("ARCH");
    REQUIRE(style != nullptr);
    CHECK(style->arrowSize == doctest::Approx(4.0));
    CHECK(pasted->styleHandle == style->handle);

    // And the note is the *copy*, not the source drawing's handle.
    const auto* linked = target->get<MTextEntity>(pasted->annotationHandle);
    REQUIRE(linked != nullptr);
    CHECK(linked->contents == "SEE 4/A5.1");
    CHECK(linked->handle != noteHandle);
}

TEST_CASE("a leader copied without its note says it has none") {
    auto source = Database::createEmpty();
    auto note = std::make_unique<MTextEntity>(Vec3{24, 10, 0}, "TYP.", 2.5);
    note->layerHandle = source->defaultLayer().handle;
    const Handle noteHandle = source->addToModelSpace(std::move(note)).handle;
    source->eraseObject(noteHandle);

    auto leader = std::make_unique<LeaderEntity>();
    leader->vertices = {{0, 0, 0}, {10, 10, 0}};
    leader->layerHandle = source->defaultLayer().handle;
    leader->annotationHandle = noteHandle;
    leader->annotationType = 0;
    const Handle leaderHandle = source->addToModelSpace(std::move(leader)).handle;

    const ExtractedContent content = extractEntities(*source, {leaderHandle});
    REQUIRE(content.report.entities.size() == 1);

    const auto* copy = content.database->get<LeaderEntity>(content.report.entities.front());
    REQUIRE(copy != nullptr);
    CHECK(copy->annotationHandle.isNull());
    // Not left claiming an MTEXT it does not name.
    CHECK(copy->annotationType == 3);
}

// -------------------------------------------------------------- multileader

namespace {

/// The hand-built sample, read back.
///
/// Written against the DXF specification rather than by other CAD software, because the
/// client drawing carries only the MULTILEADER *class* declaration and no
/// entity. What it exercises is the nesting: code 10 is a leader line vertex, a
/// leader's landing point and the content base point, and code 304 is both the
/// note and the marker that opens a leader line. Read by asking for codes by
/// number, every one of those would be the wrong value.
std::unique_ptr<Database> readSampleMLeader() {
    std::ifstream file{"samples/multileader.dxf", std::ios::binary};
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    io::DxfReadResult result = io::readDxf(contents.str());
    REQUIRE(result.database != nullptr);
    return std::move(result.database);
}

const MLeaderEntity* onlyMLeader(Database& db) {
    for (const Entity* entity : db.entitiesIn(db.modelSpace())) {
        if (const auto* found = dynamic_cast<const MLeaderEntity*>(entity)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a multileader reads both of its arrows out of the nested context data") {
    auto db = readSampleMLeader();
    const MLeaderEntity* mleader = onlyMLeader(*db);
    REQUIRE(mleader != nullptr);
    REQUIRE(mleader->leaders.size() == 2);

    // Each leader line keeps its own vertices, arrow point first.
    CHECK_POINT(mleader->leaders[0].vertices.front(), (Vec3{40.0, 40.0, 0.0}));
    CHECK_POINT(mleader->leaders[1].vertices.front(), (Vec3{40.0, 300.0, 0.0}));
    CHECK(mleader->leaders[0].vertices.size() == 2);
}

TEST_CASE("a multileader's landing is its last point plus the dogleg") {
    auto db = readSampleMLeader();
    const MLeaderEntity* mleader = onlyMLeader(*db);
    REQUIRE(mleader != nullptr);
    REQUIRE(mleader->leaders.size() == 2);
    // Last leader line point (240,200), dogleg vector +X, length 20.
    CHECK(mleader->leaders[0].hasLanding);
    CHECK_POINT(mleader->leaders[0].landingEnd, (Vec3{260.0, 200.0, 0.0}));
}

TEST_CASE("a multileader carries its own note, not a separate MTEXT") {
    auto db = readSampleMLeader();
    const MLeaderEntity* mleader = onlyMLeader(*db);
    REQUIRE(mleader != nullptr);
    CHECK(mleader->text == "SEE DETAIL 4/A5.1");
    CHECK_POINT(mleader->textLocation, (Vec3{260.0, 200.0, 0.0}));
    CHECK(mleader->textHeight == doctest::Approx(5.0));
    // Code 304 at context depth is the note; the same code inside a leader
    // opens a leader line, and reading the marker as the text would put
    // "LEADER_LINE{" on the drawing.
    CHECK(mleader->text.find("LEADER_LINE") == std::string::npos);
}

TEST_CASE("a multileader gets one arrowhead per arrow, pointing at the target") {
    auto db = readSampleMLeader();
    const MLeaderEntity* mleader = onlyMLeader(*db);
    REQUIRE(mleader != nullptr);
    const std::vector<std::array<Vec3, 3>> heads = mleader->arrowheads();
    REQUIRE(heads.size() == 2);
    CHECK_POINT(heads[0][0], (Vec3{40.0, 40.0, 0.0}));
    CHECK_POINT(heads[1][0], (Vec3{40.0, 300.0, 0.0}));
    // Sized from the context data's own arrow size, not the default.
    CHECK(mleader->arrowSize == doctest::Approx(3.0));
}

TEST_CASE("a multileader refuses to be transformed rather than pretending") {
    auto db = readSampleMLeader();
    const MLeaderEntity* found = onlyMLeader(*db);
    REQUIRE(found != nullptr);

    MLeaderEntity mleader;
    mleader.restoreFrom(*found);
    CHECK_FALSE(mleader.canTransform());

    const Vec3 before = mleader.leaders.front().vertices.front();
    mleader.transformBy(Mat4::translation(Vec3{100.0, 100.0, 0.0}));
    // It is written back from bytes a transform cannot reach, so a move that
    // appeared to work would be gone after a save. Refusing is the honest
    // answer; the commands say what they skipped.
    CHECK_POINT(mleader.leaders.front().vertices.front(), before);
}

TEST_CASE("a multileader's context data survives a round-trip byte for byte") {
    auto db = readSampleMLeader();
    const std::string text = io::writeDxf(*db);
    const io::DxfReadResult again = io::readDxf(text);
    REQUIRE(again.database != nullptr);

    const MLeaderEntity* reloaded = onlyMLeader(*again.database);
    REQUIRE(reloaded != nullptr);
    // The proof that nothing was dropped is that everything needed to draw it
    // is still there after a second pass through the writer.
    REQUIRE(reloaded->leaders.size() == 2);
    CHECK(reloaded->text == "SEE DETAIL 4/A5.1");
    CHECK_POINT(reloaded->leaders[1].vertices.front(), (Vec3{40.0, 300.0, 0.0}));
    CHECK(reloaded->arrowSize == doctest::Approx(3.0));
}
