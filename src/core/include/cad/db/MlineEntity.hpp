#pragma once

#include <string>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// One vertex of a multiline, with the offsets each element takes there.
struct MlineVertex {
    Vec3 position;
    /// Group 12. Along the segment leaving this vertex.
    Vec3 direction;
    /// Group 13. The bisector the joint is cut along, which is what keeps the
    /// elements parallel through a corner instead of overshooting it.
    Vec3 miterDirection;

    /// Groups 74/41, one list per element.
    ///
    /// Distances from the vertex along the miter direction. The first is where
    /// that element's line passes; the rest describe breaks in it. This is why
    /// a multiline can be drawn without its style: the offsets the style
    /// produced are recorded on the entity, at every vertex.
    std::vector<std::vector<double>> elementOffsets;

    /// Groups 75/42. The same for the fill between elements, kept for
    /// round-trip; nothing here fills a multiline yet.
    std::vector<std::vector<double>> fillOffsets;
};

/// A multiline — MLINE. Several parallel lines drawn as one object.
///
/// What a wall is, in a drawing produced by anybody using the tool other CAD software
/// provides for walls: two or more parallel lines that turn corners together
/// and stay joined. Until now one arrived as an invisible placeholder, so a
/// plan drawn with MLINE opened as an empty sheet that saved back correctly and
/// showed nothing.
///
/// **Drawn from the entity, not from the style.** MLINESTYLE lives in the
/// OBJECTS section and says how many elements there are and where they sit.
/// The entity records the resulting offset of every element at every vertex
/// anyway -- that is what groups 41 and 74 are -- so the geometry is complete
/// without resolving the style. What the style would add is per-element colour
/// and linetype, which is why every element here draws in the entity's own.
class MlineEntity final : public Entity {
public:
    /// Group 2, the style's name. Kept for round-trip and for a later reader
    /// that resolves MLINESTYLE.
    std::string styleName = "STANDARD";
    /// Group 340.
    Handle styleHandle;

    /// Group 40. The style's offsets are multiplied by this.
    double scale = 1.0;

    /// Group 70. 0 top, 1 zero, 2 bottom. Recorded rather than applied: the
    /// offsets on each vertex already have it baked in.
    int justification = 0;

    /// Group 71, bit 2.
    bool closed = false;
    /// Group 71, bits 4 and 8.
    bool suppressStartCaps = false;
    bool suppressEndCaps = false;

    /// Group 73. How many parallel lines this multiline draws.
    int elementCount = 2;

    /// Group 10. The insertion point, which is not necessarily a vertex.
    Vec3 startPoint;

    std::vector<MlineVertex> vertices;

    [[nodiscard]] std::string objectType() const override { return "Mline"; }

    /// One run of points per element, in element order.
    ///
    /// An element with no offset recorded at a vertex is skipped there rather
    /// than drawn through the vertex itself: a file that ran out of parameters
    /// mid-way describes a shorter element, not one that collapses onto the
    /// centre line.
    [[nodiscard]] std::vector<std::vector<Vec3>> elementLines() const;

    [[nodiscard]] Box3 bounds() const override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
