#include "cad/db/Purge.hpp"

#include <cctype>
#include <algorithm>
#include <unordered_set>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/db/Transaction.hpp"

namespace cad {
namespace {

using HandleSet = std::unordered_set<std::uint64_t>;

/// Names a drawing is not valid without, whatever refers to them.
[[nodiscard]] bool isReservedName(PurgeCandidate::Kind kind, const std::string& name) {
    switch (kind) {
        case PurgeCandidate::Kind::Layer:
            return name == kLayerZero;
        case PurgeCandidate::Kind::Linetype:
            return name == kLinetypeContinuous || name == kLinetypeByLayer ||
                   name == kLinetypeByBlock;
        case PurgeCandidate::Kind::TextStyle:
            return name == kTextStyleStandard;
        case PurgeCandidate::Kind::DimStyle:
            return name == kDimStyleStandard;
        case PurgeCandidate::Kind::Block:
            return false;
    }
    return false;
}

/// Block records that something in a layout eventually draws.
///
/// The same walk the reference manager uses to tell a placed image from one
/// sitting in a block nothing inserts, and for the same reason: a block
/// definition is not a drawing of anything until an INSERT puts it somewhere.
/// `keep` names blocks already known to survive, so the sweep can be repeated
/// as candidates are removed.
[[nodiscard]] HandleSet blocksReachedFromLayouts(const Database& db, const HandleSet& doomed) {
    HandleSet reached;
    std::vector<Handle> pending;

    const auto seed = [&](Handle handle) {
        if (!handle.isNull() && doomed.count(handle.value()) == 0 &&
            reached.insert(handle.value()).second) {
            pending.push_back(handle);
        }
    };

    for (const Layout* layout : db.layouts()) {
        if (layout != nullptr) {
            seed(layout->blockRecordHandle);
        }
    }
    // Every layout space, named as well as through the layouts, because a bare
    // file may have no LAYOUT objects and its model space is still drawn.
    for (const Handle handle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(handle);
        if (block != nullptr && !block->erased && block->isLayoutSpace()) {
            seed(handle);
        }
    }

    while (!pending.empty()) {
        const Handle current = pending.back();
        pending.pop_back();
        const auto* block = db.get<BlockTableRecord>(current);
        if (block == nullptr) {
            continue;
        }
        for (const Entity* entity : db.entitiesIn(*block)) {
            for (const Handle referenced : entity->referencedHandles()) {
                if (db.get<BlockTableRecord>(referenced) != nullptr) {
                    seed(referenced);
                }
            }
        }
    }
    return reached;
}

/// Every handle referred to by anything that survives this purge.
///
/// The contents of a block count for as long as the block does, whether or not
/// anything inserts it. A block definition nothing places still names its
/// layers, and deleting one out from under it leaves its geometry pointing at a
/// layer that is not in the file.
///
/// So the test is whether the owner is *doomed*, not whether it is drawn. That
/// makes a layer free only once the block holding its geometry is actually
/// going -- which is why purging blocks and layers together clears the layers a
/// traced-over import brought with it, and purging layers alone does not.
[[nodiscard]] HandleSet referencesFromSurvivors(const Database& db, const HandleSet& doomed) {
    HandleSet used;
    const auto note = [&](const DbObject& object) {
        for (const Handle handle : object.referencedHandles()) {
            if (!handle.isNull()) {
                used.insert(handle.value());
            }
        }
    };

    for (const Handle handle : db.objectHandles()) {
        const DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased || doomed.count(handle.value()) != 0) {
            continue;
        }
        // An entity speaks for its layer and linetype until it is erased with
        // the block that owns it.
        if (const auto* entity = dynamic_cast<const Entity*>(object)) {
            if (doomed.count(entity->ownerHandle.value()) != 0) {
                continue;
            }
        }
        note(*object);
    }
    return used;
}

}  // namespace

bool isMachineNamedBlock(std::string_view name) noexcept {
    if (!name.empty() && name.front() == '*') {
        return true;
    }
    // `A$C` then hex. The prefix alone is not enough: a person may well name a
    // block "A$Cabinets", and purging that quietly would be worse than leaving
    // a serial number in the list.
    constexpr std::string_view kCopyPrefix = "A$C";
    if (name.size() <= kCopyPrefix.size() || name.substr(0, kCopyPrefix.size()) != kCopyPrefix) {
        return false;
    }
    return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(kCopyPrefix.size()), name.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string_view purgeKindName(PurgeCandidate::Kind kind) noexcept {
    switch (kind) {
        case PurgeCandidate::Kind::Block:
            return "Block";
        case PurgeCandidate::Kind::Layer:
            return "Layer";
        case PurgeCandidate::Kind::Linetype:
            return "Linetype";
        case PurgeCandidate::Kind::TextStyle:
            return "Text style";
        case PurgeCandidate::Kind::DimStyle:
            return "Dimension style";
    }
    return "Object";
}

std::vector<PurgeCandidate> findPurgeable(const Database& db, PurgeKinds allowed) {
    const DrawingSettings& settings = db.settings;

    // In use because the next thing drawn will use them, whatever the drawing
    // currently contains.
    HandleSet current;
    const auto keepCurrent = [&](Handle handle) {
        if (!handle.isNull()) {
            current.insert(handle.value());
        }
    };
    keepCurrent(db.layers().find(settings.currentLayerName));
    keepCurrent(db.textStyles().find(settings.currentTextStyleName));
    keepCurrent(db.dimStyles().find(settings.currentDimStyleName));
    keepCurrent(db.linetypes().find(settings.currentLinetypeName));

    // Each table's records, paired with the kind they will be reported as.
    struct Table {
        PurgeCandidate::Kind kind;
        PurgeKinds flag;
        const SymbolTableIndex* index;
    };
    std::vector<Table> tables;
    const auto consider = [&](PurgeCandidate::Kind kind, PurgeKinds flag,
                              const SymbolTableIndex& index) {
        if (contains(allowed, flag)) {
            tables.push_back({kind, flag, &index});
        }
    };
    consider(PurgeCandidate::Kind::Block, PurgeKinds::Blocks, db.blocks());
    consider(PurgeCandidate::Kind::Layer, PurgeKinds::Layers, db.layers());
    consider(PurgeCandidate::Kind::Linetype, PurgeKinds::Linetypes, db.linetypes());
    consider(PurgeCandidate::Kind::TextStyle, PurgeKinds::TextStyles, db.textStyles());
    consider(PurgeCandidate::Kind::DimStyle, PurgeKinds::DimStyles, db.dimStyles());

    HandleSet doomed;
    // Repeated because purging cascades. Bounded by the number of records, so a
    // pathological drawing cannot spin here.
    for (bool changed = true; changed;) {
        changed = false;
        const HandleSet reachable = blocksReachedFromLayouts(db, doomed);
        const HandleSet used = referencesFromSurvivors(db, doomed);

        for (const Table& table : tables) {
            for (const Handle handle : table.index->handles()) {
                if (doomed.count(handle.value()) != 0) {
                    continue;
                }
                const auto* record = db.get<SymbolTableRecord>(handle);
                if (record == nullptr || record->erased) {
                    continue;
                }
                if (isReservedName(table.kind, record->name) ||
                    current.count(handle.value()) != 0) {
                    continue;
                }
                if (table.kind == PurgeCandidate::Kind::Block) {
                    const auto* block = db.get<BlockTableRecord>(handle);
                    // A layout space is not a block a user placed, and an xref
                    // is a reference to another drawing rather than an unused
                    // definition: detaching one is a different decision.
                    if (block == nullptr || block->isLayoutSpace() || block->isXref()) {
                        continue;
                    }
                    if (reachable.count(handle.value()) != 0) {
                        continue;
                    }
                } else if (used.count(handle.value()) != 0) {
                    continue;
                }
                doomed.insert(handle.value());
                changed = true;
            }
        }
    }

    std::vector<PurgeCandidate> candidates;
    for (const Table& table : tables) {
        for (const Handle handle : table.index->handles()) {
            if (doomed.count(handle.value()) == 0) {
                continue;
            }
            const auto* record = db.get<SymbolTableRecord>(handle);
            if (record == nullptr) {
                continue;
            }
            PurgeCandidate candidate;
            candidate.kind = table.kind;
            candidate.handle = handle;
            candidate.name = record->name;
            if (const auto* block = db.get<BlockTableRecord>(handle)) {
                candidate.contents = block->entityHandles.size();
            }
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

std::size_t purge(Transaction& transaction, const std::vector<PurgeCandidate>& candidates) {
    Database& db = transaction.database();
    std::size_t erased = 0;
    for (const PurgeCandidate& candidate : candidates) {
        DbObject* record = db.getObject(candidate.handle);
        if (record == nullptr || record->erased) {
            continue;
        }
        // A block's contents go with it. They are unreachable either way, but
        // leaving them in the store would keep every layer and linetype they
        // name alive for the *next* purge to find, which is exactly the
        // "run it twice" behaviour the cascading sweep exists to avoid.
        if (const auto* block = db.get<BlockTableRecord>(candidate.handle)) {
            const std::vector<Handle> contents = block->entityHandles;
            for (const Handle handle : contents) {
                if (DbObject* owned = db.getObject(handle); owned != nullptr && !owned->erased) {
                    transaction.erase(*owned);
                }
            }
        }
        if (transaction.erase(*record)) {
            ++erased;
        }
    }
    return erased;
}

}  // namespace cad
