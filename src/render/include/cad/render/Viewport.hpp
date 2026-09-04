#pragma once

#include "cad/math/Box3.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/math/Vec3.hpp"

namespace cad::render {

/// A position in device-independent pixels, origin at the surface top-left.
struct ScreenPoint {
    double x = 0.0;
    double y = 0.0;
};

/// Bounds on zoom.
///
/// A CAD drawing can legitimately span millimetres to kilometres, so the range
/// is wide — but not unbounded: beyond roughly these limits, double-precision
/// arithmetic in screen space loses enough accuracy that geometry visibly
/// jitters as it is panned.
inline constexpr double kMinScale = 1e-9;
inline constexpr double kMaxScale = 1e9;

/// The mapping between world coordinates and the screen.
///
/// Immutable: every navigation operation returns a new viewport. That makes
/// view state trivially comparable (so a redraw is only scheduled on real
/// change), trivially restorable (named views, undo of a zoom), and impossible
/// to mutate from two places at once during an animation.
///
/// Two invariants the whole renderer depends on:
///
///  - **Y is flipped.** World Y increases upward, as every drafter expects;
///    screen Y increases downward. The flip lives here and nowhere else.
///  - **Zoom is `scale`, in pixels per world unit.** Storing pixels-per-unit
///    rather than a percentage keeps every derived quantity — line widths, snap
///    apertures, tessellation tolerance, text sizes — one multiplication away,
///    with no scale-factor conventions to get wrong.
class Viewport {
public:
    Viewport() = default;

    Viewport(const Vec3& center, double scale, double width, double height,
             double rotation = 0.0, double pixelRatio = 1.0);

    [[nodiscard]] const Vec3& center() const noexcept { return center_; }
    [[nodiscard]] double scale() const noexcept { return scale_; }
    [[nodiscard]] double width() const noexcept { return width_; }
    [[nodiscard]] double height() const noexcept { return height_; }
    [[nodiscard]] double rotation() const noexcept { return rotation_; }

    /// Device pixel ratio. Kept here so hit testing, which works in
    /// device-independent pixels, and rendering, which works in device pixels,
    /// cannot disagree about the conversion.
    [[nodiscard]] double pixelRatio() const noexcept { return pixelRatio_; }

    /// World units per screen pixel — the reciprocal of `scale`.
    [[nodiscard]] double unitsPerPixel() const noexcept { return 1.0 / scale_; }

    [[nodiscard]] ScreenPoint worldToScreen(const Vec3& world) const noexcept;

    /// Convert a screen point to world coordinates on the Z = 0 plane.
    [[nodiscard]] Vec3 screenToWorld(const ScreenPoint& screen) const noexcept;

    /// The full world-to-screen transform, for uploading to a GPU.
    [[nodiscard]] Mat4 worldToScreenMatrix() const noexcept;

    /// World-space region currently visible.
    ///
    /// With rotation the visible region is a rotated rectangle; this is its
    /// axis-aligned bound, which is what culling needs.
    [[nodiscard]] Box3 visibleBounds() const noexcept;

    [[nodiscard]] double pixelsToWorld(double pixels) const noexcept {
        return pixels * unitsPerPixel();
    }

    [[nodiscard]] double worldToPixels(double units) const noexcept { return units * scale_; }

    // -------------------------------------------------------------- navigation

    /// Pan by a screen-space delta, as during a drag.
    [[nodiscard]] Viewport panByPixels(double dx, double dy) const;

    [[nodiscard]] Viewport withCenter(const Vec3& center) const;
    [[nodiscard]] Viewport withScale(double scale) const;
    [[nodiscard]] Viewport withRotation(double rotation) const;
    [[nodiscard]] Viewport withSize(double width, double height) const;
    [[nodiscard]] Viewport withPixelRatio(double pixelRatio) const;

    /// Zoom by a factor while keeping one screen point anchored.
    ///
    /// This is what makes wheel and trackpad zoom feel right: the world point
    /// under the cursor stays under the cursor. Zooming about the view centre
    /// instead — the naive implementation — forces a pan after every zoom and
    /// is immediately noticeable as wrong.
    [[nodiscard]] Viewport zoomAt(const ScreenPoint& anchor, double factor) const;

    [[nodiscard]] Viewport zoomBy(double factor) const { return withScale(scale_ * factor); }

    /// Fit a world region to the surface with a margin.
    ///
    /// Backs ZOOM EXTENTS and ZOOM WINDOW. An empty or degenerate box — a
    /// drawing containing a single point, or nothing — would otherwise produce
    /// an infinite scale, so it falls back to a sensible view.
    [[nodiscard]] Viewport zoomToFit(const Box3& bounds, double marginFraction = 0.05) const;

    /// Zoom to a screen-space rectangle, as dragged by the user.
    [[nodiscard]] Viewport zoomToScreenRect(const ScreenPoint& a, const ScreenPoint& b) const;

    /// Interpolate toward another viewport, for animated navigation.
    ///
    /// Scale is interpolated *geometrically*, not linearly. Zoom is
    /// multiplicative, so a linear path from 1x to 100x spends most of the
    /// animation already zoomed in and lurches at the start; the geometric path
    /// is what reads as smooth.
    [[nodiscard]] Viewport lerpTo(const Viewport& target, double t) const;

    [[nodiscard]] bool equals(const Viewport& other) const noexcept;

private:
    Vec3 center_;
    double scale_ = 1.0;
    double width_ = 800.0;
    double height_ = 600.0;
    double rotation_ = 0.0;
    double pixelRatio_ = 1.0;
};

/// Chord tolerance for tessellating curves at the current zoom.
///
/// Tying tessellation to the viewport is what makes curves smooth when zoomed
/// in without generating tens of thousands of segments for a circle three
/// pixels across. Half a pixel is below the threshold of visibility.
[[nodiscard]] inline double chordToleranceFor(const Viewport& viewport,
                                              double subPixel = 0.5) noexcept {
    return viewport.pixelsToWorld(subPixel);
}

}  // namespace cad::render
