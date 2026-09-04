#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cad/db/Entity.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/LineSegment.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

inline constexpr const char* kHatchPatternSolid = "SOLID";

/// One edge of a hatch boundary.
///
/// A tagged struct rather than the kernel's `Curve` hierarchy, because a
/// boundary must survive a round trip *exactly*. A spline edge that came back
/// as a polyline would be a silently different drawing, and this application
/// does not yet have a NURBS curve to hold it — so the spline's own data is
/// carried through untouched and only its *display* is approximated.
struct HatchEdge {
    enum class Kind : std::uint8_t { Line = 1, Arc = 2, Ellipse = 3, Spline = 4 };

    Kind kind = Kind::Line;

    /// Line.
    Vec3 start;
    Vec3 end;

    /// Arc and ellipse: centre, and the sweep in radians.
    Vec3 center;
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = kTau;
    /// DXF group 73. A clockwise edge is stored with its angles in the order
    /// the boundary runs, which is the opposite of the kernel's convention.
    bool counterClockwise = true;

    /// Ellipse: vector from the centre to the major-axis endpoint, and the
    /// minor/major ratio.
    Vec3 majorAxis = Vec3::unitX();
    double ratio = 1.0;

    /// Spline, preserved verbatim.
    int degree = 3;
    bool rational = false;
    bool periodic = false;
    std::vector<double> knots;
    std::vector<Vec3> controlPoints;
    std::vector<double> weights;
    /// Points the spline passes through. Present far more often than not, and
    /// the only part that can be drawn without a NURBS evaluator.
    std::vector<Vec3> fitPoints;
    Vec3 startTangent;
    Vec3 endTangent;

    [[nodiscard]] static HatchEdge line(const Vec3& from, const Vec3& to);
    [[nodiscard]] static HatchEdge arc(const Vec3& centerPoint, double radiusValue, double start,
                                       double end, bool ccw = true);

    [[nodiscard]] Vec3 startPoint() const;
    [[nodiscard]] Vec3 endPoint() const;

    /// This edge as a kernel curve, or null for a spline with too little data
    /// to approximate.
    [[nodiscard]] CurvePtr toCurve() const;
};

/// Flags on a boundary path. Matches DXF group 92.
struct HatchPathFlags {
    /// Defines the outside of the hatch rather than an island.
    bool isExternal = false;
    /// Stored as a polyline rather than as a list of edges — the common case,
    /// and a different DXF encoding rather than different geometry.
    bool isPolyline = false;
    /// Derived from the geometry it hatches rather than drawn by the user.
    bool isDerived = false;
    /// The boundary of a piece of text, which hatch fill flows around.
    bool isTextbox = false;
    /// No island lies outside this one.
    bool isOutermost = false;

    [[nodiscard]] static HatchPathFlags fromDxf(int flags) noexcept;
    [[nodiscard]] int toDxf() const noexcept;
};

/// One closed loop of a hatch boundary.
struct HatchBoundaryPath {
    HatchPathFlags flags;

    /// Used when `flags.isPolyline`. Bulges are honoured, so a polyline
    /// boundary with curved spans is exact rather than faceted.
    std::vector<PolylineVertex> vertices;
    bool closed = true;

    /// Used otherwise.
    std::vector<HatchEdge> edges;

    /// Handles of the entities this path was derived from, for associativity.
    /// Preserved so an associative hatch is not silently made static.
    std::vector<Handle> sourceHandles;

    /// The loop as kernel curves, in order.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const;

    /// The loop as a closed point ring, for filling and point-in-polygon.
    [[nodiscard]] std::vector<Vec3> toPolygon(const TessellationOptions& options = {}) const;

    [[nodiscard]] Box3 bounds() const;
};

/// One line family in a hatch pattern definition.
///
/// A pattern is a set of these, each an infinite family of parallel dashed
/// lines. `offset` is expressed in the line's own rotated frame: its x shifts
/// successive lines *along* themselves, which is what makes a brick pattern
/// stagger, and its y is the perpendicular spacing.
struct HatchPatternLine {
    double angle = 0.0;
    Vec3 basePoint;
    Vec3 offset{0.0, 1.0, 0.0};
    /// Positive is pen down, negative is pen up, zero is a dot.
    std::vector<double> dashes;

    [[nodiscard]] double patternLength() const;
};

/// A hatch pattern.
struct HatchPattern {
    std::string name = kHatchPatternSolid;
    /// Solid fill has no line definition at all; it is a different thing
    /// drawn a different way, not a pattern with very close lines.
    bool isSolid = true;
    double angle = 0.0;
    double scale = 1.0;
    /// Draw the pattern again rotated 90 degrees, for cross-hatching.
    bool isDouble = false;
    std::vector<HatchPatternLine> lines;
};

/// How islands inside the boundary are treated. Matches DXF group 75.
enum class HatchStyle : std::uint8_t {
    /// Alternate filled and unfilled, outermost inward.
    Normal = 0,
    /// Fill only between the outer boundary and the first islands.
    Outer = 1,
    /// Fill everything inside the outer boundary.
    Ignore = 2,
};

/// A gradient fill.
///
/// Stored so a gradient-filled drawing round-trips. Drawn as a flat fill in
/// the first colour for now — a wrong-looking fill is recoverable, a lost one
/// is not.
struct HatchGradient {
    bool enabled = false;
    std::string name = "LINEAR";
    double angle = 0.0;
    /// 0 shades between the two colours, 1 tints one colour toward white.
    bool isSingleColor = false;
    /// How far toward white a single-colour gradient goes, 0 to 1.
    double tint = 0.0;
    double shift = 0.0;
    Color color1 = Color::fromIndex(5);
    Color color2 = Color::fromIndex(2);
};

/// A filled region — the HATCH entity.
///
/// Its geometry is *derived*: boundary loops plus a pattern definition, from
/// which the drawn lines are generated. Storing the generated lines instead
/// would make a hatch un-editable, unable to follow its boundary, and
/// enormous — a hatched wall section can be tens of thousands of segments at
/// one zoom level and a hundred at another.
///
/// Generation is therefore view-dependent, and `generateLines` takes the
/// clipping bounds so a hatch that extends far outside the visible area costs
/// only what is on screen.
class HatchEntity final : public Entity {
public:
    std::vector<HatchBoundaryPath> paths;
    HatchPattern pattern;
    HatchGradient gradient;
    HatchStyle style = HatchStyle::Normal;

    /// Group 76: 0 for a user-defined pattern, 1 predefined, 2 custom. Read
    /// rather than assumed, and consumed: left unread it was echoed back as
    /// an unknown group, so every hatch went out saying it twice.
    int patternType = 1;

    /// True when the hatch follows the entities it was built from. The link
    /// itself lives in each path's `sourceHandles`.
    bool isAssociative = false;

    /// Points the user picked to seed boundary detection, preserved so
    /// re-deriving the boundary reproduces the same result.
    std::vector<Vec3> seedPoints;

    /// DXF group 47. The size one screen pixel had when the hatch was created,
    /// used by other CAD software to bound generation. Preserved, not relied upon.
    double pixelSize = 0.0;

    [[nodiscard]] std::string objectType() const override { return "Hatch"; }

    [[nodiscard]] bool isSolidFill() const noexcept {
        return pattern.isSolid || gradient.enabled;
    }

    [[nodiscard]] Box3 bounds() const override;

    /// The boundary curves, so the hatch snaps and trims against its own edge.
    ///
    /// The generated fill is deliberately absent: snapping to a hatch line
    /// would be maddening, and there can be a hundred thousand of them.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Picked by area, not by outline — clicking the middle of a filled region
    /// must select it.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    /// True when `p` lies in the filled part of this hatch, honouring the
    /// island style.
    [[nodiscard]] bool contains(const Vec3& p) const;

    /// The closed rings to fill, for a solid or gradient hatch.
    [[nodiscard]] std::vector<std::vector<Vec3>> fillPolygons(
        const TessellationOptions& options = {}) const;

    /// Generate the pattern's lines, clipped to the boundary.
    ///
    /// @param clip only lines crossing this box are produced; pass the
    ///   entity's own bounds for the whole hatch, or the viewport's for what
    ///   is on screen.
    /// @param maxSegments hard cap. A pattern scaled to near zero — which a
    ///   corrupt or badly imported file really does contain — would otherwise
    ///   generate segments until memory ran out.
    [[nodiscard]] std::vector<LineSegment> generateLines(const Box3& clip,
                                                         std::size_t maxSegments = 200000) const;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;
};

/// The built-in pattern definitions, by name.
///
/// A small library rather than a full `acad.pat` parser: ANSI31 and friends
/// cover most drawings, and a pattern that is not known is drawn from whatever
/// definition the file itself carried — which HATCH always includes.
[[nodiscard]] std::optional<HatchPattern> standardHatchPattern(std::string_view name);

}  // namespace cad
