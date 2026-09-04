#include "cad/io/XrefLoader.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <unordered_set>

#include "cad/db/InsertEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"

namespace cad::io {
namespace {

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string toLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

}  // namespace

bool isDwgPath(const std::filesystem::path& path) {
    return toLower(path.extension().string()) == ".dwg";
}

XrefLoadResult XrefLoader::loadAll(const std::filesystem::path& hostPath,
                                   const XrefLoadOptions& options) {
    ResolverOptions resolverOptions;
    resolverOptions.hostDirectory = std::filesystem::absolute(hostPath).parent_path();
    resolverOptions.searchPaths = options.searchPaths;
    ResourceResolver resolver{std::move(resolverOptions)};

    if (!hostPath.empty()) {
        loading_.insert(std::filesystem::absolute(hostPath).lexically_normal());
    }

    loadInto(host_, resolver, 0, options);

    XrefLoadResult result;
    result.nodes = std::move(nodes_);
    result.diagnostics = std::move(diagnostics_);
    for (const XrefNode& node : result.nodes) {
        if (node.status == XrefStatus::Resolved) {
            ++result.resolvedCount;
        } else {
            ++result.failedCount;
        }
    }
    return result;
}

void XrefLoader::loadInto(Database& database, ResourceResolver& resolver, int depth,
                          const XrefLoadOptions& options) {
    if (depth > options.maxDepth) {
        diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                "External reference nesting exceeded " +
                                    std::to_string(options.maxDepth) +
                                    " levels; deeper references were not loaded.",
                                0});
        return;
    }

    for (BlockTableRecord* block : database.externalReferences()) {
        // An overlay nested inside another xref is deliberately not loaded —
        // that is exactly what distinguishes an overlay from an attachment, and
        // how users stop site plans multiplying through a drawing set.
        if (block->xrefType == XrefType::Overlay && depth > 0 && options.skipNestedOverlays) {
            XrefNode node;
            node.blockHandle = block->handle;
            node.name = block->name;
            node.storedPath = block->xrefPath;
            node.status = XrefStatus::Unloaded;
            node.type = block->xrefType;
            node.depth = depth;
            node.problem =
                "Overlay references are not loaded when nested inside another reference.";
            nodes_.push_back(std::move(node));
            continue;
        }
        loadOne(*block, resolver, depth, options);
    }
}

void XrefLoader::loadOne(BlockTableRecord& block, ResourceResolver& resolver, int depth,
                         const XrefLoadOptions& options) {
    if (options.onProgress) {
        options.onProgress("Resolving reference " + block.name);
    }

    XrefNode node;
    node.blockHandle = block.handle;
    node.name = block.name;
    node.storedPath = block.xrefPath;
    node.type = block.xrefType;
    node.depth = depth;

    const ResolutionResult resolution = resolver.resolve(block.xrefPath, ResourceKind::Drawing);
    if (!resolution.found) {
        block.xrefStatus = XrefStatus::NotFound;
        node.status = XrefStatus::NotFound;
        node.problem = "File not found. Searched " +
                       std::to_string(resolution.attempted.size()) + " location(s).";
        node.attemptedPaths = resolution.attempted;
        diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                "External reference \"" + block.name +
                                    "\" not found: " + block.xrefPath,
                                0});
        nodes_.push_back(std::move(node));
        return;
    }

    const std::filesystem::path absolute = resolution.path.lexically_normal();
    node.resolvedPath = absolute;

    // Cycle: this file is already being loaded further up the chain.
    if (loading_.contains(absolute)) {
        block.xrefStatus = XrefStatus::Circular;
        node.status = XrefStatus::Circular;
        node.problem = "This reference forms a cycle and was skipped to avoid infinite recursion.";
        diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                "Circular external reference detected at \"" + block.name +
                                    "\" (" + absolute.string() + "); it was not loaded again.",
                                0});
        nodes_.push_back(std::move(node));
        return;
    }

    DxfReadResult referenced;
    try {
        referenced = readDrawing(absolute, options);
    } catch (const std::exception& error) {
        block.xrefStatus = XrefStatus::Unresolved;
        node.status = XrefStatus::Unresolved;
        node.problem = error.what();
        diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                "External reference \"" + block.name +
                                    "\" could not be read: " + error.what(),
                                0});
        nodes_.push_back(std::move(node));
        return;
    }

    for (const DxfDiagnostic& diagnostic : referenced.diagnostics) {
        diagnostics_.push_back(
            {diagnostic.severity, "[" + block.name + "] " + diagnostic.message, diagnostic.line});
    }

    loading_.insert(absolute);

    // Load the reference's own references first, so that when its content is
    // merged the nested geometry is already present.
    ResourceResolver nestedResolver = resolver.forNestedDrawing(absolute);
    XrefLoader nestedLoader{*referenced.database};
    nestedLoader.loading_ = loading_;
    nestedLoader.loadInto(*referenced.database, nestedResolver, depth + 1, options);

    diagnostics_.insert(diagnostics_.end(), nestedLoader.diagnostics_.begin(),
                        nestedLoader.diagnostics_.end());
    nodes_.insert(nodes_.end(), nestedLoader.nodes_.begin(), nestedLoader.nodes_.end());

    node.entityCount = mergeInto(block, *referenced.database);

    block.xrefStatus = XrefStatus::Resolved;
    block.resolvedPath = absolute.string();
    node.status = XrefStatus::Resolved;

    if (resolution.source == ResolutionSource::FilenameFallback) {
        diagnostics_.push_back({DxfDiagnostic::Severity::Info,
                                "External reference \"" + block.name + "\" was found by "
                                "filename at " + absolute.string() + "; its stored path (" +
                                    block.xrefPath + ") is out of date.",
                                0});
    }

    loading_.erase(absolute);
    nodes_.push_back(std::move(node));
}

DxfReadResult XrefLoader::readDrawing(const std::filesystem::path& path,
                                      const XrefLoadOptions& options) const {
    std::string dxfText;
    if (isDwgPath(path)) {
        if (options.translator == nullptr) {
            throw std::runtime_error(
                "This reference is a DWG file and no DWG translator is configured. "
                "Install the ODA File Converter to load DWG references.");
        }
        dxfText = options.translator->toDxf(path);
    } else {
        dxfText = readTextFile(path);
    }

    DxfReadResult result = readDxf(dxfText);
    result.database->filePath = path.string();
    return result;
}

std::size_t XrefLoader::mergeInto(BlockTableRecord& block, Database& source) {
    const std::string prefix = block.name;

    std::unordered_map<Handle, Handle> layerMap;
    std::unordered_map<Handle, Handle> linetypeMap;
    std::unordered_map<Handle, Handle> styleMap;
    std::unordered_map<Handle, Handle> blockMap;

    // Symbol records are renamed to `xrefname|recordname`, other CAD software's
    // convention. It is why a reference's layers appear in the host's layer
    // manager as a distinguishable group rather than merging into — and
    // silently changing — the host's own.
    for (const Handle handle : source.linetypes().handles()) {
        const auto* sourceLinetype = source.get<Linetype>(handle);
        if (sourceLinetype == nullptr) {
            continue;
        }
        auto copy = std::make_unique<Linetype>();
        copy->name = host_.linetypes().uniqueName(prefix + "|" + sourceLinetype->name);
        copy->description = sourceLinetype->description;
        copy->elements = sourceLinetype->elements;
        linetypeMap[handle] = host_.addLinetype(std::move(copy)).handle;
    }

    for (const Handle handle : source.textStyles().handles()) {
        const auto* sourceStyle = source.get<TextStyle>(handle);
        if (sourceStyle == nullptr) {
            continue;
        }
        DbObjectPtr cloned = sourceStyle->clone();
        auto* copy = dynamic_cast<TextStyle*>(cloned.get());
        if (copy == nullptr) {
            continue;
        }
        cloned.release();
        std::unique_ptr<TextStyle> owned{copy};
        owned->handle = Handle::null();
        owned->name = host_.textStyles().uniqueName(prefix + "|" + sourceStyle->name);
        styleMap[handle] = host_.addTextStyle(std::move(owned)).handle;
    }

    for (const Handle handle : source.layers().handles()) {
        const auto* sourceLayer = source.get<Layer>(handle);
        if (sourceLayer == nullptr) {
            continue;
        }
        DbObjectPtr cloned = sourceLayer->clone();
        auto* copy = dynamic_cast<Layer*>(cloned.get());
        if (copy == nullptr) {
            continue;
        }
        cloned.release();
        std::unique_ptr<Layer> owned{copy};
        owned->handle = Handle::null();
        owned->name = host_.layers().uniqueName(prefix + "|" + sourceLayer->name);
        owned->isXrefDependent = true;

        const auto mapped = linetypeMap.find(sourceLayer->linetypeHandle);
        owned->linetypeHandle = mapped != linetypeMap.end()
                                    ? mapped->second
                                    : host_.defaultLayer().linetypeHandle;
        layerMap[handle] = host_.addLayer(std::move(owned)).handle;
    }

    // Block definitions used inside the reference come across too, otherwise
    // its INSERTs would point at nothing.
    for (const Handle handle : source.blocks().handles()) {
        const auto* sourceBlock = source.get<BlockTableRecord>(handle);
        if (sourceBlock == nullptr || sourceBlock->isLayoutSpace()) {
            continue;
        }
        auto copy = std::make_unique<BlockTableRecord>();
        copy->name = host_.blocks().uniqueName(prefix + "|" + sourceBlock->name);
        copy->origin = sourceBlock->origin;
        copy->isXrefDependent = true;
        blockMap[handle] = host_.addBlock(std::move(copy)).handle;
    }

    const auto remap = [&](Entity& entity) {
        const auto layer = layerMap.find(entity.layerHandle);
        entity.layerHandle =
            layer != layerMap.end() ? layer->second : host_.defaultLayer().handle;

        if (const auto linetype = linetypeMap.find(entity.linetypeHandle);
            linetype != linetypeMap.end()) {
            entity.linetypeHandle = linetype->second;
        }
        if (auto* text = dynamic_cast<TextEntity*>(&entity)) {
            if (const auto style = styleMap.find(text->styleHandle); style != styleMap.end()) {
                text->styleHandle = style->second;
            }
        }
        if (auto* mtext = dynamic_cast<MTextEntity*>(&entity)) {
            if (const auto style = styleMap.find(mtext->styleHandle); style != styleMap.end()) {
                mtext->styleHandle = style->second;
            }
        }
        if (auto* insert = dynamic_cast<InsertEntity*>(&entity)) {
            if (const auto target = blockMap.find(insert->blockHandle);
                target != blockMap.end()) {
                insert->blockHandle = target->second;
            }
        }
    };

    const auto copyEntities = [&](const BlockTableRecord& from, BlockTableRecord& into) {
        std::size_t count = 0;
        for (const Entity* entity : source.entitiesIn(from)) {
            DbObjectPtr cloned = entity->clone();
            auto* copy = dynamic_cast<Entity*>(cloned.get());
            if (copy == nullptr) {
                continue;
            }
            cloned.release();
            std::unique_ptr<Entity> owned{copy};
            owned->handle = Handle::null();
            remap(*owned);
            host_.addEntity(std::move(owned), into);
            ++count;
        }
        return count;
    };

    std::size_t total = 0;

    for (const Handle handle : source.blocks().handles()) {
        const auto* sourceBlock = source.get<BlockTableRecord>(handle);
        if (sourceBlock == nullptr || sourceBlock->isLayoutSpace()) {
            continue;
        }
        const auto target = blockMap.find(handle);
        if (target == blockMap.end()) {
            continue;
        }
        if (auto* into = host_.get<BlockTableRecord>(target->second)) {
            total += copyEntities(*sourceBlock, *into);
        }
    }

    // Model space goes into the xref's own block record. Paper space is not
    // copied: an xref contributes model geometry, never the sheets it was laid
    // out on.
    total += copyEntities(source.modelSpace(), block);
    return total;
}

namespace {

/// Block records something in a layout eventually draws.
///
/// A block definition is not a drawing of anything: it is drawn only where an
/// INSERT puts it, and a block nothing inserts is inert no matter what it
/// contains. Seeded with the layout spaces and followed through nested inserts,
/// with a visited set because a file whose blocks reference each other in a
/// cycle must not hang this.
[[nodiscard]] std::unordered_set<std::uint64_t> blocksReachedFromLayouts(const Database& db) {
    std::unordered_set<std::uint64_t> reached;
    std::vector<Handle> pending;

    const auto seed = [&](Handle handle) {
        if (!handle.isNull() && reached.insert(handle.value()).second) {
            pending.push_back(handle);
        }
    };

    for (const Layout* layout : db.layouts()) {
        if (layout != nullptr) {
            seed(layout->blockRecordHandle);
        }
    }
    // Model space by name as well as through the layouts. A DXF with no LAYOUT
    // objects at all is legal -- a bare file written by a script -- and without
    // this every entity in it would be reported as drawn nowhere, which is a
    // false alarm about the one space that is always drawn.
    seed(db.blocks().find("*Model_Space"));

    while (!pending.empty()) {
        const Handle current = pending.back();
        pending.pop_back();
        const auto* block = db.get<BlockTableRecord>(current);
        if (block == nullptr) {
            continue;
        }
        for (const Entity* entity : db.entitiesIn(*block)) {
            const auto* insert = dynamic_cast<const InsertEntity*>(entity);
            if (insert == nullptr) {
                continue;
            }
            if (reached.insert(insert->blockHandle.value()).second) {
                pending.push_back(insert->blockHandle);
            }
        }
    }
    return reached;
}

/// What places each definition, and how much of that is ever drawn.
struct PlacementTally {
    std::size_t total = 0;
    std::size_t drawn = 0;
    /// Names of the blocks holding the placements that are not drawn.
    std::vector<std::string> unplacedBlocks;
};

/// How many entities place each definition, counted in one pass.
[[nodiscard]] std::unordered_map<std::uint64_t, PlacementTally> placementCounts(
    const Database& db) {
    const std::unordered_set<std::uint64_t> reached = blocksReachedFromLayouts(db);
    std::unordered_map<std::uint64_t, PlacementTally> counts;

    const auto tally = [&](const Entity& entity, Handle definition) {
        PlacementTally& entry = counts[definition.value()];
        ++entry.total;
        if (reached.count(entity.ownerHandle.value()) != 0) {
            ++entry.drawn;
            return;
        }
        // Named rather than merely counted: "it is in a block called Binder2
        // that nothing inserts" is something a user can act on, and "0 drawn"
        // on its own is not.
        const auto* owner = db.get<BlockTableRecord>(entity.ownerHandle);
        std::string name = owner == nullptr ? std::string{"(unknown)"} : owner->name;
        if (std::find(entry.unplacedBlocks.begin(), entry.unplacedBlocks.end(), name) ==
            entry.unplacedBlocks.end()) {
            entry.unplacedBlocks.push_back(std::move(name));
        }
    };

    for (const Handle handle : db.objectHandles()) {
        const DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased) {
            continue;
        }
        if (const auto* image = dynamic_cast<const RasterImageEntity*>(object)) {
            tally(*image, image->definitionHandle);
        } else if (const auto* underlay = dynamic_cast<const UnderlayEntity*>(object)) {
            tally(*underlay, underlay->definitionHandle);
        }
    }
    return counts;
}

}  // namespace

std::vector<AttachedFileNode> attachedFiles(const Database& db) {
    const std::unordered_map<std::uint64_t, PlacementTally> counts = placementCounts(db);
    std::vector<AttachedFileNode> nodes;

    for (const Handle handle : db.objectHandles()) {
        const DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased) {
            continue;
        }

        AttachedFileNode node;
        node.definitionHandle = handle;
        if (const auto* image = dynamic_cast<const ImageDefinition*>(object)) {
            node.kind = "Image";
            node.storedPath = image->fileName;
            node.resolvedPath = image->resolvedPath;
            node.problem = image->resolutionProblem;
        } else if (const auto* underlay = dynamic_cast<const UnderlayDefinition*>(object)) {
            node.kind = std::string{underlayKindName(underlay->kind)};
            node.storedPath = underlay->fileName;
            node.resolvedPath = underlay->resolvedPath;
            node.problem = underlay->resolutionProblem;
        } else {
            continue;
        }
        node.found = !node.resolvedPath.empty();
        if (const auto count = counts.find(handle.value()); count != counts.end()) {
            node.placements = count->second.total;
            node.drawnPlacements = count->second.drawn;
            node.unplacedBlocks = count->second.unplacedBlocks;
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

bool repathAttachedFile(Database& db, Handle definition, const std::filesystem::path& file) {
    // Relative to the drawing when the new file is inside its folder, absolute
    // otherwise. A project folder that is copied to another machine keeps
    // working; a file genuinely somewhere else is named where it is.
    std::string stored = file.string();
    if (!db.filePath.empty()) {
        std::error_code error;
        const std::filesystem::path host =
            std::filesystem::path{db.filePath}.parent_path();
        const std::filesystem::path relative = std::filesystem::relative(file, host, error);
        // `..` at the front means the file is outside the drawing's folder, so
        // a relative path would be longer *and* more fragile than an absolute
        // one.
        const std::string text = relative.string();
        if (!error && !text.empty() && text.rfind("..", 0) != 0) {
            stored = (std::filesystem::path{"."} / relative).string();
        }
    }

    DbObject* object = db.getObject(definition);
    if (auto* image = dynamic_cast<ImageDefinition*>(object)) {
        image->fileName = stored;
        image->resolvedPath.clear();
        image->resolutionProblem.clear();
        return true;
    }
    if (auto* underlay = dynamic_cast<UnderlayDefinition*>(object)) {
        underlay->fileName = stored;
        underlay->resolvedPath.clear();
        underlay->resolutionProblem.clear();
        return true;
    }
    return false;
}

std::vector<AttachedFileNode> resolveAttachedFiles(Database& db,
                                                   const std::filesystem::path& hostPath,
                                                   const XrefLoadOptions& options) {
    ResolverOptions resolverOptions;
    resolverOptions.hostDirectory = hostPath.parent_path();
    resolverOptions.searchPaths = options.searchPaths;
    ResourceResolver resolver{resolverOptions};

    // A user deciding whether a broken path is worth chasing wants to know
    // whether it is one photograph or forty sheets' worth.
    const std::unordered_map<std::uint64_t, PlacementTally> placements = placementCounts(db);

    const auto record = [&](Handle handle, std::string kind, const std::string& storedPath,
                            std::string& resolvedOut, std::string& problemOut) {
        AttachedFileNode node;
        node.definitionHandle = handle;
        node.kind = std::move(kind);
        node.storedPath = storedPath;
        if (const auto count = placements.find(handle.value()); count != placements.end()) {
            node.placements = count->second.total;
            node.drawnPlacements = count->second.drawn;
            node.unplacedBlocks = count->second.unplacedBlocks;
        }

        if (storedPath.empty()) {
            node.problem = "No file name is stored";
            problemOut = node.problem;
            return node;
        }

        const ResolutionResult resolution = resolver.resolve(storedPath, ResourceKind::Image);
        node.found = resolution.found;
        node.resolvedPath = resolution.path;
        if (resolution.found) {
            resolvedOut = resolution.path.string();
            problemOut.clear();
        } else {
            node.problem = "Not found: " + storedPath;
            resolvedOut.clear();
            problemOut = node.problem;
        }
        return node;
    };

    std::vector<AttachedFileNode> nodes;
    for (const Handle handle : db.objectHandles()) {
        DbObject* object = db.getObject(handle);
        if (object == nullptr || object->erased) {
            continue;
        }
        if (auto* image = dynamic_cast<ImageDefinition*>(object)) {
            nodes.push_back(record(handle, "Image", image->fileName, image->resolvedPath,
                                   image->resolutionProblem));
        } else if (auto* underlay = dynamic_cast<UnderlayDefinition*>(object)) {
            nodes.push_back(record(handle, std::string{underlayKindName(underlay->kind)},
                                   underlay->fileName, underlay->resolvedPath,
                                   underlay->resolutionProblem));
        }
    }
    return nodes;
}

DrawingLoadResult loadDrawingWithReferences(const std::filesystem::path& path,
                                            const XrefLoadOptions& options) {
    std::string dxfText;
    if (isDwgPath(path)) {
        if (options.translator == nullptr) {
            throw std::runtime_error("Opening a DWG requires a configured translator.");
        }
        dxfText = options.translator->toDxf(path);
    } else {
        dxfText = readTextFile(path);
    }

    DxfReadResult read = readDxf(dxfText);
    read.database->filePath = std::filesystem::absolute(path).string();

    XrefLoader loader{*read.database};
    XrefLoadResult xrefs = loader.loadAll(path, options);

    DrawingLoadResult result;
    result.diagnostics = std::move(read.diagnostics);
    result.diagnostics.insert(result.diagnostics.end(), xrefs.diagnostics.begin(),
                              xrefs.diagnostics.end());
    result.xrefs = std::move(xrefs);
    // After the xrefs, so that images attached inside a reference are resolved
    // too — they arrive with it and would otherwise never be looked for.
    result.attachments = resolveAttachedFiles(*read.database, path, options);
    result.database = std::move(read.database);
    return result;
}

}  // namespace cad::io
