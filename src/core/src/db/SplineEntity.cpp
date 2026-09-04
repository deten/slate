#include "cad/db/SplineEntity.hpp"

#include <stdexcept>
#include <string>

namespace cad {

std::vector<CurvePtr> SplineEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    // An invalid record contributes nothing rather than a curve that evaluates
    // to the origin: a stray line to 0,0 across a drawing is worse than a
    // missing one, because it looks like geometry somebody drew.
    if (curve.isValid()) {
        curves.push_back(std::make_unique<NurbsCurve>(curve));
    }
    return curves;
}

void SplineEntity::transformBy(const Mat4& m) {
    for (Vec3& point : curve.controlPoints) {
        point = m.transformPoint(point);
    }
    for (Vec3& point : fitPoints) {
        point = m.transformPoint(point);
    }
    // Tangents are directions, so they rotate and scale but must not be
    // translated -- moving a spline across the drawing would otherwise swing
    // its end tangents to point back at the origin.
    if (startTangent) {
        startTangent = m.transformVector(*startTangent);
    }
    if (endTangent) {
        endTangent = m.transformVector(*endTangent);
    }
}

std::vector<GripPoint> SplineEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    grips.reserve(curve.controlPoints.size());
    for (std::size_t i = 0; i < curve.controlPoints.size(); ++i) {
        grips.push_back({curve.controlPoints[i], GripKind::Vertex, i});
    }
    return grips;
}

void SplineEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index >= curve.controlPoints.size()) {
        return;
    }
    curve.controlPoints[grip.index] = to;
    // The fit points no longer describe this curve. Kept anyway, because
    // discarding them would lose data the file carried, but they are stale the
    // moment a control point moves and nothing here pretends otherwise.
}

DbObjectPtr SplineEntity::clone() const {
    auto copy = std::make_unique<SplineEntity>();
    copyEntityInto(*copy);
    copy->curve = curve;
    copy->fitPoints = fitPoints;
    copy->startTangent = startTangent;
    copy->endTangent = endTangent;
    copy->flags = flags;
    copy->knotTolerance = knotTolerance;
    copy->controlPointTolerance = controlPointTolerance;
    copy->fitTolerance = fitTolerance;
    return copy;
}

void SplineEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const SplineEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a spline");
    }
    restoreEntityFrom(*source);
    curve = source->curve;
    fitPoints = source->fitPoints;
    startTangent = source->startTangent;
    endTangent = source->endTangent;
    flags = source->flags;
    knotTolerance = source->knotTolerance;
    controlPointTolerance = source->controlPointTolerance;
    fitTolerance = source->fitTolerance;
}

}  // namespace cad
