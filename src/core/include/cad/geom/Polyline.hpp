#pragma once

#include <vector>

#include "cad/geom/Curve.hpp"

namespace cad {

/// One vertex of a polyline.
///
/// `bulge` describes the arc from *this* vertex to the next, as
/// `tan(includedAngle / 4)` — the DWG/DXF representation. Storing it on the
/// vertex rather than deriving arcs eagerly keeps the polyline losslessly
/// round-trippable and keeps vertex editing cheap.
///
/// The widths are carried for fidelity: the renderer honours them and editing
/// preserves them even where commands do not yet manipulate them.
struct PolylineVertex {
    Vec3 position;
    double bulge = 0.0;
    double startWidth = 0.0;
    double endWidth = 0.0;

    PolylineVertex() = default;

    explicit PolylineVertex(const Vec3& p, double bulgeValue = 0.0, double start = 0.0,
                            double end = -1.0)
        : position(p),
          bulge(bulgeValue),
          startWidth(start),
          // A negative sentinel means "same as start", which is the common case
          // and avoids callers repeating the value.
          endWidth(end < 0.0 ? start : end) {}
};

/// A polyline: a connected chain of line and circular-arc segments.
///
/// This is the workhorse of real drawings — most DWG geometry is polylines —
/// so it is a first-class curve rather than a container of separate entities.
///
/// Normalized parameter `t` is *segment-indexed*: `t = i / segmentCount` lands
/// exactly on vertex `i`. That makes vertex-level operations exact and keeps
/// `split` cheap; arc-length queries are available separately via
/// `paramAtLength`.
class Polyline final : public Curve {
public:
    std::vector<PolylineVertex> vertices;
    Vec3 normal = Vec3::unitZ();

    Polyline() = default;

    explicit Polyline(std::vector<PolylineVertex> vertexList, bool closedFlag = false,
                      const Vec3& extrusion = Vec3::unitZ());

    Polyline(const Polyline& other);
    Polyline& operator=(const Polyline& other);
    Polyline(Polyline&&) noexcept = default;
    Polyline& operator=(Polyline&&) noexcept = default;
    ~Polyline() override = default;

    /// Build from bare points, with straight segments throughout.
    [[nodiscard]] static Polyline fromPoints(std::span<const Vec3> points,
                                             bool closedFlag = false);

    [[nodiscard]] static Polyline fromPoints(std::initializer_list<Vec3> points,
                                             bool closedFlag = false) {
        return fromPoints(std::span<const Vec3>{points.begin(), points.size()}, closedFlag);
    }

    /// An axis-aligned rectangle, as produced by the RECTANG command.
    [[nodiscard]] static Polyline rectangle(const Vec3& corner1, const Vec3& corner2);

    [[nodiscard]] CurvePtr clone() const override { return std::make_unique<Polyline>(*this); }

    [[nodiscard]] std::size_t vertexCount() const noexcept { return vertices.size(); }

    /// Number of segments: one fewer than the vertex count when open, equal to
    /// it when closed (the extra segment runs from the last vertex to the first).
    [[nodiscard]] std::size_t segmentCount() const noexcept;

    [[nodiscard]] bool isEmpty() const noexcept { return segmentCount() == 0; }

    /// The individual segments as concrete curves.
    ///
    /// Computed once and memoized: every geometric query needs them, and
    /// rebuilding per call is the easiest way to make a large drawing slow.
    [[nodiscard]] const std::vector<CurvePtr>& segments() const;

    /// Segment at `index`, or nullptr when out of range.
    [[nodiscard]] const Curve* segmentAt(std::size_t index) const;

    [[nodiscard]] bool isClosed() const override { return closed_; }

    /// Set the closed flag, invalidating cached segments.
    void setClosed(bool value);

    [[nodiscard]] Vec3 startPoint() const override;
    [[nodiscard]] Vec3 endPoint() const override;
    [[nodiscard]] double length() const override;

    /// Decompose a normalized parameter into a segment index and local
    /// parameter.
    struct ResolvedParam {
        std::size_t index = 0;
        double local = 0.0;
    };
    [[nodiscard]] ResolvedParam resolveParam(double t) const;

    /// Inverse of `resolveParam`.
    [[nodiscard]] double makeParam(std::size_t index, double local) const;

    [[nodiscard]] Vec3 pointAt(double t) const override;
    [[nodiscard]] Vec3 tangentAt(double t) const override;

    /// Normalized parameter at a given distance along the polyline.
    [[nodiscard]] double paramAtLength(double distance) const;

    /// Distance along the polyline at a normalized parameter.
    [[nodiscard]] double lengthAtParam(double t) const;

    [[nodiscard]] Box3 bounds() const override;

    [[nodiscard]] ClosestPointResult closestPoint(const Vec3& p) const override;

    /// Polylines have no unbounded host geometry, so extension is defined only
    /// against the first and last segments — which is exactly what EXTEND and
    /// the extension snap need.
    [[nodiscard]] std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const override;

    /// Signed area in the XY plane, positive when counter-clockwise.
    ///
    /// The shoelace formula over the vertices plus the circular-segment
    /// contribution of each bulged span — the exact result for a line/arc
    /// polygon, and what the AREA command reports.
    [[nodiscard]] double signedArea() const override;
    [[nodiscard]] bool hasArea() const override { return closed_; }

    [[nodiscard]] double area() const { return std::abs(signedArea()); }

    [[nodiscard]] bool isCounterClockwise() const { return signedArea() > 0.0; }

    /// Point-in-polygon test for closed polylines, using the winding number of
    /// the tessellated boundary.
    [[nodiscard]] bool containsPointInside(const Vec3& p,
                                           const TessellationOptions& options = {}) const;

    [[nodiscard]] CurvePtr transformed(const Mat4& m) const override;
    [[nodiscard]] CurvePtr reversed() const override;

    [[nodiscard]] std::vector<Vec3> tessellate(
        const TessellationOptions& options = {}) const override;

    [[nodiscard]] CurvePtr trimmed(double t0, double t1) const override;

    /// True when any vertex carries a non-zero bulge.
    [[nodiscard]] bool hasArcs() const;

    /// True when any vertex carries a non-zero width.
    [[nodiscard]] bool hasWidth() const;

    [[nodiscard]] bool equals(const Polyline& other,
                              const Tolerance& tol = defaultTolerance()) const;

private:
    bool closed_ = false;

    /// Lazily built segment list. Mutable so the const query methods that need
    /// it can populate it; the memo is not observable state.
    mutable std::vector<CurvePtr> cachedSegments_;
    mutable std::vector<double> cachedLengths_;
    mutable bool segmentsValid_ = false;

    void invalidate() noexcept;
    void buildSegments() const;
    [[nodiscard]] const std::vector<double>& segmentLengths() const;
};

}  // namespace cad
