#include "cad/db/Transaction.hpp"

#include <algorithm>
#include <stdexcept>

namespace cad {
namespace {

/// Reverse a list of changes, newest first.
///
/// Order matters: a transaction that created an object and then modified it
/// must undo the modification before removing the object.
void undoChanges(Database& db, const std::vector<ObjectChange>& changes) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        const ObjectChange& change = *it;
        switch (change.kind) {
            case ObjectChange::Kind::Added:
                // Erase rather than delete: a redo needs the object back, and
                // other objects may still reference it.
                db.eraseObject(change.handle);
                break;
            case ObjectChange::Kind::Modified:
                if (DbObject* object = db.getObject(change.handle);
                    object != nullptr && change.before != nullptr) {
                    object->restoreFrom(*change.before);
                    db.notifyModified(change.handle);
                }
                break;
            case ObjectChange::Kind::Erased:
                db.uneraseObject(change.handle, change.index);
                break;
        }
    }
}

}  // namespace

// ------------------------------------------------------------- Transaction

void Transaction::assertOpen() const {
    if (state_ != State::Open) {
        throw std::logic_error("Transaction \"" + name_ + "\" is already " +
                               (state_ == State::Committed ? "committed" : "aborted"));
    }
}

void Transaction::recordModification(DbObject& object) {
    assertOpen();
    if (snapshotted_.contains(object.handle)) {
        return;
    }
    snapshotted_.insert(object.handle);

    ObjectChange change;
    change.kind = ObjectChange::Kind::Modified;
    change.handle = object.handle;
    change.ownerHandle = object.ownerHandle;
    change.before = object.clone();
    changes_.push_back(std::move(change));
}

Entity& Transaction::addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner) {
    assertOpen();
    // The owner's entity list changes, so it needs a snapshot too.
    recordModification(owner);

    Entity& reference = db_.addEntity(std::move(entity), owner);

    ObjectChange change;
    change.kind = ObjectChange::Kind::Added;
    change.handle = reference.handle;
    change.ownerHandle = owner.handle;
    changes_.push_back(std::move(change));
    return reference;
}

Entity& Transaction::addToModelSpace(std::unique_ptr<Entity> entity) {
    return addEntity(std::move(entity), db_.modelSpace());
}

Entity& Transaction::addToCurrentSpace(std::unique_ptr<Entity> entity) {
    return addEntity(std::move(entity), db_.currentSpace());
}

DbObject& Transaction::addObject(DbObjectPtr object) {
    assertOpen();
    DbObject& reference = db_.addObject(std::move(object));

    ObjectChange change;
    change.kind = ObjectChange::Kind::Added;
    change.handle = reference.handle;
    change.ownerHandle = reference.ownerHandle;
    changes_.push_back(std::move(change));
    return reference;
}

bool Transaction::erase(DbObject& object) {
    assertOpen();
    if (object.erased) {
        return false;
    }

    std::size_t index = 0;
    if (auto* owner = db_.get<BlockTableRecord>(object.ownerHandle)) {
        const auto& handles = owner->entityHandles;
        const auto it = std::find(handles.begin(), handles.end(), object.handle);
        index = it == handles.end() ? 0 : static_cast<std::size_t>(it - handles.begin());
        recordModification(*owner);
    }

    const Handle handle = object.handle;
    const Handle ownerHandle = object.ownerHandle;
    if (!db_.eraseObject(handle)) {
        return false;
    }

    ObjectChange change;
    change.kind = ObjectChange::Kind::Erased;
    change.handle = handle;
    change.ownerHandle = ownerHandle;
    change.index = index;
    changes_.push_back(std::move(change));
    return true;
}

UndoRecord Transaction::commit() {
    assertOpen();
    state_ = State::Committed;
    return UndoRecord{name_, std::move(changes_)};
}

void Transaction::abort() {
    if (state_ != State::Open) {
        return;
    }
    state_ = State::Aborted;
    undoChanges(db_, changes_);
    changes_.clear();
}

// ------------------------------------------------------ TransactionManager

std::string TransactionManager::undoName() const {
    return undoStack_.empty() ? std::string{} : undoStack_.back().name;
}

std::string TransactionManager::redoName() const {
    return redoStack_.empty() ? std::string{} : redoStack_.back().name;
}

std::vector<std::string> TransactionManager::history() const {
    std::vector<std::string> names;
    names.reserve(undoStack_.size());
    for (const UndoRecord& record : undoStack_) {
        names.push_back(record.name);
    }
    return names;
}

Transaction& TransactionManager::begin(const std::string& name) {
    if (active_ != nullptr) {
        throw std::logic_error("Cannot begin transaction \"" + name + "\": \"" +
                               active_->name() + "\" is still open");
    }
    active_ = std::make_unique<Transaction>(db_, name);
    return *active_;
}

bool TransactionManager::commit(Transaction& transaction) {
    if (active_.get() != &transaction) {
        throw std::logic_error("Attempted to commit a transaction that is not the open one");
    }
    UndoRecord record = transaction.commit();
    active_.reset();

    if (record.isEmpty()) {
        notify();
        return false;
    }
    push(std::move(record));
    return true;
}

void TransactionManager::abort(Transaction& transaction) {
    transaction.abort();
    if (active_.get() == &transaction) {
        active_.reset();
    }
    notify();
}

void TransactionManager::push(UndoRecord record) {
    undoStack_.push_back(std::move(record));
    // Any new edit invalidates the redo branch, matching every editor's
    // behaviour and avoiding an incoherent history tree.
    redoStack_.clear();
    while (undoStack_.size() > maxUndoDepth) {
        undoStack_.erase(undoStack_.begin());
    }
    notify();
}

std::size_t TransactionManager::indexOf(Handle ownerHandle, Handle handle) const {
    const auto* owner = db_.get<BlockTableRecord>(ownerHandle);
    if (owner == nullptr) {
        return 0;
    }
    const auto& handles = owner->entityHandles;
    const auto it = std::find(handles.begin(), handles.end(), handle);
    return it == handles.end() ? 0 : static_cast<std::size_t>(it - handles.begin());
}

UndoRecord TransactionManager::captureInverse(const UndoRecord& record) {
    std::vector<ObjectChange> inverse;
    inverse.reserve(record.changes.size());

    for (const ObjectChange& change : record.changes) {
        ObjectChange flipped;
        flipped.handle = change.handle;
        flipped.ownerHandle = change.ownerHandle;

        switch (change.kind) {
            case ObjectChange::Kind::Modified: {
                // Snapshot the *current* state before it is overwritten, so the
                // reverse operation can restore it.
                const DbObject* object = db_.getObject(change.handle);
                if (object == nullptr) {
                    continue;
                }
                flipped.kind = ObjectChange::Kind::Modified;
                flipped.before = object->clone();
                break;
            }
            case ObjectChange::Kind::Added:
                // Undoing an add erases it; the inverse must re-add it.
                flipped.kind = ObjectChange::Kind::Erased;
                flipped.index = indexOf(change.ownerHandle, change.handle);
                break;
            case ObjectChange::Kind::Erased:
                flipped.kind = ObjectChange::Kind::Added;
                break;
        }
        inverse.push_back(std::move(flipped));
    }
    return UndoRecord{record.name, std::move(inverse)};
}

bool TransactionManager::undo() {
    if (active_ != nullptr) {
        throw std::logic_error("Cannot undo while a transaction is open");
    }
    if (undoStack_.empty()) {
        return false;
    }

    UndoRecord record = std::move(undoStack_.back());
    undoStack_.pop_back();

    UndoRecord inverted = captureInverse(record);
    undoChanges(db_, record.changes);
    redoStack_.push_back(std::move(inverted));
    notify();
    return true;
}

bool TransactionManager::redo() {
    if (active_ != nullptr) {
        throw std::logic_error("Cannot redo while a transaction is open");
    }
    if (redoStack_.empty()) {
        return false;
    }

    UndoRecord record = std::move(redoStack_.back());
    redoStack_.pop_back();

    UndoRecord inverted = captureInverse(record);
    undoChanges(db_, record.changes);
    undoStack_.push_back(std::move(inverted));
    notify();
    return true;
}

void TransactionManager::clear() {
    undoStack_.clear();
    redoStack_.clear();
    notify();
}

std::size_t TransactionManager::addListener(std::function<void()> listener) {
    const std::size_t id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void TransactionManager::removeListener(std::size_t id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void TransactionManager::notify() {
    const auto snapshot = listeners_;
    for (const auto& [id, listener] : snapshot) {
        listener();
    }
}

}  // namespace cad
