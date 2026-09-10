#include "cad/db/ClipFilter.hpp"

#include <memory>
#include <string>

#include "cad/db/Dictionary.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/Transaction.hpp"

namespace cad {
namespace {

void ensurePreserved(DbObject& object) {
    if (object.preserved == nullptr) {
        object.preserved = std::make_unique<PreservedData>();
    }
}

/// A dictionary as the file holds one: the class marker, the "hard owner"
/// flag, then name and handle pairs.
void setDictionaryEntries(DbObject& dictionary,
                          const std::vector<std::pair<std::string, Handle>>& entries) {
    ensurePreserved(dictionary);
    std::vector<UnknownGroup>& groups = dictionary.preserved->unknownCodes;
    groups.clear();
    groups.push_back(UnknownGroup{100, std::string{"AcDbDictionary"}});
    groups.push_back(UnknownGroup{281, std::int64_t{1}});
    for (const auto& [name, handle] : entries) {
        groups.push_back(UnknownGroup{3, name});
        groups.push_back(UnknownGroup{350, handle});
    }
}

/// The dictionary a name leads to under `owner`, made when missing.
DbObject& ensureDictionary(Database& db, DbObject& owner, const std::string& name,
                           Transaction& transaction) {
    const Handle existing = dictionaryEntry(owner, name);
    if (DbObject* found = existing.isNull() ? nullptr : db.getObject(existing); found != nullptr) {
        return *found;
    }
    auto dictionary = std::make_unique<RawObject>();
    dictionary->dxfType = "DICTIONARY";
    dictionary->ownerHandle = owner.handle;
    setDictionaryEntries(*dictionary, {});
    DbObject& added = transaction.addObject(std::move(dictionary));
    std::vector<std::pair<std::string, Handle>> entries = dictionaryEntries(owner);
    entries.emplace_back(name, added.handle);
    transaction.openForWrite(owner);
    setDictionaryEntries(owner, entries);
    return added;
}

}  // namespace

void writeClipFilter(Database& db, InsertEntity& insert, Transaction& transaction) {
    // The reference's extension dictionary, made when it has none.
    DbObject* xdictionary = insert.extensionDictionaryHandle.isNull()
                                ? nullptr
                                : db.getObject(insert.extensionDictionaryHandle);
    if (xdictionary == nullptr) {
        auto dictionary = std::make_unique<RawObject>();
        dictionary->dxfType = "DICTIONARY";
        dictionary->ownerHandle = insert.handle;
        setDictionaryEntries(*dictionary, {});
        xdictionary = &transaction.addObject(std::move(dictionary));
        transaction.openForWrite(insert);
        insert.extensionDictionaryHandle = xdictionary->handle;
    }
    DbObject& filters = ensureDictionary(db, *xdictionary, "ACAD_FILTER", transaction);

    const Handle existing = dictionaryEntry(filters, "SPATIAL");
    RawObject* filter = existing.isNull() ? nullptr : db.get<RawObject>(existing);
    if (filter == nullptr) {
        auto made = std::make_unique<RawObject>();
        made->dxfType = "SPATIAL_FILTER";
        made->ownerHandle = filters.handle;
        filter = static_cast<RawObject*>(&transaction.addObject(std::move(made)));
        std::vector<std::pair<std::string, Handle>> entries = dictionaryEntries(filters);
        entries.emplace_back("SPATIAL", filter->handle);
        transaction.openForWrite(filters);
        setDictionaryEntries(filters, entries);
    } else {
        transaction.openForWrite(*filter);
    }

    // The filter's groups: the boundary in block coordinates with identity
    // placing matrices, which is how this drawing's own clips are kept.
    // Point count, points, the plane, on or off, no front or back clip,
    // then the two 4x3 matrices.
    ensurePreserved(*filter);
    std::vector<UnknownGroup>& groups = filter->preserved->unknownCodes;
    groups.clear();
    groups.push_back(UnknownGroup{100, std::string{"AcDbFilter"}});
    groups.push_back(UnknownGroup{100, std::string{"AcDbSpatialFilter"}});
    groups.push_back(UnknownGroup{70, static_cast<std::int64_t>(insert.clipBoundary.size())});
    for (const Vec3& p : insert.clipBoundary) {
        groups.push_back(UnknownGroup{10, p.x});
        groups.push_back(UnknownGroup{20, p.y});
    }
    groups.push_back(UnknownGroup{210, 0.0});
    groups.push_back(UnknownGroup{220, 0.0});
    groups.push_back(UnknownGroup{230, 1.0});
    groups.push_back(UnknownGroup{11, 0.0});
    groups.push_back(UnknownGroup{21, 0.0});
    groups.push_back(UnknownGroup{31, 0.0});
    groups.push_back(UnknownGroup{71, static_cast<std::int64_t>(insert.clipEnabled ? 1 : 0)});
    groups.push_back(UnknownGroup{72, std::int64_t{0}});
    groups.push_back(UnknownGroup{73, std::int64_t{0}});
    for (int matrix = 0; matrix < 2; ++matrix) {
        const double identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (const double value : identity) {
            groups.push_back(UnknownGroup{40, value});
        }
    }
}

void removeClipFilter(Database& db, InsertEntity& insert, Transaction& transaction) {
    transaction.openForWrite(insert);
    insert.clipBoundary.clear();
    insert.clipEnabled = true;
    DbObject* xdictionary = insert.extensionDictionaryHandle.isNull()
                                ? nullptr
                                : db.getObject(insert.extensionDictionaryHandle);
    if (xdictionary == nullptr) {
        return;
    }
    const Handle filtersHandle = dictionaryEntry(*xdictionary, "ACAD_FILTER");
    DbObject* filters = filtersHandle.isNull() ? nullptr : db.getObject(filtersHandle);
    if (filters == nullptr) {
        return;
    }
    const Handle spatial = dictionaryEntry(*filters, "SPATIAL");
    if (DbObject* filter = spatial.isNull() ? nullptr : db.getObject(spatial); filter != nullptr) {
        static_cast<void>(transaction.erase(*filter));
    }
    std::vector<std::pair<std::string, Handle>> entries;
    for (const auto& entry : dictionaryEntries(*filters)) {
        if (entry.first != "SPATIAL") {
            entries.push_back(entry);
        }
    }
    transaction.openForWrite(*filters);
    setDictionaryEntries(*filters, entries);
}

}  // namespace cad
