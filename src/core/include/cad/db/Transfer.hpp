#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/math/Mat4.hpp"

namespace cad {

/// Copying objects from one drawing into another.
///
/// This is the machinery behind cut/copy/paste, and the same problem xrefs,
/// WBLOCK and INSERT-a-drawing all solve: an entity means nothing without the
/// records it names. A line carries a *handle* to its layer, and that handle is
/// meaningless in another database — so a copy that moves geometry alone
/// arrives on the wrong layer, in the wrong colour, with the wrong linetype, or
/// pointing at a block definition that does not exist.
///
/// Two rules shape everything here:
///
///  - **Symbol records are matched by name, never by handle.** Pasting onto a
///    drawing that already has a layer called "Walls" puts the geometry on
///    *that* layer and leaves its colour alone. The alternative — importing a
///    second "Walls" under a made-up name — is how a drawing ends up with
///    forty layers that differ only by suffix, and it is not what a drafter
///    means by paste.
///
///  - **A reference to something that did not come along is dropped, not
///    followed.** A hatch associated with a boundary the user did not select
///    would otherwise silently drag that geometry into the paste. other CAD software
///    drops the association here too, and the count is reported rather than
///    hidden.

/// What a copy carried, and what it could not.
struct TransferReport {
    /// Handles of the copied entities, in the destination database.
    std::vector<Handle> entities;
    /// Names of symbol records created in the destination because it had none
    /// by that name. Records matched to an existing name are not listed.
    std::vector<std::string> layersAdded;
    std::vector<std::string> linetypesAdded;
    std::vector<std::string> textStylesAdded;
    std::vector<std::string> dimStylesAdded;
    std::vector<std::string> blocksAdded;
    /// Blocks whose name already existed in the destination, whose definition
    /// was therefore kept. Worth telling the user about: the pasted reference
    /// may draw as something other than what was copied.
    std::vector<std::string> blocksReused;
    /// References that pointed at objects outside the copy and were cleared.
    std::size_t droppedReferences = 0;
};

/// A self-contained copy of part of a drawing.
struct ExtractedContent {
    /// A database whose model space holds the copies, together with every
    /// symbol record they depend on. Stands alone: it can be written as DXF,
    /// put on the system clipboard, and read back somewhere else.
    std::unique_ptr<Database> database;

    /// The point the copy is measured from — the lower-left of its extents,
    /// which is what other CAD software's COPYCLIP uses when a base point is not given.
    Vec3 basePoint;

    TransferReport report;

    [[nodiscard]] bool isEmpty() const { return report.entities.empty(); }
};

/// Copy entities out of a drawing into a database that stands on its own.
///
/// Erased entities and handles that name nothing are skipped. Entities on
/// locked layers are *not* skipped: copying does not modify the source, and a
/// lock protects against editing, not against being read.
[[nodiscard]] ExtractedContent extractEntities(const Database& source,
                                               const std::vector<Handle>& handles);

/// Paste a database's model space into `owner`, offset by `placement`.
///
/// Every change goes through `transaction`, so a paste is one undo step
/// including the layers and blocks it had to create.
TransferReport pasteContent(const Database& content, BlockTableRecord& owner,
                            const Mat4& placement, Transaction& transaction);

}  // namespace cad
