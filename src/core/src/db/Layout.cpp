#include "cad/db/Layout.hpp"

#include <algorithm>
#include <stdexcept>

#include "cad/db/SymbolTable.hpp"

namespace cad {

double PlotSettings::unitScale() const {
    switch (paperUnits) {
        case PlotPaperUnits::Inches: return 25.4;
        // A pixel has no size in millimetres. Treating the stored numbers as
        // already being in paper units is the only reading that keeps a raster
        // page the size it says it is.
        case PlotPaperUnits::Pixels: return 1.0;
        case PlotPaperUnits::Millimetres: break;
    }
    return 1.0;
}

double PlotSettings::plotScale() const {
    // A zero either way means "not set" rather than "infinitely small". Files
    // written before the custom-scale codes existed leave both at zero.
    if (numerator <= 0.0 || denominator <= 0.0) {
        return 1.0;
    }
    return numerator / denominator;
}

PlotSettings::Margins PlotSettings::rotatedMargins() const {
    // Margins are stated against the unrotated sheet, so rotating the plot
    // rotates which edge each one sits against. A quarter turn clockwise of
    // the paper puts its bottom edge on the left, its right edge on the
    // bottom, and so on around.
    Margins m{marginLeft, marginBottom, marginRight, marginTop};
    for (int turned = ((rotation % 360) + 360) % 360; turned > 0; turned -= 90) {
        const double previousLeft = m.left;
        m.left = m.bottom;
        m.bottom = m.right;
        m.right = m.top;
        m.top = previousLeft;
    }
    return m;
}

bool PlotSettings::isRotatedQuarterTurn() const {
    const int normalized = ((rotation % 360) + 360) % 360;
    return normalized == 90 || normalized == 270;
}

Vec3 PlotSettings::sheetSize() const {
    const double scale = unitScale();
    const double width = paperWidth / scale;
    const double height = paperHeight / scale;
    return isRotatedQuarterTurn() ? Vec3{height, width, 0.0} : Vec3{width, height, 0.0};
}

Box3 PlotSettings::printableArea() const {
    const Vec3 size = sheetSize();
    const double scale = unitScale();
    const Margins m = rotatedMargins();

    const double lowX = m.left / scale;
    const double lowY = m.bottom / scale;
    const double highX = std::max(lowX, size.x - m.right / scale);
    const double highY = std::max(lowY, size.y - m.top / scale);
    return Box3::fromCorners(Vec3{lowX, lowY, 0.0}, Vec3{highX, highY, 0.0});
}

bool Layout::isModel() const {
    return normalizeSymbolName(name) == normalizeSymbolName(kLayoutModel);
}

Box3 Layout::printableRect() const {
    // The stored limits when the drawing has them, because they are the only
    // record of where the page *sits*. A fit-to-paper layout moves the page off
    // the paper-space origin, and nothing else in the file says by how much.
    const Box3 limits = Box3::fromCorners(limitsMin, limitsMax);
    if (limits.width() > 0.0 && limits.height() > 0.0) {
        return limits;
    }

    // Otherwise the page is at the origin — the assumption every drawing
    // written before layouts, and every sheet we create ourselves, satisfies.
    const double scale = plot.plotScale();
    const Box3 area = plot.printableArea();
    return Box3::fromCorners(area.min * (1.0 / scale), area.max * (1.0 / scale));
}

Box3 Layout::sheetRect() const {
    const Box3 printable = printableRect();
    const PlotSettings::Margins m = plot.rotatedMargins();

    // Millimetres per paper-space unit: the unit conversion and the plot scale
    // compound, because a margin is a physical distance on a physical page.
    const double perUnit = plot.unitScale() * plot.plotScale();
    if (perUnit <= 0.0) {
        return printable;
    }
    return Box3::fromCorners(
        printable.min - Vec3{m.left / perUnit, m.bottom / perUnit, 0.0},
        printable.max + Vec3{m.right / perUnit, m.top / perUnit, 0.0});
}

DbObjectPtr Layout::clone() const {
    auto copy = std::make_unique<Layout>();
    copyBaseInto(*copy);
    copy->name = name;
    copy->blockRecordHandle = blockRecordHandle;
    copy->tabOrder = tabOrder;
    copy->plot = plot;
    copy->minExtents = minExtents;
    copy->maxExtents = maxExtents;
    copy->limitsMin = limitsMin;
    copy->limitsMax = limitsMax;
    copy->viewportHandles = viewportHandles;
    return copy;
}

void Layout::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const Layout*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a layout");
    }
    restoreBaseFrom(*source);
    name = source->name;
    blockRecordHandle = source->blockRecordHandle;
    tabOrder = source->tabOrder;
    plot = source->plot;
    minExtents = source->minExtents;
    maxExtents = source->maxExtents;
    limitsMin = source->limitsMin;
    limitsMax = source->limitsMax;
    viewportHandles = source->viewportHandles;
}

std::vector<Handle> Layout::referencedHandles() const {
    std::vector<Handle> handles = DbObject::referencedHandles();
    if (!blockRecordHandle.isNull()) {
        handles.push_back(blockRecordHandle);
    }
    for (const Handle viewport : viewportHandles) {
        if (!viewport.isNull()) {
            handles.push_back(viewport);
        }
    }
    return handles;
}

// ------------------------------------------------------------------ raw object

DbObjectPtr RawObject::clone() const {
    auto copy = std::make_unique<RawObject>();
    copyBaseInto(*copy);
    copy->dxfType = dxfType;
    return copy;
}

void RawObject::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const RawObject*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a raw object");
    }
    restoreBaseFrom(*source);
    dxfType = source->dxfType;
}

}  // namespace cad
