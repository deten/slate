#include "cad/geom/BoundaryTrace.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>

#include "cad/geom/Intersect.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"
#include "cad/math/Angle.hpp"

namespace cad {
namespace {

/// A piece of an input curve between two crossings.
struct Edge {
    CurvePtr curve;
    std::size_t source = 0;
    std::size_t from = 0;
    std::size_t to = 0;
    Vec3 startTangent;
    Vec3 endTangent;
};

/// A direction along an edge. `index * 2 + reversed`.
struct HalfEdge {
    std::size_t edge = 0;
    bool reversed = false;

    [[nodiscard]] std::size_t id() const noexcept { return edge * 2 + (reversed ? 1 : 0); }
    [[nodiscard]] HalfEdge twin() const noexcept { return {edge, !reversed}; }
};

class Graph {
public:
    explicit Graph(double mergeDistance) : cell_(std::max(mergeDistance, 1e-12)) {}

    std::vector<Edge> edges;
    std::vector<Vec3> vertices;
    /// Outgoing half-edges from each vertex.
    std::vector<std::vector<HalfEdge>> outgoing;

    [[nodiscard]] std::size_t vertexFor(const Vec3& point) {
        // Quantised lookup with a look at the neighbouring cells, so two ends
        // that sit a hair apart across a cell boundary still meet.
        const long long cx = static_cast<long long>(std::floor(point.x / cell_));
        const long long cy = static_cast<long long>(std::floor(point.y / cell_));
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto found = cells_.find({cx + dx, cy + dy});
                if (found == cells_.end()) {
                    continue;
                }
                for (const std::size_t index : found->second) {
                    if (vertices[index].distanceTo(point) <= cell_) {
                        return index;
                    }
                }
            }
        }
        vertices.push_back(point);
        outgoing.emplace_back();
        cells_[{cx, cy}].push_back(vertices.size() - 1);
        return vertices.size() - 1;
    }

    void addEdge(CurvePtr curve, std::size_t source) {
        Edge edge;
        edge.source = source;
        edge.from = vertexFor(curve->startPoint());
        edge.to = vertexFor(curve->endPoint());
        edge.startTangent = curve->tangentAt(0.0);
        edge.endTangent = curve->tangentAt(1.0);
        edge.curve = std::move(curve);
        edges.push_back(std::move(edge));
        const std::size_t index = edges.size() - 1;
        outgoing[edges[index].from].push_back({index, false});
        outgoing[edges[index].to].push_back({index, true});
    }

    [[nodiscard]] std::size_t startOf(HalfEdge h) const {
        return h.reversed ? edges[h.edge].to : edges[h.edge].from;
    }
    [[nodiscard]] std::size_t endOf(HalfEdge h) const {
        return h.reversed ? edges[h.edge].from : edges[h.edge].to;
    }
    /// Direction leaving the start of the half-edge.
    [[nodiscard]] Vec3 outDirection(HalfEdge h) const {
        return h.reversed ? -edges[h.edge].endTangent : edges[h.edge].startTangent;
    }
    /// Direction arriving at the end of the half-edge.
    [[nodiscard]] Vec3 inDirection(HalfEdge h) const {
        return h.reversed ? -edges[h.edge].startTangent : edges[h.edge].endTangent;
    }

private:
    double cell_;
    std::map<std::pair<long long, long long>, std::vector<std::size_t>> cells_;
};

[[nodiscard]] double angleOf(const Vec3& direction) { return std::atan2(direction.y, direction.x); }

/// The next half-edge on the face to the left of `h`.
///
/// At the vertex `h` arrives at, the face on the left continues along the
/// outgoing edge that is the first one met turning clockwise from the way we
/// came. That is the sharpest left turn available, which is what keeps the
/// walk on the smallest face rather than drifting out into a bigger one.
[[nodiscard]] std::optional<HalfEdge> nextOnFace(const Graph& graph, HalfEdge h) {
    const std::size_t vertex = graph.endOf(h);
    const double back = angleOf(-graph.inDirection(h));
    std::optional<HalfEdge> best;
    double bestTurn = kTau + 1.0;
    for (const HalfEdge candidate : graph.outgoing[vertex]) {
        if (candidate.id() == h.twin().id()) {
            continue;
        }
        double turn = std::fmod(back - angleOf(graph.outDirection(candidate)) + 2.0 * kTau, kTau);
        // A candidate lying exactly along the way back is a full turn, not
        // none: it is the last choice, after everything that actually leads
        // somewhere else.
        if (turn < 1e-9) {
            turn = kTau;
        }
        if (turn < bestTurn) {
            bestTurn = turn;
            best = candidate;
        }
    }
    if (!best) {
        // A dead end. Walking back along the same edge lets the trace get out
        // of a spur; the pair is dropped afterwards.
        for (const HalfEdge candidate : graph.outgoing[vertex]) {
            if (candidate.id() == h.twin().id()) {
                return candidate;
            }
        }
    }
    return best;
}

/// Walk the face to the left of `start` back round to `start`.
[[nodiscard]] std::optional<std::vector<HalfEdge>> walkFace(const Graph& graph, HalfEdge start) {
    std::vector<HalfEdge> ring;
    HalfEdge h = start;
    const std::size_t limit = graph.edges.size() * 4 + 16;
    for (std::size_t step = 0; step < limit; ++step) {
        ring.push_back(h);
        const std::optional<HalfEdge> next = nextOnFace(graph, h);
        if (!next) {
            return std::nullopt;
        }
        h = *next;
        if (h.id() == start.id()) {
            break;
        }
    }
    if (h.id() != start.id()) {
        return std::nullopt;
    }
    // Drop spurs: an edge immediately followed by its own twin bounds nothing.
    bool changed = true;
    while (changed && ring.size() > 1) {
        changed = false;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const std::size_t j = (i + 1) % ring.size();
            if (ring[i].id() == ring[j].twin().id()) {
                if (j > i) {
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(j));
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                    ring.erase(ring.begin());
                }
                changed = true;
                break;
            }
        }
    }
    if (ring.empty()) {
        return std::nullopt;
    }
    return ring;
}

[[nodiscard]] std::vector<Vec3> polygonOf(const Graph& graph, std::span<const HalfEdge> ring) {
    std::vector<Vec3> polygon;
    for (const HalfEdge h : ring) {
        std::vector<Vec3> points = graph.edges[h.edge].curve->tessellate();
        if (h.reversed) {
            std::reverse(points.begin(), points.end());
        }
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            polygon.push_back(points[i]);
        }
    }
    return polygon;
}

[[nodiscard]] bool insidePolygon(std::span<const Vec3> polygon, const Vec3& p) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

[[nodiscard]] double signedArea(std::span<const Vec3> polygon) {
    double area = 0.0;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        area += polygon[j].cross2(polygon[i]);
    }
    return area * 0.5;
}

/// The half-edge of `edge` that has `point` on its left, judged at the place
/// on the edge nearest `point`.
[[nodiscard]] HalfEdge facing(const Graph& graph, std::size_t edge, const Vec3& point) {
    const Curve& curve = *graph.edges[edge].curve;
    const ClosestPointResult nearest = curve.closestPoint(point);
    const Vec3 direction = curve.tangentAt(nearest.param);
    const bool onLeft = direction.cross2(point - nearest.point) > 0.0;
    return {edge, !onLeft};
}

[[nodiscard]] BoundaryLoop loopFrom(const Graph& graph, std::span<const HalfEdge> ring) {
    BoundaryLoop loop;
    for (const HalfEdge h : ring) {
        const Edge& edge = graph.edges[h.edge];
        loop.curves.push_back(h.reversed ? edge.curve->reversed() : edge.curve->clone());
        loop.sources.push_back(edge.source);
    }
    return loop;
}

}  // namespace

BoundaryTraceResult traceBoundary(std::span<const Curve* const> curves, const Vec3& seed,
                                  const BoundaryTraceOptions& options) {
    BoundaryTraceResult result;

    // Pieces: polylines come apart into their segments so a crossing in the
    // middle of one splits it like anything else.
    struct Piece {
        const Curve* curve;
        std::size_t source;
        Box3 bounds;
        std::vector<double> cuts;
    };
    std::vector<Piece> pieces;
    Box3 everything;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const Curve* curve = curves[i];
        if (curve == nullptr) {
            continue;
        }
        if (const auto* polyline = dynamic_cast<const Polyline*>(curve)) {
            for (const CurvePtr& segment : polyline->segments()) {
                if (segment != nullptr && segment->length() > options.tolerance.point) {
                    pieces.push_back({segment.get(), i, segment->bounds(), {}});
                    everything = everything.united(segment->bounds());
                }
            }
            continue;
        }
        if (curve->length() > options.tolerance.point) {
            pieces.push_back({curve, i, curve->bounds(), {}});
            everything = everything.united(curve->bounds());
        }
    }
    if (pieces.empty()) {
        result.error = "There is nothing around the point to trace.";
        return result;
    }

    const double diagonal = std::max(everything.diagonal(), 1.0);
    const double merge = std::max(diagonal * 1e-9, options.tolerance.point);

    // Where pieces cross. Only pairs whose boxes overlap are asked, which is
    // what keeps a drawing with thousands of lines affordable.
    IntersectOptions intersectOptions;
    intersectOptions.tolerance = options.tolerance;
    for (std::size_t a = 0; a < pieces.size(); ++a) {
        for (std::size_t b = a + 1; b < pieces.size(); ++b) {
            if (!pieces[a].bounds.expanded(merge).intersects2(pieces[b].bounds)) {
                continue;
            }
            for (const Intersection& hit :
                 intersectCurves(*pieces[a].curve, *pieces[b].curve, intersectOptions)) {
                pieces[a].cuts.push_back(hit.paramA);
                pieces[b].cuts.push_back(hit.paramB);
            }
        }
    }

    Graph graph{merge};
    for (Piece& piece : pieces) {
        std::vector<double> cuts = sanitizeSplitParams(piece.cuts, 1e-9);
        std::vector<CurvePtr> parts =
            cuts.empty() ? std::vector<CurvePtr>{} : piece.curve->split(cuts);
        if (parts.empty()) {
            parts.push_back(piece.curve->clone());
        }
        for (CurvePtr& part : parts) {
            if (part != nullptr && part->length() > merge) {
                graph.addEdge(std::move(part), piece.source);
            }
        }
    }
    if (graph.edges.empty()) {
        result.error = "There is nothing around the point to trace.";
        return result;
    }

    // A ray from the seed to the right; the first edge it meets bounds the
    // face the seed is in.
    const LineSegment ray{seed, seed + Vec3{diagonal * 2.0 + 1.0, 0.0, 0.0}};
    std::optional<std::size_t> hitEdge;
    double nearest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        for (const Intersection& hit :
             intersectCurves(ray, *graph.edges[i].curve, intersectOptions)) {
            const double distance = hit.point.x - seed.x;
            if (distance >= -options.tolerance.point && distance < nearest) {
                nearest = distance;
                hitEdge = i;
            }
        }
    }
    if (!hitEdge) {
        result.error = "The point is not enclosed by anything.";
        return result;
    }

    const std::optional<std::vector<HalfEdge>> ring =
        walkFace(graph, facing(graph, *hitEdge, seed));
    if (!ring) {
        result.error = "The boundary around the point is not closed.";
        return result;
    }
    const std::vector<Vec3> outer = polygonOf(graph, *ring);
    if (outer.size() < 3 || !insidePolygon(outer, seed) || signedArea(outer) <= 0.0) {
        result.error = "The boundary around the point is not closed.";
        return result;
    }
    result.loops.push_back(loopFrom(graph, *ring));

    if (!options.detectIslands) {
        return result;
    }

    // Islands: rings made of edges inside the region. Each connected group of
    // such edges is walked once from its leftmost edge, keeping the outside of
    // the group on the left, which yields the group's outer ring.
    std::vector<bool> onOuter(graph.edges.size(), false);
    for (const HalfEdge h : *ring) {
        onOuter[h.edge] = true;
    }
    std::vector<std::size_t> parent(graph.vertices.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](std::size_t v) {
        while (parent[v] != v) {
            parent[v] = parent[parent[v]];
            v = parent[v];
        }
        return v;
    };
    std::vector<bool> inside(graph.edges.size(), false);
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        if (onOuter[i]) {
            continue;
        }
        const Edge& edge = graph.edges[i];
        if (!insidePolygon(outer, edge.curve->midPoint())) {
            continue;
        }
        inside[i] = true;
        parent[find(edge.from)] = find(edge.to);
    }
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        if (inside[i]) {
            groups[find(graph.edges[i].from)].push_back(i);
        }
    }

    std::vector<std::vector<Vec3>> islandPolygons;
    for (const auto& [root, members] : groups) {
        // The leftmost point of the group, and the edge it is on.
        std::size_t leftEdge = members.front();
        double leftX = std::numeric_limits<double>::infinity();
        for (const std::size_t index : members) {
            const double x = graph.edges[index].curve->bounds().min.x;
            if (x < leftX) {
                leftX = x;
                leftEdge = index;
            }
        }
        const Vec3 outside{leftX - merge * 10.0, graph.edges[leftEdge].curve->closestPoint(
                                                     Vec3{leftX, graph.edges[leftEdge].curve->bounds().center().y, 0.0}).point.y,
                           0.0};
        // A group already inside another island is a hole in a hole, and is
        // left to the fill rule.
        bool nested = false;
        for (const std::vector<Vec3>& island : islandPolygons) {
            nested = nested || insidePolygon(island, outside);
        }
        if (nested) {
            continue;
        }
        const std::optional<std::vector<HalfEdge>> islandRing =
            walkFace(graph, facing(graph, leftEdge, outside));
        if (!islandRing || islandRing->size() < 1) {
            continue;
        }
        std::vector<Vec3> polygon = polygonOf(graph, *islandRing);
        if (polygon.size() < 3 || insidePolygon(polygon, seed)) {
            continue;
        }
        result.loops.push_back(loopFrom(graph, *islandRing));
        islandPolygons.push_back(std::move(polygon));
    }
    return result;
}

}  // namespace cad
