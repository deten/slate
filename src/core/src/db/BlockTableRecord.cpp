#include "cad/db/BlockTableRecord.hpp"

#include <algorithm>
#include <stdexcept>

namespace cad {
namespace {

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool BlockTableRecord::isLayoutSpace() const {
    const std::string upper = normalizeSymbolName(name);
    return startsWith(upper, "*MODEL_SPACE") || startsWith(upper, "*PAPER_SPACE");
}

bool BlockTableRecord::isModelSpace() const {
    return startsWith(normalizeSymbolName(name), "*MODEL_SPACE");
}

bool BlockTableRecord::removeEntity(Handle entityHandle) {
    const auto it = std::find(entityHandles.begin(), entityHandles.end(), entityHandle);
    if (it == entityHandles.end()) {
        return false;
    }
    entityHandles.erase(it);
    return true;
}

bool BlockTableRecord::reorderEntity(Handle entityHandle, std::size_t newIndex) {
    const auto it = std::find(entityHandles.begin(), entityHandles.end(), entityHandle);
    if (it == entityHandles.end()) {
        return false;
    }
    entityHandles.erase(it);
    const std::size_t clamped = std::min(newIndex, entityHandles.size());
    entityHandles.insert(entityHandles.begin() + static_cast<std::ptrdiff_t>(clamped),
                         entityHandle);
    return true;
}

DbObjectPtr BlockTableRecord::clone() const {
    auto copy = std::make_unique<BlockTableRecord>();
    copyRecordInto(*copy);
    copy->entityHandles = entityHandles;
    copy->origin = origin;
    copy->description = description;
    copy->xrefType = xrefType;
    copy->xrefStatus = xrefStatus;
    copy->xrefPath = xrefPath;
    copy->resolvedPath = resolvedPath;
    copy->isXrefDependent = isXrefDependent;
    copy->layoutHandle = layoutHandle;
    copy->hasAttributeDefinitions = hasAttributeDefinitions;
    copy->units = units;
    copy->allowsExploding = allowsExploding;
    copy->allowsNonUniformScaling = allowsNonUniformScaling;
    return copy;
}

void BlockTableRecord::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const BlockTableRecord*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a block record");
    }
    restoreRecordFrom(*source);
    entityHandles = source->entityHandles;
    origin = source->origin;
    description = source->description;
    xrefType = source->xrefType;
    xrefStatus = source->xrefStatus;
    xrefPath = source->xrefPath;
    resolvedPath = source->resolvedPath;
    isXrefDependent = source->isXrefDependent;
    layoutHandle = source->layoutHandle;
    hasAttributeDefinitions = source->hasAttributeDefinitions;
    units = source->units;
    allowsExploding = source->allowsExploding;
    allowsNonUniformScaling = source->allowsNonUniformScaling;
}

std::vector<Handle> BlockTableRecord::referencedHandles() const {
    std::vector<Handle> refs = SymbolTableRecord::referencedHandles();
    refs.insert(refs.end(), entityHandles.begin(), entityHandles.end());
    if (!layoutHandle.isNull()) {
        refs.push_back(layoutHandle);
    }
    return refs;
}

}  // namespace cad
