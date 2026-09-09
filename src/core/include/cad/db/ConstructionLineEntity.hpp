#pragma once

#include "cad/db/Entity.hpp"

namespace cad {

/// A construction line: infinite in both directions (XLINE) or from a point
/// in one direction (RAY).
///
/// Drafters lay these down to line things up and never plot them. They
/// have no extents, which is the one thing that makes them different from
/// a line: they must not count towards ZOOM EXTENTS, and what is drawn and
/// snapped to is a long stretch of them rather than the whole.
class ConstructionLineEntity final : public Entity {
public:
    /// A point the line passes through; for a ray, where it starts.
    Vec3 base;
    /// Which way it runs. Kept a unit vector.
    Vec3 direction = Vec3::unitX();
    /// True for an XLINE, false for a RAY.
    bool bothWays = true;

    ConstructionLineEntity() = default;
    ConstructionLineEntity(const Vec3& through, const Vec3& along, bool infinite)
        : base(through), direction(along.normalized()), bothWays(infinite) {}

    [[nodiscard]] std::string objectType() const override { return bothWays ? "Xline" : "Ray"; }

    /// True: no box holds it, so the extents of a drawing leave it out.
    [[nodiscard]] bool isUnbounded() const override { return true; }

    /// The stretch of the line that is drawn and snapped to, as a segment:
    /// long enough to cross any drawing many times over, from the base.
    [[nodiscard]] Vec3 drawnStart() const;
    [[nodiscard]] Vec3 drawnEnd() const;

    /// The box of the drawn stretch. Never used for extents, see above.
    [[nodiscard]] Box3 bounds() const override;

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Exact against the infinite line or the ray, not the drawn stretch.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
