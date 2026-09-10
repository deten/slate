#pragma once

#include "cad/db/Database.hpp"

namespace cad {

class InsertEntity;
class Transaction;

/// Keep a block reference's clip in the file's form.
///
/// A reference's clip lives on the entity as `clipBoundary` for drawing and
/// editing, and in the file as a SPATIAL_FILTER object under ACAD_FILTER in
/// the reference's extension dictionary, which is what every other reader
/// expects. This writes the one from the other: after a clip is set or
/// changed, the filter and the dictionaries it hangs from are made or
/// updated in the database, so saving writes them as they are, in DXF or
/// through the translator alike. The objects are ordinary preserved
/// objects, undone with the rest of the transaction.
void writeClipFilter(Database& db, InsertEntity& insert, Transaction& transaction);

/// Drop the reference's clip: the entity's boundary and the file's filter.
void removeClipFilter(Database& db, InsertEntity& insert, Transaction& transaction);

}  // namespace cad
