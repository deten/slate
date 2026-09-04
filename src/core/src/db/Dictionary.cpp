#include "cad/db/Dictionary.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace cad {
namespace {

[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::vector<std::pair<std::string, Handle>> dictionaryEntries(const DbObject& dictionary) {
    std::vector<std::pair<std::string, Handle>> entries;
    if (dictionary.preserved == nullptr) {
        return entries;
    }

    std::optional<std::string> pending;
    for (const UnknownGroup& group : dictionary.preserved->unknownCodes) {
        if (group.code == 3) {
            if (const auto* name = std::get_if<std::string>(&group.value)) {
                pending = *name;
            }
            continue;
        }
        // 350 is a hard-owned entry and 360 a soft one. The distinction is
        // about deletion, which the file owns; both name an object.
        if (group.code != 350 && group.code != 360) {
            continue;
        }
        if (!pending) {
            continue;  // A handle with no name before it: nothing to key on.
        }
        Handle handle;
        if (const auto* stored = std::get_if<Handle>(&group.value)) {
            handle = *stored;
        } else if (const auto* text = std::get_if<std::string>(&group.value)) {
            handle = Handle::tryParse(*text).value_or(Handle{});
        }
        if (!handle.isNull()) {
            entries.emplace_back(*pending, handle);
        }
        pending.reset();
    }
    return entries;
}

Handle dictionaryEntry(const DbObject& dictionary, std::string_view name) {
    for (const auto& [key, handle] : dictionaryEntries(dictionary)) {
        if (equalsIgnoringCase(key, name)) {
            return handle;
        }
    }
    return Handle{};
}

}  // namespace cad
