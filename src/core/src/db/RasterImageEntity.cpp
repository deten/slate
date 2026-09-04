#include "cad/db/RasterImageEntity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cad/geom/Polyline.hpp"

namespace cad {
namespace {

/// The clip boundary's pixel coordinates start half a pixel outside the first
/// pixel's centre, which is how a boundary of (-0.5, -0.5) to (w - 0.5, h - 0.5)
/// describes the whole image rather than everything but its outer half-pixel.
constexpr double kPixelCentreOffset = 0.5;

}  // namespace

// --------------------------------------------------------- ImageDefinition

DbObjectPtr ImageDefinition::clone() const {
    auto copy = std::make_unique<ImageDefinition>();
    copyBaseInto(*copy);
    copy->fileName = fileName;
    copy->sizeInPixels = sizeInPixels;
    copy->pixelSize = pixelSize;
    copy->loaded = loaded;
    copy->resolutionUnits = resolutionUnits;
    copy->resolvedPath = resolvedPath;
    copy->resolutionProblem = resolutionProblem;
    return copy;
}

void ImageDefinition::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const ImageDefinition*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreBaseFrom(*source);
    fileName = source->fileName;
    sizeInPixels = source->sizeInPixels;
    pixelSize = source->pixelSize;
    loaded = source->loaded;
    resolutionUnits = source->resolutionUnits;
    resolvedPath = source->resolvedPath;
    resolutionProblem = source->resolutionProblem;
}

// -------------------------------------------------------- RasterImageEntity

std::vector<Vec3> RasterImageEntity::corners() const {
    const Vec3 across = uVector * widthPixels;
    const Vec3 up = vVector * heightPixels;
    return {origin, origin + across, origin + across + up, origin + up};
}

std::vector<Vec3> RasterImageEntity::worldClipBoundary() const {
    if (!clippingEnabled || !useClippingBoundary || clipVertices.size() < 2) {
        return {};
    }

    const auto toWorld = [this](const Vec3& pixel) {
        return origin + uVector * (pixel.x + kPixelCentreOffset) +
               vVector * (pixel.y + kPixelCentreOffset);
    };

    if (clipBoundaryType == 1) {
        // Two opposite corners. Expanded here so that nothing downstream has to
        // know which of the two storage forms it was handed.
        const Vec3& a = clipVertices.front();
        const Vec3& b = clipVertices.back();
        return {toWorld(a), toWorld(Vec3{b.x, a.y, 0.0}), toWorld(b), toWorld(Vec3{a.x, b.y, 0.0})};
    }

    std::vector<Vec3> boundary;
    boundary.reserve(clipVertices.size());
    for (const Vec3& vertex : clipVertices) {
        boundary.push_back(toWorld(vertex));
    }
    return boundary;
}

std::vector<Vec3> RasterImageEntity::displayOutline() const {
    std::vector<Vec3> boundary = worldClipBoundary();
    return boundary.empty() ? corners() : boundary;
}

Box3 RasterImageEntity::bounds() const {
    const std::vector<Vec3> outline = displayOutline();
    return Box3::fromPoints(std::span<const Vec3>{outline});
}

void RasterImageEntity::transformBy(const Mat4& m) {
    origin = m.transformPoint(origin);
    // The pixel vectors are directions, so they take the linear part only.
    // Running them through transformPoint would add the translation twice and
    // collapse the image onto its own origin.
    uVector = m.transformVector(uVector);
    vVector = m.transformVector(vVector);
    normal = m.transformVector(normal).normalized();
}

std::vector<CurvePtr> RasterImageEntity::toCurves() const {
    const std::vector<Vec3> outline = displayOutline();
    if (outline.size() < 3) {
        return {};
    }
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Polyline>(Polyline::fromPoints(outline, /*closed=*/true)));
    return curves;
}

double RasterImageEntity::distanceToPoint(const Vec3& p) const {
    const std::vector<Vec3> outline = displayOutline();
    if (outline.size() < 3) {
        return std::numeric_limits<double>::infinity();
    }
    const Polyline ring = Polyline::fromPoints(outline, /*closed=*/true);
    if (ring.containsPointInside(p)) {
        return 0.0;
    }
    return ring.distanceTo(p);
}

std::vector<GripPoint> RasterImageEntity::gripPoints() const {
    // The frame's corners, never the clip boundary's. Dragging a clip corner
    // would have to edit the clip and not the placement, and the two look
    // identical on screen — so only the ones that mean what they look like are
    // offered.
    std::vector<GripPoint> grips;
    const std::vector<Vec3> frame = corners();
    grips.reserve(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        grips.push_back(GripPoint{frame[i], GripKind::Vertex, i});
    }
    return grips;
}

void RasterImageEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index > 3 || widthPixels <= 0.0 || heightPixels <= 0.0) {
        return;
    }

    // Dragging a corner scales the image about the diagonally opposite one, so
    // the corner the user is not touching stays where they can see it does.
    const std::vector<Vec3> frame = corners();
    const Vec3 anchor = frame[(grip.index + 2) % 4];
    const Vec3 across = uVector * widthPixels;
    const Vec3 up = vVector * heightPixels;

    const double acrossLengthSquared = across.lengthSquared();
    const double upLengthSquared = up.lengthSquared();
    if (acrossLengthSquared <= 0.0 || upLengthSquared <= 0.0) {
        return;
    }

    // Signs of the two edges leaving the anchor, so the arithmetic is the same
    // whichever corner is being dragged.
    const double acrossSign = (grip.index == 1 || grip.index == 2) ? 1.0 : -1.0;
    const double upSign = (grip.index == 2 || grip.index == 3) ? 1.0 : -1.0;

    const Vec3 delta = to - anchor;
    const double acrossScale = acrossSign * delta.dot(across) / acrossLengthSquared;
    const double upScale = upSign * delta.dot(up) / upLengthSquared;

    // A degenerate drag would make the image infinitely thin and unselectable,
    // which is a state a user cannot get back out of.
    constexpr double kMinimumScale = 1e-6;
    if (std::abs(acrossScale) < kMinimumScale || std::abs(upScale) < kMinimumScale) {
        return;
    }

    uVector = uVector * acrossScale;
    vVector = vVector * upScale;

    // The anchor is a corner of the new frame too, so the origin is recovered
    // from it. That is what keeps it fixed rather than merely near where it was.
    const Vec3 newAcross = uVector * widthPixels;
    const Vec3 newUp = vVector * heightPixels;
    switch ((grip.index + 2) % 4) {
        case 0: origin = anchor; break;
        case 1: origin = anchor - newAcross; break;
        case 2: origin = anchor - newAcross - newUp; break;
        default: origin = anchor - newUp; break;
    }
}

std::vector<Handle> RasterImageEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    if (!definitionHandle.isNull()) {
        handles.push_back(definitionHandle);
    }
    if (!reactorHandle.isNull()) {
        handles.push_back(reactorHandle);
    }
    return handles;
}

DbObjectPtr RasterImageEntity::clone() const {
    auto copy = std::make_unique<RasterImageEntity>();
    copyEntityInto(*copy);
    copy->definitionHandle = definitionHandle;
    copy->reactorHandle = reactorHandle;
    copy->origin = origin;
    copy->uVector = uVector;
    copy->vVector = vVector;
    copy->widthPixels = widthPixels;
    copy->heightPixels = heightPixels;
    copy->showImage = showImage;
    copy->showWhenNotAligned = showWhenNotAligned;
    copy->useClippingBoundary = useClippingBoundary;
    copy->transparent = transparent;
    copy->clippingEnabled = clippingEnabled;
    copy->brightness = brightness;
    copy->contrast = contrast;
    copy->fade = fade;
    copy->clipBoundaryType = clipBoundaryType;
    copy->clipVertices = clipVertices;
    copy->clipInverted = clipInverted;
    return copy;
}

void RasterImageEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const RasterImageEntity*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreEntityFrom(*source);
    definitionHandle = source->definitionHandle;
    reactorHandle = source->reactorHandle;
    origin = source->origin;
    uVector = source->uVector;
    vVector = source->vVector;
    widthPixels = source->widthPixels;
    heightPixels = source->heightPixels;
    showImage = source->showImage;
    showWhenNotAligned = source->showWhenNotAligned;
    useClippingBoundary = source->useClippingBoundary;
    transparent = source->transparent;
    clippingEnabled = source->clippingEnabled;
    brightness = source->brightness;
    contrast = source->contrast;
    fade = source->fade;
    clipBoundaryType = source->clipBoundaryType;
    clipVertices = source->clipVertices;
    clipInverted = source->clipInverted;
}

}  // namespace cad
