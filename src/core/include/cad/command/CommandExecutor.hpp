#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cad/command/Command.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/command/MacroFile.hpp"

namespace cad {

struct HistoryEntry {
    enum class Kind : std::uint8_t { Command, Prompt, Input, Info, Warning, Error };

    Kind kind = Kind::Info;
    std::string text;
};

struct CommandOutcome {
    enum class Status : std::uint8_t {
        /// Still awaiting input; the command has not finished.
        Running,
        Completed,
        Cancelled,
        Failed,
        Unknown,
        Unavailable,
    };

    Status status = Status::Running;
    std::string message;
};

/// How the executor reads and writes the current selection.
///
/// An interface rather than owned state, because selection is shared with the
/// UI and the renderer, which must see the same set.
class SelectionProvider {
public:
    SelectionProvider() = default;
    virtual ~SelectionProvider() = default;

    SelectionProvider(const SelectionProvider&) = delete;
    SelectionProvider& operator=(const SelectionProvider&) = delete;
    SelectionProvider(SelectionProvider&&) = delete;
    SelectionProvider& operator=(SelectionProvider&&) = delete;

    [[nodiscard]] virtual std::vector<Handle> selection() const = 0;
    virtual void setSelection(std::vector<Handle> handles) = 0;
    virtual void clearSelection() = 0;
};

/// A selection provider backed by a plain vector, for tests and headless use.
class SimpleSelection final : public SelectionProvider {
public:
    [[nodiscard]] std::vector<Handle> selection() const override { return handles_; }
    void setSelection(std::vector<Handle> handles) override { handles_ = std::move(handles); }
    void clearSelection() override { handles_.clear(); }

private:
    std::vector<Handle> handles_;
};

/// Drives command execution.
///
/// Owns the loop a CAD command lives inside: start it, show its prompt, feed it
/// input, repeat until it finishes, and guarantee that however it ends —
/// completion, Escape, an exception, or another command interrupting it — the
/// database is left consistent and the transaction is either committed once or
/// fully rolled back.
///
/// Living in the core, with no UI dependency, means the same executor runs
/// commands from the command line, a menu, a script, a macro and a test.
class CommandExecutor {
public:
    CommandExecutor(Database& db, TransactionManager& transactions, CommandRegistry& registry,
                    SelectionProvider& selection);
    ~CommandExecutor();

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;
    CommandExecutor(CommandExecutor&&) = delete;
    CommandExecutor& operator=(CommandExecutor&&) = delete;

    /// Run a command by name or alias.
    ///
    /// Returns as soon as the command either finishes or blocks for input. A
    /// command awaiting input reports `Running`; feed it with `supply`.
    CommandOutcome execute(std::string_view nameOrAlias);

    /// Run a specific command definition.
    CommandOutcome run(const CommandDefinition& definition);

    /// Supply input to the running command.
    ///
    /// Every user interaction — a point picked on canvas, text typed at the
    /// command line, an Escape — arrives here. One entry point is what makes
    /// macro playback and scripting identical to interactive use.
    CommandOutcome supply(const InputValue& input);

    /// Cancel the running command, rolling back everything it did.
    void cancel(const std::string& reason = "Cancelled");

    // ------------------------------------------------------------ playback

    /// Queue steps to be played into the executor as it becomes ready for them.
    ///
    /// This is the whole of macro and script playback. Rather than a second
    /// path into the database, queued steps are fed through `supply` and
    /// `execute` exactly as a person's keystrokes are -- so a macro can do
    /// precisely what its author could have done by hand, and no more.
    ///
    /// A `Pause` step hands the next prompt back to the user and the rest of
    /// the queue waits for them, which is what separates a macro from a
    /// recording.
    void enqueue(std::vector<MacroStep> steps);

    [[nodiscard]] bool isPlayingBack() const noexcept { return !queue_.empty(); }

    /// Abandon whatever is queued.
    ///
    /// Escape stops the macro, not just the command it happened to be inside.
    /// Anything else means a drafter has to press it once per remaining step.
    void clearQueue();

    /// Re-run the last command, for Enter-to-repeat.
    CommandOutcome repeatLast();

    /// Report cursor movement, refreshing the preview. Never consumes input.
    void moveCursor(const Vec3& world);

    /// Geometry the running command wants shown under the cursor.
    ///
    /// Owned by the executor and rebuilt on each cursor move; cleared whenever
    /// a command ends, so a cancelled command cannot leave a ghost behind.
    [[nodiscard]] const std::vector<std::unique_ptr<Entity>>& preview() const noexcept {
        return preview_;
    }

    [[nodiscard]] bool isRunning() const noexcept { return active_ != nullptr; }

    /// The command currently running, or null.
    [[nodiscard]] const CommandDefinition* activeCommand() const noexcept;

    /// What the running command is asking for, or null.
    [[nodiscard]] const InputRequest* currentRequest() const noexcept;

    /// The last command that completed, for Enter-to-repeat.
    [[nodiscard]] const CommandDefinition* lastCommand() const noexcept { return lastCommand_; }

    [[nodiscard]] const std::vector<HistoryEntry>& history() const noexcept { return history_; }

    void clearHistory() { history_.clear(); }

    /// Cap on retained history lines.
    std::size_t maxHistory = 500;

    /// @internal Used by CommandContext.
    void push(HistoryEntry::Kind kind, const std::string& text);

    /// Register a listener notified whenever the executor's state changes.
    std::size_t addListener(std::function<void()> listener);
    void removeListener(std::size_t id);

private:
    struct ActiveCommand;

    /// Handle Escape.
    ///
    /// The command is asked first, because "cancel" does not always mean
    /// "discard everything". A repeating command such as OFFSET or LINE has
    /// already committed real work by the time the user presses Escape to stop
    /// it, and throwing that away would be wrong — other CAD software keeps it.
    ///
    /// The command cannot refuse to stop: if it responds with another prompt,
    /// it is cancelled regardless. Escape must always be an exit.
    CommandOutcome handleCancelInput();

    CommandOutcome applyStep(const StepResult& result);
    CommandOutcome handleException(const std::exception& error);

    /// End the active command exactly once, committing or rolling back.
    ///
    /// Every termination path routes through here, which makes "cancellation
    /// must never corrupt the drawing" a structural property rather than
    /// something each command has to remember.
    CommandOutcome finish(const CommandOutcome& outcome, bool commit);

    void logInput(const InputValue& input);
    void notify();

    /// Feed queued steps for as long as the executor can take them.
    ///
    /// Called after every state change. Stops at a pause, at an empty queue, or
    /// at a step the running command will not accept -- which abandons the rest
    /// rather than feeding a coordinate to whatever command happens to be next.
    /// The three entry points, without the draining wrapper around them.
    CommandOutcome executeOnce(std::string_view nameOrAlias);
    CommandOutcome runOnce(const CommandDefinition& definition);
    CommandOutcome supplyOnce(const InputValue& input);

    /// Play queued steps for as long as the executor can take them, and report
    /// where it ended up.
    ///
    /// Stops at a pause, at an empty queue, or at a step the running command
    /// will not accept -- which abandons the rest rather than feeding a
    /// coordinate to whatever command happens to run next.
    CommandOutcome drainAfter(CommandOutcome outcome);

    std::vector<MacroStep> queue_;
    /// Guards against a macro that queues itself.
    bool draining_ = false;

    Database& db_;
    TransactionManager& transactions_;
    CommandRegistry& registry_;
    SelectionProvider& selection_;

    std::unique_ptr<ActiveCommand> active_;
    std::vector<std::unique_ptr<Entity>> preview_;
    const CommandDefinition* lastCommand_ = nullptr;
    std::vector<HistoryEntry> history_;
    std::vector<std::pair<std::size_t, std::function<void()>>> listeners_;
    std::size_t nextListenerId_ = 1;
};

}  // namespace cad
