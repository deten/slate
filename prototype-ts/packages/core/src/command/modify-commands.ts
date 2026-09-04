import { Vec3 } from '../math/vec3.js';
import { Mat4 } from '../math/mat4.js';
import { EllipseArc } from '../geom/ellipse-arc.js';
import { Arc } from '../geom/arc.js';
import { LineSegment } from '../geom/line-segment.js';
import { Polyline } from '../geom/polyline.js';
import { Layer } from '../db/layer.js';
import { BlockTableRecord } from '../db/block-table-record.js';
import { offsetCurve, trimCurve, extendCurve, filletCurves } from '../geom/operations.js';
import type { Curve } from '../geom/curve.js';
import type { Entity } from '../db/entity.js';
import type { Handle } from '../db/handle.js';
import {
  ArcEntity,
  CircleEntity,
  EllipseEntity,
  LineEntity,
  PolylineEntity,
} from '../db/entities/curve-entities.js';
import {
  CommandTransactionMode,
  step,
  type CommandContext,
  type CommandDefinition,
  type CommandInstance,
  type InputValue,
  type StepResult,
} from './command.js';

/**
 * Shared scaffolding for commands that transform an existing selection.
 *
 * All of MOVE, COPY, ROTATE, SCALE and MIRROR follow the same shape: acquire a
 * selection, gather a few points, then apply a transform. Factoring that out
 * keeps each command to just the part that differs — its transform — and means
 * pickfirst, cancellation and undo behave identically across all of them.
 */
abstract class TransformCommand implements CommandInstance {
  protected selection: Handle[] = [];
  protected basePoint: Vec3 | null = null;

  /** Prompt shown once a base point is known. */
  protected abstract secondPrompt(): StepResult;

  /** Build the transform from the collected input. */
  protected abstract buildTransform(input: InputValue, context: CommandContext): Mat4 | null;

  /** `true` when the command duplicates rather than moves. */
  protected copies = false;

  start(context: CommandContext): StepResult {
    if (context.initialSelection.length > 0) {
      this.selection = [...context.initialSelection];
      return step.request({ kind: 'point', prompt: 'Specify base point' });
    }
    return step.request({
      kind: 'selection',
      prompt: 'Select objects',
      minCount: 1,
      usePickfirst: true,
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel') return step.cancelled();

    if (this.selection.length === 0) {
      if (input.kind !== 'selection' || input.value.length === 0) {
        return step.cancelled('Nothing selected');
      }
      this.selection = [...input.value];
      return step.request({ kind: 'point', prompt: 'Specify base point' });
    }

    if (this.basePoint === null) {
      if (input.kind !== 'point') return step.cancelled();
      this.basePoint = input.value;
      return this.secondPrompt();
    }

    const transform = this.buildTransform(input, context);
    if (transform === null) return step.cancelled();

    const applied = this.apply(transform, context);
    return step.done(`${applied} object(s) modified`);
  }

  /**
   * Apply the transform to every selected entity.
   *
   * Locked layers are skipped rather than silently modified — a lock the
   * application ignores is worse than no lock.
   */
  protected apply(transform: Mat4, context: CommandContext): number {
    let count = 0;

    for (const handle of this.selection) {
      const entity = context.db.getEntity(handle);
      if (entity === undefined || entity.erased) continue;

      const layer = context.db.getTyped(entity.layerHandle, Layer);
      if (layer !== undefined && layer.isLocked) {
        context.warn(`Skipped an object on locked layer "${layer.name}".`);
        continue;
      }

      if (this.copies) {
        const copy = entity.clone();
        copy.handle = '0' as Handle;
        transformEntity(copy, transform, context);
        const owner =
          context.db.getTyped(entity.ownerHandle, BlockTableRecord) ?? context.db.modelSpace;
        context.transaction.addEntity(copy, owner);
      } else {
        context.transaction.modifyEntity(entity, (e) => {
          transformEntity(e, transform, context);
        });
      }
      count++;
    }
    return count;
  }

  onCursorMove(): readonly Entity[] | null {
    return null;
  }
}

/**
 * Apply a transform to an entity, substituting a different entity type when
 * the transform makes the current one unable to represent the result.
 *
 * A circle scaled non-uniformly becomes an ellipse. Silently keeping it a
 * circle would distort the drawing; this replaces the entity so the geometry
 * stays exact.
 */
function transformEntity(entity: Entity, transform: Mat4, context: CommandContext): void {
  const needsEllipse =
    (entity instanceof CircleEntity || entity instanceof ArcEntity) &&
    entity.requiresEllipseAfter(transform);

  if (!needsEllipse) {
    entity.transformBy(transform);
    return;
  }

  const source = entity as CircleEntity | ArcEntity;
  const widened = EllipseArc.fromArc(source.geometry).transform(transform);
  const replacement = EllipseEntity.fromEllipseArc(widened);
  // Carry the presentation properties across so the substitution is invisible
  // apart from the corrected geometry.
  replacement.layerHandle = entity.layerHandle;
  replacement.color = entity.color;
  replacement.linetypeHandle = entity.linetypeHandle;
  replacement.linetypeScale = entity.linetypeScale;
  replacement.lineweight = entity.lineweight;
  replacement.transparency = entity.transparency;
  replacement.visible = entity.visible;

  const owner = context.db.getTyped(entity.ownerHandle, BlockTableRecord) ?? context.db.modelSpace;
  context.transaction.addEntity(replacement, owner);
  context.transaction.erase(entity);
}

/** MOVE — translate a selection by a displacement. */
class MoveCommand extends TransformCommand {
  protected secondPrompt(): StepResult {
    return step.request({
      kind: 'point',
      prompt: 'Specify second point',
      ...(this.basePoint === null ? {} : { basePoint: this.basePoint }),
    });
  }

  protected buildTransform(input: InputValue): Mat4 | null {
    if (input.kind !== 'point' || this.basePoint === null) return null;
    return Mat4.translation(input.value.sub(this.basePoint));
  }
}

/** COPY — duplicate a selection at a displacement. */
class CopyCommand extends MoveCommand {
  protected override copies = true;
}

/** ROTATE — rotate a selection about a base point. */
class RotateCommand extends TransformCommand {
  protected secondPrompt(): StepResult {
    return step.request({
      kind: 'angle',
      prompt: 'Specify rotation angle',
      ...(this.basePoint === null ? {} : { basePoint: this.basePoint }),
    });
  }

  protected buildTransform(input: InputValue): Mat4 | null {
    if (this.basePoint === null) return null;
    const angle =
      input.kind === 'angle' || input.kind === 'number'
        ? input.value
        : input.kind === 'point'
          ? input.value.sub(this.basePoint).angle2
          : null;
    if (angle === null) return null;
    return Mat4.rotationZ(angle, this.basePoint);
  }
}

/** SCALE — scale a selection uniformly about a base point. */
class ScaleCommand extends TransformCommand {
  protected secondPrompt(): StepResult {
    return step.request({
      kind: 'number',
      prompt: 'Specify scale factor',
      defaultValue: 1,
      min: Number.MIN_VALUE,
    });
  }

  protected buildTransform(input: InputValue): Mat4 | null {
    if (this.basePoint === null) return null;
    const factor =
      input.kind === 'number' || input.kind === 'distance'
        ? input.value
        : input.kind === 'point'
          ? this.basePoint.distanceTo(input.value)
          : null;
    if (factor === null || factor === 0) return null;
    return Mat4.scaling(factor, this.basePoint);
  }
}

/**
 * MIRROR — reflect a selection across a line.
 *
 * The originals are kept, which is the non-destructive default. other CAD software offers
 * to erase them; that prompt is deferred rather than half-implemented, since a
 * flag that silently never fires would be worse than its absence.
 */
class MirrorCommand extends TransformCommand {
  protected override copies = true;

  protected secondPrompt(): StepResult {
    return step.request({
      kind: 'point',
      prompt: 'Specify second point of mirror line',
      ...(this.basePoint === null ? {} : { basePoint: this.basePoint }),
    });
  }

  protected buildTransform(input: InputValue): Mat4 | null {
    if (input.kind !== 'point' || this.basePoint === null) return null;
    const direction = input.value.sub(this.basePoint);
    if (direction.isZero()) return null;
    return Mat4.mirror2(this.basePoint, direction);
  }
}

/** ERASE — delete the selection. */
class EraseCommand implements CommandInstance {
  private selection: Handle[] = [];

  start(context: CommandContext): StepResult {
    if (context.initialSelection.length > 0) {
      this.selection = [...context.initialSelection];
      return this.erase(context);
    }
    return step.request({ kind: 'selection', prompt: 'Select objects to erase', minCount: 1 });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'selection' || input.value.length === 0) return step.cancelled();
    this.selection = [...input.value];
    return this.erase(context);
  }

  private erase(context: CommandContext): StepResult {
    let count = 0;
    for (const handle of this.selection) {
      const entity = context.db.getEntity(handle);
      if (entity === undefined || entity.erased) continue;
      const layer = context.db.getTyped(entity.layerHandle, Layer);
      if (layer !== undefined && layer.isLocked) {
        context.warn(`Skipped an object on locked layer "${layer.name}".`);
        continue;
      }
      context.transaction.erase(entity);
      count++;
    }
    if (count === 0) return step.cancelled('Nothing was erased');
    return step.done(`${count} object(s) erased`);
  }
}

/** OFFSET — create a parallel copy at a given distance. */
class OffsetCommand implements CommandInstance {
  private distance: number | null = null;
  private sourceHandle: Handle | null = null;

  start(): StepResult {
    return step.request({
      kind: 'distance',
      prompt: 'Specify offset distance',
      allowNegative: false,
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel' || input.kind === 'empty') {
      return this.distance === null ? step.cancelled() : step.done();
    }

    if (this.distance === null) {
      if (input.kind !== 'distance' && input.kind !== 'number') return step.cancelled();
      if (!(input.value > 0)) return step.failed('Offset distance must be greater than zero.');
      this.distance = input.value;
      return step.request({ kind: 'entity', prompt: 'Select object to offset' });
    }

    if (this.sourceHandle === null) {
      if (input.kind !== 'entity') return step.cancelled();
      this.sourceHandle = input.value;
      return step.request({ kind: 'point', prompt: 'Specify point on side to offset' });
    }

    if (input.kind !== 'point') return step.cancelled();

    const source = context.db.getEntity(this.sourceHandle);
    if (source === undefined) return step.failed('The selected object no longer exists.');

    const curves = source.toCurves();
    const curve = curves[0];
    if (curve === undefined) {
      return step.failed('That object has no geometry that can be offset.');
    }

    // The sign is chosen so the result lands on the side the user clicked.
    const projection = curve.closestPoint(input.value);
    const tangent = curve.tangentAt(projection.param);
    const toSide = input.value.sub(projection.point);
    const signedDistance = tangent.cross2(toSide) >= 0 ? this.distance : -this.distance;

    const offset = offsetCurve(curve, signedDistance);
    if (offset === null) {
      return step.failed('The offset distance is too large for this geometry.');
    }

    const entity = entityFromCurve(offset);
    if (entity === null) return step.failed('The offset result cannot be represented.');
    copyProperties(source, entity);
    context.transaction.addToModelSpace(entity);

    // Offset repeats, so a run of parallel lines is one command.
    this.sourceHandle = null;
    return step.request({
      kind: 'entity',
      prompt: 'Select object to offset',
    });
  }
}

/** TRIM — cut a curve back to its intersections with cutting edges. */
class TrimCommand implements CommandInstance {
  private edges: Handle[] = [];

  start(context: CommandContext): StepResult {
    if (context.initialSelection.length > 0) {
      this.edges = [...context.initialSelection];
      return step.request({ kind: 'point', prompt: 'Select object to trim' });
    }
    return step.request({
      kind: 'selection',
      prompt: 'Select cutting edges',
      minCount: 1,
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel' || input.kind === 'empty') {
      return this.edges.length === 0 ? step.cancelled() : step.done();
    }

    if (this.edges.length === 0) {
      if (input.kind !== 'selection') return step.cancelled();
      this.edges = [...input.value];
      return step.request({ kind: 'point', prompt: 'Select object to trim' });
    }

    if (input.kind !== 'point') return step.done();

    const cutting = this.edgeCurves(context);
    const target = this.pickTarget(input.value, context, cutting);
    if (target === null) {
      context.warn('No object found at that point.');
      return step.request({ kind: 'point', prompt: 'Select object to trim', allowEmpty: true });
    }

    const curve = target.entity.toCurves()[0];
    if (curve === undefined) {
      context.warn('That object cannot be trimmed.');
      return step.request({ kind: 'point', prompt: 'Select object to trim', allowEmpty: true });
    }

    const result = trimCurve(curve, cutting, input.value);
    if (!result.trimmed) {
      context.warn('That object does not cross any cutting edge.');
      return step.request({ kind: 'point', prompt: 'Select object to trim', allowEmpty: true });
    }

    // The trimmed curve may become several pieces, so the original is erased
    // and replaced rather than mutated.
    for (const piece of result.remaining) {
      const entity = entityFromCurve(piece);
      if (entity === null) continue;
      copyProperties(target.entity, entity);
      context.transaction.addToModelSpace(entity);
    }
    context.transaction.erase(target.entity);

    return step.request({ kind: 'point', prompt: 'Select object to trim', allowEmpty: true });
  }

  private edgeCurves(context: CommandContext): Curve[] {
    const curves: Curve[] = [];
    for (const handle of this.edges) {
      const entity = context.db.getEntity(handle);
      if (entity !== undefined) curves.push(...entity.toCurves());
    }
    return curves;
  }

  private pickTarget(
    point: Vec3,
    context: CommandContext,
    exclude: readonly Curve[],
  ): { entity: Entity } | null {
    let best: Entity | null = null;
    let bestDistance = Infinity;
    for (const entity of context.db.entitiesIn(context.db.modelSpace)) {
      if (this.edges.includes(entity.handle)) continue;
      const distance = entity.distanceToPoint(point);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = entity;
      }
    }
    void exclude;
    return best === null ? null : { entity: best };
  }
}

/** EXTEND — lengthen a curve until it meets a boundary. */
class ExtendCommand implements CommandInstance {
  private boundaries: Handle[] = [];

  start(context: CommandContext): StepResult {
    if (context.initialSelection.length > 0) {
      this.boundaries = [...context.initialSelection];
      return step.request({ kind: 'point', prompt: 'Select object to extend' });
    }
    return step.request({ kind: 'selection', prompt: 'Select boundary edges', minCount: 1 });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel' || input.kind === 'empty') {
      return this.boundaries.length === 0 ? step.cancelled() : step.done();
    }

    if (this.boundaries.length === 0) {
      if (input.kind !== 'selection') return step.cancelled();
      this.boundaries = [...input.value];
      return step.request({ kind: 'point', prompt: 'Select object to extend' });
    }

    if (input.kind !== 'point') return step.done();

    const boundaryCurves: Curve[] = [];
    for (const handle of this.boundaries) {
      const entity = context.db.getEntity(handle);
      if (entity !== undefined) boundaryCurves.push(...entity.toCurves());
    }

    let target: Entity | null = null;
    let bestDistance = Infinity;
    for (const entity of context.db.entitiesIn(context.db.modelSpace)) {
      if (this.boundaries.includes(entity.handle)) continue;
      const distance = entity.distanceToPoint(input.value);
      if (distance < bestDistance) {
        bestDistance = distance;
        target = entity;
      }
    }

    if (target === null) {
      context.warn('No object found at that point.');
      return step.request({ kind: 'point', prompt: 'Select object to extend', allowEmpty: true });
    }

    const curve = target.toCurves()[0];
    if (curve === undefined) {
      context.warn('That object cannot be extended.');
      return step.request({ kind: 'point', prompt: 'Select object to extend', allowEmpty: true });
    }

    const extended = extendCurve(curve, boundaryCurves, input.value);
    if (extended === null) {
      context.warn('There is no boundary beyond that end to extend to.');
      return step.request({ kind: 'point', prompt: 'Select object to extend', allowEmpty: true });
    }

    const replacement = entityFromCurve(extended);
    if (replacement !== null) {
      copyProperties(target, replacement);
      context.transaction.addToModelSpace(replacement);
      context.transaction.erase(target);
    }

    return step.request({ kind: 'point', prompt: 'Select object to extend', allowEmpty: true });
  }
}

/** FILLET — round the corner between two lines. */
class FilletCommand implements CommandInstance {
  private radius = 0;
  private radiusSet = false;
  private firstHandle: Handle | null = null;
  private firstPick: Vec3 | null = null;

  start(): StepResult {
    return step.request({
      kind: 'distance',
      prompt: 'Specify fillet radius',
      defaultValue: 0,
      allowNegative: false,
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel') return step.cancelled();

    if (!this.radiusSet) {
      if (input.kind === 'distance' || input.kind === 'number') {
        if (input.value < 0) return step.failed('Fillet radius cannot be negative.');
        this.radius = input.value;
      }
      this.radiusSet = true;
      return step.request({ kind: 'point', prompt: 'Select first object' });
    }

    if (input.kind !== 'point') return step.cancelled();

    const picked = nearestEntity(input.value, context);
    if (picked === null) {
      context.warn('No object found at that point.');
      return step.request({ kind: 'point', prompt: 'Select first object' });
    }

    if (this.firstHandle === null) {
      this.firstHandle = picked.handle;
      this.firstPick = input.value;
      return step.request({ kind: 'point', prompt: 'Select second object' });
    }

    if (picked.handle === this.firstHandle) {
      context.warn('Select a different object for the second edge.');
      return step.request({ kind: 'point', prompt: 'Select second object' });
    }

    const firstEntity = context.db.getEntity(this.firstHandle);
    if (firstEntity === undefined || this.firstPick === null) return step.cancelled();

    const curveA = firstEntity.toCurves()[0];
    const curveB = picked.toCurves()[0];
    if (curveA === undefined || curveB === undefined) {
      return step.failed('Those objects cannot be filleted.');
    }

    const result = filletCurves(curveA, curveB, this.radius, this.firstPick, input.value);
    if (result === null) {
      return step.failed(
        'Cannot fillet those objects with that radius. Currently only line-to-line fillets are supported.',
      );
    }

    if (this.radius > 0) {
      const arcEntity = ArcEntity.fromArc(result.arc);
      copyProperties(firstEntity, arcEntity);
      context.transaction.addToModelSpace(arcEntity);
    }

    for (const [original, trimmed] of [
      [firstEntity, result.trimmedA],
      [picked, result.trimmedB],
    ] as const) {
      const replacement = entityFromCurve(trimmed);
      if (replacement === null) continue;
      copyProperties(original, replacement);
      context.transaction.addToModelSpace(replacement);
      context.transaction.erase(original);
    }

    return step.done();
  }
}

/** UNDO and REDO, which manage their own history rather than creating one. */
class UndoCommand implements CommandInstance {
  start(context: CommandContext): StepResult {
    if (!context.transactions.canUndo) return step.cancelled('Nothing to undo');
    const name = context.transactions.undoName;
    context.transactions.undo();
    return step.done(`Undo ${name ?? ''}`.trim());
  }

  supply(): StepResult {
    return step.done();
  }
}

class RedoCommand implements CommandInstance {
  start(context: CommandContext): StepResult {
    if (!context.transactions.canRedo) return step.cancelled('Nothing to redo');
    const name = context.transactions.redoName;
    context.transactions.redo();
    return step.done(`Redo ${name ?? ''}`.trim());
  }

  supply(): StepResult {
    return step.done();
  }
}

// -------------------------------------------------------------------- utils

/** Nearest entity in model space to a pick point. */
function nearestEntity(point: Vec3, context: CommandContext): Entity | null {
  let best: Entity | null = null;
  let bestDistance = Infinity;
  for (const entity of context.db.entitiesIn(context.db.modelSpace)) {
    const distance = entity.distanceToPoint(point);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = entity;
    }
  }
  return best;
}

/**
 * Wrap a bare curve in the entity type that represents it.
 *
 * Editing operations return geometry, and this converts it back to database
 * objects. Keeping the mapping in one place means every operation produces
 * consistent entity types.
 */
export function entityFromCurve(curve: Curve): Entity | null {
  if (curve instanceof LineSegment) return LineEntity.create(curve.start, curve.end);
  if (curve instanceof Arc) {
    return curve.isFullCircle
      ? CircleEntity.create(curve.center, curve.radius)
      : ArcEntity.fromArc(curve);
  }
  if (curve instanceof EllipseArc) return EllipseEntity.fromEllipseArc(curve);
  if (curve instanceof Polyline) return PolylineEntity.fromPolyline(curve);
  return null;
}

/** Copy presentation properties from one entity to another. */
export function copyProperties(source: Entity, target: Entity): void {
  target.layerHandle = source.layerHandle;
  target.color = source.color;
  target.linetypeHandle = source.linetypeHandle;
  target.linetypeScale = source.linetypeScale;
  target.lineweight = source.lineweight;
  target.transparency = source.transparency;
  target.visible = source.visible;
  target.normal = source.normal;
}

export const MODIFY_COMMANDS: readonly CommandDefinition[] = [
  {
    id: 'cad.modify.move',
    name: 'MOVE',
    aliases: ['M'],
    title: 'Move',
    description: 'Move objects by a displacement',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new MoveCommand(),
  },
  {
    id: 'cad.modify.copy',
    name: 'COPY',
    aliases: ['CO', 'CP'],
    title: 'Copy',
    description: 'Duplicate objects at a displacement',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new CopyCommand(),
  },
  {
    id: 'cad.modify.rotate',
    name: 'ROTATE',
    aliases: ['RO'],
    title: 'Rotate',
    description: 'Rotate objects about a base point',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new RotateCommand(),
  },
  {
    id: 'cad.modify.scale',
    name: 'SCALE',
    aliases: ['SC'],
    title: 'Scale',
    description: 'Scale objects about a base point',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new ScaleCommand(),
  },
  {
    id: 'cad.modify.mirror',
    name: 'MIRROR',
    aliases: ['MI'],
    title: 'Mirror',
    description: 'Reflect objects across a line',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new MirrorCommand(),
  },
  {
    id: 'cad.modify.erase',
    name: 'ERASE',
    aliases: ['E', 'DELETE'],
    title: 'Erase',
    description: 'Delete objects',
    category: 'Modify',
    supportsPickfirst: true,
    repeatable: true,
    create: () => new EraseCommand(),
  },
  {
    id: 'cad.modify.offset',
    name: 'OFFSET',
    aliases: ['O'],
    title: 'Offset',
    description: 'Create a parallel copy at a distance',
    category: 'Modify',
    repeatable: true,
    create: () => new OffsetCommand(),
  },
  {
    id: 'cad.modify.trim',
    name: 'TRIM',
    aliases: ['TR'],
    title: 'Trim',
    description: 'Cut objects back to a cutting edge',
    category: 'Modify',
    repeatable: true,
    create: () => new TrimCommand(),
  },
  {
    id: 'cad.modify.extend',
    name: 'EXTEND',
    aliases: ['EX'],
    title: 'Extend',
    description: 'Lengthen objects to meet a boundary',
    category: 'Modify',
    repeatable: true,
    create: () => new ExtendCommand(),
  },
  {
    id: 'cad.modify.fillet',
    name: 'FILLET',
    aliases: ['F'],
    title: 'Fillet',
    description: 'Round the corner between two objects',
    category: 'Modify',
    repeatable: true,
    create: () => new FilletCommand(),
  },
  {
    id: 'cad.edit.undo',
    name: 'UNDO',
    aliases: ['U'],
    title: 'Undo',
    description: 'Reverse the last operation',
    category: 'Edit',
    transactionMode: CommandTransactionMode.Manual,
    repeatable: false,
    isAvailable: () => true,
    create: () => new UndoCommand(),
  },
  {
    id: 'cad.edit.redo',
    name: 'REDO',
    title: 'Redo',
    description: 'Re-apply the last undone operation',
    category: 'Edit',
    transactionMode: CommandTransactionMode.Manual,
    repeatable: false,
    create: () => new RedoCommand(),
  },
];
