#include "cad/render/Viewport.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace cad::render {
namespace {

[[nodiscard]] double clampScale(double scale) noexcept {
    // A non-positive scale would invert or collapse the view and make the
    // inverse transform undefined.
    if (!std::isfinite(scale) || scale <= 0.0) {
        return 1.0;
    }
    return std::clamp(scale, kMinScale, kMaxScale);
}

}  // namespace

Viewport::Viewport(const Vec3& center, double scale, double width, double height,
                   double rotation, double pixelRatio)
    : center_(center),
      scale_(clampScale(scale)),
      width_(std::max(1.0, width)),
      height_(std::max(1.0, height)),
      rotation_(rotation),
      pixelRatio_(pixelRatio > 0.0 ? pixelRatio : 1.0) {}

ScreenPoint Viewport::worldToScreen(const Vec3& world) const noexcept {
    const double dx = world.x - center_.x;
    const double dy = world.y - center_.y;

    double rx = dx;
    double ry = dy;
    if (rotation_ != 0.0) {
        const double c = std::cos(-rotation_);
        const double s = std::sin(-rotation_);
        rx = dx * c - dy * s;
        ry = dx * s + dy * c;
    }

    return {width_ / 2.0 + rx * scale_,
            // Negated: screen Y grows downward while world Y grows upward.
            height_ / 2.0 - ry * scale_};
}

Vec3 Viewport::screenToWorld(const ScreenPoint& screen) const noexcept {
    const double sx = (screen.x - width_ / 2.0) * unitsPerPixel();
    const double sy = -(screen.y - height_ / 2.0) * unitsPerPixel();

    double wx = sx;
    double wy = sy;
    if (rotation_ != 0.0) {
        const double c = std::cos(rotation_);
        const double s = std::sin(rotation_);
        wx = sx * c - sy * s;
        wy = sx * s + sy * c;
    }

    return Vec3{center_.x + wx, center_.y + wy, 0.0};
}

Mat4 Viewport::worldToScreenMatrix() const noexcept {
    return Mat4::translation(Vec3{width_ / 2.0, height_ / 2.0, 0.0})
        .multiply(Mat4::scaling(Vec3{scale_, -scale_, 1.0}))
        .multiply(Mat4::rotationZ(-rotation_))
        .multiply(Mat4::translation(-center_));
}

Box3 Viewport::visibleBounds() const noexcept {
    const std::array<ScreenPoint, 4> corners{
        {{0.0, 0.0}, {width_, 0.0}, {width_, height_}, {0.0, height_}}};

    Box3 bounds;
    for (const ScreenPoint& corner : corners) {
        bounds = bounds.expandedToPoint(screenToWorld(corner));
    }
    return bounds;
}

Viewport Viewport::panByPixels(double dx, double dy) const {
    // The delta is rotated into world space so panning follows the cursor even
    // in a rotated view.
    const double worldDx = dx * unitsPerPixel();
    const double worldDy = -dy * unitsPerPixel();
    const double c = std::cos(rotation_);
    const double s = std::sin(rotation_);
    const Vec3 offset{worldDx * c - worldDy * s, worldDx * s + worldDy * c, 0.0};
    return withCenter(center_ - offset);
}

Viewport Viewport::withCenter(const Vec3& center) const {
    return Viewport{center, scale_, width_, height_, rotation_, pixelRatio_};
}

Viewport Viewport::withScale(double scale) const {
    return Viewport{center_, scale, width_, height_, rotation_, pixelRatio_};
}

Viewport Viewport::withRotation(double rotation) const {
    return Viewport{center_, scale_, width_, height_, rotation, pixelRatio_};
}

Viewport Viewport::withSize(double width, double height) const {
    return Viewport{center_, scale_, width, height, rotation_, pixelRatio_};
}

Viewport Viewport::withPixelRatio(double pixelRatio) const {
    return Viewport{center_, scale_, width_, height_, rotation_, pixelRatio};
}

Viewport Viewport::zoomAt(const ScreenPoint& anchor, double factor) const {
    const Vec3 worldAnchor = screenToWorld(anchor);
    const Viewport zoomed = withScale(scale_ * factor);
    const Vec3 afterAnchor = zoomed.screenToWorld(anchor);
    // Shift the centre by however far the anchor drifted.
    return zoomed.withCenter(zoomed.center_ + (worldAnchor - afterAnchor));
}

Viewport Viewport::zoomToFit(const Box3& bounds, double marginFraction) const {
    if (bounds.isEmpty()) {
        return *this;
    }

    const double boundsWidth = bounds.width();
    const double boundsHeight = bounds.height();
    if (boundsWidth <= 0.0 && boundsHeight <= 0.0) {
        // A single point: centre on it and keep the current zoom.
        return withCenter(bounds.center());
    }

    const double margin = 1.0 + marginFraction * 2.0;
    const double scaleX = boundsWidth > 0.0 ? width_ / (boundsWidth * margin)
                                            : std::numeric_limits<double>::infinity();
    const double scaleY = boundsHeight > 0.0 ? height_ / (boundsHeight * margin)
                                             : std::numeric_limits<double>::infinity();
    const double fitted = std::min(scaleX, scaleY);

    return withCenter(bounds.center()).withScale(std::isfinite(fitted) ? fitted : scale_);
}

Viewport Viewport::zoomToScreenRect(const ScreenPoint& a, const ScreenPoint& b) const {
    return zoomToFit(Box3::fromCorners(screenToWorld(a), screenToWorld(b)), 0.0);
}

Viewport Viewport::lerpTo(const Viewport& target, double t) const {
    const double clamped = std::clamp(t, 0.0, 1.0);
    const double scale = scale_ * std::pow(target.scale_ / scale_, clamped);
    return Viewport{center_.lerp(target.center_, clamped), scale, target.width_, target.height_,
                    rotation_ + (target.rotation_ - rotation_) * clamped, target.pixelRatio_};
}

bool Viewport::equals(const Viewport& other) const noexcept {
    return center_.equals(other.center_) && scale_ == other.scale_ &&
           width_ == other.width_ && height_ == other.height_ &&
           rotation_ == other.rotation_ && pixelRatio_ == other.pixelRatio_;
}

}  // namespace cad::render
