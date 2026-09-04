#include "test_helpers.hpp"

#include <algorithm>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include <array>
#include "cad/snap/ObjectSnap.hpp"
#include "cad/db/BlockGeometry.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Layer.hpp"
#include "cad/render/Picking.hpp"
#include "cad/render/Scene.hpp"

using namespace cad;
using namespace cad::render;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    SpatialIndex index;

    Entity& addLine(const Vec3& from, const Vec3& to) {
        return db->addToModelSpace(std::make_unique<LineEntity>(from, to));
    }

    /// A block holding two short lines in opposite corners of a wide square,
    /// so most of its bounding box is empty -- which is the shape of the
    /// problem: furniture symbols are mostly air.
    InsertEntity& addCornerBlock(const Vec3& at) {
        auto record = std::make_unique<BlockTableRecord>();
        record->name = "Corners";
        auto& block = static_cast<BlockTableRecord&>(db->addObject(std::move(record)));
        db->addEntity(std::make_unique<LineEntity>(Vec3{0.0, 0.0}, Vec3{10.0, 0.0}), block);
        db->addEntity(std::make_unique<LineEntity>(Vec3{90.0, 100.0}, Vec3{100.0, 100.0}),
                      block);

        auto insert = std::make_unique<InsertEntity>();
        insert->blockHandle = block.handle;
        insert->position = at;
        insert->setBlockExtents(db->blockExtents(block));
        return static_cast<InsertEntity&>(db->addToModelSpace(std::move(insert)));
    }

    void reindex() {
        SceneBuilder builder{*db, index};
        builder.reindex(db->modelSpace());
    }
};

}  // namespace

TEST_SUITE("Picking") {
    TEST_CASE("a block is picked by its geometry, not by its bounding box") {
        // A block reference cannot measure this for itself -- it knows its
        // extents and not its contents -- so left alone it answers "anywhere in
        // the box". In a plan full of furniture that means clicking an empty
        // patch of floor takes whichever symbol happens to span it.
        Harness h;
        InsertEntity& block = h.addCornerBlock(Vec3::zero());
        h.reindex();

        // The middle of the box, far from either line.
        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{50.0, 50.0}, 5.0).has_value());

        // On a line the block actually draws.
        const auto hit = pickEntity(*h.db, h.index, Vec3{5.0, 1.0}, 5.0);
        REQUIRE(hit.has_value());
        CHECK(hit->handle == block.handle);

        // And on the line in the far corner, which is what proves the whole
        // definition is expanded rather than just the first thing in it.
        const auto far = pickEntity(*h.db, h.index, Vec3{95.0, 99.0}, 5.0);
        REQUIRE(far.has_value());
        CHECK(far->handle == block.handle);
    }

    TEST_CASE("a block's own geometry can be snapped to") {
        // Snapping asks each candidate for its curves, and a reference has
        // none of its own -- so with nothing expanded, no endpoint, midpoint or
        // centre inside any block in the drawing could ever be snapped to. On a
        // floor plan that is every door, fixture and piece of furniture.
        Harness h;
        InsertEntity& block = h.addCornerBlock(Vec3{1000.0, 500.0, 0.0});
        h.reindex();

        const std::vector<CurvePtr> curves = blockReferenceCurves(*h.db, block);
        REQUIRE(curves.size() == 2);

        SnapCandidate candidate;
        candidate.handle = block.handle;
        candidate.curves = curves;
        candidate.insertionPoint = &block.position;
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        // Near the far end of the second line, which is at (100, 100) in the
        // definition and so at (1100, 600) once placed.
        query.cursor = Vec3{1098.0, 601.0, 0.0};
        query.aperture = 5.0;
        query.modes = toMask(SnapMode::Endpoint);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto snapped = engine.findBest(query);
        REQUIRE(snapped.has_value());
        CHECK(snapped->mode == SnapMode::Endpoint);
        // Exactly on the vertex, which is the whole point of a snap.
        CHECK(snapped->position.equals(Vec3{1100.0, 600.0, 0.0}));
    }

    TEST_CASE("a block reference is picked where it was placed") {
        // The definition's geometry is at the origin; the reference is not. If
        // the expansion missed the placement, the block would pick at the
        // wrong end of the drawing -- and every symbol in a plan would.
        Harness h;
        InsertEntity& block = h.addCornerBlock(Vec3{1000.0, 500.0, 0.0});
        h.reindex();

        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{5.0, 1.0}, 5.0).has_value());
        const auto hit = pickEntity(*h.db, h.index, Vec3{1005.0, 501.0}, 5.0);
        REQUIRE(hit.has_value());
        CHECK(hit->handle == block.handle);
    }

    TEST_CASE("selects the entity nearest the pick point") {
        Harness h;
        Entity& near = h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        h.addLine(Vec3{0.0, 50.0}, Vec3{100.0, 50.0});
        h.reindex();

        const auto hit = pickEntity(*h.db, h.index, Vec3{50.0, 1.0}, 5.0);
        REQUIRE(hit.has_value());
        CHECK(hit->handle == near.handle);
        CHECK(hit->distance == doctest::Approx(1.0));
    }

    TEST_CASE("misses when nothing is within the aperture") {
        Harness h;
        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        h.reindex();

        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{50.0, 40.0}, 5.0).has_value());
    }

    TEST_CASE("tests geometry, not just the bounding box") {
        // A long diagonal has a huge bounding box. Clicking well inside that
        // box but far from the line itself must not select it — this is the
        // whole reason picking has a precise second stage.
        Harness h;
        h.addLine(Vec3{0.0, 0.0}, Vec3{1000.0, 1000.0});
        h.reindex();

        CHECK(h.index.search(Box3{Vec3{0.0, 900.0}, Vec3{100.0, 1000.0}}).size() == 1);
        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{50.0, 950.0}, 5.0).has_value());
        // On the line, it picks.
        CHECK(pickEntity(*h.db, h.index, Vec3{500.0, 501.0}, 5.0).has_value());
    }

    TEST_CASE("refuses entities on locked and hidden layers") {
        Harness h;
        Layer& locked = h.db->ensureLayer("Locked");
        locked.isLocked = true;
        Layer& off = h.db->ensureLayer("Off");
        off.isOff = true;

        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0}).layerHandle = locked.handle;
        h.addLine(Vec3{0.0, 10.0}, Vec3{100.0, 10.0}).layerHandle = off.handle;
        h.reindex();

        // A lock the application ignores is worse than no lock, and selecting
        // something you cannot see is never useful.
        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{50.0, 0.0}, 5.0).has_value());
        CHECK_FALSE(pickEntity(*h.db, h.index, Vec3{50.0, 10.0}, 5.0).has_value());
    }

    TEST_CASE("returns overlapping entities nearest first, for selection cycling") {
        Harness h;
        Entity& closest = h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        Entity& further = h.addLine(Vec3{0.0, 2.0}, Vec3{100.0, 2.0});
        h.reindex();

        const std::vector<PickResult> hits = pickAll(*h.db, h.index, Vec3{50.0, 0.5}, 5.0);
        REQUIRE(hits.size() == 2);
        CHECK(hits[0].handle == closest.handle);
        CHECK(hits[1].handle == further.handle);
    }

    TEST_CASE("window selection takes only fully enclosed entities") {
        Harness h;
        Entity& inside = h.addLine(Vec3{10.0, 10.0}, Vec3{20.0, 20.0});
        h.addLine(Vec3{50.0, 50.0}, Vec3{500.0, 500.0});
        h.reindex();

        const std::vector<Handle> hits =
            pickWindow(*h.db, h.index, Box3{Vec3::zero(), Vec3{100.0, 100.0}}, false);
        REQUIRE(hits.size() == 1);
        CHECK(hits.front() == inside.handle);
    }

    TEST_CASE("crossing selection also takes entities the region merely touches") {
        Harness h;
        Entity& inside = h.addLine(Vec3{10.0, 10.0}, Vec3{20.0, 20.0});
        Entity& straddling = h.addLine(Vec3{50.0, 50.0}, Vec3{500.0, 500.0});
        h.reindex();

        // The distinction is not cosmetic: drafters rely on right-to-left
        // catching everything a rectangle touches.
        const std::vector<Handle> hits =
            pickWindow(*h.db, h.index, Box3{Vec3::zero(), Vec3{100.0, 100.0}}, true);
        REQUIRE(hits.size() == 2);
        CHECK(std::find(hits.begin(), hits.end(), inside.handle) != hits.end());
        CHECK(std::find(hits.begin(), hits.end(), straddling.handle) != hits.end());
    }

    TEST_CASE("crossing catches a line passing straight through with no vertex inside") {
        // Both endpoints are outside the region and no tessellated vertex lands
        // in it, so a vertex-only test would miss this entirely.
        Harness h;
        Entity& through = h.addLine(Vec3{-100.0, 50.0}, Vec3{200.0, 50.0});
        h.reindex();

        const std::vector<Handle> hits =
            pickWindow(*h.db, h.index, Box3{Vec3::zero(), Vec3{100.0, 100.0}}, true);
        REQUIRE(hits.size() == 1);
        CHECK(hits.front() == through.handle);
    }

    TEST_CASE("crossing rejects a line that only passes near the region") {
        Harness h;
        h.addLine(Vec3{-100.0, 500.0}, Vec3{200.0, 500.0});
        h.reindex();

        CHECK(pickWindow(*h.db, h.index, Box3{Vec3::zero(), Vec3{100.0, 100.0}}, true).empty());
    }

    TEST_CASE("window selection excludes locked layers too") {
        Harness h;
        Layer& locked = h.db->ensureLayer("Locked");
        locked.isLocked = true;
        h.addLine(Vec3{10.0, 10.0}, Vec3{20.0, 20.0}).layerHandle = locked.handle;
        h.reindex();

        CHECK(pickWindow(*h.db, h.index, Box3{Vec3::zero(), Vec3{100.0, 100.0}}, false).empty());
    }

    TEST_CASE("an empty region selects nothing") {
        Harness h;
        h.addLine(Vec3{10.0, 10.0}, Vec3{20.0, 20.0});
        h.reindex();

        CHECK(pickWindow(*h.db, h.index, Box3::empty(), true).empty());
        CHECK(pickWindow(*h.db, h.index, Box3::empty(), false).empty());
    }
}

TEST_SUITE("Command preview") {
    namespace {

    struct PreviewHarness {
        std::unique_ptr<Database> db = Database::createEmpty();
        TransactionManager tm{*db};
        std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
        SimpleSelection selection;
        CommandExecutor executor{*db, tm, *registry, selection};
    };

    }  // namespace

    TEST_CASE("LINE previews a rubber band from the last point") {
        PreviewHarness h;
        h.executor.execute("LINE");
        // No point yet, so nothing to rubber-band from.
        h.executor.moveCursor(Vec3{5.0, 5.0});
        CHECK(h.executor.preview().empty());

        h.executor.supply(InputValue::makePoint(Vec3::zero()));
        h.executor.moveCursor(Vec3{10.0, 0.0});

        REQUIRE(h.executor.preview().size() == 1);
        const auto* line = dynamic_cast<const LineEntity*>(h.executor.preview().front().get());
        REQUIRE(line != nullptr);
        CHECK_POINT(line->start, Vec3::zero());
        CHECK_POINT(line->end, Vec3(10, 0));

        // Preview geometry must never be part of the drawing.
        CHECK(h.db->entitiesIn(h.db->modelSpace()).empty());
    }

    TEST_CASE("CIRCLE previews the circle being sized") {
        PreviewHarness h;
        h.executor.execute("CIRCLE");
        h.executor.supply(InputValue::makePoint(Vec3::zero()));
        h.executor.moveCursor(Vec3{5.0, 0.0});

        REQUIRE(h.executor.preview().size() == 1);
        const auto* circle =
            dynamic_cast<const CircleEntity*>(h.executor.preview().front().get());
        REQUIRE(circle != nullptr);
        CHECK(circle->radius == doctest::Approx(5.0));
    }

    TEST_CASE("preview is cleared when the command ends") {
        PreviewHarness h;
        h.executor.execute("LINE");
        h.executor.supply(InputValue::makePoint(Vec3::zero()));
        h.executor.moveCursor(Vec3{10.0, 0.0});
        REQUIRE_FALSE(h.executor.preview().empty());

        h.executor.supply(InputValue::makeCancel());
        // A cancelled command must not leave a ghost rubber-band on screen.
        CHECK(h.executor.preview().empty());
    }
}
