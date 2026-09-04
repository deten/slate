#include "test_helpers.hpp"

#include <vector>

#include "cad/math/Angle.hpp"
#include "cad/snap/Tracking.hpp"

using namespace cad;

namespace {

/// The four square directions, which is what tracking uses with polar off.
constexpr double kSquare = kPi / 2.0;

}  // namespace

TEST_SUITE("Object snap tracking") {

TEST_CASE("nothing acquired aligns nothing") {
    const std::vector<Vec3> none;
    CHECK_FALSE(alignToAcquired(none, Vec3{10.0, 10.0, 0.0}, kSquare, 5.0).has_value());
}

TEST_CASE("a cursor near a path is taken onto it") {
    // Level with the acquired point, give or take: the whole point of tracking
    // is that "give or take" becomes exact.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}};
    const auto hit = alignToAcquired(acquired, Vec3{450.0, 297.0, 0.0}, kSquare, 5.0);
    REQUIRE(hit.has_value());
    CHECK(hit->point.equals(Vec3{450.0, 300.0, 0.0}));
    // Along the path, not to the point that made it.
    REQUIRE(hit->origins.size() == 1);
    CHECK(hit->origins[0].equals(Vec3{400.0, 300.0, 0.0}));
}

TEST_CASE("a cursor away from every path is left alone") {
    // Otherwise the point would be dragged onto an axis the user never asked
    // for, which is the complaint about over-eager snapping.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}};
    CHECK_FALSE(alignToAcquired(acquired, Vec3{450.0, 250.0, 0.0}, kSquare, 5.0).has_value());
}

TEST_CASE("the acquired point itself offers no direction") {
    // Every path passes through it, so there is nothing to choose between them
    // and no sensible answer to give.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}};
    CHECK_FALSE(alignToAcquired(acquired, Vec3{401.0, 300.5, 0.0}, kSquare, 5.0).has_value());
}

TEST_CASE("two paths crossing beat either one alone") {
    // The case the feature exists for: level with one point and directly above
    // another, where there is no geometry at all.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}, Vec3{600.0, 100.0, 0.0}};
    const auto hit = alignToAcquired(acquired, Vec3{601.5, 298.0, 0.0}, kSquare, 5.0);
    REQUIRE(hit.has_value());
    CHECK(hit->point.equals(Vec3{600.0, 300.0, 0.0}));
    // Both, so both paths can be drawn.
    CHECK(hit->origins.size() == 2);
}

TEST_CASE("a crossing far from the cursor is not jumped to") {
    // These two do cross -- at (600, 300) -- but the cursor is nowhere near it,
    // and it is on neither path either.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}, Vec3{600.0, 100.0, 0.0}};
    CHECK_FALSE(alignToAcquired(acquired, Vec3{500.0, 200.0, 0.0}, kSquare, 5.0).has_value());
}

TEST_CASE("parallel paths have no crossing to offer") {
    // Two points level with each other: the cursor is on both horizontals at
    // once, and the answer is the nearer path rather than a division by zero.
    const std::vector<Vec3> acquired{Vec3{0.0, 300.0, 0.0}, Vec3{100.0, 300.0, 0.0}};
    const auto hit = alignToAcquired(acquired, Vec3{500.0, 302.0, 0.0}, kSquare, 5.0);
    REQUIRE(hit.has_value());
    CHECK(hit->point.equals(Vec3{500.0, 300.0, 0.0}));
    CHECK(hit->origins.size() == 1);
}

TEST_CASE("paths only run forwards from the point that made them") {
    // The opposite direction is its own multiple of the increment, so a cursor
    // behind the acquired point is caught by that one instead -- and the answer
    // still lies on the correct side.
    const std::vector<Vec3> acquired{Vec3{400.0, 300.0, 0.0}};
    const auto hit = alignToAcquired(acquired, Vec3{200.0, 301.0, 0.0}, kSquare, 5.0);
    REQUIRE(hit.has_value());
    CHECK(hit->point.equals(Vec3{200.0, 300.0, 0.0}));
}

TEST_CASE("the increment decides which angles are tracked") {
    // With polar on at 45 degrees, a diagonal is a path; with only the square
    // directions it is not.
    const std::vector<Vec3> acquired{Vec3{0.0, 0.0, 0.0}};
    const Vec3 cursor{100.0, 98.0, 0.0};
    CHECK_FALSE(alignToAcquired(acquired, cursor, kSquare, 5.0).has_value());

    const auto diagonal = alignToAcquired(acquired, cursor, kPi / 4.0, 5.0);
    REQUIRE(diagonal.has_value());
    CHECK(diagonal->point.x == doctest::Approx(99.0));
    CHECK(diagonal->point.y == doctest::Approx(99.0));
}

TEST_CASE("reach is what decides near enough, and scales with it") {
    // Called with the pick aperture in drawing units, so the pull is the same
    // size on screen at every zoom. Four units off is a miss at a reach of
    // three and a hit at a reach of ten.
    const std::vector<Vec3> acquired{Vec3{0.0, 0.0, 0.0}};
    const Vec3 cursor{100.0, 4.0, 0.0};
    CHECK_FALSE(alignToAcquired(acquired, cursor, kSquare, 3.0).has_value());
    REQUIRE(alignToAcquired(acquired, cursor, kSquare, 10.0).has_value());
}

}  // TEST_SUITE
