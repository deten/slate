#pragma once

#include <vector>

#include "cad/db/Handle.hpp"

namespace cad {
class Database;
}

namespace cad::app {

/// Layer handles in the order a person reads a list of them: by name.
///
/// The table's own order is the order the layers were created, or the order
/// they happened to appear in the file, which on a drawing with sixty of them
/// means hunting. Every list of layers in the application uses this, so the
/// panel, the ribbon dropdown, the properties list and Quick Select all agree
/// -- a name found in one and looked for in another should be in the same
/// place.
///
/// Compared without case and with runs of digits read as numbers, so `PDF2`
/// sorts before `PDF10` rather than after it. Layer names in a real drawing are
/// numbered constantly -- `A-2.1`, `PDF3_Text` -- and plain string order puts
/// them in an order nobody expects.
///
/// Erased layers are left out; they are not layers any more.
[[nodiscard]] std::vector<Handle> layersByName(const Database& db);

}  // namespace cad::app
