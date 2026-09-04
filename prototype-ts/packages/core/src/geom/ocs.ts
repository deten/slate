import { Vec3 } from '../math/vec3.js';
import { Mat4 } from '../math/mat4.js';

/**
 * Object Coordinate System (OCS) support.
 *
 * DWG/DXF store many planar entities — arcs, circles, 2D polylines, text,
 * hatches, solids — in a local 2D coordinate system defined solely by an
 * extrusion direction (the "210" group code). The mapping from that normal to
 * a full coordinate frame is fixed by other CAD software's *arbitrary axis algorithm*.
 *
 * Implementing this correctly is not optional for DWG fidelity: entities drawn
 * on a mirrored or rotated UCS carry a non-Z extrusion, and treating their
 * stored coordinates as world coordinates places them in the wrong location
 * and with the wrong handedness. Getting this right early is far cheaper than
 * retrofitting it once the renderer, snaps and editing commands all assume
 * world coordinates.
 */

/**
 * Threshold from other CAD software specification. When both Nx and Ny are below
 * 1/64, the world Y axis is used as the reference instead of world Z.
 */
const ARBITRARY_AXIS_LIMIT = 1 / 64;

export interface CoordinateFrame {
  readonly xAxis: Vec3;
  readonly yAxis: Vec3;
  readonly zAxis: Vec3;
}

/**
 * Derive the OCS basis from an extrusion direction, per other CAD software's arbitrary
 * axis algorithm.
 *
 * @param normal extrusion direction; need not be normalized.
 */
export function arbitraryAxis(normal: Vec3): CoordinateFrame {
  const n = normal.normalizeOrNull() ?? Vec3.UNIT_Z;
  const reference =
    Math.abs(n.x) < ARBITRARY_AXIS_LIMIT && Math.abs(n.y) < ARBITRARY_AXIS_LIMIT
      ? Vec3.UNIT_Y
      : Vec3.UNIT_Z;
  const xAxis = reference.cross(n).normalize();
  const yAxis = n.cross(xAxis).normalize();
  return { xAxis, yAxis, zAxis: n };
}

/** `true` for the overwhelmingly common case of a +Z extrusion. */
export function isWorldNormal(normal: Vec3): boolean {
  return normal.x === 0 && normal.y === 0 && normal.z > 0;
}

/**
 * Transform taking OCS coordinates to world coordinates for the given
 * extrusion direction. Returns the identity for a +Z normal.
 */
export function ocsToWorld(normal: Vec3): Mat4 {
  if (isWorldNormal(normal)) return Mat4.IDENTITY;
  const { xAxis, yAxis, zAxis } = arbitraryAxis(normal);
  return Mat4.fromBasis(xAxis, yAxis, zAxis);
}

/** Inverse of {@link ocsToWorld}. The OCS basis is orthonormal, so this is the transpose. */
export function worldToOcs(normal: Vec3): Mat4 {
  if (isWorldNormal(normal)) return Mat4.IDENTITY;
  return ocsToWorld(normal).transpose();
}

/**
 * Convert a point stored in OCS to world coordinates.
 *
 * OCS points carry their elevation in `z`, measured along the extrusion
 * direction, which this handles naturally via the full basis.
 */
export function pointOcsToWorld(p: Vec3, normal: Vec3): Vec3 {
  if (isWorldNormal(normal)) return p;
  const { xAxis, yAxis, zAxis } = arbitraryAxis(normal);
  return xAxis.scale(p.x).add(yAxis.scale(p.y)).add(zAxis.scale(p.z));
}

export function pointWorldToOcs(p: Vec3, normal: Vec3): Vec3 {
  if (isWorldNormal(normal)) return p;
  const { xAxis, yAxis, zAxis } = arbitraryAxis(normal);
  return new Vec3(p.dot(xAxis), p.dot(yAxis), p.dot(zAxis));
}
