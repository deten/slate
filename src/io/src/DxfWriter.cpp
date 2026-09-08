#include "cad/io/DxfWriter.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <type_traits>
#include <variant>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/SolidEntity.hpp"
#include "cad/db/SplineEntity.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/MlineEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/OleFrameEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/math/Angle.hpp"

namespace cad::io {

/// Defined with the builder below; used by the entity writers above it.
[[nodiscard]] std::string escapeNonAscii(std::string_view utf8);
[[nodiscard]] std::string encodeDxfText(std::string_view utf8);

namespace {

/// Format a double in fixed notation, keeping every bit of it.
///
/// DXF permits scientific notation but several real consumers choke on it, so
/// it is never emitted. That is the constraint; the precision is chosen within
/// it.
///
/// The shortest representation that reads back as the same double is tried
/// first, which is what makes a save exact. Twelve decimal places -- what this
/// used to write unconditionally -- is not enough for a coordinate with four
/// digits in front of the point, and a survey drawing full of them came back
/// differing from itself by around 1e-12 in nearly every coordinate. Harmless
/// at any scale anyone drafts at, and still a drawing that does not equal
/// itself after a round trip, which is a bad property for a file format layer
/// to have.
///
/// `std::to_chars` in its shortest form may answer in scientific notation for
/// very large or very small magnitudes, so that answer is used only when it is
/// not, and fixed notation is the fallback.
[[nodiscard]] std::string formatDouble(double value) {
    if (!std::isfinite(value)) {
        // A non-finite coordinate is corrupt data. Writing "nan" would produce
        // a file no consumer can read, so it degrades to zero — the entity is
        // misplaced but the file remains loadable.
        return "0.0";
    }

    char buffer[64];
    auto [pointer, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{} ||
        std::find(buffer, pointer, 'e') != pointer ||
        std::find(buffer, pointer, 'E') != pointer) {
        const auto fixed =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 12);
        pointer = fixed.ptr;
        error = fixed.ec;
    }
    if (error != std::errc{}) {
        return "0.0";
    }

    std::string text{buffer, pointer};
    if (text.find('.') != std::string::npos) {
        while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.push_back('0');
        }
    } else {
        text += ".0";
    }
    return text;
}

/// The name a symbol record is known by, for the name-based references DXF uses
/// in place of handles.
[[nodiscard]] std::string symbolName(const Database& db, Handle handle,
                                     std::string_view fallback) {
    const auto* record = db.get<SymbolTableRecord>(handle);
    return record == nullptr ? std::string{fallback} : record->name;
}

// ------------------------------------------------------------ entity writers

void writeLine(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& line = static_cast<const LineEntity&>(entity);
    out.pair(100, "AcDbLine");
    out.point(10, line.start);
    out.point(11, line.end);
    if (line.thickness != 0.0) {
        out.pair(39, line.thickness);
    }
}

void writeCircle(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& circle = static_cast<const CircleEntity&>(entity);
    out.pair(100, "AcDbCircle");
    out.point(10, circle.center);
    out.pair(40, circle.radius);
    if (circle.thickness != 0.0) {
        out.pair(39, circle.thickness);
    }
}

void writeArc(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& arc = static_cast<const ArcEntity&>(entity);
    out.pair(100, "AcDbCircle");
    out.point(10, arc.center);
    out.pair(40, arc.radius);
    out.pair(100, "AcDbArc");
    // Angles are radians inside the kernel and degrees in the file; the
    // conversion happens exactly here, at the boundary.
    out.pair(50, radToDeg(arc.startAngle));
    out.pair(51, radToDeg(arc.endAngle));
}

void writeEllipse(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& ellipse = static_cast<const EllipseEntity&>(entity);
    out.pair(100, "AcDbEllipse");
    out.point(10, ellipse.center);
    out.point(11, ellipse.majorAxis);
    out.pair(40, ellipse.ratio);
    // 41/42 are eccentric anomaly and are already radians in both
    // representations, so they pass through unconverted.
    out.pair(41, ellipse.startParam);
    out.pair(42, ellipse.endParam);
}

void writePoint(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& point = static_cast<const PointEntity&>(entity);
    out.pair(100, "AcDbPoint");
    out.point(10, point.position);
    if (point.angle != 0.0) {
        out.pair(50, radToDeg(point.angle));
    }
}

void writeSolid(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& solid = static_cast<const SolidEntity&>(entity);
    out.pair(100, "AcDbTrace");
    // In storage order, which is not ring order -- see SolidEntity. The fourth
    // is always written even for a triangle, where it repeats the third, since
    // that is how other CAD software writes one and a reader may not default it.
    out.point(10, solid.corners[0]);
    out.point(11, solid.corners[1]);
    out.point(12, solid.corners[2]);
    out.point(13, solid.corners[3]);
    if (solid.thickness != 0.0) {
        out.pair(39, solid.thickness);
    }
}

void writeSpline(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& spline = static_cast<const SplineEntity&>(entity);
    const NurbsCurve& curve = spline.curve;
    out.pair(100, "AcDbSpline");
    out.pair(70, static_cast<std::int64_t>(spline.flags));
    out.pair(71, static_cast<std::int64_t>(curve.degree));
    // Counts before the runs they describe, as other CAD software writes them, and taken
    // from what is actually about to be written rather than from what was read.
    out.pair(72, static_cast<std::int64_t>(curve.knots.size()));
    out.pair(73, static_cast<std::int64_t>(curve.controlPoints.size()));
    out.pair(74, static_cast<std::int64_t>(spline.fitPoints.size()));
    out.pair(42, spline.knotTolerance);
    out.pair(43, spline.controlPointTolerance);
    if (!spline.fitPoints.empty()) {
        out.pair(44, spline.fitTolerance);
    }
    if (spline.startTangent) {
        out.point(12, *spline.startTangent);
    }
    if (spline.endTangent) {
        out.point(13, *spline.endTangent);
    }
    for (const double knot : curve.knots) {
        out.pair(40, knot);
    }
    // Weights only for a rational curve. Writing a run of ones for a curve that
    // had none would change what the file says about itself.
    for (const double weight : curve.weights) {
        out.pair(41, weight);
    }
    for (const Vec3& point : curve.controlPoints) {
        out.point(10, point);
    }
    for (const Vec3& point : spline.fitPoints) {
        out.point(11, point);
    }
}

void writePolyline(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& poly = static_cast<const PolylineEntity&>(entity);
    out.pair(100, "AcDbPolyline");
    out.pair(90, static_cast<std::int64_t>(poly.vertices.size()));
    out.pair(70, (poly.closed ? 1 : 0) | (poly.linetypeGeneration ? 128 : 0));
    if (poly.constantWidth != 0.0) {
        out.pair(43, poly.constantWidth);
    }

    // Once any vertex has a width, every vertex says its own, zeros included.
    // A reader that meets a vertex with no width carries the previous one
    // forward, so a wide first segment leaked into the thin rest of the line.
    const bool anyWidth = std::any_of(poly.vertices.begin(), poly.vertices.end(),
                                      [](const PolylineVertex& v) {
                                          return v.startWidth != 0.0 || v.endWidth != 0.0;
                                      });
    for (const PolylineVertex& vertex : poly.vertices) {
        // Vertex order and the placement of the optional codes *between* the
        // 10/20 pairs is what associates a bulge with its own vertex.
        out.pair(10, vertex.position.x);
        out.pair(20, vertex.position.y);
        if (anyWidth) {
            out.pair(40, vertex.startWidth);
            out.pair(41, vertex.endWidth);
        }
        if (vertex.bulge != 0.0) {
            out.pair(42, vertex.bulge);
        }
    }
}

/// Shared by TEXT, ATTDEF and ATTRIB, which differ only in their extra codes.
void writeTextBody(const TextEntity& text, const Database& db, DxfBuilder& out) {
    out.point(10, text.position);
    out.pair(40, text.height);
    out.pair(1, encodeDxfText(text.contents));

    if (text.rotation != 0.0) {
        out.pair(50, radToDeg(text.rotation));
    }
    if (text.widthFactor != 1.0) {
        out.pair(41, text.widthFactor);
    }
    if (text.obliqueAngle != 0.0) {
        out.pair(51, radToDeg(text.obliqueAngle));
    }
    out.pair(7, symbolName(db, text.styleHandle, "Standard"));

    int generationFlags = 0;
    if (text.isBackwards) {
        generationFlags |= 2;
    }
    if (text.isUpsideDown) {
        generationFlags |= 4;
    }
    if (generationFlags != 0) {
        out.pair(71, generationFlags);
    }
    if (text.horizontalAlignment != TextHorizontalAlignment::Left) {
        out.pair(72, static_cast<int>(text.horizontalAlignment));
    }
    // Code 11 is only meaningful when the text is justified, and writing a zero
    // one would drag left-aligned text to the origin on the next read.
    if (text.alignmentPoint.has_value()) {
        out.point(11, *text.alignmentPoint);
    }
}

void writeText(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& text = static_cast<const TextEntity&>(entity);
    out.pair(100, "AcDbText");
    writeTextBody(text, db, out);
    if (text.verticalAlignment != TextVerticalAlignment::Baseline) {
        out.pair(73, static_cast<int>(text.verticalAlignment));
    }
}

void writeMText(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& mtext = static_cast<const MTextEntity&>(entity);
    out.pair(100, "AcDbMText");
    out.point(10, mtext.position);
    out.pair(40, mtext.height);
    if (mtext.width != 0.0) {
        out.pair(41, mtext.width);
    }
    out.pair(71, static_cast<int>(mtext.attachment));

    // Contents over 250 characters are split across code-3 chunks with the
    // remainder in code 1, which is what other CAD software expects and what the reader
    // reassembles.
    constexpr std::size_t kChunk = 250;
    // Escaped before it is split, so no chunk boundary falls inside a
    // multi-byte character or an escape.
    const std::string contents = escapeNonAscii(encodeDxfText(mtext.contents));
    std::size_t offset = 0;
    while (contents.size() - offset > kChunk) {
        out.pair(3, contents.substr(offset, kChunk));
        offset += kChunk;
    }
    out.pair(1, contents.substr(offset));

    if (mtext.rotation != 0.0) {
        out.pair(50, radToDeg(mtext.rotation));
    }
    if (mtext.lineSpacingFactor != 1.0) {
        out.pair(44, mtext.lineSpacingFactor);
    }
    out.pair(7, symbolName(db, mtext.styleHandle, "Standard"));
}

void writeAttributeFlags(const AttributeFlags& flags, DxfBuilder& out) {
    int raw = 0;
    if (flags.isInvisible) {
        raw |= 1;
    }
    if (flags.isConstant) {
        raw |= 2;
    }
    if (flags.isVerifiable) {
        raw |= 4;
    }
    if (flags.isPreset) {
        raw |= 8;
    }
    out.pair(70, raw);
}

void writeAttributeDefinition(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& attdef = static_cast<const AttributeDefinitionEntity&>(entity);
    out.pair(100, "AcDbText");
    writeTextBody(attdef, db, out);
    out.pair(100, "AcDbAttributeDefinition");
    out.pair(3, encodeDxfText(attdef.prompt));
    out.pair(2, attdef.tag);
    writeAttributeFlags(attdef.flags, out);
    if (attdef.flags.fieldLength != 0) {
        out.pair(73, attdef.flags.fieldLength);
    }
    if (attdef.verticalAlignment != TextVerticalAlignment::Baseline) {
        out.pair(74, static_cast<int>(attdef.verticalAlignment));
    }
}

void writeAttribute(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& attribute = static_cast<const AttributeEntity&>(entity);
    out.pair(100, "AcDbText");
    writeTextBody(attribute, db, out);
    out.pair(100, "AcDbAttribute");
    out.pair(2, attribute.tag);
    writeAttributeFlags(attribute.flags, out);
    if (attribute.flags.fieldLength != 0) {
        out.pair(73, attribute.flags.fieldLength);
    }
    if (attribute.verticalAlignment != TextVerticalAlignment::Baseline) {
        out.pair(74, static_cast<int>(attribute.verticalAlignment));
    }
}

void writeInsert(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& insert = static_cast<const InsertEntity&>(entity);
    out.pair(100, "AcDbBlockReference");

    // Attributes follow the insert and are announced by code 66.
    if (!insert.attributeHandles.empty()) {
        out.pair(66, 1);
    }
    out.pair(2, symbolName(db, insert.blockHandle, "*U"));
    out.point(10, insert.position);

    if (insert.scale.x != 1.0) {
        out.pair(41, insert.scale.x);
    }
    if (insert.scale.y != 1.0) {
        out.pair(42, insert.scale.y);
    }
    if (insert.scale.z != 1.0) {
        out.pair(43, insert.scale.z);
    }
    if (insert.rotation != 0.0) {
        out.pair(50, radToDeg(insert.rotation));
    }
    if (insert.columnCount > 1) {
        out.pair(70, insert.columnCount);
    }
    if (insert.rowCount > 1) {
        out.pair(71, insert.rowCount);
    }
    if (insert.columnSpacing != 0.0) {
        out.pair(44, insert.columnSpacing);
    }
    if (insert.rowSpacing != 0.0) {
        out.pair(45, insert.rowSpacing);
    }
}

// --------------------------------------------------------------- annotation

void writeMline(const Entity& entity, const Database&, DxfBuilder& out) {
    const auto& mline = static_cast<const MlineEntity&>(entity);
    out.pair(100, "AcDbMline");
    out.pair(2, mline.styleName);
    // The handle only when the file supplied one. MLINESTYLE lives in the
    // OBJECTS section, which is carried through verbatim rather than parsed, so
    // there is nothing here to invent a handle from -- and a 340 pointing at
    // nothing is worse than a style named but not handled.
    if (!mline.styleHandle.isNull()) {
        out.pair(340, mline.styleHandle);
    }
    out.pair(40, mline.scale);
    out.pair(70, mline.justification);

    int flags = 1;  // Bit 1: unlocked, which is what an editable multiline is.
    if (mline.closed) {
        flags |= 2;
    }
    if (mline.suppressStartCaps) {
        flags |= 4;
    }
    if (mline.suppressEndCaps) {
        flags |= 8;
    }
    out.pair(71, flags);
    out.pair(72, static_cast<int>(mline.vertices.size()));
    out.pair(73, mline.elementCount);
    out.point(10, mline.startPoint);

    // Order is the format here, not decoration: every vertex is a position,
    // two vectors, and then its elements' parameter lists in element order. A
    // reader walks this sequentially, so a group emitted out of place silently
    // becomes part of the wrong element.
    for (const MlineVertex& vertex : mline.vertices) {
        out.point(11, vertex.position);
        out.point(12, vertex.direction);
        out.point(13, vertex.miterDirection);
        for (std::size_t element = 0; element < vertex.elementOffsets.size(); ++element) {
            const std::vector<double>& offsets = vertex.elementOffsets[element];
            out.pair(74, static_cast<int>(offsets.size()));
            for (const double offset : offsets) {
                out.pair(41, offset);
            }
            // The fill count is written whether or not there is any fill: it is
            // what tells a sequential reader that this element's parameters
            // have ended and the next element's are about to start.
            const std::vector<double>& fill =
                element < vertex.fillOffsets.size() ? vertex.fillOffsets[element]
                                                    : std::vector<double>{};
            out.pair(75, static_cast<int>(fill.size()));
            for (const double offset : fill) {
                out.pair(42, offset);
            }
        }
    }
}

void writeLeader(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& leader = static_cast<const LeaderEntity&>(entity);
    out.pair(100, "AcDbLeader");
    out.pair(3, symbolName(db, leader.styleHandle, kDimStyleStandard));
    out.pair(71, leader.hasArrowhead ? 1 : 0);
    // Straight segments only. A spline leader is a path type this application
    // does not draw, and writing 1 would promise a curve through the vertices
    // that the file's own geometry does not describe.
    out.pair(72, 0);
    // A leader this application made is written *unassociated*: other CAD software lays
    // out an associated note by rules of its own -- underlining it for the
    // width of its text, on the side it believes the text is -- and Slate does
    // not model those, so the file said one thing and other CAD software drew another.
    // Written as plain geometry with a free note, every consumer draws the
    // same thing; the link rides in XData and Slate restores it on read. A
    // leader read from somebody else's file keeps its association untouched.
    out.pair(73, leader.isSlateManaged() ? 3 : leader.annotationType);
    // Hookline direction: 1 when the landing runs the same way as the leader's
    // horizontal direction, which is the only case this writes.
    out.pair(74, 1);
    out.pair(75, leader.hasLanding ? 1 : 0);
    out.pair(40, leader.textHeight);
    out.pair(41, leader.textWidth);
    out.pair(76, static_cast<int>(leader.vertices.size()));
    for (const Vec3& vertex : leader.vertices) {
        out.point(10, vertex);
    }
    if (!leader.isSlateManaged() && !leader.annotationHandle.isNull()) {
        out.pair(340, leader.annotationHandle);
    }
    // Always written, whether or not the file supplied one. This is what tells
    // another application which side of the landing the note goes on; without
    // it other CAD software assumes +X, and a leader pointing leftwards has its own text
    // struck through by its landing.
    out.point(211, leader.landingDirection());
}

void writeDimension(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& dimension = static_cast<const DimensionEntity&>(entity);
    out.pair(100, "AcDbDimension");

    // The anonymous block holding the drawn result. Written back when the file
    // supplied one; a dimension this application created has none, and
    // other CAD software regenerates it on load rather than refusing the file.
    if (!dimension.blockHandle.isNull()) {
        out.pair(2, symbolName(db, dimension.blockHandle, ""));
    }
    out.point(10, dimension.definitionPoint);
    // Group 11 is where the text sits. A dimension read from a file carries
    // its own; one this application made had none, and a reader that trusts
    // the file -- most do -- put its text at the origin.
    Vec3 textMid = dimension.textMidPoint;
    if (!dimension.textPositionIsUserDefined && textMid.lengthSquared() == 0.0) {
        textMid = dimension.defaultTextPosition(db.dimStyleByHandle(dimension.styleHandle));
    }
    out.point(11, textMid);
    out.point(12, dimension.insertionPoint);

    // Bit 32 -- the block belongs to this dimension alone -- is not set:
    // other CAD software writes 0 for its own dimensions, block and all, and a file
    // claiming a block it does not carry says two things at once.
    int flags = static_cast<int>(dimension.type);
    if (dimension.isXDatum && dimension.type == DimensionType::Ordinate) {
        flags |= 64;
    }
    if (dimension.textPositionIsUserDefined) {
        flags |= 128;
    }
    out.pair(70, flags);

    out.pair(71, static_cast<int>(dimension.attachmentPoint));
    out.pair(72, dimension.lineSpacingStyle);
    if (dimension.lineSpacingFactor != 1.0) {
        out.pair(41, dimension.lineSpacingFactor);
    }
    // The measurement is recomputed rather than echoed, so a dimension whose
    // points were edited does not save a stale value.
    out.pair(42, dimension.measure());
    if (!dimension.textOverride.empty()) {
        out.pair(1, encodeDxfText(dimension.textOverride));
    }
    if (dimension.horizontalDirection != 0.0) {
        out.pair(51, radToDeg(dimension.horizontalDirection));
    }
    if (dimension.hasTextRotationOverride) {
        out.pair(53, radToDeg(dimension.textRotationOverride));
    }
    out.pair(3, symbolName(db, dimension.styleHandle, kDimStyleStandard));

    switch (dimension.type) {
        case DimensionType::Rotated:
        case DimensionType::Aligned:
            // Both start as an aligned dimension; a rotated one adds a second
            // subclass marker after its own data. The order is part of the
            // format, not a stylistic choice.
            out.pair(100, "AcDbAlignedDimension");
            out.point(13, dimension.point13);
            out.point(14, dimension.point14);
            if (dimension.type == DimensionType::Rotated) {
                out.pair(50, radToDeg(dimension.rotation));
            }
            if (dimension.obliqueAngle != 0.0) {
                out.pair(52, radToDeg(dimension.obliqueAngle));
            }
            if (dimension.type == DimensionType::Rotated) {
                out.pair(100, "AcDbRotatedDimension");
            }
            break;

        case DimensionType::Radius:
            out.pair(100, "AcDbRadialDimension");
            out.point(15, dimension.point15);
            out.pair(40, dimension.leaderLength);
            break;

        case DimensionType::Diameter:
            out.pair(100, "AcDbDiametricDimension");
            out.point(15, dimension.point15);
            out.pair(40, dimension.leaderLength);
            break;

        case DimensionType::Angular3Point:
            out.pair(100, "AcDb3PointAngularDimension");
            out.point(13, dimension.point13);
            out.point(14, dimension.point14);
            out.point(15, dimension.point15);
            break;

        case DimensionType::Angular2Line:
            out.pair(100, "AcDb2LineAngularDimension");
            out.point(13, dimension.point13);
            out.point(14, dimension.point14);
            out.point(15, dimension.point15);
            out.point(16, dimension.point16);
            break;

        case DimensionType::Ordinate:
            out.pair(100, "AcDbOrdinateDimension");
            out.point(13, dimension.point13);
            out.point(14, dimension.point14);
            break;

        default:
            break;
    }
}

void writeHatch(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& hatch = static_cast<const HatchEntity&>(entity);
    (void)db;
    out.pair(100, "AcDbHatch");

    // An elevation point rather than a plain elevation: HATCH is planar and
    // DXF records the plane's origin here.
    out.point(10, Vec3{0.0, 0.0, hatch.elevation});
    out.point(210, Vec3::unitZ());

    out.pair(2, hatch.pattern.name);
    out.pair(70, hatch.pattern.isSolid);
    out.pair(71, hatch.isAssociative);
    out.pair(91, static_cast<int>(hatch.paths.size()));

    for (const HatchBoundaryPath& path : hatch.paths) {
        out.pair(92, path.flags.toDxf());

        if (path.flags.isPolyline) {
            const bool hasBulge =
                std::any_of(path.vertices.begin(), path.vertices.end(),
                            [](const PolylineVertex& v) { return v.bulge != 0.0; });
            out.pair(72, hasBulge);
            out.pair(73, path.closed);
            out.pair(93, static_cast<int>(path.vertices.size()));
            for (const PolylineVertex& vertex : path.vertices) {
                out.pair(10, vertex.position.x);
                out.pair(20, vertex.position.y);
                if (hasBulge) {
                    out.pair(42, vertex.bulge);
                }
            }
        } else {
            out.pair(93, static_cast<int>(path.edges.size()));
            for (const HatchEdge& edge : path.edges) {
                out.pair(72, static_cast<int>(edge.kind));
                switch (edge.kind) {
                    case HatchEdge::Kind::Line:
                        out.pair(10, edge.start.x);
                        out.pair(20, edge.start.y);
                        out.pair(11, edge.end.x);
                        out.pair(21, edge.end.y);
                        break;
                    case HatchEdge::Kind::Arc:
                        out.pair(10, edge.center.x);
                        out.pair(20, edge.center.y);
                        out.pair(40, edge.radius);
                        out.pair(50, radToDeg(edge.startAngle));
                        out.pair(51, radToDeg(edge.endAngle));
                        out.pair(73, edge.counterClockwise);
                        break;
                    case HatchEdge::Kind::Ellipse:
                        out.pair(10, edge.center.x);
                        out.pair(20, edge.center.y);
                        out.pair(11, edge.majorAxis.x);
                        out.pair(21, edge.majorAxis.y);
                        out.pair(40, edge.ratio);
                        out.pair(50, radToDeg(edge.startAngle));
                        out.pair(51, radToDeg(edge.endAngle));
                        out.pair(73, edge.counterClockwise);
                        break;
                    case HatchEdge::Kind::Spline:
                        out.pair(94, edge.degree);
                        out.pair(73, edge.rational);
                        out.pair(74, edge.periodic);
                        out.pair(95, static_cast<int>(edge.knots.size()));
                        out.pair(96, static_cast<int>(edge.controlPoints.size()));
                        for (const double knot : edge.knots) {
                            out.pair(40, knot);
                        }
                        for (std::size_t i = 0; i < edge.controlPoints.size(); ++i) {
                            out.pair(10, edge.controlPoints[i].x);
                            out.pair(20, edge.controlPoints[i].y);
                            if (i < edge.weights.size()) {
                                out.pair(42, edge.weights[i]);
                            }
                        }
                        out.pair(97, static_cast<int>(edge.fitPoints.size()));
                        for (const Vec3& point : edge.fitPoints) {
                            out.pair(11, point.x);
                            out.pair(21, point.y);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        out.pair(97, static_cast<int>(path.sourceHandles.size()));
        for (const Handle source : path.sourceHandles) {
            out.pair(330, source);
        }
    }

    out.pair(75, static_cast<int>(hatch.style));
    out.pair(76, hatch.patternType);

    if (!hatch.pattern.isSolid) {
        out.pair(52, radToDeg(hatch.pattern.angle));
        out.pair(41, hatch.pattern.scale);
        out.pair(77, hatch.pattern.isDouble);
        out.pair(78, static_cast<int>(hatch.pattern.lines.size()));
        for (const HatchPatternLine& line : hatch.pattern.lines) {
            out.pair(53, radToDeg(line.angle));
            out.pair(43, line.basePoint.x);
            out.pair(44, line.basePoint.y);
            out.pair(45, line.offset.x);
            out.pair(46, line.offset.y);
            out.pair(79, static_cast<int>(line.dashes.size()));
            for (const double dash : line.dashes) {
                out.pair(49, dash);
            }
        }
    }

    out.pair(47, hatch.pixelSize);
    out.pair(98, static_cast<int>(hatch.seedPoints.size()));
    for (const Vec3& seed : hatch.seedPoints) {
        out.pair(10, seed.x);
        out.pair(20, seed.y);
    }

    if (hatch.gradient.enabled) {
        out.pair(450, 1);
        out.pair(451, 0);
        out.pair(452, hatch.gradient.isSingleColor);
        out.pair(453, 2);
        out.pair(460, hatch.gradient.angle);
        out.pair(461, hatch.gradient.tint);
        out.pair(462, hatch.gradient.shift);
        out.pair(463, static_cast<std::int64_t>(hatch.gradient.color1.toRgb()));
        out.pair(470, hatch.gradient.name);
    } else {
        // Written for a plain fill too, as other CAD software does: left out, the DWG
        // converter filled the gap with a single-colour gradient flag.
        out.pair(450, 0);
        out.pair(451, 0);
        out.pair(452, 0);
        out.pair(453, 0);
        out.pair(460, 0.0);
        out.pair(461, 0.0);
        out.pair(462, 0.0);
        out.pair(470, "");
    }
}

void writeViewport(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& viewport = static_cast<const ViewportEntity&>(entity);
    (void)db;
    out.pair(100, "AcDbViewport");

    out.point(10, viewport.center);
    out.pair(40, viewport.width);
    out.pair(41, viewport.height);
    out.pair(68, viewport.viewportId);
    out.pair(69, viewport.viewportId);

    out.pair(12, viewport.viewCenter.x);
    out.pair(22, viewport.viewCenter.y);
    out.pair(45, viewport.viewHeight);
    if (viewport.twist != 0.0) {
        out.pair(51, radToDeg(viewport.twist));
    }
    out.point(16, viewport.viewDirection * viewport.viewDirectionLength);
    out.point(17, viewport.viewTarget);

    out.pair(90, viewport.statusFlags);
    for (const Handle layer : viewport.frozenLayerHandles) {
        out.pair(331, layer);
    }
    if (!viewport.clipEntityHandle.isNull()) {
        out.pair(340, viewport.clipEntityHandle);
    }
}

}  // namespace

// ------------------------------------------------------------------ DxfBuilder

/// A string the way a DXF in a legacy code page carries characters outside
/// it: each as `\U+XXXX`. The file declares ANSI_1252, and a reader honouring
/// that turns raw UTF-8 into two wrong characters per accented letter; a
/// degree sign came back as "Â°". The escape is what other CAD software
/// writes, and what every reader of these files decodes.
[[nodiscard]] std::string escapeNonAscii(std::string_view utf8) {
    bool plain = true;
    for (const char c : utf8) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            plain = false;
            break;
        }
    }
    if (plain) {
        return std::string{utf8};
    }
    std::string out;
    out.reserve(utf8.size() + 8);
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80) {
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }
        std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (length == 1 || i + length > utf8.size()) {
            // Not UTF-8 after all; passed through rather than invented.
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }
        unsigned int codePoint = lead & (length == 2 ? 0x1Fu : length == 3 ? 0x0Fu : 0x07u);
        for (std::size_t k = 1; k < length; ++k) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        if (codePoint > 0xFFFF) {
            out.append(utf8.substr(i, length));
        } else {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\U+%04X", codePoint);
            out += buffer;
        }
        i += length;
    }
    return out;
}

void DxfBuilder::pair(int code, std::string_view value) {
    text_ += std::to_string(code);
    text_ += "\r\n";
    text_ += escapeNonAscii(value);
    text_ += "\r\n";
}

std::string encodeDxfText(std::string_view utf8) {
    // The three symbols every drafter types, spelled the way older files
    // spell them, so a callout written here reads the same in any file.
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        if (utf8.compare(i, 2, "\xC2\xB0") == 0) {
            out += "%%d";
            i += 2;
        } else if (utf8.compare(i, 3, "\xE2\x8C\x80") == 0) {
            out += "%%c";
            i += 3;
        } else if (utf8.compare(i, 2, "\xC2\xB1") == 0) {
            out += "%%p";
            i += 2;
        } else {
            out.push_back(utf8[i]);
            ++i;
        }
    }
    return out;
}

void DxfBuilder::pair(int code, double value) {
    pair(code, std::string_view{formatDouble(value)});
}

void DxfBuilder::pair(int code, int value) {
    pair(code, std::string_view{std::to_string(value)});
}

void DxfBuilder::pair(int code, std::int64_t value) {
    pair(code, std::string_view{std::to_string(value)});
}

void DxfBuilder::pair(int code, Handle handle) {
    pair(code, std::string_view{handle.toString()});
}

void DxfBuilder::pair(int code, bool value) {
    pair(code, value ? std::string_view{"1"} : std::string_view{"0"});
}

void DxfBuilder::point(int xCode, const Vec3& value, bool includeZ) {
    pair(xCode, value.x);
    pair(xCode + 10, value.y);
    if (includeZ && value.z != 0.0) {
        pair(xCode + 20, value.z);
    }
}

void DxfBuilder::identity(const DbObject& object) {
    identity(object, 5);
}

void DxfBuilder::identity(const DbObject& object, int handleCode) {
    pair(handleCode, object.handle);
    if (object.preserved == nullptr) {
        return;
    }
    for (const UnknownGroup& group : object.preserved->applicationGroups) {
        std::visit([&](const auto& value) { pair(group.code, value); }, group.value);
    }
}

void DxfBuilder::preserved(const DbObject& object) { preserved(object, {}); }

void DxfBuilder::preserved(const DbObject& object, std::span<const UnknownGroup> extra) {
    const auto emit = [this](const UnknownGroup& group) {
        std::visit([&](const auto& value) { pair(group.code, value); }, group.value);
    };

    if (object.preserved != nullptr) {
        for (const UnknownGroup& group : object.preserved->unknownCodes) {
            emit(group);
        }
    }
    for (const UnknownGroup& group : extra) {
        emit(group);
    }
    if (object.preserved != nullptr) {
        // Then the embedded object, in the order it was read. It must follow
        // the owning object's own groups and precede XData, which is where the
        // code-101 marker sat in the file.
        for (const UnknownGroup& group : object.preserved->embeddedObject) {
            emit(group);
        }
    }

    // XData is emitted last, as DXF requires, with each application's block
    // opened by its 1001 name.
    for (const auto& [application, items] : object.xdata) {
        pair(1001, application);
        for (const XDataItem& item : items) {
            std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::string>) {
                        pair(item.code, value);
                    } else if constexpr (std::is_same_v<T, double>) {
                        pair(item.code, value);
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        pair(item.code, value);
                    } else {
                        pair(item.code, value);
                    }
                },
                item.value);
        }
    }
}

void writeImage(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& image = static_cast<const RasterImageEntity&>(entity);
    (void)db;
    out.pair(100, "AcDbRasterImage");
    out.pair(90, 0);

    out.point(10, image.origin);
    out.point(11, image.uVector);
    out.point(12, image.vVector);
    // The pixel size is two codes, not a point: there is no 33, and writing a
    // zero one would be a Z coordinate on a count.
    out.pair(13, image.widthPixels);
    out.pair(23, image.heightPixels);

    out.pair(340, image.definitionHandle);

    int flags = 0;
    if (image.showImage) { flags |= 1; }
    if (image.showWhenNotAligned) { flags |= 2; }
    if (image.useClippingBoundary) { flags |= 4; }
    if (image.transparent) { flags |= 8; }
    out.pair(70, flags);

    out.pair(280, image.clippingEnabled ? 1 : 0);
    out.pair(281, image.brightness);
    out.pair(282, image.contrast);
    out.pair(283, image.fade);

    if (!image.reactorHandle.isNull()) {
        out.pair(360, image.reactorHandle);
    }

    out.pair(71, image.clipBoundaryType);
    out.pair(91, static_cast<int>(image.clipVertices.size()));
    for (const Vec3& vertex : image.clipVertices) {
        out.pair(14, vertex.x);
        out.pair(24, vertex.y);
    }
    out.pair(290, image.clipInverted ? 1 : 0);
}

void writeUnderlay(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& underlay = static_cast<const UnderlayEntity&>(entity);
    (void)db;
    out.pair(100, "AcDbUnderlayReference");

    out.pair(340, underlay.definitionHandle);
    out.point(10, underlay.position);
    out.pair(41, underlay.scale.x);
    out.pair(42, underlay.scale.y);
    out.pair(43, underlay.scale.z);
    out.pair(50, radToDeg(underlay.rotation));

    int flags = 0;
    if (underlay.clippingEnabled) { flags |= 1; }
    if (underlay.underlayOn) { flags |= 2; }
    if (underlay.monochrome) { flags |= 4; }
    if (underlay.adjustForBackground) { flags |= 8; }
    out.pair(280, flags);

    out.pair(281, underlay.contrast);
    out.pair(282, underlay.fade);

    for (const Vec3& vertex : underlay.clipVertices) {
        out.pair(11, vertex.x);
        out.pair(21, vertex.y);
    }
}

void writeOleFrame(const Entity& entity, const Database& db, DxfBuilder& out) {
    const auto& frame = static_cast<const OleFrameEntity&>(entity);
    (void)db;
    out.pair(100, "AcDbOle2Frame");
    out.point(10, frame.upperLeft);
    out.point(11, frame.lowerRight);
    // The version, type and payload follow from the preserved groups. See
    // OleFrameEntity for why none of it is re-derived here.
}

// -------------------------------------------------------- EntityWriterRegistry

void EntityWriterRegistry::registerWriter(std::string dxfType, EntityWriter writer) {
    // Keyed by the entity's own object type, which is what the writer has in
    // hand; the DXF name is the value.
    std::string objectType = dxfType;
    if (objectType == "LWPOLYLINE") {
        objectType = "Polyline";
    } else if (objectType == "ATTDEF") {
        objectType = "AttributeDefinition";
    } else if (objectType == "ATTRIB") {
        objectType = "Attribute";
    } else if (objectType == "MTEXT") {
        objectType = "MText";
    } else if (objectType == "OLE2FRAME") {
        objectType = "OleFrame";
    } else if (objectType == "PDFUNDERLAY") {
        objectType = "PdfUnderlay";
    } else if (objectType == "DWFUNDERLAY") {
        objectType = "DwfUnderlay";
    } else if (objectType == "DGNUNDERLAY") {
        objectType = "DgnUnderlay";
    } else {
        objectType = static_cast<char>(std::toupper(static_cast<unsigned char>(dxfType[0]))) +
                     [&] {
                         std::string rest = dxfType.substr(1);
                         for (char& c : rest) {
                             c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                         }
                         return rest;
                     }();
    }
    writers_[objectType] = Registration{std::move(dxfType), std::move(writer)};
}

const EntityWriter* EntityWriterRegistry::find(const std::string& objectType) const {
    const auto it = writers_.find(objectType);
    return it == writers_.end() ? nullptr : &it->second.writer;
}

std::string EntityWriterRegistry::dxfTypeFor(const Entity& entity) const {
    const auto it = writers_.find(entity.objectType());
    return it == writers_.end() ? std::string{} : it->second.dxfType;
}

EntityWriterRegistry EntityWriterRegistry::standard() {
    EntityWriterRegistry registry;
    registry.registerWriter("LINE", writeLine);
    registry.registerWriter("CIRCLE", writeCircle);
    registry.registerWriter("ARC", writeArc);
    registry.registerWriter("ELLIPSE", writeEllipse);
    registry.registerWriter("POINT", writePoint);
    registry.registerWriter("LWPOLYLINE", writePolyline);
    registry.registerWriter("SOLID", writeSolid);
    registry.registerWriter("SPLINE", writeSpline);
    registry.registerWriter("TEXT", writeText);
    registry.registerWriter("MTEXT", writeMText);
    registry.registerWriter("ATTDEF", writeAttributeDefinition);
    registry.registerWriter("ATTRIB", writeAttribute);
    registry.registerWriter("INSERT", writeInsert);
    registry.registerWriter("DIMENSION", writeDimension);
    registry.registerWriter("LEADER", writeLeader);
    registry.registerWriter("MLINE", writeMline);
    registry.registerWriter("HATCH", writeHatch);
    registry.registerWriter("VIEWPORT", writeViewport);
    registry.registerWriter("IMAGE", writeImage);
    registry.registerWriter("OLE2FRAME", writeOleFrame);
    registry.registerWriter("PDFUNDERLAY", writeUnderlay);
    registry.registerWriter("DWFUNDERLAY", writeUnderlay);
    registry.registerWriter("DGNUNDERLAY", writeUnderlay);
    return registry;
}

// -------------------------------------------------------------------- writer

DxfWriter::DxfWriter() : registry_(EntityWriterRegistry::standard()) {}

DxfWriter::DxfWriter(EntityWriterRegistry registry) : registry_(std::move(registry)) {}

Handle DxfWriter::nextSpareHandle() {
    return Handle{nextSpare_++};
}

std::string DxfWriter::write(const Database& db, const DxfWriteOptions& options) {
    diagnostics_.clear();
    // Every fabricated handle comes from beyond the database's own range, so
    // structural records like ENDBLK cannot collide with a real object.
    nextSpare_ = db.peekNextHandle().value();
    collectHatchReactors(db);
    collectOwnedAttributes(db);

    // The body is generated before the header, because writing it is what
    // allocates those fabricated handles and `$HANDSEED` has to exceed every
    // handle in the finished file. Estimating the count in the header instead
    // would be a hidden coupling that goes stale the first time a writer needs
    // one more — and a $HANDSEED that collides with a real object makes the
    // file unreadable to other CAD software and to ODA.
    DxfBuilder body;
    writeTables(db, body);
    writeBlocks(db, body);
    writeEntities(db, body);
    writeObjects(db, body);
    // Everything after OBJECTS, with ACDSDATA last because that is where the
    // format puts it.
    writePreservedSections(db, body, /*classes=*/false);

    DxfBuilder out;
    writeHeader(db, options, out);
    // CLASSES belongs immediately after HEADER, and it belongs in the file:
    // it is what tells other CAD software how to construct the object types a drawing
    // uses that are not in its base set.
    writePreservedSections(db, out, /*classes=*/true);
    out.append(body.take());
    out.pair(0, "EOF");
    return out.take();
}

void DxfWriter::writeHeader(const Database& db, const DxfWriteOptions& options,
                            DxfBuilder& out) {
    const DrawingSettings& settings = db.settings;

    out.pair(0, "SECTION");
    out.pair(2, "HEADER");

    out.pair(9, "$ACADVER");
    out.pair(1, options.version);
    out.pair(9, "$HANDSEED");
    // One past every handle written, including the fabricated ones. A seed
    // that names a handle the file also uses is rejected as a duplicate.
    out.pair(5, Handle{nextSpare_});
    out.pair(9, "$INSUNITS");
    out.pair(70, static_cast<int>(settings.insertionUnits));
    out.pair(9, "$LTSCALE");
    out.pair(40, settings.globalLinetypeScale);
    out.pair(9, "$PSLTSCALE");
    out.pair(40, settings.paperSpaceLinetypeScale);
    out.pair(9, "$LUPREC");
    out.pair(70, settings.linearPrecision);
    out.pair(9, "$AUPREC");
    out.pair(70, settings.angularPrecision);
    out.pair(9, "$ANGBASE");
    out.pair(50, radToDeg(settings.angleBase));
    out.pair(9, "$ANGDIR");
    out.pair(70, settings.angleClockwise ? 1 : 0);
    out.pair(9, "$LWDISPLAY");
    out.pair(290, settings.displayLineweights);
    out.pair(9, "$PDMODE");
    out.pair(70, settings.pointDisplayMode);
    out.pair(9, "$PDSIZE");
    out.pair(40, settings.pointDisplaySize);
    out.pair(9, "$LIMMIN");
    out.point(10, settings.limitsMin, false);
    out.pair(9, "$LIMMAX");
    out.point(10, settings.limitsMax, false);
    out.pair(9, "$INSBASE");
    out.point(10, settings.insertionBase);
    out.pair(9, "$ORTHOMODE");
    out.pair(70, settings.orthoEnabled ? 1 : 0);
    out.pair(9, "$OSMODE");
    out.pair(70, settings.osnapModes);
    out.pair(9, "$CLAYER");
    out.pair(8, settings.currentLayerName);
    out.pair(9, "$TEXTSTYLE");
    out.pair(7, settings.currentTextStyleName);
    out.pair(9, "$CELTYPE");
    out.pair(6, settings.currentLinetypeName);
    out.pair(9, "$DIMSTYLE");
    out.pair(2, settings.currentDimStyleName);

    // Variables read but not modelled are written back verbatim, so opening and
    // saving does not silently reset a drawing's configuration.
    for (const auto& [name, groups] : settings.unknownHeaderVariables) {
        out.pair(9, name);
        for (const UnknownGroup& group : groups) {
            std::visit([&](const auto& value) { out.pair(group.code, value); }, group.value);
        }
    }

    out.pair(0, "ENDSEC");
}

void DxfWriter::writeTables(const Database& db, DxfBuilder& out) {
    out.pair(0, "SECTION");
    out.pair(2, "TABLES");

    // Each TABLE header is itself an object and needs its own handle and
    // subclass marker. Omitting them leaves the table with a null id, and a
    // reader resolving a record's owner ends up casting the wrong object —
    // ODA reports it as "can't be cast to AcDbLayerTable" and refuses the file.
    //
    // The handle comes back out because every record in the table names it as
    // its owner. The file's own table handles are not reused: they are the one
    // kind of handle this writer re-issues, and a record pointing at the
    // original would point at nothing.
    Handle tableHandle;
    const auto beginTable = [&](const char* name, std::size_t count) {
        // The handle the table had, when the drawing came from a file. Only a
        // drawing built from nothing gets a fresh one.
        const auto known = db.symbolTableHeaders.find(name);
        tableHandle =
            known == db.symbolTableHeaders.end() ? nextSpareHandle() : known->second.handle;
        out.pair(0, "TABLE");
        out.pair(2, name);
        out.pair(5, tableHandle);
        // In the header slot, like any other record's.
        if (known != db.symbolTableHeaders.end()) {
            for (const UnknownGroup& group : known->second.applicationGroups) {
                std::visit([&](const auto& value) { out.pair(group.code, value); }, group.value);
            }
        }
        out.pair(100, "AcDbSymbolTable");
        out.pair(70, static_cast<int>(count));
        return tableHandle;
    };

    // --- tables this reader does not model, kept exactly as they arrived
    //
    // First, because VPORT is the usual one and the format puts it first. Their
    // TABLE headers came from the file with their own handles, so their records
    // still point at the right owner.
    for (const PreservedSection& section : db.preservedSections) {
        if (section.isTable) {
            writePreservedSection(section, out);
        }
    }

    // --- linetypes
    beginTable("LTYPE", db.linetypes().size());
    for (const Handle handle : db.linetypes().handles()) {
        const auto* linetype = db.get<Linetype>(handle);
        if (linetype == nullptr || linetype->erased) {
            continue;
        }
        out.pair(0, "LTYPE");
        out.identity(*linetype);
        out.pair(330, tableHandle);
        out.pair(100, "AcDbSymbolTableRecord");
        out.pair(100, "AcDbLinetypeTableRecord");
        out.pair(2, linetype->name);
        out.pair(70, 0);
        out.pair(3, linetype->description);
        out.pair(72, 65);  // 'A', the only alignment code other CAD software writes.
        out.pair(73, static_cast<int>(linetype->elements.size()));
        out.pair(40, linetype->patternLength());
        for (const LinetypeElement& element : linetype->elements) {
            out.pair(49, element.length);
            out.pair(74, 0);
        }
        out.preserved(*linetype);
    }
    out.pair(0, "ENDTAB");

    // --- layers
    beginTable("LAYER", db.layers().size());
    for (const Handle handle : db.layers().handles()) {
        const auto* layer = db.get<Layer>(handle);
        if (layer == nullptr || layer->erased) {
            continue;
        }
        out.pair(0, "LAYER");
        out.identity(*layer);
        out.pair(330, tableHandle);
        out.pair(100, "AcDbSymbolTableRecord");
        out.pair(100, "AcDbLayerTableRecord");
        out.pair(2, layer->name);

        int flags = 0;
        if (layer->isFrozen) {
            flags |= 1;
        }
        if (layer->isFrozenInNewViewports) {
            flags |= 2;
        }
        if (layer->isLocked) {
            flags |= 4;
        }
        if (layer->isXrefDependent) {
            flags |= 16;
        }
        out.pair(70, flags);

        // A layer that is off is written as a negative colour index, which is
        // the convention the reader expects on the way back in.
        const int aci = layer->color.method() == ColorMethod::Index
                            ? layer->color.index()
                            : rgbToAci(layer->color.toRgb());
        out.pair(62, layer->isOff ? -aci : aci);
        if (layer->color.method() == ColorMethod::TrueColor) {
            out.pair(420, static_cast<std::int64_t>(layer->color.rgb()));
        }

        out.pair(6, symbolName(db, layer->linetypeHandle, "Continuous"));
        out.pair(370, layer->lineweight);
        out.pair(290, layer->isPlottable);
        if (!layer->description.empty()) {
            out.pair(300, layer->description);
        }
        out.preserved(*layer);
    }
    out.pair(0, "ENDTAB");

    // --- text styles
    beginTable("STYLE", db.textStyles().size());
    for (const Handle handle : db.textStyles().handles()) {
        const auto* style = db.get<TextStyle>(handle);
        if (style == nullptr || style->erased) {
            continue;
        }
        out.pair(0, "STYLE");
        out.identity(*style);
        out.pair(330, tableHandle);
        out.pair(100, "AcDbSymbolTableRecord");
        out.pair(100, "AcDbTextStyleTableRecord");
        out.pair(2, style->name);

        int flags = 0;
        if (style->isShapeFile) {
            flags |= 1;
        }
        if (style->isVertical) {
            flags |= 4;
        }
        out.pair(70, flags);
        out.pair(40, style->height);
        out.pair(41, style->widthFactor);
        out.pair(50, radToDeg(style->obliqueAngle));

        int generationFlags = 0;
        if (style->isBackwards) {
            generationFlags |= 2;
        }
        if (style->isUpsideDown) {
            generationFlags |= 4;
        }
        out.pair(71, generationFlags);
        out.pair(3, style->fontFile);
        out.pair(4, style->bigFontFile);
        out.preserved(*style);
    }
    // Shape-file entries, which have no name to be indexed by and so are held
    // verbatim rather than as text styles.
    if (const auto header = db.symbolTableHeaders.find("STYLE");
        header != db.symbolTableHeaders.end()) {
        for (const std::vector<UnknownGroup>& record : header->second.unmodelledRecords) {
            for (const UnknownGroup& group : record) {
                std::visit([&](const auto& value) { out.pair(group.code, value); }, group.value);
            }
        }
    }
    out.pair(0, "ENDTAB");

    // --- dimension styles
    beginTable("DIMSTYLE", db.dimStyles().size());
    for (const Handle handle : db.dimStyles().handles()) {
        const auto* style = db.get<DimStyle>(handle);
        if (style == nullptr || style->erased) {
            continue;
        }
        out.pair(0, "DIMSTYLE");
        // 105, not 5. DIMSTYLE is the one table record whose handle uses a
        // different code, and writing 5 here makes other CAD software reject the file.
        out.identity(*style, 105);
        out.pair(330, tableHandle);
        out.pair(100, "AcDbSymbolTableRecord");
        out.pair(100, "AcDbDimStyleTableRecord");
        out.pair(2, style->name);
        out.pair(70, 0);

        out.pair(3, style->linearPostfix);
        out.pair(40, style->overallScale);
        out.pair(41, style->arrowSize);
        out.pair(42, style->extensionLineOffset);
        out.pair(43, style->baselineSpacing);
        out.pair(44, style->extensionLineExtend);
        out.pair(45, style->roundOff);
        out.pair(46, style->dimensionLineExtend);
        out.pair(49, style->fixedExtensionLength);

        out.pair(73, style->textInsideHorizontal);
        out.pair(74, style->textOutsideHorizontal);
        out.pair(75, style->suppressExtensionLine1);
        out.pair(76, style->suppressExtensionLine2);
        out.pair(77, static_cast<int>(style->textVerticalPosition));
        out.pair(78, style->linearZeroSuppression.toDimzin());
        out.pair(79, style->angularZeroSuppression.toDimzin());

        out.pair(140, style->textHeight);
        out.pair(141, style->centerMarkSize);
        out.pair(144, style->linearScale);
        out.pair(147, style->textGap);

        out.pair(173, style->separateArrowBlocks);
        out.pair(176, style->dimensionLineColor.method() == ColorMethod::Index
                          ? style->dimensionLineColor.index()
                          : 0);
        out.pair(177, style->extensionLineColor.method() == ColorMethod::Index
                          ? style->extensionLineColor.index()
                          : 0);
        out.pair(178, style->textColor.method() == ColorMethod::Index ? style->textColor.index()
                                                                      : 0);
        out.pair(179, style->angularPrecision);

        out.pair(271, style->linearPrecision);
        out.pair(275, static_cast<int>(style->angularFormat));
        out.pair(277, static_cast<int>(style->linearFormat));
        out.pair(278, static_cast<int>(style->decimalSeparator));
        out.pair(280, static_cast<int>(style->textHorizontalPosition));
        out.pair(281, style->suppressDimensionLine1);
        out.pair(282, style->suppressDimensionLine2);
        out.pair(290, style->fixedExtensionLines);
        out.pair(371, style->dimensionLineWeight);
        out.pair(372, style->extensionLineWeight);

        if (!style->textStyleHandle.isNull()) {
            out.pair(340, style->textStyleHandle);
        }
        if (!style->arrowBlock.isNull()) { out.pair(342, style->arrowBlock); }
        if (!style->arrowBlock1.isNull()) { out.pair(343, style->arrowBlock1); }
        if (!style->arrowBlock2.isNull()) { out.pair(344, style->arrowBlock2); }

        out.preserved(*style);
    }
    out.pair(0, "ENDTAB");

    // --- registered applications
    //
    // Every XData block names an application, and a name with no APPID entry
    // is a dangling reference. ACAD is always registered because dimension
    // style overrides and countless third-party tools use it.
    {
        // The table the file arrived with, in its own order, then anything the
        // drawing names that it somehow lacked. Rebuilding it from scratch was
        // wrong twice over: it dropped registrations no XData happened to use,
        // and it only ever looked at *entities* — XData hangs off block records,
        // layers and dictionary objects too, so `AcDbDynamicBlockGUID` on a
        // block record went unregistered and the result was a DXF that ODA's
        // reader rejects outright with "Invalid RegApp".
        std::vector<std::string> applications = db.registeredApplications;
        std::set<std::string> seen{applications.begin(), applications.end()};

        const auto require = [&](const std::string& application) {
            if (!application.empty() && seen.insert(application).second) {
                applications.push_back(application);
            }
        };

        // ACAD is always registered: dimension style overrides and countless
        // third-party tools use it.
        require("ACAD");
        for (const Handle handle : db.objectHandles()) {
            const DbObject* object = db.getObject(handle);
            if (object == nullptr || object->erased) {
                continue;
            }
            for (const auto& [application, items] : object->xdata) {
                require(application);
            }
        }

        beginTable("APPID", applications.size());
        for (const std::string& application : applications) {
            out.pair(0, "APPID");
            out.pair(5, nextSpareHandle());
            out.pair(330, tableHandle);
            out.pair(100, "AcDbSymbolTableRecord");
            out.pair(100, "AcDbRegAppTableRecord");
            out.pair(2, application);
            out.pair(70, 0);
        }
    }
    out.pair(0, "ENDTAB");

    // --- block records
    //
    // The table that makes a block a *thing* rather than a section heading.
    // Everything that refers to a block — a layout, a dimension style's
    // arrowheads, every contained entity's owner link — names the record's
    // handle, so omitting this table leaves all of them pointing at nothing.
    // A reader that resolves owners rejects the file outright; ODA reports it
    // as "Null object Id".
    beginTable("BLOCK_RECORD", db.blocks().size());
    for (const Handle handle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(handle);
        if (block == nullptr || block->erased) {
            continue;
        }
        out.pair(0, "BLOCK_RECORD");
        out.identity(*block);
        out.pair(330, tableHandle);
        out.pair(100, "AcDbSymbolTableRecord");
        out.pair(100, "AcDbBlockTableRecord");
        out.pair(2, block->name);
        // The layout this block *is* the space of, for the paper-space records.
        // Dropping it left every sheet's block record disowned by its layout.
        // Written for every record, null included, because other CAD software writes `340
        // 0` on ordinary blocks and a reader keying on the code's presence
        // should find it where it expects it.
        out.pair(340, block->layoutHandle);
        out.pair(70, block->units);
        // Explodability and scaling, which other CAD software writes for every record.
        out.pair(280, block->allowsExploding ? 1 : 0);
        out.pair(281, block->allowsNonUniformScaling ? 1 : 0);
        // And everything this reader did not interpret, which for a block
        // record is a great deal: the `102 {BLKREFS}` list naming every INSERT
        // that uses the block — other CAD software's answer to "is this block in use" —
        // and the thumbnail a design-centre preview is drawn from.
        out.preserved(*block);
    }
    out.pair(0, "ENDTAB");

    out.pair(0, "ENDSEC");
}

void DxfWriter::writeBlocks(const Database& db, DxfBuilder& out) {
    out.pair(0, "SECTION");
    out.pair(2, "BLOCKS");

    for (const Handle handle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(handle);
        if (block == nullptr || block->erased) {
            continue;
        }
        // Model space and the *first* paper space put their entities in the
        // ENTITIES section; every other layout keeps its own inside its block
        // here. That split is not a style choice -- it is what other CAD software and the
        // ODA converter write, and what they read back.
        //
        // Writing every layout's entities into ENTITIES instead looked right,
        // because this reader resolves each entity's owner handle and put them
        // back where they belonged. Other readers do not: they take containment
        // and code 67, so a drawing saved that way opened with all seven
        // sheets emptied and their title blocks and viewports piled into model
        // space. The round trip through this application alone could never see
        // it -- only writing a DWG and reading it back could.
        // By normalised name: a file may spell the sheet block in capitals,
        // and treating that as an ordinary block wrote every sheet's contents
        // inside it, where other readers take them for model space.
        const bool headerOnly = block->isModelSpace() ||
                                normalizeSymbolName(block->name) == normalizeSymbolName(kPaperSpace);

        out.pair(0, "BLOCK");
        // Code 5 is the BLOCK entity's own handle and code 330 is the
        // BLOCK_RECORD's — and it is the record's that everything else in the
        // file references. Writing the record handle as 330 keeps the reader
        // and writer symmetric and matches what other CAD software emits; writing it as
        // code 5 instead would silently renumber every block on save.
        out.pair(5, nextSpareHandle());
        out.pair(330, block->handle);
        out.pair(100, "AcDbEntity");
        out.pair(8, "0");
        out.pair(100, "AcDbBlockBegin");
        out.pair(2, block->name);

        int flags = 0;
        if (block->xrefType == XrefType::Attachment) {
            flags |= 4;
        } else if (block->xrefType == XrefType::Overlay) {
            flags |= 12;
        }
        if (block->isXrefDependent) {
            flags |= 16;
        }
        out.pair(70, flags);
        out.point(10, block->origin);
        out.pair(3, block->name);
        // The *stored* path is written back, never the resolved one: rewriting
        // a portable relative path as absolute breaks the project for everyone
        // else who opens it.
        out.pair(1, block->xrefPath);

        if (!headerOnly) {
            for (const Entity* entity : db.entitiesIn(*block)) {
                writeEntity(*entity, db, out);
            }
        }

        out.pair(0, "ENDBLK");
        out.pair(5, nextSpareHandle());
        out.pair(330, block->handle);
        out.pair(100, "AcDbEntity");
        out.pair(8, "0");
        out.pair(100, "AcDbBlockEnd");
    }

    out.pair(0, "ENDSEC");
}

void DxfWriter::writeEntities(const Database& db, DxfBuilder& out) {
    out.pair(0, "SECTION");
    out.pair(2, "ENTITIES");

    // Every layout space, not just the model. The ENTITIES section holds all of
    // them, distinguished by the owner handle each entity already carries;
    // writing only model space silently deletes every sheet's title block,
    // notes and viewports the first time a drawing is saved.
    const BlockTableRecord* modelSpace =
        db.get<BlockTableRecord>(db.blocks().find(kModelSpace));
    if (modelSpace != nullptr) {
        for (const Entity* entity : db.entitiesIn(*modelSpace)) {
            writeEntity(*entity, db, out);
        }
    }

    // The first paper space only. The other layouts' entities are written
    // inside their own blocks, in the BLOCKS section, which is where every
    // other reader looks for them.
    const BlockTableRecord* paperSpace =
        db.get<BlockTableRecord>(db.blocks().find(kPaperSpace));
    if (paperSpace != nullptr && !paperSpace->erased) {
        for (const Entity* entity : db.entitiesIn(*paperSpace)) {
            // Code 67 marks paper space for readers that do not resolve the
            // owner handle. Emitted by writeCommonProperties, which can see the
            // owner; here we only have to make sure the entity is written.
            writeEntity(*entity, db, out);
        }
    }

    out.pair(0, "ENDSEC");
}

void DxfWriter::writePreservedSection(const PreservedSection& section, DxfBuilder& out) {
    for (const UnknownGroup& group : section.groups) {
        std::visit([&](const auto& value) { out.pair(group.code, value); }, group.value);
    }
}

void DxfWriter::writePreservedSections(const Database& db, DxfBuilder& out, bool classes) {
    const auto isAcdsData = [](const PreservedSection& section) {
        return section.name == "ACDSDATA";
    };

    const auto write = [&](const PreservedSection& section) {
        out.pair(0, "SECTION");
        out.pair(2, section.name);
        writePreservedSection(section, out);
        out.pair(0, "ENDSEC");
    };

    if (classes) {
        // The classes the drawing needs declared for objects it holds that
        // are not in the base set. Other CAD software drops an image or a
        // PDF page whose class the file never declared; a drawing that began
        // here has no CLASSES section at all, and one that came from a file
        // may lack the class for what was attached since.
        struct ClassRecord {
            const char* name;
            const char* cppClass;
            const char* application;
            int proxyFlags;
            int instances;
            bool isEntity;
        };
        static constexpr ClassRecord kImageClasses[] = {
            {"IMAGEDEF", "AcDbRasterImageDef", "ISM", 0, 3, false},
            {"IMAGEDEF_REACTOR", "AcDbRasterImageDefReactor", "ISM", 1, 3, false},
            {"IMAGE", "AcDbRasterImage", "ISM", 127, 4, true},
            {"RASTERVARIABLES", "AcDbRasterVariables", "ISM", 0, 3, false},
        };
        static constexpr ClassRecord kPdfClasses[] = {
            {"PDFDEFINITION", "AcDbPdfDefinition", "ObjectDBX Classes", 1153, 3, false},
            // Declared under the record name the entity is written with. A
            // declaration under any other name leaves the converter with an
            // entity of no known class, which it drops without a word.
            {"PDFUNDERLAY", "AcDbPdfReference", "ObjectDBX Classes", 1153, 3, true},
        };
        bool hasImages = false;
        bool hasPdfs = false;
        for (const Handle handle : db.objectHandles()) {
            const DbObject* object = db.getObject(handle);
            if (object == nullptr || object->erased) {
                continue;
            }
            hasImages = hasImages || dynamic_cast<const ImageDefinition*>(object) != nullptr ||
                        dynamic_cast<const RasterImageEntity*>(object) != nullptr;
            if (const auto* definition = dynamic_cast<const UnderlayDefinition*>(object);
                definition != nullptr && definition->kind == UnderlayKind::Pdf) {
                hasPdfs = true;
            }
            if (const auto* underlay = dynamic_cast<const UnderlayEntity*>(object);
                underlay != nullptr && underlay->kind == UnderlayKind::Pdf) {
                hasPdfs = true;
            }
        }
        std::vector<const ClassRecord*> needed;
        if (hasImages) {
            for (const ClassRecord& record : kImageClasses) {
                needed.push_back(&record);
            }
        }
        if (hasPdfs) {
            for (const ClassRecord& record : kPdfClasses) {
                needed.push_back(&record);
            }
        }

        const PreservedSection* existing = nullptr;
        for (const PreservedSection& section : db.preservedSections) {
            if (!section.isTable && section.name == "CLASSES") {
                existing = &section;
                break;
            }
        }
        std::set<std::string> declared;
        if (existing != nullptr) {
            for (const UnknownGroup& group : existing->groups) {
                if (group.code == 1) {
                    if (const auto* text = std::get_if<std::string>(&group.value)) {
                        declared.insert(*text);
                    }
                }
            }
        }
        std::vector<const ClassRecord*> missing;
        for (const ClassRecord* record : needed) {
            if (!declared.contains(record->name)) {
                missing.push_back(record);
            }
        }
        if (existing == nullptr && missing.empty()) {
            return;
        }

        out.pair(0, "SECTION");
        out.pair(2, "CLASSES");
        if (existing != nullptr) {
            writePreservedSection(*existing, out);
        }
        for (const ClassRecord* record : missing) {
            out.pair(0, "CLASS");
            out.pair(1, record->name);
            out.pair(2, record->cppClass);
            out.pair(3, record->application);
            out.pair(90, record->proxyFlags);
            out.pair(91, record->instances);
            out.pair(280, 0);
            out.pair(281, record->isEntity ? 1 : 0);
        }
        out.pair(0, "ENDSEC");
        return;
    }

    for (const PreservedSection& section : db.preservedSections) {
        if (section.isTable || section.name == "CLASSES") {
            continue;
        }
        if (isAcdsData(section)) {
            continue;  // Written after everything else, below.
        }
        write(section);
    }
    for (const PreservedSection& section : db.preservedSections) {
        if (!section.isTable && isAcdsData(section)) {
            write(section);
        }
    }
}

void DxfWriter::writeObjects(const Database& db, DxfBuilder& out) {
    // Layouts, plus everything from the file's own OBJECTS section that this
    // application kept verbatim rather than interpreting. Both have to be
    // written or a save deletes them.
    const std::vector<const Layout*> layouts = db.layouts();

    std::vector<const RawObject*> raw;
    std::vector<const FieldObject*> fields;
    // Every object read for its meaning but written back for its bytes. They
    // share a loop because they share that contract exactly.
    std::vector<std::pair<std::string, const DbObject*>> preservedObjects;
    for (const Handle handle : db.objectHandles()) {
        if (const auto* object = db.get<RawObject>(handle);
            object != nullptr && !object->erased) {
            raw.push_back(object);
        } else if (const auto* field = db.get<FieldObject>(handle);
                   field != nullptr && !field->erased) {
            fields.push_back(field);
        } else if (const auto* image = db.get<ImageDefinition>(handle);
                   image != nullptr && !image->erased) {
            preservedObjects.emplace_back("IMAGEDEF", image);
        } else if (const auto* underlay = db.get<UnderlayDefinition>(handle);
                   underlay != nullptr && !underlay->erased) {
            preservedObjects.emplace_back(
                std::string{underlayKindName(underlay->kind)} + "DEFINITION", underlay);
        }
    }

    if (layouts.empty() && raw.empty() && fields.empty() && preservedObjects.empty()) {
        return;
    }

    out.pair(0, "SECTION");
    out.pair(2, "OBJECTS");

    // The named object dictionary, when the drawing has none of its own.
    //
    // A drawing read from a file brings its dictionaries with it, as raw
    // objects, and they go back out untouched. One this application created has
    // none -- and without them the OBJECTS section is a bare LAYOUT owned by
    // nothing. Our own reader is lenient enough not to mind; a strict consumer
    // is not, and the ODA converter refused the file outright with "Object of
    // class AcDbLayout can't be cast to AcDbDictionary". Which meant every
    // drawing drawn here could be saved as DXF and never as DWG.
    const bool hasDictionaries = std::ranges::any_of(
        raw, [](const RawObject* object) { return object->dxfType == "DICTIONARY"; });

    // Definitions made here rather than read: an image or a PDF page attached
    // in this session. They have no bytes of their own to write back, so
    // their groups come from their fields, and each needs a place in the
    // named object dictionary that a file-born definition already has.
    struct FreshDefinition {
        const DbObject* object;
        std::string dictionary;
        std::string name;
    };
    std::vector<FreshDefinition> fresh;
    const auto stemOf = [](const std::string& path) {
        const std::size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::size_t dot = name.find_last_of('.');
        return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    };
    for (const auto& [dxfType, object] : preservedObjects) {
        if (object->preserved != nullptr) {
            continue;
        }
        if (const auto* image = dynamic_cast<const ImageDefinition*>(object)) {
            fresh.push_back({object, "ACAD_IMAGE_DICT", stemOf(image->fileName)});
        } else if (const auto* underlay = dynamic_cast<const UnderlayDefinition*>(object)) {
            std::string name = stemOf(underlay->fileName);
            if (!underlay->itemName.empty()) {
                name += " - " + underlay->itemName;
            }
            fresh.push_back({object,
                             "ACAD_" + std::string{underlayKindName(underlay->kind)} + "DEFINITIONS",
                             std::move(name)});
        }
    }

    // The root dictionary the file brought, and the dictionaries it names.
    const RawObject* root = nullptr;
    std::map<std::string, Handle> dictionaryFor;
    for (const RawObject* object : raw) {
        if (object->dxfType == "DICTIONARY" && object->ownerHandle.isNull()) {
            root = object;
            break;
        }
    }
    if (root != nullptr && root->preserved != nullptr) {
        std::string pendingName;
        for (const UnknownGroup& group : root->preserved->unknownCodes) {
            if (group.code == 3) {
                if (const auto* text = std::get_if<std::string>(&group.value)) {
                    pendingName = *text;
                }
            } else if (group.code == 350 && !pendingName.empty()) {
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, std::string>) {
                            if (const auto handle = Handle::tryParse(value)) {
                                dictionaryFor[pendingName] = *handle;
                            }
                        } else if constexpr (std::is_same_v<T, Handle>) {
                            dictionaryFor[pendingName] = value;
                        }
                    },
                    group.value);
                pendingName.clear();
            }
        }
    }

    // Dictionaries the fresh definitions need that the drawing lacks, and the
    // entries to add to the ones it has.
    std::vector<std::pair<std::string, Handle>> newDictionaries;
    std::map<Handle, std::vector<UnknownGroup>> extraEntries;
    for (const FreshDefinition& definition : fresh) {
        if (!dictionaryFor.contains(definition.dictionary)) {
            const Handle handle = nextSpareHandle();
            dictionaryFor[definition.dictionary] = handle;
            newDictionaries.emplace_back(definition.dictionary, handle);
            if (root != nullptr) {
                extraEntries[root->handle].push_back({3, definition.dictionary});
                extraEntries[root->handle].push_back({350, handle.toString()});
            }
        }
        const Handle dictionary = dictionaryFor[definition.dictionary];
        const bool isNew = std::ranges::any_of(
            newDictionaries, [&](const auto& entry) { return entry.second == dictionary; });
        if (!isNew) {
            extraEntries[dictionary].push_back({3, definition.name});
            extraEntries[dictionary].push_back({350, definition.object->handle.toString()});
        }
    }

    Handle layoutDictionary;
    Handle madeRoot;
    if (!hasDictionaries && (!layouts.empty() || !fresh.empty())) {
        madeRoot = nextSpareHandle();
        if (!layouts.empty()) {
            layoutDictionary = nextSpareHandle();
        }

        out.pair(0, "DICTIONARY");
        out.pair(5, madeRoot);
        // Owned by the drawing itself, which is what handle zero means here.
        out.pair(330, Handle::null());
        out.pair(100, "AcDbDictionary");
        out.pair(281, 1);
        if (!layouts.empty()) {
            out.pair(3, "ACAD_LAYOUT");
            out.pair(350, layoutDictionary);
        }
        for (const auto& [name, handle] : newDictionaries) {
            out.pair(3, name);
            out.pair(350, handle);
        }

        if (!layouts.empty()) {
            out.pair(0, "DICTIONARY");
            out.pair(5, layoutDictionary);
            out.pair(330, madeRoot);
            out.pair(100, "AcDbDictionary");
            out.pair(281, 1);
            for (const Layout* layout : layouts) {
                out.pair(3, layout->name);
                out.pair(350, layout->handle);
            }
        }
    }

    for (const auto& [name, handle] : newDictionaries) {
        out.pair(0, "DICTIONARY");
        out.pair(5, handle);
        out.pair(330, root != nullptr ? root->handle : madeRoot);
        out.pair(100, "AcDbDictionary");
        out.pair(281, 1);
        for (const FreshDefinition& definition : fresh) {
            if (definition.dictionary == name) {
                out.pair(3, definition.name);
                out.pair(350, definition.object->handle);
            }
        }
    }

    for (const RawObject* object : raw) {
        out.pair(0, object->dxfType);
        out.identity(*object);
        if (!object->ownerHandle.isNull()) {
            out.pair(330, object->ownerHandle);
        }
        const auto extra = extraEntries.find(object->handle);
        if (extra != extraEntries.end()) {
            out.preserved(*object, extra->second);
        } else {
            out.preserved(*object);
        }
    }

    // A field is read for its meaning and written back for its bytes.
    //
    // Nothing this application interprets about a field is stored anywhere but
    // the preserved groups, so there is nothing here to re-derive: the parsed
    // evaluator and code are a *view* of those groups. Emitting them again from
    // the parsed form would mean rebuilding the nested value structure the file
    // uses to cache results, from a partial understanding of it.
    for (const FieldObject* field : fields) {
        out.pair(0, "FIELD");
        out.identity(*field);
        if (!field->ownerHandle.isNull()) {
            out.pair(330, field->ownerHandle);
        }
        out.preserved(*field);
    }

    for (const auto& [dxfType, object] : preservedObjects) {
        out.pair(0, dxfType);
        out.identity(*object);
        const auto made = std::ranges::find_if(
            fresh, [&](const FreshDefinition& definition) { return definition.object == object; });
        if (made != fresh.end()) {
            // Made here: owned by its dictionary, groups from its fields.
            out.pair(330, dictionaryFor[made->dictionary]);
            if (const auto* image = dynamic_cast<const ImageDefinition*>(object)) {
                out.pair(100, "AcDbRasterImageDef");
                out.pair(90, 0);
                out.pair(1, image->fileName);
                out.pair(10, image->sizeInPixels.x);
                out.pair(20, image->sizeInPixels.y);
                out.pair(11, image->pixelSize.x);
                out.pair(21, image->pixelSize.y);
                out.pair(280, image->loaded ? 1 : 0);
                out.pair(281, image->resolutionUnits);
            } else if (const auto* underlay = dynamic_cast<const UnderlayDefinition*>(object)) {
                out.pair(100, "AcDbUnderlayDefinition");
                out.pair(1, underlay->fileName);
                out.pair(2, underlay->itemName);
            }
            out.preserved(*object);
            continue;
        }
        if (!object->ownerHandle.isNull()) {
            out.pair(330, object->ownerHandle);
        }
        out.preserved(*object);
    }

    for (const Layout* layout : layouts) {
        out.pair(0, "LAYOUT");
        out.identity(*layout);
        // A layout is owned by the ACAD_LAYOUT dictionary. The file's own when
        // it came from one, otherwise the one written above -- a layout owned
        // by nothing is what the converter rejected.
        const Handle owner =
            layout->ownerHandle.isNull() ? layoutDictionary : layout->ownerHandle;
        if (!owner.isNull()) {
            out.pair(330, owner);
        }
        out.pair(100, "AcDbPlotSettings");
        // Code 1 first as the page setup name, then again below as the layout
        // name. Both are code 1; only their order tells them apart, and a
        // reader takes the last.
        out.pair(1, "");
        // Code 2 is the printer configuration and code 4 the paper size name.
        // Swapping them is invisible until someone plots.
        out.pair(2, layout->plot.plotterName);
        out.pair(4, layout->plot.paperName);
        out.pair(40, layout->plot.marginLeft);
        out.pair(41, layout->plot.marginBottom);
        out.pair(42, layout->plot.marginRight);
        out.pair(43, layout->plot.marginTop);
        out.pair(44, layout->plot.paperWidth);
        out.pair(45, layout->plot.paperHeight);
        out.pair(72, static_cast<std::int64_t>(layout->plot.paperUnits));
        out.pair(142, layout->plot.numerator);
        out.pair(143, layout->plot.denominator);
        out.pair(73, layout->plot.rotation / 90);

        out.pair(100, "AcDbLayout");
        out.pair(1, layout->name);
        out.pair(70, 1);
        out.pair(71, layout->tabOrder);
        out.point(10, layout->limitsMin);
        out.point(11, layout->limitsMax);
        out.point(14, layout->minExtents);
        out.point(15, layout->maxExtents);
        // The sheet's block record, as a *second* code 330 inside the
        // AcDbLayout subclass. The first, above, is the owning dictionary.
        // Both are 330 and only their order distinguishes them, which is
        // exactly how the reader takes them.
        if (!layout->blockRecordHandle.isNull()) {
            out.pair(330, layout->blockRecordHandle);
        }
        out.preserved(*layout);
    }

    out.pair(0, "ENDSEC");
}

void DxfWriter::collectHatchReactors(const Database& db) {
    hatchReactors_.clear();
    for (const Handle blockHandle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(blockHandle);
        if (block == nullptr) {
            continue;
        }
        for (const Entity* entity : db.entitiesIn(*block)) {
            const auto* hatch = dynamic_cast<const HatchEntity*>(entity);
            if (hatch == nullptr || !hatch->isAssociative) {
                continue;
            }
            for (const HatchBoundaryPath& path : hatch->paths) {
                for (const Handle source : path.sourceHandles) {
                    hatchReactors_[source].push_back(hatch->handle);
                }
            }
        }
    }
}

namespace {

/// True when the entity came from a file with a reactor list of its own,
/// which is written back as read and must not be doubled.
bool hasPreservedReactors(const Entity& entity) {
    if (entity.preserved == nullptr) {
        return false;
    }
    for (const UnknownGroup& group : entity.preserved->applicationGroups) {
        if (const auto* text = std::get_if<std::string>(&group.value);
            text != nullptr && *text == "{ACAD_REACTORS") {
            return true;
        }
    }
    return false;
}

}  // namespace

void DxfWriter::writeCommonProperties(const Entity& entity, const Database& db,
                                      DxfBuilder& out) {
    out.identity(entity);
    // A boundary names the associative hatches it feeds. Without the list
    // other CAD software keeps the hatch but drops the association on open, which the
    // file said it had -- the tool that compares against other CAD software saw 71 go
    // from 1 to 0 on every hatch this application made.
    if (const auto reactors = hatchReactors_.find(entity.handle);
        reactors != hatchReactors_.end() && !hasPreservedReactors(entity)) {
        out.pair(102, "{ACAD_REACTORS");
        for (const Handle hatch : reactors->second) {
            out.pair(330, hatch);
        }
        out.pair(102, "}");
    }
    if (!entity.ownerHandle.isNull()) {
        out.pair(330, entity.ownerHandle);
    }
    out.pair(100, "AcDbEntity");
    // Code 67 says "paper space". The owner handle above says it too, and
    // this reader and the DWG converter go by the owner -- but a reader that
    // goes by the code, as other CAD software's own DXF output invites, would put every
    // sheet's title block and viewports into model space without it.
    if (const auto* owner = db.get<BlockTableRecord>(entity.ownerHandle);
        owner != nullptr && normalizeSymbolName(owner->name).compare(0, 12, "*PAPER_SPACE") == 0) {
        out.pair(67, 1);
    }
    out.pair(8, symbolName(db, entity.layerHandle, "0"));

    switch (entity.color.method()) {
        case ColorMethod::Index:
            out.pair(62, entity.color.index());
            break;
        case ColorMethod::TrueColor:
            out.pair(420, static_cast<std::int64_t>(entity.color.rgb()));
            break;
        case ColorMethod::ByBlock:
            out.pair(62, 0);
            break;
        case ColorMethod::ByLayer:
            // ByLayer is the default and is left implicit, which is what
            // other CAD software writes and keeps files smaller.
            break;
    }

    if (!entity.linetypeHandle.isNull()) {
        out.pair(6, symbolName(db, entity.linetypeHandle, "ByLayer"));
    }
    if (entity.linetypeScale != 1.0) {
        out.pair(48, entity.linetypeScale);
    }
    if (entity.lineweight != toRaw(Lineweight::ByLayer)) {
        out.pair(370, entity.lineweight);
    }
    if (!entity.visible) {
        out.pair(60, 1);
    }
    if (entity.transparency.has_value()) {
        // Transparency is stored as 0x02000000 | (255 - alpha).
        out.pair(440, static_cast<std::int64_t>(0x02000000 | (255 - *entity.transparency)));
    }
    // The extrusion direction is deliberately *not* written here: it belongs
    // after the entity's own subclass marker, and is written by writeEntity
    // once the entity's data is out. See there.
}

void DxfWriter::collectOwnedAttributes(const Database& db) {
    ownedAttributes_.clear();
    for (const Handle blockHandle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(blockHandle);
        if (block == nullptr) {
            continue;
        }
        for (const Entity* entity : db.entitiesIn(*block)) {
            if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
                ownedAttributes_.insert(insert->attributeHandles.begin(),
                                        insert->attributeHandles.end());
            }
        }
    }
}

void DxfWriter::writeEntity(const Entity& entity, const Database& db, DxfBuilder& out) {
    if (entity.erased) {
        return;
    }
    // An attribute belongs after the block reference that owns it, and
    // nowhere else: as a record of its own it is an entity no reader accepts.
    // It is written from the reference, below.
    if (ownedAttributes_.contains(entity.handle)) {
        return;
    }
    writeRecord(entity, db, out);

    const auto* insert = dynamic_cast<const InsertEntity*>(&entity);
    if (insert == nullptr || insert->attributeHandles.empty()) {
        return;
    }
    // The reference's attributes, then SEQEND to close the sequence -- the
    // shape other CAD software writes and every reader, this one included, expects.
    for (const Handle handle : insert->attributeHandles) {
        if (const Entity* attribute = db.getEntity(handle);
            attribute != nullptr && !attribute->erased) {
            writeRecord(*attribute, db, out);
        }
    }
    out.pair(0, "SEQEND");
    out.pair(5, nextSpareHandle());
    if (!entity.ownerHandle.isNull()) {
        out.pair(330, entity.ownerHandle);
    }
    out.pair(100, "AcDbEntity");
    out.pair(8, symbolName(db, entity.layerHandle, "0"));
}

void DxfWriter::writeRecord(const Entity& entity, const Database& db, DxfBuilder& out) {
    const std::string dxfType = registry_.dxfTypeFor(entity);
    const EntityWriter* writer = registry_.find(entity.objectType());

    if (writer == nullptr || dxfType.empty()) {
        // An entity read as an unsupported placeholder still knows its original
        // type and every group code it carried, so it can be written back
        // exactly as it arrived — which is the entire point of preserving it.
        if (entity.preserved != nullptr && !entity.preserved->originalType.empty()) {
            out.pair(0, entity.preserved->originalType);
            writeCommonProperties(entity, db, out);
            out.preserved(entity);
            return;
        }
        diagnostics_.push_back({DxfDiagnostic::Severity::Warning,
                                "No DXF writer for " + entity.objectType() +
                                    "; the object was omitted",
                                0});
        return;
    }

    out.pair(0, dxfType);
    writeCommonProperties(entity, db, out);
    (*writer)(entity, db, out);
    // The extrusion direction, after the entity's own data. Written among the
    // common properties -- before the subclass marker -- it was silently
    // dropped by the DWG converter, and an ellipse drawn with -Z came back
    // as the other three quarters of itself. other CAD software checks the tool made
    // this visible: the file checks.
    if (!(entity.normal.x == 0.0 && entity.normal.y == 0.0 && entity.normal.z > 0.0)) {
        out.point(210, entity.normal);
    }
    out.preserved(entity);
}

std::string writeDxf(const Database& db, const DxfWriteOptions& options) {
    DxfWriter writer;
    return writer.write(db, options);
}

void writeDxfFile(const Database& db, const std::filesystem::path& path,
                  const DxfWriteOptions& options) {
    const std::string text = writeDxf(db, options);

    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("Failed while writing " + path.string());
    }
}

}  // namespace cad::io
