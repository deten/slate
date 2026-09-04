#include "cad/db/DimensionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "cad/geom/Intersect.hpp"

namespace cad {
namespace {

/// Ratio of an arrowhead's half-width to its length.
///
/// A closed filled arrow in other CAD software is roughly six times longer than it is
/// wide. Fixed rather than styled because DWG has no variable for it — DIMASZ
/// scales the whole arrow.
constexpr double kArrowHalfWidthRatio = 1.0 / 6.0;

/// Below this, two points are the same point and a direction between them is
/// meaningless.
constexpr double kDegenerate = 1e-12;

[[nodiscard]] Vec3 safeDirection(const Vec3& from, const Vec3& to, const Vec3& fallback) {
    const Vec3 delta = to - from;
    return delta.lengthSquared() < kDegenerate ? fallback : delta.normalized();
}

/// A unit vector perpendicular to `direction` in the entity's plane.
[[nodiscard]] Vec3 perpendicular(const Vec3& direction, const Vec3& normal) {
    const Vec3 perp = normal.cross(direction);
    return perp.lengthSquared() < kDegenerate ? direction.perp2().normalized()
                                              : perp.normalized();
}

/// A closed filled arrowhead.
///
/// `direction` points from the tip back along the shaft, so an arrow at the end
/// of a dimension line points outwards by construction rather than by the
/// caller remembering to negate.
[[nodiscard]] std::array<Vec3, 3> arrowhead(const Vec3& tip, const Vec3& direction,
                                            const Vec3& normal, double length) {
    const Vec3 back = tip + direction * length;
    const Vec3 side = perpendicular(direction, normal) * (length * kArrowHalfWidthRatio);
    return {tip, back + side, back - side};
}

/// Keep text the right way up.
///
/// Dimension text is never read upside down, however the dimension is rotated:
/// past the vertical it flips rather than inverting. Without this, half the
/// dimensions in a rotated drawing are unreadable.
[[nodiscard]] double uprightAngle(double angle) {
    const double normalized = normalizeAngle(angle);
    if (normalized > kPi / 2.0 + 1e-9 && normalized < 3.0 * kPi / 2.0 - 1e-9) {
        return normalizeAngle(normalized + kPi);
    }
    return normalized;
}

/// Intersection of two infinite lines in the XY plane, if they meet.
[[nodiscard]] std::optional<Vec3> lineIntersection(const Vec3& a1, const Vec3& a2, const Vec3& b1,
                                                   const Vec3& b2) {
    const Vec3 da = a2 - a1;
    const Vec3 db = b2 - b1;
    const double denominator = da.cross2(db);
    if (std::abs(denominator) < 1e-12) {
        return std::nullopt;
    }
    const double t = (b1 - a1).cross2(db) / denominator;
    return a1 + da * t;
}

}  // namespace

const DimStyle& fallbackDimStyle() {
    static const DimStyle style = [] {
        DimStyle defaults;
        defaults.name = kDimStyleStandard;
        return defaults;
    }();
    return style;
}

// ------------------------------------------------------------ role accessors

Vec3 DimensionEntity::arcCenter() const {
    switch (type) {
        case DimensionType::Radius:
            // Group 10 is the centre, group 15 the point on the circumference.
            return definitionPoint;
        case DimensionType::Diameter:
            // Both stored points are on the circumference; the centre is
            // halfway between them and is not stored at all.
            return definitionPoint.midpoint(point15);
        case DimensionType::Angular3Point:
            return point15;
        case DimensionType::Angular2Line: {
            const auto vertex = lineIntersection(point13, point14, point15, definitionPoint);
            return vertex.value_or(point13);
        }
        default:
            return definitionPoint;
    }
}

Vec3 DimensionEntity::chordPoint() const {
    return type == DimensionType::Radius ? point15 : definitionPoint;
}

Vec3 DimensionEntity::angleVertex() const { return arcCenter(); }

double DimensionEntity::measure() const {
    switch (type) {
        case DimensionType::Rotated: {
            // The distance along the fixed direction, not between the points.
            // Measuring point-to-point here is the single most common way to
            // get a rotated dimension wrong.
            const Vec3 axis = Vec3{std::cos(rotation), std::sin(rotation), 0.0};
            return std::abs((point14 - point13).dot(axis));
        }
        case DimensionType::Aligned:
            return point13.distanceTo(point14);
        case DimensionType::Radius:
            return definitionPoint.distanceTo(point15);
        case DimensionType::Diameter:
            return definitionPoint.distanceTo(point15);
        case DimensionType::Angular3Point: {
            const Vec3 vertex = point15;
            const double start = (point13 - vertex).angle2();
            const double end = (point14 - vertex).angle2();
            return normalizeAngle(end - start);
        }
        case DimensionType::Angular2Line: {
            const Vec3 vertex = arcCenter();
            const double start = (point14 - vertex).angle2();
            const double end = (definitionPoint - vertex).angle2();
            return normalizeAngle(end - start);
        }
        case DimensionType::Ordinate: {
            const Vec3 offset = point13 - definitionPoint;
            return isXDatum ? offset.x : offset.y;
        }
        default:
            return 0.0;
    }
}

std::string DimensionEntity::measurementText(const DimStyle& style) const {
    // Three distinct meanings, all of which occur in real files: empty means
    // "the measurement", a single space means "nothing at all", and anything
    // else is an override in which `<>` stands for the measurement.
    if (textOverride == " ") {
        return {};
    }

    const bool angular =
        type == DimensionType::Angular2Line || type == DimensionType::Angular3Point;
    const std::string measured = angular ? formatAngularMeasurement(style, measure())
                                         : formatLinearMeasurement(style, measure());

    if (textOverride.empty()) {
        // The radial prefixes are part of the convention, not of the style:
        // a radius written without its R is ambiguous with a diameter.
        if (type == DimensionType::Radius && style.linearPostfix.empty()) {
            return "R" + measured;
        }
        if (type == DimensionType::Diameter && style.linearPostfix.empty()) {
            return "\xE2\x8C\x80" + measured;  // U+2300 DIAMETER SIGN.
        }
        return measured;
    }

    std::string result = textOverride;
    for (std::size_t at = result.find("<>"); at != std::string::npos;
         at = result.find("<>", at + measured.size())) {
        result.replace(at, 2, measured);
    }
    return result;
}

// ---------------------------------------------------------------- generation

DimensionGeometry DimensionEntity::generateLinear(const DimStyle& style) const {
    DimensionGeometry out;
    out.textHeight = style.scaled(style.textHeight);
    out.text = measurementText(style);

    const Vec3 p1 = point13;
    const Vec3 p2 = point14;

    // The dimension line's direction: the measurement axis for a rotated
    // dimension, the line between the points for an aligned one.
    Vec3 along = type == DimensionType::Aligned
                     ? safeDirection(p1, p2, Vec3::unitX())
                     : Vec3{std::cos(rotation), std::sin(rotation), 0.0};
    if (along.lengthSquared() < kDegenerate) {
        along = Vec3::unitX();
    }

    // Project the measured points onto the line through the dimension line
    // location. This is what makes a rotated dimension measure along its own
    // axis rather than between its points.
    const Vec3 origin = definitionPoint;
    const Vec3 dim1 = origin + along * (p1 - origin).dot(along);
    const Vec3 dim2 = origin + along * (p2 - origin).dot(along);

    const double arrowLength = style.scaled(style.arrowSize);
    const double span = dim1.distanceTo(dim2);

    // Extension lines. Their direction leans by the oblique angle, which is a
    // deviation from perpendicular: a file storing zero — which is nearly all
    // of them — must produce square extension lines.
    const Vec3 square = perpendicular(along, normal);
    const Vec3 lean = obliqueAngle == 0.0 ? square : square.rotatedAboutAxis(normal, obliqueAngle);

    const auto extensionLine = [&](const Vec3& measured, const Vec3& onLine, bool suppressed) {
        if (suppressed) {
            return;
        }
        const Vec3 direction = safeDirection(measured, onLine, lean);
        const double reach = measured.distanceTo(onLine);
        const double offset = style.scaled(style.extensionLineOffset);
        if (reach <= offset) {
            // The dimension line is closer to the geometry than the offset
            // allows, so there is no extension line to draw.
            return;
        }
        const Vec3 start = style.fixedExtensionLines
                               ? onLine - direction * style.scaled(style.fixedExtensionLength)
                               : measured + direction * offset;
        const Vec3 end = onLine + direction * style.scaled(style.extensionLineExtend);
        out.lines.push_back(LineSegment{start, end});
    };
    extensionLine(p1, dim1, style.suppressExtensionLine1);
    extensionLine(p2, dim2, style.suppressExtensionLine2);

    // Arrows go inside when there is room and flip outside when there is not,
    // which is what stops a 1mm dimension turning into a black blob.
    const bool arrowsInside = span >= 2.0 * arrowLength;
    const Vec3 toward2 = safeDirection(dim1, dim2, along);

    const double lineExtend =
        arrowsInside ? style.scaled(style.dimensionLineExtend) : 2.0 * arrowLength;
    const Vec3 lineStart = dim1 - toward2 * lineExtend;
    const Vec3 lineEnd = dim2 + toward2 * lineExtend;
    if (!style.suppressDimensionLine1 || !style.suppressDimensionLine2) {
        out.lines.push_back(LineSegment{lineStart, lineEnd});
    }

    const Vec3 arrow1Direction = arrowsInside ? toward2 : -toward2;
    out.arrowheads.push_back(arrowhead(dim1, arrow1Direction, normal, arrowLength));
    out.arrowheads.push_back(arrowhead(dim2, -arrow1Direction, normal, arrowLength));

    // Text.
    const double textAngle = style.textInsideHorizontal ? 0.0 : uprightAngle(along.angle2());
    out.textRotation = hasTextRotationOverride ? textRotationOverride : textAngle;
    out.textPosition = textPositionIsUserDefined ? textMidPoint : defaultTextPosition(style);
    return out;
}

DimensionGeometry DimensionEntity::generateRadial(const DimStyle& style) const {
    DimensionGeometry out;
    out.textHeight = style.scaled(style.textHeight);
    out.text = measurementText(style);

    const double arrowLength = style.scaled(style.arrowSize);
    const Vec3 center = arcCenter();

    if (type == DimensionType::Diameter) {
        // Both chord points are stored, so the leader is simply the chord.
        const Vec3 first = definitionPoint;
        const Vec3 second = point15;
        const Vec3 direction = safeDirection(first, second, Vec3::unitX());
        out.lines.push_back(LineSegment{first, second});
        out.arrowheads.push_back(arrowhead(first, direction, normal, arrowLength));
        out.arrowheads.push_back(arrowhead(second, -direction, normal, arrowLength));
    } else {
        const Vec3 chord = point15;
        const Vec3 outward = safeDirection(center, chord, Vec3::unitX());
        // The leader runs from inside the circle out to the chord point. Its
        // tail is where the text hangs.
        const double leader = leaderLength > 0.0 ? leaderLength : style.scaled(style.textHeight) * 3.0;
        const Vec3 tail = chord + outward * leader;
        out.lines.push_back(LineSegment{center, tail});
        out.arrowheads.push_back(arrowhead(chord, -outward, normal, arrowLength));

        // Centre mark, whose sign selects between a mark and full centre lines.
        const double mark = style.scaled(style.centerMarkSize);
        if (mark != 0.0) {
            const double size = std::abs(mark);
            const Vec3 x = Vec3::unitX() * size;
            const Vec3 y = Vec3::unitY() * size;
            out.lines.push_back(LineSegment{center - x, center + x});
            out.lines.push_back(LineSegment{center - y, center + y});
            if (mark < 0.0) {
                const double radius = center.distanceTo(chord);
                const Vec3 xr = Vec3::unitX() * (radius + size);
                const Vec3 yr = Vec3::unitY() * (radius + size);
                out.lines.push_back(LineSegment{center + x * 2.0, center + xr});
                out.lines.push_back(LineSegment{center - xr, center - x * 2.0});
                out.lines.push_back(LineSegment{center + y * 2.0, center + yr});
                out.lines.push_back(LineSegment{center - yr, center - y * 2.0});
            }
        }
    }

    out.textRotation = hasTextRotationOverride ? textRotationOverride : 0.0;
    out.textPosition = textPositionIsUserDefined ? textMidPoint : defaultTextPosition(style);
    return out;
}

DimensionGeometry DimensionEntity::generateAngular(const DimStyle& style) const {
    DimensionGeometry out;
    out.textHeight = style.scaled(style.textHeight);
    out.text = measurementText(style);

    const Vec3 vertex = arcCenter();
    const Vec3 first = type == DimensionType::Angular3Point ? point13 : point14;
    const Vec3 second = type == DimensionType::Angular3Point ? point14 : definitionPoint;
    const Vec3 arcPoint = type == DimensionType::Angular3Point ? definitionPoint : point16;

    const double startAngle = (first - vertex).angle2();
    const double endAngle = (second - vertex).angle2();
    const double radius = vertex.distanceTo(arcPoint);
    if (radius < kDegenerate) {
        out.textPosition = textMidPoint;
        return out;
    }

    out.arcs.push_back(Arc{vertex, radius, startAngle, endAngle, normal});

    // Extension lines run from the measured points out past the dimension arc.
    const auto extension = [&](const Vec3& measured, bool suppressed) {
        if (suppressed) {
            return;
        }
        const Vec3 direction = safeDirection(vertex, measured, Vec3::unitX());
        const double reach = vertex.distanceTo(measured);
        const double start = std::min(reach + style.scaled(style.extensionLineOffset), radius);
        const double end = radius + style.scaled(style.extensionLineExtend);
        if (end > start) {
            out.lines.push_back(LineSegment{vertex + direction * start, vertex + direction * end});
        }
    };
    extension(first, style.suppressExtensionLine1);
    extension(second, style.suppressExtensionLine2);

    // Arrows sit at the arc ends, tangent to it.
    const double arrowLength = style.scaled(style.arrowSize);
    const Arc& arc = out.arcs.front();
    const Vec3 startTangent = arc.tangentAt(0.0).normalized();
    const Vec3 endTangent = arc.tangentAt(1.0).normalized();
    out.arrowheads.push_back(arrowhead(arc.startPoint(), startTangent, normal, arrowLength));
    out.arrowheads.push_back(arrowhead(arc.endPoint(), -endTangent, normal, arrowLength));

    out.textRotation = hasTextRotationOverride ? textRotationOverride : 0.0;
    out.textPosition = textPositionIsUserDefined ? textMidPoint : defaultTextPosition(style);
    return out;
}

DimensionGeometry DimensionEntity::generateOrdinate(const DimStyle& style) const {
    DimensionGeometry out;
    out.textHeight = style.scaled(style.textHeight);
    out.text = measurementText(style);

    // A stepped leader: out from the feature, a dog-leg, then a run to the
    // text. The dog-leg keeps a column of ordinates aligned even when their
    // features are not.
    const Vec3 feature = point13;
    const Vec3 leaderEnd = point14;
    const Vec3 delta = leaderEnd - feature;

    // The leader runs perpendicular to the measured axis, so an X-datum
    // ordinate leads away in Y and vice versa.
    const Vec3 firstLeg = isXDatum ? Vec3{feature.x, leaderEnd.y, feature.z}
                                   : Vec3{leaderEnd.x, feature.y, feature.z};
    if (delta.lengthSquared() > kDegenerate) {
        out.lines.push_back(LineSegment{feature, firstLeg});
        out.lines.push_back(LineSegment{firstLeg, leaderEnd});
    }

    out.textRotation = hasTextRotationOverride ? textRotationOverride : 0.0;
    out.textPosition = textPositionIsUserDefined ? textMidPoint : defaultTextPosition(style);
    return out;
}

DimensionGeometry DimensionEntity::generate(const DimStyle& style) const {
    switch (type) {
        case DimensionType::Rotated:
        case DimensionType::Aligned:
            return generateLinear(style);
        case DimensionType::Radius:
        case DimensionType::Diameter:
            return generateRadial(style);
        case DimensionType::Angular2Line:
        case DimensionType::Angular3Point:
            return generateAngular(style);
        case DimensionType::Ordinate:
            return generateOrdinate(style);
        default:
            return {};
    }
}

Vec3 DimensionEntity::defaultTextPosition(const DimStyle& style) const {
    const double gap = style.scaled(style.textGap);
    const double height = style.scaled(style.textHeight);

    switch (type) {
        case DimensionType::Rotated:
        case DimensionType::Aligned: {
            const Vec3 along = type == DimensionType::Aligned
                                   ? safeDirection(point13, point14, Vec3::unitX())
                                   : Vec3{std::cos(rotation), std::sin(rotation), 0.0};
            const Vec3 origin = definitionPoint;
            const Vec3 dim1 = origin + along * (point13 - origin).dot(along);
            const Vec3 dim2 = origin + along * (point14 - origin).dot(along);
            const Vec3 middle = dim1.midpoint(dim2);

            if (style.textVerticalPosition == TextVerticalPosition::Centered) {
                return middle;
            }
            // Above the line means above as the reader sees it, so the offset
            // follows the perpendicular that points away from the measured
            // geometry rather than a fixed side.
            Vec3 away = perpendicular(along, normal);
            if (away.dot(origin - point13.midpoint(point14)) < 0.0) {
                away = -away;
            }
            const double distance = gap + height / 2.0;
            return style.textVerticalPosition == TextVerticalPosition::Below
                       ? middle - away * distance
                       : middle + away * distance;
        }
        case DimensionType::Diameter:
            return definitionPoint.midpoint(point15);
        case DimensionType::Radius: {
            const Vec3 center = definitionPoint;
            const Vec3 outward = safeDirection(center, point15, Vec3::unitX());
            const double leader = leaderLength > 0.0 ? leaderLength : height * 3.0;
            return point15 + outward * (leader + gap);
        }
        case DimensionType::Angular2Line:
        case DimensionType::Angular3Point: {
            const Vec3 vertex = arcCenter();
            const Vec3 first = type == DimensionType::Angular3Point ? point13 : point14;
            const Vec3 second = type == DimensionType::Angular3Point ? point14 : definitionPoint;
            const Vec3 arcPoint = type == DimensionType::Angular3Point ? definitionPoint : point16;
            const double radius = vertex.distanceTo(arcPoint);
            const double start = (first - vertex).angle2();
            const double sweep = normalizeAngle((second - vertex).angle2() - start);
            const double middle = start + sweep / 2.0;
            const double offset = radius + gap + height / 2.0;
            return vertex + Vec3{std::cos(middle), std::sin(middle), 0.0} * offset;
        }
        case DimensionType::Ordinate:
            return point14 + (isXDatum ? Vec3{0.0, gap, 0.0} : Vec3{gap, 0.0, 0.0});
        default:
            return textMidPoint;
    }
}

// -------------------------------------------------------------------- entity

Box3 DimensionEntity::bounds() const {
    const DimensionGeometry geometry = generate(fallbackDimStyle());

    Box3 box;
    for (const LineSegment& line : geometry.lines) {
        box.expandToPoint(line.start);
        box.expandToPoint(line.end);
    }
    for (const Arc& arc : geometry.arcs) {
        box.unite(arc.bounds());
    }
    for (const std::array<Vec3, 3>& arrow : geometry.arrowheads) {
        for (const Vec3& corner : arrow) {
            box.expandToPoint(corner);
        }
    }

    if (!geometry.text.empty()) {
        // Conservative rather than exact: the real width needs the font, which
        // the core deliberately cannot reach.
        const double width = measureText(geometry.text, geometry.textHeight, 1.0);
        const double half = geometry.textHeight;
        box.expandToPoint(geometry.textPosition + Vec3{width / 2.0 + half, half, 0.0});
        box.expandToPoint(geometry.textPosition - Vec3{width / 2.0 + half, half, 0.0});
    }

    // A dimension whose definition points are all that survived a damaged file
    // still has to occupy space, or it vanishes from zoom extents.
    if (box.isEmpty()) {
        box.expandToPoint(definitionPoint);
        box.expandToPoint(textMidPoint);
    }
    return box;
}

std::vector<CurvePtr> DimensionEntity::toCurves() const {
    const DimensionGeometry geometry = generate(fallbackDimStyle());
    std::vector<CurvePtr> curves;
    curves.reserve(geometry.lines.size() + geometry.arcs.size());
    for (const LineSegment& line : geometry.lines) {
        curves.push_back(std::make_unique<LineSegment>(line));
    }
    for (const Arc& arc : geometry.arcs) {
        curves.push_back(std::make_unique<Arc>(arc));
    }
    return curves;
}

void DimensionEntity::transformBy(const Mat4& m) {
    definitionPoint = m.transformPoint(definitionPoint);
    textMidPoint = m.transformPoint(textMidPoint);
    point13 = m.transformPoint(point13);
    point14 = m.transformPoint(point14);
    point15 = m.transformPoint(point15);
    point16 = m.transformPoint(point16);

    // The measurement axis is a direction, so it rotates with the transform
    // rather than being carried across unchanged — a rotated dimension that
    // kept its old axis would start measuring the wrong distance.
    const Vec3 axis = m.transformVector(Vec3{std::cos(rotation), std::sin(rotation), 0.0});
    if (axis.lengthSquared() > kDegenerate) {
        rotation = axis.angle2();
    }
    normal = m.transformVector(normal).normalized();

    // Whatever the file recorded no longer describes the geometry.
    storedMeasurement = measure();
}

std::vector<GripPoint> DimensionEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    switch (type) {
        case DimensionType::Rotated:
        case DimensionType::Aligned:
            grips.push_back({point13, GripKind::Endpoint, 0});
            grips.push_back({point14, GripKind::Endpoint, 1});
            grips.push_back({definitionPoint, GripKind::Vertex, 2});
            break;
        case DimensionType::Radius:
        case DimensionType::Diameter:
            grips.push_back({definitionPoint, GripKind::Vertex, 0});
            grips.push_back({point15, GripKind::Endpoint, 1});
            break;
        case DimensionType::Angular3Point:
            grips.push_back({point13, GripKind::Endpoint, 0});
            grips.push_back({point14, GripKind::Endpoint, 1});
            grips.push_back({point15, GripKind::Center, 2});
            grips.push_back({definitionPoint, GripKind::Vertex, 3});
            break;
        case DimensionType::Angular2Line:
            grips.push_back({point16, GripKind::Vertex, 3});
            break;
        case DimensionType::Ordinate:
            grips.push_back({point13, GripKind::Endpoint, 0});
            grips.push_back({point14, GripKind::Endpoint, 1});
            break;
        default:
            break;
    }
    // The text grip is last so its index is stable across types; dragging it
    // is what sets `textPositionIsUserDefined`.
    grips.push_back({textPositionIsUserDefined ? textMidPoint
                                               : defaultTextPosition(fallbackDimStyle()),
                     GripKind::Insertion, 100});
    return grips;
}

void DimensionEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index == 100) {
        textMidPoint = to;
        textPositionIsUserDefined = true;
        return;
    }
    switch (type) {
        case DimensionType::Rotated:
        case DimensionType::Aligned:
        case DimensionType::Ordinate:
            if (grip.index == 0) { point13 = to; }
            if (grip.index == 1) { point14 = to; }
            if (grip.index == 2) { definitionPoint = to; }
            break;
        case DimensionType::Radius:
        case DimensionType::Diameter:
            if (grip.index == 0) { definitionPoint = to; }
            if (grip.index == 1) { point15 = to; }
            break;
        case DimensionType::Angular3Point:
            if (grip.index == 0) { point13 = to; }
            if (grip.index == 1) { point14 = to; }
            if (grip.index == 2) { point15 = to; }
            if (grip.index == 3) { definitionPoint = to; }
            break;
        case DimensionType::Angular2Line:
            if (grip.index == 3) { point16 = to; }
            break;
        default:
            break;
    }
    storedMeasurement = measure();
}

DbObjectPtr DimensionEntity::clone() const {
    auto copy = std::make_unique<DimensionEntity>();
    copyEntityInto(*copy);
    copy->type = type;
    copy->definitionPoint = definitionPoint;
    copy->textMidPoint = textMidPoint;
    copy->point13 = point13;
    copy->point14 = point14;
    copy->point15 = point15;
    copy->point16 = point16;
    copy->rotation = rotation;
    copy->obliqueAngle = obliqueAngle;
    copy->textRotationOverride = textRotationOverride;
    copy->hasTextRotationOverride = hasTextRotationOverride;
    copy->horizontalDirection = horizontalDirection;
    copy->leaderLength = leaderLength;
    copy->textOverride = textOverride;
    copy->styleHandle = styleHandle;
    copy->blockHandle = blockHandle;
    copy->storedMeasurement = storedMeasurement;
    copy->attachmentPoint = attachmentPoint;
    copy->lineSpacingStyle = lineSpacingStyle;
    copy->lineSpacingFactor = lineSpacingFactor;
    copy->textPositionIsUserDefined = textPositionIsUserDefined;
    copy->isXDatum = isXDatum;
    return copy;
}

void DimensionEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const DimensionEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a dimension");
    }
    restoreEntityFrom(*source);
    type = source->type;
    definitionPoint = source->definitionPoint;
    textMidPoint = source->textMidPoint;
    point13 = source->point13;
    point14 = source->point14;
    point15 = source->point15;
    point16 = source->point16;
    rotation = source->rotation;
    obliqueAngle = source->obliqueAngle;
    textRotationOverride = source->textRotationOverride;
    hasTextRotationOverride = source->hasTextRotationOverride;
    horizontalDirection = source->horizontalDirection;
    leaderLength = source->leaderLength;
    textOverride = source->textOverride;
    styleHandle = source->styleHandle;
    blockHandle = source->blockHandle;
    storedMeasurement = source->storedMeasurement;
    attachmentPoint = source->attachmentPoint;
    lineSpacingStyle = source->lineSpacingStyle;
    lineSpacingFactor = source->lineSpacingFactor;
    textPositionIsUserDefined = source->textPositionIsUserDefined;
    isXDatum = source->isXDatum;
}

std::vector<Handle> DimensionEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    if (!styleHandle.isNull()) {
        handles.push_back(styleHandle);
    }
    if (!blockHandle.isNull()) {
        handles.push_back(blockHandle);
    }
    return handles;
}

}  // namespace cad
