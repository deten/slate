#include <cmath>

#include "DxfAnnotationReaders.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/SolidEntity.hpp"
#include "cad/db/SplineEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/OleFrameEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"
#include "cad/io/DxfReader.hpp"

namespace cad::io {
namespace {

/// Map the DXF alignment codes onto the entity's enums.
///
/// Code 72 conflates horizontal alignment with the Aligned and Fit modes, and
/// 73 carries vertical alignment separately — except on ATTDEF and ATTRIB,
/// where 74 does.
void readTextAlignment(TextEntity& entity, const DxfRecord& record, int verticalCode) {
    const auto horizontal = record.integer(72, 0);
    if (horizontal >= 0 && horizontal <= 5) {
        entity.horizontalAlignment = static_cast<TextHorizontalAlignment>(horizontal);
    }
    const auto vertical = record.integer(verticalCode, 0);
    if (vertical >= 0 && vertical <= 3) {
        entity.verticalAlignment = static_cast<TextVerticalAlignment>(vertical);
    }
}

void readTextCommon(TextEntity& entity, const DxfRecord& record, ReadContext& context) {
    entity.position = record.point(10);

    // DXF always writes code 11, often as (0,0,0) when unused. Treating that as
    // a real alignment point would drag left-justified text to the origin.
    if (const auto alignment = record.optionalPoint(11); alignment && !alignment->isZero()) {
        entity.alignmentPoint = *alignment;
    }

    entity.height = record.number(40, 2.5);
    entity.contents = decodeDxfText(record.text(1));
    entity.rotation = degToRad(record.number(50, 0.0));
    entity.widthFactor = record.number(41, 1.0);
    entity.obliqueAngle = degToRad(record.number(51, 0.0));
    entity.thickness = record.number(39, 0.0);
    entity.styleHandle = context.db.ensureTextStyle(record.text(7, "Standard")).handle;

    const auto generationFlags = record.integer(71, 0);
    entity.isBackwards = (generationFlags & 2) != 0;
    entity.isUpsideDown = (generationFlags & 4) != 0;
}

void readAttributeFlags(AttributeFlags& flags, const DxfRecord& record) {
    const auto raw = record.integer(70, 0);
    flags.isInvisible = (raw & 1) != 0;
    flags.isConstant = (raw & 2) != 0;
    flags.isVerifiable = (raw & 4) != 0;
    flags.isPreset = (raw & 8) != 0;
    flags.fieldLength = static_cast<int>(record.integer(73, 0));
}

// ------------------------------------------------------------------- geometry

std::unique_ptr<Entity> readLine(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<LineEntity>();
    entity->start = record.point(10);
    entity->end = record.point(11);
    entity->thickness = record.number(39, 0.0);
    return entity;
}

}  // namespace
}  // namespace cad::io

#include "cad/db/ConstructionLineEntity.hpp"

namespace cad::io {
namespace {

/// XLINE and RAY: a base point and a unit direction. Written the same, told
/// apart by the record's type; a zero direction is taken as +X rather than
/// making an entity with no direction at all.
std::unique_ptr<Entity> readConstructionLine(const DxfRecord& record, bool bothWays) {
    auto entity = std::make_unique<ConstructionLineEntity>();
    entity->base = record.point(10);
    const Vec3 direction = record.point(11, Vec3::unitX());
    entity->direction = direction.tryNormalize().value_or(Vec3::unitX());
    entity->bothWays = bothWays;
    return entity;
}

std::unique_ptr<Entity> readXline(const DxfRecord& record, ReadContext&) {
    return readConstructionLine(record, true);
}

std::unique_ptr<Entity> readRay(const DxfRecord& record, ReadContext&) {
    return readConstructionLine(record, false);
}

std::unique_ptr<Entity> readCircle(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<CircleEntity>();
    entity->center = record.point(10);
    entity->radius = record.number(40, 1.0);
    entity->thickness = record.number(39, 0.0);
    return entity;
}

std::unique_ptr<Entity> readArc(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<ArcEntity>();
    entity->center = record.point(10);
    entity->radius = record.number(40, 1.0);
    // DXF stores arc angles in degrees; the kernel is radians throughout.
    entity->startAngle = degToRad(record.number(50, 0.0));
    entity->endAngle = degToRad(record.number(51, 360.0));
    entity->thickness = record.number(39, 0.0);
    return entity;
}

std::unique_ptr<Entity> readEllipse(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<EllipseEntity>();
    entity->center = record.point(10);
    entity->majorAxis = record.point(11, Vec3::unitX());
    entity->ratio = record.number(40, 1.0);
    // 41/42 are start and end eccentric anomaly, already in radians.
    entity->startParam = record.number(41, 0.0);
    entity->endParam = record.number(42, kTau);
    // The extrusion direction decides which way the parameter runs. Left at
    // +Z, an ellipse drawn with -Z -- a mirrored one -- came back as the
    // complementary arc: same ellipse, the other three quarters of it.
    entity->normal = record.point(210, Vec3::unitZ());
    return entity;
}

std::unique_ptr<Entity> readPoint(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<PointEntity>();
    entity->position = record.point(10);
    entity->angle = degToRad(record.number(50, 0.0));
    return entity;
}

/// SOLID and TRACE — a filled triangle or quadrilateral.
///
/// Four corners at 10/20/30 through 13/23/33. The fourth is optional in
/// practice: writers describing a triangle repeat the third, and some omit
/// 13/23/33 altogether, so it defaults to the third rather than to the origin.
/// Defaulting it to the origin would drag a corner of every triangle in the
/// drawing to 0,0.
std::unique_ptr<Entity> readSolid(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<SolidEntity>();
    entity->corners[0] = record.point(10);
    entity->corners[1] = record.point(11);
    entity->corners[2] = record.point(12);
    entity->corners[3] = record.point(13, entity->corners[2]);
    entity->thickness = record.number(39, 0.0);
    return entity;
}

/// LWPOLYLINE — the compact modern polyline.
///
/// Vertices are a flat run of repeated 10/20 pairs with optional per-vertex
/// bulge (42) and widths (40/41). The optional codes are the difficulty: a
/// bulge appears only for vertices that have one, so bulges cannot be zipped
/// positionally against vertices. The groups are therefore walked in *file
/// order*, which is the only way to associate an optional code with its vertex.
std::unique_ptr<Entity> readLwPolyline(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<PolylineEntity>();
    const auto flags = record.integer(70, 0);
    entity->closed = (flags & 1) != 0;
    entity->linetypeGeneration = (flags & 128) != 0;
    entity->constantWidth = record.number(43, 0.0);
    entity->thickness = record.number(39, 0.0);
    record.markConsumed({90, 10, 20, 30, 40, 41, 42});

    bool hasVertex = false;
    double x = 0.0;
    double y = 0.0;
    double bulge = 0.0;
    double startWidth = entity->constantWidth;
    double endWidth = entity->constantWidth;

    const auto flush = [&] {
        if (!hasVertex) {
            return;
        }
        entity->vertices.emplace_back(Vec3{x, y, 0.0}, bulge, startWidth, endWidth);
        hasVertex = false;
        bulge = 0.0;
        startWidth = entity->constantWidth;
        endWidth = entity->constantWidth;
    };

    for (const DxfGroup& group : record.groups()) {
        switch (group.code) {
            case 10:
                // A new X starts a new vertex, so emit the one being built.
                flush();
                x = group.asDouble();
                hasVertex = true;
                break;
            case 20: y = group.asDouble(); break;
            case 40: startWidth = group.asDouble(); break;
            case 41: endWidth = group.asDouble(); break;
            case 42: bulge = group.asDouble(); break;
            default: break;
        }
    }
    flush();
    return entity;
}

/// SPLINE — a NURBS curve.
///
/// Walked in file order rather than read code by code. Knots (40), weights (41)
/// and the coordinates of the control (10/20/30) and fit (11/21/31) points are
/// each repeated runs, and a run only means anything in the order it appears.
/// Weights in particular pair positionally with control points, so reading them
/// as an unordered bag would attach the wrong weight to the wrong point.
std::unique_ptr<Entity> readSpline(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<SplineEntity>();
    entity->flags = static_cast<int>(record.integer(70, 8));
    entity->curve.degree = static_cast<int>(record.integer(71, 3));
    entity->curve.closed = (entity->flags & 1) != 0;
    entity->knotTolerance = record.number(42, 1e-7);
    entity->controlPointTolerance = record.number(43, 1e-7);
    entity->fitTolerance = record.number(44, 1e-10);
    entity->startTangent = record.optionalPoint(12);
    entity->endTangent = record.optionalPoint(13);
    // The counts at 72/73/74 are redundant with the runs themselves and are
    // rewritten from what is actually present, so they are claimed here rather
    // than echoed back as unknown groups and duplicated.
    record.markConsumed({72, 73, 74, 40, 41, 10, 20, 30, 11, 21, 31});

    bool hasControl = false;
    bool hasFit = false;
    Vec3 control;
    Vec3 fit;

    const auto flushControl = [&] {
        if (hasControl) {
            entity->curve.controlPoints.push_back(control);
            hasControl = false;
        }
    };
    const auto flushFit = [&] {
        if (hasFit) {
            entity->fitPoints.push_back(fit);
            hasFit = false;
        }
    };

    for (const DxfGroup& group : record.groups()) {
        switch (group.code) {
            case 40: entity->curve.knots.push_back(group.asDouble()); break;
            case 41: entity->curve.weights.push_back(group.asDouble()); break;
            case 10:
                flushControl();
                control = Vec3{group.asDouble(), 0.0, 0.0};
                hasControl = true;
                break;
            case 20: control.y = group.asDouble(); break;
            case 30: control.z = group.asDouble(); break;
            case 11:
                flushFit();
                fit = Vec3{group.asDouble(), 0.0, 0.0};
                hasFit = true;
                break;
            case 21: fit.y = group.asDouble(); break;
            case 31: fit.z = group.asDouble(); break;
            default: break;
        }
    }
    flushControl();
    flushFit();

    // A file may write a weight of 1 for every point of a curve that is not
    // rational. Dropping a uniformly-one set keeps the non-rational path in
    // evaluation and, more importantly, writes back what was read: the flag at
    // bit 4 says whether the curve is rational, and it is echoed either way.
    const bool rational = (entity->flags & 4) != 0;
    if (!rational) {
        entity->curve.weights.clear();
    }
    return entity;
}

/// POLYLINE — the legacy heavyweight form.
///
/// Its vertices are separate VERTEX entities following it in the stream,
/// terminated by SEQEND. The section reader collects those and passes them
/// through `attachPolylineVertices`, because an entity reader sees one record.
std::unique_ptr<Entity> readPolyline(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<PolylineEntity>();
    entity->isLegacyFormat = true;
    const auto flags = record.integer(70, 0);
    entity->closed = (flags & 1) != 0;
    entity->linetypeGeneration = (flags & 128) != 0;
    entity->constantWidth = record.number(40, 0.0);
    entity->thickness = record.number(39, 0.0);
    entity->elevation = record.point(10).z;
    return entity;
}

// ----------------------------------------------------------------------- text

std::unique_ptr<Entity> readText(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<TextEntity>();
    readTextCommon(*entity, record, context);
    readTextAlignment(*entity, record, 73);
    return entity;
}

/// MTEXT — multi-line text.
///
/// Its contents may be split across several groups: code 3 carries 250-char
/// chunks and code 1 the final one. Concatenating in the right order matters,
/// or long paragraphs are truncated or scrambled.
std::unique_ptr<Entity> readMText(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<MTextEntity>();
    entity->position = record.point(10);
    entity->height = record.number(40, 2.5);
    entity->width = record.number(41, 0.0);

    std::string contents;
    for (const std::string& chunk : record.allStrings(3)) {
        contents += chunk;
    }
    contents += record.text(1);
    entity->contents = decodeDxfText(contents);

    const auto attachment = record.integer(71, 1);
    if (attachment >= 1 && attachment <= 9) {
        entity->attachment = static_cast<MTextAttachment>(attachment);
    }
    entity->lineSpacingFactor = record.number(44, 1.0);

    // Direction (11) takes precedence over rotation (50) when present.
    if (const auto direction = record.optionalPoint(11); direction && !direction->isZero()) {
        entity->directionVector = *direction;
        entity->rotation = direction->angle2();
    } else {
        entity->rotation = degToRad(record.number(50, 0.0));
    }

    entity->styleHandle = context.db.ensureTextStyle(record.text(7, "Standard")).handle;
    return entity;
}

std::unique_ptr<Entity> readAttributeDefinition(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<AttributeDefinitionEntity>();
    readTextCommon(*entity, record, context);
    readTextAlignment(*entity, record, 74);
    entity->tag = record.text(2);
    entity->prompt = record.text(3);
    entity->defaultValue = entity->contents;
    readAttributeFlags(entity->flags, record);
    return entity;
}

std::unique_ptr<Entity> readAttribute(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<AttributeEntity>();
    readTextCommon(*entity, record, context);
    readTextAlignment(*entity, record, 74);
    entity->tag = record.text(2);
    readAttributeFlags(entity->flags, record);
    return entity;
}

// --------------------------------------------------------------------- insert

/// INSERT — a block reference.
///
/// The block is named, not handle-referenced, and may be defined *after* this
/// entity in the file. Resolution is therefore deferred until the whole file is
/// read, which is the general pattern for every name-based DXF reference.
std::unique_ptr<Entity> readInsert(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<InsertEntity>();
    entity->position = record.point(10);
    entity->scale = Vec3{record.number(41, 1.0), record.number(42, 1.0), record.number(43, 1.0)};
    entity->rotation = degToRad(record.number(50, 0.0));
    entity->columnCount = static_cast<int>(record.integer(70, 1));
    entity->rowCount = static_cast<int>(record.integer(71, 1));
    entity->columnSpacing = record.number(44, 0.0);
    entity->rowSpacing = record.number(45, 0.0);

    const std::string blockName = record.text(2);
    InsertEntity* raw = entity.get();
    context.deferred.emplace_back([raw, blockName, &context] {
        const auto it = context.blockNamesToHandles.find(normalizeSymbolName(blockName));
        if (it == context.blockNamesToHandles.end()) {
            context.diagnostics.push_back(
                {DxfDiagnostic::Severity::Warning,
                 "Block reference points at undefined block '" + blockName + "'", 0});
            return;
        }
        raw->blockHandle = it->second;
    });
    return entity;
}

// ------------------------------------------------------------- references

/// A handle-valued code that is not inside a `102 {...}` application group.
///
/// `record.text(code)` is the wrong way to ask for one. An IMAGE names its
/// reactor with a 360, and an object that also carries an extension dictionary
/// names *that* with a 360 too — inside `102 {ACAD_XDICTIONARY}`, which comes
/// first. Taking the first 360 hands back the dictionary, and the image's
/// reactor is then written twice and read back as an object of the wrong type.
///
/// Marks the code consumed, because the caller writes it back itself.
[[nodiscard]] Handle handleOutsideApplicationGroup(const DxfRecord& record, int code) {
    Handle found;
    int depth = 0;
    for (const DxfGroup& group : record.groups()) {
        if (group.code == 102) {
            const std::string value = group.asString();
            if (value.starts_with("{")) {
                ++depth;
            } else if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (group.code == code && depth == 0) {
            found = Handle::tryParse(group.asString()).value_or(Handle{});
            break;
        }
    }
    record.markConsumed({code});
    return found;
}

/// Repeated coordinate pairs, in file order.
///
/// The same reason LWPOLYLINE walks its groups: a clip boundary is a run of
/// 14/24 pairs and there is no other way to keep them paired.
[[nodiscard]] std::vector<Vec3> readVertexRun(const DxfRecord& record, int xCode, int yCode) {
    std::vector<Vec3> vertices;
    bool haveX = false;
    double x = 0.0;
    for (const DxfGroup& group : record.groups()) {
        if (group.code == xCode) {
            x = group.asDouble();
            haveX = true;
        } else if (group.code == yCode && haveX) {
            vertices.push_back(Vec3{x, group.asDouble(), 0.0});
            haveX = false;
        }
    }
    record.markConsumed({xCode, yCode});
    return vertices;
}

std::unique_ptr<Entity> readImage(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<RasterImageEntity>();
    entity->origin = record.point(10);
    entity->uVector = record.point(11, Vec3::unitX());
    entity->vVector = record.point(12, Vec3::unitY());

    const Vec3 size = record.point(13);
    entity->widthPixels = size.x;
    entity->heightPixels = size.y;

    entity->definitionHandle = handleOutsideApplicationGroup(record, 340);
    entity->reactorHandle = handleOutsideApplicationGroup(record, 360);

    const auto flags = record.integer(70, 7);
    entity->showImage = (flags & 1) != 0;
    entity->showWhenNotAligned = (flags & 2) != 0;
    entity->useClippingBoundary = (flags & 4) != 0;
    entity->transparent = (flags & 8) != 0;

    entity->clippingEnabled = record.integer(280, 0) != 0;
    entity->brightness = static_cast<int>(record.integer(281, 50));
    entity->contrast = static_cast<int>(record.integer(282, 50));
    entity->fade = static_cast<int>(record.integer(283, 0));

    entity->clipBoundaryType = static_cast<int>(record.integer(71, 1));
    entity->clipVertices = readVertexRun(record, 14, 24);
    entity->clipInverted = record.integer(290, 0) != 0;
    // The vertex count is re-derived from the vertices on write, so keeping it
    // would guarantee a contradiction the first time one is edited. Code 90 is
    // the AcDbRasterImage class version, which has been 0 in every DXF version
    // that has the entity; the writer states it rather than echoing it, so that
    // it lands before the geometry instead of after the clip boundary.
    record.markConsumed({90, 91});

    return entity;
}

std::unique_ptr<Entity> readUnderlay(const DxfRecord& record, UnderlayKind kind) {
    auto entity = std::make_unique<UnderlayEntity>();
    entity->kind = kind;
    entity->definitionHandle = handleOutsideApplicationGroup(record, 340);
    entity->position = record.point(10);
    entity->scale = Vec3{record.number(41, 1.0), record.number(42, 1.0), record.number(43, 1.0)};
    entity->rotation = degToRad(record.number(50, 0.0));

    const auto flags = record.integer(280, 2);
    entity->clippingEnabled = (flags & 1) != 0;
    entity->underlayOn = (flags & 2) != 0;
    entity->monochrome = (flags & 4) != 0;
    entity->adjustForBackground = (flags & 8) != 0;

    entity->contrast = static_cast<int>(record.integer(281, 100));
    entity->fade = static_cast<int>(record.integer(282, 0));
    entity->clipVertices = readVertexRun(record, 11, 21);
    return entity;
}

/// ACAD_TABLE — a schedule.
///
/// Read by *scanning*, on the same terms as a FIELD, and for the same reason:
/// the record is a run of per-cell blocks that reuse the same group codes, so
/// claiming a code would take every other occurrence with it and the record
/// could not be written back. Nothing here is edited yet, so the interpreted
/// values are a view of the groups and the record round-trips verbatim.
///
/// The cells come after the grid, one block each, each beginning at a code 171
/// and running to its `304 ACVALUE_END`. Within a block, `302` is the literal
/// text and `344` names the field that produces the value instead — most of a
/// schedule is the latter.
std::unique_ptr<Entity> readTable(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<TableEntity>();

    bool inCells = false;
    bool haveInsertion = false;
    bool haveDirection = false;
    double pendingX = 0.0;
    double pendingY = 0.0;
    TableCell cell;
    bool inCell = false;

    const auto flushCell = [&] {
        if (inCell) {
            entity->cells.push_back(cell);
            cell = TableCell{};
            inCell = false;
        }
    };

    for (const DxfGroup& group : record.groups()) {
        switch (group.code) {
            case 342:
                if (!inCells) {
                    entity->styleHandle =
                        Handle::tryParse(group.asString()).value_or(Handle{});
                }
                break;
            case 343:
                if (!inCells) {
                    entity->blockHandle =
                        Handle::tryParse(group.asString()).value_or(Handle{});
                }
                break;
            case 10:
                if (!haveInsertion) {
                    pendingX = group.asDouble();
                }
                break;
            case 20:
                if (!haveInsertion) {
                    pendingY = group.asDouble();
                    entity->position = Vec3{pendingX, pendingY, 0.0};
                    haveInsertion = true;
                }
                break;
            case 11:
                if (!haveDirection) {
                    pendingX = group.asDouble();
                }
                break;
            case 21:
                if (!haveDirection) {
                    entity->direction = Vec3{pendingX, group.asDouble(), 0.0};
                    haveDirection = true;
                }
                break;
            case 141: entity->rowHeights.push_back(group.asDouble()); break;
            case 142: entity->columnWidths.push_back(group.asDouble()); break;

            case 171:
                // The start of a cell block. Everything before the first one is
                // the table's own geometry.
                flushCell();
                inCells = true;
                inCell = true;
                break;
            case 175:
                // Merged *width*, in columns — and 176 is the height, in rows.
                // The two read like a row/column pair and are the other way
                // round, which puts a title row's merge on the wrong axis.
                if (inCell) {
                    cell.columnSpan = std::max(1, static_cast<int>(group.asDouble()));
                }
                break;
            case 176:
                if (inCell) {
                    cell.rowSpan = std::max(1, static_cast<int>(group.asDouble()));
                }
                break;
            case 140:
                if (inCell) {
                    cell.textHeight = group.asDouble();
                }
                break;
            case 344:
                if (inCell) {
                    cell.fieldHandle = Handle::tryParse(group.asString()).value_or(Handle{});
                }
                break;
            case 302:
                if (inCell) {
                    // 302 is the value as text and 1 is the same thing earlier
                    // in the block; 302 is taken because it is the one that is
                    // present even when the value is not a string.
                    cell.text = group.asString();
                }
                break;
            default: break;
        }
    }
    flushCell();
    return entity;
}

std::unique_ptr<Entity> readOleFrame(const DxfRecord& record, ReadContext&) {
    auto entity = std::make_unique<OleFrameEntity>();
    entity->upperLeft = record.point(10);
    entity->lowerRight = record.point(11);
    // Everything else, including several megabytes of compound document, is
    // left unclaimed so that it is written back exactly as it arrived.
    return entity;
}

}  // namespace

// -------------------------------------------------------------- shared readers

void readCommonProperties(Entity& entity, const DxfRecord& record, ReadContext& context) {
    if (const auto handle = Handle::tryParse(record.text(5))) {
        entity.handle = *handle;
        // Reserve it immediately, before anything below can allocate.
        // `ensureLayer` and friends create records on demand and would
        // otherwise be handed this very handle, colliding when the entity is
        // finally added.
        context.db.observeHandle(*handle);
    }
    if (const auto owner = record.ownerHandleText()) {
        if (const auto ownerHandle = Handle::tryParse(*owner)) {
            entity.ownerHandle = *ownerHandle;
        }
    }

    // Layer is named, not handle-referenced, in DXF. Creating it on demand is
    // required: real files do reference layers absent from the LAYER table.
    entity.layerHandle = context.db.ensureLayer(record.text(8, "0")).handle;

    // Colour: 62 is the ACI index, 420 an optional true colour that overrides.
    const auto trueColor = record.optionalNumber(420);
    const auto aci = record.optionalInteger(62);
    if (trueColor) {
        entity.color = Color::fromTrueColor(static_cast<std::uint32_t>(*trueColor));
    } else if (aci) {
        // A negative index means the entity is on a layer that is turned off;
        // the magnitude is the real colour, so the sign is not part of it.
        entity.color = Color::fromIndex(static_cast<int>(std::abs(*aci)));
        if (*aci < 0) {
            entity.visible = false;
        }
    } else {
        entity.color = Color::byLayer();
    }

    if (const auto linetypeName = record.optionalText(6)) {
        const std::string normalized = normalizeSymbolName(*linetypeName);
        if (normalized != "BYLAYER") {
            entity.linetypeHandle = context.db.ensureLinetype(*linetypeName).handle;
        }
    }

    entity.linetypeScale = record.number(48, 1.0);
    entity.lineweight = static_cast<std::int16_t>(
        record.integer(370, toRaw(Lineweight::ByLayer)));

    // 60 is a visibility flag where 1 means invisible.
    if (record.has(60)) {
        entity.visible = record.integer(60, 0) == 0;
    }

    // Transparency is stored as 0x02000000 | (255 - alpha).
    if (const auto rawTransparency = record.optionalInteger(440);
        rawTransparency && *rawTransparency != 0) {
        entity.transparency =
            static_cast<std::uint8_t>(255 - (*rawTransparency & 0xFF));
    }

    entity.normal = record.extrusion();

    for (const auto& [app, items] : record.xdata()) {
        entity.setXData(app, items);
    }
}

std::string decodeDxfText(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '^' && i + 1 < raw.size() && raw[i + 1] == 'I') {
            out.push_back('\t');
            ++i;
            continue;
        }
        // `\U+XXXX` is how a file in a legacy code page spells a character
        // outside it. Decoded here so the drawing shows the character and
        // the writer, which spells it the same way, gives it back unchanged.
        if (raw[i] == '\\' && i + 6 < raw.size() && raw[i + 1] == 'U' && raw[i + 2] == '+') {
            unsigned int codePoint = 0;
            bool valid = true;
            for (std::size_t k = 0; k < 4; ++k) {
                const char c = raw[i + 3 + k];
                unsigned int digit = 0;
                if (c >= '0' && c <= '9') {
                    digit = static_cast<unsigned int>(c - '0');
                } else if (c >= 'A' && c <= 'F') {
                    digit = static_cast<unsigned int>(c - 'A' + 10);
                } else if (c >= 'a' && c <= 'f') {
                    digit = static_cast<unsigned int>(c - 'a' + 10);
                } else {
                    valid = false;
                    break;
                }
                codePoint = codePoint * 16 + digit;
            }
            if (valid) {
                if (codePoint < 0x80) {
                    out.push_back(static_cast<char>(codePoint));
                } else if (codePoint < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
                }
                i += 6;
                continue;
            }
        }
        // `%%d`-style symbol codes are kept as written. They are decoded for
        // display (see decodeTextSymbols), and kept in the text so a file
        // spelling them one way gets them back spelled that way.
        out.push_back(raw[i]);
    }
    return out;
}

std::string stripMTextFormatting(std::string_view raw) {
    MTextEntity scratch;
    scratch.contents = std::string{raw};
    return scratch.plainText();
}

// -------------------------------------------------------- UnsupportedEntity

DbObjectPtr UnsupportedEntity::clone() const {
    auto copy = std::make_unique<UnsupportedEntity>();
    copyEntityInto(*copy);
    copy->dxfType = dxfType;
    return copy;
}

void UnsupportedEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const UnsupportedEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring an unsupported entity");
    }
    restoreEntityFrom(*source);
    dxfType = source->dxfType;
}

// ----------------------------------------------------- EntityReaderRegistry

void EntityReaderRegistry::registerReader(std::string dxfType, EntityReader reader) {
    readers_[normalizeSymbolName(dxfType)] = std::move(reader);
}

const EntityReader* EntityReaderRegistry::find(const std::string& dxfType) const {
    const auto it = readers_.find(normalizeSymbolName(dxfType));
    return it == readers_.end() ? nullptr : &it->second;
}

EntityReaderRegistry EntityReaderRegistry::standard() {
    EntityReaderRegistry registry;
    registry.registerReader("LINE", readLine);
    registry.registerReader("XLINE", readXline);
    registry.registerReader("RAY", readRay);
    registry.registerReader("CIRCLE", readCircle);
    registry.registerReader("ARC", readArc);
    registry.registerReader("ELLIPSE", readEllipse);
    registry.registerReader("POINT", readPoint);
    registry.registerReader("LWPOLYLINE", readLwPolyline);
    registry.registerReader("SOLID", readSolid);
    registry.registerReader("SPLINE", readSpline);
    // Not TRACE, which has the same four corners and the same swap. Reading it
    // into a SolidEntity would write it back out as a SOLID, quietly changing
    // one entity type into another; left alone it round-trips verbatim. It is
    // rare enough that being invisible is the smaller cost of the two.
    registry.registerReader("POLYLINE", readPolyline);
    registry.registerReader("TEXT", readText);
    registry.registerReader("MTEXT", readMText);
    registry.registerReader("ATTDEF", readAttributeDefinition);
    registry.registerReader("ATTRIB", readAttribute);
    registry.registerReader("INSERT", readInsert);
    registry.registerReader("DIMENSION", readDimension);
    registry.registerReader("LEADER", readLeader);
    registry.registerReader("MLINE", readMline);
    registry.registerReader("MULTILEADER", readMLeader);
    registry.registerReader("HATCH", readHatch);
    registry.registerReader("VIEWPORT", readViewport);
    registry.registerReader("IMAGE", readImage);
    registry.registerReader("OLE2FRAME", readOleFrame);
    registry.registerReader("ACAD_TABLE", readTable);
    // The three underlay types differ only in which kind of document they show,
    // so they share a reader rather than repeating it three times with one
    // constant changed.
    // Two names for the same thing. Other CAD software writes an underlay as
    // PDFUNDERLAY; the converter that turns its DWG into text writes
    // PDFREFERENCE. A drawing that reaches here from a DWG has the second,
    // and reading only the first lost every PDF underlay on open.
    registry.registerReader("PDFUNDERLAY", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Pdf);
    });
    registry.registerReader("PDFREFERENCE", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Pdf);
    });
    registry.registerReader("DWFUNDERLAY", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Dwf);
    });
    registry.registerReader("DWFREFERENCE", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Dwf);
    });
    registry.registerReader("DGNUNDERLAY", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Dgn);
    });
    registry.registerReader("DGNREFERENCE", [](const DxfRecord& record, ReadContext&) {
        return readUnderlay(record, UnderlayKind::Dgn);
    });
    return registry;
}

}  // namespace cad::io
