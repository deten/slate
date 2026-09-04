#include <cmath>
#include "test_helpers.hpp"

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/geom/Operations.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/snap/ObjectSnap.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    [[nodiscard]] std::vector<Entity*> entities() { return db->entitiesIn(db->modelSpace()); }
    [[nodiscard]] std::size_t count() { return entities().size(); }

    [[nodiscard]] BlockTableRecord& addBlock(std::string name) {
        auto block = std::make_unique<BlockTableRecord>();
        block->name = std::move(name);
        return db->addBlock(std::move(block));
    }

    template <typename T>
    [[nodiscard]] std::vector<T*> ofType() {
        std::vector<T*> result;
        for (Entity* entity : entities()) {
            if (auto* typed = dynamic_cast<T*>(entity)) {
                result.push_back(typed);
            }
        }
        return result;
    }
};

[[nodiscard]] InputValue point(double x, double y) {
    return InputValue::makePoint(Vec3{x, y});
}

/// A no-op command instance for registry tests that never execute.
class NullCommand final : public CommandInstance {
public:
    StepResult start(CommandContext&) override { return StepResult::done(); }
    StepResult supply(const InputValue&, CommandContext&) override { return StepResult::done(); }
};

[[nodiscard]] CommandDefinition stubCommand(std::string id, std::string name,
                                            std::vector<std::string> aliases = {}) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = definition.name;
    definition.create = [] { return std::make_unique<NullCommand>(); };
    return definition;
}

}  // namespace

TEST_SUITE("CommandRegistry") {
    TEST_CASE("registers the built-in commands") {
        const auto registry = createStandardRegistry();
        CHECK(registry->size() > 10);
        CHECK(registry->lookup("LINE") != nullptr);
        CHECK(registry->lookup("line") != nullptr);
    }

    TEST_CASE("resolves aliases") {
        const auto registry = createStandardRegistry();
        CHECK(registry->lookup("L")->name == "LINE");
        CHECK(registry->lookup("REC")->name == "RECTANG");
        CHECK(registry->lookup("co")->name == "COPY");
    }

    TEST_CASE("refuses duplicate ids and names") {
        auto registry = createStandardRegistry();
        CHECK_THROWS_AS((void)registry->registerCommand(stubCommand("cad.draw.line", "SOMETHING")),
                        std::invalid_argument);
        CHECK_THROWS_AS((void)registry->registerCommand(stubCommand("unique.id", "LINE")),
                        std::invalid_argument);
    }

    TEST_CASE("rolls back a partial batch registration") {
        auto registry = createStandardRegistry();
        const std::size_t before = registry->size();

        std::vector<CommandDefinition> batch;
        batch.push_back(stubCommand("plugin.ok", "PLUGINOK"));
        // Collides, so the whole batch must be undone.
        batch.push_back(stubCommand("plugin.bad", "LINE"));

        CHECK_THROWS_AS((void)registry->registerAll(std::move(batch)), std::invalid_argument);
        CHECK(registry->size() == before);
        CHECK(registry->lookup("PLUGINOK") == nullptr);
    }

    TEST_CASE("lets a plugin add a command and remove it again") {
        auto registry = createStandardRegistry();
        const std::size_t before = registry->size();
        {
            CommandRegistration token =
                registry->registerCommand(stubCommand("plugin.hello", "HELLO", {"HI"}));
            CHECK(registry->lookup("HI")->name == "HELLO");
        }
        // The token unregisters on destruction, which is what makes plugin
        // unloading possible without the registry tracking ownership.
        CHECK(registry->size() == before);
        CHECK(registry->lookup("HELLO") == nullptr);
    }

    TEST_CASE("ranks an exact alias above a prefix match on another command") {
        const auto registry = createStandardRegistry();
        // "L" is LINE's alias; it must not be beaten by a prefix hit.
        const auto results = registry->search("L");
        REQUIRE_FALSE(results.empty());
        CHECK(results.front().command->name == "LINE");
    }

    TEST_CASE("prefers the shorter name for a prefix match") {
        auto registry = createStandardRegistry();
        registry->registerCommand(stubCommand("test.linetype", "LINETYPE")).release();
        const auto results = registry->search("LIN");
        REQUIRE_FALSE(results.empty());
        CHECK(results.front().command->name == "LINE");
    }

    TEST_CASE("autocompletes to the canonical name, not the alias") {
        const auto registry = createStandardRegistry();
        CHECK(registry->autocomplete("LI") == "LINE");
        // "REC" matches RECTANG through its alias, but the suggestion is the
        // real command name so the user learns it.
        CHECK(registry->autocomplete("REC") == "RECTANG");
    }

    TEST_CASE("suggests the shortest candidate when a prefix is ambiguous") {
        const auto registry = createStandardRegistry();
        // Both REDO and RECTANG start with "RE"; the shorter one needs fewer
        // keystrokes to disambiguate.
        CHECK(registry->autocomplete("RE") == "REDO");
        CHECK(registry->autocomplete("ZZZ").empty());
    }

    TEST_CASE("groups commands by category for menus") {
        const auto registry = createStandardRegistry();
        const auto groups = registry->byCategory();
        CHECK(groups.at("Draw").size() > 3);
        CHECK(groups.at("Modify").size() > 3);
    }
}

TEST_SUITE("Executor lifecycle") {
    TEST_CASE("reports an unknown command with a suggestion") {
        Harness h;
        const CommandOutcome outcome = h.executor.execute("LIEN");
        CHECK(outcome.status == CommandOutcome::Status::Unknown);
    }

    TEST_CASE("keeps the transaction open across interactive steps") {
        Harness h;
        // This is the exact case a synchronous transaction scope gets wrong.
        CHECK(h.executor.execute("LINE").status == CommandOutcome::Status::Running);
        CHECK(h.executor.isRunning());

        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        // The entity exists before the command has finished.
        CHECK(h.count() == 1);

        const CommandOutcome outcome = h.executor.supply(InputValue::makeEmpty());
        CHECK(outcome.status == CommandOutcome::Status::Completed);
        CHECK(h.tm.canUndo());
    }

    TEST_CASE("records one undo step for a whole multi-segment line") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(point(10, 10));
        h.executor.supply(point(0, 10));
        h.executor.supply(InputValue::makeEmpty());

        CHECK(h.count() == 3);
        CHECK(h.tm.undoDepth() == 1);
        h.tm.undo();
        CHECK(h.count() == 0);
    }

    TEST_CASE("discards an incomplete object when cancelled before commit") {
        Harness h;
        h.executor.execute("CIRCLE");
        h.executor.supply(point(0, 0));
        const CommandOutcome outcome = h.executor.supply(InputValue::makeCancel());

        CHECK(outcome.status == CommandOutcome::Status::Cancelled);
        CHECK(h.count() == 0);
        CHECK_FALSE(h.tm.canUndo());
    }

    TEST_CASE("keeps committed geometry when a repeating command is cancelled") {
        // Escape ends LINE but keeps the segments already drawn, as other CAD software
        // does. Only the rubber-band segment in progress is abandoned.
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(point(10, 10));
        CHECK(h.count() == 2);

        const CommandOutcome outcome = h.executor.supply(InputValue::makeCancel());
        CHECK(outcome.status == CommandOutcome::Status::Completed);
        CHECK(h.count() == 2);
        CHECK(h.tm.undoDepth() == 1);
    }

    TEST_CASE("always exits on Escape even if a command keeps prompting") {
        Harness h;
        CommandDefinition stubborn = stubCommand("test.stubborn", "STUBBORN");
        stubborn.create = [] {
            class Stubborn final : public CommandInstance {
            public:
                StepResult start(CommandContext&) override { return ask(); }
                StepResult supply(const InputValue&, CommandContext&) override { return ask(); }

            private:
                static StepResult ask() {
                    InputRequest request;
                    request.kind = InputRequest::Kind::Point;
                    request.prompt = "forever";
                    return StepResult::ask(request);
                }
            };
            return std::make_unique<Stubborn>();
        };
        h.registry->registerCommand(std::move(stubborn)).release();

        h.executor.execute("STUBBORN");
        const CommandOutcome outcome = h.executor.supply(InputValue::makeCancel());
        CHECK(outcome.status == CommandOutcome::Status::Cancelled);
        CHECK_FALSE(h.executor.isRunning());
    }

    TEST_CASE("cancels the running command when another is started") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(5, 0));

        h.executor.execute("CIRCLE");
        // The interrupted line was rolled back.
        CHECK(h.ofType<LineEntity>().empty());

        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(5.0));
        CHECK(h.count() == 1);
    }

    TEST_CASE("rolls back and reports when a command throws") {
        Harness h;
        CommandDefinition boom = stubCommand("test.explode", "BOOM");
        boom.create = [] {
            class Boom final : public CommandInstance {
            public:
                StepResult start(CommandContext& context) override {
                    context.transaction().addToModelSpace(
                        std::make_unique<PointEntity>(Vec3::zero()));
                    throw std::runtime_error("deliberate failure");
                }
                StepResult supply(const InputValue&, CommandContext&) override {
                    return StepResult::done();
                }
            };
            return std::make_unique<Boom>();
        };
        h.registry->registerCommand(std::move(boom)).release();

        const CommandOutcome outcome = h.executor.execute("BOOM");
        CHECK(outcome.status == CommandOutcome::Status::Failed);
        CHECK(h.count() == 0);
        CHECK_FALSE(h.tm.canUndo());
    }

    TEST_CASE("does not record an undo entry for a command that changed nothing") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(InputValue::makeEmpty());
        CHECK_FALSE(h.tm.canUndo());
    }

    TEST_CASE("tracks the last command for Enter-to-repeat") {
        Harness h;
        h.executor.execute("POINT");
        h.executor.supply(point(1, 1));
        h.executor.supply(InputValue::makeEmpty());

        REQUIRE(h.executor.lastCommand() != nullptr);
        CHECK(h.executor.lastCommand()->name == "POINT");

        h.executor.repeatLast();
        h.executor.supply(point(2, 2));
        h.executor.supply(InputValue::makeEmpty());
        CHECK(h.count() == 2);
    }

    TEST_CASE("logs prompts and input to the command history") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeEmpty());

        const auto& history = h.executor.history();
        const auto has = [&](HistoryEntry::Kind kind) {
            return std::any_of(history.begin(), history.end(),
                               [kind](const HistoryEntry& e) { return e.kind == kind; });
        };
        CHECK(has(HistoryEntry::Kind::Command));
        CHECK(has(HistoryEntry::Kind::Prompt));
        CHECK(has(HistoryEntry::Kind::Input));
    }
}

TEST_SUITE("Draw commands") {
    TEST_CASE("LINE closes the chain on the Close keyword") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(point(10, 10));
        h.executor.supply(InputValue::makeKeyword("Close"));
        // Two drawn plus the closing segment.
        CHECK(h.count() == 3);
    }

    TEST_CASE("LINE undoes the last segment on the Undo keyword") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(point(20, 0));
        CHECK(h.count() == 2);

        h.executor.supply(InputValue::makeKeyword("U"));
        CHECK(h.count() == 1);
        h.executor.supply(InputValue::makeEmpty());
    }

    TEST_CASE("LINE ignores a zero-length segment from a double-click") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(5, 5));
        h.executor.supply(point(5, 5));
        h.executor.supply(point(10, 5));
        h.executor.supply(InputValue::makeEmpty());
        CHECK(h.count() == 1);
    }

    TEST_CASE("CIRCLE draws from a centre and radius") {
        Harness h;
        h.executor.execute("C");
        h.executor.supply(point(5, 5));
        h.executor.supply(InputValue::makeNumber(3.0));

        const auto circles = h.ofType<CircleEntity>();
        REQUIRE(circles.size() == 1);
        CHECK(circles[0]->center.equals(Vec3{5, 5}));
        CHECK(circles[0]->radius == doctest::Approx(3.0));
    }

    TEST_CASE("CIRCLE supports the two-point diameter construction") {
        Harness h;
        h.executor.execute("CIRCLE");
        h.executor.supply(InputValue::makeKeyword("2P"));
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));

        const auto circles = h.ofType<CircleEntity>();
        REQUIRE(circles.size() == 1);
        CHECK(circles[0]->center.equals(Vec3{5, 0}));
        CHECK(circles[0]->radius == doctest::Approx(5.0));
    }

    TEST_CASE("CIRCLE reports collinear three-point input rather than drawing nonsense") {
        Harness h;
        h.executor.execute("CIRCLE");
        h.executor.supply(InputValue::makeKeyword("3P"));
        h.executor.supply(point(0, 0));
        h.executor.supply(point(1, 0));
        const CommandOutcome outcome = h.executor.supply(point(2, 0));

        CHECK(outcome.status == CommandOutcome::Status::Failed);
        CHECK(outcome.message.find("collinear") != std::string::npos);
        CHECK(h.count() == 0);
    }

    TEST_CASE("CIRCLE rejects a zero radius") {
        Harness h;
        h.executor.execute("CIRCLE");
        h.executor.supply(point(0, 0));
        const CommandOutcome outcome = h.executor.supply(InputValue::makeNumber(0.0));
        CHECK(outcome.status == CommandOutcome::Status::Failed);
        CHECK(h.count() == 0);
    }

    TEST_CASE("ARC draws through three points") {
        Harness h;
        h.executor.execute("ARC");
        h.executor.supply(point(1, 0));
        h.executor.supply(point(0, 1));
        h.executor.supply(point(-1, 0));

        const auto arcs = h.ofType<ArcEntity>();
        REQUIRE(arcs.size() == 1);
        CHECK(arcs[0]->radius == doctest::Approx(1.0));
        CHECK(arcs[0]->geometry().containsPoint(Vec3{0, 1}, Tolerance{1e-6, 1e-6}));
    }

    TEST_CASE("RECTANG draws a closed four-vertex polyline") {
        Harness h;
        h.executor.execute("REC");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(20, 10));

        const auto rects = h.ofType<PolylineEntity>();
        REQUIRE(rects.size() == 1);
        CHECK(rects[0]->closed);
        CHECK(rects[0]->vertices.size() == 4);
        CHECK(rects[0]->geometry().area() == doctest::Approx(200.0));
    }

    TEST_CASE("PLINE creates one entity, not several") {
        Harness h;
        h.executor.execute("PL");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(point(10, 10));
        h.executor.supply(InputValue::makeEmpty());

        CHECK(h.count() == 1);
        const auto polys = h.ofType<PolylineEntity>();
        REQUIRE(polys.size() == 1);
        CHECK(polys[0]->vertices.size() == 3);
    }

    TEST_CASE("PLINE draws arc segments tangent to what came before") {
        // The reason arc mode is worth having: a chain of arcs that flows
        // instead of kinking at every join. Two quarter turns off a straight
        // run east make a half round, and both bulges are tan(45/2).
        Harness h;
        h.executor.execute("PL");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(200, 0));
        h.executor.supply(InputValue::makeKeyword("Arc"));
        h.executor.supply(point(400, 200));
        h.executor.supply(point(200, 400));
        h.executor.supply(InputValue::makeKeyword("Line"));
        h.executor.supply(point(0, 400));
        h.executor.supply(InputValue::makeEmpty());

        const auto polys = h.ofType<PolylineEntity>();
        REQUIRE(polys.size() == 1);
        REQUIRE(polys[0]->vertices.size() == 5);
        // The straight run leaves no bulge, and the mode switch back ends it.
        CHECK(polys[0]->vertices[0].bulge == doctest::Approx(0.0));
        CHECK(polys[0]->vertices[1].bulge == doctest::Approx(std::tan(kPi / 8.0)));
        CHECK(polys[0]->vertices[2].bulge == doctest::Approx(std::tan(kPi / 8.0)));
        CHECK(polys[0]->vertices[3].bulge == doctest::Approx(0.0));
    }

    TEST_CASE("PLINE arc segments join without a kink") {
        // Tangency is the property, so it is what gets checked -- the direction
        // the first arc arrives in is the direction the second leaves in.
        Harness h;
        h.executor.execute("PL");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(100, 0));
        h.executor.supply(InputValue::makeKeyword("Arc"));
        h.executor.supply(point(180, 80));
        // Off the tangent on purpose: a point that lies along it describes a
        // straight segment, which is right but tests nothing about tangency.
        h.executor.supply(point(260, 160));
        h.executor.supply(InputValue::makeEmpty());

        const auto polys = h.ofType<PolylineEntity>();
        REQUIRE(polys.size() == 1);
        const std::vector<PolylineVertex>& v = polys[0]->vertices;
        REQUIRE(v.size() == 4);

        const std::optional<Arc> first = Arc::fromBulge(v[1].position, v[2].position, v[1].bulge);
        const std::optional<Arc> second = Arc::fromBulge(v[2].position, v[3].position, v[2].bulge);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        // Sharing a centre-line direction at the shared point is tangency: the
        // radius to the join points the same way from both centres.
        const Vec3 toJoin = (v[2].position - first->center).normalized();
        const Vec3 fromJoin = (v[2].position - second->center).normalized();
        CHECK(std::abs(toJoin.x * fromJoin.y - toJoin.y * fromJoin.x) < 1e-9);
    }

    TEST_CASE("PLINE Undo takes back the bulge with the vertex") {
        // The bulge lives on the vertex the segment starts at, so undoing a
        // point that ended an arc has to clear the curvature that pointed at
        // it -- otherwise the polyline keeps a bend to a vertex that is gone.
        Harness h;
        h.executor.execute("PL");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(100, 0));
        h.executor.supply(InputValue::makeKeyword("Arc"));
        h.executor.supply(point(180, 80));
        h.executor.supply(InputValue::makeKeyword("Undo"));
        h.executor.supply(InputValue::makeKeyword("Line"));
        h.executor.supply(point(200, 0));
        h.executor.supply(InputValue::makeEmpty());

        const auto polys = h.ofType<PolylineEntity>();
        REQUIRE(polys.size() == 1);
        REQUIRE(polys[0]->vertices.size() == 3);
        CHECK(polys[0]->vertices[1].bulge == doctest::Approx(0.0));
    }

    TEST_CASE("POLYGON inscribes vertices exactly on the circle") {
        Harness h;
        h.executor.execute("POLYGON");
        h.executor.supply(InputValue::makeNumber(6.0));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeKeyword("Inscribed"));
        h.executor.supply(InputValue::makeNumber(10.0));

        const auto polys = h.ofType<PolylineEntity>();
        REQUIRE(polys.size() == 1);
        CHECK(polys[0]->vertices.size() == 6);
        for (const PolylineVertex& vertex : polys[0]->vertices) {
            CHECK(vertex.position.length2() == doctest::Approx(10.0));
        }
    }

    TEST_CASE("POLYGON circumscribes with vertices outside the circle") {
        // A circumscribed polygon touches the circle at its edge midpoints, so
        // its vertices sit further out by 1 / cos(pi / n).
        const Polyline poly = regularPolygon(Vec3::zero(), 10.0, 6, false);
        const double expected = 10.0 / std::cos(kPi / 6.0);
        for (const PolylineVertex& vertex : poly.vertices) {
            CHECK(vertex.position.length2() == doctest::Approx(expected));
        }
    }
}

TEST_SUITE("Modify commands") {
    TEST_CASE("MOVE translates the selection") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.selection.setSelection({line.handle});

        h.executor.execute("MOVE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(5, 5));

        auto* moved = h.db->get<LineEntity>(line.handle);
        REQUIRE(moved != nullptr);
        CHECK(moved->start.equals(Vec3{5, 5}));
        CHECK(moved->end.equals(Vec3{15, 5}));
    }

    TEST_CASE("COPY leaves the original in place") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.selection.setSelection({line.handle});

        h.executor.execute("COPY");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(0, 20));

        CHECK(h.count() == 2);
        CHECK(h.db->get<LineEntity>(line.handle)->start.equals(Vec3::zero()));
    }

    TEST_CASE("ERASE deletes and undo restores") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.selection.setSelection({line.handle});

        h.executor.execute("ERASE");
        CHECK(h.count() == 0);

        h.tm.undo();
        CHECK(h.count() == 1);
    }

    TEST_CASE("ERASE refuses to touch a locked layer") {
        Harness h;
        Layer& locked = h.db->ensureLayer("Locked");
        locked.isLocked = true;

        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        line.layerHandle = locked.handle;
        h.selection.setSelection({line.handle});

        const CommandOutcome outcome = h.executor.execute("ERASE");
        CHECK(outcome.status == CommandOutcome::Status::Cancelled);
        CHECK(h.count() == 1);
    }

    TEST_CASE("ROTATE turns the selection about a base point") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.selection.setSelection({line.handle});

        h.executor.execute("ROTATE");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(kPi / 2.0));

        auto* rotated = h.db->get<LineEntity>(line.handle);
        REQUIRE(rotated != nullptr);
        CHECK(rotated->end.x == doctest::Approx(0.0).scale(10.0));
        CHECK(rotated->end.y == doctest::Approx(10.0));
    }

    TEST_CASE("SCALE keeps a circle circular under a uniform factor") {
        Harness h;
        Entity& circle =
            h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3::zero(), 5.0));
        h.selection.setSelection({circle.handle});

        h.executor.execute("SCALE");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(2.0));

        const auto circles = h.ofType<CircleEntity>();
        REQUIRE(circles.size() == 1);
        CHECK(circles[0]->radius == doctest::Approx(10.0));
    }

    TEST_CASE("MIRROR duplicates without erasing the original") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{0, 5}, Vec3{10, 5}));
        h.selection.setSelection({line.handle});

        h.executor.execute("MIRROR");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(1, 0));

        CHECK(h.count() == 2);
        CHECK(h.db->get<LineEntity>(line.handle)->start.equals(Vec3{0, 5}));
    }

    TEST_CASE("UNDO and REDO work as commands") {
        Harness h;
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(InputValue::makeEmpty());
        CHECK(h.count() == 1);

        h.executor.execute("UNDO");
        CHECK(h.count() == 0);

        h.executor.execute("REDO");
        CHECK(h.count() == 1);
    }

    TEST_CASE("UNDO reports gracefully when there is nothing to undo") {
        Harness h;
        const CommandOutcome outcome = h.executor.execute("UNDO");
        CHECK(outcome.status == CommandOutcome::Status::Cancelled);
        CHECK(outcome.message.find("Nothing to undo") != std::string::npos);
    }

    TEST_CASE("OFFSET creates a parallel line on the picked side") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));

        h.executor.execute("OFFSET");
        h.executor.supply(InputValue::makeNumber(5.0));
        h.executor.supply(InputValue::makeEntity(line.handle));
        // Pick above the line, so the offset must land above it.
        h.executor.supply(point(5, 100));
        h.executor.supply(InputValue::makeCancel());

        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 2);
        const auto* created =
            lines[0]->handle == line.handle ? lines[1] : lines[0];
        CHECK(created->start.y == doctest::Approx(5.0));
    }

    TEST_CASE("OFFSET puts the copy below when the pick is below") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));

        h.executor.execute("OFFSET");
        h.executor.supply(InputValue::makeNumber(5.0));
        h.executor.supply(InputValue::makeEntity(line.handle));
        h.executor.supply(point(5, -100));
        h.executor.supply(InputValue::makeCancel());

        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 2);
        const auto* created = lines[0]->handle == line.handle ? lines[1] : lines[0];
        CHECK(created->start.y == doctest::Approx(-5.0));
    }

    TEST_CASE("TRIM cuts a line back to a crossing edge") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        Entity& vertical =
            h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3{5, -5}, Vec3{5, 5}));

        h.selection.setSelection({vertical.handle});
        h.executor.execute("TRIM");
        // Pick the right-hand portion, which is the part to remove.
        h.executor.supply(point(8, 0));
        h.executor.supply(InputValue::makeEmpty());

        const auto lines = h.ofType<LineEntity>();
        // The vertical edge plus the surviving left-hand piece.
        REQUIRE(lines.size() == 2);
        const auto* survivor =
            lines[0]->handle == vertical.handle ? lines[1] : lines[0];
        CHECK(survivor->start.equals(Vec3::zero()));
        CHECK(survivor->end.x == doctest::Approx(5.0));
    }

    TEST_CASE("FILLET rounds a corner between two lines") {
        Harness h;
        Entity& a = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        Entity& b = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{0, 10}));

        h.executor.execute("FILLET");
        h.executor.supply(InputValue::makeNumber(2.0));
        h.executor.supply(point(8, 0));
        h.executor.supply(point(0, 8));

        const auto arcs = h.ofType<ArcEntity>();
        REQUIRE(arcs.size() == 1);
        CHECK(arcs[0]->radius == doctest::Approx(2.0));
        // The originals were replaced by trimmed versions.
        CHECK(h.db->getObject(a.handle)->erased);
        CHECK(h.db->getObject(b.handle)->erased);
    }

    TEST_CASE("CHAMFER bevels a corner, symmetrically or not") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{0, 10}));

        h.executor.execute("CHAMFER");
        h.executor.supply(InputValue::makeNumber(2.0));
        h.executor.supply(InputValue::makeNumber(4.0));
        h.executor.supply(point(8, 0));
        h.executor.supply(point(0, 8));

        // Three lines survive: the two trimmed legs and the bevel between them.
        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 3);

        // The bevel runs from 2 along the X leg to 4 up the Y leg.
        const auto bevel = std::find_if(lines.begin(), lines.end(), [](const LineEntity* line) {
            return line->start.x > 1.0 && line->start.x < 3.0 && line->end.y > 3.0;
        });
        REQUIRE(bevel != lines.end());
        CHECK((*bevel)->start.x == doctest::Approx(2.0));
        CHECK((*bevel)->start.y == doctest::Approx(0.0));
        CHECK((*bevel)->end.x == doctest::Approx(0.0));
        CHECK((*bevel)->end.y == doctest::Approx(4.0));
    }

    TEST_CASE("CHAMFER refuses anything that is not two lines") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        h.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{0, 5}, 2.0));

        h.executor.execute("CHAMFER");
        h.executor.supply(InputValue::makeNumber(1.0));
        h.executor.supply(InputValue::makeNumber(1.0));
        h.executor.supply(point(5, 0));
        h.executor.supply(point(0, 7));

        // Refused rather than approximated: no geometry was changed.
        CHECK(h.ofType<LineEntity>().size() == 1);
        CHECK(h.ofType<CircleEntity>().size() == 1);
    }

    TEST_CASE("BREAK removes the span between two points") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));

        h.executor.execute("BREAK");
        h.executor.supply(point(5, 0));   // select
        h.executor.supply(point(3, 0));   // first break point
        h.executor.supply(point(7, 0));   // second

        CHECK(h.db->getObject(line.handle)->erased);
        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 2);

        // The gap really is a gap: nothing spans 3..7.
        for (const LineEntity* piece : lines) {
            const double low = std::min(piece->start.x, piece->end.x);
            const double high = std::max(piece->start.x, piece->end.x);
            CHECK_FALSE((low < 3.0 - 1e-9 && high > 7.0 + 1e-9));
        }
    }

    TEST_CASE("BREAK with no second point splits and keeps both halves") {
        Harness h;
        h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));

        h.executor.execute("BREAK");
        h.executor.supply(point(4, 0));
        h.executor.supply(point(4, 0));
        h.executor.supply(InputValue::makeEmpty());

        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 2);
        // Total length is preserved, which is what distinguishes a split from
        // a break.
        double total = 0.0;
        for (const LineEntity* piece : lines) {
            total += piece->start.distanceTo(piece->end);
        }
        CHECK(total == doctest::Approx(10.0));
    }

    TEST_CASE("JOIN merges touching lines into one polyline") {
        Harness h;
        Entity& a = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        Entity& b = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{10, 0}, Vec3{10, 5}));

        h.executor.execute("JOIN");
        h.executor.supply(InputValue::makeSelection({a.handle, b.handle}));

        const auto polylines = h.ofType<PolylineEntity>();
        REQUIRE(polylines.size() == 1);
        CHECK(polylines[0]->vertices.size() == 3);
        CHECK(h.db->getObject(a.handle)->erased);
        CHECK(h.db->getObject(b.handle)->erased);
    }

    TEST_CASE("JOIN reverses a curve that meets end to end the other way round") {
        Harness h;
        Entity& a = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        // Drawn towards the shared point rather than away from it.
        Entity& b = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{10, 5}, Vec3{10, 0}));

        h.executor.execute("JOIN");
        h.executor.supply(InputValue::makeSelection({a.handle, b.handle}));

        const auto polylines = h.ofType<PolylineEntity>();
        REQUIRE(polylines.size() == 1);
        CHECK(polylines[0]->vertices.size() == 3);
    }

    TEST_CASE("JOIN refuses objects that do not meet") {
        Harness h;
        Entity& a = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));
        Entity& b = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{50, 50}, Vec3{60, 50}));

        h.executor.execute("JOIN");
        h.executor.supply(InputValue::makeSelection({a.handle, b.handle}));

        CHECK(h.ofType<PolylineEntity>().empty());
        CHECK_FALSE(h.db->getObject(a.handle)->erased);
        CHECK_FALSE(h.db->getObject(b.handle)->erased);
    }

    TEST_CASE("EXPLODE turns a polyline into its segments") {
        Harness h;
        Entity& polyline = h.db->addToModelSpace(std::make_unique<PolylineEntity>(
            PolylineEntity::fromPolyline(Polyline::fromPoints({Vec3::zero(), Vec3{10, 0},
                                                               Vec3{10, 5}}))));

        h.executor.execute("EXPLODE");
        h.executor.supply(InputValue::makeSelection({polyline.handle}));

        CHECK(h.db->getObject(polyline.handle)->erased);
        CHECK(h.ofType<LineEntity>().size() == 2);
    }

    TEST_CASE("EXPLODE places a block's contents under the insert's transform") {
        Harness h;
        BlockTableRecord& block = h.addBlock("WIDGET");
        h.db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}), block);

        auto insert = std::make_unique<InsertEntity>(block.handle, Vec3{100, 100});
        insert->scale = Vec3{2.0, 2.0, 1.0};
        Entity& reference = h.db->addToModelSpace(std::move(insert));

        h.executor.execute("EXPLODE");
        h.executor.supply(InputValue::makeSelection({reference.handle}));

        CHECK(h.db->getObject(reference.handle)->erased);
        const auto lines = h.ofType<LineEntity>();
        REQUIRE(lines.size() == 1);
        // Placed and scaled, not dropped at the origin at unit size.
        CHECK(lines[0]->start.x == doctest::Approx(100.0));
        CHECK(lines[0]->start.y == doctest::Approx(100.0));
        CHECK(lines[0]->end.x == doctest::Approx(102.0));
    }

    TEST_CASE("EXPLODE goes one level deep, keeping nested blocks as blocks") {
        Harness h;
        BlockTableRecord& inner = h.addBlock("INNER");
        h.db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}), inner);

        BlockTableRecord& outer = h.addBlock("OUTER");
        h.db->addEntity(std::make_unique<InsertEntity>(inner.handle, Vec3::zero()), outer);

        Entity& reference =
            h.db->addToModelSpace(std::make_unique<InsertEntity>(outer.handle, Vec3::zero()));

        h.executor.execute("EXPLODE");
        h.executor.supply(InputValue::makeSelection({reference.handle}));

        // The nesting the drawing was built with survives one explode.
        CHECK(h.ofType<InsertEntity>().size() == 1);
        CHECK(h.ofType<LineEntity>().empty());
    }

    TEST_CASE("STRETCH moves only the endpoints inside the crossing window") {
        Harness h;
        auto* line = static_cast<LineEntity*>(&h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0})));

        h.executor.execute("STRETCH");
        h.executor.supply(InputValue::makeSelection({line->handle}));
        // A window around the right-hand end only.
        h.executor.supply(point(8, -1));
        h.executor.supply(point(12, 1));
        h.executor.supply(point(0, 0));
        h.executor.supply(point(0, 5));

        // The end inside the window moved; the one outside did not. That
        // partial behaviour is the whole point of STRETCH.
        CHECK(line->start.equals(Vec3::zero()));
        CHECK(line->end.x == doctest::Approx(10.0));
        CHECK(line->end.y == doctest::Approx(5.0));
    }

    TEST_CASE("STRETCH moves an object bodily when the window encloses it") {
        Harness h;
        auto* line = static_cast<LineEntity*>(&h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0})));

        h.executor.execute("STRETCH");
        h.executor.supply(InputValue::makeSelection({line->handle}));
        h.executor.supply(point(-1, -1));
        h.executor.supply(point(11, 1));
        h.executor.supply(point(0, 0));
        h.executor.supply(point(0, 5));

        CHECK(line->start.y == doctest::Approx(5.0));
        CHECK(line->end.y == doctest::Approx(5.0));
    }

    TEST_CASE("STRETCH undoes as one operation") {
        Harness h;
        auto* line = static_cast<LineEntity*>(&h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0})));

        h.executor.execute("STRETCH");
        h.executor.supply(InputValue::makeSelection({line->handle}));
        h.executor.supply(point(8, -1));
        h.executor.supply(point(12, 1));
        h.executor.supply(point(0, 0));
        h.executor.supply(point(0, 5));
        REQUIRE(line->end.y == doctest::Approx(5.0));

        h.tm.undo();
        const auto* after = h.db->get<LineEntity>(line->handle);
        REQUIRE(after != nullptr);
        CHECK(after->end.y == doctest::Approx(0.0));
    }
}

TEST_SUITE("Text commands") {
    TEST_CASE("TEXT places a line at the point, height and rotation given") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(10, 20));
        h.executor.supply(InputValue::makeNumber(5.0));           // height
        h.executor.supply(InputValue::makeNumber(kPi / 2.0));     // rotation, radians
        h.executor.supply(InputValue::makeText("SECTION A-A"));
        h.executor.supply(InputValue::makeEmpty());               // finish

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->contents == "SECTION A-A");
        CHECK(texts[0]->height == doctest::Approx(5.0));
        CHECK(texts[0]->rotation == doctest::Approx(kPi / 2.0));
        CHECK_POINT(texts[0]->position, Vec3(10, 20));
        CHECK_FALSE(texts[0]->styleHandle.isNull());
    }

    TEST_CASE("TEXT Justify hangs the text off its point the way named") {
        Harness h;
        h.executor.execute("TEXT");
        CHECK(h.executor.currentRequest()->prompt.find("[Justify]") != std::string::npos);
        h.executor.supply(InputValue::makeKeyword("J"));
        CHECK(h.executor.currentRequest()->kind == InputRequest::Kind::Keyword);
        h.executor.supply(InputValue::makeKeyword("MC"));
        CHECK(h.executor.currentRequest()->prompt.find("middle center point") != std::string::npos);
        h.executor.supply(point(40, 30));
        h.executor.supply(InputValue::makeNumber(5.0));
        h.executor.supply(InputValue::makeNumber(0.0));
        h.executor.supply(InputValue::makeText("ROOM"));
        h.executor.supply(InputValue::makeEmpty());

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->horizontalAlignment == TextHorizontalAlignment::Center);
        CHECK(texts[0]->verticalAlignment == TextVerticalAlignment::Middle);
        REQUIRE(texts[0]->alignmentPoint.has_value());
        CHECK_POINT(*texts[0]->alignmentPoint, Vec3(40, 30));
        // Centred on the point, so the box straddles it.
        const Box3 box = texts[0]->bounds();
        CHECK(box.min.x < 40.0);
        CHECK(box.max.x > 40.0);
        CHECK(box.min.y < 30.0);
        CHECK(box.max.y > 30.0);
    }

    TEST_CASE("TEXT Justify Middle is Middle, not one of the middle row") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(InputValue::makeKeyword("J"));
        h.executor.supply(InputValue::makeText("M"));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(5.0));
        h.executor.supply(InputValue::makeNumber(0.0));
        h.executor.supply(InputValue::makeText("X"));
        h.executor.supply(InputValue::makeEmpty());
        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->horizontalAlignment == TextHorizontalAlignment::Middle);
        CHECK(texts[0]->verticalAlignment == TextVerticalAlignment::Baseline);
    }

    TEST_CASE("TEXT keeps prompting and stacks each line below the last") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(0, 100));
        h.executor.supply(InputValue::makeNumber(10.0));
        h.executor.supply(InputValue::makeNumber(0.0));
        h.executor.supply(InputValue::makeText("FIRST"));
        h.executor.supply(InputValue::makeText("SECOND"));
        h.executor.supply(InputValue::makeEmpty());

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 2);
        // Same column, and the second sits below the first rather than on it.
        CHECK(texts[0]->position.x == doctest::Approx(texts[1]->position.x));
        CHECK(texts[1]->position.y < texts[0]->position.y);
    }

    TEST_CASE("TEXT stacks along its own baseline when rotated") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(10.0));
        h.executor.supply(InputValue::makeNumber(kPi / 2.0));  // reading upwards
        h.executor.supply(InputValue::makeText("FIRST"));
        h.executor.supply(InputValue::makeText("SECOND"));
        h.executor.supply(InputValue::makeEmpty());

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 2);
        // Rotated a quarter turn, "below" is to the right on the sheet.
        CHECK(texts[1]->position.x > texts[0]->position.x);
        CHECK(texts[1]->position.y == doctest::Approx(texts[0]->position.y));
    }

    TEST_CASE("TEXT takes a picked direction as its rotation, as ROTATE does") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(2.0));
        h.executor.supply(point(0, 10));  // straight up from the start point
        h.executor.supply(InputValue::makeText("UP"));
        h.executor.supply(InputValue::makeEmpty());

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->rotation == doctest::Approx(kPi / 2.0));
    }

    /// other CAD software skips the height prompt when the style fixes the height, and a
    /// drawing set with a house style should need one fewer answer per note.
    ///
    /// Asserts the *prompts*, not just the result. Checking only the entity let
    /// a real bug through: the command skipped straight to asking for contents
    /// while its state machine still wanted a rotation, so a note typed under a
    /// fixed-height style was swallowed as the angle. Supplying a number where
    /// a user would type text hid it completely.
    TEST_CASE("TEXT skips the height prompt but not the rotation when the style fixes it") {
        Harness h;
        TextStyle& style = h.db->ensureTextStyle("Notes");
        style.height = 7.0;
        h.db->settings.currentTextStyleName = "Notes";

        h.executor.execute("TEXT");
        h.executor.supply(point(0, 0));

        const InputRequest* afterPoint = h.executor.currentRequest();
        REQUIRE(afterPoint != nullptr);
        CHECK(afterPoint->kind == InputRequest::Kind::Angle);

        h.executor.supply(InputValue::makeNumber(0.0));

        const InputRequest* afterRotation = h.executor.currentRequest();
        REQUIRE(afterRotation != nullptr);
        CHECK(afterRotation->kind == InputRequest::Kind::Text);

        h.executor.supply(InputValue::makeText("NOTE"));
        h.executor.supply(InputValue::makeEmpty());

        const auto texts = h.ofType<TextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->contents == "NOTE");
        CHECK(texts[0]->height == doctest::Approx(7.0));
        CHECK(texts[0]->styleHandle == style.handle);
    }

    TEST_CASE("TEXT asks for a height when the style leaves it to the object") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(0, 0));

        const InputRequest* afterPoint = h.executor.currentRequest();
        REQUIRE(afterPoint != nullptr);
        CHECK(afterPoint->kind == InputRequest::Kind::Distance);
    }

    TEST_CASE("MTEXT skips the height prompt and asks for text when the style fixes it") {
        Harness h;
        TextStyle& style = h.db->ensureTextStyle("Notes");
        style.height = 6.0;
        h.db->settings.currentTextStyleName = "Notes";

        h.executor.execute("MTEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(50, 20));

        // MTEXT has no rotation prompt, so contents come straight after the box.
        const InputRequest* next = h.executor.currentRequest();
        REQUIRE(next != nullptr);
        CHECK(next->kind == InputRequest::Kind::Text);

        h.executor.supply(InputValue::makeText("NOTE"));
        const auto texts = h.ofType<MTextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->contents == "NOTE");
        CHECK(texts[0]->height == doctest::Approx(6.0));
    }

    TEST_CASE("TEXT cancelled before anything is typed leaves nothing behind") {
        Harness h;
        h.executor.execute("TEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(2.0));
        h.executor.supply(InputValue::makeNumber(0.0));
        h.executor.supply(InputValue::makeCancel());

        CHECK(h.ofType<TextEntity>().empty());
    }

    TEST_CASE("MTEXT wraps to the box and hangs from its top-left corner") {
        Harness h;
        h.executor.execute("MTEXT");
        // Dragged bottom-right to top-left, to check the corners are ordered
        // rather than assumed.
        h.executor.supply(point(120, 40));
        h.executor.supply(point(20, 90));
        h.executor.supply(InputValue::makeNumber(4.0));
        h.executor.supply(InputValue::makeText("GENERAL NOTES"));

        const auto texts = h.ofType<MTextEntity>();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0]->contents == "GENERAL NOTES");
        CHECK(texts[0]->height == doctest::Approx(4.0));
        CHECK(texts[0]->width == doctest::Approx(100.0));
        CHECK(texts[0]->attachment == MTextAttachment::TopLeft);
        CHECK_POINT(texts[0]->position, Vec3(20, 90));
    }

    TEST_CASE("MTEXT with no text entered creates nothing") {
        Harness h;
        h.executor.execute("MTEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(50, 20));
        h.executor.supply(InputValue::makeNumber(3.0));
        h.executor.supply(InputValue::makeText(""));

        CHECK(h.ofType<MTextEntity>().empty());
    }

    TEST_CASE("text created by command undoes as one operation") {
        Harness h;
        h.executor.execute("MTEXT");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(50, 20));
        h.executor.supply(InputValue::makeNumber(3.0));
        h.executor.supply(InputValue::makeText("NOTE"));
        REQUIRE(h.ofType<MTextEntity>().size() == 1);

        h.tm.undo();
        CHECK(h.ofType<MTextEntity>().empty());
    }
}

TEST_SUITE("Editing text") {
    /// Add a line of text and hand back its handle.
    [[nodiscard]] Handle placeText(Harness& h, std::string contents) {
        auto text = std::make_unique<TextEntity>(Vec3{0.0, 0.0, 0.0}, std::move(contents), 5.0);
        return h.db->addToModelSpace(std::move(text)).handle;
    }

    TEST_CASE("TEXTEDIT offers the existing words for amendment") {
        Harness h;
        const Handle handle = placeText(h, "ROOM 101");

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));

        const InputRequest* request = h.executor.currentRequest();
        REQUIRE(request != nullptr);
        CHECK(request->kind == InputRequest::Kind::Text);
        // The whole point: the old text arrives in the editor. A prompt that
        // shows it and makes you retype it is not an editor.
        REQUIRE(request->editText.has_value());
        CHECK(*request->editText == "ROOM 101");
    }

    TEST_CASE("TEXTEDIT replaces the contents of a line of text") {
        Harness h;
        const Handle handle = placeText(h, "ROOM 101");

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));
        h.executor.supply(InputValue::makeText("SUITE 200"));
        h.executor.supply(InputValue::makeEmpty());

        const auto* text = h.db->get<TextEntity>(handle);
        REQUIRE(text != nullptr);
        CHECK(text->contents == "SUITE 200");
    }

    TEST_CASE("an edit undoes as one operation") {
        Harness h;
        const Handle handle = placeText(h, "ROOM 101");

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));
        h.executor.supply(InputValue::makeText("SUITE 200"));
        h.executor.supply(InputValue::makeEmpty());
        REQUIRE(h.db->get<TextEntity>(handle)->contents == "SUITE 200");

        h.tm.undo();
        CHECK(h.db->get<TextEntity>(handle)->contents == "ROOM 101");
    }

    TEST_CASE("Enter on the unchanged line leaves the text alone") {
        Harness h;
        const Handle handle = placeText(h, "ROOM 101");

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));
        // Enter accepts the default, which is what was already there. Emptying
        // the label instead would make Enter a way to delete a note by accident.
        h.executor.supply(InputValue::makeEmpty());
        h.executor.supply(InputValue::makeEmpty());

        CHECK(h.db->get<TextEntity>(handle)->contents == "ROOM 101");
    }

    TEST_CASE("TEXTEDIT edits paragraph text, formatting codes included") {
        Harness h;
        auto mtext = std::make_unique<MTextEntity>(Vec3::zero(), "\\pxi-4;FIRST\\PSECOND", 3.0);
        const Handle handle = h.db->addToModelSpace(std::move(mtext)).handle;

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));

        const InputRequest* request = h.executor.currentRequest();
        REQUIRE(request != nullptr);
        // The *stored* string, codes and all. Handing back the plain text would
        // silently strip a paragraph's formatting the moment it was edited.
        REQUIRE(request->editText.has_value());
        CHECK(*request->editText == "\\pxi-4;FIRST\\PSECOND");

        h.executor.supply(InputValue::makeText("\\pxi-4;FIRST\\PTHIRD"));
        h.executor.supply(InputValue::makeEmpty());
        CHECK(h.db->get<MTextEntity>(handle)->contents == "\\pxi-4;FIRST\\PTHIRD");
    }

    TEST_CASE("TEXTEDIT edits an attribute's value") {
        Harness h;
        auto attribute = std::make_unique<AttributeEntity>();
        attribute->tag = "SHEET";
        attribute->contents = "A-1.1";
        attribute->height = 2.5;
        const Handle handle = h.db->addToModelSpace(std::move(attribute)).handle;

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));
        h.executor.supply(InputValue::makeText("A-2.2"));
        h.executor.supply(InputValue::makeEmpty());

        // A title block is attributes, and changing one is the single most
        // common text edit in a drawing set.
        CHECK(h.db->get<AttributeEntity>(handle)->contents == "A-2.2");
    }

    TEST_CASE("TEXTEDIT keeps asking, so a sheet of labels is one command") {
        Harness h;
        const Handle first = placeText(h, "ONE");
        const Handle second = placeText(h, "TWO");

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(first));
        h.executor.supply(InputValue::makeText("1"));
        h.executor.supply(InputValue::makeEntity(second));
        h.executor.supply(InputValue::makeText("2"));
        h.executor.supply(InputValue::makeEmpty());

        CHECK(h.db->get<TextEntity>(first)->contents == "1");
        CHECK(h.db->get<TextEntity>(second)->contents == "2");
        CHECK_FALSE(h.executor.isRunning());
    }

    TEST_CASE("an object with no text says what it is instead of failing") {
        Harness h;
        const Handle line =
            h.db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}))
                .handle;

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(line));

        // Still running, still asking: a missed pick costs a click, not the
        // command. And the warning names the type, because "cannot edit that"
        // does not tell the user what to click instead.
        CHECK(h.executor.isRunning());
        const InputRequest* request = h.executor.currentRequest();
        REQUIRE(request != nullptr);
        CHECK(request->kind == InputRequest::Kind::Entity);
    }

    TEST_CASE("a selected label is the answer, so the command does not ask again") {
        Harness h;
        const Handle handle = placeText(h, "ROOM 101");
        h.selection.setSelection({handle});

        h.executor.execute("TEXTEDIT");

        // Straight to the words. This is what makes a double-click one step.
        const InputRequest* request = h.executor.currentRequest();
        REQUIRE(request != nullptr);
        CHECK(request->kind == InputRequest::Kind::Text);
        REQUIRE(request->editText.has_value());
        CHECK(*request->editText == "ROOM 101");
    }

    TEST_CASE("TEXTEDIT edits a dimension's text override") {
        Harness h;
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->definitionPoint = Vec3{0.0, 0.0, 0.0};
        dimension->textOverride = "EQ";
        const Handle handle = h.db->addToModelSpace(std::move(dimension)).handle;

        h.executor.execute("TEXTEDIT");
        h.executor.supply(InputValue::makeEntity(handle));
        h.executor.supply(InputValue::makeText("TYP."));
        h.executor.supply(InputValue::makeEmpty());

        CHECK(h.db->get<DimensionEntity>(handle)->textOverride == "TYP.");
    }
}

TEST_SUITE("Block commands") {
    TEST_CASE("BLOCK stores contents relative to the base point and replaces them") {
        Harness h;
        Entity& a = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{100, 100}, Vec3{110, 100}));
        Entity& b = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{110, 100}, Vec3{110, 105}));

        h.executor.execute("BLOCK");
        h.executor.supply(InputValue::makeSelection({a.handle, b.handle}));
        h.executor.supply(point(100, 100));  // base point
        h.executor.supply(InputValue::makeText("WIDGET"));

        const BlockTableRecord* block = h.db->findBlock("WIDGET");
        REQUIRE(block != nullptr);

        // Contents are relative to the base point, so the first line starts at
        // the block origin rather than out at its world position.
        const auto contents = h.db->entitiesIn(*block);
        REQUIRE(contents.size() == 2);
        const auto* first = dynamic_cast<const LineEntity*>(contents[0]);
        REQUIRE(first != nullptr);
        CHECK_POINT(first->start, Vec3::zero());

        // The originals are gone, replaced by one reference at the base point.
        CHECK(h.db->getObject(a.handle)->erased);
        CHECK(h.db->getObject(b.handle)->erased);
        const auto inserts = h.ofType<InsertEntity>();
        REQUIRE(inserts.size() == 1);
        CHECK(inserts[0]->blockHandle == block->handle);
        CHECK_POINT(inserts[0]->position, Vec3(100, 100));
    }

    TEST_CASE("BLOCK refuses a name already in use rather than redefining it") {
        Harness h;
        static_cast<void>(h.addBlock("WIDGET"));
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}));

        h.executor.execute("BLOCK");
        h.executor.supply(InputValue::makeSelection({line.handle}));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeText("WIDGET"));

        // Redefining would silently change every existing reference, so the
        // geometry is left exactly as it was.
        CHECK_FALSE(h.db->getObject(line.handle)->erased);
        CHECK(h.ofType<InsertEntity>().empty());
    }

    TEST_CASE("BLOCK undoes as one operation, releasing the name") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0}));

        h.executor.execute("BLOCK");
        h.executor.supply(InputValue::makeSelection({line.handle}));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeText("WIDGET"));
        REQUIRE(h.db->findBlock("WIDGET") != nullptr);

        h.tm.undo();

        // The definition, the reference and the erasure all reverse together —
        // and the name is free again, so the same block can be made a second
        // time. That only works because erasing a symbol record releases it
        // from its name index.
        CHECK(h.db->findBlock("WIDGET") == nullptr);
        CHECK(h.ofType<InsertEntity>().empty());
        CHECK_FALSE(h.db->getObject(line.handle)->erased);

        h.executor.execute("BLOCK");
        h.executor.supply(InputValue::makeSelection({line.handle}));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeText("WIDGET"));
        CHECK(h.db->findBlock("WIDGET") != nullptr);
    }

    TEST_CASE("INSERT places a reference with the scale and rotation given") {
        Harness h;
        BlockTableRecord& block = h.addBlock("WIDGET");
        h.db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}), block);

        h.executor.execute("INSERT");
        h.executor.supply(InputValue::makeText("WIDGET"));
        h.executor.supply(point(50, 60));
        h.executor.supply(InputValue::makeNumber(2.0));        // X scale
        h.executor.supply(InputValue::makeNumber(3.0));        // Y scale
        h.executor.supply(InputValue::makeNumber(kPi / 2.0));  // rotation

        const auto inserts = h.ofType<InsertEntity>();
        REQUIRE(inserts.size() == 1);
        CHECK(inserts[0]->blockHandle == block.handle);
        CHECK_POINT(inserts[0]->position, Vec3(50, 60));
        CHECK(inserts[0]->scale.x == doctest::Approx(2.0));
        CHECK(inserts[0]->scale.y == doctest::Approx(3.0));
        CHECK(inserts[0]->rotation == doctest::Approx(kPi / 2.0));
    }

    TEST_CASE("INSERT defaults the Y scale to the X scale") {
        Harness h;
        BlockTableRecord& block = h.addBlock("WIDGET");
        h.db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1, 0}), block);

        h.executor.execute("INSERT");
        h.executor.supply(InputValue::makeText("WIDGET"));
        h.executor.supply(point(0, 0));
        h.executor.supply(InputValue::makeNumber(4.0));
        h.executor.supply(InputValue::makeEmpty());  // accept the default
        h.executor.supply(InputValue::makeNumber(0.0));

        const auto inserts = h.ofType<InsertEntity>();
        REQUIRE(inserts.size() == 1);
        CHECK(inserts[0]->scale.y == doctest::Approx(4.0));
    }

    TEST_CASE("INSERT reports an unknown name rather than creating a dangling reference") {
        Harness h;
        static_cast<void>(h.addBlock("WIDGET"));

        h.executor.execute("INSERT");
        h.executor.supply(InputValue::makeText("NOSUCHBLOCK"));

        CHECK(h.ofType<InsertEntity>().empty());
    }

    TEST_CASE("INSERT refuses a layout's block record") {
        Harness h;
        h.db->ensureModelLayout();
        BlockTableRecord* paper = h.db->findBlock(kPaperSpace);
        REQUIRE(paper != nullptr);

        h.executor.execute("INSERT");
        h.executor.supply(InputValue::makeText(paper->name));

        // Inserting paper space into the model would nest a sheet in a drawing.
        CHECK(h.ofType<InsertEntity>().empty());
    }

    /// The point of a block is reuse across sessions, so it has to survive the
    /// file. A definition that exists only in memory is a copy-paste with extra
    /// steps.
    TEST_CASE("a block defined by command survives a DXF round trip") {
        Harness h;
        Entity& line = h.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3{10, 10}, Vec3{20, 10}));

        h.executor.execute("BLOCK");
        h.executor.supply(InputValue::makeSelection({line.handle}));
        h.executor.supply(point(10, 10));
        h.executor.supply(InputValue::makeText("WIDGET"));
        REQUIRE(h.db->findBlock("WIDGET") != nullptr);

        const std::string text = io::writeDxf(*h.db);
        const io::DxfReadResult reread = io::readDxf(text);
        REQUIRE(reread.database != nullptr);

        const BlockTableRecord* block = reread.database->findBlock("WIDGET");
        REQUIRE(block != nullptr);
        CHECK(reread.database->entitiesIn(*block).size() == 1);

        std::size_t references = 0;
        for (const Entity* entity : reread.database->entitiesIn(reread.database->modelSpace())) {
            if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
                CHECK(insert->blockHandle == block->handle);
                ++references;
            }
        }
        CHECK(references == 1);
    }
}

TEST_SUITE("Geometry operations") {
    TEST_CASE("offsets a line to the left for a positive distance") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        const CurvePtr offset = offsetCurve(line, 5.0);
        REQUIRE(offset != nullptr);
        CHECK_POINT(offset->startPoint(), Vec3(0, 5));
    }

    TEST_CASE("offsets an arc inward and refuses an impossible offset") {
        const Arc arc = Arc::circle(Vec3::zero(), 10.0);
        const CurvePtr inward = offsetCurve(arc, 3.0);
        REQUIRE(inward != nullptr);
        CHECK(dynamic_cast<const Arc*>(inward.get())->radius == doctest::Approx(7.0));

        // Offsetting inward by more than the radius has no result, and saying
        // so beats emitting an inside-out arc.
        CHECK(offsetCurve(arc, 15.0) == nullptr);
    }

    TEST_CASE("offsets a closed polyline keeping it closed") {
        const Polyline square = Polyline::rectangle(Vec3::zero(), Vec3{10, 10});
        const CurvePtr offset = offsetCurve(square, -1.0);
        REQUIRE(offset != nullptr);
        CHECK(offset->isClosed());
        // Offsetting outward by 1 grows a 10x10 square to 12x12.
        CHECK(offset->bounds().width() == doctest::Approx(12.0));
    }

    TEST_CASE("extends a line to the nearest boundary beyond the picked end") {
        const LineSegment line{Vec3::zero(), Vec3{5, 0}};
        const LineSegment near{Vec3{10, -5}, Vec3{10, 5}};
        const LineSegment far{Vec3{20, -5}, Vec3{20, 5}};
        const std::array<const Curve*, 2> boundaries{&near, &far};

        // Picking near the far end extends that end, and stops at the nearest
        // boundary so repeated EXTEND walks outward one at a time.
        const CurvePtr extended = extendCurve(line, boundaries, Vec3{4.5, 0});
        REQUIRE(extended != nullptr);
        CHECK_POINT(extended->endPoint(), Vec3(10, 0));
    }

    TEST_CASE("chamfers a corner between two lines") {
        const LineSegment a{Vec3::zero(), Vec3{10, 0}};
        const LineSegment b{Vec3::zero(), Vec3{0, 10}};
        const auto result = chamferLines(a, b, 2.0, 3.0, Vec3{8, 0}, Vec3{0, 8});
        REQUIRE(result.has_value());
        CHECK_POINT(result->line.start, Vec3(2, 0));
        CHECK_POINT(result->line.end, Vec3(0, 3));
    }

    TEST_CASE("breaks a curve into two pieces and discards the middle") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        const std::vector<CurvePtr> pieces = breakCurve(line, Vec3{3, 0}, Vec3{7, 0});
        REQUIRE(pieces.size() == 2);
        CHECK(pieces[0]->length() == doctest::Approx(3.0));
        CHECK(pieces[1]->length() == doctest::Approx(3.0));
    }

    TEST_CASE("divides and measures along a curve") {
        const LineSegment line{Vec3::zero(), Vec3{12, 0}};
        const std::vector<Vec3> divided = dividePoints(line, 4);
        REQUIRE(divided.size() == 3);
        CHECK_POINT(divided[0], Vec3(3, 0));

        const std::vector<Vec3> measured = measurePoints(line, 5.0);
        REQUIRE(measured.size() == 2);
        CHECK_POINT(measured[1], Vec3(10, 0));
    }
}

TEST_SUITE("Object snaps") {
    /// EXTENSION reports a point *past* the end of an object, so the object is
    /// always further from the cursor than the snap aperture. The engine relies
    /// on the caller supplying it as a candidate anyway.
    ///
    /// This is written down because getting it wrong is invisible: the app
    /// gathered candidates by searching the index around the cursor, which can
    /// never reach an object that far away, so extension silently did nothing
    /// while every unit test passed. The shell now also offers recently hovered
    /// entities, which is how other CAD software acquires an extension.
    TEST_CASE("extension is found for a curve well outside the aperture") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        // Forty units past the end of a ten unit line, with a one unit
        // aperture: nothing about this curve is "near" the cursor.
        query.cursor = Vec3{50.0, 0.1};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Extension);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK(best->mode == SnapMode::Extension);
        CHECK_POINT(best->position, Vec3(50, 0));
    }

    TEST_CASE("extension is not offered for a point that lies on the curve") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3{5.0, 0.1};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Extension);
        query.candidates = candidates;

        // Otherwise this would duplicate Nearest at a lower priority, and the
        // ellipsis glyph would appear along objects rather than past them.
        const SnapEngine engine;
        CHECK_FALSE(engine.findBest(query).has_value());
    }

    TEST_CASE("quadrant finds the cardinal points of a circle") {
        const Arc circle = Arc::circle(Vec3::zero(), 10.0);
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<Arc>(circle));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Quadrant);
        query.candidates = candidates;

        const SnapEngine engine;
        for (const Vec3& expected :
             {Vec3{10, 0}, Vec3{0, 10}, Vec3{-10, 0}, Vec3{0, -10}}) {
            query.cursor = expected + Vec3{0.2, 0.2};
            const auto best = engine.findBest(query);
            REQUIRE(best.has_value());
            CHECK(best->mode == SnapMode::Quadrant);
            CHECK_POINT(best->position, expected);
        }
    }

    TEST_CASE("nearest lands on the curve rather than at a feature") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3{3.7, 0.2};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Nearest);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK(best->mode == SnapMode::Nearest);
        CHECK_POINT(best->position, Vec3(3.7, 0));
    }

    TEST_CASE("prefers endpoint over nearest at the same location") {
        // Without deterministic priority, moving near a line's end flickers
        // between Endpoint and Nearest.
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3{0.05, 0.0};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Endpoint) | toMask(SnapMode::Nearest);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK(best->mode == SnapMode::Endpoint);
        CHECK_POINT(best->position, Vec3::zero());
    }

    TEST_CASE("finds midpoints of each polyline segment, not the whole curve") {
        const Polyline square = Polyline::rectangle(Vec3::zero(), Vec3{10, 10});
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<Polyline>(square));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3{5.0, 0.2};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Midpoint);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK_POINT(best->position, Vec3(5, 0));
    }

    TEST_CASE("snaps to the intersection of two curves") {
        const LineSegment a{Vec3::zero(), Vec3{10, 0}};
        const LineSegment b{Vec3{5, -5}, Vec3{5, 5}};
        std::vector<CurvePtr> curvesA;
        curvesA.push_back(std::make_unique<LineSegment>(a));
        std::vector<CurvePtr> curvesB;
        curvesB.push_back(std::make_unique<LineSegment>(b));

        const std::array<SnapCandidate, 2> candidates{
            SnapCandidate{Handle{1}, curvesA, nullptr, false},
            SnapCandidate{Handle{2}, curvesB, nullptr, false}};

        SnapQuery query;
        query.cursor = Vec3{5.1, 0.1};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Intersection);
        query.candidates = candidates;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK(best->mode == SnapMode::Intersection);
        CHECK_POINT(best->position, Vec3(5, 0));
    }

    TEST_CASE("finds the perpendicular foot from a base point") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        const Vec3 base{3.0, 7.0};
        SnapQuery query;
        query.cursor = Vec3{3.0, 0.1};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Perpendicular);
        query.candidates = candidates;
        query.basePoint = &base;

        const SnapEngine engine;
        const auto best = engine.findBest(query);
        REQUIRE(best.has_value());
        CHECK_POINT(best->position, Vec3(3, 0));
    }

    TEST_CASE("finds tangent points on a circle from an external point") {
        const Arc circle = Arc::circle(Vec3::zero(), 5.0);
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<Arc>(circle));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        const Vec3 base{13.0, 0.0};
        SnapQuery query;
        query.cursor = Vec3{1.9, 4.6};
        query.aperture = 2.0;
        query.modes = toMask(SnapMode::Tangent);
        query.candidates = candidates;
        query.basePoint = &base;

        const SnapEngine engine;
        const auto snaps = engine.findAll(query);
        REQUIRE_FALSE(snaps.empty());
        // A tangent point lies on the circle and is perpendicular to the radius.
        for (const SnapPoint& snap : snaps) {
            CHECK(snap.position.length2() == doctest::Approx(5.0));
            CHECK((base - snap.position).dot(snap.position) == doctest::Approx(0.0).scale(10.0));
        }
    }

    TEST_CASE("reports nothing when snapping is suppressed") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3::zero();
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Endpoint) | toMask(SnapMode::Clear);
        query.candidates = candidates;

        const SnapEngine engine;
        CHECK_FALSE(engine.findBest(query).has_value());
    }

    TEST_CASE("ignores candidates outside the aperture") {
        const LineSegment line{Vec3::zero(), Vec3{10, 0}};
        std::vector<CurvePtr> curves;
        curves.push_back(std::make_unique<LineSegment>(line));

        const SnapCandidate candidate{Handle{1}, curves, nullptr, false};
        const std::array<SnapCandidate, 1> candidates{candidate};

        SnapQuery query;
        query.cursor = Vec3{100.0, 100.0};
        query.aperture = 1.0;
        query.modes = toMask(SnapMode::Endpoint);
        query.candidates = candidates;

        const SnapEngine engine;
        CHECK_FALSE(engine.findBest(query).has_value());
    }
}

TEST_SUITE("What a double-click can edit") {
    TEST_CASE("text, paragraphs and dimensions have editable text") {
        MTextEntity paragraph;
        TextEntity label;
        DimensionEntity dimension;
        CHECK(hasEditableText(paragraph));
        CHECK(hasEditableText(label));
        CHECK(hasEditableText(dimension));
    }

    TEST_CASE("geometry does not") {
        LineEntity line{Vec3::zero(), Vec3{10, 0}};
        CircleEntity circle{Vec3::zero(), 5.0};
        PolylineEntity polyline;

        // Asked before TEXTEDIT is started rather than discovered inside it.
        // Starting it on a wall left the command asking the user to "select an
        // annotation object to edit" -- a question they had not asked, in
        // answer to a double-click that should simply have selected the wall.
        CHECK_FALSE(hasEditableText(line));
        CHECK_FALSE(hasEditableText(circle));
        CHECK_FALSE(hasEditableText(polyline));
    }
}

TEST_SUITE("LAYER") {
    TEST_CASE("makes a layer, colours it, and draws on it") {
        Harness h;
        h.executor.execute("LAYER");
        h.executor.supply(InputValue::makeText("A-WALL"));
        h.executor.supply(InputValue::makeNumber(3));

        const Layer* made = h.db->findLayer("A-WALL");
        REQUIRE(made != nullptr);
        CHECK(made->color.index() == 3);
        CHECK(h.db->settings.currentLayerName == "A-WALL");

        // And the next thing drawn lands there. This is the half that was
        // missing: the layer control set a name that nothing read, so every
        // line went to layer 0 whatever the drawing said.
        h.executor.execute("LINE");
        h.executor.supply(point(0, 0));
        h.executor.supply(point(10, 0));
        h.executor.supply(InputValue::makeEmpty());

        REQUIRE(h.ofType<LineEntity>().size() == 1);
        CHECK(h.ofType<LineEntity>().front()->layerHandle == made->handle);
    }

    TEST_CASE("an existing layer is selected, not re-coloured") {
        Harness h;
        Layer& existing = h.db->ensureLayer("A-WALL");
        existing.color = Color::fromIndex(5);

        h.executor.execute("LAYER");
        h.executor.supply(InputValue::makeText("A-WALL"));

        // No colour prompt for a layer that already exists: asking would invite
        // changing a drawing standard when all that was wanted was to draw.
        CHECK_FALSE(h.executor.isRunning());
        CHECK(h.db->findLayer("A-WALL")->color.index() == 5);
        CHECK(h.db->settings.currentLayerName == "A-WALL");
    }

    TEST_CASE("making a layer undoes with the work that needed it") {
        Harness h;
        h.executor.execute("LAYER");
        h.executor.supply(InputValue::makeText("A-ANNO"));
        h.executor.supply(InputValue::makeEmpty());
        REQUIRE(h.db->findLayer("A-ANNO") != nullptr);

        REQUIRE(h.tm.undo());
        CHECK(h.db->findLayer("A-ANNO") == nullptr);
    }

    TEST_CASE("a layer name that names nothing is refused") {
        Harness h;
        h.executor.execute("LAYER");
        h.executor.supply(InputValue::makeText(""));
        CHECK_FALSE(h.executor.isRunning());
        CHECK(h.db->settings.currentLayerName == "0");
    }
}
