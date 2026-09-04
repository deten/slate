#pragma once

#include <vector>

#include "cad/geom/Curve.hpp"

namespace cad {

class Database;
class InsertEntity;

/// The world-space curves a block reference actually draws.
///
/// Nested references are expanded, and every cell of an array reference is
/// included, so what comes back is what is on the screen.
///
/// Needed because an `InsertEntity` knows only its own extents: the geometry
/// belongs to the block record, which lives in the database and is out of reach
/// from the entity itself. Anything that has to answer "where is this block,
/// really" rather than "roughly where is it" has to come through here --
/// picking it, and outlining it when it is selected.
///
/// `maxDepth` stops a block that contains itself, which a damaged file can
/// describe, from recursing forever.
[[nodiscard]] std::vector<CurvePtr> blockReferenceCurves(const Database& db,
                                                         const InsertEntity& insert,
                                                         int maxDepth = 8);

}  // namespace cad
