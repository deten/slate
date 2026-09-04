#pragma once

#include <vector>

#include "cad/db/SymbolTable.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// Name of the block record holding model-space geometry.
inline constexpr const char* kModelSpace = "*Model_Space";
/// Name of the block record holding the first paper-space layout.
inline constexpr const char* kPaperSpace = "*Paper_Space";

/// How an external reference was attached, which determines nesting behaviour.
enum class XrefType : std::uint8_t {
    /// Not an external reference.
    None,
    /// Nested overlays of an overlay are not loaded — this is what stops site
    /// plans multiplying through a drawing set.
    Overlay,
    /// Contents are linked; nested xrefs of an attachment are also loaded.
    Attachment,
};

/// The current resolution state of an external reference.
///
/// Tracking this explicitly is what allows the reference manager to show a
/// useful status, and what allows a drawing with a broken reference to open
/// anyway — a missing xref must never prevent the host drawing from loading.
enum class XrefStatus : std::uint8_t {
    NotAnXref,
    /// Loaded successfully.
    Resolved,
    /// The file could not be found on any search path.
    NotFound,
    /// Found but failed to parse.
    Unresolved,
    /// Deliberately unloaded by the user; the reference is retained.
    Unloaded,
    /// Loading would create a cycle.
    Circular,
};

/// A block definition — the container for a named, reusable set of entities.
///
/// Block table records serve three distinct roles, which is why this class is
/// central to the whole document model:
///
///  1. **Ordinary blocks**: reusable symbol definitions instanced by INSERT.
///  2. **Layout spaces**: model space and each paper-space layout are block
///     records, so every entity in the drawing lives in one of them.
///  3. **External references**: an xref is a block record whose content comes
///     from another file.
///
/// Treating all three uniformly means layouts, blocks and xrefs share the same
/// entity storage, traversal and rendering paths rather than each needing their
/// own.
class BlockTableRecord final : public SymbolTableRecord {
public:
    /// Handles of the entities in this block, in draw order.
    ///
    /// Order is significant: it determines what is drawn on top, and DWG
    /// preserves it. Storing handles rather than objects keeps the record valid
    /// across undo, when objects are replaced by restored clones.
    std::vector<Handle> entityHandles;

    /// Base point of the block, in its own coordinate system. An INSERT places
    /// this point at its insertion point.
    Vec3 origin;

    std::string description;

    XrefType xrefType = XrefType::None;
    XrefStatus xrefStatus = XrefStatus::NotAnXref;

    /// Path exactly as stored in the file. Preserved verbatim — resolution
    /// produces a separate resolved path and never overwrites this, so saving
    /// does not silently rewrite the user's relative paths as absolute.
    std::string xrefPath;

    /// Absolute path the reference actually resolved to, once loaded.
    std::string resolvedPath;

    /// True when this record was brought in *by* an xref rather than defined
    /// locally. Xref-dependent blocks are named `xrefname|blockname`.
    bool isXrefDependent = false;

    /// Handle of the layout object, for records that are layout spaces.
    Handle layoutHandle;

    /// True when the block contains attribute definitions.
    bool hasAttributeDefinitions = false;

    /// Units the block was authored in, for automatic scaling on insertion.
    /// Matches the DXF `$INSUNITS` enumeration.
    int units = 0;

    bool allowsExploding = true;
    bool allowsNonUniformScaling = true;

    [[nodiscard]] std::string objectType() const override { return "BlockTableRecord"; }

    [[nodiscard]] bool isReserved() const override { return isLayoutSpace(); }

    /// True for anonymous blocks — those other CAD software generates internally for hatch
    /// associativity, dimensions and dynamic block states. Their names begin
    /// with `*` and they are hidden from the block picker.
    [[nodiscard]] bool isAnonymous() const { return !name.empty() && name.front() == '*'; }

    /// True when this record is model space or a paper-space layout.
    [[nodiscard]] bool isLayoutSpace() const;

    [[nodiscard]] bool isModelSpace() const;

    [[nodiscard]] bool isXref() const noexcept { return xrefType != XrefType::None; }

    /// True when the reference is present but its content is unavailable.
    ///
    /// The renderer draws a placeholder for these rather than nothing, so the
    /// user can see that part of the drawing is missing.
    [[nodiscard]] bool isMissingContent() const noexcept {
        return isXref() && xrefStatus != XrefStatus::Resolved &&
               xrefStatus != XrefStatus::Unloaded;
    }

    [[nodiscard]] std::size_t entityCount() const noexcept { return entityHandles.size(); }

    /// Append an entity handle to the end of the draw order.
    void appendEntity(Handle entityHandle) { entityHandles.push_back(entityHandle); }

    /// Remove an entity handle. Returns false when it was not present.
    bool removeEntity(Handle entityHandle);

    /// Move an entity within the draw order (DRAWORDER / BRING TO FRONT).
    bool reorderEntity(Handle entityHandle, std::size_t newIndex);

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

}  // namespace cad
