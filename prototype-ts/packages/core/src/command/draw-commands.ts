import { Vec3 } from '../math/vec3.js';
import { Arc } from '../geom/arc.js';
import { Polyline, polylineVertex, type PolylineVertex } from '../geom/polyline.js';
import {
  ArcEntity,
  CircleEntity,
  LineEntity,
  PointEntity,
  PolylineEntity,
} from '../db/entities/curve-entities.js';
import {
  CommandTransactionMode,
  step,
  type CommandContext,
  type CommandDefinition,
  type CommandInstance,
  type CommandKeyword,
  type InputRequest,
  type InputValue,
  type StepResult,
} from './command.js';
import type { Entity } from '../db/entity.js';

/** Keywords shared by the multi-point drawing commands. */
const UNDO_KEYWORD: CommandKeyword = {
  name: 'Undo',
  shortcut: 'U',
  description: 'Remove the last point',
};
const CLOSE_KEYWORD: CommandKeyword = {
  name: 'Close',
  shortcut: 'C',
  description: 'Close back to the first point',
};

/**
 * LINE — draw a connected chain of line segments.
 *
 * Continues prompting after each point, as other CAD software does, so drawing a
 * polygon outline is one command rather than several. Each segment is a
 * separate LINE entity, which is what distinguishes it from PLINE.
 */
class LineCommand implements CommandInstance {
  private points: Vec3[] = [];
  private created: Entity[] = [];

  start(): StepResult {
    return step.request({ kind: 'point', prompt: 'Specify first point' });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'empty' || input.kind === 'cancel') {
      // Enter and Escape both end the chain, keeping the segments already
      // committed — matching other CAD software, where Escape abandons the rubber-band
      // segment in progress, not the ones already drawn. With fewer than two
      // points nothing was committed, so it is a cancel rather than a finish.
      return this.created.length > 0
        ? step.done(`${this.created.length} line(s) drawn`)
        : step.cancelled();
    }

    if (input.kind === 'keyword') {
      return this.handleKeyword(input.value, context);
    }

    if (input.kind !== 'point') {
      return step.request(this.nextPrompt());
    }

    this.points.push(input.value);
    if (this.points.length >= 2) {
      const from = this.points[this.points.length - 2]!;
      const to = this.points[this.points.length - 1]!;
      // A zero-length segment is a mis-click, not geometry worth creating.
      if (!from.equals(to)) {
        const line = LineEntity.create(from, to);
        context.transaction.addToModelSpace(line);
        this.created.push(line);
      }
    }
    return step.request(this.nextPrompt());
  }

  private handleKeyword(keyword: string, context: CommandContext): StepResult {
    const upper = keyword.toUpperCase();
    if (upper.startsWith('C')) {
      if (this.points.length < 3) {
        context.warn('At least three points are needed to close.');
        return step.request(this.nextPrompt());
      }
      const last = this.points[this.points.length - 1]!;
      const first = this.points[0]!;
      if (!last.equals(first)) {
        const line = LineEntity.create(last, first);
        context.transaction.addToModelSpace(line);
        this.created.push(line);
      }
      return step.done(`${this.created.length} line(s) drawn`);
    }

    if (upper.startsWith('U')) {
      const removed = this.created.pop();
      if (removed !== undefined) {
        context.transaction.erase(removed);
        this.points.pop();
      }
      return step.request(this.nextPrompt());
    }

    return step.request(this.nextPrompt());
  }

  private nextPrompt(): InputRequest {
    return buildPointRequest(this.points);
  }

  onCursorMove(world: Vec3): readonly Entity[] | null {
    const last = this.points[this.points.length - 1];
    if (last === undefined) return null;
    return [LineEntity.create(last, world)];
  }
}

function buildPointRequest(points: readonly Vec3[]): InputRequest {
  const last = points[points.length - 1];
  const keywords =
    points.length >= 3 ? [CLOSE_KEYWORD, UNDO_KEYWORD] : points.length >= 1 ? [UNDO_KEYWORD] : [];
  return {
    kind: 'point' as const,
    prompt: points.length === 0 ? 'Specify first point' : 'Specify next point',
    ...(last === undefined ? {} : { basePoint: last }),
    keywords,
    allowEmpty: points.length >= 1,
  };
}

/**
 * PLINE — draw a polyline as a single entity.
 *
 * Distinct from LINE because the result is one object: it can be offset as a
 * unit, carries width, and reports an enclosed area.
 */
class PolylineCommand implements CommandInstance {
  private vertices: PolylineVertex[] = [];
  private entity: PolylineEntity | null = null;

  start(): StepResult {
    return step.request({ kind: 'point', prompt: 'Specify start point' });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'empty' || input.kind === 'cancel') {
      // As with LINE, Escape keeps what has already been committed.
      return this.finish(false, context);
    }
    if (input.kind === 'keyword') {
      const upper = input.value.toUpperCase();
      if (upper.startsWith('C')) return this.finish(true, context);
      if (upper.startsWith('U')) {
        this.vertices.pop();
        this.sync(context);
        return step.request(this.nextPrompt());
      }
      return step.request(this.nextPrompt());
    }
    if (input.kind !== 'point') return step.request(this.nextPrompt());

    this.vertices.push(polylineVertex(input.value));
    this.sync(context);
    return step.request(this.nextPrompt());
  }

  /**
   * Create or update the entity as points accumulate.
   *
   * The entity is created on the second point and mutated thereafter, so the
   * drawing always reflects what the user has committed — and a cancel rolls
   * back the whole thing through the transaction, not through bookkeeping here.
   */
  private sync(context: CommandContext): void {
    if (this.vertices.length < 2) return;
    if (this.entity === null) {
      const entity = new PolylineEntity();
      entity.vertices = [...this.vertices];
      context.transaction.addToModelSpace(entity);
      this.entity = entity;
      return;
    }
    const entity = this.entity;
    context.transaction.modifyEntity(entity, (e) => {
      e.vertices = [...this.vertices];
    });
  }

  private finish(close: boolean, context: CommandContext): StepResult {
    if (this.entity === null || this.vertices.length < 2) {
      return step.cancelled();
    }
    if (close) {
      const entity = this.entity;
      context.transaction.modifyEntity(entity, (e) => {
        e.closed = true;
      });
    }
    return step.done(`Polyline with ${this.vertices.length} vertices`);
  }

  private nextPrompt(): InputRequest {
    return buildPointRequest(this.vertices.map((v) => v.position));
  }

  onCursorMove(world: Vec3): readonly Entity[] | null {
    const last = this.vertices[this.vertices.length - 1];
    if (last === undefined) return null;
    return [LineEntity.create(last.position, world)];
  }
}

/**
 * CIRCLE — centre and radius, with the alternative construction methods
 * other CAD software offers.
 */
class CircleCommand implements CommandInstance {
  private mode: 'centerRadius' | 'twoPoint' | 'threePoint' = 'centerRadius';
  private center: Vec3 | null = null;
  private first: Vec3 | null = null;
  private second: Vec3 | null = null;

  start(): StepResult {
    return step.request({
      kind: 'point',
      prompt: 'Specify center point',
      keywords: [
        { name: '2Point', shortcut: '2P', description: 'Two points on the diameter' },
        { name: '3Point', shortcut: '3P', description: 'Three points on the circumference' },
      ],
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel' || input.kind === 'empty') return step.cancelled();

    if (input.kind === 'keyword') {
      const upper = input.value.toUpperCase();
      if (upper.startsWith('2')) {
        this.mode = 'twoPoint';
        return step.request({ kind: 'point', prompt: 'Specify first end of diameter' });
      }
      if (upper.startsWith('3')) {
        this.mode = 'threePoint';
        return step.request({ kind: 'point', prompt: 'Specify first point on circle' });
      }
      return step.request({ kind: 'point', prompt: 'Specify center point' });
    }

    switch (this.mode) {
      case 'centerRadius':
        return this.supplyCenterRadius(input, context);
      case 'twoPoint':
        return this.supplyTwoPoint(input, context);
      case 'threePoint':
        return this.supplyThreePoint(input, context);
    }
  }

  private supplyCenterRadius(input: InputValue, context: CommandContext): StepResult {
    if (this.center === null) {
      if (input.kind !== 'point') return step.request({ kind: 'point', prompt: 'Specify center point' });
      this.center = input.value;
      return step.request({
        kind: 'distance',
        prompt: 'Specify radius',
        basePoint: this.center,
        keywords: [{ name: 'Diameter', shortcut: 'D', description: 'Enter a diameter instead' }],
      });
    }

    const radius =
      input.kind === 'distance' || input.kind === 'number'
        ? input.value
        : input.kind === 'point'
          ? this.center.distanceTo(input.value)
          : 0;

    if (!(radius > 0)) return step.failed('Radius must be greater than zero.');
    context.transaction.addToModelSpace(CircleEntity.create(this.center, radius));
    return step.done();
  }

  private supplyTwoPoint(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'point') return step.cancelled();
    if (this.first === null) {
      this.first = input.value;
      return step.request({
        kind: 'point',
        prompt: 'Specify second end of diameter',
        basePoint: this.first,
      });
    }
    const center = this.first.midpoint(input.value);
    const radius = this.first.distanceTo(input.value) / 2;
    if (!(radius > 0)) return step.failed('The two points are coincident.');
    context.transaction.addToModelSpace(CircleEntity.create(center, radius));
    return step.done();
  }

  private supplyThreePoint(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'point') return step.cancelled();
    if (this.first === null) {
      this.first = input.value;
      return step.request({ kind: 'point', prompt: 'Specify second point on circle' });
    }
    if (this.second === null) {
      this.second = input.value;
      return step.request({ kind: 'point', prompt: 'Specify third point on circle' });
    }
    const arc = Arc.fromThreePoints(this.first, this.second, input.value);
    if (arc === null) return step.failed('The three points are collinear; no circle passes through them.');
    context.transaction.addToModelSpace(CircleEntity.create(arc.center, arc.radius));
    return step.done();
  }

  onCursorMove(world: Vec3): readonly Entity[] | null {
    if (this.mode === 'centerRadius' && this.center !== null) {
      const radius = this.center.distanceTo(world);
      if (radius > 0) return [CircleEntity.create(this.center, radius)];
    }
    return null;
  }
}

/**
 * ARC — three points on the arc, the most predictable of other CAD software's many arc
 * constructions and the one that needs no direction convention explained.
 */
class ArcCommand implements CommandInstance {
  private first: Vec3 | null = null;
  private second: Vec3 | null = null;

  start(): StepResult {
    return step.request({ kind: 'point', prompt: 'Specify start point of arc' });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'point') return step.cancelled();

    if (this.first === null) {
      this.first = input.value;
      return step.request({
        kind: 'point',
        prompt: 'Specify second point on arc',
        basePoint: this.first,
      });
    }
    if (this.second === null) {
      this.second = input.value;
      return step.request({ kind: 'point', prompt: 'Specify end point of arc' });
    }

    const arc = Arc.fromThreePoints(this.first, this.second, input.value);
    if (arc === null) {
      return step.failed('The three points are collinear; no arc passes through them.');
    }
    context.transaction.addToModelSpace(ArcEntity.fromArc(arc));
    return step.done();
  }

  onCursorMove(world: Vec3): readonly Entity[] | null {
    if (this.first !== null && this.second !== null) {
      const arc = Arc.fromThreePoints(this.first, this.second, world);
      if (arc !== null) return [ArcEntity.fromArc(arc)];
    } else if (this.first !== null) {
      return [LineEntity.create(this.first, world)];
    }
    return null;
  }
}

/** RECTANG — a closed rectangular polyline from two opposite corners. */
class RectangleCommand implements CommandInstance {
  private first: Vec3 | null = null;

  start(): StepResult {
    return step.request({ kind: 'point', prompt: 'Specify first corner' });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'point') return step.cancelled();
    if (this.first === null) {
      this.first = input.value;
      return step.request({
        kind: 'point',
        prompt: 'Specify opposite corner',
        basePoint: this.first,
      });
    }
    if (this.first.equals2(input.value)) {
      return step.failed('The corners are coincident; the rectangle would be empty.');
    }
    const rectangle = Polyline.rectangle(this.first, input.value);
    context.transaction.addToModelSpace(PolylineEntity.fromPolyline(rectangle));
    return step.done();
  }

  onCursorMove(world: Vec3): readonly Entity[] | null {
    if (this.first === null || this.first.equals2(world)) return null;
    return [PolylineEntity.fromPolyline(Polyline.rectangle(this.first, world))];
  }
}

/** POINT — place a node, repeating until cancelled. */
class PointCommand implements CommandInstance {
  private count = 0;

  start(): StepResult {
    return step.request({ kind: 'point', prompt: 'Specify a point' });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind !== 'point') {
      return this.count > 0 ? step.done(`${this.count} point(s) placed`) : step.cancelled();
    }
    context.transaction.addToModelSpace(PointEntity.create(input.value));
    this.count++;
    return step.request({ kind: 'point', prompt: 'Specify a point', allowEmpty: true });
  }
}

/** POLYGON — an inscribed or circumscribed regular polygon. */
class PolygonCommand implements CommandInstance {
  private sides = 6;
  private center: Vec3 | null = null;
  private inscribed = true;

  start(): StepResult {
    return step.request({
      kind: 'number',
      prompt: 'Enter number of sides',
      defaultValue: 6,
      min: 3,
      max: 1024,
      integerOnly: true,
    });
  }

  supply(input: InputValue, context: CommandContext): StepResult {
    if (input.kind === 'cancel') return step.cancelled();

    if (this.center === null) {
      if (input.kind === 'number') {
        this.sides = Math.round(input.value);
        if (this.sides < 3) return step.failed('A polygon needs at least three sides.');
        return step.request({ kind: 'point', prompt: 'Specify center of polygon' });
      }
      if (input.kind === 'empty') {
        return step.request({ kind: 'point', prompt: 'Specify center of polygon' });
      }
      if (input.kind === 'point') {
        this.center = input.value;
        return step.request({
          kind: 'keyword',
          prompt: 'Inscribed in circle or circumscribed about circle?',
          keywords: [
            { name: 'Inscribed', shortcut: 'I' },
            { name: 'Circumscribed', shortcut: 'C' },
          ],
          defaultValue: 'Inscribed',
        });
      }
      return step.request({ kind: 'point', prompt: 'Specify center of polygon' });
    }

    if (input.kind === 'keyword' || input.kind === 'empty') {
      if (input.kind === 'keyword') {
        this.inscribed = input.value.toUpperCase().startsWith('I');
      }
      return step.request({
        kind: 'distance',
        prompt: this.inscribed ? 'Specify radius of circle' : 'Specify radius of circle',
        basePoint: this.center,
      });
    }

    const radius =
      input.kind === 'distance' || input.kind === 'number'
        ? input.value
        : input.kind === 'point'
          ? this.center.distanceTo(input.value)
          : 0;
    if (!(radius > 0)) return step.failed('Radius must be greater than zero.');

    context.transaction.addToModelSpace(
      PolylineEntity.fromPolyline(regularPolygon(this.center, radius, this.sides, this.inscribed)),
    );
    return step.done();
  }
}

/**
 * Build a regular polygon.
 *
 * A circumscribed polygon touches the circle at its edge midpoints, so its
 * vertices sit further out by `1 / cos(pi / n)` — the distinction users rely
 * on when drawing bolt heads to an across-flats dimension.
 */
export function regularPolygon(
  center: Vec3,
  radius: number,
  sides: number,
  inscribed: boolean,
): Polyline {
  const effectiveRadius = inscribed ? radius : radius / Math.cos(Math.PI / sides);
  const points: Vec3[] = [];
  for (let i = 0; i < sides; i++) {
    const angle = (i * 2 * Math.PI) / sides + Math.PI / 2;
    points.push(center.add(Vec3.fromAngle(angle, effectiveRadius)));
  }
  return Polyline.fromPoints(points, true);
}

/** The drawing commands, ready to register. */
export const DRAW_COMMANDS: readonly CommandDefinition[] = [
  {
    id: 'cad.draw.line',
    name: 'LINE',
    aliases: ['L'],
    title: 'Line',
    description: 'Draw a series of straight line segments',
    category: 'Draw',
    transactionMode: CommandTransactionMode.Single,
    repeatable: true,
    create: () => new LineCommand(),
  },
  {
    id: 'cad.draw.polyline',
    name: 'PLINE',
    aliases: ['PL', 'POLYLINE'],
    title: 'Polyline',
    description: 'Draw a connected sequence of segments as a single object',
    category: 'Draw',
    repeatable: true,
    create: () => new PolylineCommand(),
  },
  {
    id: 'cad.draw.circle',
    name: 'CIRCLE',
    aliases: ['C'],
    title: 'Circle',
    description: 'Draw a circle by center and radius, two points or three points',
    category: 'Draw',
    repeatable: true,
    create: () => new CircleCommand(),
  },
  {
    id: 'cad.draw.arc',
    name: 'ARC',
    aliases: ['A'],
    title: 'Arc',
    description: 'Draw an arc through three points',
    category: 'Draw',
    repeatable: true,
    create: () => new ArcCommand(),
  },
  {
    id: 'cad.draw.rectangle',
    name: 'RECTANG',
    aliases: ['REC', 'RECTANGLE'],
    title: 'Rectangle',
    description: 'Draw a rectangular polyline',
    category: 'Draw',
    repeatable: true,
    create: () => new RectangleCommand(),
  },
  {
    id: 'cad.draw.point',
    name: 'POINT',
    aliases: ['PO'],
    title: 'Point',
    description: 'Place point objects',
    category: 'Draw',
    repeatable: true,
    create: () => new PointCommand(),
  },
  {
    id: 'cad.draw.polygon',
    name: 'POLYGON',
    aliases: ['POL'],
    title: 'Polygon',
    description: 'Draw a regular polygon',
    category: 'Draw',
    repeatable: true,
    create: () => new PolygonCommand(),
  },
];
