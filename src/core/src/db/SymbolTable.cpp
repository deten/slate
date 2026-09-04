#include "cad/db/SymbolTable.hpp"

#include <cctype>
#include <stdexcept>

namespace cad {

std::string normalizeSymbolName(std::string_view name) {
    // Trim, then uppercase. other CAD software treats symbol names case-insensitively and
    // ignores surrounding whitespace, so both must be normalized or a drawing
    // could end up with two layers that the user cannot tell apart.
    std::size_t begin = 0;
    std::size_t end = name.size();
    const auto isSpace = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (begin < end && isSpace(name[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(name[end - 1])) {
        --end;
    }

    std::string result;
    result.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        result.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(name[i]))));
    }
    return result;
}

Handle SymbolTableIndex::find(std::string_view name) const {
    const auto it = byName_.find(normalizeSymbolName(name));
    return it == byName_.end() ? Handle::null() : it->second;
}

void SymbolTableIndex::add(std::string_view name, Handle handle) {
    const std::string key = normalizeSymbolName(name);
    const auto existing = byName_.find(key);

    if (existing != byName_.end()) {
        if (existing->second != handle) {
            throw std::invalid_argument(tableName_ + " already contains a record named '" +
                                        std::string{name} + "'");
        }
        return;
    }
    byName_.emplace(key, handle);
    order_.push_back(handle);
}

bool SymbolTableIndex::remove(std::string_view name) {
    const std::string key = normalizeSymbolName(name);
    const auto it = byName_.find(key);
    if (it == byName_.end()) {
        return false;
    }
    const Handle handle = it->second;
    byName_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), handle), order_.end());
    return true;
}

bool SymbolTableIndex::rename(std::string_view oldName, std::string_view newName) {
    const std::string oldKey = normalizeSymbolName(oldName);
    const std::string newKey = normalizeSymbolName(newName);

    const auto it = byName_.find(oldKey);
    if (it == byName_.end()) {
        return false;
    }
    // A rename that only changes casing is allowed; one that collides is not.
    if (oldKey != newKey && byName_.contains(newKey)) {
        return false;
    }

    const Handle handle = it->second;
    byName_.erase(it);
    byName_[newKey] = handle;
    return true;
}

std::string SymbolTableIndex::uniqueName(std::string_view base) const {
    if (!has(base)) {
        return std::string{base};
    }
    for (int i = 1;; ++i) {
        std::string candidate = std::string{base} + std::to_string(i);
        if (!has(candidate)) {
            return candidate;
        }
    }
}

void SymbolTableIndex::clear() {
    byName_.clear();
    order_.clear();
}

}  // namespace cad
