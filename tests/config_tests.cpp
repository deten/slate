/// Tests for the configuration store and command customisation.
///
/// The property that matters most here is not that a value round-trips — it is
/// that writing one leaves the rest of the user's file alone. A settings file
/// that reformats itself, or quietly drops the comments explaining why a
/// setting is what it is, teaches people not to edit it and then not to trust
/// it.

#include "test_helpers.hpp"

#include <string>

#include "cad/command/CommandConfig.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/config/ConfigFile.hpp"

using namespace cad;

namespace {

/// A file with everything that could be damaged: comments, blank lines,
/// unusual spacing, an unknown section, and a line that is not valid at all.
[[nodiscard]] std::string messyConfig() {
    return
        "# Slate configuration\n"
        "; a second comment style\n"
        "\n"
        "[aliases]\n"
        "c = COPY\n"
        "   zz   =   ZOOM   \n"
        "\n"
        "[future]\n"
        "# a section this build knows nothing about\n"
        "somethingNew = 42\n"
        "this line is not a setting at all\n";
}

}  // namespace

TEST_SUITE("ConfigFile") {
    TEST_CASE("reads values, trimming space and ignoring case in keys") {
        const ConfigFile config = ConfigFile::parse(messyConfig());
        CHECK(config.text("aliases", "c") == "COPY");
        CHECK(config.text("aliases", "zz") == "ZOOM");
        // Sections and keys are case-insensitive; values are not.
        CHECK(config.text("ALIASES", "ZZ") == "ZOOM");
        CHECK(config.text("aliases", "missing", "fallback") == "fallback");
    }

    TEST_CASE("editing a value leaves comments, spacing and unknown data intact") {
        ConfigFile config = ConfigFile::parse(messyConfig());
        config.set("aliases", "c", "CIRCLE");

        const std::string out = config.toText();
        CHECK(out.find("# Slate configuration") != std::string::npos);
        CHECK(out.find("; a second comment style") != std::string::npos);
        CHECK(out.find("# a section this build knows nothing about") != std::string::npos);
        CHECK(out.find("somethingNew = 42") != std::string::npos);
        // Even the line that is not a setting survives, because a typo should
        // cost the user that setting and nothing else.
        CHECK(out.find("this line is not a setting at all") != std::string::npos);

        CHECK(out.find("c = CIRCLE") != std::string::npos);
        CHECK(out.find("c = COPY") == std::string::npos);
    }

    TEST_CASE("an unchanged file round-trips to itself") {
        // The strongest form of "nothing was damaged": parse and write with no
        // edits at all, and get the same bytes back.
        const std::string original = messyConfig();
        CHECK(ConfigFile::parse(original).toText() == original);
    }

    TEST_CASE("a new key joins its existing section rather than the end of the file") {
        ConfigFile config = ConfigFile::parse(messyConfig());
        config.set("aliases", "mm", "MOVE");

        const std::string out = config.toText();
        const std::size_t alias = out.find("mm = MOVE");
        const std::size_t future = out.find("[future]");
        REQUIRE(alias != std::string::npos);
        REQUIRE(future != std::string::npos);
        CHECK(alias < future);
    }

    TEST_CASE("a new section is appended with a heading and a blank line before it") {
        ConfigFile config = ConfigFile::parse(messyConfig());
        config.set("shortcuts", "LINE", "Ctrl+Shift+L");

        const std::string out = config.toText();
        CHECK(out.find("[shortcuts]") != std::string::npos);
        CHECK(out.find("LINE = Ctrl+Shift+L") != std::string::npos);
        CHECK(ConfigFile::parse(out).text("shortcuts", "line") == "Ctrl+Shift+L");
    }

    TEST_CASE("typed accessors parse, and refuse half-parsed values") {
        ConfigFile config;
        config.set("general", "count", "12");
        config.set("general", "scale", "2.5");
        config.set("general", "rubbish", "12abc");
        config.setBoolean("general", "enabled", true);
        config.set("general", "loose", "YES");
        config.set("general", "off", "no");

        CHECK(config.integer("general", "count") == 12);
        CHECK(config.number("general", "scale") == doctest::Approx(2.5));
        // Trailing rubbish means the user meant something else.
        CHECK(config.integer("general", "rubbish", -1) == -1);
        CHECK(config.number("general", "rubbish", -1.0) == doctest::Approx(-1.0));
        CHECK(config.boolean("general", "enabled"));
        CHECK(config.boolean("general", "loose"));
        CHECK_FALSE(config.boolean("general", "off", true));
    }

    TEST_CASE("a missing file is an empty configuration rather than an error") {
        const ConfigFile config = ConfigFile::load("no-such-directory/no-such-file.ini");
        CHECK(config.empty());
        CHECK(config.text("aliases", "c", "none") == "none");
    }

    TEST_CASE("saving and loading preserves the file exactly") {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "slate_config_test.ini";
        std::filesystem::remove(path);

        ConfigFile config = ConfigFile::parse(messyConfig());
        config.set("aliases", "mm", "MOVE");
        REQUIRE(config.save(path));

        const ConfigFile reloaded = ConfigFile::load(path);
        CHECK(reloaded.toText() == config.toText());
        CHECK(reloaded.text("aliases", "mm") == "MOVE");
        std::filesystem::remove(path);
    }

    TEST_CASE("entries and sections come back in file order") {
        const ConfigFile config = ConfigFile::parse(messyConfig());
        const auto aliases = config.entries("aliases");
        REQUIRE(aliases.size() == 2);
        CHECK(aliases[0].first == "c");
        CHECK(aliases[1].first == "zz");

        const auto sections = config.sections();
        REQUIRE(sections.size() == 2);
        CHECK(sections[0] == "aliases");
        CHECK(sections[1] == "future");
    }

    TEST_CASE("removing a key takes only that line") {
        ConfigFile config = ConfigFile::parse(messyConfig());
        CHECK(config.remove("aliases", "c"));
        CHECK_FALSE(config.remove("aliases", "c"));

        const std::string out = config.toText();
        CHECK(out.find("c = COPY") == std::string::npos);
        CHECK(out.find("zz") != std::string::npos);
        CHECK(out.find("# Slate configuration") != std::string::npos);
    }
}

TEST_SUITE("Command customisation") {
    TEST_CASE("a user alias overrides a built-in one") {
        auto registry = createStandardRegistry();
        // C is CIRCLE out of the box; many other CAD software users want it to be COPY.
        REQUIRE(registry->lookup("C") != nullptr);
        CHECK(registry->lookup("C")->name == "CIRCLE");

        const ConfigFile config = ConfigFile::parse("[aliases]\nc = COPY\n");
        const ConfigApplyResult result = applyAliases(*registry, config);

        CHECK(result.ok());
        CHECK(result.applied == 1);
        CHECK(registry->lookup("C")->name == "COPY");
        // CIRCLE is still reachable by its own name, which is the guarantee
        // that makes overriding safe.
        CHECK(registry->lookup("CIRCLE")->name == "CIRCLE");
    }

    TEST_CASE("an alias cannot shadow a command's own name") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse("[aliases]\nMOVE = CIRCLE\n");
        const ConfigApplyResult result = applyAliases(*registry, config);

        CHECK(result.applied == 0);
        CHECK(result.problems.size() == 1);
        // Otherwise a bad config line could make a command unreachable from
        // inside the application that would have to fix it.
        CHECK(registry->lookup("MOVE")->name == "MOVE");
    }

    TEST_CASE("a bad alias is reported and the rest are still applied") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse(
            "[aliases]\n"
            "mm = MOVE\n"
            "oops = NOSUCHCOMMAND\n"
            "cc = COPY\n");

        const ConfigApplyResult result = applyAliases(*registry, config);

        CHECK(result.applied == 2);
        REQUIRE(result.problems.size() == 1);
        CHECK(result.problems[0].find("NOSUCHCOMMAND") != std::string::npos);
        CHECK(registry->lookup("mm")->name == "MOVE");
        CHECK(registry->lookup("cc")->name == "COPY");
    }

    TEST_CASE("an alias may be given by command id as well as by name") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse("[aliases]\nq = cad.modify.move\n");
        CHECK(applyAliases(*registry, config).applied == 1);
        CHECK(registry->lookup("q")->name == "MOVE");
    }

    TEST_CASE("aliases are listed back for a command, excluding its name") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse("[aliases]\nmm = MOVE\n");
        CHECK(applyAliases(*registry, config).applied == 1);

        // Normalized to upper case, as command names are throughout.
        const std::vector<std::string> aliases = registry->aliasesFor("cad.modify.move");
        CHECK(std::find(aliases.begin(), aliases.end(), "MM") != aliases.end());
        CHECK(std::find(aliases.begin(), aliases.end(), "M") != aliases.end());
        CHECK(std::find(aliases.begin(), aliases.end(), "MOVE") == aliases.end());
    }

    TEST_CASE("shortcuts resolve to command ids and report unknown commands") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse(
            "[shortcuts]\n"
            "LINE = Ctrl+Shift+L\n"
            "NOSUCHCOMMAND = Ctrl+K\n");

        ConfigApplyResult result;
        const std::vector<ShortcutBinding> bindings = readShortcuts(*registry, config, result);

        REQUIRE(bindings.size() == 1);
        CHECK(bindings[0].commandId == "cad.draw.line");
        CHECK(bindings[0].keys == "Ctrl+Shift+L");
        CHECK(result.problems.size() == 1);
    }

    /// Muscle memory is the feature. A drafter coming from other CAD software types `cp`
    /// for COPY and `c` for CIRCLE without looking, so these are pinned rather
    /// than left to drift as commands are added and someone reaches for a
    /// letter that is already spoken for.
    ///
    /// Every pair below is stock acad.pgp. View commands are registered by the
    /// shell and so are absent here; they are checked where they are built.
    TEST_CASE("built-in aliases match other CAD software's") {
        auto registry = createStandardRegistry();

        const std::pair<const char*, const char*> expected[] = {
            {"L", "LINE"},         {"PL", "PLINE"},        {"C", "CIRCLE"},
            {"A", "ARC"},          {"REC", "RECTANG"},     {"POL", "POLYGON"},
            {"PO", "POINT"},       {"H", "HATCH"},         {"BH", "HATCH"},
            {"M", "MOVE"},         {"CO", "COPY"},         {"CP", "COPY"},
            {"RO", "ROTATE"},      {"SC", "SCALE"},        {"MI", "MIRROR"},
            {"O", "OFFSET"},       {"TR", "TRIM"},         {"EX", "EXTEND"},
            {"F", "FILLET"},       {"CHA", "CHAMFER"},     {"BR", "BREAK"},
            {"J", "JOIN"},         {"X", "EXPLODE"},       {"S", "STRETCH"},
            {"E", "ERASE"},        {"DT", "TEXT"},         {"T", "MTEXT"},
            {"MT", "MTEXT"},       {"B", "BLOCK"},         {"I", "INSERT"},
            {"U", "UNDO"},         {"DLI", "DIMLINEAR"},   {"DAL", "DIMALIGNED"},
            {"DRA", "DIMRADIUS"},  {"DDI", "DIMDIAMETER"}, {"DAN", "DIMANGULAR"},
            {"DOR", "DIMORDINATE"},
        };

        for (const auto& [alias, command] : expected) {
            CAPTURE(alias);
            const CommandDefinition* found = registry->lookup(alias);
            REQUIRE(found != nullptr);
            CHECK(found->name == command);
        }
    }

    TEST_CASE("the starter configuration is valid and applies cleanly") {
        auto registry = createStandardRegistry();
        const ConfigFile config = ConfigFile::parse(defaultConfigText());

        // Everything in it is commented out, so it changes nothing — but it
        // must still parse, and its sections must be the ones the loader reads.
        const ConfigApplyResult result = applyAliases(*registry, config);
        CHECK(result.ok());
        CHECK(result.applied == 0);

        const std::vector<std::string> sections = config.sections();
        CHECK(std::find(sections.begin(), sections.end(), kAliasSection) != sections.end());
        CHECK(std::find(sections.begin(), sections.end(), kShortcutSection) != sections.end());
    }
}
