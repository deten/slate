#include "cad/db/Layer.hpp"

#include <stdexcept>

namespace cad {

DbObjectPtr Layer::clone() const {
    auto copy = std::make_unique<Layer>();
    copyRecordInto(*copy);
    copy->color = color;
    copy->linetypeHandle = linetypeHandle;
    copy->lineweight = lineweight;
    copy->transparency = transparency;
    copy->isOff = isOff;
    copy->isFrozen = isFrozen;
    copy->isLocked = isLocked;
    copy->isPlottable = isPlottable;
    copy->isFrozenInNewViewports = isFrozenInNewViewports;
    copy->plotStyleHandle = plotStyleHandle;
    copy->description = description;
    copy->isXrefDependent = isXrefDependent;
    return copy;
}

void Layer::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const Layer*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a layer");
    }
    restoreRecordFrom(*source);
    color = source->color;
    linetypeHandle = source->linetypeHandle;
    lineweight = source->lineweight;
    transparency = source->transparency;
    isOff = source->isOff;
    isFrozen = source->isFrozen;
    isLocked = source->isLocked;
    isPlottable = source->isPlottable;
    isFrozenInNewViewports = source->isFrozenInNewViewports;
    plotStyleHandle = source->plotStyleHandle;
    description = source->description;
    isXrefDependent = source->isXrefDependent;
}

std::vector<Handle> Layer::referencedHandles() const {
    std::vector<Handle> refs = SymbolTableRecord::referencedHandles();
    if (!linetypeHandle.isNull()) {
        refs.push_back(linetypeHandle);
    }
    if (!plotStyleHandle.isNull()) {
        refs.push_back(plotStyleHandle);
    }
    return refs;
}

}  // namespace cad
