import { DbObject } from './db-object.js';
import { NULL_HANDLE, type Handle } from './handle.js';
import { Color, Lineweight, type Transparency } from './color.js';
import { Vec3 } from '../math/vec3.js';
import type { Mat4 } from '../math/mat4.js';
import type { Box3 } from '../math/box3.js';
import type { Curve } from '../geom/curve.js';

/**
 * Fully resolved display properties for an entity.
 *
 * Produced by walking the ByLayer/ByBlock inheritance chain. The renderer
 * consumes this and never inspects raw entity properties, which keeps
 * inheritance logic in exactly one place.
 */
export interface ResolvedProperties {
  readonly rgb: number;
  readonly linetypeHandle: Handle;
  readonly linetypeScale: number;
  /** Hundredths of a millimetre; never a negative sentinel. */
  readonly lineweight: number;
  /** 0-255, 255 fully opaque. */
  readonly alpha: number;
  readonly visible: boolean;
}

/**
 * The context an entity's properties resolve against.
 *
 * When an entity is drawn inside a block reference, ByBlock properties come
 * from the *reference*, not the block definition — so the same block can look
 * different at two insertion points. Passing this context explicitly, rather
 * than storing resolved values on the entity, is what makes that work.
 */
export interface ResolutionContext {
  /** Layer of the entity being resolved. */
  readonly layerColor: Color;
  readonly layerLinetypeHandle: Handle;
  readonly layerLineweight: number;
  readonly layerTransparency: Transparency;
  readonly layerVisible: boolean;
  /** Properties of the enclosing block reference, when nested. */
  readonly blockColor?: Color;
  readonly blockLinetypeHandle?: Handle;
  readonly blockLineweight?: number;
  /** Global LTSCALE system variable. */
  readonly globalLinetypeScale: number;
}

/**
 * Base class for every drawable object.
 *
 * Entities hold *presentation* state (layer, colour, linetype) and *geometric*
 * state (defined by the subclass). The split matters: presentation is uniform
 * across all entity types and handled here once, while geometry varies and is
 * delegated.
 *
 * Every entity can expose its geometry as {@link Curve}s via {@link toCurves}.
 * That single method is what lets snapping, trimming, filleting, offsetting and
 * selection work on *any* entity — including ones added later by a plugin —
 * without those systems knowing the entity's concrete type.
 */
export abstract class Entity extends DbObject {
  /** Handle of the layer this entity belongs to. */
  layerHandle: Handle = NULL_HANDLE;

  /** Entity colour, usually ByLayer. */
  color: Color = Color.BY_LAYER;

  /** Handle of the linetype record; the null handle means ByLayer. */
  linetypeHandle: Handle = NULL_HANDLE;

  /** Per-entity linetype scale, multiplied with the global LTSCALE. */
  linetypeScale = 1;

  /** Lineweight, or one of the negative {@link Lineweight} sentinels. */
  lineweight: number = Lineweight.ByLayer;

  /** Per-entity transparency; `null` inherits from the layer. */
  transparency: Transparency = null;

  /** Explicitly hidden, independent of layer visibility. */
  visible = true;

  /**
   * Extrusion direction defining this entity's OCS. Almost always +Z; a
   * different value means the entity was drawn on a rotated or mirrored UCS
   * and its stored coordinates are not world coordinates.
   */
  normal: Vec3 = Vec3.UNIT_Z;

  /**
   * Elevation along the normal. DWG stores this separately from the entity's
   * points for planar entities.
   */
  elevation = 0;

  /** Handle of the material, preserved for fidelity. */
  materialHandle: Handle | null = null;

  /**
   * Axis-aligned world-space extents.
   *
   * Must be tight enough for spatial indexing to be effective but is allowed
   * to be conservative where an exact bound is expensive (text, for instance,
   * before fonts are measured).
   */
  abstract get bounds(): Box3;

  /**
   * Apply a transform to this entity's geometry, in place.
   *
   * In-place because MOVE on a 50,000-entity selection must not allocate
   * 50,000 replacement entities; the transaction layer has already snapshotted
   * anything that needs undoing.
   */
  abstract transformBy(m: Mat4): void;

  /**
   * This entity's geometry as curves, in world coordinates.
   *
   * Entities with no curve geometry (text, points, block references before
   * explosion) return an empty array. Entities made of several curves — a
   * polyline is one curve, but a dimension is many — return all of them.
   */
  abstract toCurves(): Curve[];

  /**
   * Shortest distance from a world point to this entity's geometry, or
   * `Infinity` when the entity cannot be picked geometrically.
   *
   * The default walks {@link toCurves}, which is correct for every
   * curve-backed entity; types with special pick behaviour (text, hatches,
   * which are picked by their bounding area) override it.
   */
  distanceToPoint(p: Vec3): number {
    let best = Infinity;
    for (const curve of this.toCurves()) {
      const d = curve.distanceTo(p);
      if (d < best) best = d;
    }
    return best;
  }

  /**
   * Resolve display properties through the ByLayer/ByBlock chain.
   *
   * Resolution order for each property is: explicit entity value, then the
   * block reference's value when the entity value is ByBlock, then the layer's
   * value when it is ByLayer. This mirrors other CAD software exactly, including the
   * subtlety that ByBlock outside a block reference falls back to the layer.
   */
  resolveProperties(context: ResolutionContext): ResolvedProperties {
    return {
      rgb: this.resolveColor(context).toRgb(),
      linetypeHandle: this.resolveLinetype(context),
      linetypeScale: this.linetypeScale * context.globalLinetypeScale,
      lineweight: this.resolveLineweight(context),
      alpha: this.resolveAlpha(context),
      visible: this.visible && context.layerVisible,
    };
  }

  private resolveColor(context: ResolutionContext): Color {
    if (this.color.isConcrete) return this.color;
    if (this.color.isByBlock) {
      const fromBlock = context.blockColor;
      // ByBlock outside any block reference behaves as ByLayer.
      if (fromBlock === undefined) return context.layerColor;
      return fromBlock.isConcrete ? fromBlock : context.layerColor;
    }
    return context.layerColor;
  }

  private resolveLinetype(context: ResolutionContext): Handle {
    if (this.linetypeHandle !== NULL_HANDLE) return this.linetypeHandle;
    const fromBlock = context.blockLinetypeHandle;
    if (fromBlock !== undefined && fromBlock !== NULL_HANDLE) return fromBlock;
    return context.layerLinetypeHandle;
  }

  private resolveLineweight(context: ResolutionContext): number {
    if (this.lineweight >= 0) return this.lineweight;
    if (this.lineweight === Lineweight.ByBlock) {
      const fromBlock = context.blockLineweight;
      if (fromBlock !== undefined && fromBlock >= 0) return fromBlock;
    }
    const layer = context.layerLineweight;
    // Lineweight "Default" resolves to the LWDEFAULT system variable, 25/100mm.
    return layer >= 0 ? layer : 25;
  }

  private resolveAlpha(context: ResolutionContext): number {
    if (this.transparency !== null) return this.transparency;
    return context.layerTransparency ?? 255;
  }

  protected copyEntityInto(target: Entity): void {
    this.copyBaseInto(target);
    target.layerHandle = this.layerHandle;
    target.color = this.color;
    target.linetypeHandle = this.linetypeHandle;
    target.linetypeScale = this.linetypeScale;
    target.lineweight = this.lineweight;
    target.transparency = this.transparency;
    target.visible = this.visible;
    target.normal = this.normal;
    target.elevation = this.elevation;
    target.materialHandle = this.materialHandle;
  }

  protected restoreEntityFrom(snapshot: Entity): void {
    this.restoreBaseFrom(snapshot);
    this.layerHandle = snapshot.layerHandle;
    this.color = snapshot.color;
    this.linetypeHandle = snapshot.linetypeHandle;
    this.linetypeScale = snapshot.linetypeScale;
    this.lineweight = snapshot.lineweight;
    this.transparency = snapshot.transparency;
    this.visible = snapshot.visible;
    this.normal = snapshot.normal;
    this.elevation = snapshot.elevation;
    this.materialHandle = snapshot.materialHandle;
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    if (this.layerHandle !== NULL_HANDLE) refs.push(this.layerHandle);
    if (this.linetypeHandle !== NULL_HANDLE) refs.push(this.linetypeHandle);
    if (this.materialHandle !== null) refs.push(this.materialHandle);
    return refs;
  }

  abstract override clone(): Entity;
}

/**
 * Grip points for an entity — the small squares a user drags to edit geometry
 * directly, without invoking a command.
 */
export interface GripPoint {
  readonly position: Vec3;
  /** Identifies which part of the entity this grip drives. */
  readonly kind: 'endpoint' | 'midpoint' | 'center' | 'vertex' | 'quadrant' | 'insertion';
  /** Index within a repeated set, e.g. the vertex number on a polyline. */
  readonly index: number;
}

/**
 * Implemented by entities that support direct grip editing.
 *
 * Kept as a separate interface rather than a base-class method so that an
 * entity type can be fully functional before its grip behaviour is written —
 * which keeps partially implemented entity types honest rather than shipping
 * a grip implementation that silently does nothing.
 */
export interface GripEditable {
  getGripPoints(): GripPoint[];
  /** Move one grip to a new position, mutating the entity. */
  moveGrip(grip: GripPoint, to: Vec3): void;
}

export function isGripEditable(entity: Entity): entity is Entity & GripEditable {
  return (
    typeof (entity as Partial<GripEditable>).getGripPoints === 'function' &&
    typeof (entity as Partial<GripEditable>).moveGrip === 'function'
  );
}
