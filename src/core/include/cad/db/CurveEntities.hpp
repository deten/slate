#pragma once

#include "cad/db/Entity.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

/// A straight line between two points — the LINE entity.
class LineEntity final : public Entity {
public:
    Vec3 start;
    Vec3 end = Vec3::unitX();
    /// Thickness extrudes the line into a plane; preserved, rarely used in 2D.
    double thickness = 0.0;

    LineEntity() = default;
    LineEntity(const Vec3& from, const Vec3& to) : start(from), end(to) {}

    [[nodiscard]] std::string objectType() const override { return "Line"; }

    [[nodiscard]] LineSegment geometry() const { return LineSegment{start, end}; }

    [[nodiscard]] Box3 bounds() const override { return Box3::fromCorners(start, end); }

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override {
        start = m.transformPoint(start);
        end = m.transformPoint(end);
    }

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A full circle — the CIRCLE entity.
///
/// Kept distinct from ArcEntity because DWG does, and collapsing them would
/// lose the distinction on save. The shared behaviour lives in the Arc geometry
/// class rather than in a common entity base.
class CircleEntity final : public Entity {
public:
    Vec3 center;
    double radius = 1.0;
    double thickness = 0.0;

    CircleEntity() = default;
    CircleEntity(const Vec3& centerPoint, double radiusValue)
        : center(centerPoint), radius(radiusValue) {}

    [[nodiscard]] std::string objectType() const override { return "Circle"; }

    [[nodiscard]] Arc geometry() const { return Arc::circle(center, radius, normal); }

    [[nodiscard]] Box3 bounds() const override { return geometry().bounds(); }

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    /// True when `m` would turn this circle into a genuine ellipse, so the
    /// caller must substitute an EllipseEntity to stay exact.
    [[nodiscard]] bool requiresEllipseAfter(const Mat4& m) const { return !m.isConformal(); }

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A circular arc — the ARC entity. Angles are radians and always describe a
/// counter-clockwise sweep, matching DWG.
class ArcEntity final : public Entity {
public:
    Vec3 center;
    double radius = 1.0;
    double startAngle = 0.0;
    double endAngle = kPi;
    double thickness = 0.0;

    ArcEntity() = default;
    ArcEntity(const Vec3& centerPoint, double radiusValue, double start, double end)
        : center(centerPoint), radius(radiusValue), startAngle(start), endAngle(end) {}

    [[nodiscard]] static ArcEntity fromArc(const Arc& arc);

    [[nodiscard]] std::string objectType() const override { return "Arc"; }

    [[nodiscard]] Arc geometry() const {
        return Arc{center, radius, startAngle, endAngle, normal};
    }

    [[nodiscard]] Box3 bounds() const override { return geometry().bounds(); }

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] bool requiresEllipseAfter(const Mat4& m) const { return !m.isConformal(); }

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    /// Solved as one arc rather than one grip at a time.
    ///
    /// Every grip an arc has is a consequence of the same centre and radius, so
    /// moving them in sequence has each partly undo the last: two ends dragged
    /// together by an equal amount came out a unit apart with the centre
    /// wandered off. Given all the moves at once, the arc is rebuilt through
    /// where the three points are meant to end up.
    void moveGrips(std::span<const GripMove> moves) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// An elliptical arc — the ELLIPSE entity.
class EllipseEntity final : public Entity {
public:
    Vec3 center;
    /// Vector from centre to the major-axis endpoint.
    Vec3 majorAxis = Vec3::unitX();
    double ratio = 1.0;
    double startParam = 0.0;
    double endParam = kTau;

    [[nodiscard]] static EllipseEntity fromEllipseArc(const EllipseArc& e);

    [[nodiscard]] std::string objectType() const override { return "Ellipse"; }

    [[nodiscard]] EllipseArc geometry() const {
        return EllipseArc{center, majorAxis, ratio, startParam, endParam, normal};
    }

    [[nodiscard]] Box3 bounds() const override { return geometry().bounds(); }

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A polyline — the LWPOLYLINE and POLYLINE entities.
///
/// Both DXF forms map to this single entity. `isLegacyFormat` records which one
/// the file used so a round-trip writes back the same type; everything else
/// about them is equivalent for 2D drafting.
class PolylineEntity final : public Entity {
public:
    std::vector<PolylineVertex> vertices;
    bool closed = false;
    /// Group 70 bit 128: the linetype pattern runs continuously around the
    /// vertices instead of restarting at each one. Carried so a file keeps
    /// it; the renderer does not yet draw the difference.
    bool linetypeGeneration = false;
    /// Uniform width applied when no per-vertex width is set.
    double constantWidth = 0.0;
    double thickness = 0.0;
    /// True when the source was the heavyweight POLYLINE entity.
    bool isLegacyFormat = false;

    [[nodiscard]] static PolylineEntity fromPolyline(const Polyline& poly);
    [[nodiscard]] static PolylineEntity fromPoints(std::span<const Vec3> points,
                                                   bool closedFlag = false);

    [[nodiscard]] std::string objectType() const override { return "Polyline"; }

    [[nodiscard]] Polyline geometry() const { return Polyline{vertices, closed, normal}; }

    [[nodiscard]] Box3 bounds() const override { return geometry().bounds(); }

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    /// How many segments the polyline has: one fewer than its vertices, or one
    /// per vertex when it is closed.
    [[nodiscard]] std::size_t segmentCount() const;

    /// The middle of one segment -- on the arc, not the chord, where the
    /// segment is bulged.
    [[nodiscard]] Vec3 segmentMidpoint(std::size_t segment) const;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A single point marker — the POINT entity. Used as a node snap target and by
/// survey and setting-out workflows.
class PointEntity final : public Entity {
public:
    Vec3 position;
    /// Rotation of the point's display marker, in radians.
    double angle = 0.0;

    PointEntity() = default;
    explicit PointEntity(const Vec3& p) : position(p) {}

    [[nodiscard]] std::string objectType() const override { return "Point"; }

    [[nodiscard]] Box3 bounds() const override { return Box3::fromPoint(position); }

    /// A point has no curve geometry; it is picked by proximity to its position.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override {
        return position.distanceTo(p);
    }

    void transformBy(const Mat4& m) override { position = m.transformPoint(position); }

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
