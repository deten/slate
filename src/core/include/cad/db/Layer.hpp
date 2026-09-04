#pragma once

#include "cad/db/Color.hpp"
#include "cad/db/SymbolTable.hpp"

namespace cad {

/// The layer every drawing has and none may delete or rename.
inline constexpr const char* kLayerZero = "0";
/// The layer other CAD software reserves for dimension definition points.
inline constexpr const char* kLayerDefpoints = "Defpoints";

/// A drawing layer.
///
/// Layers carry both *visibility* state (on/off, frozen, viewport-frozen) and
/// *default property* state (colour, linetype, lineweight) that entities
/// inherit through ByLayer. Both are modelled here rather than resolved at load
/// time, so toggling a layer updates every entity on it immediately.
///
/// The distinction between "off" and "frozen" is preserved because it is real:
/// frozen layers are skipped during regeneration and excluded from ZOOM
/// EXTENTS, while layers that are merely off still participate.
class Layer final : public SymbolTableRecord {
public:
    /// Default colour for entities whose colour is ByLayer.
    Color color = Color::fromIndex(7);

    /// Handle of the default linetype record.
    Handle linetypeHandle;

    /// Default lineweight, in hundredths of a millimetre.
    std::int16_t lineweight = toRaw(Lineweight::Default);

    /// Default transparency; empty means fully opaque.
    Transparency transparency;

    /// Turned off: entities are hidden but still regenerated and still
    /// included in extents.
    bool isOff = false;

    /// Frozen: entities are hidden *and* excluded from regeneration and
    /// extents. Freezing is the performance tool; turning off is the visual one.
    bool isFrozen = false;

    /// Locked layers render normally but reject selection and modification.
    bool isLocked = false;

    /// Excluded from plotting while still visible on screen.
    bool isPlottable = true;

    /// Frozen by default in newly created viewports.
    bool isFrozenInNewViewports = false;

    /// Handle of the plot style record, for named plot style drawings.
    Handle plotStyleHandle;

    /// Optional description shown in the layer manager.
    std::string description;

    /// True when this layer came from an external reference. Xref layers are
    /// named `xrefname|layername` and may not be renamed or deleted directly.
    bool isXrefDependent = false;

    [[nodiscard]] std::string objectType() const override { return "Layer"; }

    [[nodiscard]] bool isReserved() const override {
        return normalizeSymbolName(name) == normalizeSymbolName(kLayerZero);
    }

    /// True when entities on this layer should be drawn.
    ///
    /// Locked layers are still visible — the lock restricts editing only —
    /// which is why locking is not part of this test.
    [[nodiscard]] bool isVisible() const noexcept { return !isOff && !isFrozen; }

    /// True when entities on this layer may be selected or edited.
    [[nodiscard]] bool isEditable() const noexcept { return isVisible() && !isLocked; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

}  // namespace cad
