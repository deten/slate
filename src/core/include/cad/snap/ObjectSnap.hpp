#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "cad/db/Handle.hpp"
#include "cad/geom/Curve.hpp"

namespace cad {

/// Object snap modes.
///
/// The bit values match the `OSMODE` setting in other CAD software system variable, so a drawing's
/// stored snap configuration transfers exactly rather than needing a mapping
/// that would drift.
enum class SnapMode : std::uint32_t {
    None = 0,
    Endpoint = 1u << 0,
    Midpoint = 1u << 1,
    Center = 1u << 2,
    Node = 1u << 3,
    Quadrant = 1u << 4,
    Intersection = 1u << 5,
    Insertion = 1u << 6,
    Perpendicular = 1u << 7,
    Tangent = 1u << 8,
    Nearest = 1u << 9,
    /// Explicitly suppress snapping.
    Clear = 1u << 10,
    ApparentIntersection = 1u << 11,
    Extension = 1u << 12,
    Parallel = 1u << 13,
};

[[nodiscard]] constexpr std::uint32_t toMask(SnapMode mode) noexcept {
    return static_cast<std::uint32_t>(mode);
}

[[nodiscard]] constexpr bool hasMode(std::uint32_t modes, SnapMode mode) noexcept {
    return (modes & toMask(mode)) != 0;
}

/// Every snap that can be enabled, in the order they appear in the UI.
[[nodiscard]] std::span<const SnapMode> allSnapModes() noexcept;

/// Human-readable name, for the tooltip and status bar.
[[nodiscard]] std::string_view snapModeName(SnapMode mode) noexcept;

/// A candidate snap position.
struct SnapPoint {
    Vec3 position;
    SnapMode mode = SnapMode::None;
    /// Entity the snap belongs to; null for geometry not in the database.
    Handle entityHandle;
    /// Distance from the cursor, in world units.
    double distance = 0.0;
    /// Priority for resolving ties. Higher wins.
    int priority = 0;
};

/// A pickable entity paired with its world-space curves.
///
/// The engine takes curves rather than entities so it can also snap to preview
/// geometry that is not in the database at all.
struct SnapCandidate {
    Handle handle;
    std::span<const CurvePtr> curves;
    /// Placement point, for entities that have one (blocks, text, points).
    /// Drives the INSERTION and NODE snaps.
    const Vec3* insertionPoint = nullptr;
    /// True when this candidate is a POINT entity, for the NODE snap.
    bool isNode = false;
};

struct SnapQuery {
    /// Cursor position in world coordinates.
    Vec3 cursor;
    /// Search radius in world units, derived from the pixel aperture and the
    /// current zoom. Passing world units keeps the engine independent of the
    /// view.
    double aperture = 1.0;
    /// Bitmask of enabled modes.
    std::uint32_t modes = 0;
    /// Entities within range, supplied by the spatial index.
    std::span<const SnapCandidate> candidates;
    /// Reference point for snaps that need one: PERPENDICULAR and TANGENT are
    /// meaningless without knowing where the line is being drawn *from*.
    const Vec3* basePoint = nullptr;
    Tolerance tolerance = defaultTolerance();
};

/// Computes object snap positions.
///
/// Object snaps are the single largest contributor to whether a CAD program
/// feels precise or approximate. Three requirements shape this implementation:
///
///  - **Deterministic priority.** Ties resolve by snap specificity, never by
///    floating-point luck, so the same cursor position always gives the same
///    snap. Without it, moving near a line's end flickers between Endpoint and
///    Nearest.
///  - **Bounded work.** Intersection snapping is quadratic in candidate curves,
///    so the candidate set arrives pre-filtered from the spatial index and the
///    pairwise search is capped.
///  - **No database access.** Candidates are passed in, which keeps this pure
///    and testable.
class SnapEngine {
public:
    /// Find the best snap for a cursor position; empty when nothing is in range.
    [[nodiscard]] std::optional<SnapPoint> findBest(const SnapQuery& query) const;

    /// Every snap within the aperture, unsorted.
    [[nodiscard]] std::vector<SnapPoint> findAll(const SnapQuery& query) const;

    /// Cap on pairwise intersection tests per query. Generous relative to what
    /// fits in a snap aperture, but prevents a pick in dense geometry from
    /// stalling the cursor.
    int maxIntersectionPairs = 400;
};

/// Convert a pixel aperture to world units at a given zoom.
[[nodiscard]] constexpr double apertureInWorld(double pixels, double unitsPerPixel) noexcept {
    return pixels * unitsPerPixel;
}

}  // namespace cad
