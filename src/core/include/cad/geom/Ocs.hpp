#pragma once

#include "cad/math/Mat4.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// Object Coordinate System (OCS) support.
///
/// DWG/DXF store many planar entities — arcs, circles, 2D polylines, text,
/// hatches, solids — in a local 2D coordinate system defined solely by an
/// extrusion direction (the "210" group code). The mapping from that normal to
/// a full coordinate frame is fixed by other CAD software's *arbitrary axis algorithm*.
///
/// Implementing this correctly is not optional for DWG fidelity: entities drawn
/// on a mirrored or rotated UCS carry a non-Z extrusion, and treating their
/// stored coordinates as world coordinates places them in the wrong location
/// and with the wrong handedness. Getting it right early is far cheaper than
/// retrofitting it once the renderer, snaps and editing commands all assume
/// world coordinates.

struct CoordinateFrame {
    Vec3 xAxis;
    Vec3 yAxis;
    Vec3 zAxis;
};

/// Threshold from other CAD software specification. When both Nx and Ny are below
/// 1/64, the world Y axis is used as the reference instead of world Z.
inline constexpr double kArbitraryAxisLimit = 1.0 / 64.0;

/// Derive the OCS basis from an extrusion direction, per other CAD software's arbitrary
/// axis algorithm. `normal` need not be normalized.
[[nodiscard]] CoordinateFrame arbitraryAxis(const Vec3& normal) noexcept;

/// True for the overwhelmingly common case of a +Z extrusion.
[[nodiscard]] constexpr bool isWorldNormal(const Vec3& normal) noexcept {
    return normal.x == 0.0 && normal.y == 0.0 && normal.z > 0.0;
}

/// Transform taking OCS coordinates to world coordinates.
/// Returns the identity for a +Z normal.
[[nodiscard]] Mat4 ocsToWorld(const Vec3& normal) noexcept;

/// Inverse of `ocsToWorld`. The OCS basis is orthonormal, so this is the
/// transpose.
[[nodiscard]] Mat4 worldToOcs(const Vec3& normal) noexcept;

/// Convert a point stored in OCS to world coordinates.
///
/// OCS points carry their elevation in `z`, measured along the extrusion
/// direction, which the full basis handles naturally.
[[nodiscard]] Vec3 pointOcsToWorld(const Vec3& p, const Vec3& normal) noexcept;

[[nodiscard]] Vec3 pointWorldToOcs(const Vec3& p, const Vec3& normal) noexcept;

}  // namespace cad
