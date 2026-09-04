#include "DxfAnnotationReaders.hpp"

#include <cmath>
#include <span>

#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/MlineEntity.hpp"
#include "cad/db/ViewportEntity.hpp"

namespace cad::io {
namespace {

/// A cursor over a record's groups in file order.
///
/// HATCH cannot be read from a code-keyed map. Its boundary data is a nested
/// sequence — n paths, each with m edges, each with its own codes, several of
/// which (10, 40, 50, 72, 73) are reused at every level with a different
/// meaning. Only the order disambiguates them.
class GroupWalker {
public:
    explicit GroupWalker(std::span<const DxfGroup> groups) : groups_(groups) {}

    [[nodiscard]] bool done() const noexcept { return index_ >= groups_.size(); }

    [[nodiscard]] const DxfGroup& peek() const { return groups_[index_]; }

    const DxfGroup& take() { return groups_[index_++]; }

    /// Advance to the next group with `code`, or to the end.
    bool seek(int code) {
        while (!done() && peek().code != code) {
            ++index_;
        }
        return !done();
    }

    /// Read a point from a `code`/`code+10`/`code+20` run starting here.
    ///
    /// DXF writes the components consecutively, so taking them positionally is
    /// both correct and the only option once a code repeats.
    [[nodiscard]] Vec3 takePoint(int xCode) {
        Vec3 point;
        if (!done() && peek().code == xCode) {
            point.x = take().asDouble();
        }
        if (!done() && peek().code == xCode + 10) {
            point.y = take().asDouble();
        }
        if (!done() && peek().code == xCode + 20) {
            point.z = take().asDouble();
        }
        return point;
    }

private:
    std::span<const DxfGroup> groups_;
    std::size_t index_ = 0;
};

// ---------------------------------------------------------------- dimension

/// Split DXF group 70 into the type and its flag bits.
void readDimensionFlags(DimensionEntity& entity, std::int64_t flags) {
    const auto base = static_cast<std::uint8_t>(flags & 7);
    entity.type = base <= 6 ? static_cast<DimensionType>(base) : DimensionType::Rotated;

    // Bit 32 says the entity references a block that this reader keeps but does
    // not draw; bit 64 selects the ordinate axis; bit 128 says the text has
    // been dragged and its stored position is authoritative.
    entity.isXDatum = (flags & 64) != 0;
    entity.textPositionIsUserDefined = (flags & 128) != 0;
}

}  // namespace

std::unique_ptr<Entity> readDimension(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<DimensionEntity>();

    readDimensionFlags(*entity, record.integer(70, 0));

    entity->definitionPoint = record.point(10);
    entity->textMidPoint = record.point(11);
    entity->insertionPoint = record.point(12);
    entity->point13 = record.point(13);
    entity->point14 = record.point(14);
    entity->point15 = record.point(15);
    entity->point16 = record.point(16);

    entity->rotation = degToRad(record.number(50, 0.0));
    entity->obliqueAngle = degToRad(record.number(52, 0.0));
    entity->horizontalDirection = degToRad(record.number(51, 0.0));
    if (const auto textRotation = record.optionalNumber(53)) {
        entity->textRotationOverride = degToRad(*textRotation);
        entity->hasTextRotationOverride = true;
    }
    entity->leaderLength = record.number(40, 0.0);
    entity->storedMeasurement = record.number(42, 0.0);

    // Group 1 is deliberately not defaulted to empty-when-absent-or-blank: an
    // absent code means "write the measurement" and a single space means
    // "write nothing", and conflating them loses a real authoring decision.
    if (const auto text = record.optionalText(1)) {
        entity->textOverride = decodeDxfText(*text);
    }

    const auto attachment = record.integer(71, 5);
    if (attachment >= 1 && attachment <= 9) {
        entity->attachmentPoint = static_cast<MTextAttachment>(attachment);
    }
    entity->lineSpacingStyle = static_cast<int>(record.integer(72, 1));
    entity->lineSpacingFactor = record.number(41, 1.0);

    // The style is named, and the DIMSTYLE table may not have been read yet
    // when this entity is; creating on demand is the same rule the layer and
    // linetype references follow.
    entity->styleHandle = context.db.ensureDimStyle(record.text(3, kDimStyleStandard)).handle;

    // The generated-geometry block is resolved after the whole file is read,
    // for the same reason INSERT's is: it may be defined later in the stream.
    const std::string blockName = record.text(2);
    if (!blockName.empty()) {
        DimensionEntity* raw = entity.get();
        context.deferred.emplace_back([raw, blockName, &context] {
            const auto it = context.blockNamesToHandles.find(normalizeSymbolName(blockName));
            if (it != context.blockNamesToHandles.end()) {
                raw->blockHandle = it->second;
            }
            // A missing block is not worth a diagnostic. The geometry is
            // regenerated from the definition points regardless, so nothing
            // is lost by the reference dangling.
        });
    }

    return entity;
}

// --------------------------------------------------------------- multileader

std::unique_ptr<Entity> readMLeader(const DxfRecord& record, ReadContext& /*context*/) {
    auto entity = std::make_unique<MLeaderEntity>();

    // Nothing is claimed. A multileader's content sits in a CONTEXT_DATA block
    // whose codes repeat at three depths -- code 10 is a leader line's vertex,
    // a leader's landing point *and* the content base point -- and whose code
    // 304 is both the note's text and the marker that opens a leader line. No
    // code can be asked for by number, so this walks the groups in file order
    // with a depth counter, reads what it needs to draw, and leaves every group
    // unconsumed so the whole entity is written back exactly as it arrived.
    enum Depth : int { Outside = 0, Context = 1, Leader = 2, LeaderLine = 3 };
    int depth = Outside;

    // A LEADER block holds the landing, and one or more LEADER_LINE blocks
    // under it hold the arrows. They are separate things: several arrows
    // routinely share one landing and one note -- "these three walls are all
    // 2x6" is one multileader with three arrows -- so a leader line is the unit
    // that gets its own arrowhead, and the landing belongs to all of them.
    std::vector<std::vector<Vec3>> pendingLines;
    Vec3 landingFrom;
    Vec3 doglegDirection;
    double doglegLength = 0.0;
    bool hasLandingPoint = false;

    std::vector<Vec3> line;
    Vec3 vertex;
    bool building = false;

    const auto flushVertex = [&] {
        if (building) {
            line.push_back(vertex);
            vertex = Vec3{};
            building = false;
        }
    };

    const auto flushLine = [&] {
        flushVertex();
        if (!line.empty()) {
            pendingLines.push_back(line);
        }
        line.clear();
    };

    const auto flushLeader = [&] {
        flushLine();
        if (pendingLines.empty()) {
            return;
        }
        const bool hasLanding = hasLandingPoint && doglegDirection.lengthSquared() > 0.0;
        const Vec3 landingEnd =
            hasLanding ? landingFrom + doglegDirection.normalized() * doglegLength : Vec3{};

        for (std::vector<Vec3>& vertices : pendingLines) {
            MLeaderEntity::LeaderRun run;
            run.vertices = std::move(vertices);
            run.landingEnd = landingEnd;
            run.hasLanding = hasLanding;
            entity->leaders.push_back(std::move(run));
        }
        pendingLines.clear();
        hasLandingPoint = false;
        doglegLength = 0.0;
        doglegDirection = Vec3{};
    };

    for (const DxfGroup& group : record.groups()) {
        const std::string value = group.code == 300 || group.code == 302 || group.code == 304
                                      ? group.asString()
                                      : std::string{};
        switch (group.code) {
            case 300: depth = Context; continue;
            case 301: flushLeader(); depth = Outside; continue;
            case 302:
                flushLeader();
                depth = Leader;
                continue;
            case 303: flushLeader(); depth = Context; continue;
            case 305: flushLine(); depth = Leader; continue;
            case 304:
                // The overloaded one. A LEADER_LINE marker opens a block; at
                // context depth the same code carries the note itself.
                if (value == "LEADER_LINE{") {
                    flushLine();
                    depth = LeaderLine;
                } else if (depth == Context) {
                    entity->text = decodeDxfText(value);
                }
                continue;
            default: break;
        }

        if (depth == LeaderLine) {
            switch (group.code) {
                case 10:
                    flushVertex();
                    vertex.x = group.asDouble();
                    building = true;
                    break;
                case 20: vertex.y = group.asDouble(); break;
                case 30: vertex.z = group.asDouble(); break;
                default: break;
            }
            continue;
        }

        if (depth == Leader) {
            switch (group.code) {
                case 10:
                    landingFrom.x = group.asDouble();
                    hasLandingPoint = true;
                    break;
                case 20: landingFrom.y = group.asDouble(); break;
                case 30: landingFrom.z = group.asDouble(); break;
                case 11: doglegDirection.x = group.asDouble(); break;
                case 21: doglegDirection.y = group.asDouble(); break;
                case 31: doglegDirection.z = group.asDouble(); break;
                case 40: doglegLength = group.asDouble(); break;
                default: break;
            }
            continue;
        }

        if (depth == Context) {
            switch (group.code) {
                case 12: entity->textLocation.x = group.asDouble(); break;
                case 22: entity->textLocation.y = group.asDouble(); break;
                case 32: entity->textLocation.z = group.asDouble(); break;
                case 41: entity->textHeight = group.asDouble(); break;
                case 42: entity->textRotation = group.asDouble(); break;
                case 140: entity->arrowSize = group.asDouble(); break;
                case 171: entity->attachment = static_cast<int>(group.asInteger()); break;
                default: break;
            }
        }
    }
    flushLeader();

    return entity;
}

// -------------------------------------------------------------------- mline

std::unique_ptr<Entity> readMline(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<MlineEntity>();

    entity->styleName = record.text(2, "STANDARD");
    entity->scale = record.number(40, 1.0);
    entity->justification = static_cast<int>(record.integer(70, 0));
    const auto flags = record.integer(71, 0);
    entity->closed = (flags & 2) != 0;
    entity->suppressStartCaps = (flags & 4) != 0;
    entity->suppressEndCaps = (flags & 8) != 0;
    entity->elementCount = static_cast<int>(record.integer(73, 2));
    entity->startPoint = record.point(10);
    if (const auto style = record.optionalText(340)) {
        entity->styleHandle = Handle::tryParse(*style).value_or(Handle::null());
    }

    // The vertices are a *sequence*, not a set: each one is a position, two
    // vectors and then a run of per-element parameter lists, and the same codes
    // repeat for every vertex and every element. Read out of order it is
    // meaningless, so this walks the groups in file order rather than asking
    // for codes by number.
    record.markConsumed({11, 21, 31, 12, 22, 32, 13, 23, 33, 41, 42, 72, 74, 75});

    MlineVertex vertex;
    bool building = false;
    // Which element's parameter list the 41s are currently filling.
    bool inElement = false;
    bool inFill = false;

    const auto flush = [&] {
        if (building) {
            entity->vertices.push_back(vertex);
            vertex = MlineVertex{};
            building = false;
            inElement = false;
            inFill = false;
        }
    };

    for (const DxfGroup& group : record.groups()) {
        switch (group.code) {
            case 11:
                flush();
                vertex.position.x = group.asDouble();
                building = true;
                break;
            case 21: vertex.position.y = group.asDouble(); break;
            case 31: vertex.position.z = group.asDouble(); break;
            case 12: vertex.direction.x = group.asDouble(); break;
            case 22: vertex.direction.y = group.asDouble(); break;
            case 32: vertex.direction.z = group.asDouble(); break;
            case 13: vertex.miterDirection.x = group.asDouble(); break;
            case 23: vertex.miterDirection.y = group.asDouble(); break;
            case 33: vertex.miterDirection.z = group.asDouble(); break;
            case 74:
                // Opens an element's parameter list, even when the count is
                // zero -- an element with no parameters at this vertex still
                // occupies its place in the order, and skipping it would shift
                // every element after it onto the wrong offsets.
                vertex.elementOffsets.emplace_back();
                inElement = true;
                inFill = false;
                break;
            case 75:
                vertex.fillOffsets.emplace_back();
                inFill = true;
                inElement = false;
                break;
            case 41:
                if (inElement && !vertex.elementOffsets.empty()) {
                    vertex.elementOffsets.back().push_back(group.asDouble());
                }
                break;
            case 42:
                if (inFill && !vertex.fillOffsets.empty()) {
                    vertex.fillOffsets.back().push_back(group.asDouble());
                }
                break;
            default: break;
        }
    }
    flush();

    // A file that disagrees with itself about how many elements there are is
    // trusted about the parameters it actually wrote, since those are what the
    // geometry comes from.
    std::size_t widest = 0;
    for (const MlineVertex& v : entity->vertices) {
        widest = std::max(widest, v.elementOffsets.size());
    }
    if (widest > 0 && static_cast<std::size_t>(entity->elementCount) != widest) {
        context.diagnostics.push_back(
            {DxfDiagnostic::Severity::Warning,
             "MLINE says it has " + std::to_string(entity->elementCount) +
                 " element(s) but carries offsets for " + std::to_string(widest),
             0});
        entity->elementCount = static_cast<int>(widest);
    }

    return entity;
}

// ------------------------------------------------------------------- leader

std::unique_ptr<Entity> readLeader(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<LeaderEntity>();

    // The path arrives as a run of repeated 10/20/30 triples, with a count in
    // 76 that real files do not always agree with -- so the vertices are taken
    // from the groups themselves and the count is ignored.
    record.markConsumed({10, 20, 30, 76});
    bool building = false;
    Vec3 vertex;
    const auto flush = [&] {
        if (building) {
            entity->vertices.push_back(vertex);
            building = false;
            vertex = Vec3{};
        }
    };
    for (const DxfGroup& group : record.groups()) {
        switch (group.code) {
            case 10:
                flush();
                vertex.x = group.asDouble();
                building = true;
                break;
            case 20: vertex.y = group.asDouble(); break;
            case 30: vertex.z = group.asDouble(); break;
            default: break;
        }
    }
    flush();

    entity->hasArrowhead = record.integer(71, 1) != 0;
    entity->annotationType = static_cast<int>(record.integer(73, 3));
    // Absent means *no* hookline. A writer that leaves out groups at their
    // default -- the DWG converter is one -- drops 75 exactly when it is 0, and
    // reading that as "on" gave every leader back from a DWG a second landing,
    // drawn out past its last vertex and through the note that sat there.
    entity->hasLanding = record.integer(75, 0) != 0;
    entity->textHeight = record.number(40, 0.0);
    entity->textWidth = record.number(41, 0.0);
    if (const auto direction = record.optionalPoint(211)) {
        entity->horizontalDirection = *direction;
        entity->hasHorizontalDirection = true;
    }

    // Named, and the DIMSTYLE table may not have been read yet -- the same
    // rule the dimension reader follows.
    entity->styleHandle = context.db.ensureDimStyle(record.text(3, kDimStyleStandard)).handle;

    // The note is a separate entity, and one that may appear later in the
    // stream, so the link is resolved after the whole file is read. A handle
    // that resolves to nothing leaves a leader with no note, which is a
    // drawable leader rather than a failed read.
    if (const auto annotation = record.optionalText(340)) {
        entity->annotationHandle = Handle::tryParse(*annotation).value_or(Handle::null());
    }

    // A leader this application wrote carries its note link in XData rather
    // than in group 340 (see LeaderEntity::setAnnotation). The XData itself is
    // copied onto the entity later by the common path; the link is read here
    // so the entity is whole from the moment it exists.
    const auto slate = record.xdata().find(LeaderEntity::kSlateApplication);
    if (slate != record.xdata().end()) {
        for (const XDataItem& item : slate->second) {
            if (item.code != 1005) {
                continue;
            }
            if (const auto* handle = std::get_if<Handle>(&item.value)) {
                entity->annotationHandle = *handle;
            } else if (const auto* text = std::get_if<std::string>(&item.value)) {
                entity->annotationHandle = Handle::tryParse(*text).value_or(Handle::null());
            }
        }
        if (!entity->annotationHandle.isNull()) {
            entity->annotationType = 0;
        }
    }

    return entity;
}

// ----------------------------------------------------------------- viewport

std::unique_ptr<Entity> readViewport(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<ViewportEntity>();

    // Paper coordinates: where the window sits on the sheet.
    entity->center = record.point(10);
    entity->width = record.number(40, 100.0);
    entity->height = record.number(41, 100.0);
    entity->viewportId = static_cast<int>(record.integer(69, 2));
    // 68 is the same number under a different code -- "status", which is the
    // viewport's stacking order and is zero when the viewport is off. The
    // writer re-derives it, so claiming it here is what stops the preserved
    // copy being emitted alongside and giving the record two of them.
    record.markConsumed({68});

    // Model coordinates: what it looks at. Mixing these two up is the classic
    // paper space bug, so they are read in separate blocks.
    entity->viewCenter = record.point(12);
    entity->viewHeight = record.number(45, 100.0);
    entity->twist = degToRad(record.number(51, 0.0));
    entity->viewTarget = record.point(17);
    if (const auto direction = record.optionalPoint(16);
        direction && !direction->isZero()) {
        // A unit direction for the geometry, with its length kept beside it:
        // other CAD software stores the target distance in this vector's length, and a
        // viewport written back at unit length had lost it.
        entity->viewDirection = direction->normalized();
        entity->viewDirectionLength = direction->length();
    }

    entity->statusFlags = static_cast<int>(record.integer(90, 0));
    entity->locked = (entity->statusFlags & 0x4000) != 0;

    // Layers frozen in this viewport alone: the mechanism that lets one model
    // serve a demolition sheet and a new-work sheet. Stored as handles, which
    // for once need no deferral because layers precede entities in the file.
    for (const std::string& raw : record.allStrings(331)) {
        if (const auto layer = Handle::tryParse(raw)) {
            entity->frozenLayerHandles.push_back(*layer);
        }
    }
    if (const auto clip = Handle::tryParse(record.text(340))) {
        entity->clipEntityHandle = *clip;
    }

    (void)context;
    return entity;
}

// -------------------------------------------------------------------- hatch

std::unique_ptr<Entity> readHatch(const DxfRecord& record, ReadContext& context) {
    auto entity = std::make_unique<HatchEntity>();

    entity->pattern.name = record.text(2, kHatchPatternSolid);
    entity->pattern.isSolid = record.integer(70, 1) != 0;
    entity->isAssociative = record.integer(71, 0) != 0;
    entity->pattern.angle = degToRad(record.number(52, 0.0));
    entity->pattern.scale = record.number(41, 1.0);
    entity->pattern.isDouble = record.integer(77, 0) != 0;
    entity->pixelSize = record.number(47, 0.0);
    entity->elevation = record.number(30, 0.0);

    entity->patternType = static_cast<int>(record.integer(76, 1));
    const auto style = record.integer(75, 0);
    if (style >= 0 && style <= 2) {
        entity->style = static_cast<HatchStyle>(style);
    }

    // Gradient data, kept so a gradient-filled drawing survives a save even
    // though it is currently drawn flat.
    if (const auto gradientFlag = record.optionalInteger(450); gradientFlag.value_or(0) != 0) {
        entity->gradient.enabled = true;
        entity->gradient.isSingleColor = record.integer(452, 0) != 0;
        entity->gradient.angle = record.number(460, 0.0);
        entity->gradient.tint = record.number(461, 0.0);
        entity->gradient.shift = record.number(453, 0.0) != 0.0 ? record.number(462, 0.0) : 0.0;
        entity->gradient.name = record.text(470, "LINEAR");
        if (const auto first = record.optionalNumber(463)) {
            entity->gradient.color1 = Color::fromTrueColor(static_cast<std::uint32_t>(*first));
        }
    }

    // Everything from here needs file order.
    record.markConsumed({91, 92, 93, 97, 72, 73, 78, 98, 53, 43, 44, 45, 46, 79, 49, 10, 20,
                         11, 21, 40, 42, 50, 51, 97, 330, 451, 452, 453, 460, 461, 462, 463,
                         470, 450});

    GroupWalker walker{record.groups()};

    // Boundary paths, introduced by code 91 (the path count).
    if (walker.seek(91)) {
        const auto pathCount = static_cast<std::size_t>(walker.take().asInteger());
        for (std::size_t p = 0; p < pathCount; ++p) {
            if (!walker.seek(92)) {
                break;
            }
            HatchBoundaryPath path;
            path.flags = HatchPathFlags::fromDxf(static_cast<int>(walker.take().asInteger()));

            if (path.flags.isPolyline) {
                // A polyline path: 72 says whether bulges are present, 73
                // whether it is closed, 93 the vertex count.
                bool hasBulge = false;
                std::size_t vertexCount = 0;
                while (!walker.done()) {
                    const int code = walker.peek().code;
                    if (code == 72) {
                        hasBulge = walker.take().asInteger() != 0;
                    } else if (code == 73) {
                        path.closed = walker.take().asInteger() != 0;
                    } else if (code == 93) {
                        vertexCount = static_cast<std::size_t>(walker.take().asInteger());
                        break;
                    } else {
                        walker.take();
                    }
                }
                for (std::size_t v = 0; v < vertexCount && !walker.done(); ++v) {
                    if (!walker.seek(10)) {
                        break;
                    }
                    PolylineVertex vertex;
                    vertex.position = walker.takePoint(10);
                    if (hasBulge && !walker.done() && walker.peek().code == 42) {
                        vertex.bulge = walker.take().asDouble();
                    }
                    path.vertices.push_back(vertex);
                }
            } else {
                if (!walker.seek(93)) {
                    break;
                }
                const auto edgeCount = static_cast<std::size_t>(walker.take().asInteger());
                for (std::size_t e = 0; e < edgeCount && !walker.done(); ++e) {
                    if (!walker.seek(72)) {
                        break;
                    }
                    const auto edgeType = walker.take().asInteger();
                    HatchEdge edge;
                    switch (edgeType) {
                        case 1: {
                            edge.kind = HatchEdge::Kind::Line;
                            walker.seek(10);
                            edge.start = walker.takePoint(10);
                            walker.seek(11);
                            edge.end = walker.takePoint(11);
                            break;
                        }
                        case 2: {
                            edge.kind = HatchEdge::Kind::Arc;
                            walker.seek(10);
                            edge.center = walker.takePoint(10);
                            if (walker.seek(40)) {
                                edge.radius = walker.take().asDouble();
                            }
                            if (walker.seek(50)) {
                                edge.startAngle = degToRad(walker.take().asDouble());
                            }
                            if (!walker.done() && walker.peek().code == 51) {
                                edge.endAngle = degToRad(walker.take().asDouble());
                            }
                            if (!walker.done() && walker.peek().code == 73) {
                                edge.counterClockwise = walker.take().asInteger() != 0;
                            }
                            break;
                        }
                        case 3: {
                            edge.kind = HatchEdge::Kind::Ellipse;
                            walker.seek(10);
                            edge.center = walker.takePoint(10);
                            walker.seek(11);
                            edge.majorAxis = walker.takePoint(11);
                            if (walker.seek(40)) {
                                edge.ratio = walker.take().asDouble();
                            }
                            // Ellipse edge angles are parameters in degrees,
                            // matching the ELLIPSE entity rather than the arc.
                            if (walker.seek(50)) {
                                edge.startAngle = degToRad(walker.take().asDouble());
                            }
                            if (!walker.done() && walker.peek().code == 51) {
                                edge.endAngle = degToRad(walker.take().asDouble());
                            }
                            if (!walker.done() && walker.peek().code == 73) {
                                edge.counterClockwise = walker.take().asInteger() != 0;
                            }
                            break;
                        }
                        case 4: {
                            edge.kind = HatchEdge::Kind::Spline;
                            if (walker.seek(94)) {
                                edge.degree = static_cast<int>(walker.take().asInteger());
                            }
                            if (!walker.done() && walker.peek().code == 73) {
                                edge.rational = walker.take().asInteger() != 0;
                            }
                            if (!walker.done() && walker.peek().code == 74) {
                                edge.periodic = walker.take().asInteger() != 0;
                            }
                            std::size_t knotCount = 0;
                            std::size_t controlCount = 0;
                            if (!walker.done() && walker.peek().code == 95) {
                                knotCount = static_cast<std::size_t>(walker.take().asInteger());
                            }
                            if (!walker.done() && walker.peek().code == 96) {
                                controlCount = static_cast<std::size_t>(walker.take().asInteger());
                            }
                            for (std::size_t k = 0; k < knotCount && walker.seek(40); ++k) {
                                edge.knots.push_back(walker.take().asDouble());
                            }
                            for (std::size_t c = 0; c < controlCount && walker.seek(10); ++c) {
                                edge.controlPoints.push_back(walker.takePoint(10));
                                if (!walker.done() && walker.peek().code == 42) {
                                    edge.weights.push_back(walker.take().asDouble());
                                }
                            }
                            if (!walker.done() && walker.peek().code == 97) {
                                const auto fitCount =
                                    static_cast<std::size_t>(walker.take().asInteger());
                                for (std::size_t f = 0; f < fitCount && walker.seek(11); ++f) {
                                    edge.fitPoints.push_back(walker.takePoint(11));
                                }
                            }
                            break;
                        }
                        default:
                            // An edge type this reader does not know. Skipping
                            // it leaves a gap in the loop, which is why the
                            // whole record's groups are preserved for save.
                            continue;
                    }
                    path.edges.push_back(std::move(edge));
                }
            }

            // Source object handles for associativity, introduced by code 97.
            if (!walker.done() && walker.peek().code == 97) {
                const auto sourceCount = static_cast<std::size_t>(walker.take().asInteger());
                for (std::size_t s = 0; s < sourceCount && walker.seek(330); ++s) {
                    if (const auto handle = Handle::tryParse(walker.take().asString())) {
                        path.sourceHandles.push_back(*handle);
                    }
                }
            }

            entity->paths.push_back(std::move(path));
        }
    }

    // Pattern definition lines, introduced by code 78 (the line count).
    GroupWalker patternWalker{record.groups()};
    if (patternWalker.seek(78)) {
        const auto lineCount = static_cast<std::size_t>(patternWalker.take().asInteger());
        for (std::size_t l = 0; l < lineCount; ++l) {
            if (!patternWalker.seek(53)) {
                break;
            }
            HatchPatternLine line;
            line.angle = degToRad(patternWalker.take().asDouble());
            if (patternWalker.seek(43)) {
                line.basePoint.x = patternWalker.take().asDouble();
            }
            if (!patternWalker.done() && patternWalker.peek().code == 44) {
                line.basePoint.y = patternWalker.take().asDouble();
            }
            if (!patternWalker.done() && patternWalker.peek().code == 45) {
                line.offset.x = patternWalker.take().asDouble();
            }
            if (!patternWalker.done() && patternWalker.peek().code == 46) {
                line.offset.y = patternWalker.take().asDouble();
            }
            if (!patternWalker.done() && patternWalker.peek().code == 79) {
                const auto dashCount = static_cast<std::size_t>(patternWalker.take().asInteger());
                for (std::size_t d = 0; d < dashCount && patternWalker.seek(49); ++d) {
                    line.dashes.push_back(patternWalker.take().asDouble());
                }
            }
            entity->pattern.lines.push_back(std::move(line));
        }
    }

    // A non-solid hatch with no definition in the file — which happens when a
    // writer assumes the reader has the pattern library — falls back to the
    // built-in definition rather than being drawn as nothing.
    if (!entity->pattern.isSolid && entity->pattern.lines.empty()) {
        if (const auto known = standardHatchPattern(entity->pattern.name)) {
            const double angle = entity->pattern.angle;
            const double scale = entity->pattern.scale;
            entity->pattern = *known;
            entity->pattern.angle = angle;
            entity->pattern.scale = scale;
        } else {
            context.diagnostics.push_back(
                {DxfDiagnostic::Severity::Warning,
                 "Hatch pattern '" + entity->pattern.name +
                     "' has no definition in the file and is not a known pattern",
                 0});
        }
    }

    // Seed points, introduced by code 98.
    GroupWalker seedWalker{record.groups()};
    if (seedWalker.seek(98)) {
        const auto seedCount = static_cast<std::size_t>(seedWalker.take().asInteger());
        for (std::size_t s = 0; s < seedCount && seedWalker.seek(10); ++s) {
            entity->seedPoints.push_back(seedWalker.takePoint(10));
        }
    }

    return entity;
}

}  // namespace cad::io
