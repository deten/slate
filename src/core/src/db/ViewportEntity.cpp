#include "cad/db/ViewportEntity.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Ocs.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

Box3 ViewportEntity::paperRect() const {
    const Vec3 half{std::abs(width) / 2.0, std::abs(height) / 2.0, 0.0};
    return Box3::fromCorners(center - half, center + half);
}

Vec3 ViewportEntity::worldViewCenter() const {
    // The overwhelmingly common case: a plan view, where the display axes are
    // the world axes and the offset is a plain addition.
    if (isWorldNormal(viewDirection)) {
        return viewTarget + viewCenter;
    }
    // Anything else is a 3D view on a 2D drawing. The display frame is derived
    // the same way an entity's OCS is, which is right for the axonometric views
    // a 2D drawing occasionally carries and is a defined answer rather than a
    // silent one for the rest.
    return viewTarget + pointOcsToWorld(viewCenter, viewDirection);
}

Box3 ViewportEntity::modelRegion() const {
    const double scale = modelPerPaper();
    const Vec3 half{std::abs(width) * scale / 2.0, std::abs(viewHeight) / 2.0, 0.0};
    const Vec3 middle = worldViewCenter();
    return Box3::fromCorners(middle - half, middle + half);
}

Mat4 ViewportEntity::modelToPaper() const {
    const double scale = modelPerPaper();
    const double paperPerModel = scale > 0.0 ? 1.0 / scale : 1.0;

    // Read right to left: bring the view centre to the origin, scale the model
    // down to paper size, apply the view twist, then place it at the window's
    // centre on the sheet. Composing them in any other order puts the drawing
    // in the wrong place by a scaled offset — which looks like a bug in the
    // viewport's position rather than in the order of two matrices.
    const Mat4 toOrigin = Mat4::translation(-worldViewCenter());
    const Mat4 shrink = Mat4::scaling(paperPerModel);
    const Mat4 rotate = twist == 0.0 ? Mat4::identity() : Mat4::rotationZ(twist);
    const Mat4 place = Mat4::translation(center);

    return place.multiply(rotate).multiply(shrink).multiply(toOrigin);
}

bool ViewportEntity::isLayerFrozen(Handle layer) const {
    return std::find(frozenLayerHandles.begin(), frozenLayerHandles.end(), layer) !=
           frozenLayerHandles.end();
}

std::vector<CurvePtr> ViewportEntity::toCurves() const {
    // The frame only. The contents belong to model space and are drawn by the
    // scene builder; returning them here would make every entity in the model
    // snappable twice and pickable through the sheet.
    const Box3 rect = paperRect();
    if (rect.isEmpty()) {
        return {};
    }
    const std::array<Vec3, 4> corners = rect.corners2();
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Polyline>(
        Polyline::fromPoints(std::vector<Vec3>{corners[0], corners[1], corners[2], corners[3]},
                             true)));
    return curves;
}

void ViewportEntity::transformBy(const Mat4& m) {
    // Only the window moves on the sheet. What it *looks at* is unaffected:
    // sliding a viewport across a title block must not pan the model inside it.
    center = m.transformPoint(center);
    const double scale = m.scaleFactor2();
    width *= scale;
    height *= scale;
    // The view height follows, so the viewport's scale is preserved rather
    // than the model appearing to zoom when the window is resized.
    viewHeight *= scale;
}

std::vector<GripPoint> ViewportEntity::gripPoints() const {
    const Box3 rect = paperRect();
    if (rect.isEmpty()) {
        return {};
    }
    const std::array<Vec3, 4> corners = rect.corners2();
    std::vector<GripPoint> grips{{center, GripKind::Center, 0}};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        grips.push_back({corners[i], GripKind::Vertex, i + 1});
    }
    return grips;
}

void ViewportEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index == 0) {
        center = to;
        return;
    }
    const Box3 rect = paperRect();
    if (rect.isEmpty()) {
        return;
    }
    // Dragging a corner resizes against the opposite one, which is what a
    // rectangle's corner grip means everywhere else in the application.
    const std::array<Vec3, 4> corners = rect.corners2();
    const std::size_t index = grip.index - 1;
    if (index >= corners.size()) {
        return;
    }
    const Vec3& opposite = corners[(index + 2) % corners.size()];

    const Box3 resized = Box3::fromCorners(opposite, to);
    center = resized.min.midpoint(resized.max);
    // The view height tracks the window so the scale stays put; a drafter
    // resizing a viewport wants to see more of the model, not a bigger model.
    const double previousHeight = height;
    width = resized.width();
    height = resized.height();
    if (previousHeight > 0.0) {
        viewHeight *= height / previousHeight;
    }
}

DbObjectPtr ViewportEntity::clone() const {
    auto copy = std::make_unique<ViewportEntity>();
    copyEntityInto(*copy);
    copy->center = center;
    copy->width = width;
    copy->height = height;
    copy->viewportId = viewportId;
    copy->viewCenter = viewCenter;
    copy->viewHeight = viewHeight;
    copy->twist = twist;
    copy->viewDirection = viewDirection;
    copy->viewDirectionLength = viewDirectionLength;
    copy->viewTarget = viewTarget;
    copy->statusFlags = statusFlags;
    copy->locked = locked;
    copy->frozenLayerHandles = frozenLayerHandles;
    copy->clipEntityHandle = clipEntityHandle;
    return copy;
}

void ViewportEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const ViewportEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a viewport");
    }
    restoreEntityFrom(*source);
    center = source->center;
    width = source->width;
    height = source->height;
    viewportId = source->viewportId;
    viewCenter = source->viewCenter;
    viewHeight = source->viewHeight;
    twist = source->twist;
    viewDirection = source->viewDirection;
    viewDirectionLength = source->viewDirectionLength;
    viewTarget = source->viewTarget;
    statusFlags = source->statusFlags;
    locked = source->locked;
    frozenLayerHandles = source->frozenLayerHandles;
    clipEntityHandle = source->clipEntityHandle;
}

std::vector<Handle> ViewportEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    for (const Handle layer : frozenLayerHandles) {
        if (!layer.isNull()) {
            handles.push_back(layer);
        }
    }
    if (!clipEntityHandle.isNull()) {
        handles.push_back(clipEntityHandle);
    }
    return handles;
}

}  // namespace cad
