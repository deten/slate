#pragma once

#include <string>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// One cell of a table.
struct TableCell {
    /// The literal text, when the cell holds one.
    std::string text;

    /// The field that produces the value, when the cell is derived.
    ///
    /// A schedule is mostly derived: the areas in a room schedule come from the
    /// polylines that bound the rooms, and the total comes from a formula over
    /// the column. Those cells carry no text of their own — only this handle —
    /// so a table read without following it comes out as an empty grid.
    Handle fieldHandle;

    /// What the field evaluated to, filled in by `refreshTableCells`.
    ///
    /// Held apart from `text` rather than written into it, because the two say
    /// different things: `text` is what the drawing stores and `resolvedText`
    /// is what it currently means. Collapsing them would make an edit to a
    /// literal cell indistinguishable from a stale evaluation.
    std::string resolvedText;

    /// The value to draw: the evaluated one when there is one.
    [[nodiscard]] const std::string& displayText() const noexcept {
        return resolvedText.empty() ? text : resolvedText;
    }

    /// Text height for this cell, which a title row overrides.
    double textHeight = 0.0;

    /// How many cells this one covers, for merged cells. One means itself.
    int rowSpan = 1;
    int columnSpan = 1;
};

/// A table — the ACAD_TABLE entity.
///
/// A room schedule, a door schedule, a general-notes block: the parts of a
/// drawing set that are *read* rather than looked at. They arrive as a grid of
/// cells whose values are mostly fields, and before this they rendered as
/// nothing at all.
///
/// **Generated, not replayed.** The file also carries an anonymous block holding
/// the lines and text other CAD software drew, and a cached bitmap of the same — this one
/// is 11 kilobytes of proxy graphics. Neither is used, for the reason ADR 0005
/// gives for dimensions: a table drawn from its block is a picture of a table,
/// and the moment a value changes, or a column is widened, or the drawing is
/// opened at a different scale, the picture is a lie. The block is kept so a
/// save does not lose it.
class TableEntity final : public Entity {
public:
    /// The anonymous block holding the drawn table. Kept, never drawn.
    Handle blockHandle;
    /// The table style, from group 342.
    Handle styleHandle;

    /// Upper-left corner: a table grows *down* from its insertion point.
    Vec3 position;

    /// Direction of the first row, from group 11. Almost always +X.
    Vec3 direction{1.0, 0.0, 0.0};

    /// Row heights top to bottom, and column widths left to right.
    std::vector<double> rowHeights;
    std::vector<double> columnWidths;

    /// Cells in row-major order; `rowHeights.size() * columnWidths.size()` of
    /// them when the file is well-formed, and possibly fewer when it is not.
    std::vector<TableCell> cells;

    [[nodiscard]] std::string objectType() const override { return "Table"; }

    [[nodiscard]] std::size_t rowCount() const noexcept { return rowHeights.size(); }
    [[nodiscard]] std::size_t columnCount() const noexcept { return columnWidths.size(); }

    /// The cell at a row and column, or null when the file did not supply one.
    [[nodiscard]] const TableCell* cellAt(std::size_t row, std::size_t column) const;

    /// Total width across all columns, and total height down all rows.
    [[nodiscard]] double totalWidth() const;
    [[nodiscard]] double totalHeight() const;

    /// The rectangle a cell occupies, in world coordinates, honouring its span.
    /// The `min` corner is its lower-left.
    [[nodiscard]] Box3 cellBounds(std::size_t row, std::size_t column) const;

    [[nodiscard]] Box3 bounds() const override;
    void transformBy(const Mat4& m) override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Zero anywhere inside the table, so clicking a schedule selects it.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
