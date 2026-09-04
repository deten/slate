#pragma once

/// Readers for the two annotation entities complex enough to need their own
/// translation unit: DIMENSION, whose meaning depends on a type code, and
/// HATCH, whose boundary data is a nested sequential structure rather than a
/// flat set of group codes.

#include <memory>

#include "cad/io/DxfReader.hpp"

namespace cad::io {

[[nodiscard]] std::unique_ptr<Entity> readDimension(const DxfRecord& record,
                                                    ReadContext& context);

[[nodiscard]] std::unique_ptr<Entity> readHatch(const DxfRecord& record, ReadContext& context);

/// MLINE — several parallel lines drawn as one object, its per-vertex offsets
/// arriving as a sequential run of group codes rather than a flat set.
[[nodiscard]] std::unique_ptr<Entity> readMline(const DxfRecord& record, ReadContext& context);

/// MULTILEADER — a note with any number of arrows, whose content is a nested
/// CONTEXT_DATA structure rather than a flat set of group codes.
[[nodiscard]] std::unique_ptr<Entity> readMLeader(const DxfRecord& record,
                                                  ReadContext& context);

/// LEADER — an arrow, a path back from it, and a note at the far end.
[[nodiscard]] std::unique_ptr<Entity> readLeader(const DxfRecord& record, ReadContext& context);

/// VIEWPORT — a window from a sheet onto the model.
[[nodiscard]] std::unique_ptr<Entity> readViewport(const DxfRecord& record,
                                                   ReadContext& context);

}  // namespace cad::io
