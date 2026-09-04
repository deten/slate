#pragma once

#include <array>

#include "cad/db/Entity.hpp"

namespace cad {

/// SOLID — a filled triangle or quadrilateral.
///
/// One of the oldest entities in DXF and still everywhere in architectural
/// drawings: filled poché, arrowheads, solid markers, the black triangle on a
/// section tag.
///
/// The corner order is the trap. DXF stores four corners, but the shape is
/// traced first, second, *fourth*, third -- the last two are swapped relative
/// to the ring you would draw by hand. Reading them in the stored order gives a
/// bow tie. When the third and fourth corners are equal the shape is a
/// triangle, which is how DXF says "only three".
class SolidEntity final : public Entity {
public:
    /// The four corners in DXF's own order, which is *not* ring order. Use
    /// `ring()` for anything geometric.
    std::array<Vec3, 4> corners{};

    /// Extrusion depth. Preserved and written back; the 2D view ignores it.
    double thickness = 0.0;

    [[nodiscard]] std::string objectType() const override { return "Solid"; }

    /// The corners in the order they are traced, three or four of them.
    [[nodiscard]] std::vector<Vec3> ring() const;

    [[nodiscard]] Box3 bounds() const override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Zero anywhere inside, like any filled region: a drafter clicking the
    /// middle of a solid triangle is pointing at it, not missing it.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
