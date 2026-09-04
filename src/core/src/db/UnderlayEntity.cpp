#include "cad/db/UnderlayEntity.hpp"

#include <limits>

#include "cad/geom/Polyline.hpp"

namespace cad {

std::string_view underlayKindName(UnderlayKind kind) noexcept {
    switch (kind) {
        case UnderlayKind::Dwf: return "DWF";
        case UnderlayKind::Dgn: return "DGN";
        case UnderlayKind::Pdf: break;
    }
    return "PDF";
}

// ------------------------------------------------------ UnderlayDefinition

DbObjectPtr UnderlayDefinition::clone() const {
    auto copy = std::make_unique<UnderlayDefinition>();
    copyBaseInto(*copy);
    copy->kind = kind;
    copy->fileName = fileName;
    copy->itemName = itemName;
    copy->resolvedPath = resolvedPath;
    copy->resolutionProblem = resolutionProblem;
    return copy;
}

void UnderlayDefinition::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const UnderlayDefinition*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreBaseFrom(*source);
    kind = source->kind;
    fileName = source->fileName;
    itemName = source->itemName;
    resolvedPath = source->resolvedPath;
    resolutionProblem = source->resolutionProblem;
}

// ---------------------------------------------------------- UnderlayEntity

std::string UnderlayEntity::objectType() const {
    switch (kind) {
        case UnderlayKind::Dwf: return "DwfUnderlay";
        case UnderlayKind::Dgn: return "DgnUnderlay";
        case UnderlayKind::Pdf: break;
    }
    return "PdfUnderlay";
}

Mat4 UnderlayEntity::placementTransform() const {
    return Mat4::blockTransform(position, scale, rotation);
}

std::vector<Vec3> UnderlayEntity::displayOutline() const {
    const Mat4 placement = placementTransform();

    if (clippingEnabled && clipVertices.size() >= 2) {
        if (clipVertices.size() == 2) {
            const Vec3& a = clipVertices.front();
            const Vec3& b = clipVertices.back();
            return {placement.transformPoint(a), placement.transformPoint(Vec3{b.x, a.y, 0.0}),
                    placement.transformPoint(b), placement.transformPoint(Vec3{a.x, b.y, 0.0})};
        }
        std::vector<Vec3> outline;
        outline.reserve(clipVertices.size());
        for (const Vec3& vertex : clipVertices) {
            outline.push_back(placement.transformPoint(vertex));
        }
        return outline;
    }

    if (pageSize.x > 0.0 && pageSize.y > 0.0) {
        return {placement.transformPoint(Vec3::zero()),
                placement.transformPoint(Vec3{pageSize.x, 0.0, 0.0}),
                placement.transformPoint(Vec3{pageSize.x, pageSize.y, 0.0}),
                placement.transformPoint(Vec3{0.0, pageSize.y, 0.0})};
    }

    return {};
}

Box3 UnderlayEntity::bounds() const {
    const std::vector<Vec3> outline = displayOutline();
    if (outline.empty()) {
        // A degenerate box at the insertion point, not an empty one. An
        // underlay whose page size nobody knows still has a location, and
        // reporting nothing would drop it out of ZOOM EXTENTS entirely — so the
        // user would have no way of finding the thing they cannot see.
        return Box3::fromPoints({position});
    }
    return Box3::fromPoints(std::span<const Vec3>{outline});
}

void UnderlayEntity::transformBy(const Mat4& m) {
    position = m.transformPoint(position);
    const double factor = m.scaleFactor2();
    scale = Vec3{scale.x * factor, scale.y * factor, scale.z * factor};
    rotation += m.rotation2();
    normal = m.transformVector(normal).normalized();
}

std::vector<CurvePtr> UnderlayEntity::toCurves() const {
    const std::vector<Vec3> outline = displayOutline();
    if (outline.size() < 3) {
        return {};
    }
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Polyline>(Polyline::fromPoints(outline, /*closed=*/true)));
    return curves;
}

double UnderlayEntity::distanceToPoint(const Vec3& p) const {
    const std::vector<Vec3> outline = displayOutline();
    if (outline.size() < 3) {
        // With no outline the only thing that can be picked is the insertion
        // point itself, which at least makes the entity selectable.
        return position.distanceTo(p);
    }
    const Polyline ring = Polyline::fromPoints(outline, /*closed=*/true);
    if (ring.containsPointInside(p)) {
        return 0.0;
    }
    return ring.distanceTo(p);
}

std::vector<GripPoint> UnderlayEntity::gripPoints() const {
    std::vector<GripPoint> grips{GripPoint{position, GripKind::Insertion, 0}};
    const std::vector<Vec3> outline = displayOutline();
    for (std::size_t i = 0; i < outline.size(); ++i) {
        grips.push_back(GripPoint{outline[i], GripKind::Vertex, i + 1});
    }
    return grips;
}

void UnderlayEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    // Only the insertion grip moves the underlay. The outline grips are there
    // to show its extent and to snap to; dragging one would have to resize the
    // page, which is not something the drawing gets to decide.
    if (grip.kind == GripKind::Insertion) {
        position = to;
    }
}

std::vector<Handle> UnderlayEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    if (!definitionHandle.isNull()) {
        handles.push_back(definitionHandle);
    }
    return handles;
}

DbObjectPtr UnderlayEntity::clone() const {
    auto copy = std::make_unique<UnderlayEntity>();
    copyEntityInto(*copy);
    copy->kind = kind;
    copy->definitionHandle = definitionHandle;
    copy->position = position;
    copy->scale = scale;
    copy->rotation = rotation;
    copy->clippingEnabled = clippingEnabled;
    copy->underlayOn = underlayOn;
    copy->monochrome = monochrome;
    copy->adjustForBackground = adjustForBackground;
    copy->contrast = contrast;
    copy->fade = fade;
    copy->clipVertices = clipVertices;
    copy->pageSize = pageSize;
    return copy;
}

void UnderlayEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const UnderlayEntity*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreEntityFrom(*source);
    kind = source->kind;
    definitionHandle = source->definitionHandle;
    position = source->position;
    scale = source->scale;
    rotation = source->rotation;
    clippingEnabled = source->clippingEnabled;
    underlayOn = source->underlayOn;
    monochrome = source->monochrome;
    adjustForBackground = source->adjustForBackground;
    contrast = source->contrast;
    fade = source->fade;
    clipVertices = source->clipVertices;
    pageSize = source->pageSize;
}

}  // namespace cad
