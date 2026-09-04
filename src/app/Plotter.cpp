#include "Plotter.hpp"

#include "ImageCache.hpp"

#include <QPainter>

#include "Document.hpp"
#include "ScenePainter.hpp"
#include "Theme.hpp"
#include "cad/db/Layout.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/Viewport.hpp"

namespace cad::app {
namespace {

/// A theme for paper.
///
/// The screen theme is dark, and `displayColor` swaps colours that would vanish
/// against the background — so it has to be told the background is now white,
/// or every "contrasts with the background" colour comes out white on white.
[[nodiscard]] Theme paperTheme() {
    Theme theme = currentTheme();
    theme.background = Qt::white;
    theme.foreground = Qt::black;
    return theme;
}

}  // namespace

bool paintPlot(QPainter& painter, Document& document, const QRectF& pageRect,
               const PlotRequest& request, std::size_t* contentOmitted) {
    if (pageRect.width() <= 0.0 || pageRect.height() <= 0.0) {
        return false;
    }

    // What region of the drawing goes on the page.
    Box3 extents;
    if (request.layout != nullptr && !request.layout->isModel()) {
        // The sheet itself, not its contents: paper space is already laid out
        // at paper size, and fitting to the geometry would crop the border and
        // scale the title block differently on every sheet.
        extents = request.layout->sheetRect();
        if (extents.width() <= 0.0 || extents.height() <= 0.0) {
            return false;
        }
    } else {
        extents = render::spaceExtents(document.db(), document.db().modelSpace(),
                                       &document.index());
        if (extents.isEmpty()) {
            return false;
        }
    }

    render::Viewport viewport{extents.center(), 1.0, pageRect.width(), pageRect.height()};
    // No margin: the caller has already handed us the printable area, and
    // insetting again would shrink every plot for no reason.
    viewport = viewport.zoomToFit(extents, 0.0);

    render::SpatialIndex& index = document.index();
    render::SceneBuilder builder{document.db(), index};

    render::SceneOptions options;
    // A plot is not interactive, so the level-of-detail cull that keeps the
    // canvas responsive would only throw away detail the paper can resolve.
    options.minPixelSize = 0.0;
    options.maxItems = std::numeric_limits<std::size_t>::max();
    // Content this application cannot draw is left blank rather than outlined.
    // The outline is an editing aid; printing it puts a mark on a deliverable
    // that is not in the drawing, and other CAD software would not print it either. What
    // was left out comes back on the result instead.
    options.showContentPlaceholders = request.showContentPlaceholders;

    const render::Scene scene =
        request.layout != nullptr && !request.layout->isModel()
            ? builder.buildLayout(*request.layout, viewport, options)
            : builder.build(document.db().modelSpace(), viewport, options);

    const Theme theme = paperTheme();

    painter.save();
    painter.translate(pageRect.topLeft());
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (request.showSheetOutline && request.layout != nullptr && !request.layout->isModel()) {
        const Box3 printable = request.layout->printableRect();
        if (!printable.isEmpty()) {
            const render::ScreenPoint low = viewport.worldToScreen(printable.min);
            const render::ScreenPoint high = viewport.worldToScreen(printable.max);
            QPen pen{QColor{160, 160, 160}};
            pen.setStyle(Qt::DashLine);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawRect(QRectF{QPointF{std::min(low.x, high.x), std::min(low.y, high.y)},
                                    QPointF{std::max(low.x, high.x), std::max(low.y, high.y)}});
        }
    }

    ScenePaintOptions paint;
    paint.monochrome = request.monochrome;
    paint.lineWidthScale = request.lineWidthScale;
    // Scaled with the device, or a plot silently loses its annotation: three
    // device pixels is a readable smudge on a screen and a speck at 600 dpi.
    paint.minimumTextPixels = 3.0 * request.lineWidthScale;
    // Field shading is an editing aid, not ink. other CAD software does not plot it
    // either, and a sheet of grey boxes behind every area is not a drawing.
    paint.showFieldBackground = false;
    // A plot draws its images at the paper's resolution, so the cache is local
    // to this plot rather than shared with the canvas: the levels it decodes
    // are far finer than any screen wants, and keeping them would evict
    // everything the canvas is using to hold pictures nothing will ask for
    // again.
    ImageCache images;
    paint.images = &images;

    paintScene(painter, scene, viewport, theme, paint);
    painter.restore();
    if (contentOmitted != nullptr) {
        *contentOmitted = scene.placeholdersOmitted;
    }
    return true;
}

QString plotDescription(const Document& document, const PlotRequest& request) {
    if (request.layout != nullptr && !request.layout->isModel()) {
        const Vec3 size = request.layout->plot.sheetSize();
        const char* unit = request.layout->plot.paperUnits == PlotPaperUnits::Inches ? "in"
                           : request.layout->plot.paperUnits == PlotPaperUnits::Pixels
                               ? "px"
                               : "mm";
        return QStringLiteral("%1 (%2 × %3 %4)")
            .arg(QString::fromStdString(request.layout->name))
            .arg(size.x, 0, 'f', 1)
            .arg(size.y, 0, 'f', 1)
            .arg(QString::fromUtf8(unit));
    }
    const Box3 extents = document.db().modelExtents();
    if (extents.isEmpty()) {
        return QStringLiteral("Model (empty)");
    }
    return QStringLiteral("Model (%1 × %2)")
        .arg(extents.width(), 0, 'f', 1)
        .arg(extents.height(), 0, 'f', 1);
}

}  // namespace cad::app
