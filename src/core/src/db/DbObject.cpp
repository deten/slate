#include "cad/db/DbObject.hpp"

#include <variant>

namespace cad {

void DbObject::copyBaseInto(DbObject& target) const {
    target.ownerHandle = ownerHandle;
    target.extensionDictionaryHandle = extensionDictionaryHandle;
    target.fieldHandle = fieldHandle;
    target.reactorHandles = reactorHandles;
    target.erased = erased;
    target.xdata = xdata;
    target.preserved = preserved ? std::make_unique<PreservedData>(*preserved) : nullptr;
}

void DbObject::restoreBaseFrom(const DbObject& snapshot) {
    ownerHandle = snapshot.ownerHandle;
    extensionDictionaryHandle = snapshot.extensionDictionaryHandle;
    fieldHandle = snapshot.fieldHandle;
    reactorHandles = snapshot.reactorHandles;
    erased = snapshot.erased;
    xdata = snapshot.xdata;
    preserved =
        snapshot.preserved ? std::make_unique<PreservedData>(*snapshot.preserved) : nullptr;
}

namespace {

/// The first group with `code`, when it holds a number.
[[nodiscard]] UnknownGroup* findNumber(std::vector<UnknownGroup>& groups, int code) {
    for (UnknownGroup& group : groups) {
        if (group.code == code) {
            return std::holds_alternative<double>(group.value) ? &group : nullptr;
        }
    }
    return nullptr;
}

}  // namespace

bool PreservedData::rewritePoint(int xCode, const Vec3& value) {
    // Both found before either is written. A point half-updated is worse than
    // one left alone: the object would move diagonally by whatever the file
    // happened to hold in the coordinate that was not there.
    UnknownGroup* x = findNumber(unknownCodes, xCode);
    UnknownGroup* y = findNumber(unknownCodes, xCode + 10);
    if (x == nullptr || y == nullptr) {
        return false;
    }
    x->value = value.x;
    y->value = value.y;
    // Z is genuinely optional in 2D DXF, so its absence is not a failure.
    if (UnknownGroup* z = findNumber(unknownCodes, xCode + 20)) {
        z->value = value.z;
    }
    return true;
}

bool PreservedData::rewriteNumber(int code, double value) {
    UnknownGroup* group = findNumber(unknownCodes, code);
    if (group == nullptr) {
        return false;
    }
    group->value = value;
    return true;
}

std::vector<Handle> DbObject::referencedHandles() const {
    std::vector<Handle> refs = reactorHandles;
    if (!extensionDictionaryHandle.isNull()) {
        refs.push_back(extensionDictionaryHandle);
    }
    return refs;
}

const std::vector<XDataItem>* DbObject::getXData(const std::string& appName) const {
    const auto it = xdata.find(appName);
    return it == xdata.end() ? nullptr : &it->second;
}

void DbObject::setXData(const std::string& appName, std::vector<XDataItem> items) {
    xdata[appName] = std::move(items);
}

}  // namespace cad
