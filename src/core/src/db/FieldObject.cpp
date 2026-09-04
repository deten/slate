#include "cad/db/FieldObject.hpp"

#include <stdexcept>

namespace cad {

std::string FieldObject::evaluatorName() const {
    // `AcObjProp.16.2` is the same evaluator as `AcObjProp`; the suffix is the
    // version of the evaluator that wrote it. Matching on the whole string
    // would make every file from a different other CAD software release unrecognized.
    const std::size_t dot = evaluatorId.find('.');
    return dot == std::string::npos ? evaluatorId : evaluatorId.substr(0, dot);
}

DbObjectPtr FieldObject::clone() const {
    auto copy = std::make_unique<FieldObject>();
    copyBaseInto(*copy);
    copy->evaluatorId = evaluatorId;
    copy->fieldCode = fieldCode;
    copy->cachedValue = cachedValue;
    copy->childFields = childFields;
    copy->referencedObjects = referencedObjects;
    return copy;
}

void FieldObject::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const FieldObject*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a field");
    }
    restoreBaseFrom(*source);
    evaluatorId = source->evaluatorId;
    fieldCode = source->fieldCode;
    cachedValue = source->cachedValue;
    childFields = source->childFields;
    referencedObjects = source->referencedObjects;
}

std::vector<Handle> FieldObject::referencedHandles() const {
    std::vector<Handle> handles = DbObject::referencedHandles();
    handles.insert(handles.end(), childFields.begin(), childFields.end());
    // Deliberately *not* the referenced objects. A field naming an entity does
    // not keep it alive, and reporting it as a reference would make erasing the
    // entity look unsafe when the correct outcome is a field that says so.
    return handles;
}

}  // namespace cad
