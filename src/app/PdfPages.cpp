#include "PdfPages.hpp"

#include <algorithm>
#include <exception>
#include <string>

#include <QPdfDocument>

#include "cad/db/Database.hpp"
#include "cad/db/UnderlayEntity.hpp"

namespace cad::app {

int pdfPageCount(const QString& path) {
    QPdfDocument document;
    if (document.load(path) != QPdfDocument::Error::None) {
        return 0;
    }
    return document.pageCount();
}

std::optional<QSizeF> pdfPageSizePoints(const QString& path, int page) {
    QPdfDocument document;
    if (document.load(path) != QPdfDocument::Error::None || page < 1 ||
        page > document.pageCount()) {
        return std::nullopt;
    }
    const QSizeF size = document.pagePointSize(page - 1);
    if (size.width() <= 0.0 || size.height() <= 0.0) {
        return std::nullopt;
    }
    return size;
}

std::size_t fillUnderlayPageSizes(Database& db) {
    // A PDF page is measured in inches; the drawing in whatever it is in.
    // A unitless drawing takes the page in inches, which is what other CAD
    // software does with it too.
    const DrawingUnits units = db.settings.insertionUnits;
    const double unitsPerInch =
        units == DrawingUnits::Unitless ? 1.0 : unitConversionFactor(DrawingUnits::Inches, units);

    std::size_t filled = 0;
    for (const Handle handle : db.objectHandles()) {
        auto* underlay = db.get<UnderlayEntity>(handle);
        if (underlay == nullptr || underlay->erased || underlay->kind != UnderlayKind::Pdf ||
            (underlay->pageSize.x > 0.0 && underlay->pageSize.y > 0.0)) {
            continue;
        }
        const auto* definition = db.get<UnderlayDefinition>(underlay->definitionHandle);
        if (definition == nullptr || definition->resolvedPath.empty()) {
            continue;
        }
        int page = 1;
        try {
            page = std::max(1, std::stoi(definition->itemName));
        } catch (const std::exception&) {
            page = 1;
        }
        const std::optional<QSizeF> points =
            pdfPageSizePoints(QString::fromStdString(definition->resolvedPath), page);
        if (!points) {
            continue;
        }
        underlay->pageSize = Vec3{points->width() / 72.0 * unitsPerInch,
                                  points->height() / 72.0 * unitsPerInch, 0.0};
        ++filled;
    }
    return filled;
}

}  // namespace cad::app
