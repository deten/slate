#include "cad/db/TableEntity.hpp"

#include <numeric>

#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

const TableCell* TableEntity::cellAt(std::size_t row, std::size_t column) const {
    if (row >= rowCount() || column >= columnCount()) {
        return nullptr;
    }
    const std::size_t index = row * columnCount() + column;
    return index < cells.size() ? &cells[index] : nullptr;
}

double TableEntity::totalWidth() const {
    return std::accumulate(columnWidths.begin(), columnWidths.end(), 0.0);
}

double TableEntity::totalHeight() const {
    return std::accumulate(rowHeights.begin(), rowHeights.end(), 0.0);
}

Box3 TableEntity::cellBounds(std::size_t row, std::size_t column) const {
    if (row >= rowCount() || column >= columnCount()) {
        return Box3::empty();
    }

    double left = 0.0;
    for (std::size_t i = 0; i < column; ++i) {
        left += columnWidths[i];
    }
    double top = 0.0;
    for (std::size_t i = 0; i < row; ++i) {
        top += rowHeights[i];
    }

    // A merged cell covers the columns and rows it spans, so its text is
    // centred across the whole of it rather than in its first cell.
    const TableCell* cell = cellAt(row, column);
    const std::size_t columnSpan =
        cell == nullptr ? 1 : static_cast<std::size_t>(std::max(1, cell->columnSpan));
    const std::size_t rowSpan =
        cell == nullptr ? 1 : static_cast<std::size_t>(std::max(1, cell->rowSpan));

    double width = 0.0;
    for (std::size_t i = column; i < column + columnSpan && i < columnCount(); ++i) {
        width += columnWidths[i];
    }
    double height = 0.0;
    for (std::size_t i = row; i < row + rowSpan && i < rowCount(); ++i) {
        height += rowHeights[i];
    }

    // Down from the insertion point, which is where a table grows.
    const Vec3 upperLeft{position.x + left, position.y - top, position.z};
    return Box3::fromCorners(upperLeft,
                             Vec3{upperLeft.x + width, upperLeft.y - height, position.z});
}

Box3 TableEntity::bounds() const {
    if (rowCount() == 0 || columnCount() == 0) {
        // An empty table still has a location; reporting nothing would drop it
        // out of ZOOM EXTENTS and leave the user unable to find it.
        return Box3::fromPoints({position});
    }
    return Box3::fromCorners(
        position, Vec3{position.x + totalWidth(), position.y - totalHeight(), position.z});
}

void TableEntity::transformBy(const Mat4& m) {
    position = m.transformPoint(position);
    direction = m.transformVector(direction);

    // A table is read for its meaning and written back for its *bytes*: its
    // per-cell blocks reuse group codes, so nothing in it is claimed and there
    // is no writer for it. Those bytes are therefore what a save emits, and
    // moving the entity without moving them put the table back where it started
    // -- the drawing on screen and the drawing in the file disagreed, silently,
    // and the file won.
    if (preserved != nullptr) {
        preserved->rewritePoint(10, position);
        preserved->rewritePoint(11, direction);
    }

    // Row and column sizes are lengths, so they scale. A table dragged through
    // a SCALE that left them alone would keep its old grid and lose its text.
    const double factor = m.scaleFactor2();
    if (factor > 0.0 && factor != 1.0) {
        for (double& height : rowHeights) {
            height *= factor;
        }
        for (double& width : columnWidths) {
            width *= factor;
        }
        for (TableCell& cell : cells) {
            cell.textHeight *= factor;
        }
    }
}

std::vector<CurvePtr> TableEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    if (rowCount() == 0 || columnCount() == 0) {
        return curves;
    }

    const double width = totalWidth();
    const double height = totalHeight();

    // The outer border first, then the interior rules. Separate curves rather
    // than one polyline, because they are separate lines: snapping to a column
    // rule should find that rule, not a vertex of a path that happens to pass
    // through it.
    curves.push_back(std::make_unique<Polyline>(Polyline::fromPoints(
        {position, Vec3{position.x + width, position.y, position.z},
         Vec3{position.x + width, position.y - height, position.z},
         Vec3{position.x, position.y - height, position.z}},
        /*closed=*/true)));

    double y = position.y;
    for (std::size_t row = 0; row + 1 < rowCount(); ++row) {
        y -= rowHeights[row];
        curves.push_back(std::make_unique<LineSegment>(
            Vec3{position.x, y, position.z}, Vec3{position.x + width, y, position.z}));
    }

    double x = position.x;
    for (std::size_t column = 0; column + 1 < columnCount(); ++column) {
        x += columnWidths[column];
        curves.push_back(std::make_unique<LineSegment>(
            Vec3{x, position.y, position.z}, Vec3{x, position.y - height, position.z}));
    }
    return curves;
}

double TableEntity::distanceToPoint(const Vec3& p) const {
    const Box3 extents = bounds();
    if (extents.isEmpty()) {
        return position.distanceTo(p);
    }
    // Picked by its area, like a hatch: clicking the middle of a schedule must
    // select the schedule, not miss between two rules.
    if (extents.containsPoint(p)) {
        return 0.0;
    }
    const double dx = std::max({extents.min.x - p.x, 0.0, p.x - extents.max.x});
    const double dy = std::max({extents.min.y - p.y, 0.0, p.y - extents.max.y});
    return std::hypot(dx, dy);
}

std::vector<GripPoint> TableEntity::gripPoints() const {
    // The insertion point, which is the corner other CAD software gives a table too.
    // Column and row grips would resize the table, which needs a writer for the
    // entity before it can be saved — so they are not offered rather than
    // offered and quietly lost.
    return {GripPoint{position, GripKind::Insertion, 0}};
}

void TableEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.kind == GripKind::Insertion) {
        position = to;
    }
}

std::vector<Handle> TableEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    if (!blockHandle.isNull()) {
        handles.push_back(blockHandle);
    }
    if (!styleHandle.isNull()) {
        handles.push_back(styleHandle);
    }
    for (const TableCell& cell : cells) {
        if (!cell.fieldHandle.isNull()) {
            handles.push_back(cell.fieldHandle);
        }
    }
    return handles;
}

DbObjectPtr TableEntity::clone() const {
    auto copy = std::make_unique<TableEntity>();
    copyEntityInto(*copy);
    copy->blockHandle = blockHandle;
    copy->styleHandle = styleHandle;
    copy->position = position;
    copy->direction = direction;
    copy->rowHeights = rowHeights;
    copy->columnWidths = columnWidths;
    copy->cells = cells;
    return copy;
}

void TableEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const TableEntity*>(&snapshot);
    if (source == nullptr) {
        return;
    }
    restoreEntityFrom(*source);
    blockHandle = source->blockHandle;
    styleHandle = source->styleHandle;
    position = source->position;
    direction = source->direction;
    rowHeights = source->rowHeights;
    columnWidths = source->columnWidths;
    cells = source->cells;
}

}  // namespace cad
