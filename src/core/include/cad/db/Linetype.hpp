#pragma once

#include <optional>
#include <span>
#include <vector>

#include "cad/db/SymbolTable.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

inline constexpr const char* kLinetypeContinuous = "Continuous";
inline constexpr const char* kLinetypeByLayer = "ByLayer";
inline constexpr const char* kLinetypeByBlock = "ByBlock";

/// One element of a linetype pattern.
///
/// A plain dash pattern uses only `length`. Complex linetypes — the ones that
/// draw text or shapes along the line, such as a gas line marked "GAS" or a
/// fence line with ticks — additionally carry an embedded element. Those are
/// modelled here so importing a professional drawing does not silently flatten
/// them to plain dashes.
struct LinetypeElement {
    /// Dash length in drawing units. Positive draws a dash, negative leaves a
    /// gap, and zero produces a dot.
    double length = 0.0;

    /// Embedded text, for complex linetypes.
    std::string text;
    /// Handle of the text style used to draw `text`.
    Handle styleHandle;
    /// Shape index within an SHX shape file, when the element is a shape.
    std::optional<int> shapeNumber;
    /// Offset of the embedded element from the line.
    Vec3 offset;
    /// Rotation of the embedded element, in radians.
    double rotation = 0.0;
    /// True when the rotation is absolute rather than relative to the line.
    bool rotationIsAbsolute = false;
    double scale = 1.0;
};

/// A linetype definition: the repeating dash/gap pattern applied along a curve.
class Linetype final : public SymbolTableRecord {
public:
    /// Human-readable description, e.g. `Dashed __ __ __ __`.
    std::string description;

    /// The repeating pattern. An empty pattern means a solid line.
    std::vector<LinetypeElement> elements;

    [[nodiscard]] std::string objectType() const override { return "Linetype"; }

    [[nodiscard]] bool isReserved() const override;

    /// True when this linetype draws an unbroken line.
    [[nodiscard]] bool isContinuous() const noexcept { return elements.empty(); }

    /// Total length of one repetition of the pattern.
    [[nodiscard]] double patternLength() const noexcept;

    /// True when any element embeds text or a shape.
    [[nodiscard]] bool isComplex() const noexcept;

    /// The dash pattern as alternating on/off lengths, suitable for a
    /// renderer's dash array.
    ///
    /// Complex elements contribute their length only; their embedded glyphs are
    /// drawn separately by the renderer.
    ///
    /// @param scale combined linetype scale (global * entity * block).
    /// @returns alternating draw/skip lengths, or empty for a solid line.
    [[nodiscard]] std::vector<double> toDashArray(double scale = 1.0) const;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

/// A standard linetype definition, for seeding a new drawing.
struct StandardLinetype {
    const char* name;
    const char* description;
    std::span<const double> pattern;
};

/// Definitions for the linetypes other CAD software ships in `acad.lin`, so a new drawing
/// is usable without an external resource file. Lengths are in drawing units at
/// a scale of 1.
[[nodiscard]] std::span<const StandardLinetype> standardLinetypes() noexcept;

}  // namespace cad
