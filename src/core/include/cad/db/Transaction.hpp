#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "cad/db/Database.hpp"

namespace cad {

/// One reversible change to a single object.
///
/// Storing whole-object snapshots rather than field-level diffs is a deliberate
/// trade: snapshots cost more memory per change but make undo *provably*
/// correct for every object type, including ones added later by plugins, with
/// no per-type undo code to get wrong. Memory is bounded instead by capping the
/// undo stack depth.
struct ObjectChange {
    enum class Kind : std::uint8_t { Added, Modified, Erased };

    Kind kind = Kind::Modified;
    Handle handle;
    /// Owner at the time of the change, for restoring an erased object.
    Handle ownerHandle;
    /// Position in the owner's draw order, so redo restores it in place.
    std::size_t index = 0;
    /// State before the change; only set for Modified.
    DbObjectPtr before;
};

/// A completed, reversible unit of work.
///
/// One command produces exactly one of these however many objects it touched,
/// which is what makes a complex operation like FILLET undo as a single step
/// rather than a confusing sequence of partial states.
class UndoRecord {
public:
    UndoRecord(std::string operationName, std::vector<ObjectChange> operationChanges)
        : name(std::move(operationName)), changes(std::move(operationChanges)) {}

    std::string name;
    std::vector<ObjectChange> changes;

    [[nodiscard]] bool isEmpty() const noexcept { return changes.empty(); }
};

/// A scope in which database modifications are recorded so they can be undone.
///
/// Obtained from TransactionManager, which guarantees a transaction is either
/// committed in full or aborted with every change rolled back.
class Transaction {
public:
    Transaction(Database& db, std::string transactionName)
        : db_(db), name_(std::move(transactionName)) {}

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction() = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool isOpen() const noexcept { return state_ == State::Open; }
    [[nodiscard]] Database& database() noexcept { return db_; }

    /// Declare that an object is about to be modified.
    ///
    /// **Every** mutation of a database object must be preceded by this call.
    /// Forgetting it does not corrupt the database immediately — it silently
    /// makes that change un-undoable, which surfaces later as a confusing bug.
    /// The helpers below cover the common cases so most command code never
    /// calls this directly.
    template <typename T>
    T& openForWrite(T& object) {
        recordModification(object);
        return object;
    }

    /// Add a new entity to a block record, recording it for undo.
    Entity& addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner);

    Entity& addToModelSpace(std::unique_ptr<Entity> entity);

    /// Add to the current space -- model, a sheet, or a block being edited --
    /// which is where every drawing command puts what it makes.
    Entity& addToCurrentSpace(std::unique_ptr<Entity> entity);

    /// Add a non-entity object (a layer, style, layout) for undo.
    DbObject& addObject(DbObjectPtr object);

    /// Erase an object, recording enough to restore it in draw order.
    bool erase(DbObject& object);

    /// Apply a mutation to an entity and notify the database.
    ///
    /// The convenience path most commands use: it snapshots, mutates, and fires
    /// the change notification in the right order, so none of those can be
    /// forgotten independently.
    template <typename T, typename F>
    T& modifyEntity(T& entity, F&& mutate) {
        recordModification(entity);
        std::forward<F>(mutate)(entity);
        db_.notifyModified(entity.handle);
        return entity;
    }

    /// @internal Finalize and hand the recorded changes to the manager.
    [[nodiscard]] UndoRecord commit();

    /// Roll every change back, leaving the database exactly as it was.
    ///
    /// Called when a command is cancelled or throws. This is what makes
    /// "cancellation must never corrupt the drawing" true by construction
    /// rather than by each command remembering to clean up after itself.
    void abort();

private:
    enum class State : std::uint8_t { Open, Committed, Aborted };

    void assertOpen() const;
    void recordModification(DbObject& object);

    Database& db_;
    std::string name_;
    std::vector<ObjectChange> changes_;
    /// Handles already snapshotted. An object modified ten times still needs
    /// only its original state kept.
    std::unordered_set<Handle> snapshotted_;
    State state_ = State::Open;
};

/// Owns the undo and redo stacks and is the only supported way to modify the
/// database.
class TransactionManager {
public:
    explicit TransactionManager(Database& db) : db_(db) {}

    [[nodiscard]] bool hasActiveTransaction() const noexcept { return active_ != nullptr; }
    [[nodiscard]] bool canUndo() const noexcept { return !undoStack_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redoStack_.empty(); }

    /// Name of the operation that would be undone, for the menu item.
    [[nodiscard]] std::string undoName() const;
    [[nodiscard]] std::string redoName() const;

    [[nodiscard]] std::size_t undoDepth() const noexcept { return undoStack_.size(); }
    [[nodiscard]] std::size_t redoDepth() const noexcept { return redoStack_.size(); }

    /// Names of the operations on the undo stack, newest last.
    [[nodiscard]] std::vector<std::string> history() const;

    /// Open a transaction with an explicit lifetime.
    ///
    /// Needed because an interactive command's lifetime is *asynchronous*: LINE
    /// opens a transaction, then waits — possibly for many seconds — while the
    /// user picks points. A synchronous scope cannot express that, so the
    /// caller takes responsibility for calling commit or abort.
    ///
    /// Prefer `run` for synchronous work, which cannot leak a transaction.
    /// @throws std::logic_error when a transaction is already open.
    [[nodiscard]] Transaction& begin(const std::string& name);

    /// Commit an explicitly opened transaction, pushing it onto the undo stack.
    ///
    /// A transaction that changed nothing is discarded rather than recorded, so
    /// a cancelled or no-op command leaves no confusing empty history entry.
    /// @returns true when an undo entry was recorded.
    bool commit(Transaction& transaction);

    /// Roll back an explicitly opened transaction. Safe to call twice.
    void abort(Transaction& transaction);

    /// Run `fn` inside a transaction, committing on return and aborting if it
    /// throws, so a command that fails partway cannot leave the drawing in a
    /// half-edited state.
    ///
    /// A nested call joins the outer transaction rather than creating a second
    /// undo entry: a command that calls another command still undoes as one
    /// step.
    template <typename F>
    decltype(auto) run(const std::string& name, F&& fn) {
        if (active_ != nullptr) {
            return std::forward<F>(fn)(*active_);
        }
        Transaction& transaction = begin(name);
        try {
            if constexpr (std::is_void_v<decltype(fn(transaction))>) {
                std::forward<F>(fn)(transaction);
                commit(transaction);
                return;
            } else {
                decltype(auto) result = std::forward<F>(fn)(transaction);
                commit(transaction);
                return result;
            }
        } catch (...) {
            abort(transaction);
            throw;
        }
    }

    /// Undo the most recent operation.
    ///
    /// Before reversing, the current state of every modified object is captured
    /// so redo can restore it — which is why undo and redo are symmetric rather
    /// than redo needing its own recorded forward diff.
    /// @throws std::logic_error when a transaction is open.
    bool undo();

    bool redo();

    /// Discard all history, e.g. after opening a new file.
    void clear();

    /// Maximum undo depth. Bounded because whole-object snapshots would
    /// otherwise grow without limit over a long editing session.
    std::size_t maxUndoDepth = 200;

    /// Register a listener notified whenever the history changes.
    std::size_t addListener(std::function<void()> listener);
    void removeListener(std::size_t id);

private:
    void push(UndoRecord record);
    void notify();
    [[nodiscard]] UndoRecord captureInverse(const UndoRecord& record);
    [[nodiscard]] std::size_t indexOf(Handle ownerHandle, Handle handle) const;

    Database& db_;
    std::unique_ptr<Transaction> active_;
    std::vector<UndoRecord> undoStack_;
    std::vector<UndoRecord> redoStack_;
    std::vector<std::pair<std::size_t, std::function<void()>>> listeners_;
    std::size_t nextListenerId_ = 1;
};

}  // namespace cad
