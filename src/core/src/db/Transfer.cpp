#include "cad/db/Transfer.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/db/ViewportEntity.hpp"

namespace cad {
namespace {

/// Clone an object and hand back the concrete type, ready for a fresh handle.
///
/// `clone()` deliberately leaves the handle null but copies the owner and the
/// other structural references, all of which name objects in the *source*
/// database. Every caller below clears or remaps them.
template <typename T>
[[nodiscard]] std::unique_ptr<T> cloneAs(const T& source) {
    DbObjectPtr cloned = source.clone();
    auto* typed = dynamic_cast<T*>(cloned.get());
    if (typed == nullptr) {
        return nullptr;
    }
    cloned.release();
    std::unique_ptr<T> owned{typed};
    owned->handle = Handle::null();
    return owned;
}

/// Copies objects between two databases, matching symbol records by name.
///
/// One instance per operation. It accumulates the handle maps as it goes, which
/// is why relinking entity-to-entity references is a second pass: a hatch may
/// name a boundary that is copied after it.
class Transfer {
public:
    Transfer(const Database& from, Database& to, Transaction* transaction)
        : from_(from), to_(to), transaction_(transaction) {}

    /// Copy one entity, together with anything it owns outright.
    ///
    /// `topLevel` distinguishes an entity being placed where the caller asked
    /// from one being copied into a block definition. Only the former is
    /// reported: after a paste the application selects what it pasted, and the
    /// line inside a door block is not something the user pasted.
    Entity* copyEntity(const Entity& source, BlockTableRecord& owner, const Mat4* placement,
                       bool topLevel = true);

    /// Rewrite the references that could only be resolved once everything was
    /// copied: attributes, hatch boundaries, viewport contents, field subjects.
    void relink();

    [[nodiscard]] TransferReport& report() noexcept { return report_; }

private:
    // ------------------------------------------------------------- symbols

    Handle layer(Handle source);
    Handle linetype(Handle source);
    Handle textStyle(Handle source);
    Handle dimStyle(Handle source);

    /// Find or create the block definition, copying its contents when new.
    ///
    /// A name that already exists in the destination keeps the destination's
    /// definition. Two drawings routinely disagree about what "TB" or "DOOR"
    /// draws, and a paste that redefined the host's block would silently
    /// change every other reference to it on the sheet.
    Handle block(Handle source);

    /// The shared shape of every symbol lookup: cache, match by name, or clone.
    template <typename T, typename Fixup>
    Handle mapSymbol(Handle source, const SymbolTableIndex& table,
                     std::unordered_map<Handle, Handle>& cache,
                     std::vector<std::string>& added, Fixup&& fixup);

    // ------------------------------------------------------------- objects

    DbObject& add(DbObjectPtr object);
    Entity& addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner);

    /// Copy a field and its children, remapping what it names.
    Handle field(Handle source);

    /// Map a copied entity's handle, or the null handle when it did not come.
    [[nodiscard]] Handle mappedEntity(Handle source) const;

    /// Remap a list of entity references in place, dropping the absent ones.
    void relinkEntityList(std::vector<Handle>& handles);

    const Database& from_;
    Database& to_;
    Transaction* transaction_;

    std::unordered_map<Handle, Handle> layers_;
    std::unordered_map<Handle, Handle> linetypes_;
    std::unordered_map<Handle, Handle> textStyles_;
    std::unordered_map<Handle, Handle> dimStyles_;
    std::unordered_map<Handle, Handle> blocks_;
    std::unordered_map<Handle, Handle> entities_;
    std::unordered_map<Handle, Handle> fields_;

    /// Source-to-copy pairs for the relink pass, in copy order.
    std::vector<std::pair<Handle, Handle>> copied_;

    /// Blocks whose contents are being copied, so a definition that references
    /// itself cannot recurse forever. Corrupt files containing exactly that
    /// exist, and the alternative is a stack overflow on open.
    std::unordered_set<Handle> blocksInProgress_;

    TransferReport report_;
};

template <typename T, typename Fixup>
Handle Transfer::mapSymbol(Handle source, const SymbolTableIndex& table,
                           std::unordered_map<Handle, Handle>& cache,
                           std::vector<std::string>& added, Fixup&& fixup) {
    if (source.isNull()) {
        return Handle::null();
    }
    if (const auto known = cache.find(source); known != cache.end()) {
        return known->second;
    }

    const auto* record = from_.get<T>(source);
    if (record == nullptr) {
        // A reference to something the source database does not hold. Nulling
        // it is right: the property falls back to ByLayer or to the drawing's
        // current setting, which is a valid drawing.
        ++report_.droppedReferences;
        cache[source] = Handle::null();
        return Handle::null();
    }

    if (const Handle existing = table.find(record->name); !existing.isNull()) {
        cache[source] = existing;
        return existing;
    }

    std::unique_ptr<T> copy = cloneAs(*record);
    if (copy == nullptr) {
        cache[source] = Handle::null();
        return Handle::null();
    }
    copy->ownerHandle = Handle::null();
    copy->extensionDictionaryHandle = Handle::null();
    copy->fieldHandle = Handle::null();
    copy->reactorHandles.clear();

    // Reserve the cache slot before fixing up, so a record that references its
    // own table -- a linetype whose shape uses a text style, a layer whose
    // linetype names a layer -- terminates.
    T& placed = static_cast<T&>(add(std::move(copy)));
    cache[source] = placed.handle;
    added.push_back(placed.name);
    fixup(placed);
    return placed.handle;
}

Handle Transfer::linetype(Handle source) {
    return mapSymbol<Linetype>(source, to_.linetypes(), linetypes_, report_.linetypesAdded,
                               [this](Linetype& copy) {
                                   // A complex linetype draws text along the
                                   // line -- "GAS", "FIBER OPTIC" -- in a style
                                   // that has to come across with it.
                                   for (LinetypeElement& element : copy.elements) {
                                       element.styleHandle = textStyle(element.styleHandle);
                                   }
                               });
}

Handle Transfer::textStyle(Handle source) {
    return mapSymbol<TextStyle>(source, to_.textStyles(), textStyles_,
                                report_.textStylesAdded, [](TextStyle&) {});
}

Handle Transfer::layer(Handle source) {
    return mapSymbol<Layer>(source, to_.layers(), layers_, report_.layersAdded,
                            [this](Layer& copy) {
                                copy.linetypeHandle = linetype(copy.linetypeHandle);
                                // Names a plot style table the destination has
                                // no reason to share.
                                copy.plotStyleHandle = Handle::null();
                            });
}

Handle Transfer::dimStyle(Handle source) {
    return mapSymbol<DimStyle>(source, to_.dimStyles(), dimStyles_, report_.dimStylesAdded,
                               [this](DimStyle& copy) {
                                   copy.textStyleHandle = textStyle(copy.textStyleHandle);
                                   copy.arrowBlock = block(copy.arrowBlock);
                                   copy.arrowBlock1 = block(copy.arrowBlock1);
                                   copy.arrowBlock2 = block(copy.arrowBlock2);
                               });
}

Handle Transfer::block(Handle source) {
    if (source.isNull()) {
        return Handle::null();
    }
    if (const auto known = blocks_.find(source); known != blocks_.end()) {
        return known->second;
    }

    const auto* record = from_.get<BlockTableRecord>(source);
    if (record == nullptr) {
        ++report_.droppedReferences;
        blocks_[source] = Handle::null();
        return Handle::null();
    }

    // A layout space is not a block definition, whatever the block table says.
    // Copying one would import the source drawing's Model or a whole sheet.
    if (record->isLayoutSpace()) {
        ++report_.droppedReferences;
        blocks_[source] = Handle::null();
        return Handle::null();
    }

    if (const Handle existing = to_.blocks().find(record->name); !existing.isNull()) {
        blocks_[source] = existing;
        report_.blocksReused.push_back(record->name);
        return existing;
    }

    auto copy = std::make_unique<BlockTableRecord>();
    copy->name = record->name;
    copy->origin = record->origin;
    copy->description = record->description;
    copy->isXrefDependent = record->isXrefDependent;
    copy->xrefPath = record->xrefPath;

    auto& placed = static_cast<BlockTableRecord&>(add(std::move(copy)));
    blocks_[source] = placed.handle;
    report_.blocksAdded.push_back(placed.name);

    if (blocksInProgress_.insert(source).second) {
        for (const Entity* member : from_.entitiesIn(*record)) {
            if (member != nullptr && !member->erased) {
                copyEntity(*member, placed, nullptr, false);
            }
        }
        blocksInProgress_.erase(source);
    }
    return placed.handle;
}

DbObject& Transfer::add(DbObjectPtr object) {
    return transaction_ != nullptr ? transaction_->addObject(std::move(object))
                                   : to_.addObject(std::move(object));
}

Entity& Transfer::addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner) {
    return transaction_ != nullptr ? transaction_->addEntity(std::move(entity), owner)
                                   : to_.addEntity(std::move(entity), owner);
}

Handle Transfer::field(Handle source) {
    if (source.isNull()) {
        return Handle::null();
    }
    if (const auto known = fields_.find(source); known != fields_.end()) {
        return known->second;
    }

    const auto* record = from_.get<FieldObject>(source);
    if (record == nullptr) {
        fields_[source] = Handle::null();
        return Handle::null();
    }

    std::unique_ptr<FieldObject> copy = cloneAs(*record);
    if (copy == nullptr) {
        fields_[source] = Handle::null();
        return Handle::null();
    }
    copy->ownerHandle = Handle::null();
    copy->extensionDictionaryHandle = Handle::null();
    copy->reactorHandles.clear();

    auto& placed = static_cast<FieldObject&>(add(std::move(copy)));
    // Cached before recursing: a field tree that names itself is malformed, but
    // it must not hang the application.
    fields_[source] = placed.handle;

    for (Handle& child : placed.childFields) {
        child = field(child);
    }
    std::erase_if(placed.childFields, [](Handle h) { return h.isNull(); });
    return placed.handle;
}

Handle Transfer::mappedEntity(Handle source) const {
    const auto found = entities_.find(source);
    return found == entities_.end() ? Handle::null() : found->second;
}

void Transfer::relinkEntityList(std::vector<Handle>& handles) {
    for (Handle& handle : handles) {
        const Handle mapped = mappedEntity(handle);
        if (mapped.isNull() && !handle.isNull()) {
            ++report_.droppedReferences;
        }
        handle = mapped;
    }
    std::erase_if(handles, [](Handle h) { return h.isNull(); });
}

Entity* Transfer::copyEntity(const Entity& source, BlockTableRecord& owner,
                             const Mat4* placement, bool topLevel) {
    if (const Handle already = mappedEntity(source.handle); !already.isNull()) {
        return to_.getEntity(already);
    }

    std::unique_ptr<Entity> copy = cloneAs(source);
    if (copy == nullptr) {
        return nullptr;
    }

    // Structural references into the source database. The owner is set by the
    // add below; the rest name objects that are not coming along.
    copy->ownerHandle = Handle::null();
    copy->extensionDictionaryHandle = Handle::null();
    copy->reactorHandles.clear();
    copy->materialHandle = Handle::null();
    copy->fieldHandle = field(copy->fieldHandle);

    copy->layerHandle = layer(source.layerHandle);
    if (copy->layerHandle.isNull()) {
        copy->layerHandle = to_.defaultLayer().handle;
    }
    copy->linetypeHandle = linetype(source.linetypeHandle);

    if (auto* text = dynamic_cast<TextEntity*>(copy.get())) {
        text->styleHandle = textStyle(text->styleHandle);
    }
    if (auto* mtext = dynamic_cast<MTextEntity*>(copy.get())) {
        mtext->styleHandle = textStyle(mtext->styleHandle);
    }
    if (auto* dimension = dynamic_cast<DimensionEntity*>(copy.get())) {
        dimension->styleHandle = dimStyle(dimension->styleHandle);
        // The anonymous block a file stores the drawn dimension in. It is kept
        // but never drawn (ADR 0005), and it belongs to the source drawing's
        // handle space, so the copy regenerates instead of pointing at it.
        dimension->blockHandle = Handle::null();
    }
    if (auto* table = dynamic_cast<TableEntity*>(copy.get())) {
        // Same reasoning as a dimension's block, and a table style the
        // destination does not have is not something a paste should invent.
        table->blockHandle = Handle::null();
        table->styleHandle = Handle::null();
        for (TableCell& cell : table->cells) {
            cell.fieldHandle = field(cell.fieldHandle);
        }
    }
    if (auto* insert = dynamic_cast<InsertEntity*>(copy.get())) {
        insert->blockHandle = block(insert->blockHandle);
    }
    if (auto* leader = dynamic_cast<LeaderEntity*>(copy.get())) {
        // A leader takes its arrows and text height from a dimension style,
        // exactly as a dimension does. Its note is another entity and is
        // relinked in the second pass.
        leader->styleHandle = dimStyle(leader->styleHandle);
    }
    if (auto* viewport = dynamic_cast<ViewportEntity*>(copy.get())) {
        for (Handle& frozen : viewport->frozenLayerHandles) {
            frozen = layer(frozen);
        }
        std::erase_if(viewport->frozenLayerHandles, [](Handle h) { return h.isNull(); });
    }

    if (placement != nullptr) {
        copy->transformBy(*placement);
    }

    Entity& placed = addEntity(std::move(copy), owner);
    entities_[source.handle] = placed.handle;
    copied_.emplace_back(source.handle, placed.handle);
    if (topLevel) {
        report_.entities.push_back(placed.handle);
    }

    // An INSERT's attributes are separate entities that belong to it. Selecting
    // the block reference is how a drafter selects them, so a copy that left
    // them behind would paste a door with no number on it.
    if (const auto* sourceInsert = dynamic_cast<const InsertEntity*>(&source)) {
        for (const Handle attribute : sourceInsert->attributeHandles) {
            const Entity* value = from_.getEntity(attribute);
            if (value != nullptr && !value->erased) {
                copyEntity(*value, owner, placement, topLevel);
            }
        }
    }

    // A leader's note, for the same reason. The two are separate entities but
    // one thing on the sheet, and copying an arrow with no words on it is not
    // what selecting a callout and pressing Ctrl+C meant.
    if (const auto* sourceLeader = dynamic_cast<const LeaderEntity*>(&source)) {
        const Entity* note = from_.getEntity(sourceLeader->annotationHandle);
        if (note != nullptr && !note->erased) {
            copyEntity(*note, owner, placement, topLevel);
        }
    }

    return &placed;
}

void Transfer::relink() {
    for (const auto& [sourceHandle, copyHandle] : copied_) {
        const Entity* source = from_.getEntity(sourceHandle);
        Entity* copy = to_.getEntity(copyHandle);
        if (source == nullptr || copy == nullptr) {
            continue;
        }

        if (auto* insert = dynamic_cast<InsertEntity*>(copy)) {
            relinkEntityList(insert->attributeHandles);
        }
        if (auto* hatch = dynamic_cast<HatchEntity*>(copy)) {
            // A hatch keeps its own boundary geometry, so dropping an
            // association to something that stayed behind loses the link to the
            // original, not the shape. Following it instead would silently
            // paste geometry the user never selected.
            for (HatchBoundaryPath& path : hatch->paths) {
                relinkEntityList(path.sourceHandles);
            }
        }
        if (auto* viewport = dynamic_cast<ViewportEntity*>(copy)) {
            viewport->clipEntityHandle = mappedEntity(viewport->clipEntityHandle);
        }
        if (auto* leader = dynamic_cast<LeaderEntity*>(copy)) {
            const Handle note = mappedEntity(leader->annotationHandle);
            if (note.isNull() && !leader->annotationHandle.isNull()) {
                ++report_.droppedReferences;
            }
            // A pasted copy is a leader this application made, so it takes the
            // link the way a new one does -- and a copy whose note stayed
            // behind says it has none.
            leader->setAnnotation(note);
        }
    }

    for (const auto& [sourceField, copyField] : fields_) {
        auto* copy = to_.get<FieldObject>(copyField);
        if (copy == nullptr) {
            continue;
        }
        // A field naming an entity that did not come along keeps its cached
        // value, which is the documented behaviour for a field that cannot be
        // evaluated: a stale number beats an error.
        for (Handle& subject : copy->referencedObjects) {
            subject = mappedEntity(subject);
        }
        std::erase_if(copy->referencedObjects, [](Handle h) { return h.isNull(); });
    }
}

}  // namespace

ExtractedContent extractEntities(const Database& source, const std::vector<Handle>& handles) {
    ExtractedContent content;
    content.database = Database::createEmpty();

    Transfer transfer{source, *content.database, nullptr};
    BlockTableRecord& model = content.database->modelSpace();

    for (const Handle handle : handles) {
        const Entity* entity = source.getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        transfer.copyEntity(*entity, model, nullptr);
    }
    transfer.relink();
    content.report = std::move(transfer.report());

    // The lower-left of the extents, which is where other CAD software measures a copy
    // from when the user does not name a base point. Taken after the copy so it
    // reflects what actually came across.
    const Box3 extents = content.database->blockExtents(model);
    content.basePoint = extents.isEmpty() ? Vec3{} : extents.min;

    // Recorded in the fragment itself, under the two header variables that
    // already mean this. A copy that leaves the process -- onto the clipboard,
    // into a file -- then carries its own base point and its own units, so
    // whatever reads it back can place and scale it without being told.
    content.database->settings.insertionBase = content.basePoint;
    content.database->settings.insertionUnits = source.settings.insertionUnits;
    return content;
}

TransferReport pasteContent(const Database& content, BlockTableRecord& owner,
                            const Mat4& placement, Transaction& transaction) {
    Transfer transfer{content, transaction.database(), &transaction};

    const auto* model = content.get<BlockTableRecord>(content.blocks().find(kModelSpace));
    if (model != nullptr) {
        for (const Entity* entity : content.entitiesIn(*model)) {
            if (entity != nullptr && !entity->erased) {
                transfer.copyEntity(*entity, owner, &placement);
            }
        }
    }
    transfer.relink();
    return std::move(transfer.report());
}

}  // namespace cad
