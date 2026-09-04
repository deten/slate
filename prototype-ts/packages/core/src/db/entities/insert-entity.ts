import { Entity, type GripEditable, type GripPoint } from '../entity.js';
import type { DbObject } from '../db-object.js';
import { Vec3 } from '../../math/vec3.js';
import { Mat4 } from '../../math/mat4.js';
import { Box3 } from '../../math/box3.js';
import type { Curve } from '../../geom/curve.js';
import { NULL_HANDLE, type Handle } from '../handle.js';
import { TextEntity } from './text-entities.js';

/**
 * A block reference — the INSERT entity.
 *
 * An insert places a {@link BlockTableRecord}'s contents into a drawing under
 * a transform. It does *not* copy them: one definition, many references, which
 * is what makes blocks memory-efficient and globally editable.
 *
 * Inserts may nest arbitrarily (a block containing inserts of other blocks),
 * and the same mechanism carries external references. Bounds and rendering
 * therefore need the block definition, which the entity cannot reach on its
 * own — the database supplies it. That indirection is deliberate: it keeps the
 * entity free of a back-pointer that undo would have to maintain.
 */
export class InsertEntity extends Entity implements GripEditable {
  /** Handle of the block definition being referenced. */
  blockHandle: Handle = NULL_HANDLE;

  /** Where the block's origin lands, in the containing space. */
  position: Vec3 = Vec3.ZERO;

  /** Per-axis scale. Negative values mirror. */
  scale: Vec3 = new Vec3(1, 1, 1);

  /** Rotation about the normal, in radians. */
  rotation = 0;

  /** Handles of ATTRIB entities attached to this reference. */
  attributeHandles: Handle[] = [];

  /** Column count for a rectangular array insert (MINSERT). */
  columnCount = 1;
  rowCount = 1;
  columnSpacing = 0;
  rowSpacing = 0;

  /**
   * Cached extents of the referenced block's contents, in block coordinates.
   *
   * Recomputing a deeply nested block's extents on every bounds query is a
   * dominant cost on large drawings. The database invalidates this whenever
   * the definition changes; it is never persisted.
   */
  private cachedBlockExtents: Box3 | null = null;

  static create(blockHandle: Handle, position: Vec3): InsertEntity {
    const e = new InsertEntity();
    e.blockHandle = blockHandle;
    e.position = position;
    return e;
  }

  override get objectType(): string {
    return 'Insert';
  }

  /** `true` when this is a MINSERT array rather than a single placement. */
  get isArray(): boolean {
    return this.columnCount > 1 || this.rowCount > 1;
  }

  /**
   * Transform from the block definition's coordinate system into the
   * containing space. Scale, then rotate, then translate — the order DWG
   * defines and the one every nested traversal must use.
   */
  get blockTransform(): Mat4 {
    return Mat4.blockTransform(this.position, this.scale, this.rotation);
  }

  /** Transform for one cell of a MINSERT array. */
  transformForCell(column: number, row: number): Mat4 {
    if (!this.isArray) return this.blockTransform;
    // Array offsets are applied in the *rotated* frame, matching other CAD software.
    const offset = new Vec3(column * this.columnSpacing, row * this.rowSpacing, 0).rotateZ(
      this.rotation,
    );
    return Mat4.translation(this.position.add(offset))
      .multiply(Mat4.rotationZ(this.rotation))
      .multiply(Mat4.scaling(this.scale));
  }

  /** All cell transforms, one per placement. */
  *cellTransforms(): IterableIterator<Mat4> {
    for (let row = 0; row < Math.max(1, this.rowCount); row++) {
      for (let col = 0; col < Math.max(1, this.columnCount); col++) {
        yield this.transformForCell(col, row);
      }
    }
  }

  /**
   * Supply the extents of the referenced block's contents so that
   * {@link bounds} can be computed. Called by the database, which is the only
   * component able to traverse the definition.
   */
  setBlockExtents(extents: Box3 | null): void {
    this.cachedBlockExtents = extents;
  }

  get blockExtents(): Box3 | null {
    return this.cachedBlockExtents;
  }

  /**
   * World extents of this reference.
   *
   * Falls back to a degenerate box at the insertion point when the block's
   * extents are unknown — a reference to a missing xref still has a location,
   * and reporting an empty box there would exclude it from ZOOM EXTENTS and
   * make the problem invisible.
   */
  override get bounds(): Box3 {
    if (this.cachedBlockExtents === null || this.cachedBlockExtents.isEmpty) {
      return Box3.fromPoint(this.position);
    }
    let result = Box3.EMPTY;
    for (const t of this.cellTransforms()) {
      result = result.union(this.cachedBlockExtents.transform(t));
    }
    return result;
  }

  /**
   * Block references expose no curves of their own. Their geometry belongs to
   * the definition and is reached by traversal, not by flattening — flattening
   * here would defeat the point of blocks and break global edits.
   */
  override toCurves(): Curve[] {
    return [];
  }

  override distanceToPoint(p: Vec3): number {
    const b = this.bounds;
    return b.containsPoint(p) ? 0 : b.distanceToPoint(p);
  }

  override transformBy(m: Mat4): void {
    // Compose the new placement rather than transforming the stored scalars
    // independently, so that rotation and mirroring interact correctly.
    const composed = m.multiply(this.blockTransform);
    this.position = composed.translation;
    this.rotation = composed.rotation2;

    // Extract per-axis scale from the composed basis. The X and Y column
    // lengths give the magnitudes; the determinant's sign gives the mirror.
    const e = composed.m;
    const sx = Math.hypot(e[0]!, e[1]!, e[2]!);
    const sy = Math.hypot(e[4]!, e[5]!, e[6]!);
    const sz = Math.hypot(e[8]!, e[9]!, e[10]!);
    this.scale = new Vec3(
      composed.isMirrored ? -sx : sx,
      sy,
      sz === 0 ? this.scale.z : sz,
    );

    const spacingScale = m.scaleFactor2;
    this.columnSpacing *= spacingScale;
    this.rowSpacing *= spacingScale;
  }

  getGripPoints(): GripPoint[] {
    return [{ position: this.position, kind: 'insertion', index: 0 }];
  }

  moveGrip(_grip: GripPoint, to: Vec3): void {
    this.position = to;
  }

  override clone(): InsertEntity {
    const copy = new InsertEntity();
    this.copyEntityInto(copy);
    copy.blockHandle = this.blockHandle;
    copy.position = this.position;
    copy.scale = this.scale;
    copy.rotation = this.rotation;
    copy.attributeHandles = [...this.attributeHandles];
    copy.columnCount = this.columnCount;
    copy.rowCount = this.rowCount;
    copy.columnSpacing = this.columnSpacing;
    copy.rowSpacing = this.rowSpacing;
    copy.cachedBlockExtents = this.cachedBlockExtents;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof InsertEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an insert');
    }
    this.restoreEntityFrom(snapshot);
    this.blockHandle = snapshot.blockHandle;
    this.position = snapshot.position;
    this.scale = snapshot.scale;
    this.rotation = snapshot.rotation;
    this.attributeHandles = [...snapshot.attributeHandles];
    this.columnCount = snapshot.columnCount;
    this.rowCount = snapshot.rowCount;
    this.columnSpacing = snapshot.columnSpacing;
    this.rowSpacing = snapshot.rowSpacing;
    this.cachedBlockExtents = snapshot.cachedBlockExtents;
  }

  override referencedHandles(): Handle[] {
    const refs = super.referencedHandles();
    if (this.blockHandle !== NULL_HANDLE) refs.push(this.blockHandle);
    refs.push(...this.attributeHandles);
    return refs;
  }
}

/**
 * An attribute definition inside a block — ATTDEF.
 *
 * Attribute definitions are the *template*; each INSERT of the block carries
 * {@link AttributeEntity} instances holding the actual values. Modelling them
 * as two types (rather than one with a mode flag) matches DWG and keeps the
 * "definition vs. instance" distinction explicit.
 */
export class AttributeDefinitionEntity extends TextEntity {
  /** Programmatic name used to look the attribute up. */
  tag = '';
  /** Prompt shown when the block is inserted. */
  prompt = '';
  /** Default value. */
  defaultValue = '';

  /** Hidden from display but still stored. */
  isInvisible = false;
  /** Value is fixed and not prompted for. */
  isConstant = false;
  /** Value is verified with a second prompt. */
  isVerifiable = false;
  /** Value is filled in automatically without prompting. */
  isPreset = false;
  /** Multi-line attribute using MTEXT formatting. */
  isMultiline = false;
  /** Field order when prompting. */
  fieldLength = 0;

  override get objectType(): string {
    return 'AttributeDefinition';
  }

  override clone(): AttributeDefinitionEntity {
    const copy = new AttributeDefinitionEntity();
    this.copyTextInto(copy);
    copy.tag = this.tag;
    copy.prompt = this.prompt;
    copy.defaultValue = this.defaultValue;
    copy.isInvisible = this.isInvisible;
    copy.isConstant = this.isConstant;
    copy.isVerifiable = this.isVerifiable;
    copy.isPreset = this.isPreset;
    copy.isMultiline = this.isMultiline;
    copy.fieldLength = this.fieldLength;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof AttributeDefinitionEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an attribute definition');
    }
    this.restoreTextFrom(snapshot);
    this.tag = snapshot.tag;
    this.prompt = snapshot.prompt;
    this.defaultValue = snapshot.defaultValue;
    this.isInvisible = snapshot.isInvisible;
    this.isConstant = snapshot.isConstant;
    this.isVerifiable = snapshot.isVerifiable;
    this.isPreset = snapshot.isPreset;
    this.isMultiline = snapshot.isMultiline;
    this.fieldLength = snapshot.fieldLength;
  }
}

/**
 * An attribute value attached to a block reference — ATTRIB.
 *
 * This is what carries the real data in a professional drawing: door numbers,
 * room names, equipment tags, title block fields. Preserving it accurately is
 * as important as preserving geometry.
 */
export class AttributeEntity extends TextEntity {
  tag = '';
  isInvisible = false;
  isConstant = false;
  isVerifiable = false;
  isPreset = false;
  isMultiline = false;
  fieldLength = 0;

  override get objectType(): string {
    return 'Attribute';
  }

  /** The attribute's value; stored in the inherited text contents. */
  get value(): string {
    return this.contents;
  }

  set value(v: string) {
    this.contents = v;
  }

  override clone(): AttributeEntity {
    const copy = new AttributeEntity();
    this.copyTextInto(copy);
    copy.tag = this.tag;
    copy.isInvisible = this.isInvisible;
    copy.isConstant = this.isConstant;
    copy.isVerifiable = this.isVerifiable;
    copy.isPreset = this.isPreset;
    copy.isMultiline = this.isMultiline;
    copy.fieldLength = this.fieldLength;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof AttributeEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an attribute');
    }
    this.restoreTextFrom(snapshot);
    this.tag = snapshot.tag;
    this.isInvisible = snapshot.isInvisible;
    this.isConstant = snapshot.isConstant;
    this.isVerifiable = snapshot.isVerifiable;
    this.isPreset = snapshot.isPreset;
    this.isMultiline = snapshot.isMultiline;
    this.fieldLength = snapshot.fieldLength;
  }
}
