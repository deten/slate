#pragma once

#include <array>
#include <optional>
#include <string>

#include "cad/db/DimStyle.hpp"
#include "cad/db/Entity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {

/// What is being measured. Matches the low three bits of DXF group 70.
enum class DimensionType : std::uint8_t {
    /// Distance along a fixed direction, which need not be the direction
    /// between the two points.
    Rotated = 0,
    Aligned = 1,
    /// Angle between two lines, given by four points.
    Angular2Line = 2,
    Diameter = 3,
    Radius = 4,
    /// Angle at a vertex, given by three points.
    Angular3Point = 5,
    /// Distance from a datum along one axis.
    Ordinate = 6,
};

/// One drawn piece of a dimension.
///
/// A dimension is not a single curve, and not a block of entities either — it
/// is geometry *derived* from a few definition points and a style. Generating
/// it on demand is what makes editing work: drag the dimension line and the
/// extension lines follow, change the style and every dimension using it
/// restyles. Storing the drawn result instead, as DWG's anonymous blocks do,
/// makes all of that a rebuild-from-scratch problem.
struct DimensionGeometry {
    /// Extension lines, the dimension line, and leader segments.
    std::vector<LineSegment> lines;
    /// Dimension arcs, for angular dimensions.
    std::vector<Arc> arcs;
    /// Filled arrowhead triangles, in world coordinates. Filled rather than
    /// stroked because that is what a closed filled arrow is, and stroking a
    /// triangle at small sizes looks like a smudge.
    std::vector<std::array<Vec3, 3>> arrowheads;

    /// Where the measurement text goes, and what it says.
    Vec3 textPosition;
    /// Rotation of the text about the entity's normal, in radians.
    double textRotation = 0.0;
    double textHeight = 2.5;
    std::string text;
    /// True when the text should be centred on `textPosition` rather than
    /// hanging from it as a left baseline.
    bool textCentered = true;
};

/// A dimension.
///
/// One class covering all seven kinds, discriminated by `type`, because that is
/// exactly how DWG stores them: a single DIMENSION entity with a type code and
/// a shared set of definition points whose meaning depends on it. Splitting
/// into seven classes would mean seven readers, seven writers, and a decision
/// to make about a malformed file whose type code and point set disagree.
///
/// The definition points are named for their DXF group codes rather than for
/// their role, since their role is what the type code decides. The accessors
/// below give them their role-specific names for each type.
class DimensionEntity final : public Entity {
public:
    DimensionType type = DimensionType::Rotated;

    /// Group 10. The dimension line location for linear dimensions, the arc
    /// location for angular ones, the centre for a radius, a chord point for a
    /// diameter.
    Vec3 definitionPoint;

    /// Group 11. Middle of the measurement text.
    Vec3 textMidPoint;

    /// Group 12. Where a baseline or continued dimension made from this one
    /// starts. Carried for the file; nothing here uses it yet.
    Vec3 insertionPoint;

    /// Groups 13, 14. The two measured points, or the first measured line.
    Vec3 point13;
    Vec3 point14;

    /// Groups 15, 16. The second measured line, the arc centre, or the point
    /// on the circumference.
    Vec3 point15;
    Vec3 point16;

    /// Group 50. Direction the measurement is taken along, for a rotated
    /// linear dimension. Ignored by every other type.
    double rotation = 0.0;

    /// Group 52. Extension lines leaned away from perpendicular.
    double obliqueAngle = 0.0;

    /// Group 53. Text rotated relative to its default orientation.
    double textRotationOverride = 0.0;
    bool hasTextRotationOverride = false;

    /// Group 51. Horizontal direction of the drawing's UCS at creation.
    double horizontalDirection = 0.0;

    /// Group 40. Leader length, for radial dimensions.
    double leaderLength = 0.0;

    /// Group 1. Empty means "write the measured value". A single space means
    /// "write nothing". Anything else overrides, with `<>` standing in for the
    /// measurement. All three are distinct and all three occur in real files.
    std::string textOverride;

    /// Group 3, resolved. Null falls back to the drawing's current style.
    Handle styleHandle;

    /// Group 2, resolved. The anonymous block DWG stores the drawn result in.
    ///
    /// Kept but not drawn. Regenerating is what makes the dimension editable,
    /// but a file's own block is what another application will expect to find
    /// on save, so the reference is preserved rather than discarded.
    Handle blockHandle;

    /// Group 42. The measurement other CAD software recorded, kept for round-trip
    /// fidelity and as a cross-check. `measure()` recomputes it from the
    /// definition points instead, because those are what editing changes.
    double storedMeasurement = 0.0;

    /// Group 71/72/41. Text attachment and line spacing, as MTEXT.
    MTextAttachment attachmentPoint = MTextAttachment::MiddleCenter;
    int lineSpacingStyle = 1;
    double lineSpacingFactor = 1.0;

    /// Bit 128 of group 70: the user dragged the text away from its default
    /// position, so `textMidPoint` is authoritative rather than derived.
    bool textPositionIsUserDefined = false;

    /// Bit 64 of group 70, meaningful only for an ordinate dimension: the
    /// measurement is along X rather than along Y.
    bool isXDatum = false;

    [[nodiscard]] std::string objectType() const override { return "Dimension"; }

    // ------------------------------------------------------- role accessors
    //
    // The DXF group codes are the storage; these are the meanings. Reading
    // `firstExtensionPoint()` at a call site is the difference between code
    // that can be checked against the geometry and code that can only be
    // checked against the DXF specification.

    /// Rotated and aligned: the two points being measured.
    [[nodiscard]] const Vec3& firstExtensionPoint() const noexcept { return point13; }
    [[nodiscard]] const Vec3& secondExtensionPoint() const noexcept { return point14; }

    /// Radius and diameter: the centre and a point on the circumference.
    [[nodiscard]] Vec3 arcCenter() const;
    [[nodiscard]] Vec3 chordPoint() const;

    /// Angular: the vertex the angle is measured at.
    [[nodiscard]] Vec3 angleVertex() const;

    /// The measured value, recomputed from the definition points.
    ///
    /// A length for linear and radial dimensions, an angle in radians for
    /// angular ones. Recomputed rather than read from group 42, so moving a
    /// dimension's points changes what it says.
    [[nodiscard]] double measure() const;

    /// Build the drawn geometry for this dimension under `style`.
    [[nodiscard]] DimensionGeometry generate(const DimStyle& style) const;

    /// The default text position, when the user has not dragged it.
    [[nodiscard]] Vec3 defaultTextPosition(const DimStyle& style) const;

    /// The measurement text, with the override rules applied.
    [[nodiscard]] std::string measurementText(const DimStyle& style) const;

    [[nodiscard]] Box3 bounds() const override;

    /// The dimension's lines and arcs, so it snaps and picks like anything
    /// else. Generated with a default style, since bounds and picking must not
    /// depend on a database lookup — a dimension in a drawing whose style
    /// table is damaged still has to be selectable.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

private:
    [[nodiscard]] DimensionGeometry generateLinear(const DimStyle& style) const;
    [[nodiscard]] DimensionGeometry generateRadial(const DimStyle& style) const;
    [[nodiscard]] DimensionGeometry generateAngular(const DimStyle& style) const;
    [[nodiscard]] DimensionGeometry generateOrdinate(const DimStyle& style) const;
};

/// The style a dimension should be drawn with when its own is missing.
///
/// A drawing whose DIMSTYLE table is incomplete must still display; returning a
/// default is what makes `toCurves` and `bounds` work without a database.
[[nodiscard]] const DimStyle& fallbackDimStyle();

}  // namespace cad
