#include "cad/command/MacroFile.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>

#include "cad/command/Command.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string trim(std::string_view text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    const auto begin = std::ranges::find_if(text, notSpace);
    const auto end = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    return begin >= end ? std::string{} : std::string{begin, end};
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Split on a separator, trimming each piece and dropping the empty ones.
[[nodiscard]] std::vector<std::string> splitList(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(separator, start);
        const std::size_t end = at == std::string_view::npos ? text.size() : at;
        std::string piece = trim(text.substr(start, end - start));
        if (!piece.empty()) {
            out.push_back(std::move(piece));
        }
        if (at == std::string_view::npos) {
            break;
        }
        start = at + 1;
    }
    return out;
}

/// A command name has to be typable at the command line, which rules out
/// whitespace: the line is split on it, so a two-word name could never be
/// matched and would register a command nobody can reach.
[[nodiscard]] bool isUsableName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::ranges::none_of(name, [](unsigned char c) { return std::isspace(c) != 0; });
}

/// Undo the escapes that only matter at the start of a body line.
[[nodiscard]] std::string unescapeBody(std::string_view line) {
    if (line.starts_with("\\#") || line.starts_with("\\;") || line.starts_with("\\\\")) {
        return std::string{line.substr(1)};
    }
    return std::string{line};
}

/// Split text into lines, with CRLF endings normalized away.
[[nodiscard]] std::vector<std::string> readLines(std::string_view text) {
    std::string normalized{text};
    std::erase(normalized, '\r');
    std::vector<std::string> out;
    std::istringstream stream{normalized};
    std::string line;
    while (std::getline(stream, line)) {
        out.push_back(std::move(line));
    }
    return out;
}

/// The body: one step per meaningful line.
[[nodiscard]] std::vector<MacroStep> readSteps(const std::vector<std::string>& lines,
                                               std::size_t from) {
    std::vector<MacroStep> steps;
    for (std::size_t index = from; index < lines.size(); ++index) {
        const std::string line = trim(lines[index]);
        const int number = static_cast<int>(index) + 1;
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line == "\\") {
            steps.push_back({MacroStep::Kind::Pause, {}, number});
        } else if (line == ";") {
            steps.push_back({MacroStep::Kind::Enter, {}, number});
        } else {
            steps.push_back({MacroStep::Kind::Input, unescapeBody(line), number});
        }
    }
    return steps;
}

void readParameter(MacroDefinition& macro, std::string_view value, int lineNumber,
                   std::vector<std::string>& problems) {
    // `NAME = default | prompt`, with everything after the name optional.
    std::string_view rest = value;
    std::string prompt;
    if (const std::size_t bar = rest.find('|'); bar != std::string_view::npos) {
        prompt = trim(rest.substr(bar + 1));
        rest = rest.substr(0, bar);
    }

    MacroParameter parameter;
    if (const std::size_t equals = rest.find('='); equals != std::string_view::npos) {
        parameter.defaultValue = trim(rest.substr(equals + 1));
        rest = rest.substr(0, equals);
    }
    parameter.name = trim(rest);
    parameter.prompt = prompt.empty() ? parameter.name : prompt;

    if (parameter.name.empty()) {
        problems.push_back("line " + std::to_string(lineNumber) + ": a param needs a name");
        return;
    }
    if (!isUsableName(parameter.name)) {
        problems.push_back("line " + std::to_string(lineNumber) + ": parameter name \"" +
                           parameter.name + "\" cannot contain spaces");
        return;
    }
    macro.parameters.push_back(std::move(parameter));
}

}  // namespace

std::string macroCommandId(std::string_view name) {
    std::string id = "cad.macro.";
    for (const char c : name) {
        id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return id;
}

std::string expandMacroText(std::string_view text,
                            const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '$') {
            out += text[i];
            continue;
        }
        std::size_t end = i + 1;
        while (end < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_')) {
            ++end;
        }
        const std::string name{text.substr(i + 1, end - i - 1)};
        const auto found = values.find(name);
        if (found == values.end()) {
            // Left alone. `$50` at a text prompt is fifty dollars, and blanking
            // it would be a silent edit to what the drafter wrote.
            out += text[i];
            continue;
        }
        out += found->second;
        i = end - 1;
    }
    return out;
}

std::vector<MacroStep> parseMacroSteps(std::string_view body) {
    return readSteps(readLines(body), 0);
}

MacroParseResult parseMacro(std::string_view text, std::string source) {
    MacroParseResult result;
    MacroDefinition macro;
    macro.source = std::move(source);

    const std::vector<std::string> lines = readLines(text);

    // The header runs to the first blank line. An explicit boundary rather than
    // "the first line that is not key: value", because a body line can
    // perfectly well contain a colon -- a note reading "SCALE: 1/4in = 1ft"
    // would otherwise be read as a header called SCALE.
    std::size_t index = 0;
    bool sawBlank = false;
    bool sawSetting = false;
    for (; index < lines.size(); ++index) {
        const std::string line = trim(lines[index]);
        const int number = static_cast<int>(index) + 1;
        if (line.empty()) {
            // Blank lines before the first setting are just spacing under a
            // comment block, which is how a file that explains itself begins.
            // Only a blank line *after* the header has started ends it --
            // otherwise the first thing anybody writes, a paragraph of comment
            // followed by a gap, silently turns the whole header into body.
            if (!sawSetting) {
                continue;
            }
            sawBlank = true;
            ++index;
            break;
        }
        if (line.starts_with('#')) {
            continue;
        }
        sawSetting = true;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            result.problems.push_back("line " + std::to_string(number) +
                                      ": expected \"key: value\" in the header, got \"" + line +
                                      "\"");
            continue;
        }
        const std::string key = upper(trim(std::string_view{line}.substr(0, colon)));
        const std::string value = trim(std::string_view{line}.substr(colon + 1));

        if (key == "NAME") {
            macro.name = upper(value);
        } else if (key == "ALIAS" || key == "ALIASES") {
            for (const std::string& alias : splitList(value, ',')) {
                macro.aliases.push_back(upper(alias));
            }
        } else if (key == "TITLE") {
            macro.title = value;
        } else if (key == "HELP" || key == "DESCRIPTION") {
            macro.description = value;
        } else if (key == "CATEGORY") {
            macro.category = value;
        } else if (key == "PARAM") {
            readParameter(macro, value, number, result.problems);
        } else {
            result.problems.push_back("line " + std::to_string(number) + ": unknown setting \"" +
                                      key + "\"");
        }
    }

    // Ran off the end without a blank line, so there is no body.
    macro.steps = sawBlank ? readSteps(lines, index) : std::vector<MacroStep>{};

    if (macro.name.empty()) {
        result.problems.push_back("no name: a macro needs \"name: SOMETHING\"");
    } else if (!isUsableName(macro.name)) {
        result.problems.push_back("the name \"" + macro.name + "\" cannot contain spaces");
    }
    if (macro.steps.empty()) {
        result.problems.push_back("no steps: a macro that does nothing is not a command");
    }
    if (macro.title.empty()) {
        macro.title = macro.name;
    }

    if (result.problems.empty()) {
        result.macro = std::move(macro);
    }
    return result;
}

MacroLibrary loadMacros(std::span<const std::filesystem::path> folders) {
    MacroLibrary library;

    for (const std::filesystem::path& folder : folders) {
        std::error_code error;
        if (!std::filesystem::is_directory(folder, error)) {
            // Not a problem. Most installations have no user macros, and
            // reporting an absent folder every start would train people to
            // ignore the report that matters.
            continue;
        }

        // Sorted, because a directory iterator's order is the filesystem's and
        // two machines must load the same macros in the same order -- which is
        // what decides a shadowing contest between two files in one folder.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator{folder, error}) {
            if (entry.is_regular_file(error) && entry.path().extension() == ".macro") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);

        for (const std::filesystem::path& file : files) {
            std::ifstream stream{file, std::ios::binary};
            if (!stream) {
                library.problems.push_back(file.string() + ": could not be read");
                continue;
            }
            std::ostringstream contents;
            contents << stream.rdbuf();

            MacroParseResult parsed = parseMacro(contents.str(), file.string());
            if (!parsed.macro) {
                for (const std::string& problem : parsed.problems) {
                    library.problems.push_back(file.filename().string() + ": " + problem);
                }
                continue;
            }
            library.macros.push_back(std::move(*parsed.macro));
        }
    }
    return library;
}

namespace {

/// The command a macro file becomes.
///
/// It collects the parameters and then *queues* the body rather than running
/// it. Running commands from inside a running command would nest the executor
/// in itself; queueing hands the steps back to the loop that was already there,
/// which is the same path a typed command, a script and the agent take.
class MacroInstance final : public CommandInstance {
public:
    explicit MacroInstance(MacroDefinition definition) : macro_(std::move(definition)) {}

    StepResult start(CommandContext& context) override { return next(context); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        const MacroParameter& parameter = macro_.parameters[collected_];
        std::string value = input.kind == InputValue::Kind::Text ? input.text : std::string{};
        if (value.empty()) {
            value = parameter.defaultValue;
        }
        values_[parameter.name] = std::move(value);
        ++collected_;
        return next(context);
    }

private:
    StepResult next(CommandContext& context) {
        if (collected_ < macro_.parameters.size()) {
            const MacroParameter& parameter = macro_.parameters[collected_];
            InputRequest request;
            request.kind = InputRequest::Kind::Text;
            request.prompt = parameter.prompt;
            request.allowEmpty = true;
            if (!parameter.defaultValue.empty()) {
                request.defaultText = parameter.defaultValue;
            }
            return StepResult::ask(request);
        }

        std::vector<MacroStep> steps = macro_.steps;
        for (MacroStep& step : steps) {
            if (step.kind == MacroStep::Kind::Input) {
                step.text = expandMacroText(step.text, values_);
            }
        }
        context.enqueue(std::move(steps));
        return StepResult::done();
    }

    MacroDefinition macro_;
    std::map<std::string, std::string> values_;
    std::size_t collected_ = 0;
};

/// SCRIPT — run a file of steps once, without registering anything.
class ScriptCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Script file to run";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Text || input.text.empty()) {
            return StepResult::cancelled();
        }

        const std::filesystem::path path{input.text};
        std::ifstream stream{path, std::ios::binary};
        if (!stream) {
            return StepResult::failed("Cannot read " + input.text);
        }
        std::ostringstream contents;
        contents << stream.rdbuf();

        std::vector<MacroStep> steps = parseMacroSteps(contents.str());
        if (steps.empty()) {
            return StepResult::cancelled("The script has no steps");
        }
        const std::size_t count = steps.size();
        context.enqueue(std::move(steps));
        return StepResult::done("Running " + std::to_string(count) + " step(s)");
    }
};

}  // namespace

std::vector<CommandDefinition> scriptCommands() {
    CommandDefinition script;
    script.id = "cad.tools.script";
    script.name = "SCRIPT";
    script.aliases = {"SCR"};
    script.title = "Run Script";
    script.description = "Run a file of command steps once";
    script.category = "Tools";
    // The script's own steps open their own transactions as they run. One
    // wrapped around the whole playback would stay open across all of them.
    script.transactionMode = CommandTransactionMode::ReadOnly;
    // Repeating with Enter would silently re-run the whole file.
    script.repeatable = false;
    script.create = [] { return std::make_unique<ScriptCommand>(); };

    std::vector<CommandDefinition> commands;
    commands.push_back(std::move(script));
    return commands;
}

std::vector<CommandDefinition> macroCommands(std::vector<MacroDefinition> macros) {
    std::vector<CommandDefinition> commands;
    commands.reserve(macros.size());
    // Where a name is decided. Registration deliberately refuses to overwrite,
    // so two macros of one name have to be resolved before they reach it --
    // and the later one wins, which is what makes a user's folder shadow the
    // installed one. The earlier keeps its position in the list so the menus do
    // not reorder themselves when somebody overrides one.
    std::map<std::string, std::size_t> byId;

    for (MacroDefinition& macro : macros) {
        CommandDefinition definition;
        definition.id = macroCommandId(macro.name);
        definition.name = macro.name;
        definition.aliases = macro.aliases;
        definition.title = macro.title;
        definition.description = macro.description;
        definition.category = macro.category.empty() ? std::string{"Custom"} : macro.category;
        definition.help = macro.source;
        definition.fromMacro = true;
        // The macro command itself only collects parameters; the steps it
        // queues open their own transactions as they run. Wrapping it in one
        // would leave an empty transaction open across the whole playback.
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        definition.create = [macro] { return std::make_unique<MacroInstance>(macro); };

        const auto seen = byId.find(definition.id);
        if (seen != byId.end()) {
            commands[seen->second] = std::move(definition);
            continue;
        }
        byId[definition.id] = commands.size();
        commands.push_back(std::move(definition));
    }
    return commands;
}

}  // namespace cad
