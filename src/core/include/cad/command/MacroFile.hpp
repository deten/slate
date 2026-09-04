#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cad/command/CommandRegistry.hpp"

namespace cad {

/// One thing a macro does at the command line.
///
/// Deliberately not a syntax tree. A macro is *the command line, written down*:
/// every step is text that would have worked had a drafter typed it, and it is
/// turned into input by `parseTypedInput`, the same function the command line,
/// the agent and the test harness all use. There is one command language, and
/// a macro does not get a second one.
struct MacroStep {
    enum class Kind : std::uint8_t {
        /// Typed text: a command name, a coordinate, a keyword, a word of text.
        Input,
        /// Return pressed with nothing typed -- accept a default, or end an
        /// open-ended command like LINE.
        Enter,
        /// Hand the next prompt back to the user, then carry on.
        ///
        /// The thing that makes a macro useful rather than a recording: "run
        /// LEADER, let them pick the three points, then type the note" is the
        /// shape almost every real macro has.
        Pause,
    };

    Kind kind = Kind::Input;
    std::string text;
    /// Line in the source file, for saying where a problem is.
    int line = 0;
};

/// A value the macro asks for before it runs.
struct MacroParameter {
    std::string name;
    /// Shown at the prompt. Defaults to the name when not given.
    std::string prompt;
    /// Accepted when the user presses Enter. Empty means there is no default.
    std::string defaultValue;
};

/// A command defined by a file rather than by C++.
///
/// Registered through the same `CommandDefinition` a built-in and a plugin use,
/// so it appears on the menus, takes an alias, accepts a keyboard shortcut and
/// is reachable from a script without any of those systems knowing it came from
/// a file. That is what "first-class" has to mean to be worth claiming.
struct MacroDefinition {
    std::string name;
    std::vector<std::string> aliases;
    std::string title;
    std::string description;
    /// Grouping for the palette and the generated menus.
    std::string category = "Custom";
    std::vector<MacroParameter> parameters;
    std::vector<MacroStep> steps;

    /// The file it was read from, for reporting and for shadowing.
    std::string source;
};

/// What reading one macro file produced.
struct MacroParseResult {
    std::optional<MacroDefinition> macro;
    /// One message per problem, each naming the line. Reported rather than
    /// thrown: a broken macro must cost the user that macro and no others.
    std::vector<std::string> problems;

    [[nodiscard]] bool ok() const noexcept { return macro.has_value() && problems.empty(); }
};

/// Read a macro file.
///
/// The format is a header of `key: value` lines, a blank line, then the body:
///
///     name: WALLTAG
///     alias: WT
///     title: Wall Tag
///     category: Annotate
///     help: A leader carrying the standard stud-wall note
///     param: SIZE = 2x6 | Stud size
///
///     LEADER
///     \
///     \
///     \
///     ;
///     TYP. $SIZE STUD WALL
///
/// In the body, `\` alone pauses for the user, `;` alone is Enter, a line
/// starting with `#` is a comment, and everything else is typed. `$NAME`
/// substitutes a parameter.
///
/// **A blank line is not Enter**, which is where other CAD software's script files trip
/// people: a file that reads clearly, with its steps grouped, must not mean
/// something different from the same file written densely. Enter is `;`,
/// spelled out, and blank lines are free.
[[nodiscard]] MacroParseResult parseMacro(std::string_view text, std::string source = {});

/// Replace every `$NAME` with its value.
///
/// An unknown name is left as it stands rather than blanked: a macro that types
/// `$50` into a text prompt means fifty dollars, and swallowing it would be a
/// silent edit to what the drafter wrote.
[[nodiscard]] std::string expandMacroText(std::string_view text,
                                          const std::map<std::string, std::string>& values);

/// Read only the body of a macro file: the steps, with no header.
///
/// What a script is. `SCRIPT` runs a file of steps once without registering
/// anything, which is the difference between "do this now" and "give me a
/// command that does this".
[[nodiscard]] std::vector<MacroStep> parseMacroSteps(std::string_view body);

/// Every macro found in a list of folders, and everything wrong with them.
struct MacroLibrary {
    /// In load order. A later macro of the same name shadows an earlier one,
    /// which is how a user's folder overrides the installed one.
    std::vector<MacroDefinition> macros;
    std::vector<std::string> problems;
};

/// Load `*.macro` from each folder in turn.
///
/// A folder that does not exist is not a problem -- most installations have no
/// user macros -- but a file that will not parse is, and it is reported by name
/// so it can be fixed. One bad file costs the user that file and no others.
[[nodiscard]] MacroLibrary loadMacros(std::span<const std::filesystem::path> folders);

/// SCRIPT: run a file of steps once.
[[nodiscard]] std::vector<CommandDefinition> scriptCommands();

/// Turn parsed macros into commands the registry can hold.
///
/// Later definitions win on a name clash, which is what makes a user's folder
/// shadow the installed one.
[[nodiscard]] std::vector<CommandDefinition> macroCommands(std::vector<MacroDefinition> macros);

/// The id a macro registers under, derived from its name.
[[nodiscard]] std::string macroCommandId(std::string_view name);

}  // namespace cad
