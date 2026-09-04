#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/MacroFile.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/TextEntities.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    /// Register a macro from its file text, as the loader does.
    const CommandDefinition& install(std::string_view text) {
        MacroParseResult parsed = parseMacro(text, "test.macro");
        REQUIRE(parsed.macro.has_value());
        REQUIRE(parsed.problems.empty());
        std::vector<CommandDefinition> commands = macroCommands({*parsed.macro});
        REQUIRE(commands.size() == 1);
        const std::string id = commands.front().id;
        registry->registerCommand(std::move(commands.front())).release();
        const CommandDefinition* installed = registry->get(id);
        REQUIRE(installed != nullptr);
        return *installed;
    }

    [[nodiscard]] std::vector<Entity*> entities() { return db->entitiesIn(db->modelSpace()); }

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

}  // namespace

// ------------------------------------------------------------------- parsing

TEST_CASE("a macro file reads its header and its steps") {
    const MacroParseResult parsed = parseMacro(R"(name: box
alias: BX, BOXIT
title: Quick Box
category: Draw
help: A 400 by 300 rectangle at the origin

RECTANGLE
0,0
400,300
)",
                                               "box.macro");

    REQUIRE(parsed.ok());
    const MacroDefinition& macro = *parsed.macro;
    // Names and aliases are upper-cased: the command line matches them that
    // way, and a macro written in lower case must still be typable.
    CHECK(macro.name == "BOX");
    CHECK(macro.aliases == std::vector<std::string>{"BX", "BOXIT"});
    CHECK(macro.title == "Quick Box");
    CHECK(macro.category == "Draw");
    CHECK(macro.description == "A 400 by 300 rectangle at the origin");
    CHECK(macro.source == "box.macro");
    REQUIRE(macro.steps.size() == 3);
    CHECK(macro.steps[0].text == "RECTANGLE");
    CHECK(macro.steps[2].text == "400,300");
}

TEST_CASE("the body's three special lines") {
    const MacroParseResult parsed = parseMacro(R"(name: T

LINE
\
;
# a comment
\;
\#
)");
    REQUIRE(parsed.ok());
    const std::vector<MacroStep>& steps = parsed.macro->steps;
    REQUIRE(steps.size() == 5);
    CHECK(steps[0].kind == MacroStep::Kind::Input);
    CHECK(steps[1].kind == MacroStep::Kind::Pause);
    CHECK(steps[2].kind == MacroStep::Kind::Enter);
    // The comment is gone, and the escaped forms are literal text.
    CHECK(steps[3].kind == MacroStep::Kind::Input);
    CHECK(steps[3].text == ";");
    CHECK(steps[4].text == "#");
}

TEST_CASE("a file may open with a comment block and a blank line") {
    // The first thing anybody writes. A blank line under the comments must
    // not end a header that has not started, or the whole file becomes body
    // and the command has no name.
    const MacroParseResult parsed = parseMacro(R"(# What this does.
#
# And why.

name: TAGGED
alias: TG

RECTANGLE
0,0
9,9
)");
    REQUIRE(parsed.ok());
    CHECK(parsed.macro->name == "TAGGED");
    CHECK(parsed.macro->aliases == std::vector<std::string>{"TG"});
    CHECK(parsed.macro->steps.size() == 3);
}

TEST_CASE("a blank line is formatting, not Enter") {
    // other CAD software's script files treat a blank line as Enter, so a file that reads
    // clearly does something different from the same file written densely.
    const MacroParseResult parsed = parseMacro("name: T\n\nLINE\n\n0,0\n\n\n1,1\n");
    REQUIRE(parsed.ok());
    REQUIRE(parsed.macro->steps.size() == 3);
    for (const MacroStep& step : parsed.macro->steps) {
        CHECK(step.kind == MacroStep::Kind::Input);
    }
}

TEST_CASE("a body line may contain a colon without being read as a header") {
    const MacroParseResult parsed = parseMacro("name: NOTE\n\nMTEXT\n\\\n\\\n2.5\nSCALE: 1/4in\n");
    REQUIRE(parsed.ok());
    CHECK(parsed.macro->steps.back().text == "SCALE: 1/4in");
}

TEST_CASE("a macro that cannot work says why, and yields nothing") {
    const MacroParseResult noName = parseMacro("title: Nameless\n\nLINE\n");
    CHECK_FALSE(noName.macro.has_value());
    CHECK(noName.problems.size() == 1);

    const MacroParseResult noSteps = parseMacro("name: EMPTY\n\n");
    CHECK_FALSE(noSteps.macro.has_value());

    const MacroParseResult spaced = parseMacro("name: TWO WORDS\n\nLINE\n");
    CHECK_FALSE(spaced.macro.has_value());

    // The line number is in the message, because that is what makes a broken
    // macro fixable rather than merely reported.
    const MacroParseResult unknown = parseMacro("name: T\nwidget: 3\n\nLINE\n");
    REQUIRE(unknown.problems.size() == 1);
    CHECK(unknown.problems.front().starts_with("line 2:"));
}

TEST_CASE("parameters carry a default and a prompt") {
    const MacroParseResult parsed =
        parseMacro("name: T\nparam: SIZE = 2x6 | Stud size\nparam: BARE\n\nLINE\n");
    REQUIRE(parsed.ok());
    REQUIRE(parsed.macro->parameters.size() == 2);
    CHECK(parsed.macro->parameters[0].name == "SIZE");
    CHECK(parsed.macro->parameters[0].defaultValue == "2x6");
    CHECK(parsed.macro->parameters[0].prompt == "Stud size");
    CHECK(parsed.macro->parameters[1].name == "BARE");
    CHECK(parsed.macro->parameters[1].defaultValue.empty());
    // With no prompt given, the name is the prompt.
    CHECK(parsed.macro->parameters[1].prompt == "BARE");
}

TEST_CASE("substitution replaces known names and leaves the rest alone") {
    const std::map<std::string, std::string> values{{"SIZE", "2x8"}, {"N", "4"}};
    CHECK(expandMacroText("TYP. $SIZE STUD", values) == "TYP. 2x8 STUD");
    CHECK(expandMacroText("$N of $N", values) == "4 of 4");
    // Not a parameter: a price at a text prompt must survive.
    CHECK(expandMacroText("$50 each", values) == "$50 each");
    CHECK(expandMacroText("$UNKNOWN", values) == "$UNKNOWN");
}

// ----------------------------------------------------------------- playback

TEST_CASE("a macro runs its steps as though they were typed") {
    Harness h;
    h.install("name: BOX\n\nRECTANGLE\n0,0\n400,300\n");

    const CommandOutcome outcome = h.executor.execute("BOX");
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    CHECK_FALSE(h.executor.isRunning());
    CHECK(h.ofType<PolylineEntity>().size() == 1);
}

TEST_CASE("a macro chains several commands") {
    Harness h;
    h.install("name: TWO\n\nLINE\n0,0\n100,0\n;\nCIRCLE\n50,50\n25\n");

    h.executor.execute("TWO");
    CHECK(h.ofType<LineEntity>().size() == 1);
    CHECK(h.ofType<CircleEntity>().size() == 1);
}

TEST_CASE("a pause hands the prompt to the user and the rest plays on after") {
    Harness h;
    h.install("name: HALF\n\nLINE\n0,0\n\\\n;\n");

    // Runs up to the pause and stops there, with LINE waiting for a point.
    const CommandOutcome outcome = h.executor.execute("HALF");
    CHECK(outcome.status == CommandOutcome::Status::Running);
    REQUIRE(h.executor.isRunning());
    REQUIRE(h.executor.currentRequest() != nullptr);
    CHECK(h.executor.currentRequest()->kind == InputRequest::Kind::Point);
    CHECK(h.ofType<LineEntity>().empty());

    // The user's point satisfies the pause; the trailing Enter then ends LINE.
    h.executor.supply(InputValue::makePoint(Vec3{100.0, 50.0}));
    CHECK_FALSE(h.executor.isRunning());
    REQUIRE(h.ofType<LineEntity>().size() == 1);
    CHECK_POINT(h.ofType<LineEntity>().front()->end, (Vec3{100.0, 50.0, 0.0}));
}

TEST_CASE("parameters are asked for, then substituted into the steps") {
    Harness h;
    h.install("name: NOTE\nparam: WORDS = HELLO\n\nMTEXT\n0,0\n100,20\n2.5\n$WORDS\n");

    // The macro asks for its parameter first.
    const CommandOutcome asked = h.executor.execute("NOTE");
    REQUIRE(asked.status == CommandOutcome::Status::Running);
    REQUIRE(h.executor.currentRequest() != nullptr);
    CHECK(h.executor.currentRequest()->prompt == "WORDS");

    h.executor.supply(InputValue::makeText("SEE PLAN"));
    REQUIRE(h.ofType<MTextEntity>().size() == 1);
    CHECK(h.ofType<MTextEntity>().front()->contents == "SEE PLAN");
}

TEST_CASE("Enter at a parameter prompt takes the default") {
    Harness h;
    h.install("name: NOTE\nparam: WORDS = HELLO\n\nMTEXT\n0,0\n100,20\n2.5\n$WORDS\n");

    h.executor.execute("NOTE");
    h.executor.supply(InputValue::makeEmpty());
    REQUIRE(h.ofType<MTextEntity>().size() == 1);
    CHECK(h.ofType<MTextEntity>().front()->contents == "HELLO");
}

TEST_CASE("a step the command cannot use stops the macro rather than the drawing") {
    Harness h;
    // "sideways" is not a point. The CIRCLE that follows must not receive it,
    // and must not run with the leftovers either.
    h.install("name: BAD\n\nLINE\n0,0\nsideways\n100,100\n;\nCIRCLE\n50,50\n25\n");

    const CommandOutcome outcome = h.executor.execute("BAD");
    CHECK(outcome.status == CommandOutcome::Status::Failed);
    CHECK_FALSE(h.executor.isRunning());
    CHECK(h.executor.isPlayingBack() == false);
    CHECK(h.ofType<LineEntity>().empty());
    CHECK(h.ofType<CircleEntity>().empty());

    const std::vector<HistoryEntry>& history = h.executor.history();
    const bool explained = std::ranges::any_of(history, [](const HistoryEntry& entry) {
        return entry.kind == HistoryEntry::Kind::Error &&
               entry.text.find("sideways") != std::string::npos;
    });
    CHECK(explained);
}

TEST_CASE("Escape during a macro stops the whole macro") {
    Harness h;
    h.install("name: TWO\n\nLINE\n0,0\n\\\n;\nCIRCLE\n50,50\n25\n");

    h.executor.execute("TWO");
    REQUIRE(h.executor.isRunning());
    REQUIRE(h.executor.isPlayingBack());

    h.executor.supply(InputValue::makeCancel());
    CHECK_FALSE(h.executor.isPlayingBack());
    // The circle after the pause never runs. Otherwise Escape would have to be
    // pressed once per remaining step.
    CHECK(h.ofType<CircleEntity>().empty());
}

TEST_CASE("a macro that runs itself stops instead of hanging") {
    Harness h;
    h.install("name: LOOP\n\nLOOP\n");

    const CommandOutcome outcome = h.executor.execute("LOOP");
    CHECK_FALSE(h.executor.isPlayingBack());
    static_cast<void>(outcome);

    const bool stopped = std::ranges::any_of(h.executor.history(), [](const HistoryEntry& e) {
        return e.kind == HistoryEntry::Kind::Error &&
               e.text.find("running itself") != std::string::npos;
    });
    CHECK(stopped);
}

TEST_CASE("a macro is a first-class command") {
    Harness h;
    const CommandDefinition& definition =
        h.install("name: BOX\nalias: BX\ncategory: Draw\ntitle: Quick Box\n\nRECTANGLE\n0,0\n9,9\n");

    // Reachable by its alias, filed under a category, and carrying a title --
    // which is what the menus, the palette and the ribbon read.
    CHECK(h.registry->lookup("BX") == &definition);
    CHECK(definition.category == "Draw");
    CHECK(definition.title == "Quick Box");

    h.executor.execute("bx");
    CHECK(h.ofType<PolylineEntity>().size() == 1);
}

TEST_CASE("a later macro of the same name replaces an earlier one") {
    // What makes a user's folder shadow the installed one.
    MacroDefinition shipped;
    shipped.name = "BOX";
    shipped.source = "install/box.macro";
    shipped.steps = {{MacroStep::Kind::Input, "RECTANGLE", 1}};

    MacroDefinition mine = shipped;
    mine.source = "user/box.macro";

    const std::vector<CommandDefinition> commands = macroCommands({shipped, mine});
    // Collapsed to one before it reaches the registry, which refuses to
    // overwrite -- registering both would have thrown rather than shadowed.
    REQUIRE(commands.size() == 1);
    CHECK(commands.front().help == "user/box.macro");
}

// ------------------------------------------------------------------ folders

namespace {

namespace fs = std::filesystem;

/// A throwaway folder of macro files. The loader is about what is on disk --
/// extensions, ordering, shadowing, a file that will not open -- so mocking the
/// filesystem here would test the mock.
class TempFolders {
public:
    TempFolders() {
        root_ = fs::temp_directory_path() /
                ("cad-macro-test-" + std::to_string(std::hash<const void*>{}(this)));
        fs::create_directories(root_ / "shipped");
        fs::create_directories(root_ / "mine");
    }

    ~TempFolders() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempFolders(const TempFolders&) = delete;
    TempFolders& operator=(const TempFolders&) = delete;
    TempFolders(TempFolders&&) = delete;
    TempFolders& operator=(TempFolders&&) = delete;

    void write(const std::string& folder, const std::string& file, std::string_view text) const {
        std::ofstream out{root_ / folder / file, std::ios::binary};
        out << text;
    }

    [[nodiscard]] std::vector<fs::path> order() const {
        return {root_ / "shipped", root_ / "mine", root_ / "absent"};
    }

private:
    fs::path root_;
};

}  // namespace

TEST_CASE("macros load from a folder, and a missing folder is not a problem") {
    const TempFolders folders;
    folders.write("shipped", "box.macro", R"(name: BOX

RECTANGLE
0,0
9,9
)");
    folders.write("shipped", "notes.txt", R"(name: NOPE

LINE
)");

    const std::vector<fs::path> order = folders.order();
    const MacroLibrary library = loadMacros(order);

    // Only .macro files, and the absent third folder says nothing.
    REQUIRE(library.macros.size() == 1);
    CHECK(library.macros.front().name == "BOX");
    CHECK(library.problems.empty());
}

TEST_CASE("a user's folder shadows the installed one") {
    const TempFolders folders;
    folders.write("shipped", "box.macro", R"(name: BOX
title: Shipped

RECTANGLE
0,0
9,9
)");
    folders.write("mine", "box.macro", R"(name: BOX
title: Mine

CIRCLE
0,0
5
)");

    const std::vector<fs::path> order = folders.order();
    const MacroLibrary library = loadMacros(order);
    REQUIRE(library.macros.size() == 2);

    // Two files, one command: the contest is settled before registration.
    auto registry = createStandardRegistry();
    for (CommandDefinition command : macroCommands(library.macros)) {
        registry->registerCommand(std::move(command)).release();
    }
    const CommandDefinition* found = registry->lookup("BOX");
    REQUIRE(found != nullptr);
    CHECK(found->title == "Mine");
}

TEST_CASE("a broken macro costs the user that file and no others") {
    const TempFolders folders;
    folders.write("shipped", "good.macro", R"(name: GOOD

RECTANGLE
0,0
9,9
)");
    folders.write("shipped", "broken.macro", R"(title: no name here

LINE
)");

    const std::vector<fs::path> order = folders.order();
    const MacroLibrary library = loadMacros(order);

    REQUIRE(library.macros.size() == 1);
    CHECK(library.macros.front().name == "GOOD");
    REQUIRE(library.problems.size() == 1);
    // Named, so it can be found and fixed.
    CHECK(library.problems.front().starts_with("broken.macro:"));
}

TEST_CASE("SCRIPT runs a file of steps once") {
    const TempFolders folders;
    folders.write("mine", "setup.macro", R"(RECTANGLE
0,0
40,30
)");

    Harness h;
    for (CommandDefinition command : scriptCommands()) {
        h.registry->registerCommand(std::move(command)).release();
    }

    const fs::path script = folders.order()[1] / "setup.macro";
    h.executor.execute("SCRIPT");
    h.executor.supply(InputValue::makeText(script.string()));

    CHECK(h.ofType<PolylineEntity>().size() == 1);
    // Nothing was registered: a script is "do this now", not "give me a
    // command that does this".
    CHECK(h.registry->lookup("SETUP") == nullptr);
}

TEST_CASE("SCRIPT on a file that is not there fails without stopping the session") {
    Harness h;
    for (CommandDefinition command : scriptCommands()) {
        h.registry->registerCommand(std::move(command)).release();
    }
    h.executor.execute("SCRIPT");
    const CommandOutcome outcome = h.executor.supply(InputValue::makeText("no-such-file.macro"));
    CHECK(outcome.status == CommandOutcome::Status::Failed);
    CHECK_FALSE(h.executor.isRunning());
}
