#pragma once

#include <optional>
#include <string>

#include "cad/db/FieldObject.hpp"
#include "cad/db/Property.hpp"

namespace cad {

class TableEntity;

class Database;

/// How a field's raw value is turned into the text that appears on the drawing.
///
/// other CAD software writes this as a format string on the field code, `\f "%lu2%pr2%ct8
/// [0.0069444444444444]"`. It is terse and it matters: that particular one is
/// how a room label reading `423.08` is produced from an area of 60924 square
/// inches — the conversion factor is 1/144, and without it the drawing states
/// the area of a house in the hundreds of thousands.
struct FieldFormat {
    /// Decimal places, from `%pr`. Absent means "as many as it takes".
    std::optional<int> precision;
    /// Multiplier from `%ct<n>[factor]`, for unit conversions.
    double conversion = 1.0;
    /// From `%tc`: 1 upper-cases the result, 2 lower-cases it.
    int textCase = 0;
    /// From `%ps[prefix][suffix]`.
    std::string prefix;
    std::string suffix;
    /// True when the format asked for a thousands separator (`%lu` variants
    /// that use one). Kept separate because it applies only to numbers.
    bool separators = false;

    /// Parse the `\f "..."` clause out of a field code. A code with no clause
    /// yields the defaults, which format a number plainly.
    [[nodiscard]] static FieldFormat parse(const std::string& fieldCode);

    /// Parse a bare spec — `%pr2%lu2` — with no `\f "..."` clause around it.
    ///
    /// That is how a field's *value block* records the format actually applied,
    /// which is not always what the field code says.
    [[nodiscard]] static FieldFormat parseSpec(const std::string& spec);

    /// @param fallbackPrecision Used when the format states none.
    [[nodiscard]] std::string applyToNumber(double value, int fallbackPrecision = 4) const;
    [[nodiscard]] std::string applyToText(const std::string& value) const;
};

/// Everything a field can ask about the drawing that is not in the drawing.
///
/// Passed in rather than read from the environment so that evaluation is a pure
/// function of its inputs: the same drawing and the same context always produce
/// the same text, which is what makes it testable and what stops a plot from
/// differing from the preview it was made from.
struct FieldContext {
    /// The drawing's file name, for `Filename` and its variants.
    std::string fileName;
    /// Full path, for the `%fn` variants that include it.
    std::string filePath;
    /// The layout being evaluated for, for `PageSetupName` and friends.
    std::string layoutName;
    /// Seconds since the Unix epoch, for date fields. Zero means "unknown",
    /// which formats as the cached value rather than as 1970.
    long long currentTime = 0;
};

/// The result of evaluating one field.
struct FieldResult {
    /// The text to display.
    std::string text;
    /// True when this application computed the value, false when it fell back
    /// to the value cached in the file.
    bool evaluated = false;
    /// Set when evaluation failed and why, for the command line to report.
    std::string problem;

    /// The value before formatting, when the field produced a number.
    ///
    /// An expression over other fields has to use this and not the text. A
    /// child that reports square feet as `3339.41` has already divided by 144
    /// and rounded; summing the *text* of two such children and dividing again
    /// gives a number that is wrong by a factor of 144 and looks plausible.
    std::optional<double> number;
};

/// Computes the text a field displays.
///
/// Evaluation is a read: it never edits the database, so it is safe to call
/// while drawing. Applying a result to the entity that carries the field is a
/// separate, undoable step.
class FieldEvaluator {
public:
    FieldEvaluator(const Database& db, FieldContext context,
                   const PropertyRegistry& registry = PropertyRegistry::standard());

    [[nodiscard]] FieldResult evaluate(const FieldObject& field) const;

    /// Evaluate a field that belongs to a cell of `table`.
    ///
    /// A schedule's total is a formula over its own grid -- `SUM(B2:B11)` --
    /// and a field cannot find the table it sits in: the reference runs the
    /// other way, from the cell to the field. So it is told, not searched for,
    /// and a formula evaluated without a table says so rather than guessing.
    [[nodiscard]] FieldResult evaluate(const FieldObject& field, const TableEntity& table) const;

    /// Evaluate the field a given object carries, if it carries one.
    [[nodiscard]] std::optional<FieldResult> evaluateFor(const DbObject& object) const;

    /// Map other CAD software property name — `Area`, `TextString`, `Layer` — onto a
    /// property id in our registry.
    ///
    /// The names are ObjectARX's, and mostly do not match ours: other CAD software's
    /// `TextString` is our `text.contents`. An explicit table for those, then a
    /// match on the property's display name, which catches `Area`, `Length`,
    /// `Radius` and anything a plugin contributes without a table entry.
    [[nodiscard]] const PropertyDefinition* propertyFor(const DbObject& object,
                                                        const std::string& foreignName) const;

private:
    [[nodiscard]] FieldResult evaluateText(const FieldObject& field) const;
    [[nodiscard]] FieldResult evaluateObjectProperty(const FieldObject& field) const;
    [[nodiscard]] FieldResult evaluateExpression(const FieldObject& field) const;

    /// Replace `SUM(B2:B11)` and friends with the number they stand for.
    ///
    /// Cells are summed by their *value*, not by what they display. A cell
    /// showing `423.08` square feet holds an area in square inches and a format
    /// that scales it for display; adding the displayed numbers and dividing by
    /// 144 again, which is what the formula around the sum does, would be wrong
    /// by exactly that factor and would still look like an area.
    [[nodiscard]] std::string substituteCellFunctions(const std::string& text, bool& ok,
                                                      std::string& problem) const;

    /// One cell's numeric value, following its field to the number underneath.
    [[nodiscard]] std::optional<double> cellNumber(std::size_t row, std::size_t column) const;
    [[nodiscard]] FieldResult evaluateVariable(const FieldObject& field) const;

    /// Substitute `%<\_FldIdx N>%` holes with the children's results.
    ///
    /// @param raw True to substitute each child's unformatted number where it
    ///   has one, which is what an expression must be built from.
    /// @param problem Receives the first child's complaint, so a template
    ///   reports *why* it is stale rather than only that it is.
    [[nodiscard]] std::string expandChildren(const FieldObject& field, const std::string& text,
                                             bool raw, bool& ok, std::string& problem) const;

    /// Decimal places to use when the format does not say — the drawing's own
    /// `LUPREC`, which is what other CAD software falls back to.
    [[nodiscard]] int defaultPrecision() const;

    const Database* db_;
    FieldContext context_;
    /// The table a cell formula is being evaluated inside, when there is one.
    mutable const TableEntity* table_ = nullptr;
    const PropertyRegistry* registry_;
    /// Guards against a field that names itself, directly or round a cycle.
    /// A malformed file should not hang the application.
    mutable int depth_ = 0;
};

/// Evaluate a simple arithmetic expression: `+ - * / ( )` over decimals.
///
/// Exposed because `AcExpr` is not the only thing that wants it — a future
/// calculator and the numeric entry on the command line both do — and because
/// it is worth testing on its own.
[[nodiscard]] std::optional<double> evaluateArithmetic(const std::string& expression);

/// Work out what every table cell driven by a field currently says.
///
/// A schedule's cells hold a field handle and no text, and — unlike an MTEXT,
/// whose evaluated contents are in the file — the value has to be composed. The
/// cell's own field usually caches nothing: the room label's field is `2nd
/// %<\_FldIdx 0>%` over a child that reads a polyline's name, and only the
/// child has a cached value. Reading the cache alone gives an empty grid.
///
/// Run once after a drawing is read, and again by UPDATEFIELD.
///
/// @returns how many cells now say something.
std::size_t refreshTableCells(Database& db, const FieldContext& context = {});

}  // namespace cad
