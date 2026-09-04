#pragma once

#include <span>
#include <string>
#include <vector>

#include "cad/geom/Curve.hpp"
#include "cad/math/Tolerance.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// One closed ring found by tracing: the pieces of the input curves that
/// bound it, in order, each running into the next.
struct BoundaryLoop {
    std::vector<CurvePtr> curves;
    /// Which input curve each piece came from, by index into the input span.
    std::vector<std::size_t> sources;
};

struct BoundaryTraceOptions {
    Tolerance tolerance = defaultTolerance();
    /// Whether to look for closed rings inside the region and report them as
    /// islands, so a hatch leaves a column unfilled.
    bool detectIslands = true;
};

struct BoundaryTraceResult {
    /// The ring around the seed point first, then any islands inside it.
    std::vector<BoundaryLoop> loops;
    /// Why nothing was found, in words for the user. Empty on success.
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && !loops.empty(); }
};

/// Find the region that encloses a point, from the curves around it.
///
/// This is what picking an empty spot inside a room and asking for a hatch
/// needs: the walls around it, in whatever objects they happen to be drawn as,
/// traced into a ring. The curves are split where they cross, so lines that
/// run past a corner still make one; and a line that dangles into the region
/// without bounding anything is walked into and back out of, then dropped.
///
/// The ring found is the smallest face containing the seed. Every closed ring
/// lying inside it that does not itself contain the seed is an island.
[[nodiscard]] BoundaryTraceResult traceBoundary(std::span<const Curve* const> curves,
                                                const Vec3& seed,
                                                const BoundaryTraceOptions& options = {});

}  // namespace cad
