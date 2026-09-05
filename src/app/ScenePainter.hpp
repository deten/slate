#pragma once

#include <QColor>
#include <QPolygonF>
#include <vector>

#include "Theme.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/Viewport.hpp"

class QPainter;

namespace cad::app {

/// How a scene should be drawn.
///
/// The screen and a sheet of paper want the same geometry and different
/// presentation, and the difference is entirely in these few values rather than
/// in a second copy of the drawing code.
struct ScenePaintOptions {
    /// Force everything to one colour. Plotting wants this: a drawing authored
    /// against a dark background is mostly white, and white on paper is
    /// nothing at all.
    bool monochrome = false;
    QColor monochromeColor{Qt::black};

    /// Multiplier on line widths. A printer's pixels are far smaller than a
    /// screen's, so a one-pixel hairline that reads well on a monitor is close
    /// to invisible at 600 dpi.
    double lineWidthScale = 1.0;

    /// Device pixels per millimetre of paper, when lineweights are to show
    /// at their true width. Zero draws every line as a hairline, which is
    /// how a screen normally shows a drawing and how it stays readable when
    /// zoomed out.
    double pixelsPerMillimetre = 0.0;

    /// Smallest text worth drawing, in device pixels. Below this, text is an
    /// unreadable smudge on screen — but on paper the threshold has to scale
    /// with the device or a plot silently loses its annotation.
    double minimumTextPixels = 3.0;

    /// Where decoded raster images come from.
    ///
    /// Optional: without one, images draw as their outline, which is exactly
    /// what a missing file does and is the right answer for a caller that has
    /// no business decoding pictures. Callers that show images share a cache,
    /// so panning does not re-decode a 34-megapixel scan every frame.
    class ImageCache* images = nullptr;

    /// Shade text produced by a field, as the `FIELDDISPLAY` setting in other CAD software does.
    ///
    /// On by default on screen and off for plotting: it is an editing aid, and
    /// a grey box behind every derived number would be printed onto the sheet.
    bool showFieldBackground = true;
};

/// Paint a built scene.
///
/// Shared by the canvas and the plotter, which is the point: a printed drawing
/// that went through different code from the one on screen would differ from it
/// in ways nobody discovers until the paper is in their hand. A QPainter is a
/// QPainter, whether it is aimed at a widget, a printer or a PDF.
void paintScene(QPainter& painter, const render::Scene& scene,
                const render::Viewport& viewport, const Theme& theme,
                const ScenePaintOptions& options = {});

/// Substitute a colour that would be invisible against the background.
///
/// White geometry on a light background and black on a dark one both vanish;
/// swapping to the foreground colour keeps the drawing readable without
/// touching the document, which stores what the author chose.
[[nodiscard]] QColor displayColor(std::uint32_t rgb, std::uint8_t alpha, const Theme& theme);

/// Project world points to a device-space polygon.
[[nodiscard]] QPolygonF toPolygon(const render::Viewport& viewport,
                                  const std::vector<Vec3>& points);

}  // namespace cad::app
