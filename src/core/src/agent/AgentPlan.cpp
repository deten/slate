#include "cad/agent/AgentPlan.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/TypedInput.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

/// A line that opens or closes a fenced block, whatever language it names.
[[nodiscard]] bool isFence(const std::string& line) {
    return line.rfind("```", 0) == 0;
}

/// Split on whitespace. Coordinates use commas, so they survive intact.
[[nodiscard]] std::vector<std::string> words(const std::string& line) {
    std::istringstream stream{line};
    std::vector<std::string> out;
    std::string word;
    while (stream >> word) {
        out.push_back(word);
    }
    return out;
}

}  // namespace

AgentPlan parseAgentPlan(std::string_view reply) {
    AgentPlan plan;

    std::vector<std::string> lines;
    {
        std::string text{reply};
        std::istringstream stream{text};
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
    }

    // A reply that fences its commands is read only inside the fences; one that
    // does not is read throughout. Told to answer in commands, a model often
    // just does, and refusing that reply because it lacked decoration would
    // fail for a reason the user cannot see.
    const bool fenced = std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return isFence(trimmed(line));
    });

    bool inside = !fenced;
    std::vector<std::string> prose;
    for (const std::string& raw : lines) {
        const std::string line = trimmed(raw);
        if (fenced && isFence(line)) {
            inside = !inside;
            continue;
        }
        if (!inside) {
            if (!line.empty()) {
                prose.push_back(line);
            }
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::vector<std::string> parts = words(line);
        if (parts.empty()) {
            continue;
        }
        AgentCommand command;
        command.source = line;
        command.name = parts.front();
        // Command names are matched without case, and the registry is keyed in
        // upper case. Doing it here means a model writing "line" is not refused
        // for the shape of its letters.
        std::transform(command.name.begin(), command.name.end(), command.name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        command.arguments.assign(parts.begin() + 1, parts.end());
        plan.commands.push_back(std::move(command));
    }

    std::string summary;
    for (const std::string& line : prose) {
        if (!summary.empty()) {
            summary += ' ';
        }
        summary += line;
    }
    plan.summary = std::move(summary);
    return plan;
}

void validateAgentPlan(AgentPlan& plan, const CommandRegistry& registry) {
    std::vector<AgentCommand> kept;
    kept.reserve(plan.commands.size());
    for (AgentCommand& command : plan.commands) {
        if (registry.lookup(command.name) != nullptr) {
            kept.push_back(std::move(command));
            continue;
        }
        plan.refused.push_back(command.source + "  (no such command)");
    }
    plan.commands = std::move(kept);
}

AgentRunResult applyAgentPlan(CommandExecutor& executor, const AgentPlan& plan) {
    AgentRunResult result;
    for (const AgentCommand& command : plan.commands) {
        // Anything still running from the step before would swallow this one's
        // name as an argument.
        if (executor.isRunning()) {
            executor.cancel("Agent step did not finish");
        }

        const CommandOutcome started = executor.execute(command.name);
        if (started.status != CommandOutcome::Status::Running &&
            started.status != CommandOutcome::Status::Completed) {
            ++result.failed;
            result.log.push_back(command.source + "  -> would not start: " + started.message);
            continue;
        }

        bool broke = false;
        for (const std::string& argument : command.arguments) {
            if (!executor.isRunning()) {
                // Finished earlier than the plan expected. The extra inputs are
                // reported rather than fed to whatever runs next, which is how
                // a stray argument becomes a command nobody asked for.
                broke = true;
                result.log.push_back(command.source + "  -> finished before \"" + argument +
                                     "\"");
                break;
            }
            const InputRequest* request = executor.currentRequest();
            const std::optional<InputValue> value =
                request == nullptr ? std::nullopt : parseTypedInput(*request, argument);
            if (!value) {
                broke = true;
                result.log.push_back(command.source + "  -> did not understand \"" + argument +
                                     "\"");
                break;
            }
            const CommandOutcome fed = executor.supply(*value);
            if (fed.status == CommandOutcome::Status::Failed ||
                fed.status == CommandOutcome::Status::Cancelled) {
                broke = true;
                result.log.push_back(command.source + "  -> rejected \"" + argument +
                                     "\": " + fed.message);
                break;
            }
        }

        if (broke) {
            if (executor.isRunning()) {
                executor.cancel("Agent step rejected");
            }
            ++result.failed;
            continue;
        }

        // One Enter to close an open-ended command, and only while one is still
        // asking. At the bare prompt an Enter repeats the last command, which
        // would run every step of the plan twice.
        if (executor.isRunning()) {
            executor.supply(InputValue::makeEmpty());
        }
        if (executor.isRunning()) {
            executor.cancel("Agent step left a command open");
            ++result.failed;
            result.log.push_back(command.source + "  -> wanted more input");
            continue;
        }

        ++result.completed;
        result.log.push_back(command.source + "  -> done");
    }
    return result;
}

}  // namespace cad
