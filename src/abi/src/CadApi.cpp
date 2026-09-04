/// The function table handed to plugins.
///
/// Every function here is an entry point from foreign code, so all of them
/// share the same shape: validate the arguments, unwrap the opaque handles,
/// do the work inside `guard` so no exception can escape, and return a status.
/// The repetition is deliberate — a clever dispatcher would save lines here and
/// cost clarity at exactly the boundary where clarity is worth most.

#include <cmath>

#include "AbiInternal.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/geom/Intersect.hpp"

namespace cad::abi {
namespace {

using detail::fail;
using detail::guard;
using detail::toCadPoint;
using detail::toVec3;

// ---------------------------------------------------------------- unwrapping

[[nodiscard]] HostDocument* asDocument(CadDocument* document) noexcept {
    return reinterpret_cast<HostDocument*>(document);
}

[[nodiscard]] CadDocument* asCadDocument(HostDocument* document) noexcept {
    return reinterpret_cast<CadDocument*>(document);
}

[[nodiscard]] TransactionBinding* asTransaction(CadTransaction* transaction) noexcept {
    return reinterpret_cast<TransactionBinding*>(transaction);
}

[[nodiscard]] detail::CommandContextBinding* asContext(CadCommandContext* context) noexcept {
    return reinterpret_cast<detail::CommandContextBinding*>(context);
}

[[nodiscard]] PluginHost* asHost(CadPluginHost* host) noexcept {
    return reinterpret_cast<PluginHost*>(host);
}

/// Unwrap a transaction and confirm it is usable for writing.
///
/// Returns null and records the reason when it is not, so every mutating call
/// can start with two lines rather than five.
[[nodiscard]] Transaction* writableTransaction(CadTransaction* handle) {
    TransactionBinding* binding = asTransaction(handle);
    if (binding == nullptr || binding->transaction == nullptr) {
        fail(CAD_ERROR_NO_TRANSACTION, "a transaction is required for this operation");
        return nullptr;
    }
    if (!binding->transaction->isOpen()) {
        fail(CAD_ERROR_NO_TRANSACTION, "the transaction is already committed or aborted");
        return nullptr;
    }
    return binding->transaction;
}

/// The block record an entity should be added to. A null handle means model
/// space, which is what a plugin that does not care about layouts wants.
[[nodiscard]] BlockTableRecord* resolveBlock(Database& db, CadHandle block) {
    if (block == CAD_NULL_HANDLE) { return &db.modelSpace(); }
    BlockTableRecord* record = db.get<BlockTableRecord>(Handle{block});
    if (record == nullptr) { fail(CAD_ERROR_NOT_FOUND, "no such block record"); }
    return record;
}

// ------------------------------------------------------------- properties

/// Apply the properties a plugin supplied to a freshly built entity.
///
/// Null means "accept the defaults", which for a new entity is layer 0 and
/// ByLayer everything — the same result as drawing with the built-in commands
/// on a default drawing.
[[nodiscard]] CadStatus applyProperties(Database& db, Entity& entity,
                                        const CadEntityProperties* properties) {
    if (properties == nullptr) { return CAD_OK; }
    const std::uint32_t size = properties->struct_size;
    if (size == 0) {
        return fail(CAD_ERROR_INVALID_ARGUMENT,
                    "CadEntityProperties.struct_size must be set");
    }

    if (CAD_ABI_HAS(size, CadEntityProperties, layer) && properties->layer != CAD_NULL_HANDLE) {
        const Handle layerHandle{properties->layer};
        if (db.get<Layer>(layerHandle) == nullptr) {
            return fail(CAD_ERROR_NOT_FOUND, "no such layer");
        }
        entity.layerHandle = layerHandle;
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, color)) {
        entity.color = detail::toColor(properties->color);
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, linetype) &&
        properties->linetype != CAD_NULL_HANDLE) {
        const Handle linetypeHandle{properties->linetype};
        if (db.get<Linetype>(linetypeHandle) == nullptr) {
            return fail(CAD_ERROR_NOT_FOUND, "no such linetype");
        }
        entity.linetypeHandle = linetypeHandle;
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, linetype_scale) &&
        properties->linetype_scale > 0.0) {
        // Zero is what a zero-initialized struct carries, and a zero linetype
        // scale would make dashes vanish. Treated as "unset" rather than
        // faithfully applied.
        entity.linetypeScale = properties->linetype_scale;
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, lineweight) && properties->lineweight != 0) {
        entity.lineweight = static_cast<std::int16_t>(properties->lineweight);
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, visible)) {
        entity.visible = properties->visible != 0;
    }
    return CAD_OK;
}

/// Add a fully built entity and report its handle.
[[nodiscard]] CadStatus finishEntity(Transaction& transaction, CadHandle block,
                                     std::unique_ptr<Entity> entity,
                                     const CadEntityProperties* properties,
                                     CadHandle* outEntity) {
    Database& db = transaction.database();
    BlockTableRecord* owner = resolveBlock(db, block);
    if (owner == nullptr) { return CAD_ERROR_NOT_FOUND; }

    const CadStatus status = applyProperties(db, *entity, properties);
    if (status != CAD_OK) { return status; }

    const Entity& added = transaction.addEntity(std::move(entity), *owner);
    if (outEntity != nullptr) { *outEntity = added.handle.value(); }
    return CAD_OK;
}

// ------------------------------------------------------------- diagnostics

const char* CAD_CALL apiLastError() {
    return detail::errorSlot().c_str();
}

// ---------------------------------------------------------------- documents

CadDocument* CAD_CALL apiHostDocument(CadPluginHost* host) {
    PluginHost* plugin = asHost(host);
    return plugin == nullptr ? nullptr : asCadDocument(&plugin->document());
}

CadStatus CAD_CALL apiDocumentExtents(CadDocument* document, CadBox* outExtents) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outExtents == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_extents are required");
    }
    return guard([&] {
        *outExtents = detail::toCadBox(host->db().modelExtents());
        return CAD_OK;
    });
}

// ------------------------------------------------------------------- layers

CadHandle CAD_CALL apiFindLayer(CadDocument* document, const char* name) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || name == nullptr) { return CAD_NULL_HANDLE; }
    const Layer* layer = host->db().findLayer(name);
    return layer == nullptr ? CAD_NULL_HANDLE : layer->handle.value();
}

CadStatus CAD_CALL apiEnsureLayer(CadTransaction* transaction, const char* name,
                                  CadHandle* outLayer) {
    if (name == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "layer name is required"); }
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    return guard([&] {
        Database& db = tx->database();
        if (Layer* existing = db.findLayer(name); existing != nullptr) {
            if (outLayer != nullptr) { *outLayer = existing->handle.value(); }
            return CAD_OK;
        }
        // Created through the transaction so it lands on the undo stack with
        // the rest of the command's work; `Database::ensureLayer` would create
        // it behind undo's back.
        auto layer = std::make_unique<Layer>();
        layer->name = name;
        layer->linetypeHandle = db.linetypes().find(kLinetypeContinuous);
        Layer& added = static_cast<Layer&>(tx->addObject(std::move(layer)));
        db.layersMutable().add(added.name, added.handle);
        if (outLayer != nullptr) { *outLayer = added.handle.value(); }
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiLayerName(CadDocument* document, CadHandle layer, char* buffer,
                                size_t capacity, size_t* outRequired) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        const Layer* record = host->db().get<Layer>(Handle{layer});
        if (record == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such layer"); }
        return detail::copyString(record->name, buffer, capacity, outRequired);
    });
}

CadStatus CAD_CALL apiLayerColor(CadDocument* document, CadHandle layer, CadColor* outColor) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outColor == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_color are required");
    }
    const Layer* record = host->db().get<Layer>(Handle{layer});
    if (record == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such layer"); }
    *outColor = detail::toCadColor(record->color);
    return CAD_OK;
}

CadStatus CAD_CALL apiSetLayerColor(CadTransaction* transaction, CadHandle layer,
                                    CadColor color) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    return guard([&] {
        Layer* record = tx->database().get<Layer>(Handle{layer});
        if (record == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such layer"); }
        tx->openForWrite(*record).color = detail::toColor(color);
        tx->database().notifyModified(record->handle);
        return CAD_OK;
    });
}

uint32_t CAD_CALL apiLayerCount(CadDocument* document) {
    HostDocument* host = asDocument(document);
    return host == nullptr ? 0u : static_cast<std::uint32_t>(host->db().layers().size());
}

CadStatus CAD_CALL apiLayerAt(CadDocument* document, uint32_t index, CadHandle* outLayer) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outLayer == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_layer are required");
    }
    const std::vector<Handle>& handles = host->db().layers().handles();
    if (index >= handles.size()) { return fail(CAD_ERROR_NOT_FOUND, "layer index out of range"); }
    *outLayer = handles[index].value();
    return CAD_OK;
}

// ------------------------------------------------------------------- blocks

CadHandle CAD_CALL apiFindBlock(CadDocument* document, const char* name) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || name == nullptr) { return CAD_NULL_HANDLE; }
    const BlockTableRecord* block = host->db().findBlock(name);
    return block == nullptr ? CAD_NULL_HANDLE : block->handle.value();
}

CadHandle CAD_CALL apiModelSpace(CadDocument* document) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return CAD_NULL_HANDLE; }
    try {
        return host->db().modelSpace().handle.value();
    } catch (const std::exception&) {
        return CAD_NULL_HANDLE;
    }
}

// ------------------------------------------------------------ entity queries

int32_t CAD_CALL apiEntityKind(CadDocument* document, CadHandle entity) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return CAD_ENTITY_UNKNOWN; }
    const Entity* object = host->db().getEntity(Handle{entity});
    if (object == nullptr) { return CAD_ENTITY_UNKNOWN; }

    // Ordered most-derived first: the attribute types derive from TextEntity,
    // so testing TextEntity earlier would swallow them.
    if (dynamic_cast<const LineEntity*>(object) != nullptr) { return CAD_ENTITY_LINE; }
    if (dynamic_cast<const CircleEntity*>(object) != nullptr) { return CAD_ENTITY_CIRCLE; }
    if (dynamic_cast<const ArcEntity*>(object) != nullptr) { return CAD_ENTITY_ARC; }
    if (dynamic_cast<const EllipseEntity*>(object) != nullptr) { return CAD_ENTITY_ELLIPSE; }
    if (dynamic_cast<const PolylineEntity*>(object) != nullptr) { return CAD_ENTITY_POLYLINE; }
    if (dynamic_cast<const PointEntity*>(object) != nullptr) { return CAD_ENTITY_POINT; }
    if (dynamic_cast<const MTextEntity*>(object) != nullptr) { return CAD_ENTITY_MTEXT; }
    if (dynamic_cast<const TextEntity*>(object) != nullptr) { return CAD_ENTITY_TEXT; }
    if (object->objectType() == "Insert") { return CAD_ENTITY_INSERT; }
    return CAD_ENTITY_UNKNOWN;
}

CadStatus CAD_CALL apiEntityBounds(CadDocument* document, CadHandle entity, CadBox* outBounds) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outBounds == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_bounds are required");
    }
    return guard([&] {
        const Entity* object = host->db().getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }
        *outBounds = detail::toCadBox(object->bounds());
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiEntityProperties(CadDocument* document, CadHandle entity,
                                       CadEntityProperties* outProperties) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outProperties == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_properties are required");
    }
    const Entity* object = host->db().getEntity(Handle{entity});
    if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }

    // Filled to the size the caller declared, so a plugin built against an
    // older header never has fields written past the end of its struct.
    const std::uint32_t size = outProperties->struct_size;
    if (size == 0) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "CadEntityProperties.struct_size must be set");
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, layer)) {
        outProperties->layer = object->layerHandle.value();
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, color)) {
        outProperties->color = detail::toCadColor(object->color);
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, linetype)) {
        outProperties->linetype = object->linetypeHandle.value();
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, linetype_scale)) {
        outProperties->linetype_scale = object->linetypeScale;
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, lineweight)) {
        outProperties->lineweight = object->lineweight;
    }
    if (CAD_ABI_HAS(size, CadEntityProperties, visible)) {
        outProperties->visible = object->visible ? 1 : 0;
    }
    return CAD_OK;
}

CadStatus CAD_CALL apiSetEntityProperties(CadTransaction* transaction, CadHandle entity,
                                          const CadEntityProperties* properties) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (properties == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "properties are required");
    }
    return guard([&] {
        Database& db = tx->database();
        Entity* object = db.getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }

        CadStatus status = CAD_OK;
        tx->modifyEntity(*object, [&](Entity& target) {
            status = applyProperties(db, target, properties);
        });
        return status;
    });
}

CadStatus CAD_CALL apiBlockEntities(CadDocument* document, CadHandle block,
                                    CadHandle* outEntities, uint32_t capacity,
                                    uint32_t* outCount) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        Database& db = host->db();
        BlockTableRecord* record = resolveBlock(db, block);
        if (record == nullptr) { return CAD_ERROR_NOT_FOUND; }
        std::vector<Handle> handles;
        for (const Entity* child : db.entitiesIn(*record)) { handles.push_back(child->handle); }
        return detail::copyHandles(handles, outEntities, capacity, outCount);
    });
}

// ----------------------------------------------------------- entity creation

CadStatus CAD_CALL apiAddLine(CadTransaction* transaction, CadHandle block, CadPoint start,
                              CadPoint end, const CadEntityProperties* properties,
                              CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    return guard([&] {
        auto line = std::make_unique<LineEntity>(toVec3(start), toVec3(end));
        return finishEntity(*tx, block, std::move(line), properties, outEntity);
    });
}

CadStatus CAD_CALL apiAddCircle(CadTransaction* transaction, CadHandle block, CadPoint center,
                                double radius, const CadEntityProperties* properties,
                                CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "circle radius must be positive and finite");
    }
    return guard([&] {
        auto circle = std::make_unique<CircleEntity>(toVec3(center), radius);
        return finishEntity(*tx, block, std::move(circle), properties, outEntity);
    });
}

CadStatus CAD_CALL apiAddArc(CadTransaction* transaction, CadHandle block, CadPoint center,
                             double radius, double startAngle, double endAngle,
                             const CadEntityProperties* properties, CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "arc radius must be positive and finite");
    }
    if (!std::isfinite(startAngle) || !std::isfinite(endAngle)) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "arc angles must be finite");
    }
    return guard([&] {
        auto arc = std::make_unique<ArcEntity>(toVec3(center), radius, startAngle, endAngle);
        return finishEntity(*tx, block, std::move(arc), properties, outEntity);
    });
}

CadStatus CAD_CALL apiAddPolyline(CadTransaction* transaction, CadHandle block,
                                  const CadVertex* vertices, uint32_t vertexCount,
                                  int32_t closed, const CadEntityProperties* properties,
                                  CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (vertices == nullptr || vertexCount < 2) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "a polyline needs at least two vertices");
    }
    return guard([&] {
        auto polyline = std::make_unique<PolylineEntity>();
        polyline->closed = closed != 0;
        polyline->vertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i) {
            PolylineVertex vertex;
            vertex.position = toVec3(vertices[i].position);
            vertex.bulge = vertices[i].bulge;
            vertex.startWidth = vertices[i].start_width;
            vertex.endWidth = vertices[i].end_width;
            polyline->vertices.push_back(vertex);
        }
        return finishEntity(*tx, block, std::move(polyline), properties, outEntity);
    });
}

CadStatus CAD_CALL apiAddPoint(CadTransaction* transaction, CadHandle block, CadPoint position,
                               const CadEntityProperties* properties, CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    return guard([&] {
        auto point = std::make_unique<PointEntity>(toVec3(position));
        return finishEntity(*tx, block, std::move(point), properties, outEntity);
    });
}

CadStatus CAD_CALL apiAddText(CadTransaction* transaction, CadHandle block, CadPoint position,
                              const char* contents, double height, double rotation,
                              const CadEntityProperties* properties, CadHandle* outEntity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (contents == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "contents are required"); }
    if (!(height > 0.0) || !std::isfinite(height)) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "text height must be positive and finite");
    }
    return guard([&] {
        auto text = std::make_unique<TextEntity>(toVec3(position), contents, height);
        text->rotation = rotation;
        return finishEntity(*tx, block, std::move(text), properties, outEntity);
    });
}

// ------------------------------------------------------- entity modification

CadStatus CAD_CALL apiEraseEntity(CadTransaction* transaction, CadHandle entity) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    return guard([&] {
        Entity* object = tx->database().getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }
        return tx->erase(*object) ? CAD_OK
                                  : fail(CAD_ERROR_NOT_PERMITTED, "the entity could not be erased");
    });
}

CadStatus CAD_CALL apiTransformEntity(CadTransaction* transaction, CadHandle entity,
                                      const double matrix[16]) {
    Transaction* tx = writableTransaction(transaction);
    if (tx == nullptr) { return CAD_ERROR_NO_TRANSACTION; }
    if (matrix == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "matrix is required"); }
    return guard([&] {
        Entity* object = tx->database().getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }

        std::array<double, 16> elements{};
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (!std::isfinite(matrix[i])) {
                return fail(CAD_ERROR_INVALID_ARGUMENT, "matrix contains a non-finite value");
            }
            elements[i] = matrix[i];
        }
        const Mat4 transform{elements};
        tx->modifyEntity(*object, [&](Entity& target) { target.transformBy(transform); });
        return CAD_OK;
    });
}

// ---------------------------------------------------------- geometry queries

CadStatus CAD_CALL apiClosestPoint(CadDocument* document, CadHandle entity, CadPoint from,
                                   CadPoint* outPoint, double* outDistance) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        const Entity* object = host->db().getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }

        const Vec3 query = toVec3(from);
        const std::vector<CurvePtr> curves = object->toCurves();
        if (curves.empty()) {
            // No curve geometry — text, a point, a block reference. The
            // entity's own distance function still has an answer, but there is
            // no position on a curve to report.
            if (outPoint != nullptr) { *outPoint = from; }
            if (outDistance != nullptr) { *outDistance = object->distanceToPoint(query); }
            return CAD_OK;
        }

        ClosestPointResult best = curves.front()->closestPoint(query);
        for (std::size_t i = 1; i < curves.size(); ++i) {
            const ClosestPointResult candidate = curves[i]->closestPoint(query);
            if (candidate.distance < best.distance) { best = candidate; }
        }
        if (outPoint != nullptr) { *outPoint = toCadPoint(best.point); }
        if (outDistance != nullptr) { *outDistance = best.distance; }
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiEntityLength(CadDocument* document, CadHandle entity, double* outLength) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outLength == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_length are required");
    }
    return guard([&] {
        const Entity* object = host->db().getEntity(Handle{entity});
        if (object == nullptr) { return fail(CAD_ERROR_NOT_FOUND, "no such entity"); }
        double total = 0.0;
        for (const CurvePtr& curve : object->toCurves()) { total += curve->length(); }
        *outLength = total;
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiIntersectEntities(CadDocument* document, CadHandle a, CadHandle b,
                                        CadPoint* outPoints, uint32_t capacity,
                                        uint32_t* outCount) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        const Entity* first = host->db().getEntity(Handle{a});
        const Entity* second = host->db().getEntity(Handle{b});
        if (first == nullptr || second == nullptr) {
            return fail(CAD_ERROR_NOT_FOUND, "no such entity");
        }

        std::vector<Vec3> points;
        for (const CurvePtr& curveA : first->toCurves()) {
            for (const CurvePtr& curveB : second->toCurves()) {
                for (const Intersection& hit : intersectCurves(*curveA, *curveB)) {
                    points.push_back(hit.point);
                }
            }
        }

        const auto required = static_cast<std::uint32_t>(points.size());
        if (outCount != nullptr) { *outCount = required; }
        if (outPoints == nullptr) { return CAD_OK; }
        if (capacity < required) {
            return fail(CAD_ERROR_BUFFER_TOO_SMALL,
                        "array of " + std::to_string(capacity) + " needs " +
                            std::to_string(required));
        }
        for (std::uint32_t i = 0; i < required; ++i) { outPoints[i] = toCadPoint(points[i]); }
        return CAD_OK;
    });
}

// ---------------------------------------------------------------- selection

CadStatus CAD_CALL apiSelection(CadCommandContext* context, CadHandle* outEntities,
                                uint32_t capacity, uint32_t* outCount) {
    detail::CommandContextBinding* binding = asContext(context);
    if (binding == nullptr || binding->context == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "a command context is required");
    }
    return guard([&] {
        return detail::copyHandles(binding->context->initialSelection(), outEntities, capacity,
                                   outCount);
    });
}

// ------------------------------------------------------------- transactions

CadTransaction* CAD_CALL apiContextTransaction(CadCommandContext* context) {
    detail::CommandContextBinding* binding = asContext(context);
    if (binding == nullptr || binding->transaction.transaction == nullptr) { return nullptr; }
    return reinterpret_cast<CadTransaction*>(&binding->transaction);
}

CadDocument* CAD_CALL apiContextDocument(CadCommandContext* context) {
    detail::CommandContextBinding* binding = asContext(context);
    return binding == nullptr ? nullptr : asCadDocument(binding->document);
}

CadStatus CAD_CALL apiBeginTransaction(CadDocument* document, const char* name,
                                       CadTransaction** outTransaction) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || outTransaction == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and out_transaction are required");
    }
    return guard([&] {
        TransactionBinding& slot = host->pluginTransaction();
        if (slot.transaction != nullptr) {
            return fail(CAD_ERROR_NOT_PERMITTED, "a plugin transaction is already open");
        }
        slot.transaction = &host->transactions().begin(name != nullptr ? name : "Plugin");
        slot.owner = &host->transactions();
        *outTransaction = reinterpret_cast<CadTransaction*>(&slot);
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiCommitTransaction(CadTransaction* transaction) {
    TransactionBinding* binding = asTransaction(transaction);
    if (binding == nullptr || binding->transaction == nullptr) {
        return fail(CAD_ERROR_NO_TRANSACTION, "no transaction to commit");
    }
    if (binding->owner == nullptr) {
        return fail(CAD_ERROR_NOT_PERMITTED,
                    "this transaction belongs to the running command; the executor commits it");
    }
    return guard([&] {
        binding->owner->commit(*binding->transaction);
        *binding = TransactionBinding{};
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiAbortTransaction(CadTransaction* transaction) {
    TransactionBinding* binding = asTransaction(transaction);
    if (binding == nullptr || binding->transaction == nullptr) {
        return fail(CAD_ERROR_NO_TRANSACTION, "no transaction to abort");
    }
    if (binding->owner == nullptr) {
        return fail(CAD_ERROR_NOT_PERMITTED,
                    "this transaction belongs to the running command; return "
                    "CAD_STEP_CANCELLED to roll it back");
    }
    return guard([&] {
        binding->owner->abort(*binding->transaction);
        *binding = TransactionBinding{};
        return CAD_OK;
    });
}

CadStatus CAD_CALL apiUndo(CadDocument* document) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        return host->transactions().undo() ? CAD_OK
                                           : fail(CAD_ERROR_NOT_FOUND, "nothing to undo");
    });
}

CadStatus CAD_CALL apiRedo(CadDocument* document) {
    HostDocument* host = asDocument(document);
    if (host == nullptr) { return fail(CAD_ERROR_INVALID_ARGUMENT, "document is required"); }
    return guard([&] {
        return host->transactions().redo() ? CAD_OK
                                           : fail(CAD_ERROR_NOT_FOUND, "nothing to redo");
    });
}

// -------------------------------------------------------------- interaction

void CAD_CALL apiLogMessage(CadCommandContext* context, const char* message) {
    detail::CommandContextBinding* binding = asContext(context);
    if (binding == nullptr || binding->context == nullptr || message == nullptr) { return; }
    try {
        binding->context->log(message);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Logging must never be the thing that takes a command down.
    }
}

void CAD_CALL apiLogWarning(CadCommandContext* context, const char* message) {
    detail::CommandContextBinding* binding = asContext(context);
    if (binding == nullptr || binding->context == nullptr || message == nullptr) { return; }
    try {
        binding->context->warn(message);
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
}

// ----------------------------------------------------------------- commands

CadStatus CAD_CALL apiRegisterCommand(CadPluginHost* host, const CadCommandDesc* desc) {
    PluginHost* plugin = asHost(host);
    if (plugin == nullptr || desc == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "host and desc are required");
    }
    return guard([&] { return plugin->registerCommand(*desc); });
}

CadStatus CAD_CALL apiUnregisterCommand(CadPluginHost* host, const char* id) {
    PluginHost* plugin = asHost(host);
    if (plugin == nullptr || id == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "host and id are required");
    }
    return guard([&] {
        return plugin->unregisterCommand(id) ? CAD_OK
                                             : fail(CAD_ERROR_NOT_FOUND, "no such command");
    });
}

CadStatus CAD_CALL apiExecuteCommand(CadDocument* document, const char* name) {
    HostDocument* host = asDocument(document);
    if (host == nullptr || name == nullptr) {
        return fail(CAD_ERROR_INVALID_ARGUMENT, "document and name are required");
    }
    CommandExecutor* executor = host->executor();
    if (executor == nullptr) {
        return fail(CAD_ERROR_NOT_PERMITTED, "this document has no command executor");
    }
    return guard([&] {
        const CommandOutcome outcome = executor->execute(name);
        switch (outcome.status) {
            case CommandOutcome::Status::Unknown:
                return fail(CAD_ERROR_NOT_FOUND, "no such command: " + std::string{name});
            case CommandOutcome::Status::Unavailable:
                return fail(CAD_ERROR_NOT_PERMITTED, "the command is not available right now");
            case CommandOutcome::Status::Failed:
                return fail(CAD_ERROR_INTERNAL, outcome.message);
            default:
                // Running, Completed and Cancelled are all ordinary outcomes.
                // A command that stops for input has not failed; it is simply
                // waiting, and the caller drives it through the host's UI.
                return CAD_OK;
        }
    });
}

/// The one table instance, built once.
[[nodiscard]] CadApi buildApi() noexcept {
    CadApi table{};
    table.struct_size = static_cast<std::uint32_t>(sizeof(CadApi));
    table.version_major = CAD_ABI_VERSION_MAJOR;
    table.version_minor = CAD_ABI_VERSION_MINOR;

    table.last_error = &apiLastError;

    table.host_document = &apiHostDocument;
    table.document_extents = &apiDocumentExtents;

    table.find_layer = &apiFindLayer;
    table.ensure_layer = &apiEnsureLayer;
    table.layer_name = &apiLayerName;
    table.layer_color = &apiLayerColor;
    table.set_layer_color = &apiSetLayerColor;
    table.layer_count = &apiLayerCount;
    table.layer_at = &apiLayerAt;

    table.find_block = &apiFindBlock;
    table.model_space = &apiModelSpace;

    table.entity_kind = &apiEntityKind;
    table.entity_bounds = &apiEntityBounds;
    table.entity_properties = &apiEntityProperties;
    table.set_entity_properties = &apiSetEntityProperties;
    table.block_entities = &apiBlockEntities;

    table.add_line = &apiAddLine;
    table.add_circle = &apiAddCircle;
    table.add_arc = &apiAddArc;
    table.add_polyline = &apiAddPolyline;
    table.add_point = &apiAddPoint;
    table.add_text = &apiAddText;

    table.erase_entity = &apiEraseEntity;
    table.transform_entity = &apiTransformEntity;

    table.closest_point = &apiClosestPoint;
    table.entity_length = &apiEntityLength;
    table.intersect_entities = &apiIntersectEntities;

    table.selection = &apiSelection;

    table.context_transaction = &apiContextTransaction;
    table.context_document = &apiContextDocument;
    table.begin_transaction = &apiBeginTransaction;
    table.commit_transaction = &apiCommitTransaction;
    table.abort_transaction = &apiAbortTransaction;
    table.undo = &apiUndo;
    table.redo = &apiRedo;

    table.log_message = &apiLogMessage;
    table.log_warning = &apiLogWarning;

    table.register_command = &apiRegisterCommand;
    table.unregister_command = &apiUnregisterCommand;
    table.execute_command = &apiExecuteCommand;

    return table;
}

}  // namespace

const CadApi& api() noexcept {
    // Function-local static so initialisation order cannot bite: a plugin
    // loaded from another translation unit's static constructor still gets a
    // fully built table.
    static const CadApi table = buildApi();
    return table;
}

}  // namespace cad::abi
