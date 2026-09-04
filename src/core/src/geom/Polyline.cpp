#include "cad/geom/Polyline.hpp"

#include <algorithm>

#include "cad/geom/Arc.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {

Polyline::Polyline(std::vector<PolylineVertex> vertexList, bool closedFlag, const Vec3& extrusion)
    : vertices(std::move(vertexList)), normal(extrusion) {
    // Two vertices are enough to close a polyline: with bulges, the pair forms
    // a lens or a full circle, which other CAD software both permits and produces. Only a
    // single vertex has no closing segment to speak of.
    closed_ = closedFlag && vertices.size() >= 2;
}

Polyline::Polyline(const Polyline& other)
    : vertices(other.vertices), normal(other.normal), closed_(other.closed_) {
    // Cached segments are deliberately not copied: they hold unique_ptrs and
    // are cheap to rebuild on demand.
}

Polyline& Polyline::operator=(const Polyline& other) {
    if (this != &other) {
        vertices = other.vertices;
        normal = other.normal;
        closed_ = other.closed_;
        invalidate();
    }
    return *this;
}

Polyline Polyline::fromPoints(std::span<const Vec3> points, bool closedFlag) {
    std::vector<PolylineVertex> vertexList;
    vertexList.reserve(points.size());
    for (const Vec3& p : points) {
        vertexList.emplace_back(p);
    }
    return Polyline{std::move(vertexList), closedFlag};
}

Polyline Polyline::rectangle(const Vec3& corner1, const Vec3& corner2) {
    const double z = corner1.z;
    const std::array<Vec3, 4> points{{{corner1.x, corner1.y, z},
                                      {corner2.x, corner1.y, z},
                                      {corner2.x, corner2.y, z},
                                      {corner1.x, corner2.y, z}}};
    return fromPoints(points, true);
}

void Polyline::invalidate() noexcept {
    cachedSegments_.clear();
    cachedLengths_.clear();
    segmentsValid_ = false;
}

void Polyline::setClosed(bool value) {
    const bool next = value && vertices.size() >= 2;
    if (next != closed_) {
        closed_ = next;
        invalidate();
    }
}

std::size_t Polyline::segmentCount() const noexcept {
    const std::size_t n = vertices.size();
    if (n < 2) {
        return 0;
    }
    return closed_ ? n : n - 1;
}

void Polyline::buildSegments() const {
    cachedSegments_.clear();
    const std::size_t n = vertices.size();
    const std::size_t count = segmentCount();
    cachedSegments_.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const PolylineVertex& a = vertices[i];
        const PolylineVertex& b = vertices[(i + 1) % n];

        std::optional<Arc> arc;
        if (a.bulge != 0.0) {
            arc = Arc::fromBulge(a.position, b.position, a.bulge);
        }
        if (arc) {
            cachedSegments_.push_back(std::make_unique<Arc>(*arc));
        } else {
            cachedSegments_.push_back(std::make_unique<LineSegment>(a.position, b.position));
        }
    }

    cachedLengths_.clear();
    cachedLengths_.reserve(cachedSegments_.size());
    for (const CurvePtr& segment : cachedSegments_) {
        cachedLengths_.push_back(segment->length());
    }
    segmentsValid_ = true;
}

const std::vector<CurvePtr>& Polyline::segments() const {
    if (!segmentsValid_) {
        buildSegments();
    }
    return cachedSegments_;
}

const std::vector<double>& Polyline::segmentLengths() const {
    if (!segmentsValid_) {
        buildSegments();
    }
    return cachedLengths_;
}

const Curve* Polyline::segmentAt(std::size_t index) const {
    const auto& segs = segments();
    return index < segs.size() ? segs[index].get() : nullptr;
}

Vec3 Polyline::startPoint() const {
    return vertices.empty() ? Vec3::zero() : vertices.front().position;
}

Vec3 Polyline::endPoint() const {
    if (closed_) {
        return startPoint();
    }
    return vertices.empty() ? Vec3::zero() : vertices.back().position;
}

double Polyline::length() const {
    double total = 0.0;
    for (const double l : segmentLengths()) {
        total += l;
    }
    return total;
}

Polyline::ResolvedParam Polyline::resolveParam(double t) const {
    const std::size_t count = segmentCount();
    if (count == 0) {
        return {0, 0.0};
    }
    const double scaled = clampParam(t) * static_cast<double>(count);
    auto index = static_cast<std::size_t>(std::floor(scaled));
    if (index >= count) {
        index = count - 1;
    }
    return {index, scaled - static_cast<double>(index)};
}

double Polyline::makeParam(std::size_t index, double local) const {
    const std::size_t count = segmentCount();
    if (count == 0) {
        return 0.0;
    }
    return clamp((static_cast<double>(index) + local) / static_cast<double>(count), 0.0, 1.0);
}

Vec3 Polyline::pointAt(double t) const {
    if (isEmpty()) {
        return startPoint();
    }
    const ResolvedParam resolved = resolveParam(t);
    return segments()[resolved.index]->pointAt(resolved.local);
}

Vec3 Polyline::tangentAt(double t) const {
    if (isEmpty()) {
        return Vec3::unitX();
    }
    const ResolvedParam resolved = resolveParam(t);
    return segments()[resolved.index]->tangentAt(resolved.local);
}

double Polyline::paramAtLength(double distance) const {
    const auto& lengths = segmentLengths();
    const double total = length();
    if (total <= 0.0) {
        return 0.0;
    }

    double remaining = clamp(distance, 0.0, total);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        const double segmentLength = lengths[i];
        if (remaining <= segmentLength || i == lengths.size() - 1) {
            const double local = segmentLength > 0.0 ? remaining / segmentLength : 0.0;
            return makeParam(i, clamp(local, 0.0, 1.0));
        }
        remaining -= segmentLength;
    }
    return 1.0;
}

double Polyline::lengthAtParam(double t) const {
    const ResolvedParam resolved = resolveParam(t);
    const auto& lengths = segmentLengths();
    double total = 0.0;
    for (std::size_t i = 0; i < resolved.index && i < lengths.size(); ++i) {
        total += lengths[i];
    }
    const double current = resolved.index < lengths.size() ? lengths[resolved.index] : 0.0;
    return total + current * resolved.local;
}

Box3 Polyline::bounds() const {
    Box3 box;
    for (const CurvePtr& segment : segments()) {
        box = box.united(segment->bounds());
    }
    return box;
}

ClosestPointResult Polyline::closestPoint(const Vec3& p) const {
    if (isEmpty()) {
        const Vec3 point = startPoint();
        return {point, 0.0, p.distanceTo(point)};
    }

    const auto& segs = segments();
    ClosestPointResult best = segs[0]->closestPoint(p);
    std::size_t bestIndex = 0;

    for (std::size_t i = 1; i < segs.size(); ++i) {
        const ClosestPointResult candidate = segs[i]->closestPoint(p);
        if (candidate.distance < best.distance) {
            best = candidate;
            bestIndex = i;
        }
    }
    return {best.point, makeParam(bestIndex, best.param), best.distance};
}

std::optional<ClosestPointResult> Polyline::closestPointExtended(const Vec3& p) const {
    if (closed_ || isEmpty()) {
        return std::nullopt;
    }
    const auto& segs = segments();
    const auto first = segs.front()->closestPointExtended(p);
    const auto last = segs.back()->closestPointExtended(p);

    if (!first) {
        return last;
    }
    if (!last) {
        return first;
    }
    return first->distance <= last->distance ? first : last;
}

double Polyline::signedArea() const {
    const std::size_t n = vertices.size();
    if (n < 3 && !closed_) {
        return 0.0;
    }

    // Every term, wrapping, whether or not the polyline is flagged closed.
    //
    // The shoelace sum is over the edges of a *loop*, so leaving out the one
    // from the last vertex back to the first does not measure the open shape —
    // it measures nothing in particular. An open polyline whose ends happen to
    // coincide came out right, which is why this survived: that is most of them.
    //
    // Area boundaries in real drawings are regularly drawn by clicking back
    // onto the start point rather than by using Close, and those are exactly the
    // ones the omitted term is zero for. The ones drawn to a *different* last
    // point, and closed implicitly, were reported wrong.
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3& a = vertices[i].position;
        const Vec3& b = vertices[(i + 1) % n].position;
        area += a.x * b.y - b.x * a.y;
    }
    area *= 0.5;

    // Each arc adds or removes the circular segment between chord and arc.
    const auto& segs = segments();
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const auto* arc = dynamic_cast<const Arc*>(segs[i].get());
        if (arc == nullptr) {
            continue;
        }
        const double bulge = vertices[i].bulge;
        const double segmentArea =
            0.5 * arc->radius * arc->radius * (arc->sweep() - std::sin(arc->sweep()));
        area += bulge > 0.0 ? segmentArea : -segmentArea;
    }
    return area;
}

bool Polyline::containsPointInside(const Vec3& p, const TessellationOptions& options) const {
    if (!closed_) {
        return false;
    }
    const std::vector<Vec3> points = tessellate(options);
    int winding = 0;

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[i + 1];
        const double side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
        if (a.y <= p.y) {
            if (b.y > p.y && side > 0.0) {
                ++winding;
            }
        } else if (b.y <= p.y && side < 0.0) {
            --winding;
        }
    }
    return winding != 0;
}

CurvePtr Polyline::transformed(const Mat4& m) const {
    // Bulge is invariant under conformal transforms but flips sign under a
    // mirror, since the sweep direction reverses.
    const double flip = m.isMirrored() ? -1.0 : 1.0;
    const double scale = m.scaleFactor2();

    std::vector<PolylineVertex> transformedVertices;
    transformedVertices.reserve(vertices.size());
    for (const PolylineVertex& v : vertices) {
        transformedVertices.emplace_back(m.transformPoint(v.position), v.bulge * flip,
                                         v.startWidth * scale, v.endWidth * scale);
    }

    const Vec3 newNormal = m.transformVector(normal).tryNormalize().value_or(normal);
    return std::make_unique<Polyline>(std::move(transformedVertices), closed_, newNormal);
}

CurvePtr Polyline::reversed() const {
    const std::size_t n = vertices.size();
    if (n == 0) {
        return clone();
    }

    std::vector<PolylineVertex> reversedVertices;
    reversedVertices.reserve(n);

    // Bulge and widths belong to the span *after* each vertex, so reversing the
    // order also shifts those attributes back by one and negates the bulge.
    for (std::size_t i = n; i-- > 0;) {
        const PolylineVertex& v = vertices[i];
        double carriedBulge = 0.0;
        double carriedStart = 0.0;
        double carriedEnd = 0.0;

        if (i > 0 || closed_) {
            const std::size_t sourceIndex = (i + n - 1) % n;
            const PolylineVertex& source = vertices[sourceIndex];
            carriedBulge = -source.bulge;
            carriedStart = source.endWidth;
            carriedEnd = source.startWidth;
        }
        reversedVertices.emplace_back(v.position, carriedBulge, carriedStart, carriedEnd);
    }
    return std::make_unique<Polyline>(std::move(reversedVertices), closed_, normal);
}

std::vector<Vec3> Polyline::tessellate(const TessellationOptions& options) const {
    std::vector<Vec3> points;
    const auto& segs = segments();

    for (std::size_t i = 0; i < segs.size(); ++i) {
        const std::vector<Vec3> segmentPoints = segs[i]->tessellate(options);
        // Skip the leading point of every segment after the first; it
        // duplicates the previous segment's endpoint.
        for (std::size_t j = (i == 0 ? 0 : 1); j < segmentPoints.size(); ++j) {
            points.push_back(segmentPoints[j]);
        }
    }

    if (points.empty() && vertices.size() == 1) {
        points.push_back(vertices.front().position);
    }
    return points;
}

CurvePtr Polyline::trimmed(double t0, double t1) const {
    const double lower = clampParam(std::min(t0, t1));
    const double upper = clampParam(std::max(t0, t1));
    const ResolvedParam start = resolveParam(lower);
    const ResolvedParam end = resolveParam(upper);

    std::vector<PolylineVertex> out;
    const auto& segs = segments();

    if (segs.empty()) {
        return std::make_unique<Polyline>(std::move(out), false, normal);
    }

    const auto bulgeOf = [](const Curve& c) {
        const auto* arc = dynamic_cast<const Arc*>(&c);
        return arc == nullptr ? 0.0 : arc->bulge();
    };

    if (start.index == end.index) {
        const CurvePtr piece = segs[start.index]->trimmed(start.local, end.local);
        out.emplace_back(piece->startPoint(), bulgeOf(*piece));
        out.emplace_back(piece->endPoint());
        return std::make_unique<Polyline>(std::move(out), false, normal);
    }

    const CurvePtr firstPiece = segs[start.index]->trimmed(start.local, 1.0);
    out.emplace_back(firstPiece->startPoint(), bulgeOf(*firstPiece));

    for (std::size_t i = start.index + 1; i < end.index; ++i) {
        out.push_back(vertices[i % vertices.size()]);
    }

    const CurvePtr lastPiece = segs[end.index]->trimmed(0.0, end.local);
    out.emplace_back(lastPiece->startPoint(), bulgeOf(*lastPiece));
    out.emplace_back(lastPiece->endPoint());

    return std::make_unique<Polyline>(std::move(out), false, normal);
}

bool Polyline::hasArcs() const {
    return std::any_of(vertices.begin(), vertices.end(),
                       [](const PolylineVertex& v) { return v.bulge != 0.0; });
}

bool Polyline::hasWidth() const {
    return std::any_of(vertices.begin(), vertices.end(), [](const PolylineVertex& v) {
        return v.startWidth != 0.0 || v.endWidth != 0.0;
    });
}

bool Polyline::equals(const Polyline& other, const Tolerance& tol) const {
    if (closed_ != other.closed_ || vertices.size() != other.vertices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (!vertices[i].position.equals(other.vertices[i].position, tol)) {
            return false;
        }
        if (std::abs(vertices[i].bulge - other.vertices[i].bulge) > tol.point) {
            return false;
        }
    }
    return true;
}

}  // namespace cad
