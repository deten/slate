#include "test_helpers.hpp"

#include "cad/math/Angle.hpp"
#include "cad/render/SpatialIndex.hpp"
#include "cad/render/Viewport.hpp"

using namespace cad;
using namespace cad::render;

TEST_SUITE("Viewport") {
    TEST_CASE("maps world to screen with Y flipped") {
        const Viewport viewport{Vec3::zero(), 1.0, 800.0, 600.0};

        // The view centre lands at the surface centre.
        const ScreenPoint origin = viewport.worldToScreen(Vec3::zero());
        CHECK(origin.x == doctest::Approx(400.0));
        CHECK(origin.y == doctest::Approx(300.0));

        // World Y increases upward; screen Y increases downward.
        const ScreenPoint up = viewport.worldToScreen(Vec3{0.0, 100.0});
        CHECK(up.y == doctest::Approx(200.0));
    }

    TEST_CASE("round-trips a point through screen space") {
        const Viewport viewport{Vec3{50.0, -20.0}, 2.5, 1024.0, 768.0, 0.4};
        const Vec3 world{123.0, -45.0, 0.0};
        CHECK_POINT(viewport.screenToWorld(viewport.worldToScreen(world)), world);
    }

    TEST_CASE("the matrix agrees with the scalar transform") {
        const Viewport viewport{Vec3{10.0, 5.0}, 3.0, 800.0, 600.0, 0.3};
        const Vec3 world{42.0, 17.0, 0.0};

        const ScreenPoint direct = viewport.worldToScreen(world);
        const Vec3 viaMatrix = viewport.worldToScreenMatrix().transformPoint(world);
        CHECK(viaMatrix.x == doctest::Approx(direct.x));
        CHECK(viaMatrix.y == doctest::Approx(direct.y));
    }

    TEST_CASE("keeps the anchor point fixed when zooming") {
        // This is what makes wheel zoom feel right: the world point under the
        // cursor must stay under the cursor.
        const Viewport viewport{Vec3::zero(), 1.0, 800.0, 600.0};
        const ScreenPoint anchor{200.0, 150.0};
        const Vec3 before = viewport.screenToWorld(anchor);

        const Viewport zoomed = viewport.zoomAt(anchor, 2.5);
        CHECK_POINT(zoomed.screenToWorld(anchor), before);
        CHECK(zoomed.scale() == doctest::Approx(2.5));
    }

    TEST_CASE("pans by a screen delta, following the cursor even when rotated") {
        const Viewport viewport{Vec3::zero(), 2.0, 800.0, 600.0, kPi / 3.0};
        const ScreenPoint probe{300.0, 200.0};
        const Vec3 before = viewport.screenToWorld(probe);

        // After panning by (dx, dy), the world point previously under `probe`
        // should appear at probe + (dx, dy).
        const Viewport panned = viewport.panByPixels(40.0, -25.0);
        const ScreenPoint moved = panned.worldToScreen(before);
        CHECK(moved.x == doctest::Approx(probe.x + 40.0));
        CHECK(moved.y == doctest::Approx(probe.y - 25.0));
    }

    TEST_CASE("fits a region with a margin") {
        const Viewport viewport{Vec3::zero(), 1.0, 800.0, 400.0};
        const Box3 region{Vec3::zero(), Vec3{200.0, 100.0}};

        const Viewport fitted = viewport.zoomToFit(region, 0.0);
        CHECK_POINT(fitted.center(), region.center());
        // 800/200 and 400/100 both give 4; the smaller wins.
        CHECK(fitted.scale() == doctest::Approx(4.0));

        // The whole region must be inside the visible bounds.
        const Box3 visible = fitted.visibleBounds();
        CHECK(visible.containsBox(region, Tolerance{1e-6, 1e-6}));
    }

    TEST_CASE("survives degenerate fit targets") {
        const Viewport viewport{Vec3::zero(), 7.0, 800.0, 600.0};

        // An empty box leaves the view alone rather than producing an infinite
        // scale.
        CHECK(viewport.zoomToFit(Box3::empty()).equals(viewport));

        // A single point centres without changing zoom.
        const Viewport onPoint = viewport.zoomToFit(Box3::fromPoint(Vec3{5.0, 5.0}));
        CHECK_POINT(onPoint.center(), Vec3(5, 5));
        CHECK(onPoint.scale() == doctest::Approx(7.0));
    }

    TEST_CASE("clamps scale to a usable range") {
        const Viewport viewport{Vec3::zero(), 1.0, 800.0, 600.0};
        // Beyond these limits double precision in screen space loses enough
        // accuracy that geometry visibly jitters when panned.
        CHECK(viewport.withScale(0.0).scale() == doctest::Approx(1.0));
        CHECK(viewport.withScale(1e30).scale() == doctest::Approx(kMaxScale));
        CHECK(viewport.withScale(-5.0).scale() == doctest::Approx(1.0));
    }

    TEST_CASE("bounds a rotated view conservatively") {
        const Viewport straight{Vec3::zero(), 1.0, 800.0, 600.0};
        const Viewport rotated = straight.withRotation(kPi / 4.0);
        // A rotated rectangle's axis-aligned bound is larger than the
        // unrotated one, which is what culling needs.
        CHECK(rotated.visibleBounds().width() > straight.visibleBounds().width());
    }

    TEST_CASE("interpolates zoom geometrically") {
        const Viewport from{Vec3::zero(), 1.0, 800.0, 600.0};
        const Viewport to = from.withScale(100.0);

        // Halfway must be the geometric mean (10), not the arithmetic one
        // (50.5), or the animation lurches at the start.
        CHECK(from.lerpTo(to, 0.5).scale() == doctest::Approx(10.0));
        CHECK(from.lerpTo(to, 0.0).scale() == doctest::Approx(1.0));
        CHECK(from.lerpTo(to, 1.0).scale() == doctest::Approx(100.0));
    }

    TEST_CASE("derives chord tolerance from the zoom") {
        const Viewport zoomedOut{Vec3::zero(), 0.1, 800.0, 600.0};
        const Viewport zoomedIn{Vec3::zero(), 100.0, 800.0, 600.0};
        // Tessellation must get finer as the user zooms in, or curves are
        // faceted; and coarser as they zoom out, or a 3-pixel circle costs
        // thousands of segments.
        CHECK(chordToleranceFor(zoomedIn) < chordToleranceFor(zoomedOut));
        CHECK(chordToleranceFor(zoomedIn) == doctest::Approx(0.005));
    }
}

TEST_SUITE("SpatialIndex") {
    namespace {

    /// A grid of unit boxes, for exercising the tree at a realistic size.
    [[nodiscard]] std::vector<SpatialIndex::Item> gridItems(int side) {
        std::vector<SpatialIndex::Item> items;
        items.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
        std::uint64_t next = 1;
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const Vec3 min{static_cast<double>(x) * 10.0, static_cast<double>(y) * 10.0};
                items.push_back({Handle{next++}, Box3{min, min + Vec3{1.0, 1.0}}});
            }
        }
        return items;
    }

    }  // namespace

    TEST_CASE("finds items intersecting a region") {
        SpatialIndex index;
        index.rebuild(gridItems(20));
        CHECK(index.size() == 400);

        // A region covering the first 3x3 block of cells.
        const std::vector<Handle> hits = index.search(Box3{Vec3{-1.0, -1.0}, Vec3{21.0, 21.0}});
        CHECK(hits.size() == 9);
    }

    TEST_CASE("separates crossing from window selection") {
        SpatialIndex index;
        index.insert(Handle{1}, Box3{Vec3{0.0, 0.0}, Vec3{10.0, 10.0}});
        index.insert(Handle{2}, Box3{Vec3{5.0, 5.0}, Vec3{20.0, 20.0}});

        const Box3 region{Vec3{-1.0, -1.0}, Vec3{11.0, 11.0}};
        // Crossing catches both; window catches only the fully contained one.
        CHECK(index.search(region).size() == 2);
        const std::vector<Handle> contained = index.searchContained(region);
        REQUIRE(contained.size() == 1);
        CHECK(contained.front() == Handle{1});
    }

    TEST_CASE("ignores empty bounds rather than corrupting ancestors") {
        SpatialIndex index;
        index.insert(Handle{1}, Box3{Vec3::zero(), Vec3{1.0, 1.0}});
        // An entity with no extent cannot be found spatially, and indexing an
        // empty box would poison every ancestor node's bounds.
        index.insert(Handle{2}, Box3::empty());

        CHECK(index.size() == 1);
        CHECK(index.bounds().max.x == doctest::Approx(1.0));
    }

    TEST_CASE("reports overall extents") {
        SpatialIndex index;
        index.rebuild(gridItems(10));
        const Box3 bounds = index.bounds();
        CHECK(bounds.min.x == doctest::Approx(0.0));
        // Last cell starts at 90 and is 1 wide.
        CHECK(bounds.max.x == doctest::Approx(91.0));
    }

    TEST_CASE("finds the nearest items in order") {
        SpatialIndex index;
        index.rebuild(gridItems(10));

        const std::vector<Handle> nearest = index.nearest(Vec3{-5.0, -5.0}, 3);
        REQUIRE(nearest.size() == 3);
        // The cell at the origin is closest to a query below and left of it.
        CHECK(nearest.front() == Handle{1});
    }

    TEST_CASE("respects a maximum distance") {
        SpatialIndex index;
        index.insert(Handle{1}, Box3{Vec3::zero(), Vec3{1.0, 1.0}});
        index.insert(Handle{2}, Box3{Vec3{1000.0, 0.0}, Vec3{1001.0, 1.0}});

        CHECK(index.nearest(Vec3{-1.0, 0.0}, 5, 10.0).size() == 1);
        CHECK(index.nearest(Vec3{-1.0, 0.0}, 5).size() == 2);
    }

    TEST_CASE("rebuilds lazily after a mutation") {
        SpatialIndex index;
        index.rebuild(gridItems(10));
        CHECK(index.search(Box3{Vec3{-1.0, -1.0}, Vec3{1.0, 1.0}}).size() == 1);

        index.remove(Handle{1});
        // The query must see the removal without an explicit rebuild call.
        CHECK(index.search(Box3{Vec3{-1.0, -1.0}, Vec3{1.0, 1.0}}).empty());
        CHECK(index.size() == 99);
    }

    TEST_CASE("stays shallow on a large index") {
        SpatialIndex index;
        index.rebuild(gridItems(60));  // 3600 items
        CHECK(index.size() == 3600);
        // A packed tree with capacity 16 over 3600 items should be about three
        // levels; anything much deeper means the packing is not working and
        // queries would degenerate toward a scan.
        CHECK(index.treeHeight() <= 4);
    }

    TEST_CASE("returns nothing for an empty index or empty region") {
        const SpatialIndex empty;
        CHECK(empty.search(Box3{Vec3::zero(), Vec3{1.0, 1.0}}).empty());
        CHECK(empty.bounds().isEmpty());
        CHECK(empty.nearest(Vec3::zero()).empty());

        SpatialIndex index;
        index.insert(Handle{1}, Box3{Vec3::zero(), Vec3{1.0, 1.0}});
        CHECK(index.search(Box3::empty()).empty());
    }
}
