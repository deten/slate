#include "cad/db/Entity.hpp"

#include <algorithm>

#include <limits>

namespace cad {

void Entity::moveGrips(std::span<const GripMove> moves) {
    for (const GripMove& move : moves) {
        // Re-found by position on the entity as it now stands. Grip indices do
        // not survive an edit of the same object: a polyline midpoint grip
        // inserts a vertex, renumbering the grips after it and shifting the
        // boundary between vertex indices and midpoint indices.
        const std::vector<GripPoint> current = gripPoints();
        const auto found = std::find_if(current.begin(), current.end(),
                                        [&](const GripPoint& candidate) {
                                            return candidate.position.equals(move.grip.position);
                                        });
        // Not there any more, because an earlier move in this same set carried
        // it: a line's midpoint moves when either end does. The index it was
        // recorded with is the best remaining guess.
        moveGrip(found == current.end() ? move.grip : *found, move.to);
    }
}

double Entity::distanceToPoint(const Vec3& p) const {
    double best = std::numeric_limits<double>::infinity();
    for (const CurvePtr& curve : toCurves()) {
        best = std::min(best, curve->distanceTo(p));
    }
    return best;
}

ResolvedProperties Entity::resolveProperties(const ResolutionContext& context) const {
    return ResolvedProperties{
        resolveColor(context).toRgb(),
        resolveLinetype(context),
        linetypeScale * context.globalLinetypeScale,
        resolveLineweight(context),
        resolveAlpha(context),
        visible && context.layerVisible,
    };
}

Color Entity::resolveColor(const ResolutionContext& context) const {
    if (color.isConcrete()) {
        return color;
    }
    if (color.isByBlock()) {
        // ByBlock outside any block reference behaves as ByLayer.
        if (!context.insideBlock) {
            return context.layerColor;
        }
        return context.blockColor.isConcrete() ? context.blockColor : context.layerColor;
    }
    return context.layerColor;
}

Handle Entity::resolveLinetype(const ResolutionContext& context) const {
    if (!linetypeHandle.isNull()) {
        return linetypeHandle;
    }
    if (context.insideBlock && !context.blockLinetypeHandle.isNull()) {
        return context.blockLinetypeHandle;
    }
    return context.layerLinetypeHandle;
}

std::int16_t Entity::resolveLineweight(const ResolutionContext& context) const {
    if (lineweight >= 0) {
        return lineweight;
    }
    if (lineweight == toRaw(Lineweight::ByBlock) && context.insideBlock &&
        context.blockLineweight >= 0) {
        return context.blockLineweight;
    }
    // Lineweight "Default" resolves to the LWDEFAULT system variable, 0.25 mm.
    return context.layerLineweight >= 0 ? context.layerLineweight : std::int16_t{25};
}

std::uint8_t Entity::resolveAlpha(const ResolutionContext& context) const {
    if (transparency.has_value()) {
        return *transparency;
    }
    return context.layerTransparency.value_or(kTransparencyOpaque);
}

void Entity::copyEntityInto(Entity& target) const {
    copyBaseInto(target);
    target.layerHandle = layerHandle;
    target.color = color;
    target.linetypeHandle = linetypeHandle;
    target.linetypeScale = linetypeScale;
    target.lineweight = lineweight;
    target.transparency = transparency;
    target.visible = visible;
    target.normal = normal;
    target.elevation = elevation;
    target.materialHandle = materialHandle;
}

void Entity::restoreEntityFrom(const Entity& snapshot) {
    restoreBaseFrom(snapshot);
    layerHandle = snapshot.layerHandle;
    color = snapshot.color;
    linetypeHandle = snapshot.linetypeHandle;
    linetypeScale = snapshot.linetypeScale;
    lineweight = snapshot.lineweight;
    transparency = snapshot.transparency;
    visible = snapshot.visible;
    normal = snapshot.normal;
    elevation = snapshot.elevation;
    materialHandle = snapshot.materialHandle;
}

std::vector<Handle> Entity::referencedHandles() const {
    std::vector<Handle> refs = DbObject::referencedHandles();
    if (!layerHandle.isNull()) {
        refs.push_back(layerHandle);
    }
    if (!linetypeHandle.isNull()) {
        refs.push_back(linetypeHandle);
    }
    if (!materialHandle.isNull()) {
        refs.push_back(materialHandle);
    }
    return refs;
}

}  // namespace cad
