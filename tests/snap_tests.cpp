// Every object snap, one by one, against geometry whose answer is known.
#include "test_helpers.hpp"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/math/Angle.hpp"
#include "cad/snap/ObjectSnap.hpp"

using namespace cad;

namespace {

/// Geometry as the snap engine sees it: each entity's curves, kept alive
/// for the span the candidate holds.
struct Scene {
    std::vector<std::vector<CurvePtr>> owned;
    std::vector<SnapCandidate> candidates;
    std::vector<Vec3> insertions;
    std::vector<std::unique_ptr<Entity>> entities;

    void add(std::unique_ptr<Entity> entity, Handle handle) {
        entity->handle = handle;
        owned.push_back(entity->toCurves());
        SnapCandidate candidate;
        candidate.handle = handle;
        candidate.curves = owned.back();
        // A point's node is its placement, as a block's insertion is.
        if (const auto* point = dynamic_cast<const PointEntity*>(entity.get())) {
            candidate.isNode = true;
            candidate.insertionPoint = &point->position;
        }
        candidates.push_back(candidate);
        entities.push_back(std::move(entity));
    }

    [[nodiscard]] std::optional<SnapPoint> snap(const Vec3& cursor, SnapMode mode,
                                                const Vec3* base = nullptr,
                                                double aperture = 5.0) const {
        SnapQuery query;
        query.cursor = cursor;
        query.aperture = aperture;
        query.modes = toMask(mode);
        query.candidates = candidates;
        query.basePoint = base;
        const SnapEngine engine;
        return engine.findBest(query);
    }
};

[[nodiscard]] std::unique_ptr<Entity> line(double x0, double y0, double x1, double y1) {
    return std::make_unique<LineEntity>(Vec3{x0, y0, 0.0}, Vec3{x1, y1, 0.0});
}

[[nodiscard]] std::unique_ptr<Entity> circle(double cx, double cy, double r) {
    return std::make_unique<CircleEntity>(Vec3{cx, cy, 0.0}, r);
}

}  // namespace

TEST_SUITE("Object snaps") {

TEST_CASE("Endpoint and Midpoint land exactly on the line's points") {
    Scene scene;
    scene.add(line(0, 0, 100, 0), Handle{10});
    const auto end = scene.snap(Vec3{98, 3, 0}, SnapMode::Endpoint);
    REQUIRE(end.has_value());
    CHECK(end->mode == SnapMode::Endpoint);
    CHECK(end->position.equals(Vec3{100, 0, 0}));
    const auto mid = scene.snap(Vec3{52, -2, 0}, SnapMode::Midpoint);
    REQUIRE(mid.has_value());
    CHECK(mid->position.equals(Vec3{50, 0, 0}));
    // Out of the aperture: nothing.
    CHECK_FALSE(scene.snap(Vec3{60, 40, 0}, SnapMode::Endpoint).has_value());
}

TEST_CASE("Center and Quadrant on a circle") {
    Scene scene;
    scene.add(circle(100, 100, 30), Handle{11});
    // The centre is found from anywhere on the circle within the aperture.
    const auto centre = scene.snap(Vec3{131, 99, 0}, SnapMode::Center);
    REQUIRE(centre.has_value());
    CHECK(centre->position.equals(Vec3{100, 100, 0}));
    const auto top = scene.snap(Vec3{102, 128, 0}, SnapMode::Quadrant);
    REQUIRE(top.has_value());
    CHECK(top->position.equals(Vec3{100, 130, 0}));
    const auto left = scene.snap(Vec3{72, 101, 0}, SnapMode::Quadrant);
    REQUIRE(left.has_value());
    CHECK(left->position.equals(Vec3{70, 100, 0}));
}

TEST_CASE("Node on a point, Insertion on a block") {
    Scene scene;
    auto point = std::make_unique<PointEntity>();
    point->position = Vec3{40, 40, 0};
    scene.add(std::move(point), Handle{12});
    const auto node = scene.snap(Vec3{42, 38, 0}, SnapMode::Node);
    REQUIRE(node.has_value());
    CHECK(node->mode == SnapMode::Node);
    CHECK(node->position.equals(Vec3{40, 40, 0}));

    // A block placed at 200,200: its insertion is the snap, not its geometry.
    const std::unique_ptr<Database> db = Database::createEmpty();
    auto definition = std::make_unique<BlockTableRecord>();
    definition->name = "B";
    BlockTableRecord& block = db->addBlock(std::move(definition));
    db->addEntity(line(0, 0, 10, 0), block);
    auto insert = std::make_unique<InsertEntity>(block.handle, Vec3{200, 200, 0});
    InsertEntity& placed = static_cast<InsertEntity&>(db->addToModelSpace(std::move(insert)));
    SnapCandidate candidate;
    candidate.handle = placed.handle;
    const std::vector<CurvePtr> none;
    candidate.curves = none;
    candidate.insertionPoint = &placed.position;
    const std::array<SnapCandidate, 1> candidates{candidate};
    SnapQuery query;
    query.cursor = Vec3{203, 198, 0};
    query.aperture = 5.0;
    query.modes = toMask(SnapMode::Insertion);
    query.candidates = candidates;
    const auto insertion = SnapEngine{}.findBest(query);
    REQUIRE(insertion.has_value());
    CHECK(insertion->mode == SnapMode::Insertion);
    CHECK(insertion->position.equals(Vec3{200, 200, 0}));
}

TEST_CASE("Intersection where two lines cross, Apparent where they would") {
    Scene scene;
    scene.add(line(0, 0, 100, 100), Handle{13});
    scene.add(line(0, 100, 100, 0), Handle{14});
    const auto cross = scene.snap(Vec3{52, 49, 0}, SnapMode::Intersection);
    REQUIRE(cross.has_value());
    CHECK(cross->mode == SnapMode::Intersection);
    CHECK(cross->position.equals(Vec3{50, 50, 0}));

    Scene apart;
    // Two lines that would meet at 100,100 if extended, but stop short.
    apart.add(line(0, 100, 60, 100), Handle{15});
    apart.add(line(100, 0, 100, 60), Handle{16});
    CHECK_FALSE(apart.snap(Vec3{99, 101, 0}, SnapMode::Intersection).has_value());
    const auto apparent = apart.snap(Vec3{99, 101, 0}, SnapMode::ApparentIntersection);
    REQUIRE(apparent.has_value());
    CHECK(apparent->mode == SnapMode::ApparentIntersection);
    CHECK(apparent->position.equals(Vec3{100, 100, 0}));
}

TEST_CASE("Perpendicular and Tangent are measured from the point already placed") {
    Scene scene;
    scene.add(line(0, 0, 100, 0), Handle{17});
    const Vec3 base{30, 40, 0};
    const auto foot = scene.snap(Vec3{31, 2, 0}, SnapMode::Perpendicular, &base);
    REQUIRE(foot.has_value());
    CHECK(foot->mode == SnapMode::Perpendicular);
    CHECK(foot->position.equals(Vec3{30, 0, 0}));

    Scene round;
    round.add(circle(0, 0, 50), Handle{18});
    // From 100,0 the tangent points are at x = 25, y = ±43.3.
    const Vec3 outside{100, 0, 0};
    const auto tangent = round.snap(Vec3{26, 44, 0}, SnapMode::Tangent, &outside);
    REQUIRE(tangent.has_value());
    CHECK(tangent->mode == SnapMode::Tangent);
    CHECK(tangent->position.x == doctest::Approx(25.0));
    CHECK(tangent->position.y == doctest::Approx(50.0 * std::sqrt(0.75)));
    // The line from the base to the snap really is tangent: it meets the
    // radius at a right angle.
    CHECK((tangent->position - outside).dot(tangent->position) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("Nearest stays on the object under the cursor") {
    Scene scene;
    scene.add(line(0, 0, 100, 100), Handle{19});
    const auto nearest = scene.snap(Vec3{52, 48, 0}, SnapMode::Nearest);
    REQUIRE(nearest.has_value());
    CHECK(nearest->position.equals(Vec3{50, 50, 0}));
    // Fourteen units off the line is out of a five-unit aperture.
    CHECK_FALSE(scene.snap(Vec3{60, 40, 0}, SnapMode::Nearest).has_value());
}

TEST_CASE("Extension reaches past an end, along the object's own line") {
    Scene scene;
    scene.add(line(0, 0, 100, 0), Handle{20});
    const auto beyond = scene.snap(Vec3{140, 2, 0}, SnapMode::Extension);
    REQUIRE(beyond.has_value());
    CHECK(beyond->mode == SnapMode::Extension);
    CHECK(beyond->position.equals(Vec3{140, 0, 0}));
    // On the object itself it is Nearest's job, not Extension's.
    CHECK_FALSE(scene.snap(Vec3{60, 2, 0}, SnapMode::Extension).has_value());
}

TEST_CASE("Parallel runs the way a segment runs, through the point already placed") {
    Scene scene;
    // A wall at 30 degrees; a new line starts at 0,100 and should run
    // parallel to it.
    scene.add(line(0, 0, 100, 100 * std::tan(kPi / 6.0)), Handle{21});
    const Vec3 base{0, 100, 0};
    const Vec3 direction{std::cos(kPi / 6.0), std::sin(kPi / 6.0), 0.0};
    // The cursor a little off the parallel line, 80 along it.
    const Vec3 onLine = base + direction * 80.0;
    const Vec3 cursor = onLine + Vec3{-direction.y, direction.x, 0.0} * 2.0;
    const auto parallel = scene.snap(cursor, SnapMode::Parallel, &base);
    REQUIRE(parallel.has_value());
    CHECK(parallel->mode == SnapMode::Parallel);
    CHECK(parallel->position.x == doctest::Approx(onLine.x));
    CHECK(parallel->position.y == doctest::Approx(onLine.y));
    // Far from the parallel line: nothing. Without a base point: nothing.
    CHECK_FALSE(scene.snap(onLine + Vec3{0, 30, 0}, SnapMode::Parallel, &base).has_value());
    CHECK_FALSE(scene.snap(cursor, SnapMode::Parallel).has_value());

    // A circle has no one direction, so it offers no parallel.
    Scene round;
    round.add(circle(0, 0, 50), Handle{22});
    CHECK_FALSE(round.snap(Vec3{60, 100, 0}, SnapMode::Parallel, &base).has_value());
}

TEST_CASE("the nearer snap wins, and a higher priority breaks a tie") {
    Scene scene;
    scene.add(line(0, 0, 100, 0), Handle{23});
    // Endpoint and Midpoint both on: near the end, the end wins; near the
    // middle, the middle.
    SnapQuery query;
    query.cursor = Vec3{97, 1, 0};
    query.aperture = 8.0;
    query.modes = toMask(SnapMode::Endpoint) | toMask(SnapMode::Midpoint) |
                  toMask(SnapMode::Nearest);
    query.candidates = scene.candidates;
    auto best = SnapEngine{}.findBest(query);
    REQUIRE(best.has_value());
    CHECK(best->mode == SnapMode::Endpoint);
    query.cursor = Vec3{51, 1, 0};
    best = SnapEngine{}.findBest(query);
    REQUIRE(best.has_value());
    CHECK(best->mode == SnapMode::Midpoint);
    // Nearest is exactly under the cursor, but a midpoint within reach is
    // what the drafter meant.
    CHECK(best->position.equals(Vec3{50, 0, 0}));
}

}  // TEST_SUITE
