#pragma once

#include <vector>

#include "cad/db/Entity.hpp"
#include "cad/geom/NurbsCurve.hpp"

namespace cad {

/// SPLINE — a NURBS curve.
///
/// The geometry lives in `NurbsCurve`; this holds it along with the parts of
/// the DXF record that are not geometry but have to survive a round trip: the
/// fit points a curve may have been built from, the tangents at its ends, and
/// the tolerances the writer used.
///
/// Fit points are kept and never interpreted. other CAD software can rebuild a spline
/// from them, but the control points already describe the curve exactly, and
/// re-fitting would move it -- the drawing would come back subtly different
/// from the one that was opened.
class SplineEntity final : public Entity {
public:
    NurbsCurve curve;

    /// Points the spline was fitted through, if it was. Preserved verbatim.
    std::vector<Vec3> fitPoints;

    /// Start and end tangents, present only on a fitted spline.
    std::optional<Vec3> startTangent;
    std::optional<Vec3> endTangent;

    /// The flag word as read (1 closed, 2 periodic, 4 rational, 8 planar,
    /// 16 linear). Kept whole rather than unpacked: the bits that are not
    /// acted on still have to be written back.
    int flags = 8;

    double knotTolerance = 1e-7;
    double controlPointTolerance = 1e-7;
    double fitTolerance = 1e-10;

    [[nodiscard]] std::string objectType() const override { return "Spline"; }

    [[nodiscard]] Box3 bounds() const override { return curve.bounds(); }
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    /// One per control point. other CAD software offers fit point grips instead when a
    /// spline has fit points; ours are the control points either way, because
    /// moving a fit point means re-fitting, and re-fitting is exactly what this
    /// class declines to do.
    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
