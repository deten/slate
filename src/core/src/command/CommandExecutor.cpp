#include "cad/command/CommandExecutor.hpp"

#include <algorithm>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <unordered_set>

#include "cad/command/TypedInput.hpp"
#include "cad/db/InsertEntity.hpp"

namespace cad {
namespace {

/// A selection with every selected block reference's attributes added.
///
/// A reference and its attributes are one thing to the person selecting it:
/// moving, copying or erasing the reference is expected to take the text
/// along. Done once here, where every selection enters a command, rather
/// than in each command that transforms things.
[[nodiscard]] std::vector<Handle> withAttributes(Database& db, std::vector<Handle> handles) {
    std::unordered_set<Handle> seen(handles.begin(), handles.end());
    const std::size_t picked = handles.size();
    for (std::size_t i = 0; i < picked; ++i) {
        const auto* insert = db.get<InsertEntity>(handles[i]);
        if (insert == nullptr) {
            continue;
        }
        for (const Handle attribute : insert->attributeHandles) {
            if (seen.insert(attribute).second) {
                handles.push_back(attribute);
            }
        }
    }
    return handles;
}

}  // namespace
}  // namespace cad

namespace cad {

// -------------------------------------------------------------- CommandContext

Transaction& CommandContext::transaction() const {
    if (transaction_ == nullptr) {
        throw std::logic_error(
            "This command is registered as read-only and has no transaction. "
            "Change its transactionMode if it needs to modify the drawing.");
    }
    return *transaction_;
}

void CommandContext::log(const std::string& message) const {
    executor_.push(HistoryEntry::Kind::Info, message);
}

void CommandContext::warn(const std::string& message) const {
    executor_.push(HistoryEntry::Kind::Warning, message);
}

void CommandContext::enqueue(std::vector<MacroStep> steps) const {
    executor_.enqueue(std::move(steps));
}

// ------------------------------------------------------------- ActiveCommand

struct CommandExecutor::ActiveCommand {
    const CommandDefinition* definition = nullptr;
    CommandInstancePtr instance;
    std::unique_ptr<CommandContext> context;
    Transaction* transaction = nullptr;
    InputRequest request;
    bool hasRequest = false;
};

// ------------------------------------------------------------ CommandExecutor

CommandExecutor::CommandExecutor(Database& db, TransactionManager& transactions,
                                 CommandRegistry& registry, SelectionProvider& selection)
    : db_(db), transactions_(transactions), registry_(registry), selection_(selection) {}

CommandExecutor::~CommandExecutor() {
    // A command still running at teardown must not leave its transaction open.
    if (active_ != nullptr) {
        cancel("Shutting down");
    }
}

const CommandDefinition* CommandExecutor::activeCommand() const noexcept {
    return active_ == nullptr ? nullptr : active_->definition;
}

const InputRequest* CommandExecutor::currentRequest() const noexcept {
    if (active_ == nullptr || !active_->hasRequest) {
        return nullptr;
    }
    return &active_->request;
}

CommandOutcome CommandExecutor::execute(std::string_view nameOrAlias) {
    return drainAfter(executeOnce(nameOrAlias));
}

CommandOutcome CommandExecutor::executeOnce(std::string_view nameOrAlias) {
    const CommandDefinition* command = registry_.lookup(nameOrAlias);
    if (command == nullptr) {
        const std::string suggestion = registry_.autocomplete(nameOrAlias);
        std::string message = "Unknown command: " + std::string{nameOrAlias};
        if (!suggestion.empty()) {
            message += ". Did you mean " + suggestion + "?";
        }
        push(HistoryEntry::Kind::Error, message);
        return {CommandOutcome::Status::Unknown, message};
    }
    return runOnce(*command);
}

CommandOutcome CommandExecutor::run(const CommandDefinition& definition) {
    return drainAfter(runOnce(definition));
}

CommandOutcome CommandExecutor::runOnce(const CommandDefinition& definition) {
    // Starting a command while one is running cancels the first. This matches
    // other CAD software and is far less surprising than queueing or refusing.
    if (active_ != nullptr) {
        cancel("Interrupted by another command");
    }

    std::vector<Handle> currentSelection = selection_.selection();
    const bool actsOnPriorSelection = definition.supportsPickfirst && !currentSelection.empty();
    if (definition.isAvailable && !definition.isAvailable(db_, currentSelection)) {
        const std::string message = definition.name + " is not available right now.";
        push(HistoryEntry::Kind::Warning, message);
        return {CommandOutcome::Status::Unavailable, message};
    }

    push(HistoryEntry::Kind::Command, definition.name);

    // A command that awaits input outlives this function, so its transaction is
    // opened with an explicit lifetime and closed by `finish` — which every
    // termination path routes through. A scoped transaction would commit while
    // the command was still waiting for its first point.
    Transaction* transaction = nullptr;
    if (definition.transactionMode == CommandTransactionMode::Single) {
        transaction = &transactions_.begin(definition.name);
    }

    auto active = std::make_unique<ActiveCommand>();
    active->definition = &definition;
    active->instance = definition.create();
    active->transaction = transaction;
    active->context = std::make_unique<CommandContext>(
        db_, transactions_, transaction, withAttributes(db_, std::move(currentSelection)), *this);
    active_ = std::move(active);

    try {
        const CommandOutcome outcome = applyStep(active_->instance->start(*active_->context));
        // Used up the selection it was started on and finished at once: see
        // supplyOnce for the same rule when the selection is supplied later.
        if (actsOnPriorSelection && active_ == nullptr) {
            selection_.clearSelection();
        }
        return outcome;
    } catch (const std::exception& error) {
        return handleException(error);
    }
}

CommandOutcome CommandExecutor::supply(const InputValue& input) {
    return drainAfter(supplyOnce(input));
}

CommandOutcome CommandExecutor::supplyOnce(const InputValue& input) {
    if (active_ == nullptr) {
        return {CommandOutcome::Status::Unknown, "No command is running"};
    }

    if (input.kind == InputValue::Kind::Cancel) {
        return handleCancelInput();
    }

    logInput(input);

    try {
        if (input.kind == InputValue::Kind::Selection) {
            InputValue expanded = input;
            expanded.selection = withAttributes(db_, std::move(expanded.selection));
            const CommandOutcome outcome =
                applyStep(active_->instance->supply(expanded, *active_->context));
            // A command that took the selection and finished with it -- LAYOFF,
            // ERASE -- has used it up. Left selected, the objects stay
            // highlighted with nothing about to act on them, on layers that may
            // just have gone off. A command still running keeps its selection.
            if (active_ == nullptr) {
                selection_.clearSelection();
            }
            return outcome;
        }
        return applyStep(active_->instance->supply(input, *active_->context));
    } catch (const std::exception& error) {
        return handleException(error);
    }
}

CommandOutcome CommandExecutor::handleCancelInput() {
    StepResult result;
    try {
        result = active_->instance->supply(InputValue::makeCancel(), *active_->context);
    } catch (const std::exception& error) {
        return handleException(error);
    }

    // Escape stops the macro, not merely the command it happened to be inside.
    // Anything else would make a drafter press it once per remaining step.
    if (!queue_.empty()) {
        push(HistoryEntry::Kind::Info, "Macro cancelled.");
        queue_.clear();
    }

    // The command cannot refuse to stop. If it responds with another prompt, it
    // is cancelled regardless: Escape must always be an exit.
    if (result.kind == StepResult::Kind::Request) {
        push(HistoryEntry::Kind::Info, "*Cancel*");
        return finish({CommandOutcome::Status::Cancelled, "Cancelled"}, false);
    }
    if (result.kind == StepResult::Kind::Cancelled) {
        push(HistoryEntry::Kind::Info, "*Cancel*");
    }
    return applyStep(result);
}

CommandOutcome CommandExecutor::applyStep(const StepResult& result) {
    switch (result.kind) {
        case StepResult::Kind::Request:
            active_->request = result.request;
            active_->hasRequest = true;
            push(HistoryEntry::Kind::Prompt, result.request.prompt);
            notify();
            return {CommandOutcome::Status::Running, {}};

        case StepResult::Kind::Done:
            if (!result.message.empty()) {
                push(HistoryEntry::Kind::Info, result.message);
            }
            return finish({CommandOutcome::Status::Completed, result.message}, true);

        case StepResult::Kind::Cancelled:
            return finish({CommandOutcome::Status::Cancelled,
                           result.message.empty() ? "Cancelled" : result.message},
                          false);

        case StepResult::Kind::Failed:
            push(HistoryEntry::Kind::Error, result.message);
            return finish({CommandOutcome::Status::Failed, result.message}, false);
    }
    return {CommandOutcome::Status::Running, {}};
}

CommandOutcome CommandExecutor::handleException(const std::exception& error) {
    const std::string message = std::string{"Command failed: "} + error.what();
    push(HistoryEntry::Kind::Error, message);
    return finish({CommandOutcome::Status::Failed, error.what()}, false);
}

CommandOutcome CommandExecutor::finish(const CommandOutcome& outcome, bool commit) {
    if (active_ == nullptr) {
        return outcome;
    }

    // Move out first, so a listener that inspects the executor during commit
    // sees no active command rather than a half-torn-down one.
    std::unique_ptr<ActiveCommand> finished = std::move(active_);
    active_.reset();
    // A cancelled command must not leave a ghost rubber-band on screen.
    preview_.clear();

    try {
        finished->instance->cleanup(*finished->context);
    } catch (...) {
        // Cleanup must never mask the real outcome.
    }

    if (finished->transaction != nullptr) {
        if (commit) {
            transactions_.commit(*finished->transaction);
        } else {
            transactions_.abort(*finished->transaction);
        }
    }

    if (outcome.status == CommandOutcome::Status::Completed) {
        lastCommand_ = finished->definition;
    }

    notify();
    return outcome;
}

void CommandExecutor::enqueue(std::vector<MacroStep> steps) {
    queue_.insert(queue_.end(), std::make_move_iterator(steps.begin()),
                  std::make_move_iterator(steps.end()));
}

void CommandExecutor::clearQueue() {
    queue_.clear();
}

CommandOutcome CommandExecutor::drainAfter(CommandOutcome outcome) {
    if (draining_ || queue_.empty()) {
        return outcome;
    }

    // A macro whose last step starts it again would otherwise spin forever with
    // no way to interrupt it, since none of this yields to the event loop. The
    // cap is generous enough that no honest macro reaches it and small enough
    // that a runaway one stops in front of the user rather than hanging.
    constexpr int kMaxSteps = 10000;

    draining_ = true;
    int budget = kMaxSteps;
    while (!queue_.empty()) {
        if (budget-- <= 0) {
            push(HistoryEntry::Kind::Error,
                 "Macro stopped after " + std::to_string(kMaxSteps) +
                     " steps; it may be running itself.");
            queue_.clear();
            break;
        }

        const MacroStep step = queue_.front();
        queue_.erase(queue_.begin());

        if (active_ == nullptr) {
            if (step.kind == MacroStep::Kind::Enter) {
                // Enter at the bare prompt repeats the last command, which is
                // never what a trailing step in a macro meant.
                continue;
            }
            if (step.kind == MacroStep::Kind::Pause) {
                push(HistoryEntry::Kind::Warning,
                     "A pause with no command running was skipped.");
                continue;
            }
            outcome = executeOnce(step.text);
            if (outcome.status == CommandOutcome::Status::Unknown ||
                outcome.status == CommandOutcome::Status::Unavailable ||
                outcome.status == CommandOutcome::Status::Failed) {
                queue_.clear();
                break;
            }
            continue;
        }

        if (step.kind == MacroStep::Kind::Pause) {
            // The user's turn. What they supply satisfies this prompt, and the
            // rest of the queue plays on from their answer.
            break;
        }

        if (step.kind == MacroStep::Kind::Enter) {
            outcome = supplyOnce(InputValue::makeEmpty());
        } else {
            const InputRequest* request = currentRequest();
            const std::optional<InputValue> value =
                request == nullptr
                    ? std::nullopt
                    : parseTypedInput(*request, step.text, selection_.selection());
            if (!value) {
                push(HistoryEntry::Kind::Error,
                     "Macro stopped at line " + std::to_string(step.line) + ": \"" + step.text +
                         "\" is not what the command is asking for.");
                queue_.clear();
                cancel("Macro step rejected");
                outcome = {CommandOutcome::Status::Failed, "Macro step rejected"};
                break;
            }
            outcome = supplyOnce(*value);
        }

        if (outcome.status == CommandOutcome::Status::Failed ||
            outcome.status == CommandOutcome::Status::Cancelled) {
            queue_.clear();
            break;
        }
    }
    draining_ = false;
    notify();
    return outcome;
}

void CommandExecutor::cancel(const std::string& reason) {
    if (active_ == nullptr) {
        return;
    }
    finish({CommandOutcome::Status::Cancelled, reason}, false);
    push(HistoryEntry::Kind::Info, "*Cancel*");
}

CommandOutcome CommandExecutor::repeatLast() {
    if (lastCommand_ == nullptr || !lastCommand_->repeatable) {
        return {CommandOutcome::Status::Unavailable, "No repeatable command to repeat"};
    }
    return run(*lastCommand_);
}

void CommandExecutor::moveCursor(const Vec3& world) {
    if (active_ == nullptr) {
        return;
    }
    active_->context->setCursor(world);
    preview_ = active_->instance->preview(world, *active_->context);
    notify();
}

void CommandExecutor::logInput(const InputValue& input) {
    switch (input.kind) {
        case InputValue::Kind::Point: {
            std::ostringstream out;
            out << input.point.x << ',' << input.point.y;
            if (input.point.z != 0.0) {
                out << ',' << input.point.z;
            }
            push(HistoryEntry::Kind::Input, out.str());
            break;
        }
        case InputValue::Kind::Number:
            push(HistoryEntry::Kind::Input, std::to_string(input.number));
            break;
        case InputValue::Kind::Text:
        case InputValue::Kind::Keyword:
            push(HistoryEntry::Kind::Input, input.text);
            break;
        case InputValue::Kind::Selection:
            push(HistoryEntry::Kind::Input,
                 std::to_string(input.selection.size()) + " selected");
            break;
        case InputValue::Kind::Entity:
            push(HistoryEntry::Kind::Input, "entity picked");
            break;
        case InputValue::Kind::Empty:
            push(HistoryEntry::Kind::Input, {});
            break;
        case InputValue::Kind::Cancel:
            break;
    }
}

void CommandExecutor::push(HistoryEntry::Kind kind, const std::string& text) {
    history_.push_back({kind, text});
    while (history_.size() > maxHistory) {
        history_.erase(history_.begin());
    }
    notify();
}

std::size_t CommandExecutor::addListener(std::function<void()> listener) {
    const std::size_t id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void CommandExecutor::removeListener(std::size_t id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void CommandExecutor::notify() {
    const auto snapshot = listeners_;
    for (const auto& [id, listener] : snapshot) {
        listener();
    }
}

}  // namespace cad
