#include "test_helpers.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "cad/db/MlineEntity.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

/// An L-shaped two-element multiline: a 20-wide wall turning a right angle.
///
/// The offsets are the ones a real file carries -- measured along the miter
/// direction, so the corner's are larger than the ends' by root two. Getting
/// that wrong draws a wall that changes thickness at every corner.
MlineEntity lWall() {
    const double d = 10.0;
    const double r2 = std::sqrt(2.0);

    MlineEntity mline;
    mline.elementCount = 2;
    mline.startPoint = Vec3{0.0, 0.0, 0.0};
    mline.vertices = {
        {Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, {{d}, {-d}}, {}},
        {Vec3{400.0, 0.0, 0.0},
         Vec3{0.0, 1.0, 0.0},
         Vec3{-1.0 / r2, 1.0 / r2, 0.0},
         {{d * r2}, {-d * r2}},
         {}},
        {Vec3{400.0, 300.0, 0.0}, Vec3{0.0, 1.0, 0.0}, Vec3{-1.0, 0.0, 0.0}, {{d}, {-d}}, {}},
    };
    return mline;
}

[[nodiscard]] const MlineEntity* onlyMline(const Database& db) {
    for (const Entity* entity : db.entitiesIn(*db.get<BlockTableRecord>(
             db.blocks().find(kModelSpace)))) {
        if (const auto* found = dynamic_cast<const MlineEntity*>(entity)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("a multiline draws one line per element, offset along the miter") {
    const std::vector<std::vector<Vec3>> lines = lWall().elementLines();
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].size() == 3);
    REQUIRE(lines[1].size() == 3);

    // The outer element runs 10 above the spine, and turns the corner at the
    // point where its two straight runs actually meet -- (390, 10), not (400,
    // 10). That is what the larger corner offset buys.
    CHECK_POINT(lines[0][0], (Vec3{0.0, 10.0, 0.0}));
    CHECK_POINT(lines[0][1], (Vec3{390.0, 10.0, 0.0}));
    CHECK_POINT(lines[0][2], (Vec3{390.0, 300.0, 0.0}));

    CHECK_POINT(lines[1][0], (Vec3{0.0, -10.0, 0.0}));
    CHECK_POINT(lines[1][1], (Vec3{410.0, -10.0, 0.0}));
    CHECK_POINT(lines[1][2], (Vec3{410.0, 300.0, 0.0}));
}

TEST_CASE("a multiline is picked and bounded by the lines it draws") {
    const MlineEntity wall = lWall();
    const Box3 box = wall.bounds();
    CHECK(box.min.x == doctest::Approx(0.0));
    CHECK(box.max.x == doctest::Approx(410.0));
    CHECK(box.min.y == doctest::Approx(-10.0));

    // Four segments: two runs per element.
    CHECK(wall.toCurves().size() == 4);
    // On an element line, not on the spine between them.
    CHECK(wall.distanceToPoint(Vec3{200.0, 10.0, 0.0}) == doctest::Approx(0.0));
    CHECK(wall.distanceToPoint(Vec3{200.0, 0.0, 0.0}) == doctest::Approx(10.0));
}

TEST_CASE("a closed multiline joins its last element point back to its first") {
    MlineEntity wall = lWall();
    wall.closed = true;
    const std::vector<std::vector<Vec3>> lines = wall.elementLines();
    REQUIRE(lines[0].size() == 4);
    CHECK_POINT(lines[0].back(), lines[0].front());
}

TEST_CASE("scaling a multiline scales its thickness as well as its length") {
    MlineEntity wall = lWall();
    wall.transformBy(Mat4::scaling(2.0));
    const std::vector<std::vector<Vec3>> lines = wall.elementLines();

    // A wall scaled up that kept its original thickness would not be a wall.
    CHECK_POINT(lines[0][0], (Vec3{0.0, 20.0, 0.0}));
    CHECK_POINT(lines[0][1], (Vec3{780.0, 20.0, 0.0}));
    CHECK(wall.scale == doctest::Approx(2.0));
}

TEST_CASE("moving a multiline leaves its thickness alone") {
    MlineEntity wall = lWall();
    wall.transformBy(Mat4::translation(Vec3{100.0, 50.0, 0.0}));
    const std::vector<std::vector<Vec3>> lines = wall.elementLines();
    CHECK_POINT(lines[0][0], (Vec3{100.0, 60.0, 0.0}));
    CHECK_POINT(lines[1][0], (Vec3{100.0, 40.0, 0.0}));
    CHECK(wall.scale == doctest::Approx(1.0));
}

TEST_CASE("a multiline survives a DXF round-trip") {
    auto db = Database::createEmpty();
    auto wall = std::make_unique<MlineEntity>(lWall());
    wall->layerHandle = db->defaultLayer().handle;
    db->addToModelSpace(std::move(wall));

    const std::string text = io::writeDxf(*db);
    const io::DxfReadResult result = io::readDxf(text);
    REQUIRE(result.database != nullptr);
    REQUIRE(result.succeeded());

    const MlineEntity* reloaded = onlyMline(*result.database);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->elementCount == 2);
    CHECK(reloaded->styleName == "STANDARD");
    REQUIRE(reloaded->vertices.size() == 3);

    // The geometry is what matters, and it is rebuilt from the parameters --
    // so comparing the drawn lines checks the vertices, the miters and the
    // per-element offsets in the one assertion that would notice any of them
    // being lost or reordered.
    const std::vector<std::vector<Vec3>> before = lWall().elementLines();
    const std::vector<std::vector<Vec3>> after = reloaded->elementLines();
    REQUIRE(after.size() == before.size());
    for (std::size_t element = 0; element < before.size(); ++element) {
        REQUIRE(after[element].size() == before[element].size());
        for (std::size_t i = 0; i < before[element].size(); ++i) {
            CHECK_POINT(after[element][i], before[element][i]);
        }
    }
}

TEST_CASE("the sample multiline drawing reads as a multiline") {
    // A file written by hand against the DXF specification rather than by
    // other CAD software, because nothing in the samples had an MLINE in it. What it
    // checks is that the sequential per-vertex structure is walked in file
    // order: read by asking for codes by number, every vertex would collapse
    // onto the first one's values.
    std::ifstream file{"samples/mline-walls.dxf", std::ios::binary};
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();

    const io::DxfReadResult result = io::readDxf(contents.str());
    REQUIRE(result.database != nullptr);
    const MlineEntity* wall = onlyMline(*result.database);
    REQUIRE(wall != nullptr);
    REQUIRE(wall->vertices.size() == 3);
    CHECK_POINT(wall->vertices[1].position, (Vec3{400.0, 0.0, 0.0}));
    CHECK(wall->vertices[1].elementOffsets.size() == 2);
}

TEST_CASE("a multiline whose element count disagrees with its parameters is believed about the parameters") {
    // Files like this exist. The offsets are what the geometry comes from, so
    // a count that contradicts them is the part that is wrong.
    MlineEntity wall = lWall();
    wall.elementCount = 5;
    const std::vector<std::vector<Vec3>> lines = wall.elementLines();
    REQUIRE(lines.size() == 5);
    // The three phantom elements draw nothing rather than collapsing onto the
    // spine.
    CHECK(lines[2].empty());
    CHECK(lines[4].empty());
}
