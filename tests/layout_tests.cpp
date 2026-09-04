/// Tests for paper space: layouts, viewports, and not losing the rest.
///
/// A set of construction drawings is mostly one model seen through a dozen
/// viewports on a dozen sheets. Losing the sheets keeps the working notes and
/// throws away the deliverable, so a good half of what is checked here is that
/// nothing goes missing on the way through.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/render/Scene.hpp"
#include "test_helpers.hpp"

using namespace cad;

namespace {

/// A drawing with a model and one sheet holding a viewport.
struct SheetHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    render::SpatialIndex index;

    Layout* sheet = nullptr;
    BlockTableRecord* paper = nullptr;
    ViewportEntity* window = nullptr;

    SheetHarness() {
        db->ensureModelLayout();

        paper = db->findBlock(kPaperSpace);
        REQUIRE(paper != nullptr);

        auto layout = std::make_unique<Layout>();
        layout->name = "A-1 Plan";
        layout->blockRecordHandle = paper->handle;
        layout->tabOrder = 1;
        layout->plot.paperWidth = 420.0;
        layout->plot.paperHeight = 297.0;
        sheet = &db->add(std::move(layout));

        auto viewport = std::make_unique<ViewportEntity>();
        viewport->viewportId = 2;
        viewport->center = Vec3{200.0, 150.0, 0.0};
        viewport->width = 200.0;
        viewport->height = 100.0;
        viewport->viewCenter = Vec3{5000.0, 3000.0, 0.0};
        viewport->viewHeight = 10000.0;  // 1:100
        window = static_cast<ViewportEntity*>(&db->addEntity(std::move(viewport), *paper));
    }

    void reindexModel() {
        render::SceneBuilder builder{*db, index};
        builder.reindex(db->modelSpace());
    }
};

[[nodiscard]] render::Viewport sheetView() {
    // A view large enough to hold a 420x297 sheet comfortably.
    return render::Viewport{}.withSize(1000, 700).zoomToFit(
        Box3::fromCorners(Vec3::zero(), Vec3{420.0, 297.0, 0.0}));
}

}  // namespace

TEST_SUITE("Layouts") {
    TEST_CASE("every drawing has a Model layout, even one that declared none") {
        auto db = Database::createEmpty();
        CHECK(db->layouts().empty());

        Layout& model = db->ensureModelLayout();
        CHECK(model.isModel());
        CHECK(model.blockRecordHandle == db->modelSpace().handle);
        // Idempotent: the shell calls this on every document.
        CHECK(&db->ensureModelLayout() == &model);
        CHECK(db->layouts().size() == 1);
    }

    TEST_CASE("layouts come back in tab order with Model first") {
        auto db = Database::createEmpty();
        for (const auto& [name, order] : std::vector<std::pair<std::string, int>>{
                 {"C-3", 3}, {"A-1", 1}, {"B-2", 2}}) {
            auto layout = std::make_unique<Layout>();
            layout->name = name;
            layout->tabOrder = order;
            db->add(std::move(layout));
        }
        db->ensureModelLayout();

        const std::vector<Layout*> ordered = db->layouts();
        REQUIRE(ordered.size() == 4);
        // Model first whatever its stored order, then by tab order.
        CHECK(ordered[0]->isModel());
        CHECK(ordered[1]->name == "A-1");
        CHECK(ordered[2]->name == "B-2");
        CHECK(ordered[3]->name == "C-3");
    }

    TEST_CASE("layout order is stable when tab orders collide") {
        // The object store is a hash map, so without a tiebreak the tabs would
        // shuffle between runs — and real drawings, edited by several people
        // over years, do contain duplicate tab orders.
        auto db = Database::createEmpty();
        for (const char* name : {"Zebra", "Alpha", "Middle"}) {
            auto layout = std::make_unique<Layout>();
            layout->name = name;
            layout->tabOrder = 1;
            db->add(std::move(layout));
        }
        const std::vector<Layout*> first = db->layouts();
        const std::vector<Layout*> second = db->layouts();
        REQUIRE(first.size() == 3);
        for (std::size_t i = 0; i < first.size(); ++i) {
            CHECK(first[i]->name == second[i]->name);
        }
        CHECK(first[0]->name == "Alpha");
    }

    TEST_CASE("a layout is found by name and by block") {
        SheetHarness harness;
        CHECK(harness.db->findLayout("a-1 plan") == harness.sheet);
        CHECK(harness.db->findLayout("nope") == nullptr);
        CHECK(harness.db->layoutForBlock(harness.paper->handle) == harness.sheet);
    }

    TEST_CASE("the printable area is the sheet less its margins") {
        PlotSettings plot;
        plot.paperWidth = 420.0;
        plot.paperHeight = 297.0;
        plot.marginLeft = 10.0;
        plot.marginBottom = 10.0;
        plot.marginRight = 5.0;
        plot.marginTop = 5.0;

        const Box3 printable = plot.printableArea();
        CHECK(printable.min.x == doctest::Approx(10.0));
        CHECK(printable.min.y == doctest::Approx(10.0));
        CHECK(printable.max.x == doctest::Approx(415.0));
        CHECK(printable.max.y == doctest::Approx(292.0));
    }

    /// The numbers here are from a real drawing: US Letter, plotted at 90
    /// degrees, in an imperial drawing whose paper space is in inches. Its one
    /// viewport is centred at (5.25, 4.0) and is 8.4 x 6.4 — which only makes
    /// sense on an 11 x 8.5 sheet, not on the stored 215.9 x 279.4.
    TEST_CASE("sheet size converts stored millimetres into paper units") {
        PlotSettings plot;
        plot.paperWidth = 215.9;
        plot.paperHeight = 279.4;
        plot.paperUnits = PlotPaperUnits::Inches;

        CHECK(plot.sheetSize().x == doctest::Approx(8.5));
        CHECK(plot.sheetSize().y == doctest::Approx(11.0));

        plot.rotation = 90;
        CHECK(plot.sheetSize().x == doctest::Approx(11.0));
        CHECK(plot.sheetSize().y == doctest::Approx(8.5));

        // The viewport that drawing carries must land inside the sheet, and
        // near enough centred that the page is clearly the right one.
        const Vec3 size = plot.sheetSize();
        CHECK(5.25 + 8.4 / 2.0 < size.x);
        CHECK(4.0 + 6.4 / 2.0 < size.y);
        CHECK(std::abs(5.25 - size.x / 2.0) < 0.3);
        CHECK(std::abs(4.0 - size.y / 2.0) < 0.3);
    }

    TEST_CASE("millimetre sheets are unchanged, so ISO drawings keep working") {
        PlotSettings plot;
        plot.paperWidth = 420.0;
        plot.paperHeight = 297.0;
        plot.paperUnits = PlotPaperUnits::Millimetres;
        CHECK(plot.sheetSize().x == doctest::Approx(420.0));
        CHECK(plot.sheetSize().y == doctest::Approx(297.0));
    }

    TEST_CASE("margins follow the sheet round when the plot is rotated") {
        PlotSettings plot;
        plot.paperWidth = 200.0;
        plot.paperHeight = 100.0;
        plot.marginLeft = 10.0;
        plot.marginBottom = 20.0;
        plot.marginRight = 30.0;
        plot.marginTop = 40.0;

        // Turning the paper a quarter turn puts its bottom edge on the left.
        plot.rotation = 90;
        const Box3 printable = plot.printableArea();
        CHECK(printable.min.x == doctest::Approx(20.0));   // was the bottom
        CHECK(printable.min.y == doctest::Approx(30.0));   // was the right
        CHECK(printable.max.x == doctest::Approx(60.0));   // 100 - 40, was top
        CHECK(printable.max.y == doctest::Approx(190.0));  // 200 - 10, was left

        // Four quarter turns is where it started.
        plot.rotation = 360;
        CHECK(plot.printableArea().min.x == doctest::Approx(10.0));
        CHECK(plot.printableArea().min.y == doctest::Approx(20.0));
    }

    TEST_CASE("an unset plot scale is 1:1 rather than a division by zero") {
        PlotSettings plot;
        CHECK(plot.plotScale() == doctest::Approx(1.0));
        plot.numerator = 0.0;
        plot.denominator = 0.0;
        CHECK(plot.plotScale() == doctest::Approx(1.0));
        plot.numerator = 1.0;
        plot.denominator = 0.5;
        CHECK(plot.plotScale() == doctest::Approx(2.0));
    }

    /// Every number here is from sheet A-2.2 New of the sample drawing.
    ///
    /// The layout is set to "fit to paper", so its plot scale is 1.3092 rather
    /// than 1 — and that changes what the page *is* in the coordinates the
    /// sheet's own geometry uses. An 11 x 8.5 page fitted at 1.3092 is 8.40 x
    /// 6.49 paper-space units, and its corner is not at the origin.
    ///
    /// Drawing the page 11 x 8.5 at the origin instead is not a cosmetic error:
    /// the viewport is 8.4 x 6.4, so it went from filling the page to covering
    /// three quarters of it, and everything seen through it came out a third too
    /// small and off-centre. That was the visible symptom.
    TEST_CASE("a fit-to-paper layout puts the page where its geometry is") {
        Layout layout;
        layout.plot.paperWidth = 215.9;
        layout.plot.paperHeight = 279.4;
        layout.plot.paperUnits = PlotPaperUnits::Inches;
        layout.plot.rotation = 90;
        layout.plot.numerator = 1.0;
        layout.plot.denominator = 0.763803310358106;
        layout.limitsMin = Vec3{1.049344841659106, 0.7537062944693419, 0.0};
        layout.limitsMax = Vec3{9.451181072059477, 7.246034707821439, 0.0};

        CHECK(layout.plot.plotScale() == doctest::Approx(1.30924).epsilon(1e-4));

        // The page is the physical sheet divided by the plot scale.
        const Box3 sheet = layout.sheetRect();
        CHECK(sheet.width() == doctest::Approx(11.0 / 1.30924).epsilon(1e-4));
        CHECK(sheet.height() == doctest::Approx(8.5 / 1.30924).epsilon(1e-4));

        // And the drawing's one viewport, 8.4 x 6.4 centred at (5.25, 4.0),
        // sits centred on it — which is the check that the *position* is right
        // and not only the size.
        const Vec3 middle = sheet.min.midpoint(sheet.max);
        CHECK(middle.x == doctest::Approx(5.25).epsilon(1e-3));
        CHECK(middle.y == doctest::Approx(4.0).epsilon(1e-3));
    }

    TEST_CASE("without stored limits the page sits at the paper-space origin") {
        Layout layout;
        layout.plot.paperWidth = 420.0;
        layout.plot.paperHeight = 297.0;

        const Box3 sheet = layout.sheetRect();
        CHECK_POINT(sheet.min, Vec3::zero());
        CHECK(sheet.width() == doctest::Approx(420.0));
        CHECK(sheet.height() == doctest::Approx(297.0));
    }

    TEST_CASE("margins are physical, so the plot scale applies to them too") {
        Layout layout;
        layout.plot.paperWidth = 200.0;
        layout.plot.paperHeight = 100.0;
        layout.plot.marginLeft = 10.0;
        layout.plot.marginBottom = 10.0;
        layout.plot.marginRight = 10.0;
        layout.plot.marginTop = 10.0;
        layout.plot.numerator = 1.0;
        layout.plot.denominator = 0.5;  // plotted at 2x
        layout.limitsMin = Vec3{0.0, 0.0, 0.0};
        layout.limitsMax = Vec3{90.0, 40.0, 0.0};

        // 10 mm of margin printed at 2x is 5 paper-space units of margin.
        const Box3 sheet = layout.sheetRect();
        CHECK_POINT(sheet.min, (Vec3{-5.0, -5.0, 0.0}));
        CHECK_POINT(sheet.max, (Vec3{95.0, 45.0, 0.0}));
        CHECK_POINT(layout.printableRect().max, (Vec3{90.0, 40.0, 0.0}));
    }
}

TEST_SUITE("Viewports") {
    TEST_CASE("scale is model units per paper unit") {
        SheetHarness harness;
        // 10000 model units shown in a 100 unit tall window is 1:100.
        CHECK(harness.window->modelPerPaper() == doctest::Approx(100.0));
    }

    TEST_CASE("the model-to-paper transform lands the view on the window") {
        SheetHarness harness;
        const Mat4 toPaper = harness.window->modelToPaper();

        // The view centre lands at the window centre.
        CHECK_POINT_EPS(toPaper.transformPoint(harness.window->viewCenter),
                        (Vec3{200.0, 150.0, 0.0}), 1e-9);

        // And a point 1000 model units up lands 10 paper units up, at 1:100.
        const Vec3 above = harness.window->viewCenter + Vec3{0.0, 1000.0, 0.0};
        CHECK_POINT_EPS(toPaper.transformPoint(above), (Vec3{200.0, 160.0, 0.0}), 1e-9);
    }

    TEST_CASE("the model region is what the window shows") {
        SheetHarness harness;
        const Box3 region = harness.window->modelRegion();
        // 200 paper wide at 1:100 is 20000 model units.
        CHECK(region.width() == doctest::Approx(20000.0));
        CHECK(region.height() == doctest::Approx(10000.0));
        CHECK(region.min.midpoint(region.max).equals(harness.window->viewCenter));
    }

    /// The view is stored as a target plus an offset from it, and the offset is
    /// in display coordinates, not world ones. A viewport that has been panned
    /// — which is every viewport on a real sheet — carries a non-zero target,
    /// and reading the offset alone puts the drawing off by exactly that much.
    ///
    /// Taken from A-2.2 New in the sample DWG: other CAD software shows the floor plan
    /// centred, and we showed a corner of it in the wrong place until the target
    /// was added back in.
    TEST_CASE("the view centre is measured from the view target") {
        SheetHarness harness;
        harness.window->viewCenter = Vec3{12.29, -31.53, 0.0};
        harness.window->viewTarget = Vec3{392.44, 341.79, 0.0};

        CHECK_POINT_EPS(harness.window->worldViewCenter(),
                        (Vec3{404.73, 310.26, 0.0}), 1e-9);

        // Both derived views agree, because both go through the same function.
        const Box3 region = harness.window->modelRegion();
        CHECK_POINT_EPS(region.min.midpoint(region.max), (Vec3{404.73, 310.26, 0.0}), 1e-9);
        CHECK_POINT_EPS(
            harness.window->modelToPaper().transformPoint(Vec3{404.73, 310.26, 0.0}),
            harness.window->center, 1e-9);
    }

    /// A target of zero is the case that hid the bug: a viewport zoomed to
    /// extents and never panned stores the whole view in group 12, so every
    /// hand-built test drawing works and every real sheet does not.
    TEST_CASE("a viewport with no target is centred on its view centre") {
        SheetHarness harness;
        CHECK_POINT(harness.window->viewTarget, Vec3::zero());
        CHECK_POINT_EPS(harness.window->worldViewCenter(), harness.window->viewCenter, 1e-9);
    }

    TEST_CASE("moving a viewport does not pan the model inside it") {
        SheetHarness harness;
        const Vec3 viewBefore = harness.window->viewCenter;
        harness.window->transformBy(Mat4::translation(Vec3{50.0, 0.0, 0.0}));

        CHECK_POINT(harness.window->center, (Vec3{250.0, 150.0, 0.0}));
        // Sliding a viewport across a title block must not move the drawing.
        CHECK_POINT(harness.window->viewCenter, viewBefore);
        CHECK(harness.window->modelPerPaper() == doctest::Approx(100.0));
    }

    TEST_CASE("scaling a viewport keeps its scale, showing more of the model") {
        SheetHarness harness;
        harness.window->transformBy(Mat4::scaling(2.0));
        // A drafter enlarging a viewport wants to see more, not a bigger model.
        CHECK(harness.window->modelPerPaper() == doctest::Approx(100.0));
        CHECK(harness.window->width == doctest::Approx(400.0));
    }

    TEST_CASE("dragging a corner grip resizes against the opposite corner") {
        SheetHarness harness;
        const std::vector<GripPoint> grips = harness.window->gripPoints();
        REQUIRE(grips.size() == 5);  // centre plus four corners

        const Box3 before = harness.window->paperRect();
        // Dragged toward the centre, so the window shrinks against the
        // opposite corner. Which corner grips[1] happens to be does not
        // matter: moving it inward always reduces the rectangle.
        const Vec3 inward = (before.min.midpoint(before.max) - grips[1].position).normalized();
        harness.window->moveGrip(grips[1], grips[1].position + inward * 20.0);
        const Box3 after = harness.window->paperRect();

        CHECK(after.width() < before.width());
        CHECK(after.height() < before.height());
        // The scale is preserved, so the sheet still reads 1:100.
        CHECK(harness.window->modelPerPaper() == doctest::Approx(100.0));
    }

    TEST_CASE("the sheet's own pseudo-viewport is recognised") {
        SheetHarness harness;
        CHECK_FALSE(harness.window->isPaperSpaceViewport());

        ViewportEntity paperItself;
        paperItself.viewportId = 1;
        // Drawing it would frame the whole sheet and put a second copy of the
        // model inside it.
        CHECK(paperItself.isPaperSpaceViewport());
    }

    TEST_CASE("a viewport freezes layers for itself alone") {
        SheetHarness harness;
        Layer& demolition = harness.db->ensureLayer("Demolition");
        harness.window->frozenLayerHandles.push_back(demolition.handle);

        CHECK(harness.window->isLayerFrozen(demolition.handle));
        CHECK_FALSE(harness.window->isLayerFrozen(harness.db->defaultLayer().handle));
    }
}

TEST_SUITE("PaperSpaceRendering") {
    TEST_CASE("a sheet draws its own geometry and the model through its window") {
        SheetHarness harness;
        // Something in the model, inside what the viewport looks at.
        harness.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{4000.0, 3000.0, 0.0}, Vec3{6000.0, 3000.0, 0.0}));
        // And a title block line on the sheet itself.
        harness.db->addEntity(
            std::make_unique<LineEntity>(Vec3{10.0, 10.0, 0.0}, Vec3{410.0, 10.0, 0.0}),
            *harness.paper);
        harness.reindexModel();

        render::SceneBuilder builder{*harness.db, harness.index};
        const render::Scene scene = builder.buildLayout(*harness.sheet, sheetView());

        // The sheet line, the viewport frame, and the model line seen through
        // the window.
        CHECK(scene.items.size() >= 3);

        std::size_t clipped = 0;
        for (const render::DrawItem& item : scene.items) {
            if (item.clip.has_value()) {
                ++clipped;
            }
        }
        // Exactly the viewport contents are clipped; the sheet's own geometry
        // is not, or the title block would vanish outside the window.
        CHECK(clipped == 1);
    }

    TEST_CASE("model geometry is scaled onto the sheet") {
        SheetHarness harness;
        harness.db->addToModelSpace(
            std::make_unique<LineEntity>(harness.window->viewCenter,
                                         harness.window->viewCenter + Vec3{1000.0, 0.0, 0.0}));
        harness.reindexModel();

        render::SceneBuilder builder{*harness.db, harness.index};
        const render::Scene scene = builder.buildLayout(*harness.sheet, sheetView());

        const render::DrawItem* through = nullptr;
        for (const render::DrawItem& item : scene.items) {
            if (item.clip.has_value() && item.kind == render::DrawItem::Kind::Geometry &&
                !item.geometry.empty() && item.geometry.front().points.size() == 2) {
                through = &item;
            }
        }
        REQUIRE(through != nullptr);

        // A 1000-unit line at 1:100 is 10mm on the sheet, starting at the
        // window's centre.
        const std::vector<Vec3>& points = through->geometry.front().points;
        CHECK_POINT_EPS(points.front(), (Vec3{200.0, 150.0, 0.0}), 1e-6);
        CHECK_POINT_EPS(points.back(), (Vec3{210.0, 150.0, 0.0}), 1e-6);
    }

    TEST_CASE("a layer frozen in a viewport is absent from that sheet only") {
        SheetHarness harness;
        Layer& demolition = harness.db->ensureLayer("Demolition");
        Entity& line = harness.db->addToModelSpace(
            std::make_unique<LineEntity>(harness.window->viewCenter,
                                         harness.window->viewCenter + Vec3{1000.0, 0.0, 0.0}));
        line.layerHandle = demolition.handle;
        harness.reindexModel();

        render::SceneBuilder builder{*harness.db, harness.index};
        const std::size_t before = builder.buildLayout(*harness.sheet, sheetView()).items.size();

        harness.window->frozenLayerHandles.push_back(demolition.handle);
        const render::Scene after = builder.buildLayout(*harness.sheet, sheetView());

        // This is the mechanism that lets one model serve a demolition sheet
        // and a new-work sheet.
        CHECK(after.items.size() == before - 1);
        CHECK(after.culled >= 1);

        // The model itself is untouched: viewport freezing is per sheet, not a
        // property of the layer. Framed on the model, not on the sheet — the
        // two are different coordinate systems entirely.
        const render::Viewport modelView =
            render::Viewport{}.withSize(1000, 700).zoomToFit(harness.window->modelRegion());
        CHECK(builder.build(harness.db->modelSpace(), modelView).items.size() >= 1);
    }

    /// Only bit 0x10000 of group 90 switches a viewport off, and it is *set* to
    /// turn it off. The rest of that group is view settings — perspective,
    /// clipping, snap, grid — which a real drawing sets freely: a sheet set
    /// from other CAD software carries 0xCC060 and must still draw.
    TEST_CASE("a viewport is off only when group 90's off bit is set") {
        SheetHarness harness;
        harness.db->addToModelSpace(
            std::make_unique<LineEntity>(harness.window->viewCenter,
                                         harness.window->viewCenter + Vec3{1000.0, 0.0, 0.0}));
        harness.reindexModel();

        render::SceneBuilder builder{*harness.db, harness.index};
        const auto clippedItems = [&] {
            const render::Scene scene = builder.buildLayout(*harness.sheet, sheetView());
            return std::count_if(scene.items.begin(), scene.items.end(),
                                 [](const render::DrawItem& item) {
                                     return item.clip.has_value();
                                 });
        };

        // The flags a real sheet carries: perspective and clipping bits among
        // them, off bit clear.
        harness.window->statusFlags = 0xCC060;
        CHECK(harness.window->isOn());
        CHECK(clippedItems() > 0);

        // other CAD software turns viewports off rather than deleting them.
        harness.window->statusFlags = 0xCC060 | ViewportEntity::kStatusOff;
        CHECK_FALSE(harness.window->isOn());
        CHECK(clippedItems() == 0);

        // A viewport with no flags at all — a hand-written DXF — is on.
        harness.window->statusFlags = 0;
        CHECK(harness.window->isOn());
        CHECK(clippedItems() > 0);
    }

    TEST_CASE("a layout with no block record draws nothing rather than crashing") {
        SheetHarness harness;
        harness.sheet->blockRecordHandle = Handle{999999};

        render::SceneBuilder builder{*harness.db, harness.index};
        const render::Scene scene = builder.buildLayout(*harness.sheet, sheetView());
        CHECK(scene.items.empty());
    }
}

TEST_SUITE("ObjectsSectionPreservation") {
    TEST_CASE("layouts survive a write and read") {
        SheetHarness harness;
        harness.sheet->plot.plotterName = "Acme Plotter";
        harness.sheet->plot.paperName = "ISO_A3";
        harness.sheet->plot.marginLeft = 7.5;

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*harness.db));

        const Layout* read = result.database->findLayout("A-1 Plan");
        REQUIRE(read != nullptr);
        CHECK(read->tabOrder == 1);
        CHECK(read->plot.paperWidth == doctest::Approx(420.0));
        CHECK(read->plot.paperHeight == doctest::Approx(297.0));
        CHECK(read->plot.marginLeft == doctest::Approx(7.5));
        // Code 2 is the plotter and 4 the paper size; swapping them is
        // invisible until someone plots.
        CHECK(read->plot.plotterName == "Acme Plotter");
        CHECK(read->plot.paperName == "ISO_A3");

        // And it still points at its sheet.
        const auto* block = result.database->get<BlockTableRecord>(read->blockRecordHandle);
        REQUIRE(block != nullptr);
        CHECK(block->isLayoutSpace());
    }

    TEST_CASE("paper space entities survive a write and read") {
        SheetHarness harness;
        harness.db->addEntity(
            std::make_unique<LineEntity>(Vec3{10.0, 10.0, 0.0}, Vec3{410.0, 10.0, 0.0}),
            *harness.paper);

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*harness.db));

        const Layout* read = result.database->findLayout("A-1 Plan");
        REQUIRE(read != nullptr);
        const auto* block = result.database->get<BlockTableRecord>(read->blockRecordHandle);
        REQUIRE(block != nullptr);

        // Writing only model space would silently delete every sheet's title
        // block, notes and viewports the first time a drawing was saved.
        const std::vector<Entity*> contents = result.database->entitiesIn(*block);
        CHECK(contents.size() == 2);

        std::size_t viewports = 0;
        std::size_t lines = 0;
        for (const Entity* entity : contents) {
            if (dynamic_cast<const ViewportEntity*>(entity) != nullptr) { ++viewports; }
            if (dynamic_cast<const LineEntity*>(entity) != nullptr) { ++lines; }
        }
        CHECK(viewports == 1);
        CHECK(lines == 1);
        // Model space did not gain the sheet's line.
        CHECK(result.database->entitiesIn(result.database->modelSpace()).empty());
    }

    TEST_CASE("a viewport's own properties survive a round trip") {
        SheetHarness harness;
        Layer& frozen = harness.db->ensureLayer("Frozen");
        harness.window->frozenLayerHandles.push_back(frozen.handle);
        harness.window->twist = degToRad(15.0);

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*harness.db));

        const Layout* read = result.database->findLayout("A-1 Plan");
        REQUIRE(read != nullptr);
        const auto* block = result.database->get<BlockTableRecord>(read->blockRecordHandle);
        REQUIRE(block != nullptr);

        const ViewportEntity* viewport = nullptr;
        for (const Entity* entity : result.database->entitiesIn(*block)) {
            if (const auto* candidate = dynamic_cast<const ViewportEntity*>(entity)) {
                viewport = candidate;
            }
        }
        REQUIRE(viewport != nullptr);
        CHECK(viewport->width == doctest::Approx(200.0));
        CHECK(viewport->height == doctest::Approx(100.0));
        CHECK(viewport->viewHeight == doctest::Approx(10000.0));
        CHECK(viewport->modelPerPaper() == doctest::Approx(100.0));
        CHECK(viewport->twist == doctest::Approx(degToRad(15.0)));
        REQUIRE(viewport->frozenLayerHandles.size() == 1);
    }

    TEST_CASE("objects this reader does not interpret are written back out") {
        // The OBJECTS section holds dictionaries, plot settings, layer states,
        // table and multileader styles, and a great deal of third-party data.
        // Dropping it is silent: the file opens, looks right, and loses half
        // its configuration the first time it is saved.
        const std::string dxf =
            "0\nSECTION\n2\nOBJECTS\n"
            "0\nDICTIONARY\n5\nA1\n330\nA0\n3\nACAD_GROUP\n350\nA2\n"
            "0\nMLEADERSTYLE\n5\nB1\n330\nA1\n1\nStandard\n40\n2.5\n"
            "0\nENDSEC\n0\nEOF\n";

        const io::DxfReadResult read = io::readDxf(dxf);
        CHECK(read.database->get<RawObject>(Handle::parse("A1")) != nullptr);
        CHECK(read.database->get<RawObject>(Handle::parse("B1")) != nullptr);

        io::DxfWriter writer;
        const std::string written = writer.write(*read.database);
        CHECK(written.find("MLEADERSTYLE") != std::string::npos);
        CHECK(written.find("ACAD_GROUP") != std::string::npos);

        // And they survive a second trip, so the preservation is stable rather
        // than merely present once.
        const io::DxfReadResult again = io::readDxf(written);
        const auto* style = again.database->get<RawObject>(Handle::parse("B1"));
        REQUIRE(style != nullptr);
        CHECK(style->dxfType == "MLEADERSTYLE");
        CHECK(style->ownerHandle == Handle::parse("A1"));
    }

    TEST_CASE("the OBJECTS section comes out in a stable order") {
        // The object store is a hash map. Without a deterministic order a
        // drawing under version control would show a whole-file diff after a
        // no-op edit.
        const std::string dxf =
            "0\nSECTION\n2\nOBJECTS\n"
            "0\nDICTIONARY\n5\n30\n330\n1\n"
            "0\nDICTIONARY\n5\n10\n330\n1\n"
            "0\nDICTIONARY\n5\n20\n330\n1\n"
            "0\nENDSEC\n0\nEOF\n";

        const io::DxfReadResult read = io::readDxf(dxf);
        io::DxfWriter first;
        io::DxfWriter second;
        CHECK(first.write(*read.database) == second.write(*read.database));
    }
}
