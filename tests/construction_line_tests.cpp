// Construction lines: XLINE and RAY as entities, in files, and by command.
#include "test_helpers.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/ConstructionLineEntity.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registryOwner = createStandardRegistry();
    CommandRegistry& registry = *registryOwner;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};

    [[nodiscard]] std::vector<const ConstructionLineEntity*> lines() const {
        std::vector<const ConstructionLineEntity*> out;
        for (const Entity* entity : db->entitiesIn(db->modelSpace())) {
            if (const auto* line = dynamic_cast<const ConstructionLineEntity*>(entity);
                line != nullptr && !line->erased) {
                out.push_back(line);
            }
        }
        return out;
    }
};

[[nodiscard]] InputValue at(double x, double y) { return InputValue::makePoint(Vec3{x, y, 0.0}); }

}  // namespace

TEST_SUITE("Construction lines") {

TEST_CASE("an xline is infinite both ways and a ray one way") {
    const ConstructionLineEntity xline{Vec3{10, 10, 0}, Vec3{1, 0, 0}, true};
    CHECK(xline.objectType() == "Xline");
    CHECK(xline.isUnbounded());
    // Distance is to the infinite line: a point far behind the base is
    // still measured square to it.
    CHECK(xline.distanceToPoint(Vec3{-5000, 13, 0}) == doctest::Approx(3.0));
    CHECK(xline.drawnStart().x < -100000.0);
    CHECK(xline.drawnEnd().x > 100000.0);

    const ConstructionLineEntity ray{Vec3{10, 10, 0}, Vec3{1, 0, 0}, false};
    CHECK(ray.objectType() == "Ray");
    CHECK(ray.drawnStart() == Vec3{10, 10, 0});
    // Behind a ray's start the nearest point is the start itself.
    CHECK(ray.distanceToPoint(Vec3{0, 10, 0}) == doctest::Approx(10.0));
    CHECK(ray.distanceToPoint(Vec3{500, 14, 0}) == doctest::Approx(4.0));

    // A direction given unnormalised is kept a unit vector.
    const ConstructionLineEntity slanted{Vec3{}, Vec3{3, 4, 0}, true};
    CHECK(slanted.direction.length() == doctest::Approx(1.0));
    CHECK(slanted.direction.x == doctest::Approx(0.6));
}

TEST_CASE("construction lines do not count towards the drawing's extents") {
    Harness h;
    h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0, 0, 0}, Vec3{100, 50, 0}));
    h.db->addToModelSpace(std::make_unique<ConstructionLineEntity>(Vec3{20, 20, 0}, Vec3{1, 1, 0}, true));
    h.db->addToModelSpace(std::make_unique<ConstructionLineEntity>(Vec3{20, 20, 0}, Vec3{0, 1, 0}, false));
    const Box3 extents = h.db->modelExtents();
    CHECK(extents.min.x == doctest::Approx(0.0));
    CHECK(extents.max.x == doctest::Approx(100.0));
    CHECK(extents.max.y == doctest::Approx(50.0));
}

TEST_CASE("moving and turning by grips, and transforming, keep the line a line") {
    ConstructionLineEntity line{Vec3{0, 0, 0}, Vec3{1, 0, 0}, true};
    const std::vector<GripPoint> grips = line.gripPoints();
    REQUIRE(grips.size() == 2);
    line.moveGrip(grips[0], Vec3{5, 5, 0});
    CHECK(line.base == Vec3{5, 5, 0});
    line.moveGrip(grips[1], Vec3{5, 50, 0});
    CHECK(line.direction.y == doctest::Approx(1.0));
    CHECK(line.direction.x == doctest::Approx(0.0));

    line.transformBy(Mat4::rotationZ(kPi / 2.0, Vec3{}));
    CHECK(line.direction.x == doctest::Approx(-1.0));
    CHECK(line.direction.length() == doctest::Approx(1.0));
    CHECK(line.base.x == doctest::Approx(-5.0));
}

TEST_CASE("XLINE and RAY survive the trip through a DXF") {
    Harness h;
    h.db->addToModelSpace(std::make_unique<ConstructionLineEntity>(Vec3{1, 2, 0}, Vec3{0, 1, 0}, true));
    h.db->addToModelSpace(std::make_unique<ConstructionLineEntity>(Vec3{3, 4, 0}, Vec3{1, 1, 0}, false));
    io::DxfWriter writer;
    const std::string text = writer.write(*h.db);
    // Line endings are the file's business; the record names are what matter.
    const bool hasXline = text.find("\nXLINE\r") != std::string::npos ||
                          text.find("\nXLINE\n") != std::string::npos;
    const bool hasRay = text.find("\nRAY\r") != std::string::npos ||
                        text.find("\nRAY\n") != std::string::npos;
    CHECK(hasXline);
    CHECK(hasRay);
    CHECK(text.find("AcDbXline") != std::string::npos);
    CHECK(text.find("AcDbRay") != std::string::npos);

    io::DxfReadResult result = io::readDxf(text);
    REQUIRE(result.database != nullptr);
    Database& back = *result.database;
    std::vector<const ConstructionLineEntity*> lines;
    for (const Entity* entity : back.entitiesIn(back.modelSpace())) {
        if (const auto* line = dynamic_cast<const ConstructionLineEntity*>(entity)) {
            lines.push_back(line);
        }
    }
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]->bothWays);
    CHECK(lines[0]->base == Vec3{1, 2, 0});
    CHECK(lines[0]->direction.y == doctest::Approx(1.0));
    CHECK_FALSE(lines[1]->bothWays);
    CHECK(lines[1]->base == Vec3{3, 4, 0});
    CHECK(lines[1]->direction.x == doctest::Approx(std::sqrt(0.5)));
}

TEST_CASE("XLINE lays a line through a point and each further point until Enter") {
    Harness h;
    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(100.0, 0.0));
    h.executor.supply(at(0.0, 100.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message == "2 construction lines drawn");
    const auto lines = h.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]->direction.x == doctest::Approx(1.0));
    CHECK(lines[1]->direction.y == doctest::Approx(1.0));
    CHECK(lines[0]->bothWays);
    CHECK(h.transactions.undo());
    CHECK(h.lines().empty());
}

TEST_CASE("XLINE's options: horizontal, vertical, at an angle, bisecting, offset") {
    Harness h;
    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("H"));
    h.executor.supply(at(3.0, 7.0));
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    REQUIRE(h.lines().size() == 1);
    CHECK(h.lines()[0]->base == Vec3{3, 7, 0});
    CHECK(h.lines()[0]->direction.x == doctest::Approx(1.0));

    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("V"));
    h.executor.supply(at(3.0, 7.0));
    h.executor.supply(InputValue::makeEmpty());
    REQUIRE(h.lines().size() == 2);
    CHECK(h.lines()[1]->direction.y == doctest::Approx(1.0));

    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("A"));
    h.executor.supply(InputValue::makeNumber(kPi / 4.0));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(InputValue::makeEmpty());
    REQUIRE(h.lines().size() == 3);
    CHECK(h.lines()[2]->direction.x == doctest::Approx(std::sqrt(0.5)));

    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("B"));
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(10.0, 0.0));
    h.executor.supply(at(0.0, 10.0));
    h.executor.supply(InputValue::makeEmpty());
    REQUIRE(h.lines().size() == 4);
    CHECK(h.lines()[3]->direction.x == doctest::Approx(std::sqrt(0.5)));
    CHECK(h.lines()[3]->direction.y == doctest::Approx(std::sqrt(0.5)));

    const Handle guide =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0, 0, 0}, Vec3{100, 0, 0})).handle;
    REQUIRE(h.executor.execute("XLINE").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("O"));
    h.executor.supply(InputValue::makeNumber(25.0));
    h.executor.supply(InputValue::makeEntity(guide));
    h.executor.supply(at(50.0, -10.0));
    h.executor.supply(InputValue::makeEmpty());
    REQUIRE(h.lines().size() == 5);
    CHECK(h.lines()[4]->base.y == doctest::Approx(-25.0));
    CHECK(h.lines()[4]->direction.x == doctest::Approx(1.0));
}

TEST_CASE("RAY runs from its start through each further point") {
    Harness h;
    REQUIRE(h.executor.execute("RAY").status == CommandOutcome::Status::Running);
    h.executor.supply(at(10.0, 10.0));
    h.executor.supply(at(10.0, 60.0));
    h.executor.supply(at(60.0, 10.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(outcome.message == "2 rays drawn");
    const auto lines = h.lines();
    REQUIRE(lines.size() == 2);
    CHECK_FALSE(lines[0]->bothWays);
    CHECK(lines[0]->base == Vec3{10, 10, 0});
    CHECK(lines[0]->direction.y == doctest::Approx(1.0));
    CHECK(lines[1]->direction.x == doctest::Approx(1.0));
}

}  // TEST_SUITE
