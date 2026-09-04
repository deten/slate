#include "cad/db/Database.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_set>

#include "cad/db/InsertEntity.hpp"

namespace cad {
namespace {

/// Metres per one drawing unit, for converting between drawings.
[[nodiscard]] double unitToMeters(DrawingUnits units) noexcept {
    switch (units) {
        case DrawingUnits::Inches:            return 0.0254;
        case DrawingUnits::Feet:              return 0.3048;
        case DrawingUnits::Miles:             return 1609.344;
        case DrawingUnits::Millimeters:       return 0.001;
        case DrawingUnits::Centimeters:       return 0.01;
        case DrawingUnits::Meters:            return 1.0;
        case DrawingUnits::Kilometers:        return 1000.0;
        case DrawingUnits::Microinches:       return 2.54e-8;
        case DrawingUnits::Mils:              return 2.54e-5;
        case DrawingUnits::Yards:             return 0.9144;
        case DrawingUnits::Angstroms:         return 1e-10;
        case DrawingUnits::Nanometers:        return 1e-9;
        case DrawingUnits::Microns:           return 1e-6;
        case DrawingUnits::Decimeters:        return 0.1;
        case DrawingUnits::Decameters:        return 10.0;
        case DrawingUnits::Hectometers:       return 100.0;
        case DrawingUnits::Gigameters:        return 1e9;
        case DrawingUnits::AstronomicalUnits: return 1.495978707e11;
        case DrawingUnits::LightYears:        return 9.4607304725808e15;
        case DrawingUnits::Parsecs:           return 3.0856775814913673e16;
        case DrawingUnits::Unitless:          break;
    }
    return 1.0;
}

}  // namespace

double unitConversionFactor(DrawingUnits from, DrawingUnits to) noexcept {
    if (from == DrawingUnits::Unitless || to == DrawingUnits::Unitless) {
        return 1.0;
    }
    return unitToMeters(from) / unitToMeters(to);
}

Database::Database() = default;
Database::~Database() = default;

std::unique_ptr<Database> Database::createEmpty() {
    auto db = std::make_unique<Database>();

    for (const StandardLinetype& definition : standardLinetypes()) {
        auto linetype = std::make_unique<Linetype>();
        linetype->name = definition.name;
        linetype->description = definition.description;
        for (const double length : definition.pattern) {
            LinetypeElement element;
            element.length = length;
            linetype->elements.push_back(std::move(element));
        }
        db->addLinetype(std::move(linetype));
    }

    auto style = std::make_unique<TextStyle>();
    style->name = kTextStyleStandard;
    const Handle standardTextStyle = db->addTextStyle(std::move(style)).handle;

    auto dimStyle = std::make_unique<DimStyle>();
    dimStyle->name = kDimStyleStandard;
    dimStyle->textStyleHandle = standardTextStyle;
    db->addDimStyle(std::move(dimStyle));

    auto layer0 = std::make_unique<Layer>();
    layer0->name = kLayerZero;
    layer0->color = Color::fromIndex(7);
    layer0->linetypeHandle = db->linetypes_.find(kLinetypeContinuous);
    layer0->lineweight = toRaw(Lineweight::Default);
    db->addLayer(std::move(layer0));

    auto model = std::make_unique<BlockTableRecord>();
    model->name = kModelSpace;
    db->addBlock(std::move(model));

    auto paper = std::make_unique<BlockTableRecord>();
    paper->name = kPaperSpace;
    db->addBlock(std::move(paper));

    return db;
}

// ---------------------------------------------------------------- objects

DbObject& Database::addObject(DbObjectPtr object) {
    if (object == nullptr) {
        throw std::invalid_argument("Cannot add a null object to the database");
    }

    if (object->handle.isNull()) {
        object->handle = allocator_.allocate();
    } else {
        allocator_.observe(object->handle);
        const auto existing = objects_.find(object->handle);
        if (existing != objects_.end() && existing->second.get() != object.get()) {
            throw std::invalid_argument("Handle collision: " + object->handle.toString() +
                                        " is already held by a " +
                                        existing->second->objectType());
        }
    }

    const Handle handle = object->handle;
    DbObject& reference = *object;
    objects_[handle] = std::move(object);

    // A symbol table record joins its name index here, so that adding one is
    // the mirror of erasing one — which has always removed the name. Doing it
    // in this single place is what makes `Transaction::addObject` usable for a
    // layer or a block: routed through the generic path, a record would
    // otherwise exist but be unfindable by name, and absent from the tables
    // written back out on save.
    if (SymbolTableIndex* table = tableFor(reference)) {
        table->add(static_cast<SymbolTableRecord&>(reference).name, handle);
    }

    notifyListeners({DatabaseEvent::Kind::ObjectAdded, handle});
    return reference;
}

DbObject* Database::getObject(Handle handle) {
    if (handle.isNull()) {
        return nullptr;
    }
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : it->second.get();
}

const DbObject* Database::getObject(Handle handle) const {
    if (handle.isNull()) {
        return nullptr;
    }
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : it->second.get();
}

std::vector<Handle> Database::objectHandles() const {
    std::vector<Handle> handles;
    handles.reserve(objects_.size());
    for (const auto& [handle, object] : objects_) {
        handles.push_back(handle);
    }
    std::sort(handles.begin(), handles.end());
    return handles;
}

SymbolTableIndex* Database::tableFor(const DbObject& object) noexcept {
    if (dynamic_cast<const Layer*>(&object) != nullptr) { return &layers_; }
    if (dynamic_cast<const Linetype*>(&object) != nullptr) { return &linetypes_; }
    if (dynamic_cast<const TextStyle*>(&object) != nullptr) { return &textStyles_; }
    if (dynamic_cast<const DimStyle*>(&object) != nullptr) { return &dimStyles_; }
    if (dynamic_cast<const BlockTableRecord*>(&object) != nullptr) { return &blocks_; }
    return nullptr;
}

bool Database::eraseObject(Handle handle) {
    DbObject* object = getObject(handle);
    if (object == nullptr || object->erased) {
        return false;
    }
    object->erased = true;
    // Detach from its owner's ordering so it stops being drawn and iterated.
    if (auto* owner = get<BlockTableRecord>(object->ownerHandle)) {
        owner->removeEntity(handle);
    }
    // A symbol table record must also leave its name index, or the name stays
    // taken by an object that no longer exists — `findLayer` would keep
    // returning an erased layer, and re-creating one with the same name would
    // fail. This matters most for undo: a layer created inside a transaction
    // and then rolled back has to release its name.
    if (SymbolTableIndex* table = tableFor(*object)) {
        table->remove(static_cast<SymbolTableRecord*>(object)->name);
    }
    notifyListeners({DatabaseEvent::Kind::ObjectErased, handle});
    return true;
}

bool Database::uneraseObject(Handle handle, std::size_t index) {
    DbObject* object = getObject(handle);
    if (object == nullptr || !object->erased) {
        return false;
    }
    object->erased = false;

    if (auto* owner = get<BlockTableRecord>(object->ownerHandle)) {
        const auto& handles = owner->entityHandles;
        if (std::find(handles.begin(), handles.end(), handle) == handles.end()) {
            if (index == static_cast<std::size_t>(-1) || index >= handles.size()) {
                owner->appendEntity(handle);
            } else {
                owner->entityHandles.insert(
                    owner->entityHandles.begin() + static_cast<std::ptrdiff_t>(index), handle);
            }
        }
    }
    // Reclaim its name. If something else took the name while it was erased,
    // the record comes back nameless in the index rather than displacing the
    // live one — a collision that undo cannot silently resolve, so it is left
    // for `validate` to report instead of being papered over.
    if (SymbolTableIndex* table = tableFor(*object)) {
        const std::string& name = static_cast<SymbolTableRecord*>(object)->name;
        if (!table->has(name)) {
            table->add(name, handle);
        }
    }
    notifyListeners({DatabaseEvent::Kind::ObjectUnerased, handle});
    return true;
}

void Database::notifyModified(Handle handle) {
    notifyListeners({DatabaseEvent::Kind::ObjectModified, handle});
}

std::size_t Database::purgeErased() {
    std::unordered_set<Handle> referenced;
    for (const auto& [handle, object] : objects_) {
        if (object->erased) {
            continue;
        }
        for (const Handle ref : object->referencedHandles()) {
            referenced.insert(ref);
        }
    }

    std::vector<Handle> doomed;
    for (const auto& [handle, object] : objects_) {
        if (object->erased && !referenced.contains(handle)) {
            doomed.push_back(handle);
        }
    }
    for (const Handle handle : doomed) {
        objects_.erase(handle);
    }
    return doomed.size();
}

// ----------------------------------------------------------- symbol tables

// These five once each repeated "add the object, then index its name". The
// indexing now happens inside addObject, so they are the typed front doors and
// nothing more — and a record added by any other route is indexed too.

Layer& Database::addLayer(std::unique_ptr<Layer> layer) { return add(std::move(layer)); }

Linetype& Database::addLinetype(std::unique_ptr<Linetype> linetype) {
    return add(std::move(linetype));
}

TextStyle& Database::addTextStyle(std::unique_ptr<TextStyle> style) {
    return add(std::move(style));
}

DimStyle& Database::addDimStyle(std::unique_ptr<DimStyle> style) {
    return add(std::move(style));
}

BlockTableRecord& Database::addBlock(std::unique_ptr<BlockTableRecord> block) {
    return add(std::move(block));
}

DimStyle* Database::findDimStyle(std::string_view name) {
    return get<DimStyle>(dimStyles_.find(name));
}

Layer* Database::findLayer(std::string_view name) {
    return get<Layer>(layers_.find(name));
}

Linetype* Database::findLinetype(std::string_view name) {
    return get<Linetype>(linetypes_.find(name));
}

TextStyle* Database::findTextStyle(std::string_view name) {
    return get<TextStyle>(textStyles_.find(name));
}

BlockTableRecord* Database::findBlock(std::string_view name) {
    return get<BlockTableRecord>(blocks_.find(name));
}

Layer& Database::defaultLayer() {
    Layer* layer = findLayer(kLayerZero);
    if (layer == nullptr) {
        throw std::logic_error("Drawing is missing layer 0; the database was not initialized");
    }
    return *layer;
}

BlockTableRecord& Database::modelSpace() {
    BlockTableRecord* block = findBlock(kModelSpace);
    if (block == nullptr) {
        throw std::logic_error("Drawing is missing model space; the database was not initialized");
    }
    return *block;
}

Layer& Database::ensureLayer(std::string_view name) {
    if (Layer* existing = findLayer(name)) {
        return *existing;
    }
    auto layer = std::make_unique<Layer>();
    layer->name = std::string{name};
    layer->linetypeHandle = linetypes_.find(kLinetypeContinuous);
    return addLayer(std::move(layer));
}

Linetype& Database::ensureLinetype(std::string_view name) {
    if (Linetype* existing = findLinetype(name)) {
        return *existing;
    }
    auto linetype = std::make_unique<Linetype>();
    linetype->name = std::string{name};
    return addLinetype(std::move(linetype));
}

TextStyle& Database::ensureTextStyle(std::string_view name) {
    if (TextStyle* existing = findTextStyle(name)) {
        return *existing;
    }
    auto style = std::make_unique<TextStyle>();
    style->name = std::string{name};
    return addTextStyle(std::move(style));
}

DimStyle& Database::ensureDimStyle(std::string_view name) {
    if (DimStyle* existing = findDimStyle(name)) {
        return *existing;
    }
    auto style = std::make_unique<DimStyle>();
    style->name = std::string{name};
    style->textStyleHandle = textStyles_.find(kTextStyleStandard);
    return addDimStyle(std::move(style));
}

const DimStyle& Database::dimStyleFor(const DimensionEntity& dimension) const {
    return dimStyleByHandle(dimension.styleHandle);
}

const DimStyle& Database::dimStyleByHandle(Handle handle) const {
    if (const DimStyle* named = get<DimStyle>(handle)) {
        return *named;
    }
    if (const DimStyle* current = get<DimStyle>(dimStyles_.find(settings.currentDimStyleName))) {
        return *current;
    }
    if (const DimStyle* standard = get<DimStyle>(dimStyles_.find(kDimStyleStandard))) {
        return *standard;
    }
    // A drawing whose DIMSTYLE table is damaged must still display.
    return fallbackDimStyle();
}

DimStyle Database::effectiveDimStyle(const DimensionEntity& dimension) const {
    // `restoreFrom` rather than a copy: a DbObject holds a unique_ptr, so it is
    // deliberately not copyable, and this is the sanctioned way to duplicate
    // one's state. The scratch style is never added to a database, so the
    // handle it does not receive is not missed.
    DimStyle style;
    style.restoreFrom(dimStyleFor(dimension));
    applyDimStyleOverrides(style, dimension);
    return style;
}

// ---------------------------------------------------------------- entities

Entity& Database::addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner) {
    Entity& reference = add(std::move(entity));
    if (reference.layerHandle.isNull()) {
        // The drawing's current layer, which is what a drafter has just chosen
        // in the layer control and expects the next thing they draw to land on.
        // This defaulted to layer 0 regardless, so picking a current layer
        // changed the control and nothing else -- every line, every note and
        // every dimension went to 0 whatever the drawing said.
        Layer* current = findLayer(settings.currentLayerName);
        reference.layerHandle = current != nullptr ? current->handle : defaultLayer().handle;
    }
    reference.ownerHandle = owner.handle;
    owner.appendEntity(reference.handle);
    return reference;
}

Entity& Database::addToModelSpace(std::unique_ptr<Entity> entity) {
    return addEntity(std::move(entity), modelSpace());
}

std::vector<Entity*> Database::entitiesIn(const BlockTableRecord& block) {
    std::vector<Entity*> result;
    result.reserve(block.entityHandles.size());
    for (const Handle handle : block.entityHandles) {
        Entity* entity = getEntity(handle);
        if (entity != nullptr && !entity->erased) {
            result.push_back(entity);
        }
    }
    return result;
}

std::vector<const Entity*> Database::entitiesIn(const BlockTableRecord& block) const {
    std::vector<const Entity*> result;
    result.reserve(block.entityHandles.size());
    for (const Handle handle : block.entityHandles) {
        const Entity* entity = getEntity(handle);
        if (entity != nullptr && !entity->erased) {
            result.push_back(entity);
        }
    }
    return result;
}

// ----------------------------------------------------------------- extents

Box3 Database::blockExtents(const BlockTableRecord& block, int maxDepth) {
    std::vector<Handle> visiting;
    return blockExtentsImpl(block, maxDepth, visiting);
}

Box3 Database::blockExtentsImpl(const BlockTableRecord& block, int maxDepth,
                                std::vector<Handle>& visiting) {
    if (maxDepth <= 0 ||
        std::find(visiting.begin(), visiting.end(), block.handle) != visiting.end()) {
        return Box3::empty();
    }
    visiting.push_back(block.handle);

    Box3 result;
    for (Entity* entity : entitiesIn(block)) {
        // A block reference cannot know its own extents: it needs the
        // definition, which only the database can traverse. Resolve that first
        // so the insert's own bounds are meaningful.
        if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
            if (auto* definition = get<BlockTableRecord>(insert->blockHandle)) {
                insert->setBlockExtents(blockExtentsImpl(*definition, maxDepth - 1, visiting));
            }
        }

        const Layer* layer = get<Layer>(entity->layerHandle);
        // Frozen layers are excluded from extents, matching ZOOM EXTENTS.
        if (layer != nullptr && layer->isFrozen) {
            continue;
        }
        result = result.united(entity->bounds());
    }

    visiting.pop_back();
    return result;
}

Box3 Database::modelExtents() {
    return blockExtents(modelSpace());
}

// -------------------------------------------------------------- properties

ResolutionContext Database::resolutionContextFor(const Entity& entity) const {
    ResolutionContext context;
    const Layer* layer = get<Layer>(entity.layerHandle);
    if (layer == nullptr) {
        layer = get<Layer>(layers_.find(kLayerZero));
    }
    if (layer != nullptr) {
        context.layerColor = layer->color;
        context.layerLinetypeHandle = layer->linetypeHandle;
        context.layerLineweight = layer->lineweight;
        context.layerTransparency = layer->transparency;
        context.layerVisible = layer->isVisible();
    }
    context.globalLinetypeScale = settings.globalLinetypeScale;
    return context;
}

ResolutionContext Database::resolutionContextFor(const Entity& entity,
                                                 const Entity& blockReference) const {
    ResolutionContext context = resolutionContextFor(entity);
    context.insideBlock = true;
    context.blockColor = blockReference.color;
    context.blockLinetypeHandle = blockReference.linetypeHandle;
    context.blockLineweight = blockReference.lineweight;
    return context;
}

// ----------------------------------------------------------------- layouts

std::vector<Layout*> Database::layouts() {
    std::vector<Layout*> found;
    for (const auto& [handle, object] : objects_) {
        if (object->erased) {
            continue;
        }
        if (auto* layout = dynamic_cast<Layout*>(object.get())) {
            found.push_back(layout);
        }
    }
    // Model first whatever its stored order, then by tab order, then by name.
    // The flat object store has no inherent order, so without the name tiebreak
    // the tabs would shuffle between runs on a drawing whose tab orders collide
    // — which real drawings, edited by several people over years, do.
    std::sort(found.begin(), found.end(), [](const Layout* a, const Layout* b) {
        if (a->isModel() != b->isModel()) {
            return a->isModel();
        }
        if (a->tabOrder != b->tabOrder) {
            return a->tabOrder < b->tabOrder;
        }
        return normalizeSymbolName(a->name) < normalizeSymbolName(b->name);
    });
    return found;
}

std::vector<const Layout*> Database::layouts() const {
    std::vector<const Layout*> result;
    for (const Layout* layout : const_cast<Database*>(this)->layouts()) {
        result.push_back(layout);
    }
    return result;
}

Layout* Database::findLayout(std::string_view name) {
    const std::string wanted = normalizeSymbolName(name);
    for (Layout* layout : layouts()) {
        if (normalizeSymbolName(layout->name) == wanted) {
            return layout;
        }
    }
    return nullptr;
}

Layout* Database::layoutForBlock(Handle blockRecord) {
    for (Layout* layout : layouts()) {
        if (layout->blockRecordHandle == blockRecord) {
            return layout;
        }
    }
    return nullptr;
}

BlockTableRecord& Database::currentSpace() {
    if (auto* block = get<BlockTableRecord>(settings.currentSpaceHandle);
        block != nullptr && !block->erased) {
        return *block;
    }
    return modelSpace();
}

std::string Database::nextPaperSpaceName() const {
    std::string name = kPaperSpace;
    for (int i = 0; blocks_.has(name); ++i) {
        name = std::string{kPaperSpace} + std::to_string(i);
    }
    return name;
}

Layout& Database::ensureModelLayout() {
    for (Layout* layout : layouts()) {
        if (layout->isModel()) {
            return *layout;
        }
    }
    // A drawing with no layouts at all is legal — a bare DXF written by a
    // script has none — and the shell still needs a tab to show.
    auto layout = std::make_unique<Layout>();
    layout->name = kLayoutModel;
    layout->blockRecordHandle = modelSpace().handle;
    layout->tabOrder = 0;
    return add(std::move(layout));
}

// ------------------------------------------------------------------- xrefs

std::vector<BlockTableRecord*> Database::externalReferences() {
    std::vector<BlockTableRecord*> result;
    for (const Handle handle : blocks_.handles()) {
        auto* block = get<BlockTableRecord>(handle);
        if (block != nullptr && block->isXref()) {
            result.push_back(block);
        }
    }
    return result;
}

std::vector<BlockTableRecord*> Database::unresolvedReferences() {
    std::vector<BlockTableRecord*> result;
    for (BlockTableRecord* block : externalReferences()) {
        if (block->xrefStatus == XrefStatus::NotFound ||
            block->xrefStatus == XrefStatus::Unresolved) {
            result.push_back(block);
        }
    }
    return result;
}

// ------------------------------------------------------------------ events

std::size_t Database::addListener(DatabaseListener listener) {
    const std::size_t id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void Database::removeListener(std::size_t id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void Database::notifyListeners(const DatabaseEvent& event) {
    if (bulkLoading_ && event.kind != DatabaseEvent::Kind::Reset) {
        return;
    }
    // Iterate a copy: a listener may add or remove listeners in response.
    const auto snapshot = listeners_;
    for (const auto& [id, listener] : snapshot) {
        listener(event);
    }
}

// ------------------------------------------------------------- diagnostics

std::vector<DatabaseIssue> Database::validate() const {
    std::vector<DatabaseIssue> issues;

    for (const auto& [handle, object] : objects_) {
        if (object->erased) {
            continue;
        }
        for (const Handle ref : object->referencedHandles()) {
            if (ref.isNull()) {
                continue;
            }
            if (!objects_.contains(ref)) {
                issues.push_back({DatabaseIssue::Severity::Error, handle,
                                  object->objectType() + " " + handle.toString() +
                                      " references missing object " + ref.toString()});
            }
        }
        if (const auto* entity = dynamic_cast<const Entity*>(object.get())) {
            if (entity->layerHandle.isNull()) {
                issues.push_back({DatabaseIssue::Severity::Warning, handle,
                                  object->objectType() + " " + handle.toString() +
                                      " has no layer assigned"});
            }
        }
    }
    return issues;
}

}  // namespace cad
