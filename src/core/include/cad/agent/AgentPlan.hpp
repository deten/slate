#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cad {

class CommandExecutor;
class CommandRegistry;

/// One instruction an agent proposed: a command and the inputs it needs.
///
/// The same thing a person types. An agent that can only produce command
/// invocations can only do what the user could have done by hand, which is the
/// property that makes handing it a drawing reasonable at all -- there is no
/// second path into the database for it to take, and every edit it makes is
/// undoable by the ordinary means.
struct AgentCommand {
    std::string name;
    std::vector<std::string> arguments;
    /// The line as the model wrote it, for showing back to the user.
    std::string source;
};

/// What an agent proposed, after reading its reply.
struct AgentPlan {
    /// Prose the model wrote outside its command block.
    std::string summary;
    std::vector<AgentCommand> commands;
    /// Lines that will not be run, each with the reason. Shown rather than
    /// dropped: a plan silently missing a step is worse than one that says
    /// which step it refused and why.
    std::vector<std::string> refused;

    [[nodiscard]] bool empty() const noexcept { return commands.empty(); }
};

/// Read a model's reply into a plan.
///
/// The reply is expected to carry its commands in a fenced block, one
/// invocation per line: a command name followed by the inputs it needs,
/// separated by spaces. Coordinates use commas and so survive the split.
///
///     I will draw the outline and a door swing.
///     ```
///     RECTANGLE 0,0 400,300
///     ARC 0,0 60,60 120,0
///     ```
///
/// A reply with no fence is read as commands throughout, because a model told
/// to answer in commands often simply does. Blank lines and lines starting with
/// `#` are ignored, so a model that annotates its plan is not punished for it.
///
/// Parsing is deliberately separate from deciding what may run: this function
/// never consults a registry and never refuses anything. `validateAgentPlan`
/// does that, so a plan can be read, shown and argued about before any
/// judgement is applied to it.
[[nodiscard]] AgentPlan parseAgentPlan(std::string_view reply);

/// Move anything the registry does not know into `refused`.
///
/// An agent may only invoke commands this application actually has. A model
/// inventing a plausible-sounding command is the common failure, and running
/// its arguments through whatever else happened to match would be worse than
/// refusing: the drawing would change in a way nobody asked for and the reply
/// would still read as though it had worked.
void validateAgentPlan(AgentPlan& plan, const CommandRegistry& registry);

/// What running a plan did.
struct AgentRunResult {
    int completed = 0;
    int failed = 0;
    /// One line per command, saying what happened, in order.
    std::vector<std::string> log;

    [[nodiscard]] bool ok() const noexcept { return failed == 0; }
};

/// Run a validated plan through the executor.
///
/// Each command is started by name and then fed its inputs one at a time,
/// which is exactly the path a typed command takes. A command still asking for
/// something after its inputs run out is given one Enter, which is how a person
/// ends an open-ended command like LINE; a command that has already finished is
/// not, because an Enter at the bare prompt repeats the last command and the
/// plan would run each step twice.
///
/// A command that fails does not stop the rest. The drawing is left in whatever
/// state the successful steps produced and the result says which failed --
/// stopping half way would leave the same partial state with less explanation.
AgentRunResult applyAgentPlan(CommandExecutor& executor, const AgentPlan& plan);

}  // namespace cad
