#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cad/db/Handle.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

class DbObject;
using DbObjectPtr = std::unique_ptr<DbObject>;

/// A single extended-data (XData) value.
///
/// XData is how third-party applications and vertical products (structural,
/// MEP, survey) attach their own information to entities. It is opaque to this
/// application, but discarding it silently corrupts other people's data — so it
/// is carried through load, edit and save untouched.
using XDataValue = std::variant<std::string, double, std::int64_t, Handle>;

struct XDataItem {
    /// DXF group code, in the 1000-1071 XData range.
    int code = 0;
    XDataValue value;
};

/// A group code read from a file that no property mapped to.
struct UnknownGroup {
    int code = 0;
    XDataValue value;
};

/// Data read from a file that this application does not yet interpret.
///
/// Rather than dropping unknown group codes, the reader parks them here and the
/// writer emits them again. This is what makes it safe to open a professional
/// drawing containing features that are not implemented, edit an unrelated part
/// of it, and save without destroying anything.
struct PreservedData {
    std::vector<UnknownGroup> unknownCodes;

    /// The `102 {APPNAME ... 102 }` groups, which belong in the *header* of a
    /// record -- immediately after its handle, before its owner and subclass
    /// markers.
    ///
    /// Separate from `unknownCodes` because position is the whole point. Held
    /// with the rest and written at the end, a `{ACAD_XDICTIONARY}` group is
    /// ignored by readers that expect it where DXF puts it, and an extension
    /// dictionary nothing appears to reference is discarded along with
    /// everything it owns. That cost this drawing 223 dictionaries and the 157
    /// fields, records and sort tables under them, while every count in the
    /// file still matched.
    std::vector<UnknownGroup> applicationGroups;

    /// A second object stored inside this one, introduced by group `101
    /// Embedded Object` — an annotative MTEXT's scaling data, most commonly.
    ///
    /// Held separately from `unknownCodes` and in file order, because its codes
    /// restart from scratch and repeat: merged into the unknown codes they
    /// would be grouped and reordered by code, which for a positional format
    /// destroys them.
    std::vector<UnknownGroup> embeddedObject;

    /// The object's original DXF/DWG class name, for round-tripping unknown
    /// types.
    std::string originalType;

    /// Rewrite a point held as a `xCode`, `xCode+10`, `xCode+20` triple.
    ///
    /// For the objects that are *read for their meaning and written back for
    /// their bytes* -- a table, and anything else whose group codes repeat too
    /// ambiguously to claim. Those bytes are what a save actually emits, so
    /// moving such an object without moving them writes it back where it used
    /// to be: the drawing on screen and the drawing in the file disagree, and
    /// the file wins.
    ///
    /// @returns false when the triple is not there, so a caller can tell
    ///   "updated" from "silently did nothing".
    bool rewritePoint(int xCode, const Vec3& value);

    /// The same for a single number, e.g. a row height that a scale changed.
    bool rewriteNumber(int code, double value);
};

/// A whole section or symbol table kept verbatim, because nothing here
/// interprets it.
///
/// `PreservedData` protects an object's unread group codes; this protects an
/// entire structure the reader walked past. A DXF file is a sequence of
/// sections and a save rewrites all of them, so a section that is not written
/// is a section that is deleted — silently, from a file that opened without
/// complaint.
struct PreservedSection {
    /// Section name (`CLASSES`, `ACDSDATA`) or symbol table name (`VPORT`).
    std::string name;

    /// True when this is a table inside the TABLES section rather than a
    /// section of its own, which is what decides where the writer puts it back.
    bool isTable = false;

    /// Everything between the name and its ENDSEC or ENDTAB, in file order,
    /// including the code-0 record markers.
    std::vector<UnknownGroup> groups;
};

/// Base class for everything stored in the drawing database — entities, symbol
/// table records, dictionaries, layouts, and the objects yet to be implemented.
///
/// Objects are *mutable*. This is a considered choice: a real drawing holds
/// hundreds of thousands of objects, and an immutable graph would make every
/// edit allocate proportionally to the drawing rather than to the change.
/// Undo correctness is instead provided by the transaction layer, which clones
/// an object's state the first time it is opened for write within a
/// transaction. That is the model other CAD software itself uses.
class DbObject {
public:
    DbObject() = default;
    virtual ~DbObject() = default;

    /// Permanent identity. Assigned when the object is added to a database.
    Handle handle;

    /// The object that owns this one — a block record for an entity, the block
    /// table for a block record, and so on. Ownership is a tree and drives
    /// deletion; other references are non-owning.
    Handle ownerHandle;

    /// Handle of this object's extension dictionary, if it has one. Extension
    /// dictionaries hold arbitrary named object references and are how features
    /// such as multileader styles and layer states attach to objects.
    Handle extensionDictionaryHandle;

    /// The field driving this object's text, when it has one.
    ///
    /// Derived rather than stored: the file expresses it as a chain of
    /// dictionaries hanging off the extension dictionary, which is preserved
    /// verbatim and is where a save writes it back from. This is the resolved
    /// answer, so that asking "is this text a field" costs nothing.
    Handle fieldHandle;

    /// Handles of persistent reactors — objects notified when this one changes.
    /// Preserved for fidelity; the notification mechanism is not implemented.
    std::vector<Handle> reactorHandles;

    /// Application-registered extended data, keyed by application name.
    std::unordered_map<std::string, std::vector<XDataItem>> xdata;

    /// Group codes read from file that no property claimed.
    std::unique_ptr<PreservedData> preserved;

    /// Erased objects remain in the database so undo can restore them and so
    /// dangling references resolve to something rather than nothing. They are
    /// skipped by iteration, rendering and selection.
    bool erased = false;

    /// Discriminator used for serialization and diagnostics.
    [[nodiscard]] virtual std::string objectType() const = 0;

    /// Deep-copy this object's mutable state.
    ///
    /// Used by the transaction layer for undo snapshots and by COPY. Subclasses
    /// must override, call `copyBaseInto`, and copy their own fields; anything
    /// missed silently breaks undo, so each type has a round-trip test covering
    /// exactly this.
    [[nodiscard]] virtual DbObjectPtr clone() const = 0;

    /// Restore this object's state from a snapshot taken by `clone`.
    ///
    /// The handle is intentionally left alone, so the object keeps its identity
    /// across an undo.
    /// @throws std::invalid_argument when the snapshot is the wrong type.
    virtual void restoreFrom(const DbObject& snapshot) = 0;

    /// Handles this object references but does not own.
    ///
    /// Used to validate a loaded database, to detect what an operation would
    /// break, and to remap references when objects are cloned between databases
    /// — the mechanism behind block insertion and cross-drawing copy/paste.
    [[nodiscard]] virtual std::vector<Handle> referencedHandles() const;

    [[nodiscard]] const std::vector<XDataItem>* getXData(const std::string& appName) const;

    void setXData(const std::string& appName, std::vector<XDataItem> items);

protected:
    DbObject(const DbObject&) = default;
    DbObject& operator=(const DbObject&) = default;
    DbObject(DbObject&&) = default;
    DbObject& operator=(DbObject&&) = default;

    /// Copy the base-class state into `target`.
    ///
    /// Deliberately does not copy the handle: a clone used for undo restores
    /// onto the same handle, while a clone used for COPY must receive a fresh
    /// one from the database.
    void copyBaseInto(DbObject& target) const;

    void restoreBaseFrom(const DbObject& snapshot);
};

}  // namespace cad
