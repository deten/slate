#pragma once

#include <cstddef>

#include <QRectF>
#include <QString>

namespace cad {
class Layout;
}

class QPainter;

namespace cad::app {

class Document;

/// What to plot and how.
struct PlotRequest {
    /// The sheet to plot. Null plots model space, fitted to its extents —
    /// which is what a user means by printing a drawing that has no layouts.
    const Layout* layout = nullptr;

    /// Draw everything in one colour.
    ///
    /// On by default because it is almost always right: a drawing authored
    /// against a dark background is largely white, and white ink is no ink. It
    /// is also what a monochrome plot style does, which is how most
    /// construction drawings are actually issued.
    bool monochrome = true;

    /// Scale line widths up for the device. A printer's pixels are far smaller
    /// than a screen's, so a hairline that reads well on a monitor is close to
    /// invisible at 600 dpi.
    double lineWidthScale = 1.0;

    /// Draw the sheet outline and its printable area. Useful in a preview,
    /// wrong on the paper itself — the page *is* the paper.
    bool showSheetOutline = false;

    /// Outline content this application cannot draw — a missing image, an
    /// underlay's page, an OLE object.
    ///
    /// Off for paper and on for a preview, which is the same split as the sheet
    /// outline and for the same reason. The outline is an editing aid: printing
    /// it puts a mark on a deliverable that is not in the drawing. Showing it in
    /// the preview is how the drafter finds out the sheet has a hole in it
    /// *before* spending a sheet of A1 on it.
    bool showContentPlaceholders = false;
};

/// Render a drawing onto a page.
///
/// Takes a painter rather than a printer so that a printer, a PDF writer, a
/// preview widget and an image all go through one path. Anything else risks a
/// plot that differs from its own preview, which is the failure mode that
/// costs a sheet of A1 to discover.
///
/// The sheet is fitted to `pageRect`, which the caller sets to the device's
/// *printable* area rather than its full page — plotting into the physical
/// margin is how a title block loses its border.
///
/// @param contentOmitted receives the number of attachments left blank because
///        their outline was suppressed, when not null. A plot that silently
///        drops somebody's site photograph is a plot that reaches a client with
///        a hole in it.
/// @returns false when there is nothing to plot.
bool paintPlot(QPainter& painter, Document& document, const QRectF& pageRect,
               const PlotRequest& request, std::size_t* contentOmitted = nullptr);

/// A short description of what would be plotted, for a dialog or the log.
[[nodiscard]] QString plotDescription(const Document& document, const PlotRequest& request);

}  // namespace cad::app
