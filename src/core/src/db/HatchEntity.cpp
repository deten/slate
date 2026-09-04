#include "cad/db/HatchEntity.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "cad/db/SymbolTable.hpp"
#include "cad/geom/Intersect.hpp"

namespace cad {
namespace {

constexpr double kDegenerate = 1e-12;

/// Rotate a 2D offset into world orientation.
[[nodiscard]] Vec3 rotated(const Vec3& v, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return Vec3{v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

/// Crossing number of a ray from `p` against a closed ring, in the XY plane.
///
/// The half-open rule on the vertical span is what makes a point that is level
/// with a vertex count once rather than twice or not at all — the classic
/// point-in-polygon bug, and one that shows up as single-pixel holes in a fill.
[[nodiscard]] bool pointInRing(const std::vector<Vec3>& ring, const Vec3& p) {
    bool inside = false;
    const std::size_t count = ring.size();
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        const Vec3& a = ring[i];
        const Vec3& b = ring[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/// Shortest distance from a point to a closed ring's edges.
[[nodiscard]] double distanceToRing(const std::vector<Vec3>& ring, const Vec3& p) {
    double best = std::numeric_limits<double>::infinity();
    const std::size_t count = ring.size();
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        best = std::min(best, LineSegment{ring[j], ring[i]}.distanceTo(p));
    }
    return best;
}

}  // namespace

// ------------------------------------------------------------------- edges

HatchEdge HatchEdge::line(const Vec3& from, const Vec3& to) {
    HatchEdge edge;
    edge.kind = Kind::Line;
    edge.start = from;
    edge.end = to;
    return edge;
}

HatchEdge HatchEdge::arc(const Vec3& centerPoint, double radiusValue, double start, double end,
                         bool ccw) {
    HatchEdge edge;
    edge.kind = Kind::Arc;
    edge.center = centerPoint;
    edge.radius = radiusValue;
    edge.startAngle = start;
    edge.endAngle = end;
    edge.counterClockwise = ccw;
    return edge;
}

Vec3 HatchEdge::startPoint() const {
    switch (kind) {
        case Kind::Line:
            return start;
        case Kind::Arc:
            return center + Vec3{std::cos(startAngle), std::sin(startAngle), 0.0} * radius;
        case Kind::Ellipse:
            return EllipseArc{center, majorAxis, ratio, startAngle, endAngle}.startPoint();
        case Kind::Spline:
            if (!fitPoints.empty()) { return fitPoints.front(); }
            if (!controlPoints.empty()) { return controlPoints.front(); }
            return {};
        default:
            return {};
    }
}

Vec3 HatchEdge::endPoint() const {
    switch (kind) {
        case Kind::Line:
            return end;
        case Kind::Arc:
            return center + Vec3{std::cos(endAngle), std::sin(endAngle), 0.0} * radius;
        case Kind::Ellipse:
            return EllipseArc{center, majorAxis, ratio, startAngle, endAngle}.endPoint();
        case Kind::Spline:
            if (!fitPoints.empty()) { return fitPoints.back(); }
            if (!controlPoints.empty()) { return controlPoints.back(); }
            return {};
        default:
            return {};
    }
}

CurvePtr HatchEdge::toCurve() const {
    switch (kind) {
        case Kind::Line:
            return std::make_unique<LineSegment>(start, end);

        case Kind::Arc: {
            // Group 73 says which way the *loop* traverses this edge, not what
            // shape it is. The shape is always the counter-clockwise arc from
            // group 50 to group 51, matching the ARC entity; the flag exists
            // because a closed boundary has to run end-to-end, and an edge
            // shared with a neighbouring loop runs the other way round.
            //
            // Reading it as "the angles are measured clockwise" — which mirrors
            // the arc about the X axis — is the other interpretation in
            // circulation. It is rejected here because it makes one flag change
            // both the geometry and the direction, and only the direction is
            // needed for the loop to close.
            const Arc arc{center, radius, startAngle, endAngle};
            return counterClockwise ? std::make_unique<Arc>(arc) : arc.reversed();
        }

        case Kind::Ellipse: {
            const EllipseArc ellipse{center, majorAxis, ratio, startAngle, endAngle};
            return counterClockwise ? std::make_unique<EllipseArc>(ellipse) : ellipse.reversed();
        }

        case Kind::Spline: {
            // No NURBS evaluator in the kernel yet. The fit points are the
            // curve's own record of where it passes, so a polyline through
            // them is a faithful approximation rather than a guess; control
            // points are the fallback and are looser, which is why they are
            // second choice.
            const std::vector<Vec3>& source = !fitPoints.empty() ? fitPoints : controlPoints;
            if (source.size() < 2) {
                return nullptr;
            }
            return std::make_unique<Polyline>(Polyline::fromPoints(source, periodic));
        }

        default:
            return nullptr;
    }
}

// -------------------------------------------------------------------- flags

HatchPathFlags HatchPathFlags::fromDxf(int flags) noexcept {
    HatchPathFlags out;
    out.isExternal = (flags & 1) != 0;
    out.isPolyline = (flags & 2) != 0;
    out.isDerived = (flags & 4) != 0;
    out.isTextbox = (flags & 8) != 0;
    out.isOutermost = (flags & 16) != 0;
    return out;
}

int HatchPathFlags::toDxf() const noexcept {
    int flags = 0;
    if (isExternal) { flags |= 1; }
    if (isPolyline) { flags |= 2; }
    if (isDerived) { flags |= 4; }
    if (isTextbox) { flags |= 8; }
    if (isOutermost) { flags |= 16; }
    return flags;
}

// -------------------------------------------------------------------- paths

std::vector<CurvePtr> HatchBoundaryPath::toCurves() const {
    std::vector<CurvePtr> curves;
    if (flags.isPolyline || !vertices.empty()) {
        if (vertices.size() >= 2) {
            curves.push_back(std::make_unique<Polyline>(vertices, closed));
        }
        return curves;
    }
    curves.reserve(edges.size());
    for (const HatchEdge& edge : edges) {
        if (CurvePtr curve = edge.toCurve()) {
            curves.push_back(std::move(curve));
        }
    }
    return curves;
}

std::vector<Vec3> HatchBoundaryPath::toPolygon(const TessellationOptions& options) const {
    std::vector<Vec3> ring;
    for (const CurvePtr& curve : toCurves()) {
        std::vector<Vec3> points = curve->tessellate(options);
        if (points.empty()) {
            continue;
        }
        // Successive edges share an endpoint; keeping both would put a
        // zero-length segment into every corner of the fill.
        const std::size_t first = ring.empty() ? 0 : 1;
        ring.insert(ring.end(), points.begin() + static_cast<std::ptrdiff_t>(first), points.end());
    }
    // The ring is implicitly closed, so a repeated final point is redundant.
    if (ring.size() >= 2 && ring.front().equals2(ring.back())) {
        ring.pop_back();
    }
    return ring;
}

Box3 HatchBoundaryPath::bounds() const {
    Box3 box;
    for (const CurvePtr& curve : toCurves()) {
        box.unite(curve->bounds());
    }
    return box;
}

// ------------------------------------------------------------------ pattern

double HatchPatternLine::patternLength() const {
    double total = 0.0;
    for (const double dash : dashes) {
        total += std::abs(dash);
    }
    return total;
}

std::optional<HatchPattern> standardHatchPattern(std::string_view name) {
    // Definitions in the same terms as an `acad.pat` file: angle, origin,
    // offset (x along the line, y between lines), then the dash list.
    struct Definition {
        const char* name;
        double angle;
        double offsetX;
        double offsetY;
        std::initializer_list<double> dashes;
        double secondAngle;
        bool hasSecond;
    };

    static const std::initializer_list<Definition> definitions = {
        {"ANSI31", kPi / 4.0, 0.0, 3.175, {}, 0.0, false},
        {"ANSI32", kPi / 4.0, 0.0, 9.525, {}, 0.0, false},
        {"ANSI37", kPi / 4.0, 0.0, 3.175, {}, -kPi / 4.0, true},
        {"NET", 0.0, 0.0, 3.175, {}, kPi / 2.0, true},
        {"LINE", 0.0, 0.0, 3.175, {}, 0.0, false},
        {"DOTS", 0.0, 0.0, 1.5875, {0.0, -1.5875}, 0.0, false},
    };

    const std::string wanted = normalizeSymbolName(name);
    if (wanted == normalizeSymbolName(kHatchPatternSolid)) {
        HatchPattern solid;
        solid.name = kHatchPatternSolid;
        solid.isSolid = true;
        return solid;
    }

    for (const Definition& definition : definitions) {
        if (normalizeSymbolName(definition.name) != wanted) {
            continue;
        }
        HatchPattern pattern;
        pattern.name = definition.name;
        pattern.isSolid = false;

        HatchPatternLine line;
        line.angle = definition.angle;
        line.offset = Vec3{definition.offsetX, definition.offsetY, 0.0};
        line.dashes = definition.dashes;
        pattern.lines.push_back(line);

        if (definition.hasSecond) {
            HatchPatternLine second = line;
            second.angle = definition.secondAngle;
            pattern.lines.push_back(second);
        }
        return pattern;
    }
    return std::nullopt;
}

// -------------------------------------------------------------------- entity

Box3 HatchEntity::bounds() const {
    Box3 box;
    for (const HatchBoundaryPath& path : paths) {
        box.unite(path.bounds());
    }
    return box;
}

std::vector<CurvePtr> HatchEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    for (const HatchBoundaryPath& path : paths) {
        std::vector<CurvePtr> loop = path.toCurves();
        curves.insert(curves.end(), std::make_move_iterator(loop.begin()),
                      std::make_move_iterator(loop.end()));
    }
    return curves;
}

std::vector<std::vector<Vec3>> HatchEntity::fillPolygons(
    const TessellationOptions& options) const {
    std::vector<std::vector<Vec3>> polygons;
    polygons.reserve(paths.size());
    for (const HatchBoundaryPath& path : paths) {
        if (style == HatchStyle::Ignore && !path.flags.isExternal && !path.flags.isOutermost) {
            // Islands are ignored, so their rings must not punch holes.
            continue;
        }
        std::vector<Vec3> ring = path.toPolygon(options);
        if (ring.size() >= 3) {
            polygons.push_back(std::move(ring));
        }
    }
    return polygons;
}

bool HatchEntity::contains(const Vec3& p) const {
    int crossings = 0;
    for (const HatchBoundaryPath& path : paths) {
        if (style == HatchStyle::Ignore && !path.flags.isExternal && !path.flags.isOutermost) {
            continue;
        }
        const std::vector<Vec3> ring = path.toPolygon();
        if (ring.size() >= 3 && pointInRing(ring, p)) {
            ++crossings;
        }
    }
    // Odd nesting depth is inside, which is the even-odd rule that also gives
    // Normal style its alternating islands.
    return (crossings % 2) == 1;
}

double HatchEntity::distanceToPoint(const Vec3& p) const {
    if (contains(p)) {
        return 0.0;
    }
    double best = std::numeric_limits<double>::infinity();
    for (const HatchBoundaryPath& path : paths) {
        const std::vector<Vec3> ring = path.toPolygon();
        if (ring.size() >= 2) {
            best = std::min(best, distanceToRing(ring, p));
        }
    }
    return best;
}

std::vector<LineSegment> HatchEntity::generateLines(const Box3& clip,
                                                    std::size_t maxSegments) const {
    std::vector<LineSegment> segments;
    if (pattern.isSolid || pattern.lines.empty()) {
        return segments;
    }

    const Box3 area = bounds().intersected(clip);
    if (area.isEmpty()) {
        return segments;
    }

    // Collect the boundary once. Every pattern line intersects against all of
    // it, so tessellating per line would dominate the cost.
    std::vector<CurvePtr> boundary;
    for (const HatchBoundaryPath& path : paths) {
        if (style == HatchStyle::Ignore && !path.flags.isExternal && !path.flags.isOutermost) {
            continue;
        }
        if (style == HatchStyle::Outer && !path.flags.isExternal && !path.flags.isOutermost) {
            continue;
        }
        std::vector<CurvePtr> loop = path.toCurves();
        boundary.insert(boundary.end(), std::make_move_iterator(loop.begin()),
                        std::make_move_iterator(loop.end()));
    }
    if (boundary.empty()) {
        return segments;
    }

    // A line long enough to cross the whole region from outside it, so the
    // crossing count is complete however the region is shaped.
    const double reach = area.diagonal() + 1.0;
    const Vec3 middle = area.min.midpoint(area.max);
    const double scale = pattern.scale > 0.0 ? pattern.scale : 1.0;

    std::vector<HatchPatternLine> families = pattern.lines;
    if (pattern.isDouble) {
        const std::size_t original = families.size();
        for (std::size_t i = 0; i < original; ++i) {
            HatchPatternLine crossed = families[i];
            crossed.angle += kPi / 2.0;
            families.push_back(crossed);
        }
    }

    for (const HatchPatternLine& family : families) {
        const double angle = family.angle + pattern.angle;
        const Vec3 direction{std::cos(angle), std::sin(angle), 0.0};
        const Vec3 across{-direction.y, direction.x, 0.0};

        const double spacing = std::abs(family.offset.y) * scale;
        if (spacing < 1e-9) {
            // A zero perpendicular spacing describes an infinite number of
            // coincident lines. Skipped rather than clamped: a pattern this
            // broken cannot be drawn usefully at any spacing we invent.
            continue;
        }
        const double shift = family.offset.x * scale;

        // Where the family's origin sits relative to the region, measured
        // across the lines, so only the lines that can reach it are generated.
        const Vec3 origin = rotated(family.basePoint * scale, pattern.angle);
        const double centreOffset = (middle - origin).dot(across);
        const auto lowest = static_cast<long long>(std::floor((centreOffset - reach) / spacing));
        const auto highest = static_cast<long long>(std::ceil((centreOffset + reach) / spacing));

        // Measured in world units, so a pattern scaled to nothing is caught
        // here rather than dividing by zero further down.
        const double scaledPattern = family.patternLength() * scale;
        const bool dashed = scaledPattern > 1e-9 && !family.dashes.empty();

        for (long long index = lowest; index <= highest; ++index) {
            if (segments.size() >= maxSegments) {
                return segments;
            }
            const auto n = static_cast<double>(index);

            // Where this line's dash pattern starts. The offset's x component
            // slides successive lines *along themselves*, which is what makes a
            // brick pattern stagger; since a line is unchanged by sliding along
            // itself, that shift affects the dash phase and nothing else.
            const Vec3 phaseOrigin = origin + across * (n * spacing) + direction * (n * shift);

            // The drawn span has to cover the hatched region, so the finite ray
            // is centred on the region rather than on the phase origin. Those
            // coincide only when the shift is zero — and when it is not, a ray
            // built around the phase origin walks further from the hatch with
            // every line and the pattern silently draws nothing at all.
            const Vec3 centre =
                phaseOrigin + direction * (middle - phaseOrigin).dot(direction);
            const LineSegment ray{centre - direction * reach, centre + direction * reach};

            // Where the line enters and leaves the boundary.
            std::vector<double> crossings;
            for (const CurvePtr& curve : boundary) {
                for (const Intersection& hit : intersectCurves(ray, *curve)) {
                    crossings.push_back(hit.paramA);
                }
            }
            if (crossings.size() < 2) {
                continue;
            }
            std::sort(crossings.begin(), crossings.end());

            for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                const double t0 = crossings[i];
                const double t1 = crossings[i + 1];
                if (t1 - t0 < 1e-12) {
                    continue;
                }
                const Vec3 from = ray.pointAt(t0);
                const Vec3 to = ray.pointAt(t1);

                if (!dashed) {
                    segments.push_back(LineSegment{from, to});
                    if (segments.size() >= maxSegments) {
                        return segments;
                    }
                    continue;
                }

                // Walk the dash pattern along the span, phased from the
                // family's own origin so adjacent spans line up.
                const double spanStart = (from - phaseOrigin).dot(direction);
                const double spanEnd = (to - phaseOrigin).dot(direction);
                double at = std::floor(spanStart / scaledPattern) * scaledPattern;
                std::size_t dash = 0;
                while (at < spanEnd) {
                    const double length = std::abs(family.dashes[dash]) * scale;
                    const double step = length < 1e-9 ? 1e-9 : length;
                    const double next = at + step;
                    if (family.dashes[dash] >= 0.0 && next > spanStart) {
                        const double a = std::max(at, spanStart);
                        const double b = std::min(next, spanEnd);
                        if (b > a) {
                            segments.push_back(LineSegment{phaseOrigin + direction * a,
                                                           phaseOrigin + direction * b});
                            if (segments.size() >= maxSegments) {
                                return segments;
                            }
                        }
                    }
                    at = next;
                    dash = (dash + 1) % family.dashes.size();
                }
            }
        }
    }
    return segments;
}

void HatchEntity::transformBy(const Mat4& m) {
    for (HatchBoundaryPath& path : paths) {
        for (PolylineVertex& vertex : path.vertices) {
            vertex.position = m.transformPoint(vertex.position);
        }
        for (HatchEdge& edge : path.edges) {
            edge.start = m.transformPoint(edge.start);
            edge.end = m.transformPoint(edge.end);
            edge.center = m.transformPoint(edge.center);
            edge.majorAxis = m.transformVector(edge.majorAxis);
            for (Vec3& point : edge.controlPoints) { point = m.transformPoint(point); }
            for (Vec3& point : edge.fitPoints) { point = m.transformPoint(point); }
            edge.radius *= m.scaleFactor2();
            // A mirror reverses which way round the boundary runs, and a
            // boundary traversed backwards leaves the fill on the wrong side.
            if (m.isMirrored()) {
                edge.counterClockwise = !edge.counterClockwise;
            }
        }
    }
    for (Vec3& seed : seedPoints) {
        seed = m.transformPoint(seed);
    }
    // The pattern scales and rotates with the region, so hatching stays
    // attached to the geometry rather than sliding across it.
    pattern.scale *= m.scaleFactor2();
    const Vec3 rotatedAxis = m.transformVector(Vec3::unitX());
    if (rotatedAxis.lengthSquared() > kDegenerate) {
        pattern.angle += rotatedAxis.angle2();
    }
}

std::vector<GripPoint> HatchEntity::gripPoints() const {
    // The boundary vertices, so a hatch can be reshaped directly. Capped:
    // a boundary derived from a complex region can have thousands, and a
    // screen full of grips is not an editing aid.
    constexpr std::size_t kMaxGrips = 64;
    std::vector<GripPoint> grips;
    for (const HatchBoundaryPath& path : paths) {
        for (const PolylineVertex& vertex : path.vertices) {
            if (grips.size() >= kMaxGrips) { return grips; }
            grips.push_back({vertex.position, GripKind::Vertex, grips.size()});
        }
        for (const HatchEdge& edge : path.edges) {
            if (grips.size() >= kMaxGrips) { return grips; }
            grips.push_back({edge.startPoint(), GripKind::Vertex, grips.size()});
        }
    }
    return grips;
}

void HatchEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    std::size_t index = 0;
    for (HatchBoundaryPath& path : paths) {
        for (PolylineVertex& vertex : path.vertices) {
            if (index++ == grip.index) {
                vertex.position = to;
                return;
            }
        }
        for (HatchEdge& edge : path.edges) {
            if (index++ == grip.index) {
                // Moving a vertex moves the end of the previous edge too, or
                // the loop comes apart.
                edge.start = to;
                return;
            }
        }
    }
}

DbObjectPtr HatchEntity::clone() const {
    auto copy = std::make_unique<HatchEntity>();
    copyEntityInto(*copy);
    copy->paths = paths;
    copy->pattern = pattern;
    copy->gradient = gradient;
    copy->style = style;
    copy->isAssociative = isAssociative;
    copy->seedPoints = seedPoints;
    copy->pixelSize = pixelSize;
    return copy;
}

void HatchEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const HatchEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a hatch");
    }
    restoreEntityFrom(*source);
    paths = source->paths;
    pattern = source->pattern;
    gradient = source->gradient;
    style = source->style;
    isAssociative = source->isAssociative;
    seedPoints = source->seedPoints;
    pixelSize = source->pixelSize;
}

std::vector<Handle> HatchEntity::referencedHandles() const {
    std::vector<Handle> handles = Entity::referencedHandles();
    for (const HatchBoundaryPath& path : paths) {
        for (const Handle source : path.sourceHandles) {
            if (!source.isNull()) {
                handles.push_back(source);
            }
        }
    }
    return handles;
}

}  // namespace cad
