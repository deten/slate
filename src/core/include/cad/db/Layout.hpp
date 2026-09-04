#pragma once

#include <string>
#include <vector>

#include "cad/db/DbObject.hpp"
#include "cad/math/Box3.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// The name of the layout that is model space rather than a sheet.
inline constexpr const char* kLayoutModel = "Model";

/// The unit the paper-space coordinate system is measured in (DXF code 72).
///
/// Not a display preference: it is the unit paper-space *entities* are drawn
/// in, so it is the conversion between a sheet's stated size and the geometry
/// sitting on it.
enum class PlotPaperUnits {
    Inches = 0,
    Millimetres = 1,
    /// Raster devices, where the sizes are already pixel counts.
    Pixels = 2,
};

/// How a sheet is oriented and scaled when plotted.
///
/// Kept as data rather than resolved into a transform, because a layout is
/// edited far more often than it is plotted and the user's answer to "what size
/// is this sheet" has to survive a round trip unchanged.
struct PlotSettings {
    /// Printer or plotter configuration name, as stored.
    std::string plotterName;
    /// Paper size name, e.g. `ISO_A1_(841.00_x_594.00_MM)`.
    std::string paperName;

    /// Sheet size in millimetres, always — DXF stores plot geometry in
    /// millimetres regardless of the drawing's own units.
    ///
    /// This is *not* the size to draw the sheet at. Paper-space entities are in
    /// `paperUnits`, so an imperial drawing puts its title block at 11 x 8.5
    /// while these two read 279.4 x 215.9. Use `sheetSize()`.
    double paperWidth = 210.0;
    double paperHeight = 297.0;

    /// Unprintable margins in millimetres: left, bottom, right, top.
    double marginLeft = 0.0;
    double marginBottom = 0.0;
    double marginRight = 0.0;
    double marginTop = 0.0;

    /// The unit paper-space geometry is drawn in.
    PlotPaperUnits paperUnits = PlotPaperUnits::Millimetres;

    /// Plot scale as a ratio of drawing units to paper millimetres.
    double numerator = 1.0;
    double denominator = 1.0;
    /// 0 degrees, 90, 180 or 270.
    int rotation = 0;
    /// True when the sheet is plotted mirrored.
    bool plotUpsideDown = false;

    /// Unprintable margins after the plot rotation has been applied, still in
    /// millimetres. A quarter turn moves each margin to a different edge.
    struct Margins {
        double left = 0.0;
        double bottom = 0.0;
        double right = 0.0;
        double top = 0.0;
    };
    [[nodiscard]] Margins rotatedMargins() const;

    /// Millimetres per paper-space unit.
    [[nodiscard]] double unitScale() const;

    /// Printed size per paper-space unit — the plot scale, as a single number.
    ///
    /// Usually 1: a sheet is normally laid out at the size it prints at, which
    /// is the whole point of paper space. It is *not* 1 when the layout is set
    /// to "fit to paper", and then the paper is not the size the plot settings
    /// state: an 11 x 8.5 sheet fitted at 1.309 is 8.4 x 6.49 in the units its
    /// own title block is drawn in. Draw it 11 x 8.5 anyway and everything on
    /// the sheet comes out a third too small and off-centre.
    [[nodiscard]] double plotScale() const;

    /// True when `rotation` puts the sheet on its side, swapping width for
    /// height.
    [[nodiscard]] bool isRotatedQuarterTurn() const;

    /// The sheet's size in paper-space units, as the drawing sees it —
    /// converted from millimetres and with the plot rotation applied.
    ///
    /// A US Letter sheet plotted at 90 degrees from an imperial drawing is
    /// stored as 215.9 x 279.4 and drawn as 11 x 8.5. Drawing the stored
    /// numbers instead puts a portrait page twenty-five times too large behind
    /// a title block huddled in one corner.
    [[nodiscard]] Vec3 sheetSize() const;

    /// The printable area at 1:1, with the origin at the sheet's bottom-left
    /// corner. Where the sheet actually *sits* in paper space is the layout's
    /// business, not the plot settings' — see `Layout::printableRect`.
    [[nodiscard]] Box3 printableArea() const;
};

/// A paper-space sheet.
///
/// One of these exists for every tab in a drawing, including Model. A layout
/// pairs a *name* and plot configuration with a block record holding the
/// sheet's own geometry — the title block, notes and viewport frames that live
/// on paper rather than in the model.
///
/// Modelled as a real object rather than inferred from the `*Paper_Spacennn`
/// block records, because the tab order, the sheet size and the plot setup all
/// live here and none of them can be recovered from the block. A drawing whose
/// layouts were dropped opens showing only the model, which for a set of
/// construction drawings is losing the deliverable and keeping the working
/// notes.
class Layout final : public DbObject {
public:
    /// Tab name, as shown and as stored in group 1.
    std::string name;

    /// The block record holding this sheet's geometry.
    ///
    /// For the Model layout this is `*Model_Space`; for a sheet it is one of
    /// the `*Paper_Space` records.
    Handle blockRecordHandle;

    /// Position among the tabs. other CAD software writes Model as 0 and sheets from 1.
    int tabOrder = 0;

    PlotSettings plot;

    /// Extents of the sheet's own geometry, as last computed by the authoring
    /// application. Informational; the renderer recomputes.
    Vec3 minExtents;
    Vec3 maxExtents;

    /// The drawing limits (LIMMIN/LIMMAX), which for a layout is where the
    /// printable area lands in paper-space coordinates.
    ///
    /// Not the user-set limits of a hand-drafted model: other CAD software maintains these
    /// for a layout, and they are the only record of the page's *position* once
    /// a fit-to-paper scale has moved it off the origin.
    Vec3 limitsMin;
    Vec3 limitsMax;

    /// Handles of the viewports on this sheet, in the order they were created.
    ///
    /// The first is the *paper space viewport* itself, which represents the
    /// sheet rather than a window onto the model, and is not drawn.
    std::vector<Handle> viewportHandles;

    [[nodiscard]] std::string objectType() const override { return "Layout"; }

    /// True for the layout that shows the model rather than a sheet.
    [[nodiscard]] bool isModel() const;

    /// Where the printable area sits, in the coordinates the sheet's own
    /// geometry uses. This is the rectangle a title block is drawn against.
    [[nodiscard]] Box3 printableRect() const;

    /// Where the whole sheet sits, in paper-space coordinates: the printable
    /// area plus the unprintable margins.
    [[nodiscard]] Box3 sheetRect() const;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

/// An object read from the file's OBJECTS section that this application does
/// not interpret.
///
/// The OBJECTS section is where other CAD software keeps dictionaries, plot settings,
/// layer states, table styles, multileader styles, and a great deal of
/// third-party data. Skipping the section on read means *deleting* all of it on
/// save — which for a professional drawing is the single most destructive thing
/// a file reader can do, and it does it silently.
///
/// So every object is kept: interpreted where we can, and verbatim where we
/// cannot. This type is the "verbatim" half. It is never drawn, never selected
/// and never edited; it exists to be written back out exactly as it arrived.
class RawObject final : public DbObject {
public:
    /// The DXF type name, e.g. `DICTIONARY` or `MLEADERSTYLE`.
    std::string dxfType;

    [[nodiscard]] std::string objectType() const override { return "Raw:" + dxfType; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
