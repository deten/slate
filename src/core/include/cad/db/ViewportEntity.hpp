#pragma once

#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// A window from a sheet onto the model — the VIEWPORT entity.
///
/// This is what makes paper space work: a rectangle on the sheet showing part
/// of model space at a chosen scale. A set of construction drawings is mostly
/// one model seen through a dozen of these at different scales and clipped to
/// different areas.
///
/// Two coordinate systems meet here, and confusing them is the classic paper
/// space bug:
///
///  - `center` and `width`/`height` are in **paper** units, describing where
///    the window sits on the sheet.
///  - `viewHeight` is in **model** units, describing how much of the model the
///    window covers.
///
/// The ratio between them is the viewport's scale, and it is derived rather
/// than stored so the two can never disagree.
///
/// There is a third system, and it is the one that catches people out:
/// `viewCenter` is in neither. It is a *display* coordinate, measured from
/// `viewTarget`. `worldViewCenter` is the model point the window is actually
/// centred on, and is what every consumer should use.
class ViewportEntity final : public Entity {
public:
    /// Centre of the window on the sheet, in paper units.
    Vec3 center;
    /// Size of the window on the sheet, in paper units.
    double width = 100.0;
    double height = 100.0;

    /// DXF group 69. The paper space viewport itself is number 1 and stands
    /// for the sheet rather than a window onto the model; real viewports are
    /// numbered from 2.
    int viewportId = 2;

    /// Centre of the view, in the **display** coordinate system.
    ///
    /// Not world coordinates, despite naming the point the window is centred
    /// on. The DCS origin is `viewTarget`, so the world centre is the two
    /// added — see `worldViewCenter`, which is what every consumer wants.
    Vec3 viewCenter;
    /// Height of the view in model units. With the window's paper height, this
    /// is what fixes the scale.
    double viewHeight = 100.0;
    /// View twist about the view direction, in radians.
    double twist = 0.0;

    /// Direction the view looks along; +Z for an ordinary plan viewport.
    Vec3 viewDirection = Vec3::unitZ();
    /// Length of the direction vector as the file had it. other CAD software keeps the
    /// distance to the target there; it is written back as read.
    double viewDirectionLength = 1.0;
    Vec3 viewTarget;

    /// Group 90 status flags. Mostly view settings — perspective, clipping,
    /// snap and grid — with one bit that matters here: `kStatusOff`.
    int statusFlags = 0;

    /// The one status bit that decides whether a viewport draws.
    ///
    /// It is *set* to turn the viewport off, which is the opposite of the
    /// obvious reading. other CAD software switches viewports off rather than deleting
    /// them, so a whole drawing set can be made to render nothing by getting
    /// this backwards — the low bits it is easy to mistake for it are
    /// perspective mode and front clipping, which a real drawing sets freely.
    static constexpr int kStatusOff = 0x10000;
    /// True when the viewport's contents are locked against zooming.
    bool locked = false;

    /// Layers frozen in this viewport alone — the mechanism that lets one
    /// model serve a demolition sheet and a new-work sheet.
    std::vector<Handle> frozenLayerHandles;

    /// Handle of the entity clipping this viewport to a non-rectangular
    /// boundary, when one is set.
    Handle clipEntityHandle;

    [[nodiscard]] std::string objectType() const override { return "Viewport"; }

    /// True when this is the sheet's own pseudo-viewport rather than a window.
    ///
    /// It is stored like any other viewport but represents the paper itself,
    /// and drawing it would put a rectangle around the whole sheet showing a
    /// second copy of the model.
    [[nodiscard]] bool isPaperSpaceViewport() const noexcept { return viewportId <= 1; }

    /// True when the viewport is switched on and should show its contents.
    [[nodiscard]] bool isOn() const noexcept { return (statusFlags & kStatusOff) == 0; }

    /// Model units per paper unit.
    ///
    /// The number a drafter thinks of as the viewport's scale, inverted: a
    /// 1:50 detail has a scale factor of 50.
    [[nodiscard]] double modelPerPaper() const noexcept {
        return height > 0.0 ? viewHeight / height : 1.0;
    }

    /// The point of the model the window is centred on, in world coordinates.
    ///
    /// other CAD software stores a view as a *target* plus an offset from it: group 17 is
    /// the target in world coordinates, and group 12 is the centre expressed in
    /// the display coordinate system, whose origin sits on that target. Reading
    /// group 12 as a world point works only while the target is the origin —
    /// which is exactly what a viewport created by ZOOM Extents and never panned
    /// looks like, so it survives every test drawing and fails on every real
    /// sheet. The drawing then appears offset by the target, at the right scale,
    /// with most of it outside the window.
    [[nodiscard]] Vec3 worldViewCenter() const;

    /// The window's rectangle on the sheet, in paper units.
    [[nodiscard]] Box3 paperRect() const;

    /// The region of model space this window shows.
    [[nodiscard]] Box3 modelRegion() const;

    /// Transform taking model coordinates onto the sheet.
    ///
    /// This is the whole of paper space in one function: scale by the inverse
    /// of `modelPerPaper`, rotate by the twist, and place the view centre at
    /// the window centre.
    [[nodiscard]] Mat4 modelToPaper() const;

    /// True when `layer` is frozen in this viewport specifically.
    [[nodiscard]] bool isLayerFrozen(Handle layer) const;

    [[nodiscard]] Box3 bounds() const override { return paperRect(); }

    /// The window's frame, so a viewport can be selected and snapped to by its
    /// edge like any rectangle.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

}  // namespace cad
