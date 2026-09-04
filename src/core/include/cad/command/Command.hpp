#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// One selectable option within a prompt, e.g. the "Center" in ARC.
struct CommandKeyword {
    /// Full word, e.g. `Center`.
    std::string name;
    /// Shortest unambiguous prefix, conventionally the capitalized letters.
    /// Typing this is equivalent to typing the whole word.
    std::string shortcut;
    std::string description;
};

/// What a command is currently asking the user for.
///
/// A CAD command is a state machine that alternates between prompting and
/// receiving input, and must be interruptible at every step. Modelling the
/// request as *data* — rather than as a blocking call inside imperative command
/// code — is what makes cancellation, undo, scripting, macro playback and
/// command preview all work through one mechanism instead of four.
struct InputRequest {
    enum class Kind : std::uint8_t {
        Point,
        Distance,
        Angle,
        Number,
        Text,
        Selection,
        Entity,
        Keyword,
    };

    Kind kind = Kind::Point;
    std::string prompt;

    /// Rubber-band origin; the UI draws from here to the cursor.
    std::optional<Vec3> basePoint;
    /// Keyword alternatives the user may type instead of picking.
    std::vector<CommandKeyword> keywords;
    /// Accepted when the user presses Enter with no input.
    std::optional<double> defaultNumber;
    std::optional<std::string> defaultText;

    /// Seed the editor with this value, selected, ready to be amended.
    ///
    /// Distinct from `defaultText`, which is merely *accepted* when the user
    /// presses Enter on an empty line. Editing needs the existing value in the
    /// box: retyping a paragraph to change one word in it is not editing, and a
    /// prompt that shows the old text and makes you type it again is the reason
    /// people edit annotation in a properties palette instead.
    std::optional<std::string> editText;

    /// The object whose value `editText` came from.
    ///
    /// Told, not guessed. A shell that can edit a label *where it sits* has to
    /// know which label, and inferring it from the selection would be a second
    /// source of truth that goes wrong the first time a command edits something
    /// it did not select.
    Handle editEntity;
    /// Allow the user to finish here rather than requiring input.
    bool allowEmpty = false;
    /// Bounds for numeric input.
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool integerOnly = false;
    /// Selection sizing.
    std::size_t minCount = 0;
    std::size_t maxCount = 0;
};

/// A value supplied in response to an InputRequest.
struct InputValue {
    enum class Kind : std::uint8_t {
        Point,
        Number,
        Text,
        Selection,
        Entity,
        Keyword,
        /// The user pressed Enter with no input.
        Empty,
        /// The user pressed Escape.
        Cancel,
    };

    Kind kind = Kind::Empty;
    Vec3 point;
    double number = 0.0;
    std::string text;
    std::vector<Handle> selection;
    Handle entity;

    [[nodiscard]] static InputValue makePoint(const Vec3& p) {
        InputValue v;
        v.kind = Kind::Point;
        v.point = p;
        return v;
    }
    [[nodiscard]] static InputValue makeNumber(double n) {
        InputValue v;
        v.kind = Kind::Number;
        v.number = n;
        return v;
    }
    [[nodiscard]] static InputValue makeText(std::string s) {
        InputValue v;
        v.kind = Kind::Text;
        v.text = std::move(s);
        return v;
    }
    [[nodiscard]] static InputValue makeKeyword(std::string s) {
        InputValue v;
        v.kind = Kind::Keyword;
        v.text = std::move(s);
        return v;
    }
    [[nodiscard]] static InputValue makeSelection(std::vector<Handle> handles) {
        InputValue v;
        v.kind = Kind::Selection;
        v.selection = std::move(handles);
        return v;
    }
    [[nodiscard]] static InputValue makeEntity(Handle handle) {
        InputValue v;
        v.kind = Kind::Entity;
        v.entity = handle;
        return v;
    }
    [[nodiscard]] static InputValue makeEmpty() {
        InputValue v;
        v.kind = Kind::Empty;
        return v;
    }
    [[nodiscard]] static InputValue makeCancel() {
        InputValue v;
        v.kind = Kind::Cancel;
        return v;
    }
};

/// What a command step returns: either another request, or completion.
struct StepResult {
    enum class Kind : std::uint8_t { Request, Done, Cancelled, Failed };

    Kind kind = Kind::Done;
    InputRequest request;
    std::string message;

    [[nodiscard]] static StepResult ask(InputRequest request) {
        StepResult r;
        r.kind = Kind::Request;
        r.request = std::move(request);
        return r;
    }
    [[nodiscard]] static StepResult done(std::string message = {}) {
        StepResult r;
        r.kind = Kind::Done;
        r.message = std::move(message);
        return r;
    }
    [[nodiscard]] static StepResult cancelled(std::string message = {}) {
        StepResult r;
        r.kind = Kind::Cancelled;
        r.message = std::move(message);
        return r;
    }
    [[nodiscard]] static StepResult failed(std::string message) {
        StepResult r;
        r.kind = Kind::Failed;
        r.message = std::move(message);
        return r;
    }
};

class CommandExecutor;
struct MacroStep;

/// Services a running command may use.
///
/// Deliberately narrow. A command gets the database, a transaction, the current
/// selection and a way to talk to the user — and nothing else. It cannot reach
/// the renderer or the UI, which keeps commands scriptable and testable
/// headlessly, and is what will let the same command objects run from the
/// plugin API.
class CommandContext {
public:
    CommandContext(Database& db, TransactionManager& transactions, Transaction* transaction,
                   std::vector<Handle> initialSelection, CommandExecutor& executor)
        : db_(db),
          transactions_(transactions),
          transaction_(transaction),
          initialSelection_(std::move(initialSelection)),
          executor_(executor) {}

    [[nodiscard]] Database& db() const noexcept { return db_; }
    [[nodiscard]] TransactionManager& transactions() const noexcept { return transactions_; }

    /// The transaction this command's changes belong to.
    /// @throws std::logic_error for a command registered as read-only.
    [[nodiscard]] Transaction& transaction() const;

    /// @internal Nullable form, for the executor's own abort handling.
    [[nodiscard]] Transaction* transactionOrNull() const noexcept { return transaction_; }

    /// Entities selected before the command started.
    [[nodiscard]] const std::vector<Handle>& initialSelection() const noexcept {
        return initialSelection_;
    }

    /// Write a line to the command history.
    void log(const std::string& message) const;

    /// Report a problem without failing the command.
    void warn(const std::string& message) const;

    /// Queue steps to be played once this command finishes.
    ///
    /// How a macro runs its body. A command cannot run another command --
    /// that would nest the executor inside itself -- so it hands the steps back
    /// to the loop it is already inside, which feeds them exactly as it feeds a
    /// person's keystrokes.
    void enqueue(std::vector<MacroStep> steps) const;

    /// Current cursor position in world coordinates, when known.
    [[nodiscard]] const Vec3* cursorWorld() const noexcept {
        return hasCursor_ ? &cursor_ : nullptr;
    }

    /// @internal
    void setCursor(const Vec3& world) {
        cursor_ = world;
        hasCursor_ = true;
    }

private:
    Database& db_;
    TransactionManager& transactions_;
    Transaction* transaction_;
    std::vector<Handle> initialSelection_;
    CommandExecutor& executor_;
    Vec3 cursor_;
    bool hasCursor_ = false;
};

/// A command's executable behaviour.
///
/// An explicit state machine: `start` returns the first request, and `supply`
/// is called with each response and returns the next step. The instance holds
/// the command's own working state.
class CommandInstance {
public:
    CommandInstance() = default;
    virtual ~CommandInstance() = default;

    CommandInstance(const CommandInstance&) = delete;
    CommandInstance& operator=(const CommandInstance&) = delete;
    CommandInstance(CommandInstance&&) = delete;
    CommandInstance& operator=(CommandInstance&&) = delete;

    /// Begin. Returns the first prompt, or completes immediately.
    [[nodiscard]] virtual StepResult start(CommandContext& context) = 0;

    /// Handle one input and return what to do next.
    [[nodiscard]] virtual StepResult supply(const InputValue& input, CommandContext& context) = 0;

    /// Called when the command ends for any reason, including cancellation and
    /// failure. Cleanup only; the transaction is already handled.
    virtual void cleanup(CommandContext& /*context*/) {}

    /// Geometry to show under the cursor, rebuilt as it moves.
    ///
    /// Returned as entities the command *does not* add to the database: a
    /// rubber-band line is not part of the drawing until the user commits it,
    /// and putting it there would make it selectable, undoable and saveable,
    /// which it must not be.
    ///
    /// Called on every cursor move, so implementations must be cheap.
    [[nodiscard]] virtual std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& /*cursor*/, CommandContext& /*context*/) const {
        return {};
    }
};

using CommandInstancePtr = std::unique_ptr<CommandInstance>;

/// How a command interacts with the undo system.
enum class CommandTransactionMode : std::uint8_t {
    /// Wrapped in one transaction; the normal case.
    Single,
    /// Makes no database changes, e.g. ZOOM. Creates no undo entry.
    ReadOnly,
    /// Manages its own transactions, e.g. UNDO itself.
    Manual,
};

/// The registration record for a command.
///
/// Everything the application needs to expose a command — in the command line,
/// a menu, the palette, a keyboard shortcut, or a script — comes from here. A
/// command contributed by a plugin is indistinguishable from a built-in one,
/// which is the stated requirement.
struct CommandDefinition {
    /// Stable identifier, e.g. `cad.draw.line`. Never shown to the user.
    std::string id;
    /// Primary name typed at the command line, e.g. `LINE`.
    std::string name;
    /// Alternative names, e.g. `L` for LINE.
    std::vector<std::string> aliases;
    /// Short label for menus and the palette.
    std::string title;
    std::string description;
    /// Grouping for the palette, e.g. `Draw`, `Modify`, `View`.
    std::string category;
    std::string help;

    CommandTransactionMode transactionMode = CommandTransactionMode::Single;

    /// True when this command came from a plugin rather than from the
    /// application itself.
    ///
    /// The ribbon uses it to say where a command came from instead of filing it
    /// under "More", which tells a drafter nothing and quietly equates "we have
    /// not curated this yet" with "somebody else added this".
    bool fromPlugin = false;

    /// True when this command came from a macro file the user can edit.
    ///
    /// Distinct from `fromPlugin` for the reason that one is distinct from a
    /// built-in: "I wrote this", "somebody's add-in supplied this" and "this
    /// application has not curated this yet" are three different facts, and a
    /// ribbon that files all three under "More" tells the drafter none of them.
    bool fromMacro = false;

    /// True when the command can act on a selection made before it started.
    bool supportsPickfirst = false;
    /// True when repeating with Enter should re-run it. Nearly all drawing
    /// commands do; ones with side effects like SAVE should not.
    bool repeatable = true;

    /// Whether the command may run right now. Used to grey out menu items and
    /// to refuse gracefully rather than failing mid-way.
    std::function<bool(const Database&, const std::vector<Handle>&)> isAvailable;

    /// Construct a fresh instance. Called once per invocation.
    std::function<CommandInstancePtr()> create;
};

}  // namespace cad
