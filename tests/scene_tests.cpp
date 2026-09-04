#include "test_helpers.hpp"

#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/render/Scene.hpp"

using namespace cad;
using namespace cad::render;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    SpatialIndex index;
    std::unique_ptr<SceneBuilder> builder;

    Harness() { builder = std::make_unique<SceneBuilder>(*db, index); }

    Entity& addLine(const Vec3& from, const Vec3& to) {
        return db->addToModelSpace(std::make_unique<LineEntity>(from, to));
    }

    [[nodiscard]] Scene build(const Viewport& viewport, const SceneOptions& options = {}) {
        builder->reindex(db->modelSpace());
        return builder->build(db->modelSpace(), viewport, options);
    }
};

/// A view showing roughly the region [-50, 150] in both axes.
[[nodiscard]] Viewport standardView() {
    return Viewport{Vec3{50.0, 50.0}, 4.0, 800.0, 800.0};
}

}  // namespace

TEST_SUITE("SceneBuilder") {
    TEST_CASE("produces tessellated geometry with resolved colour") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Red");
        layer.color = Color::fromIndex(1);

        Entity& line = h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        line.layerHandle = layer.handle;

        const Scene scene = h.build(standardView());
        REQUIRE(scene.items.size() == 1);

        const DrawItem& item = scene.items.front();
        CHECK(item.kind == DrawItem::Kind::Geometry);
        CHECK(item.handle == line.handle);
        // ByLayer has already been resolved; the backend needs no lookup.
        CHECK(item.style.rgb == 0xFF0000u);
        REQUIRE(item.geometry.size() == 1);
        CHECK(item.geometry.front().points.size() == 2);
    }

    TEST_CASE("culls geometry outside the view") {
        Harness h;
        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        h.addLine(Vec3{100000.0, 0.0}, Vec3{100100.0, 0.0});

        const Scene scene = h.build(standardView());
        // Spatial culling is the difference between a few frames per second and
        // a smooth one on a large drawing.
        CHECK(scene.items.size() == 1);
    }

    TEST_CASE("skips entities on hidden and frozen layers") {
        Harness h;
        Layer& off = h.db->ensureLayer("Off");
        off.isOff = true;
        Layer& frozen = h.db->ensureLayer("Frozen");
        frozen.isFrozen = true;

        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        h.addLine(Vec3{0.0, 10.0}, Vec3{100.0, 10.0}).layerHandle = off.handle;
        h.addLine(Vec3{0.0, 20.0}, Vec3{100.0, 20.0}).layerHandle = frozen.handle;

        const Scene scene = h.build(standardView());
        CHECK(scene.items.size() == 1);
    }

    TEST_CASE("drops sub-pixel geometry as level of detail") {
        Harness h;
        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 0.0});
        // A 0.01-unit line at 4 pixels per unit is far below one pixel.
        h.addLine(Vec3{50.0, 50.0}, Vec3{50.01, 50.0});

        const Scene scene = h.build(standardView());
        CHECK(scene.items.size() == 1);
        CHECK(scene.culled >= 1);
    }

    TEST_CASE("tessellates curves more finely when zoomed in") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{50.0, 50.0}, 40.0));

        const Scene coarse = h.build(Viewport{Vec3{50.0, 50.0}, 1.0, 400.0, 400.0});
        const Scene fine = h.build(Viewport{Vec3{50.0, 50.0}, 40.0, 400.0, 400.0});

        REQUIRE_FALSE(coarse.items.empty());
        REQUIRE_FALSE(fine.items.empty());
        // Chord tolerance follows the zoom, so a circle stays smooth when
        // magnified without costing thousands of segments when tiny.
        CHECK(fine.items.front().geometry.front().points.size() >
              coarse.items.front().geometry.front().points.size());
    }

    TEST_CASE("expands a block reference into its contents") {
        Harness h;
        auto block = std::make_unique<BlockTableRecord>();
        block->name = "WIDGET";
        BlockTableRecord& definition = h.db->addBlock(std::move(block));
        h.db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0}), definition);

        h.db->addToModelSpace(
            std::make_unique<InsertEntity>(definition.handle, Vec3{40.0, 40.0}));

        const Scene scene = h.build(standardView());
        REQUIRE(scene.items.size() == 1);

        // The contents arrive already transformed into world space.
        const std::vector<Vec3>& points = scene.items.front().geometry.front().points;
        CHECK_POINT(points.front(), Vec3(40, 40));
        CHECK_POINT(points.back(), Vec3(50, 40));
    }

    TEST_CASE("resolves ByBlock colour from the reference, not the definition") {
        Harness h;
        Layer& layer = h.db->ensureLayer("L");
        layer.color = Color::fromIndex(1);  // red

        auto block = std::make_unique<BlockTableRecord>();
        block->name = "WIDGET";
        BlockTableRecord& definition = h.db->addBlock(std::move(block));

        auto child = std::make_unique<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0});
        child->color = Color::byBlock();
        child->layerHandle = layer.handle;
        h.db->addEntity(std::move(child), definition);

        auto insert = std::make_unique<InsertEntity>(definition.handle, Vec3{40.0, 40.0});
        insert->color = Color::fromIndex(3);  // green
        h.db->addToModelSpace(std::move(insert));

        const Scene scene = h.build(standardView());
        REQUIRE(scene.items.size() == 1);
        // The same block inserted elsewhere with a different colour must look
        // different; that is the whole point of ByBlock.
        CHECK(scene.items.front().style.rgb == 0x00FF00u);
    }

    TEST_CASE("draws a placeholder for a reference whose content is missing") {
        Harness h;
        auto xref = std::make_unique<BlockTableRecord>();
        xref->name = "MISSING";
        xref->xrefType = XrefType::Attachment;
        xref->xrefStatus = XrefStatus::NotFound;
        BlockTableRecord& definition = h.db->addBlock(std::move(xref));

        h.db->addToModelSpace(
            std::make_unique<InsertEntity>(definition.handle, Vec3{50.0, 50.0}));

        const Scene scene = h.build(standardView());
        REQUIRE(scene.items.size() == 1);
        // A missing reference rendering as nothing leaves the user unable to
        // tell that part of the drawing is absent.
        CHECK(scene.items.front().kind == DrawItem::Kind::MissingReference);
        CHECK_POINT(scene.items.front().position, Vec3(50, 50));
    }

    TEST_CASE("emits text as content rather than geometry") {
        Harness h;
        h.db->addToModelSpace(
            std::make_unique<TextEntity>(Vec3{50.0, 50.0}, "ROOM 101", 5.0));

        const Scene scene = h.build(standardView());
        REQUIRE(scene.items.size() == 1);
        // The backend owns font measurement and layout, which the core
        // deliberately cannot do.
        CHECK(scene.items.front().kind == DrawItem::Kind::Text);
        CHECK(scene.items.front().text == "ROOM 101");
        CHECK(scene.items.front().textHeight == doctest::Approx(5.0));
    }

    TEST_CASE("emits a point as a fixed-size marker rather than culling it") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<PointEntity>(Vec3{50.0, 50.0}));

        const Scene scene = h.build(standardView());
        // A point has zero extent by definition and is drawn at a fixed pixel
        // size, so the sub-pixel level-of-detail rule must not apply to it —
        // otherwise every point in every drawing disappears.
        REQUIRE(scene.items.size() == 1);
        CHECK(scene.items.front().kind == DrawItem::Kind::Marker);
        CHECK(scene.culled == 0);
    }

    TEST_CASE("respects the item cap on a pathological drawing") {
        Harness h;
        for (int i = 0; i < 200; ++i) {
            h.addLine(Vec3{0.0, static_cast<double>(i) * 0.5},
                      Vec3{100.0, static_cast<double>(i) * 0.5});
        }

        SceneOptions options;
        options.maxItems = 50;
        const Scene scene = h.build(standardView(), options);
        CHECK(scene.items.size() <= 50);
    }

    TEST_CASE("a zoomed-in view costs a fraction of the drawing") {
        // The property the whole renderer rests on, and the reason ADR 0003
        // held off on a GPU backend: zoomed in, work is proportional to what
        // is *visible*, not to the drawing. Measured as a count rather than a
        // duration, because the count is deterministic and a timing assertion
        // on a shared machine is a coin toss — and if the index ever stops
        // narrowing the search, the count is what changes first.
        Harness h;
        constexpr int kGrid = 100;  // 10,000 entities over a wide area
        for (int x = 0; x < kGrid; ++x) {
            for (int y = 0; y < kGrid; ++y) {
                const Vec3 at{x * 100.0, y * 100.0, 0.0};
                h.addLine(at, at + Vec3{60.0, 0.0, 0.0});
            }
        }

        // A window over roughly three grid cells square.
        const Viewport zoomed{Vec3{5000.0, 5000.0, 0.0}, 2.0, 800.0, 800.0};
        const Scene scene = h.build(zoomed);

        CHECK(scene.totalCandidates == kGrid * kGrid);
        CHECK_FALSE(scene.items.empty());
        // Well under one percent of the drawing. The exact number depends on
        // the window; the order of magnitude is the point.
        CHECK(scene.items.size() < 100);
    }

    TEST_CASE("layer appearance changes take effect on the next frame") {
        // The resolution context is cached per layer for the duration of a
        // build. If that cache outlived the frame, the layer manager would
        // appear not to work — the change would land in the database and not
        // on the screen.
        Harness h;
        Layer& walls = h.db->ensureLayer("Walls");
        walls.color = Color::fromIndex(1);
        Entity& line = h.addLine(Vec3::zero(), Vec3{100.0, 0.0, 0.0});
        line.layerHandle = walls.handle;

        const Scene before = h.build(standardView());
        REQUIRE_FALSE(before.items.empty());
        const std::uint32_t red = before.items.front().style.rgb;

        walls.color = Color::fromIndex(3);
        const Scene after = h.build(standardView());
        REQUIRE_FALSE(after.items.empty());
        CHECK(after.items.front().style.rgb != red);
    }

    TEST_CASE("reports extents through the index once populated") {
        Harness h;
        h.addLine(Vec3{0.0, 0.0}, Vec3{100.0, 40.0});
        h.builder->reindex(h.db->modelSpace());

        const Box3 extents = spaceExtents(*h.db, h.db->modelSpace(), &h.index);
        CHECK(extents.max.x == doctest::Approx(100.0));
        CHECK(extents.max.y == doctest::Approx(40.0));
    }

    /// MTEXT anchors by a nine-way attachment point; the backend justifies by
    /// TEXT's separate horizontal and vertical codes. If the scene does not
    /// translate between them, text is drawn from its insertion point while
    /// bounds() places the pick box by the attachment — and a centred room
    /// label cannot be selected by clicking on it.
    ///
    /// Checked as a *correspondence* rather than by asserting nine literals,
    /// because the requirement is that the two sides agree.
    TEST_CASE("MText justification matches the box it is picked by") {
        struct Case {
            MTextAttachment attachment;
            int horizontal;
            int vertical;
        };
        const Case cases[] = {
            {MTextAttachment::TopLeft, 0, 3},      {MTextAttachment::TopCenter, 1, 3},
            {MTextAttachment::TopRight, 2, 3},     {MTextAttachment::MiddleLeft, 0, 2},
            {MTextAttachment::MiddleCenter, 1, 2}, {MTextAttachment::MiddleRight, 2, 2},
            {MTextAttachment::BottomLeft, 0, 1},   {MTextAttachment::BottomCenter, 1, 1},
            {MTextAttachment::BottomRight, 2, 1},
        };

        for (const Case& testCase : cases) {
            Harness h;
            auto text = std::make_unique<MTextEntity>();
            text->position = Vec3{50.0, 50.0, 0.0};
            text->contents = "Master Bedroom";
            text->height = 4.0;
            text->attachment = testCase.attachment;
            const Entity& entity = h.db->addToModelSpace(std::move(text));
            const Box3 box = entity.bounds();

            const Scene scene = h.build(standardView());
            REQUIRE(scene.items.size() == 1);
            const DrawItem& item = scene.items.front();
            REQUIRE(item.kind == DrawItem::Kind::Text);

            CHECK(item.horizontalAlignment == testCase.horizontal);
            CHECK(item.verticalAlignment == testCase.vertical);

            // The anchor must sit on the edge of the box the justification
            // names: left-justified text starts at the box's left edge, and so
            // on round. That is the property a click depends on.
            const double x = item.position.x;
            const double expectedX = testCase.horizontal == 0   ? box.min.x
                                     : testCase.horizontal == 1 ? (box.min.x + box.max.x) / 2.0
                                                                : box.max.x;
            CHECK(x == doctest::Approx(expectedX));

            const double y = item.position.y;
            const double expectedY = testCase.vertical == 3   ? box.max.y
                                     : testCase.vertical == 2 ? (box.min.y + box.max.y) / 2.0
                                                              : box.min.y;
            CHECK(y == doctest::Approx(expectedY));
        }
    }

    /// The defined width is a wrap rectangle, not the text's extent. Nothing
    /// below the backend wraps, so a label drawn past its rectangle must still
    /// be picked along its whole length.
    TEST_CASE("MText bounds cover the drawn text, not just the wrap rectangle") {
        Harness h;
        auto text = std::make_unique<MTextEntity>();
        text->position = Vec3{50.0, 50.0, 0.0};
        text->contents = "Master Bedroom";
        text->height = 4.0;
        text->width = 1.0;  // far narrower than the label
        const Entity& entity = h.db->addToModelSpace(std::move(text));

        const Box3 box = entity.bounds();
        // Compared with a tolerance because the box is built at the origin and
        // then translated, so its width comes back as a difference of two
        // larger numbers and lands a few bits under the value it was built from.
        const double drawn = measureText("Master Bedroom", 4.0, 1.0);
        CAPTURE(box.width());
        CAPTURE(drawn);
        CHECK(box.width() >= drawn - 1e-9);
    }
}
