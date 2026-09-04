// The current space, and the commands that make a sheet: MVIEW and PAGESETUP.
#include "test_helpers.hpp"

#include <memory>
#include <string>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/SpatialIndex.hpp"
#include "cad/render/Viewport.hpp"

using namespace cad;

namespace {

struct SheetHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};
    BlockTableRecord* sheet = nullptr;
    Layout* layout = nullptr;

    SheetHarness() {
        db->ensureModelLayout();
        auto block = std::make_unique<BlockTableRecord>();
        block->name = db->nextPaperSpaceName();
        sheet = &db->addBlock(std::move(block));
        auto page = std::make_unique<Layout>();
        page->name = "Sheet1";
        page->blockRecordHandle = sheet->handle;
        page->tabOrder = 1;
        page->plot.paperWidth = 210.0;
        page->plot.paperHeight = 297.0;
        page->plot.rotation = 90;
        page->plot.marginLeft = page->plot.marginRight = 10.0;
        page->plot.marginBottom = page->plot.marginTop = 10.0;
        layout = &db->add(std::move(page));
    }

    void showSheet() { db->settings.currentSpaceHandle = sheet->handle; }
    void showModel() { db->settings.currentSpaceHandle = Handle{}; }

    template <typename T>
    [[nodiscard]] std::vector<T*> onSheet() {
        std::vector<T*> found;
        for (Entity* entity : db->entitiesIn(*sheet)) {
            if (auto* typed = dynamic_cast<T*>(entity)) {
                found.push_back(typed);
            }
        }
        return found;
    }
};

[[nodiscard]] InputValue at(double x, double y) {
    return InputValue::makePoint(Vec3{x, y, 0.0});
}

}  // namespace

TEST_CASE("a drawing command draws into the current space, not always the model") {
    // A line drawn while a sheet's tab was showing used to land in model
    // space, because every command asked for model space by name.
    SheetHarness h;
    h.showSheet();
    REQUIRE(h.executor.execute("LINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(50.0, 0.0));
    h.executor.supply(InputValue::makeEmpty());
    CHECK(h.onSheet<LineEntity>().size() == 1);
    CHECK(h.db->entitiesIn(h.db->modelSpace()).empty());

    h.showModel();
    REQUIRE(h.executor.execute("LINE").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 5.0));
    h.executor.supply(at(50.0, 5.0));
    h.executor.supply(InputValue::makeEmpty());
    CHECK(h.onSheet<LineEntity>().size() == 1);
    CHECK(h.db->entitiesIn(h.db->modelSpace()).size() == 1);
}

TEST_CASE("the current space falls back to the model when its handle is stale") {
    SheetHarness h;
    h.db->settings.currentSpaceHandle = Handle{0xFFFFFF};
    CHECK(&h.db->currentSpace() == &h.db->modelSpace());
}

TEST_CASE("paper-space blocks are named the way other CAD software names them") {
    // An empty drawing already has *Paper_Space; the next ones count from 0.
    auto db = Database::createEmpty();
    REQUIRE(db->findBlock(kPaperSpace) != nullptr);
    CHECK(db->nextPaperSpaceName() == "*Paper_Space0");
    auto first = std::make_unique<BlockTableRecord>();
    first->name = db->nextPaperSpaceName();
    db->addBlock(std::move(first));
    CHECK(db->nextPaperSpaceName() == "*Paper_Space1");
    auto second = std::make_unique<BlockTableRecord>();
    second->name = db->nextPaperSpaceName();
    db->addBlock(std::move(second));
    CHECK(db->nextPaperSpaceName() == "*Paper_Space2");
}

TEST_CASE("MVIEW refuses to make a viewport in model space") {
    SheetHarness h;
    h.showModel();
    const CommandOutcome outcome = h.executor.execute("MVIEW");
    CHECK(outcome.status == CommandOutcome::Status::Failed);
    CHECK(h.onSheet<ViewportEntity>().empty());
}

TEST_CASE("MVIEW makes a viewport at the given scale") {
    SheetHarness h;
    // Something in the model to look at: 1000 x 500 units.
    h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 500.0, 0.0}));
    h.showSheet();
    REQUIRE(h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    h.executor.supply(at(20.0, 20.0));
    h.executor.supply(at(120.0, 80.0));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeText("1:50"));
    CHECK(outcome.status == CommandOutcome::Status::Completed);

    const auto windows = h.onSheet<ViewportEntity>();
    REQUIRE(windows.size() == 1);
    const ViewportEntity& window = *windows.front();
    CHECK(window.viewportId == 2);
    CHECK(window.center.x == doctest::Approx(70.0));
    CHECK(window.center.y == doctest::Approx(50.0));
    CHECK(window.width == doctest::Approx(100.0));
    CHECK(window.height == doctest::Approx(60.0));
    // 60 paper units tall at 1:50 shows 3000 model units.
    CHECK(window.viewHeight == doctest::Approx(3000.0));
    CHECK(window.modelPerPaper() == doctest::Approx(50.0));
    // Centred on the model.
    CHECK(window.viewCenter.x == doctest::Approx(500.0));
    CHECK(window.viewCenter.y == doctest::Approx(250.0));
    CHECK(window.isOn());
}

TEST_CASE("MVIEW fitted to the model shows all of it, whichever way it is wider") {
    SheetHarness h;
    // Wider than the window's shape: width decides how much height shows.
    h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 100.0, 0.0}));
    h.showSheet();
    REQUIRE(h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    h.executor.supply(at(0.0, 0.0));
    h.executor.supply(at(100.0, 50.0));
    h.executor.supply(InputValue::makeEmpty());
    const auto windows = h.onSheet<ViewportEntity>();
    REQUIRE(windows.size() == 1);
    // 1000 wide in a 2:1 window means 500 tall of view.
    CHECK(windows.front()->viewHeight == doctest::Approx(500.0));
}

TEST_CASE("MVIEW Fit fills the printable area and numbers viewports upward") {
    SheetHarness h;
    h.showSheet();
    REQUIRE(h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeKeyword("F"));
    h.executor.supply(InputValue::makeKeyword("Fit"));
    REQUIRE(h.onSheet<ViewportEntity>().size() == 1);
    const Box3 printable = h.layout->printableRect();
    const ViewportEntity& window = *h.onSheet<ViewportEntity>().front();
    CHECK(window.width == doctest::Approx(printable.width()));
    CHECK(window.height == doctest::Approx(printable.height()));

    REQUIRE(h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    h.executor.supply(at(10.0, 10.0));
    h.executor.supply(at(60.0, 40.0));
    h.executor.supply(InputValue::makeText("100"));
    REQUIRE(h.onSheet<ViewportEntity>().size() == 2);
    CHECK(h.onSheet<ViewportEntity>().back()->viewportId == 3);
    CHECK(h.onSheet<ViewportEntity>().back()->modelPerPaper() == doctest::Approx(100.0));
}

TEST_CASE("PAGESETUP sets paper, orientation and scale for the current sheet") {
    SheetHarness h;
    h.showSheet();
    REQUIRE(h.executor.execute("PAGESETUP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeText("A3"));
    h.executor.supply(InputValue::makeText("Portrait"));
    const CommandOutcome outcome = h.executor.supply(InputValue::makeText("1:2"));
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK(h.layout->plot.paperWidth == doctest::Approx(297.0));
    CHECK(h.layout->plot.paperHeight == doctest::Approx(420.0));
    CHECK_FALSE(h.layout->plot.isRotatedQuarterTurn());
    CHECK(h.layout->plot.numerator == doctest::Approx(1.0));
    CHECK(h.layout->plot.denominator == doctest::Approx(2.0));
    // The sheet the drawing now sees is A3 portrait.
    const Vec3 sheet = h.layout->plot.sheetSize();
    CHECK(sheet.x == doctest::Approx(297.0));
    CHECK(sheet.y == doctest::Approx(420.0));

    // Undo puts the old page back.
    REQUIRE(h.tm.undo());
    CHECK(h.layout->plot.paperWidth == doctest::Approx(210.0));
    CHECK(h.layout->plot.isRotatedQuarterTurn());
}

TEST_CASE("a sheet's viewport shows the model, not the sheet's own geometry") {
    // The shell indexes whatever is on screen. With a sheet showing, that is
    // the sheet -- and a viewport searching that index drew the sheet's own
    // frames and notes inside itself, at the viewport's scale, while the
    // model never appeared.
    SheetHarness h;
    const Handle modelLine =
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 500.0, 0.0})).handle;
    auto sheetLine = std::make_unique<LineEntity>(Vec3{10.0, 10.0, 0.0}, Vec3{50.0, 10.0, 0.0});
    const Handle noteLine = h.db->addEntity(std::move(sheetLine), *h.sheet).handle;
    auto window = std::make_unique<ViewportEntity>();
    window->viewportId = 2;
    window->center = {120.0, 85.0, 0.0};
    window->width = 200.0;
    window->height = 130.0;
    window->viewCenter = {500.0, 250.0, 0.0};
    window->viewHeight = 1300.0;
    h.db->addEntity(std::move(window), *h.sheet);

    // Indexed as the shell does it: the sheet on screen, the model beside it.
    render::SpatialIndex sheetIndex;
    render::SpatialIndex modelIndex;
    render::SceneBuilder builder{*h.db, sheetIndex};
    builder.reindex(*h.sheet);
    render::SceneBuilder modelBuilder{*h.db, modelIndex};
    modelBuilder.reindex(h.db->modelSpace());
    builder.setModelIndex(&modelIndex);

    const render::Viewport view = render::Viewport{}.withSize(1000, 700).zoomToFit(h.layout->sheetRect());
    const render::Scene scene = builder.buildLayout(*h.layout, view);

    bool modelSeen = false;
    bool sheetSeenThroughWindow = false;
    for (const render::DrawItem& item : scene.items) {
        if (!item.clip.has_value()) {
            continue;  // the sheet's own geometry, drawn unclipped
        }
        modelSeen = modelSeen || item.handle == modelLine;
        sheetSeenThroughWindow = sheetSeenThroughWindow || item.handle == noteLine;
    }
    CHECK(modelSeen);
    CHECK_FALSE(sheetSeenThroughWindow);
}

namespace {

/// A sheet with two viewports onto a model that has three layers.
struct TwoViewports {
    SheetHarness h;
    Handle first;
    Handle second;
    Handle demo;
    Handle newWork;
    Handle notes;

    TwoViewports() {
        demo = h.db->ensureLayer("A-DEMO").handle;
        newWork = h.db->ensureLayer("A-NEW").handle;
        notes = h.db->ensureLayer("A-NOTES").handle;
        h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{0.0, 0.0, 0.0}, Vec3{1000.0, 500.0, 0.0}));
        h.showSheet();
        for (const double x : {20.0, 120.0}) {
            REQUIRE(h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
            h.executor.supply(at(x, 20.0));
            h.executor.supply(at(x + 80.0, 80.0));
            REQUIRE(h.executor.supply(InputValue::makeText("1:50")).status ==
                    CommandOutcome::Status::Completed);
        }
        const auto windows = h.onSheet<ViewportEntity>();
        REQUIRE(windows.size() == 2);
        first = windows[0]->handle;
        second = windows[1]->handle;
    }

    [[nodiscard]] const ViewportEntity& window(Handle handle) {
        return *h.db->get<ViewportEntity>(handle);
    }
};

}  // namespace

TEST_CASE("VPLAYER freezes layers in every viewport, or in chosen ones, with wildcards") {
    TwoViewports t;
    REQUIRE(t.h.executor.execute("VPLAYER").status == CommandOutcome::Status::Running);
    CHECK(t.h.executor.currentRequest()->kind == InputRequest::Kind::Keyword);
    t.h.executor.supply(InputValue::makeKeyword("F"));
    t.h.executor.supply(InputValue::makeText("A-DEMO"));
    t.h.executor.supply(InputValue::makeKeyword("A"));          // all viewports
    CHECK(t.window(t.first).isLayerFrozen(t.demo));
    CHECK(t.window(t.second).isLayerFrozen(t.demo));

    // A wildcard and a list, in the second viewport only.
    t.h.executor.supply(InputValue::makeKeyword("F"));
    t.h.executor.supply(InputValue::makeText("A-N*, A-DEMO"));
    t.h.executor.supply(InputValue::makeKeyword("S"));
    t.h.executor.supply(InputValue::makeSelection({t.second}));
    CHECK_FALSE(t.window(t.first).isLayerFrozen(t.newWork));
    CHECK(t.window(t.second).isLayerFrozen(t.newWork));
    CHECK(t.window(t.second).isLayerFrozen(t.notes));
    CHECK(t.window(t.second).frozenLayerHandles.size() == 3);   // DEMO not added twice

    // Thaw one, reset the other, then Enter ends it with a count.
    t.h.executor.supply(InputValue::makeKeyword("T"));
    t.h.executor.supply(InputValue::makeText("A-DEMO"));
    t.h.executor.supply(InputValue::makeKeyword("S"));
    t.h.executor.supply(InputValue::makeSelection({t.first}));
    CHECK_FALSE(t.window(t.first).isLayerFrozen(t.demo));
    t.h.executor.supply(InputValue::makeKeyword("R"));
    t.h.executor.supply(InputValue::makeKeyword("S"));
    t.h.executor.supply(InputValue::makeSelection({t.second}));
    CHECK(t.window(t.second).frozenLayerHandles.empty());
    CHECK(t.h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);

    // Model space is not the place for it.
    t.h.showModel();
    CHECK(t.h.executor.execute("VPLAYER").status == CommandOutcome::Status::Failed);
}

TEST_CASE("VPLAYER with viewports selected first asks only for the layers") {
    TwoViewports t;
    t.h.selection.setSelection({t.first});
    REQUIRE(t.h.executor.execute("VPLAYER").status == CommandOutcome::Status::Running);
    t.h.executor.supply(InputValue::makeKeyword("F"));
    t.h.executor.supply(InputValue::makeText("A-NOTES"));
    CHECK(t.window(t.first).isLayerFrozen(t.notes));
    CHECK_FALSE(t.window(t.second).isLayerFrozen(t.notes));
    CHECK(t.h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);

    // Undo takes the freeze back with it.
    t.h.tm.undo();
    CHECK_FALSE(t.window(t.first).isLayerFrozen(t.notes));
}

TEST_CASE("MVIEW Lock locks and unlocks the view of chosen viewports") {
    TwoViewports t;
    REQUIRE(t.h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    t.h.executor.supply(InputValue::makeKeyword("L"));
    t.h.executor.supply(InputValue::makeKeyword("ON"));
    CHECK(t.h.executor.supply(InputValue::makeSelection({t.first, t.second})).status ==
          CommandOutcome::Status::Completed);
    CHECK(t.window(t.first).locked);
    CHECK(t.window(t.second).locked);

    REQUIRE(t.h.executor.execute("MVIEW").status == CommandOutcome::Status::Running);
    t.h.executor.supply(InputValue::makeKeyword("L"));
    t.h.executor.supply(InputValue::makeKeyword("OFF"));
    CHECK(t.h.executor.supply(InputValue::makeSelection({t.second})).status ==
          CommandOutcome::Status::Completed);
    CHECK(t.window(t.first).locked);
    CHECK_FALSE(t.window(t.second).locked);
}

TEST_CASE("PAGESETUP keeps what Enter leaves alone, and refuses an ambiguous paper") {
    SheetHarness h;
    h.showSheet();
    REQUIRE(h.executor.execute("PAGESETUP").status == CommandOutcome::Status::Running);
    CHECK(h.executor.supply(InputValue::makeText("L")).status == CommandOutcome::Status::Failed);

    REQUIRE(h.executor.execute("PAGESETUP").status == CommandOutcome::Status::Running);
    h.executor.supply(InputValue::makeEmpty());
    h.executor.supply(InputValue::makeEmpty());
    CHECK(h.executor.supply(InputValue::makeEmpty()).status == CommandOutcome::Status::Completed);
    CHECK(h.layout->plot.paperWidth == doctest::Approx(210.0));
    CHECK(h.layout->plot.isRotatedQuarterTurn());
    CHECK(h.layout->plot.denominator == doctest::Approx(1.0));
}
