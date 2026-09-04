#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cad/db/DbObject.hpp"

namespace cad {

/// Reading a DXF DICTIONARY that was kept verbatim.
///
/// Dictionaries are not modelled as their own type: almost every one in a real
/// drawing belongs to a feature this application does not implement, and reading
/// them into a structure would mean deciding how to write them back, which is
/// how preserved data gets damaged. They are kept as raw groups instead.
///
/// The entries are *positional*: a code 3 names an entry, and the 350 or 360
/// after it is the object it names. Nothing else pairs them, which is why the
/// preserved groups have to stay in file order.

/// A dictionary's entries in file order.
///
/// Empty for an object that is not a dictionary, or one with no entries — the
/// caller cannot usefully tell those apart and does not need to.
[[nodiscard]] std::vector<std::pair<std::string, Handle>> dictionaryEntries(
    const DbObject& dictionary);

/// The handle a dictionary gives a name, or a null handle. Case-insensitive,
/// as DXF names are.
[[nodiscard]] Handle dictionaryEntry(const DbObject& dictionary, std::string_view name);

}  // namespace cad
