#pragma once

#include <span>
#include <string>
#include <string_view>

#include "cad/agent/AgentPlan.hpp"
#include "cad/db/Handle.hpp"

namespace cad {

class CommandRegistry;
class Database;

/// What is sent to an agent.
struct AgentRequest {
    /// How to answer: the command language, and the rule that it may only use
    /// commands this application has.
    std::string system;
    /// What the drawing currently contains.
    std::string context;
    /// What the user asked for, in their own words.
    std::string instruction;
};

/// What came back.
struct AgentReply {
    std::string text;
    /// Empty when the exchange succeeded. Carries the transport's own words --
    /// a refused key, a timeout -- because a user who has just pasted a key
    /// needs to know which of those it was.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Somewhere to send a request.
///
/// An interface, so the whole path from a user's sentence to an edited drawing
/// can be tested without a network, a key, or a bill. The implementation that
/// speaks HTTP lives in the application layer, because `cad_core` has no
/// business opening sockets; everything above it here is ordinary code that
/// works the same whichever provider is plugged in.
class AgentProvider {
public:
    AgentProvider() = default;
    virtual ~AgentProvider() = default;

    AgentProvider(const AgentProvider&) = delete;
    AgentProvider& operator=(const AgentProvider&) = delete;
    AgentProvider(AgentProvider&&) = delete;
    AgentProvider& operator=(AgentProvider&&) = delete;

    /// Human-readable, for the log and for error messages.
    [[nodiscard]] virtual std::string name() const = 0;

    [[nodiscard]] virtual AgentReply send(const AgentRequest& request) = 0;
};

/// A description of the drawing, for the model to work from.
///
/// Deliberately a summary and not the drawing: layers, how much of what, the
/// extents, what is selected. A model given eight thousand entities would spend
/// its context on them and still not be able to act on any one in particular,
/// because it cannot click. What it can do is issue commands, and commands need
/// coordinates and layer names -- which is what this carries.
[[nodiscard]] std::string describeDrawing(const Database& db,
                                          std::span<const Handle> selection = {});

/// The instructions that go with every request.
///
/// The command list is generated from the registry rather than written down, so
/// it cannot drift from what the application actually has: a plugin's commands
/// are offered the moment it is loaded, and a command that is removed stops
/// being suggested.
[[nodiscard]] std::string agentSystemPrompt(const CommandRegistry& registry);

/// Ask an agent for a plan: build the request, send it, read and check the
/// reply. Nothing is applied -- see `applyAgentPlan` -- so a caller can show
/// the plan and ask before anything touches the drawing.
[[nodiscard]] AgentPlan requestAgentPlan(AgentProvider& provider,
                                         const std::string& instruction, const Database& db,
                                         const CommandRegistry& registry,
                                         std::span<const Handle> selection,
                                         std::string* error,
                                         std::string_view standards = {});

}  // namespace cad
