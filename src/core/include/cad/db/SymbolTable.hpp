#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cad/db/DbObject.hpp"

namespace cad {

/// Base class for named records in a symbol table — layers, linetypes, text
/// styles, dimension styles, block definitions, views and UCSs.
class SymbolTableRecord : public DbObject {
public:
    /// Record name. Symbol names are case-insensitive for lookup, but their
    /// original casing is preserved for display and for writing back out.
    std::string name;

    /// True for records the application depends on and that must not be
    /// deleted — layer "0", linetype "Continuous", style "Standard", and the
    /// model and paper space block records.
    [[nodiscard]] virtual bool isReserved() const { return false; }

protected:
    SymbolTableRecord() = default;
    SymbolTableRecord(const SymbolTableRecord&) = default;
    SymbolTableRecord& operator=(const SymbolTableRecord&) = default;
    SymbolTableRecord(SymbolTableRecord&&) = default;
    SymbolTableRecord& operator=(SymbolTableRecord&&) = default;

    void copyRecordInto(SymbolTableRecord& target) const {
        copyBaseInto(target);
        target.name = name;
    }

    void restoreRecordFrom(const SymbolTableRecord& snapshot) {
        restoreBaseFrom(snapshot);
        name = snapshot.name;
    }
};

/// Canonical form used for case-insensitive symbol name comparison.
[[nodiscard]] std::string normalizeSymbolName(std::string_view name);

/// A name-keyed index over symbol table records.
///
/// Lookup is case-insensitive, matching other CAD software: a drawing cannot contain both
/// "Walls" and "WALLS". The table keeps a normalized-name index alongside the
/// insertion order, so the very common "find layer by name" path is O(1)
/// without forcing callers to normalize.
///
/// The table stores *handles*, not objects. Objects live in the database's flat
/// store, which is what keeps them valid across undo when they are replaced by
/// restored clones.
class SymbolTableIndex {
public:
    explicit SymbolTableIndex(std::string tableName) : tableName_(std::move(tableName)) {}

    [[nodiscard]] const std::string& tableName() const noexcept { return tableName_; }

    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }

    [[nodiscard]] bool has(std::string_view name) const {
        return byName_.contains(normalizeSymbolName(name));
    }

    /// Handle for a name, or the null handle when absent.
    [[nodiscard]] Handle find(std::string_view name) const;

    /// Register a record's handle under its name.
    /// @throws std::invalid_argument when the name is taken by another record.
    void add(std::string_view name, Handle handle);

    /// Remove a name from the index. Does not erase the object itself; the
    /// database decides whether it stays for undo.
    bool remove(std::string_view name);

    /// Rename, keeping the index consistent.
    /// @returns false when the new name is already in use.
    bool rename(std::string_view oldName, std::string_view newName);

    /// Handles in insertion order, which is the order records are written out.
    [[nodiscard]] const std::vector<Handle>& handles() const noexcept { return order_; }

    /// Generate a name not already present, e.g. `Layer1`, `Layer2`.
    /// Used when importing a drawing whose names collide with the host's.
    [[nodiscard]] std::string uniqueName(std::string_view base) const;

    void clear();

private:
    std::string tableName_;
    /// Normalized name -> handle.
    std::unordered_map<std::string, Handle> byName_;
    /// Insertion order.
    std::vector<Handle> order_;
};

}  // namespace cad
