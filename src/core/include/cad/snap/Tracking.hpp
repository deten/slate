#pragma once

#include <optional>
#include <span>
#include <vector>

#include "cad/math/Vec3.hpp"

namespace cad {

/// A point being held on alignment paths radiating from acquired points.
struct TrackingAlignment {
    Vec3 point;
    /// The acquired points that produced it: one where the cursor is on a
    /// single path, two where it is at their crossing. Kept because the paths
    /// have to be drawn, and with several points acquired a path on its own
    /// does not say which one it came from.
    std::vector<Vec3> origins;
};

/// Pull `cursor` onto an alignment path from one of the points `acquired`.
///
/// This is the geometry behind other CAD software's object snap tracking. Each acquired
/// point radiates paths at multiples of `angleIncrement`; a cursor within
/// `reach` of one is taken onto it, and a cursor within `reach` of where two
/// of them cross is taken to the crossing.
///
/// The crossing is the case worth having. A point level with one corner and
/// directly above another has no geometry marking it, and finding it any other
/// way means drawing construction lines and rubbing them out again.
///
/// Returns nothing when the cursor is on no path, so the caller can fall back
/// to whatever constraint would otherwise apply.
///
/// `reach` is in drawing units and is normally the pick aperture converted from
/// pixels, so the pull feels the same size at every zoom.
[[nodiscard]] std::optional<TrackingAlignment> alignToAcquired(std::span<const Vec3> acquired,
                                                               const Vec3& cursor,
                                                               double angleIncrement,
                                                               double reach);

}  // namespace cad
