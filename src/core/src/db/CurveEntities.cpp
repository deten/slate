#include "cad/db/CurveEntities.hpp"

#include <stdexcept>

namespace cad {
namespace {

/// Every entity's `restoreFrom` needs the same downcast-or-throw. Centralizing
/// it keeps the message consistent and the call sites to one line.
template <typename T>
[[nodiscard]] const T& expectSnapshot(const DbObject& snapshot, const char* what) {
    const auto* source = dynamic_cast<const T*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument(std::string{"Snapshot type mismatch restoring "} + what);
    }
    return *source;
}

}  // namespace

// ------------------------------------------------------------------ LineEntity

std::vector<CurvePtr> LineEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<LineSegment>(start, end));
    return curves;
}

std::vector<GripPoint> LineEntity::gripPoints() const {
    return {{start, GripKind::Endpoint, 0},
            {start.midpoint(end), GripKind::Midpoint, 1},
            {end, GripKind::Endpoint, 2}};
}

void LineEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index == 0) {
        start = to;
    } else if (grip.index == 2) {
        end = to;
    } else {
        // Dragging the midpoint translates the whole line.
        const Vec3 delta = to - start.midpoint(end);
        start += delta;
        end += delta;
    }
}

DbObjectPtr LineEntity::clone() const {
    auto copy = std::make_unique<LineEntity>();
    copyEntityInto(*copy);
    copy->start = start;
    copy->end = end;
    copy->thickness = thickness;
    return copy;
}

void LineEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<LineEntity>(snapshot, "a line");
    restoreEntityFrom(source);
    start = source.start;
    end = source.end;
    thickness = source.thickness;
}

// ---------------------------------------------------------------- CircleEntity

std::vector<CurvePtr> CircleEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Arc>(geometry()));
    return curves;
}

void CircleEntity::transformBy(const Mat4& m) {
    const CurvePtr transformed = geometry().transformed(m);
    if (const auto* arc = dynamic_cast<const Arc*>(transformed.get())) {
        center = arc->center;
        radius = arc->radius;
        normal = arc->normal;
        return;
    }
    // A non-uniform scale has made this an ellipse, which this entity type
    // cannot represent. It keeps a circular approximation; callers that need
    // exactness check `requiresEllipseAfter` first and substitute an
    // EllipseEntity. The database-level transform helper does exactly that.
    center = m.transformPoint(center);
    radius *= m.scaleFactor2();
}

std::vector<GripPoint> CircleEntity::gripPoints() const {
    std::vector<GripPoint> grips{{center, GripKind::Center, 0}};
    const Arc g = geometry();
    for (int i = 0; i < 4; ++i) {
        grips.push_back({g.pointAtAngle(static_cast<double>(i) * kPi / 2.0), GripKind::Quadrant,
                         static_cast<std::size_t>(i) + 1});
    }
    return grips;
}

void CircleEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.kind == GripKind::Center) {
        center = to;
    } else {
        radius = center.distanceTo(to);
    }
}

DbObjectPtr CircleEntity::clone() const {
    auto copy = std::make_unique<CircleEntity>();
    copyEntityInto(*copy);
    copy->center = center;
    copy->radius = radius;
    copy->thickness = thickness;
    return copy;
}

void CircleEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<CircleEntity>(snapshot, "a circle");
    restoreEntityFrom(source);
    center = source.center;
    radius = source.radius;
    thickness = source.thickness;
}

// ------------------------------------------------------------------- ArcEntity

ArcEntity ArcEntity::fromArc(const Arc& arc) {
    ArcEntity entity;
    entity.center = arc.center;
    entity.radius = arc.radius;
    entity.startAngle = arc.startAngle;
    entity.endAngle = arc.endAngle;
    entity.normal = arc.normal;
    return entity;
}

std::vector<CurvePtr> ArcEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Arc>(geometry()));
    return curves;
}

void ArcEntity::transformBy(const Mat4& m) {
    const CurvePtr transformed = geometry().transformed(m);
    if (const auto* arc = dynamic_cast<const Arc*>(transformed.get())) {
        center = arc->center;
        radius = arc->radius;
        startAngle = arc->startAngle;
        endAngle = arc->endAngle;
        normal = arc->normal;
    }
    // As with CircleEntity, a non-conformal transform needs an EllipseEntity;
    // the database-level helper performs the substitution.
}

std::vector<GripPoint> ArcEntity::gripPoints() const {
    const Arc g = geometry();
    return {{center, GripKind::Center, 0},
            {g.startPoint(), GripKind::Endpoint, 1},
            {g.midPoint(), GripKind::Midpoint, 2},
            {g.endPoint(), GripKind::Endpoint, 3}};
}

void ArcEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    const Arc g = geometry();
    switch (grip.index) {
        case 0:
            center = to;
            break;
        case 1:
            startAngle = g.angleAtPoint(to);
            radius = center.distanceTo(to);
            break;
        case 3:
            endAngle = g.angleAtPoint(to);
            radius = center.distanceTo(to);
            break;
        default: {
            // Dragging the midpoint reshapes the arc through the three points.
            const auto rebuilt = Arc::fromThreePoints(g.startPoint(), to, g.endPoint());
            if (rebuilt) {
                center = rebuilt->center;
                radius = rebuilt->radius;
                startAngle = rebuilt->startAngle;
                endAngle = rebuilt->endAngle;
            }
            break;
        }
    }
}

void ArcEntity::moveGrips(std::span<const GripMove> moves) {
    // One grip is not the case this exists for, and rebuilding through three
    // points would quietly change what a single drag does: an endpoint alone
    // keeps the centre and sets the radius from where it was dragged, which is
    // how an arc is resized about a fixed hinge. Only a set of grips that have
    // to agree with each other needs solving.
    if (moves.size() < 2) {
        Entity::moveGrips(moves);
        return;
    }

    const Arc g = geometry();
    Vec3 start = g.startPoint();
    Vec3 middle = g.midPoint();
    Vec3 finish = g.endPoint();
    bool movedStart = false;
    bool movedMiddle = false;
    bool movedFinish = false;
    std::optional<Vec3> movedCentre;

    for (const GripMove& move : moves) {
        switch (move.grip.index) {
            case 0: movedCentre = move.to; break;
            case 1: start = move.to; movedStart = true; break;
            case 2: middle = move.to; movedMiddle = true; break;
            case 3: finish = move.to; movedFinish = true; break;
            default: break;
        }
    }

    if (movedCentre) {
        // The centre carries the arc. Points not named by a move ride along
        // with it, so dragging an arc by its centre translates it and leaves
        // the radius and the sweep alone.
        const Vec3 delta = *movedCentre - center;
        if (!movedStart) {
            start += delta;
        }
        if (!movedMiddle) {
            middle += delta;
        }
        if (!movedFinish) {
            finish += delta;
        }
    }

    const auto rebuilt = Arc::fromThreePoints(start, middle, finish);
    if (!rebuilt) {
        // Three points in a line describe no arc. Left alone rather than
        // collapsed: the drag is simply refused, which is visible and
        // recoverable, where a degenerate arc is neither.
        return;
    }
    center = rebuilt->center;
    radius = rebuilt->radius;
    startAngle = rebuilt->startAngle;
    endAngle = rebuilt->endAngle;
}

DbObjectPtr ArcEntity::clone() const {
    auto copy = std::make_unique<ArcEntity>();
    copyEntityInto(*copy);
    copy->center = center;
    copy->radius = radius;
    copy->startAngle = startAngle;
    copy->endAngle = endAngle;
    copy->thickness = thickness;
    return copy;
}

void ArcEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<ArcEntity>(snapshot, "an arc");
    restoreEntityFrom(source);
    center = source.center;
    radius = source.radius;
    startAngle = source.startAngle;
    endAngle = source.endAngle;
    thickness = source.thickness;
}

// --------------------------------------------------------------- EllipseEntity

EllipseEntity EllipseEntity::fromEllipseArc(const EllipseArc& e) {
    EllipseEntity entity;
    entity.center = e.center;
    entity.majorAxis = e.majorAxis;
    entity.ratio = e.ratio;
    entity.startParam = e.startParam;
    entity.endParam = e.isClosed() ? e.startParam + kTau : e.endParam;
    entity.normal = e.normal;
    return entity;
}

std::vector<CurvePtr> EllipseEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<EllipseArc>(geometry()));
    return curves;
}

void EllipseEntity::transformBy(const Mat4& m) {
    const CurvePtr transformed = geometry().transformed(m);
    const auto* e = dynamic_cast<const EllipseArc*>(transformed.get());
    if (e == nullptr) {
        return;
    }
    center = e->center;
    majorAxis = e->majorAxis;
    ratio = e->ratio;
    startParam = e->startParam;
    endParam = e->isClosed() ? e->startParam + kTau : e->endParam;
    normal = e->normal;
}

std::vector<GripPoint> EllipseEntity::gripPoints() const {
    const Vec3 minor = geometry().minorAxis();
    return {{center, GripKind::Center, 0},
            {center + majorAxis, GripKind::Quadrant, 1},
            {center - majorAxis, GripKind::Quadrant, 2},
            {center + minor, GripKind::Quadrant, 3},
            {center - minor, GripKind::Quadrant, 4}};
}

void EllipseEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    switch (grip.index) {
        case 0:
            center = to;
            break;
        case 1:
        case 2: {
            const Vec3 newMajor = grip.index == 1 ? to - center : center - to;
            const double minorLength = geometry().semiMinor();
            const double majorLength = newMajor.length();
            if (majorLength > 0.0) {
                majorAxis = newMajor;
                ratio = std::min(1.0, std::max(1e-6, minorLength / majorLength));
            }
            break;
        }
        default: {
            const double minorLength = center.distanceTo(to);
            const double majorLength = majorAxis.length();
            if (majorLength > 0.0) {
                ratio = std::min(1.0, std::max(1e-6, minorLength / majorLength));
            }
            break;
        }
    }
}

DbObjectPtr EllipseEntity::clone() const {
    auto copy = std::make_unique<EllipseEntity>();
    copyEntityInto(*copy);
    copy->center = center;
    copy->majorAxis = majorAxis;
    copy->ratio = ratio;
    copy->startParam = startParam;
    copy->endParam = endParam;
    return copy;
}

void EllipseEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<EllipseEntity>(snapshot, "an ellipse");
    restoreEntityFrom(source);
    center = source.center;
    majorAxis = source.majorAxis;
    ratio = source.ratio;
    startParam = source.startParam;
    endParam = source.endParam;
}

// -------------------------------------------------------------- PolylineEntity

PolylineEntity PolylineEntity::fromPolyline(const Polyline& poly) {
    PolylineEntity entity;
    entity.vertices = poly.vertices;
    entity.closed = poly.isClosed();
    entity.normal = poly.normal;
    return entity;
}

PolylineEntity PolylineEntity::fromPoints(std::span<const Vec3> points, bool closedFlag) {
    PolylineEntity entity;
    entity.vertices.reserve(points.size());
    for (const Vec3& p : points) {
        entity.vertices.emplace_back(p);
    }
    entity.closed = closedFlag;
    return entity;
}

std::vector<CurvePtr> PolylineEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    curves.push_back(std::make_unique<Polyline>(geometry()));
    return curves;
}

void PolylineEntity::transformBy(const Mat4& m) {
    const CurvePtr transformed = geometry().transformed(m);
    const auto* poly = dynamic_cast<const Polyline*>(transformed.get());
    if (poly == nullptr) {
        return;
    }
    vertices = poly->vertices;
    normal = poly->normal;
    constantWidth *= m.scaleFactor2();
}

std::size_t PolylineEntity::segmentCount() const {
    if (vertices.size() < 2) {
        return 0;
    }
    // A closed polyline has one more segment than an open one: the last vertex
    // joins back to the first.
    return closed ? vertices.size() : vertices.size() - 1;
}

Vec3 PolylineEntity::segmentMidpoint(std::size_t segment) const {
    if (segment >= segmentCount()) {
        return {};
    }
    const Vec3& from = vertices[segment].position;
    const Vec3& to = vertices[(segment + 1) % vertices.size()].position;
    const Vec3 middle = from.midpoint(to);
    const double bulge = vertices[segment].bulge;
    if (bulge == 0.0) {
        return middle;
    }
    // On an arc segment the grip belongs on the arc, not on the chord it
    // subtends. The bulge *is* the sagitta over the half-chord -- both are
    // tan(included angle / 4) -- so the offset from the chord's middle is a
    // perpendicular scaled by half the bulge, with no trigonometry.
    //
    // To the *right* of the chord for a positive bulge: `Arc::fromBulge` puts
    // the centre on the left, so the arc swings the other way. Guessing the
    // other side puts the grip where there is nothing to grab.
    const Vec3 chord = to - from;
    return middle + Vec3{chord.y, -chord.x, 0.0} * (bulge * 0.5);
}

std::vector<GripPoint> PolylineEntity::gripPoints() const {
    std::vector<GripPoint> grips;
    const std::size_t count = vertices.size();
    const std::size_t segments = segmentCount();
    grips.reserve(count + segments);
    for (std::size_t i = 0; i < count; ++i) {
        grips.push_back({vertices[i].position, GripKind::Vertex, i});
    }
    // A grip in the middle of every segment as well, which is how other CAD software puts
    // a new corner in a straight run, and how an arc segment gets reshaped,
    // without redrawing the polyline. Numbered after the vertices so one index
    // still says which grip is meant.
    for (std::size_t i = 0; i < segments; ++i) {
        grips.push_back({segmentMidpoint(i), GripKind::Midpoint, count + i});
    }
    return grips;
}

void PolylineEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index < vertices.size()) {
        vertices[grip.index].position = to;
        return;
    }

    const std::size_t segment = grip.index - vertices.size();
    if (segment >= segmentCount()) {
        return;
    }
    const std::size_t next = (segment + 1) % vertices.size();
    const Vec3& from = vertices[segment].position;
    const Vec3 chord = vertices[next].position - from;
    const double span = chord.length();
    if (span < 1e-12) {
        return;
    }

    if (vertices[segment].bulge != 0.0) {
        // Already an arc: keep it one and bend it through the point rather than
        // breaking it into two. Splitting an arc where the user meant to adjust
        // its curvature would lose the shape they were adjusting.
        //
        // Only the component across the chord counts. Dragging along the chord
        // does not describe a different arc through the same two ends, and
        // reading it as though it did makes the grip jitter.
        const Vec3 offset = to - from.midpoint(vertices[next].position);
        // Measured along the same perpendicular `segmentMidpoint` offsets by,
        // so dragging the grip to a point puts the arc through it.
        const double sagitta = (chord.y * offset.x - chord.x * offset.y) / span;
        vertices[segment].bulge = sagitta / (span * 0.5);
        return;
    }

    // A straight segment gains a corner. Width comes from the segment being
    // divided, so a tapered run stays tapered rather than resetting at the new
    // vertex.
    PolylineVertex inserted = vertices[segment];
    inserted.position = to;
    inserted.bulge = 0.0;
    vertices.insert(vertices.begin() + static_cast<std::ptrdiff_t>(segment) + 1, inserted);
}

DbObjectPtr PolylineEntity::clone() const {
    auto copy = std::make_unique<PolylineEntity>();
    copyEntityInto(*copy);
    copy->vertices = vertices;
    copy->closed = closed;
    copy->constantWidth = constantWidth;
    copy->thickness = thickness;
    copy->isLegacyFormat = isLegacyFormat;
    return copy;
}

void PolylineEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<PolylineEntity>(snapshot, "a polyline");
    restoreEntityFrom(source);
    vertices = source.vertices;
    closed = source.closed;
    constantWidth = source.constantWidth;
    thickness = source.thickness;
    isLegacyFormat = source.isLegacyFormat;
}

// ----------------------------------------------------------------- PointEntity

std::vector<GripPoint> PointEntity::gripPoints() const {
    return {{position, GripKind::Endpoint, 0}};
}

void PointEntity::moveGrip(const GripPoint& /*grip*/, const Vec3& to) {
    position = to;
}

DbObjectPtr PointEntity::clone() const {
    auto copy = std::make_unique<PointEntity>();
    copyEntityInto(*copy);
    copy->position = position;
    copy->angle = angle;
    return copy;
}

void PointEntity::restoreFrom(const DbObject& snapshot) {
    const auto& source = expectSnapshot<PointEntity>(snapshot, "a point");
    restoreEntityFrom(source);
    position = source.position;
    angle = source.angle;
}

}  // namespace cad
