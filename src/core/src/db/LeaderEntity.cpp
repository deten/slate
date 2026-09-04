#include "cad/db/LeaderEntity.hpp"

#include <cmath>
#include <stdexcept>

#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

/// Ratio of an arrowhead's half-width to its length.
///
/// The same number a dimension's arrow uses, and for the same reason: DWG has
/// no variable for it, DIMASZ scales the whole arrow, and a leader whose arrow
/// were a different shape from the dimensions beside it would look like a
/// mistake on the sheet.
constexpr double kArrowHalfWidthRatio = 1.0 / 6.0;

constexpr double kDegenerate = 1e-12;

[[nodiscard]] Vec3 perpendicular(const Vec3& direction, const Vec3& normal) {
    const Vec3 perp = normal.cross(direction);
    return perp.lengthSquared() < kDegenerate ? direction.perp2().normalized()
                                              : perp.normalized();
}

[[nodiscard]] std::array<Vec3, 3> arrowhead(const Vec3& tip, const Vec3& direction,
                                            const Vec3& normal, double length) {
    const Vec3 back = tip + direction * length;
    const Vec3 side = perpendicular(direction, normal) * (length * kArrowHalfWidthRatio);
    return {tip, back + side, back - side};
}

}  // namespace

void LeaderEntity::setAnnotation(Handle note) {
    annotationHandle = note;
    // 0 is "the annotation is MTEXT", 3 is "there is none". A file that says 0
    // and names nothing is one another reader has to guess about.
    annotationType = note.isNull() ? 3 : 0;
    if (note.isNull()) {
        xdata.erase(kSlateApplication);
        return;
    }
    // 1005 is the XData code for a database handle, which is what tells a
    // converter to treat the value as a reference rather than as a number.
    setXData(kSlateApplication, {XDataItem{1005, XDataValue{note}}});
}

Vec3 LeaderEntity::landingDirection() const {
    if (hasHorizontalDirection && horizontalDirection.lengthSquared() > kDegenerate) {
        return horizontalDirection.normalized();
    }
    if (vertices.size() < 2) {
        return Vec3::unitX();
    }
    // Which way the note sits is decided by the direction the leader arrives
    // from, not by the drawing's axes: a leader coming in from the right hangs
    // its text on the right. Getting this backwards puts every note on a plan
    // through the geometry it is labelling.
    const Vec3 approach = vertices.back() - vertices[vertices.size() - 2];
    return approach.x >= 0.0 ? Vec3::unitX() : -Vec3::unitX();
}

LeaderGeometry LeaderEntity::geometry(const DimStyle& style) const {
    LeaderGeometry out;
    if (vertices.size() < 2) {
        out.landingEnd = vertices.empty() ? Vec3{} : vertices.front();
        return out;
    }

    for (std::size_t i = 0; i + 1 < vertices.size(); ++i) {
        out.lines.push_back(LineSegment{vertices[i], vertices[i + 1]});
    }

    const Vec3 tip = vertices.front();
    const Vec3 next = vertices[1];
    const Vec3 shaft = next - tip;
    const double arrowLength = style.scaled(style.arrowSize);

    if (hasArrowhead && shaft.lengthSquared() > kDegenerate) {
        // The direction runs from the tip back along the first segment, so the
        // arrow points at the thing being called out however the leader was
        // drawn.
        out.arrowheads.push_back(arrowhead(tip, shaft.normalized(), normal, arrowLength));
    }

    const Vec3 last = vertices.back();

    const Vec3 landing = landingDirection();
    out.landingToTheRight = landing.x >= 0.0;

    if (!hasLanding) {
        out.landingEnd = last;
        return out;
    }

    // Long enough to carry the hook clear of the angled segment, which is what
    // the landing is for. other CAD software sizes it from DIMASZ and adds the text gap.
    const double length = style.scaled(style.arrowSize) + style.scaled(style.textGap);
    out.landingEnd = last + landing * length;
    out.lines.push_back(LineSegment{last, out.landingEnd});
    return out;
}

Box3 LeaderEntity::bounds() const {
    // The vertices alone. The landing's length depends on a style this entity
    // cannot reach from here, and it is short next to the leader itself; a
    // bound that needed the style would make every caller carry one.
    Box3 box = Box3::empty();
    for (const Vec3& vertex : vertices) {
        box.expandToPoint(vertex);
    }
    return box;
}

std::vector<CurvePtr> LeaderEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    for (std::size_t i = 0; i + 1 < vertices.size(); ++i) {
        curves.push_back(std::make_unique<LineSegment>(vertices[i], vertices[i + 1]));
    }
    return curves;
}

void LeaderEntity::transformBy(const Mat4& m) {
    for (Vec3& vertex : vertices) {
        vertex = m.transformPoint(vertex);
    }
    if (hasHorizontalDirection) {
        horizontalDirection = m.transformVector(horizontalDirection);
    }
}

std::vector<GripPoint> LeaderEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    grips.reserve(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        grips.push_back({vertices[i], GripKind::Endpoint, i});
    }
    return grips;
}

void LeaderEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index < vertices.size()) {
        vertices[grip.index] = to;
    }
}

DbObjectPtr LeaderEntity::clone() const {
    auto copy = std::make_unique<LeaderEntity>();
    copyEntityInto(*copy);
    copy->vertices = vertices;
    copy->styleHandle = styleHandle;
    copy->annotationHandle = annotationHandle;
    copy->hasArrowhead = hasArrowhead;
    copy->hasLanding = hasLanding;
    copy->annotationType = annotationType;
    copy->textHeight = textHeight;
    copy->textWidth = textWidth;
    copy->horizontalDirection = horizontalDirection;
    copy->hasHorizontalDirection = hasHorizontalDirection;
    return copy;
}

void LeaderEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const LeaderEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a leader");
    }
    restoreEntityFrom(*source);
    vertices = source->vertices;
    styleHandle = source->styleHandle;
    annotationHandle = source->annotationHandle;
    hasArrowhead = source->hasArrowhead;
    hasLanding = source->hasLanding;
    annotationType = source->annotationType;
    textHeight = source->textHeight;
    textWidth = source->textWidth;
    horizontalDirection = source->horizontalDirection;
    hasHorizontalDirection = source->hasHorizontalDirection;
}

std::vector<Handle> LeaderEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    if (!styleHandle.isNull()) {
        handles.push_back(styleHandle);
    }
    if (!annotationHandle.isNull()) {
        handles.push_back(annotationHandle);
    }
    return handles;
}

}  // namespace cad
