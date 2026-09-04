/// Tests for the C plugin boundary.
///
/// The claim these tests exist to check is the one the brief makes: a command
/// contributed from outside the engine must be a *first-class* command, not a
/// second tier with its own rules. So they do not merely assert that
/// registration returns CAD_OK — they run the plugin command through the same
/// executor as a built-in, then undo it, then look for it in the same
/// category listing the menus are generated from.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cad/abi/PluginHost.hpp"
#include "cad/abi/PluginManager.hpp"
#include "cad/db/CurveEntities.hpp"
#include "test_helpers.hpp"

using namespace cad;

namespace {

const CadApi& api() { return abi::api(); }

/// Everything a document needs, wired the way the application wires it.
struct AbiFixture {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    CommandRegistry registry;
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, registry, selection};
    abi::HostDocument document{*db, transactions, &executor};
    abi::PluginHost host{"test", registry, document};

    [[nodiscard]] CadPluginHost* pluginHost() {
        return reinterpret_cast<CadPluginHost*>(&host);
    }
    [[nodiscard]] CadDocument* cadDocument() {
        return reinterpret_cast<CadDocument*>(&document);
    }
    [[nodiscard]] std::size_t liveEntityCount() {
        return db->entitiesIn(db->modelSpace()).size();
    }
};

/// State for the test command below. A single instance, reset per test case;
/// doctest runs cases sequentially in one thread, so this is safe and keeps the
/// callbacks readable.
struct TestCommandState {
    int step = 0;
    CadPoint first{};
    int createCalls = 0;
    int destroyCalls = 0;
    int cleanupCalls = 0;
    CadStatus lastStatus = CAD_OK;
    /// Set to make the command try something it is not allowed to do.
    bool attemptCommit = false;
    CadHandle created = CAD_NULL_HANDLE;
};

TestCommandState g_test;

[[nodiscard]] CadStepResult ask(int32_t kind, const char* prompt) {
    CadStepResult step{};
    step.struct_size = static_cast<std::uint32_t>(sizeof(step));
    step.kind = CAD_STEP_REQUEST;
    step.request.struct_size = static_cast<std::uint32_t>(sizeof(step.request));
    step.request.kind = kind;
    step.request.prompt = prompt;
    return step;
}

[[nodiscard]] CadStepResult finish(int32_t kind, const char* message) {
    CadStepResult step{};
    step.struct_size = static_cast<std::uint32_t>(sizeof(step));
    step.kind = kind;
    step.message = message;
    return step;
}

}  // namespace

// The callbacks live in an `extern "C"` block because that is what a plugin
// would have. The function pointer types are declared inside `extern "C"` in
// the header, and while every mainstream compiler ignores the language linkage
// of a function pointer type, an example that has to be right should be right.
extern "C" {

static void* CAD_CALL testCreate(void* /*userData*/) {
    ++g_test.createCalls;
    g_test.step = 0;
    return &g_test;
}

static void CAD_CALL testDestroy(void* /*instance*/) { ++g_test.destroyCalls; }

static void CAD_CALL testCleanup(CadCommandContext* /*context*/, void* /*instance*/) {
    ++g_test.cleanupCalls;
}

static CadStepResult CAD_CALL testStart(CadCommandContext* /*context*/, void* /*instance*/) {
    g_test.step = 0;
    return ask(CAD_INPUT_POINT, "First point");
}

static CadStepResult CAD_CALL testSupply(CadCommandContext* context, const CadInputValue* input,
                                         void* /*instance*/) {
    if (input->kind == CAD_VALUE_CANCEL) { return finish(CAD_STEP_CANCELLED, "cancelled"); }

    if (g_test.step == 0) {
        g_test.first = input->point;
        g_test.step = 1;
        CadStepResult step = ask(CAD_INPUT_POINT, "Second point");
        step.request.has_base_point = 1;
        step.request.base_point = g_test.first;
        return step;
    }

    CadTransaction* transaction = api().context_transaction(context);
    g_test.lastStatus = api().add_line(transaction, CAD_NULL_HANDLE, g_test.first, input->point,
                                       nullptr, &g_test.created);
    if (g_test.lastStatus != CAD_OK) { return finish(CAD_STEP_FAILED, api().last_error()); }

    if (g_test.attemptCommit) {
        // Deliberately illegal: the executor owns this transaction.
        g_test.lastStatus = api().commit_transaction(transaction);
    }
    return finish(CAD_STEP_DONE, "line drawn");
}

}  // extern "C"

namespace {

/// A descriptor for the command above, filled the way a plugin would fill it.
[[nodiscard]] CadCommandDesc testDescriptor() {
    static const char* const aliases[] = {"TL"};
    CadCommandDesc desc{};
    desc.struct_size = static_cast<std::uint32_t>(sizeof(desc));
    desc.id = "test.draw.line";
    desc.name = "TESTLINE";
    desc.title = "Test Line";
    desc.description = "Draws a line, from a plugin";
    desc.category = "Draw";
    desc.aliases = aliases;
    desc.alias_count = 1;
    desc.transaction_mode = CAD_TRANSACTION_SINGLE;
    desc.repeatable = 1;
    desc.on_create = testCreate;
    desc.on_destroy = testDestroy;
    desc.on_start = testStart;
    desc.on_supply = testSupply;
    desc.on_cleanup = testCleanup;
    return desc;
}

void resetTestState() { g_test = TestCommandState{}; }

}  // namespace

TEST_SUITE("AbiTable") {
    TEST_CASE("the table reports its own version and size") {
        CHECK(api().version_major == CAD_ABI_VERSION_MAJOR);
        CHECK(api().version_minor == CAD_ABI_VERSION_MINOR);
        CHECK(api().struct_size == sizeof(CadApi));
    }

    TEST_CASE("every function pointer in the table is populated") {
        // A null entry is a crash in a plugin, at a call site with no clue as
        // to which function was forgotten. Reading the table as an array of
        // pointers catches an omission here instead.
        const CadApi& table = api();
        const auto* first = reinterpret_cast<const void* const*>(&table.last_error);
        const auto* end = reinterpret_cast<const void* const*>(&table + 1);
        std::size_t nulls = 0;
        for (const void* const* slot = first; slot < end; ++slot) {
            if (*slot == nullptr) { ++nulls; }
        }
        CHECK(nulls == 0);
    }

    TEST_CASE("version compatibility refuses a different major version") {
        CHECK(abi::isCompatibleVersion(CAD_ABI_VERSION_MAJOR, CAD_ABI_VERSION_MINOR));
        CHECK_FALSE(abi::isCompatibleVersion(CAD_ABI_VERSION_MAJOR + 1, 0));
        // A plugin from the future would call past the end of the table.
        CHECK_FALSE(abi::isCompatibleVersion(CAD_ABI_VERSION_MAJOR, CAD_ABI_VERSION_MINOR + 1));
    }
}

TEST_SUITE("AbiRegistration") {
    TEST_CASE("a registered command is indistinguishable from a built-in") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        // Found by name, by alias and case-insensitively, like any command.
        REQUIRE(fixture.registry.lookup("TESTLINE") != nullptr);
        CHECK(fixture.registry.lookup("testline") != nullptr);
        CHECK(fixture.registry.lookup("TL") != nullptr);

        // Present in the category listing the menus are generated from, which
        // is what "appears in the UI without a second registration" means.
        const auto categories = fixture.registry.byCategory();
        REQUIRE(categories.count("Draw") == 1);
        CHECK(categories.at("Draw").size() == 1);

        // And rankable by the command line's search.
        const auto matches = fixture.registry.search("TESTL");
        REQUIRE_FALSE(matches.empty());
        CHECK(matches.front().command->id == "test.draw.line");
    }

    TEST_CASE("an uncategorized command still reaches the menus") {
        resetTestState();
        AbiFixture fixture;
        CadCommandDesc desc = testDescriptor();
        desc.category = nullptr;
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        const auto categories = fixture.registry.byCategory();
        CHECK(categories.count("Plugins") == 1);
    }

    TEST_CASE("registration refuses a descriptor with no struct_size") {
        resetTestState();
        AbiFixture fixture;
        CadCommandDesc desc = testDescriptor();
        desc.struct_size = 0;
        CHECK(api().register_command(fixture.pluginHost(), &desc) == CAD_ERROR_INVALID_ARGUMENT);
        CHECK(fixture.registry.size() == 0);
    }

    TEST_CASE("registration refuses a descriptor with no callbacks") {
        resetTestState();
        AbiFixture fixture;
        CadCommandDesc desc = testDescriptor();
        desc.on_start = nullptr;
        CHECK(api().register_command(fixture.pluginHost(), &desc) == CAD_ERROR_INVALID_ARGUMENT);
        CHECK(std::string{api().last_error()}.find("on_start") != std::string::npos);
    }

    TEST_CASE("a colliding name is refused rather than silently shadowing") {
        // The interesting case is not the duplicate id but the duplicate
        // *name*: one plugin quietly taking over another's command would be
        // nearly impossible to diagnose from the drafting side.
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        CadCommandDesc second = desc;
        second.id = "other.draw.line";
        CHECK(api().register_command(fixture.pluginHost(), &second) == CAD_ERROR_ALREADY_EXISTS);
        CHECK(fixture.registry.size() == 1);
    }

    TEST_CASE("destroying the host unregisters its commands") {
        resetTestState();
        AbiFixture fixture;
        {
            abi::PluginHost temporary{"temp", fixture.registry, fixture.document};
            const CadCommandDesc desc = testDescriptor();
            REQUIRE(api().register_command(reinterpret_cast<CadPluginHost*>(&temporary), &desc) ==
                    CAD_OK);
            CHECK(fixture.registry.size() == 1);
        }
        // This is what makes unloading a DLL safe: the registry can no longer
        // reach into it.
        CHECK(fixture.registry.size() == 0);
        CHECK(fixture.registry.lookup("TESTLINE") == nullptr);
    }

    TEST_CASE("unregister_command removes one command by id") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        CHECK(api().unregister_command(fixture.pluginHost(), "nope") == CAD_ERROR_NOT_FOUND);
        CHECK(api().unregister_command(fixture.pluginHost(), "test.draw.line") == CAD_OK);
        CHECK(fixture.registry.size() == 0);
        CHECK(fixture.host.commandCount() == 0);
    }
}

TEST_SUITE("AbiExecution") {
    TEST_CASE("a plugin command runs through the ordinary executor") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        CommandOutcome outcome = fixture.executor.execute("TESTLINE");
        REQUIRE(outcome.status == CommandOutcome::Status::Running);
        CHECK(g_test.createCalls == 1);

        // The prompt the plugin returned reached the executor intact.
        REQUIRE(fixture.executor.currentRequest() != nullptr);
        CHECK(fixture.executor.currentRequest()->prompt == "First point");

        outcome = fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        REQUIRE(outcome.status == CommandOutcome::Status::Running);
        // The base point crossed the boundary, so the UI can rubber-band.
        REQUIRE(fixture.executor.currentRequest() != nullptr);
        REQUIRE(fixture.executor.currentRequest()->basePoint.has_value());
        CHECK_POINT(*fixture.executor.currentRequest()->basePoint, (Vec3{0.0, 0.0, 0.0}));

        outcome = fixture.executor.supply(InputValue::makePoint(Vec3{100.0, 50.0, 0.0}));
        CHECK(outcome.status == CommandOutcome::Status::Completed);
        CHECK(outcome.message == "line drawn");
        CHECK(g_test.cleanupCalls == 1);
        CHECK(g_test.destroyCalls == 1);

        REQUIRE(fixture.liveEntityCount() == 1);
        const auto* line = fixture.db->get<LineEntity>(Handle{g_test.created});
        REQUIRE(line != nullptr);
        CHECK_POINT(line->start, (Vec3{0.0, 0.0, 0.0}));
        CHECK_POINT(line->end, (Vec3{100.0, 50.0, 0.0}));
        // Defaulted properties, because the plugin passed none.
        CHECK(line->layerHandle == fixture.db->defaultLayer().handle);
        CHECK(line->color.isByLayer());
    }

    TEST_CASE("a plugin command's work is one undo step") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        REQUIRE(fixture.executor.execute("TESTLINE").status == CommandOutcome::Status::Running);
        fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        fixture.executor.supply(InputValue::makePoint(Vec3{10.0, 0.0, 0.0}));
        REQUIRE(fixture.liveEntityCount() == 1);

        REQUIRE(fixture.transactions.canUndo());
        CHECK(fixture.transactions.undoName() == "TESTLINE");
        CHECK(fixture.transactions.undo());
        CHECK(fixture.liveEntityCount() == 0);
        CHECK(fixture.transactions.redo());
        CHECK(fixture.liveEntityCount() == 1);
    }

    TEST_CASE("cancelling a plugin command rolls its changes back") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        REQUIRE(fixture.executor.execute("TESTLINE").status == CommandOutcome::Status::Running);
        fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        const CommandOutcome outcome = fixture.executor.supply(InputValue::makeCancel());

        CHECK(outcome.status == CommandOutcome::Status::Cancelled);
        CHECK(fixture.liveEntityCount() == 0);
        CHECK_FALSE(fixture.transactions.canUndo());
        CHECK(fixture.transactions.hasActiveTransaction() == false);
    }

    TEST_CASE("a plugin cannot commit the transaction the executor owns") {
        // The executor guarantees exactly one commit or rollback per command.
        // A plugin committing early would break that quietly, so the call is
        // refused rather than obeyed.
        resetTestState();
        g_test.attemptCommit = true;
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        REQUIRE(fixture.executor.execute("TESTLINE").status == CommandOutcome::Status::Running);
        fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        const CommandOutcome outcome =
            fixture.executor.supply(InputValue::makePoint(Vec3{5.0, 0.0, 0.0}));

        CHECK(g_test.lastStatus == CAD_ERROR_NOT_PERMITTED);
        // The command still completed and its work was still committed once.
        CHECK(outcome.status == CommandOutcome::Status::Completed);
        CHECK(fixture.liveEntityCount() == 1);
        CHECK(fixture.transactions.undoDepth() == 1);
    }

    TEST_CASE("execute_command reaches built-in and plugin commands alike") {
        resetTestState();
        AbiFixture fixture;
        const CadCommandDesc desc = testDescriptor();
        REQUIRE(api().register_command(fixture.pluginHost(), &desc) == CAD_OK);

        CHECK(api().execute_command(fixture.cadDocument(), "TESTLINE") == CAD_OK);
        CHECK(fixture.executor.isRunning());
        fixture.executor.cancel();

        CHECK(api().execute_command(fixture.cadDocument(), "NOSUCHCOMMAND") ==
              CAD_ERROR_NOT_FOUND);
    }
}

TEST_SUITE("AbiDatabase") {
    /// A standalone transaction, the way a plugin acts outside a command.
    struct Scoped {
        const CadApi& table;
        CadTransaction* transaction = nullptr;

        Scoped(const CadApi& api_, CadDocument* document) : table(api_) {
            REQUIRE(table.begin_transaction(document, "Plugin edit", &transaction) == CAD_OK);
        }
        ~Scoped() {
            if (transaction != nullptr) { table.commit_transaction(transaction); }
        }
        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;
        Scoped(Scoped&&) = delete;
        Scoped& operator=(Scoped&&) = delete;
    };

    TEST_CASE("a standalone transaction is its own undo step") {
        AbiFixture fixture;
        CadHandle line = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().add_line(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0},
                                   CadPoint{1, 0, 0}, nullptr, &line) == CAD_OK);
        }
        CHECK(fixture.liveEntityCount() == 1);
        CHECK(fixture.transactions.undoDepth() == 1);

        CHECK(api().undo(fixture.cadDocument()) == CAD_OK);
        CHECK(fixture.liveEntityCount() == 0);
        CHECK(api().redo(fixture.cadDocument()) == CAD_OK);
        CHECK(fixture.liveEntityCount() == 1);
        CHECK(api().undo(fixture.cadDocument()) == CAD_OK);
        CHECK(api().undo(fixture.cadDocument()) == CAD_ERROR_NOT_FOUND);
    }

    TEST_CASE("a second concurrent transaction is refused") {
        AbiFixture fixture;
        Scoped scope{api(), fixture.cadDocument()};
        CadTransaction* second = nullptr;
        CHECK(api().begin_transaction(fixture.cadDocument(), "Nested", &second) ==
              CAD_ERROR_NOT_PERMITTED);
    }

    TEST_CASE("creating entities without a transaction is refused, not ignored") {
        AbiFixture fixture;
        CHECK(api().add_line(nullptr, CAD_NULL_HANDLE, CadPoint{0, 0, 0}, CadPoint{1, 0, 0},
                             nullptr, nullptr) == CAD_ERROR_NO_TRANSACTION);
        CHECK(fixture.liveEntityCount() == 0);
    }

    TEST_CASE("degenerate geometry is rejected at the boundary") {
        AbiFixture fixture;
        Scoped scope{api(), fixture.cadDocument()};
        CHECK(api().add_circle(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0}, 0.0,
                               nullptr, nullptr) == CAD_ERROR_INVALID_ARGUMENT);
        CHECK(api().add_circle(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0}, -5.0,
                               nullptr, nullptr) == CAD_ERROR_INVALID_ARGUMENT);
        CHECK(api().add_text(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0}, "hi", 0.0,
                             0.0, nullptr, nullptr) == CAD_ERROR_INVALID_ARGUMENT);
        CHECK(fixture.liveEntityCount() == 0);
    }

    TEST_CASE("ensure_layer creates a layer that undo takes back") {
        AbiFixture fixture;
        const std::size_t before = fixture.db->layers().size();
        CadHandle layer = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().ensure_layer(scope.transaction, "Walls", &layer) == CAD_OK);
            CHECK(layer != CAD_NULL_HANDLE);

            // Idempotent: asking again finds the one just made.
            CadHandle again = CAD_NULL_HANDLE;
            CHECK(api().ensure_layer(scope.transaction, "walls", &again) == CAD_OK);
            CHECK(again == layer);
        }
        CHECK(fixture.db->layers().size() == before + 1);
        CHECK(api().find_layer(fixture.cadDocument(), "Walls") == layer);

        REQUIRE(api().undo(fixture.cadDocument()) == CAD_OK);
        // The name must be released too, not merely the object erased —
        // otherwise "Walls" stays taken by a layer that no longer exists.
        CHECK(fixture.db->layers().size() == before);
        CHECK(api().find_layer(fixture.cadDocument(), "Walls") == CAD_NULL_HANDLE);
    }

    TEST_CASE("layer colour round-trips through the boundary") {
        AbiFixture fixture;
        const CadHandle layer = api().find_layer(fixture.cadDocument(), "0");
        REQUIRE(layer != CAD_NULL_HANDLE);

        CadColor color{};
        color.method = CAD_COLOR_TRUE;
        color.rgb = 0x336699;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().set_layer_color(scope.transaction, layer, color) == CAD_OK);
        }

        CadColor read{};
        REQUIRE(api().layer_color(fixture.cadDocument(), layer, &read) == CAD_OK);
        CHECK(read.method == CAD_COLOR_TRUE);
        CHECK(read.rgb == 0x336699u);
    }

    TEST_CASE("strings use the two-call buffer pattern") {
        AbiFixture fixture;
        const CadHandle layer = api().find_layer(fixture.cadDocument(), "0");

        std::size_t required = 0;
        REQUIRE(api().layer_name(fixture.cadDocument(), layer, nullptr, 0, &required) == CAD_OK);
        CHECK(required == 2);  // "0" plus the terminator.

        char tooSmall[1] = {};
        CHECK(api().layer_name(fixture.cadDocument(), layer, tooSmall, sizeof(tooSmall),
                               &required) == CAD_ERROR_BUFFER_TOO_SMALL);
        // A truncated layer name would be worse than a clear failure, so
        // nothing was written.
        CHECK(tooSmall[0] == '\0');

        char buffer[8] = {};
        REQUIRE(api().layer_name(fixture.cadDocument(), layer, buffer, sizeof(buffer),
                                 &required) == CAD_OK);
        CHECK(std::string{buffer} == "0");
    }

    TEST_CASE("entity properties are readable and writable") {
        AbiFixture fixture;
        CadHandle line = CAD_NULL_HANDLE;
        CadHandle layer = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().ensure_layer(scope.transaction, "Detail", &layer) == CAD_OK);
            REQUIRE(api().add_line(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0},
                                   CadPoint{10, 0, 0}, nullptr, &line) == CAD_OK);
        }

        CadEntityProperties properties{};
        properties.struct_size = static_cast<std::uint32_t>(sizeof(properties));
        REQUIRE(api().entity_properties(fixture.cadDocument(), line, &properties) == CAD_OK);
        CHECK(properties.color.method == CAD_COLOR_BY_LAYER);
        CHECK(properties.visible == 1);

        properties.layer = layer;
        properties.color.method = CAD_COLOR_INDEX;
        properties.color.index = 1;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().set_entity_properties(scope.transaction, line, &properties) == CAD_OK);
        }

        const Entity* entity = fixture.db->getEntity(Handle{line});
        REQUIRE(entity != nullptr);
        CHECK(entity->layerHandle == Handle{layer});
        CHECK(entity->color.index() == 1);

        // And the change is undoable like any other.
        REQUIRE(api().undo(fixture.cadDocument()) == CAD_OK);
        CHECK(fixture.db->getEntity(Handle{line})->color.isByLayer());
    }

    TEST_CASE("properties without a struct_size are refused") {
        AbiFixture fixture;
        Scoped scope{api(), fixture.cadDocument()};
        CadEntityProperties properties{};  // struct_size deliberately left at 0
        CHECK(api().add_line(scope.transaction, CAD_NULL_HANDLE, CadPoint{0, 0, 0},
                             CadPoint{1, 0, 0}, &properties, nullptr) ==
              CAD_ERROR_INVALID_ARGUMENT);
    }

    TEST_CASE("entity kinds are reported for each concrete type") {
        AbiFixture fixture;
        CadHandle line = CAD_NULL_HANDLE;
        CadHandle circle = CAD_NULL_HANDLE;
        CadHandle arc = CAD_NULL_HANDLE;
        CadHandle point = CAD_NULL_HANDLE;
        CadHandle text = CAD_NULL_HANDLE;
        CadHandle polyline = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            const CadVertex vertices[3] = {{{0, 0, 0}, 0, 0, 0},
                                           {{10, 0, 0}, 0.5, 0, 0},
                                           {{10, 10, 0}, 0, 0, 0}};
            REQUIRE(api().add_line(scope.transaction, 0, CadPoint{0, 0, 0}, CadPoint{1, 0, 0},
                                   nullptr, &line) == CAD_OK);
            REQUIRE(api().add_circle(scope.transaction, 0, CadPoint{0, 0, 0}, 5.0, nullptr,
                                     &circle) == CAD_OK);
            REQUIRE(api().add_arc(scope.transaction, 0, CadPoint{0, 0, 0}, 5.0, 0.0, 1.5, nullptr,
                                  &arc) == CAD_OK);
            REQUIRE(api().add_point(scope.transaction, 0, CadPoint{3, 3, 0}, nullptr, &point) ==
                    CAD_OK);
            REQUIRE(api().add_text(scope.transaction, 0, CadPoint{0, 0, 0}, "hello", 2.5, 0.0,
                                   nullptr, &text) == CAD_OK);
            REQUIRE(api().add_polyline(scope.transaction, 0, vertices, 3, 0, nullptr,
                                       &polyline) == CAD_OK);
        }

        CHECK(api().entity_kind(fixture.cadDocument(), line) == CAD_ENTITY_LINE);
        CHECK(api().entity_kind(fixture.cadDocument(), circle) == CAD_ENTITY_CIRCLE);
        CHECK(api().entity_kind(fixture.cadDocument(), arc) == CAD_ENTITY_ARC);
        CHECK(api().entity_kind(fixture.cadDocument(), point) == CAD_ENTITY_POINT);
        CHECK(api().entity_kind(fixture.cadDocument(), text) == CAD_ENTITY_TEXT);
        CHECK(api().entity_kind(fixture.cadDocument(), polyline) == CAD_ENTITY_POLYLINE);
        CHECK(api().entity_kind(fixture.cadDocument(), 999999) == CAD_ENTITY_UNKNOWN);

        // The bulge survived, so the middle span really is an arc.
        const auto* poly = fixture.db->get<PolylineEntity>(Handle{polyline});
        REQUIRE(poly != nullptr);
        CHECK(poly->vertices[1].bulge == doctest::Approx(0.5));
    }

    TEST_CASE("block_entities uses the two-call array pattern") {
        AbiFixture fixture;
        {
            Scoped scope{api(), fixture.cadDocument()};
            for (int i = 0; i < 3; ++i) {
                REQUIRE(api().add_point(scope.transaction, 0,
                                        CadPoint{static_cast<double>(i), 0, 0}, nullptr,
                                        nullptr) == CAD_OK);
            }
        }

        std::uint32_t count = 0;
        REQUIRE(api().block_entities(fixture.cadDocument(), 0, nullptr, 0, &count) == CAD_OK);
        REQUIRE(count == 3);

        CadHandle one[1] = {};
        CHECK(api().block_entities(fixture.cadDocument(), 0, one, 1, &count) ==
              CAD_ERROR_BUFFER_TOO_SMALL);

        std::vector<CadHandle> handles(count);
        REQUIRE(api().block_entities(fixture.cadDocument(), 0, handles.data(), count, &count) ==
                CAD_OK);
        CHECK(handles.size() == 3);
        for (const CadHandle handle : handles) { CHECK(handle != CAD_NULL_HANDLE); }
    }

    TEST_CASE("geometry queries answer through the entity's curves") {
        AbiFixture fixture;
        CadHandle horizontal = CAD_NULL_HANDLE;
        CadHandle vertical = CAD_NULL_HANDLE;
        CadHandle circle = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().add_line(scope.transaction, 0, CadPoint{0, 0, 0}, CadPoint{10, 0, 0},
                                   nullptr, &horizontal) == CAD_OK);
            REQUIRE(api().add_line(scope.transaction, 0, CadPoint{5, -5, 0}, CadPoint{5, 5, 0},
                                   nullptr, &vertical) == CAD_OK);
            REQUIRE(api().add_circle(scope.transaction, 0, CadPoint{0, 0, 0}, 4.0, nullptr,
                                     &circle) == CAD_OK);
        }

        double length = 0.0;
        REQUIRE(api().entity_length(fixture.cadDocument(), horizontal, &length) == CAD_OK);
        CHECK(length == doctest::Approx(10.0));
        REQUIRE(api().entity_length(fixture.cadDocument(), circle, &length) == CAD_OK);
        CHECK(length == doctest::Approx(2.0 * kPi * 4.0));

        CadPoint closest{};
        double distance = 0.0;
        REQUIRE(api().closest_point(fixture.cadDocument(), horizontal, CadPoint{3, 7, 0},
                                    &closest, &distance) == CAD_OK);
        CHECK(closest.x == doctest::Approx(3.0));
        CHECK(closest.y == doctest::Approx(0.0));
        CHECK(distance == doctest::Approx(7.0));

        std::uint32_t hits = 0;
        REQUIRE(api().intersect_entities(fixture.cadDocument(), horizontal, vertical, nullptr, 0,
                                         &hits) == CAD_OK);
        REQUIRE(hits == 1);
        CadPoint point{};
        REQUIRE(api().intersect_entities(fixture.cadDocument(), horizontal, vertical, &point, 1,
                                         &hits) == CAD_OK);
        CHECK(point.x == doctest::Approx(5.0));
        CHECK(point.y == doctest::Approx(0.0));
    }

    TEST_CASE("transform_entity applies a column-major matrix") {
        AbiFixture fixture;
        CadHandle line = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().add_line(scope.transaction, 0, CadPoint{0, 0, 0}, CadPoint{1, 0, 0},
                                   nullptr, &line) == CAD_OK);
        }
        {
            Scoped scope{api(), fixture.cadDocument()};
            // Translation lives in the last column of a column-major matrix.
            const double matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 3, 0, 1};
            REQUIRE(api().transform_entity(scope.transaction, line, matrix) == CAD_OK);
        }
        const auto* entity = fixture.db->get<LineEntity>(Handle{line});
        REQUIRE(entity != nullptr);
        CHECK_POINT(entity->start, (Vec3{5.0, 3.0, 0.0}));
        CHECK_POINT(entity->end, (Vec3{6.0, 3.0, 0.0}));
    }

    TEST_CASE("erase_entity removes an entity, undoably") {
        AbiFixture fixture;
        CadHandle line = CAD_NULL_HANDLE;
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().add_line(scope.transaction, 0, CadPoint{0, 0, 0}, CadPoint{1, 0, 0},
                                   nullptr, &line) == CAD_OK);
        }
        {
            Scoped scope{api(), fixture.cadDocument()};
            REQUIRE(api().erase_entity(scope.transaction, line) == CAD_OK);
        }
        CHECK(fixture.liveEntityCount() == 0);
        REQUIRE(api().undo(fixture.cadDocument()) == CAD_OK);
        CHECK(fixture.liveEntityCount() == 1);
    }

    TEST_CASE("a missing entity is reported, not assumed") {
        AbiFixture fixture;
        CadBox bounds{};
        CHECK(api().entity_bounds(fixture.cadDocument(), 424242, &bounds) == CAD_ERROR_NOT_FOUND);
        CHECK(std::string{api().last_error()}.find("entity") != std::string::npos);
    }
}

#ifdef CAD_ABI_TEST_PLUGIN

TEST_SUITE("AbiPluginLoading") {
    TEST_CASE("the example plugin loads and contributes a first-class command") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};

        const abi::PluginLoadResult result = manager.load(CAD_ABI_TEST_PLUGIN);
        INFO("loading ", CAD_ABI_TEST_PLUGIN, ": ", result.message);
        REQUIRE(result.loaded);
        REQUIRE(manager.count() == 1);

        const std::vector<abi::PluginInfo> infos = manager.plugins();
        REQUIRE(infos.size() == 1);
        CHECK(infos.front().versionMajor == CAD_ABI_VERSION_MAJOR);
        REQUIRE(infos.front().commandIds.size() == 1);
        CHECK(infos.front().commandIds.front() == "example.draw.boltcircle");

        // It is in the registry alongside anything else, findable by alias.
        REQUIRE(fixture.registry.lookup("BOLTCIRCLE") != nullptr);
        CHECK(fixture.registry.lookup("BC") != nullptr);
        CHECK(fixture.registry.byCategory().count("Draw") == 1);
    }

    TEST_CASE("the example plugin's command draws through the executor") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};
        REQUIRE(manager.load(CAD_ABI_TEST_PLUGIN).loaded);

        REQUIRE(fixture.executor.execute("BOLTCIRCLE").status == CommandOutcome::Status::Running);
        fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        fixture.executor.supply(InputValue::makeNumber(100.0));
        fixture.executor.supply(InputValue::makeNumber(4.0));
        const CommandOutcome outcome = fixture.executor.supply(InputValue::makeEmpty());

        CHECK(outcome.status == CommandOutcome::Status::Completed);
        // Four holes plus the construction circle.
        CHECK(fixture.liveEntityCount() == 5);
        CHECK(fixture.db->findLayer("BOLTCIRCLE") != nullptr);

        // Everything the plugin did is one undo step, including the layer.
        CHECK(fixture.transactions.undoDepth() == 1);
        REQUIRE(fixture.transactions.undo());
        CHECK(fixture.liveEntityCount() == 0);
        CHECK(fixture.db->findLayer("BOLTCIRCLE") == nullptr);
    }

    TEST_CASE("the Marks keyword reaches the plugin") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};
        REQUIRE(manager.load(CAD_ABI_TEST_PLUGIN).loaded);

        REQUIRE(fixture.executor.execute("BC").status == CommandOutcome::Status::Running);
        fixture.executor.supply(InputValue::makePoint(Vec3{0.0, 0.0, 0.0}));
        fixture.executor.supply(InputValue::makeNumber(50.0));
        fixture.executor.supply(InputValue::makeNumber(3.0));

        // The keyword was advertised in the request the plugin returned.
        REQUIRE(fixture.executor.currentRequest() != nullptr);
        REQUIRE(fixture.executor.currentRequest()->keywords.size() == 1);
        CHECK(fixture.executor.currentRequest()->keywords.front().name == "Marks");
        CHECK(fixture.executor.currentRequest()->keywords.front().shortcut == "M");

        fixture.executor.supply(InputValue::makeKeyword("Marks"));

        // Three point nodes plus the construction circle.
        CHECK(fixture.liveEntityCount() == 4);
        const auto entities = fixture.db->entitiesIn(fixture.db->modelSpace());
        std::size_t points = 0;
        for (const Entity* entity : entities) {
            if (dynamic_cast<const PointEntity*>(entity) != nullptr) { ++points; }
        }
        CHECK(points == 3);
    }

    TEST_CASE("unloading a plugin removes its commands") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};
        REQUIRE(manager.load(CAD_ABI_TEST_PLUGIN).loaded);
        REQUIRE(fixture.registry.lookup("BOLTCIRCLE") != nullptr);

        CHECK(manager.unload(CAD_ABI_TEST_PLUGIN));
        CHECK(manager.count() == 0);
        CHECK(fixture.registry.lookup("BOLTCIRCLE") == nullptr);
        CHECK_FALSE(manager.unload(CAD_ABI_TEST_PLUGIN));
    }

    TEST_CASE("loading the same plugin twice is refused rather than duplicated") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};
        REQUIRE(manager.load(CAD_ABI_TEST_PLUGIN).loaded);
        const abi::PluginLoadResult again = manager.load(CAD_ABI_TEST_PLUGIN);
        CHECK_FALSE(again.loaded);
        CHECK(again.message == "already loaded");
        CHECK(manager.count() == 1);
    }

    TEST_CASE("a missing plugin file fails without throwing") {
        AbiFixture fixture;
        abi::PluginManager manager{fixture.registry, fixture.document};
        const abi::PluginLoadResult result = manager.load("no_such_plugin_xyz.dll");
        CHECK_FALSE(result.loaded);
        CHECK(result.message == "file not found");
    }
}

#endif  // CAD_ABI_TEST_PLUGIN
