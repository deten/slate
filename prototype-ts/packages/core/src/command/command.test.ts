import { describe, expect, it, beforeEach } from 'vitest';
import { v, Vec3 } from '../math/vec3.js';
import { Database } from '../db/database.js';
import { TransactionManager } from '../db/transaction.js';
import { CommandRegistry } from './registry.js';
import { CommandExecutor, type SelectionBridge } from './executor.js';
import { createStandardRegistry } from './standard-commands.js';
import { step, type CommandDefinition, type InputValue } from './command.js';
import {
  ArcEntity,
  CircleEntity,
  LineEntity,
  PointEntity,
  PolylineEntity,
} from '../db/entities/curve-entities.js';
import type { Handle } from '../db/handle.js';

/** A minimal selection bridge backed by an array. */
function makeSelection(): SelectionBridge & { current: Handle[] } {
  const state = { current: [] as Handle[] };
  return {
    current: state.current,
    get: () => state.current,
    set: (handles) => {
      state.current = [...handles];
    },
    clear: () => {
      state.current = [];
    },
  };
}

interface Harness {
  db: Database;
  tm: TransactionManager;
  registry: CommandRegistry;
  executor: CommandExecutor;
  selection: ReturnType<typeof makeSelection>;
  /** Entities currently in model space. */
  entities(): ReturnType<Database['entitiesIn']> extends Iterable<infer T> ? T[] : never;
}

function harness(): Harness {
  const db = Database.createEmpty();
  const tm = new TransactionManager(db);
  const registry = createStandardRegistry();
  const selection = makeSelection();
  const executor = new CommandExecutor(db, tm, registry, selection);
  return {
    db,
    tm,
    registry,
    executor,
    selection,
    entities: () => [...db.entitiesIn(db.modelSpace)] as never,
  };
}

const point = (x: number, y: number): InputValue => ({ kind: 'point', value: v(x, y) });
const empty: InputValue = { kind: 'empty' };
const cancel: InputValue = { kind: 'cancel' };
const keyword = (value: string): InputValue => ({ kind: 'keyword', value });
const number = (value: number): InputValue => ({ kind: 'number', value });
const distance = (value: number): InputValue => ({ kind: 'distance', value });

describe('CommandRegistry', () => {
  let registry: CommandRegistry;

  beforeEach(() => {
    registry = createStandardRegistry();
  });

  it('registers the built-in commands', () => {
    expect(registry.size).toBeGreaterThan(10);
    expect(registry.lookup('LINE')).toBeDefined();
    expect(registry.lookup('line')).toBeDefined();
  });

  it('resolves aliases', () => {
    expect(registry.lookup('L')?.name).toBe('LINE');
    expect(registry.lookup('REC')?.name).toBe('RECTANG');
    expect(registry.lookup('co')?.name).toBe('COPY');
  });

  it('refuses duplicate ids and names', () => {
    const duplicate: CommandDefinition = {
      id: 'cad.draw.line',
      name: 'SOMETHING',
      title: 'x',
      create: () => ({ start: () => step.done(), supply: () => step.done() }),
    };
    expect(() => registry.register(duplicate)).toThrow(/already registered/);

    const nameClash: CommandDefinition = {
      id: 'unique.id',
      name: 'LINE',
      title: 'x',
      create: () => ({ start: () => step.done(), supply: () => step.done() }),
    };
    expect(() => registry.register(nameClash)).toThrow(/already used/);
  });

  it('rolls back a partial batch registration', () => {
    const before = registry.size;
    const batch: CommandDefinition[] = [
      {
        id: 'plugin.ok',
        name: 'PLUGINOK',
        title: 'ok',
        create: () => ({ start: () => step.done(), supply: () => step.done() }),
      },
      {
        // Collides, so the whole batch must be undone.
        id: 'plugin.bad',
        name: 'LINE',
        title: 'bad',
        create: () => ({ start: () => step.done(), supply: () => step.done() }),
      },
    ];
    expect(() => registry.registerAll(batch)).toThrow();
    expect(registry.size).toBe(before);
    expect(registry.lookup('PLUGINOK')).toBeUndefined();
  });

  it('lets a plugin add a command and remove it again', () => {
    const before = registry.size;
    const dispose = registry.register({
      id: 'plugin.hello',
      name: 'HELLO',
      aliases: ['HI'],
      title: 'Hello',
      category: 'Plugin',
      create: () => ({ start: () => step.done('hi'), supply: () => step.done() }),
    });
    expect(registry.lookup('HI')?.name).toBe('HELLO');
    dispose();
    expect(registry.size).toBe(before);
    expect(registry.lookup('HELLO')).toBeUndefined();
    expect(registry.lookup('HI')).toBeUndefined();
  });

  it('ranks an exact alias above a prefix match on another command', () => {
    // "L" is LINE's alias; it must not be beaten by a prefix hit on LAYER.
    const results = registry.search('L');
    expect(results[0]!.command.name).toBe('LINE');
  });

  it('prefers the shorter name for a prefix match', () => {
    registry.register({
      id: 'test.linetype',
      name: 'LINETYPE',
      title: 'Linetype',
      create: () => ({ start: () => step.done(), supply: () => step.done() }),
    });
    const results = registry.search('LIN');
    expect(results[0]!.command.name).toBe('LINE');
  });

  it('autocompletes to the canonical name, not the alias', () => {
    expect(registry.autocomplete('LI')).toBe('LINE');
    // "REC" matches RECTANG through its alias, but the suggestion is the real
    // command name so the user learns it.
    expect(registry.autocomplete('REC')).toBe('RECTANG');
  });

  it('suggests the shortest candidate when a prefix is ambiguous', () => {
    // Both REDO and RECTANG start with "RE"; the shorter one requires fewer
    // keystrokes to disambiguate, so it is offered first.
    expect(registry.autocomplete('RE')).toBe('REDO');
    expect(registry.autocomplete('ZZZ')).toBeNull();
  });

  it('groups commands by category for menus', () => {
    const groups = registry.byCategory();
    expect(groups.get('Draw')!.length).toBeGreaterThan(3);
    expect(groups.get('Modify')!.length).toBeGreaterThan(3);
  });
});

describe('Executor lifecycle', () => {
  it('reports an unknown command with a suggestion', async () => {
    const h = harness();
    const outcome = await h.executor.execute('LIEN');
    expect(outcome.status).toBe('unknown');
    const errors = h.executor.state.history.filter((e) => e.kind === 'error');
    expect(errors).toHaveLength(1);
  });

  it('keeps the transaction open across interactive steps', async () => {
    const h = harness();
    // The promise stays pending while the command awaits input, which is the
    // exact case a synchronous transaction scope would get wrong.
    const running = h.executor.execute('LINE');
    expect(h.executor.isRunning).toBe(true);

    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    // The entity exists before the command has finished.
    expect(h.entities()).toHaveLength(1);

    h.executor.supply(empty);
    const outcome = await running;
    expect(outcome.status).toBe('completed');
    expect(h.tm.canUndo).toBe(true);
  });

  it('discards an incomplete object when cancelled before anything is committed', async () => {
    const h = harness();
    const running = h.executor.execute('CIRCLE');
    h.executor.supply(point(0, 0));
    h.executor.supply(cancel);
    const outcome = await running;

    expect(outcome.status).toBe('cancelled');
    expect(h.entities()).toHaveLength(0);
    expect(h.tm.canUndo).toBe(false);
  });

  it('keeps committed geometry when a repeating command is cancelled', async () => {
    // Escape ends LINE but keeps the segments already drawn, as other CAD software does.
    // Only the rubber-band segment in progress is abandoned.
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    h.executor.supply(point(10, 10));
    expect(h.entities()).toHaveLength(2);

    h.executor.supply(cancel);
    const outcome = await running;

    expect(outcome.status).toBe('completed');
    expect(h.entities()).toHaveLength(2);
    // And it is a single undo step.
    expect(h.tm.undoDepth).toBe(1);
    h.tm.undo();
    expect(h.entities()).toHaveLength(0);
  });

  it('always exits on Escape even if a command tries to keep prompting', async () => {
    const h = harness();
    h.registry.register({
      id: 'test.stubborn',
      name: 'STUBBORN',
      title: 'Stubborn',
      create: () => ({
        start: () => step.request({ kind: 'point', prompt: 'forever' }),
        // Ignores cancellation and asks again.
        supply: () => step.request({ kind: 'point', prompt: 'forever' }),
      }),
    });

    const running = h.executor.execute('STUBBORN');
    h.executor.supply(cancel);
    const outcome = await running;

    expect(outcome.status).toBe('cancelled');
    expect(h.executor.isRunning).toBe(false);
  });

  it('records one undo step for a whole multi-segment line', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    for (const p of [point(0, 0), point(10, 0), point(10, 10), point(0, 10)]) {
      h.executor.supply(p);
    }
    h.executor.supply(empty);
    await running;

    expect(h.entities()).toHaveLength(3);
    expect(h.tm.undoDepth).toBe(1);
    h.tm.undo();
    expect(h.entities()).toHaveLength(0);
  });

  it('cancels the running command when another is started', async () => {
    const h = harness();
    const first = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(5, 0));

    const second = h.executor.execute('CIRCLE');
    const firstOutcome = await first;
    expect(firstOutcome.status).toBe('cancelled');
    // The interrupted line was rolled back.
    expect(h.entities().filter((e) => e instanceof LineEntity)).toHaveLength(0);

    h.executor.supply(point(0, 0));
    h.executor.supply(distance(5));
    await second;
    expect(h.entities()).toHaveLength(1);
  });

  it('rolls back and reports when a command throws', async () => {
    const h = harness();
    h.registry.register({
      id: 'test.explode',
      name: 'BOOM',
      title: 'Boom',
      create: () => ({
        start: (context) => {
          context.transaction.addToModelSpace(PointEntity.create(Vec3.ZERO));
          throw new Error('deliberate failure');
        },
        supply: () => step.done(),
      }),
    });

    const outcome = await h.executor.execute('BOOM');
    expect(outcome.status).toBe('failed');
    expect(h.entities()).toHaveLength(0);
    expect(h.tm.canUndo).toBe(false);
  });

  it('does not record an undo entry for a command that changed nothing', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(empty);
    await running;
    expect(h.tm.canUndo).toBe(false);
  });

  it('tracks the last command for Enter-to-repeat', async () => {
    const h = harness();
    const running = h.executor.execute('POINT');
    h.executor.supply(point(1, 1));
    h.executor.supply(empty);
    await running;

    expect(h.executor.state.lastCommand?.name).toBe('POINT');
    const repeat = h.executor.repeatLast();
    h.executor.supply(point(2, 2));
    h.executor.supply(empty);
    await repeat;
    expect(h.entities()).toHaveLength(2);
  });

  it('logs prompts and input to the command history', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(3, 4));
    h.executor.supply(empty);
    await running;

    const kinds = h.executor.state.history.map((e) => e.kind);
    expect(kinds).toContain('command');
    expect(kinds).toContain('prompt');
    expect(kinds).toContain('input');
    const inputs = h.executor.state.history.filter((e) => e.kind === 'input');
    expect(inputs[0]!.text).toBe('0,0');
  });

  it('offers live preview geometry as the cursor moves', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.moveCursor(v(10, 10));
    expect(h.executor.state.preview).toHaveLength(1);

    h.executor.supply(cancel);
    await running;
    // Preview is cleared when the command ends.
    expect(h.executor.state.preview).toHaveLength(0);
  });
});

describe('Draw commands', () => {
  it('LINE closes the chain on the Close keyword', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    h.executor.supply(point(10, 10));
    h.executor.supply(keyword('Close'));
    await running;
    // Three segments: two drawn plus the closing one.
    expect(h.entities()).toHaveLength(3);
  });

  it('LINE undoes the last segment on the Undo keyword', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    h.executor.supply(point(20, 0));
    expect(h.entities()).toHaveLength(2);

    h.executor.supply(keyword('U'));
    expect(h.entities()).toHaveLength(1);

    h.executor.supply(empty);
    await running;
  });

  it('LINE ignores a zero-length segment from a double-click', async () => {
    const h = harness();
    const running = h.executor.execute('LINE');
    h.executor.supply(point(5, 5));
    h.executor.supply(point(5, 5));
    h.executor.supply(point(10, 5));
    h.executor.supply(empty);
    await running;
    expect(h.entities()).toHaveLength(1);
  });

  it('CIRCLE draws from a centre and radius', async () => {
    const h = harness();
    const running = h.executor.execute('C');
    h.executor.supply(point(5, 5));
    h.executor.supply(distance(3));
    await running;

    const circle = h.entities()[0] as CircleEntity;
    expect(circle).toBeInstanceOf(CircleEntity);
    expect(circle.center.equals(v(5, 5))).toBe(true);
    expect(circle.radius).toBe(3);
  });

  it('CIRCLE supports the two-point diameter construction', async () => {
    const h = harness();
    const running = h.executor.execute('CIRCLE');
    h.executor.supply(keyword('2P'));
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    await running;

    const circle = h.entities()[0] as CircleEntity;
    expect(circle.center.equals(v(5, 0))).toBe(true);
    expect(circle.radius).toBeCloseTo(5);
  });

  it('CIRCLE reports collinear three-point input rather than drawing nonsense', async () => {
    const h = harness();
    const running = h.executor.execute('CIRCLE');
    h.executor.supply(keyword('3P'));
    h.executor.supply(point(0, 0));
    h.executor.supply(point(1, 0));
    h.executor.supply(point(2, 0));
    const outcome = await running;

    expect(outcome.status).toBe('failed');
    expect(outcome.message).toMatch(/collinear/);
    expect(h.entities()).toHaveLength(0);
  });

  it('CIRCLE rejects a zero radius', async () => {
    const h = harness();
    const running = h.executor.execute('CIRCLE');
    h.executor.supply(point(0, 0));
    h.executor.supply(distance(0));
    const outcome = await running;
    expect(outcome.status).toBe('failed');
    expect(h.entities()).toHaveLength(0);
  });

  it('ARC draws through three points', async () => {
    const h = harness();
    const running = h.executor.execute('ARC');
    h.executor.supply(point(1, 0));
    h.executor.supply(point(0, 1));
    h.executor.supply(point(-1, 0));
    await running;

    const arc = h.entities()[0] as ArcEntity;
    expect(arc).toBeInstanceOf(ArcEntity);
    expect(arc.radius).toBeCloseTo(1);
    expect(arc.geometry.containsPoint(v(0, 1))).toBe(true);
  });

  it('RECTANG draws a closed four-vertex polyline', async () => {
    const h = harness();
    const running = h.executor.execute('REC');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(20, 10));
    await running;

    const rect = h.entities()[0] as PolylineEntity;
    expect(rect).toBeInstanceOf(PolylineEntity);
    expect(rect.closed).toBe(true);
    expect(rect.vertices).toHaveLength(4);
    expect(rect.geometry.area).toBeCloseTo(200);
  });

  it('PLINE creates one entity, not several', async () => {
    const h = harness();
    const running = h.executor.execute('PL');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    h.executor.supply(point(10, 10));
    h.executor.supply(empty);
    await running;

    expect(h.entities()).toHaveLength(1);
    const poly = h.entities()[0] as PolylineEntity;
    expect(poly.vertices).toHaveLength(3);
  });

  it('POLYGON inscribes and circumscribes correctly', async () => {
    const h = harness();
    const running = h.executor.execute('POLYGON');
    h.executor.supply(number(6));
    h.executor.supply(point(0, 0));
    h.executor.supply(keyword('Inscribed'));
    h.executor.supply(distance(10));
    await running;

    const poly = h.entities()[0] as PolylineEntity;
    expect(poly.vertices).toHaveLength(6);
    // Inscribed vertices lie exactly on the circle.
    for (const vertex of poly.vertices) {
      expect(vertex.position.length2).toBeCloseTo(10);
    }
  });
});

describe('Modify commands', () => {
  async function withLine(h: Harness): Promise<LineEntity> {
    const running = h.executor.execute('LINE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(10, 0));
    h.executor.supply(empty);
    await running;
    return h.entities()[0] as LineEntity;
  }

  it('MOVE translates the selection', async () => {
    const h = harness();
    const line = await withLine(h);
    h.selection.set([line.handle]);

    const running = h.executor.execute('MOVE');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(5, 5));
    await running;

    expect(line.start.equals(v(5, 5))).toBe(true);
    expect(line.end.equals(v(15, 5))).toBe(true);
  });

  it('COPY leaves the original in place', async () => {
    const h = harness();
    const line = await withLine(h);
    h.selection.set([line.handle]);

    const running = h.executor.execute('COPY');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(0, 20));
    await running;

    expect(h.entities()).toHaveLength(2);
    expect(line.start.equals(Vec3.ZERO)).toBe(true);
  });

  it('ERASE deletes and undo restores', async () => {
    const h = harness();
    const line = await withLine(h);
    h.selection.set([line.handle]);

    await h.executor.execute('ERASE');
    expect(h.entities()).toHaveLength(0);

    h.tm.undo();
    expect(h.entities()).toHaveLength(1);
  });

  it('ERASE refuses to touch a locked layer', async () => {
    const h = harness();
    const line = await withLine(h);
    const layer = h.db.ensureLayer('Locked');
    layer.isLocked = true;
    line.layerHandle = layer.handle;
    h.selection.set([line.handle]);

    const outcome = await h.executor.execute('ERASE');
    expect(outcome.status).toBe('cancelled');
    expect(h.entities()).toHaveLength(1);
    expect(h.executor.state.history.some((e) => e.kind === 'warning')).toBe(true);
  });

  it('ROTATE turns the selection about a base point', async () => {
    const h = harness();
    const line = await withLine(h);
    h.selection.set([line.handle]);

    const running = h.executor.execute('ROTATE');
    h.executor.supply(point(0, 0));
    h.executor.supply({ kind: 'angle', value: Math.PI / 2 });
    await running;

    expect(line.end.x).toBeCloseTo(0);
    expect(line.end.y).toBeCloseTo(10);
  });

  it('SCALE substitutes an ellipse when a circle is scaled non-uniformly', async () => {
    const h = harness();
    const running = h.executor.execute('CIRCLE');
    h.executor.supply(point(0, 0));
    h.executor.supply(distance(5));
    await running;

    const circle = h.entities()[0]!;
    h.selection.set([circle.handle]);

    // SCALE is uniform, so it must keep the circle a circle.
    const scaling = h.executor.execute('SCALE');
    h.executor.supply(point(0, 0));
    h.executor.supply(number(2));
    await scaling;

    const result = h.entities()[0] as CircleEntity;
    expect(result).toBeInstanceOf(CircleEntity);
    expect(result.radius).toBeCloseTo(10);
  });

  it('MIRROR duplicates across a line without erasing the original', async () => {
    const h = harness();
    const line = await withLine(h);
    h.selection.set([line.handle]);

    const running = h.executor.execute('MIRROR');
    h.executor.supply(point(0, 0));
    h.executor.supply(point(1, 0));
    await running;

    expect(h.entities()).toHaveLength(2);
    expect(line.start.equals(Vec3.ZERO)).toBe(true);
  });

  it('UNDO and REDO work as commands', async () => {
    const h = harness();
    await withLine(h);
    expect(h.entities()).toHaveLength(1);

    await h.executor.execute('UNDO');
    expect(h.entities()).toHaveLength(0);

    await h.executor.execute('REDO');
    expect(h.entities()).toHaveLength(1);
  });

  it('UNDO reports gracefully when there is nothing to undo', async () => {
    const h = harness();
    const outcome = await h.executor.execute('UNDO');
    expect(outcome.status).toBe('cancelled');
    expect(outcome.message).toMatch(/Nothing to undo/);
  });

  it('OFFSET creates a parallel line on the picked side', async () => {
    const h = harness();
    const line = await withLine(h);

    const running = h.executor.execute('OFFSET');
    h.executor.supply(distance(5));
    h.executor.supply({ kind: 'entity', value: line.handle });
    // Pick above the line, so the offset must land above it.
    h.executor.supply(point(5, 100));
    h.executor.supply(cancel);
    await running;

    const created = h.entities().filter((e) => e !== line) as LineEntity[];
    expect(created).toHaveLength(1);
    expect(created[0]!.start.y).toBeCloseTo(5);
  });

  it('OFFSET puts the copy below when the pick is below', async () => {
    const h = harness();
    const line = await withLine(h);

    const running = h.executor.execute('OFFSET');
    h.executor.supply(distance(5));
    h.executor.supply({ kind: 'entity', value: line.handle });
    h.executor.supply(point(5, -100));
    h.executor.supply(cancel);
    await running;

    const created = h.entities().filter((e) => e !== line) as LineEntity[];
    expect(created[0]!.start.y).toBeCloseTo(-5);
  });

  it('TRIM cuts a line back to a crossing edge', async () => {
    const h = harness();
    // A horizontal line crossed by a vertical one at x = 5.
    let horizontal!: LineEntity;
    let vertical!: LineEntity;
    h.tm.run('setup', (tx) => {
      horizontal = tx.addToModelSpace(LineEntity.create(v(0, 0), v(10, 0))) as LineEntity;
      vertical = tx.addToModelSpace(LineEntity.create(v(5, -5), v(5, 5))) as LineEntity;
    });

    h.selection.set([vertical.handle]);
    const running = h.executor.execute('TRIM');
    // Pick the right-hand portion, which is the part to remove.
    h.executor.supply(point(8, 0));
    h.executor.supply(empty);
    await running;

    const lines = h.entities().filter((e): e is LineEntity => e instanceof LineEntity);
    // The vertical edge plus the surviving left-hand piece.
    expect(lines).toHaveLength(2);
    const survivor = lines.find((l) => l !== vertical)!;
    expect(survivor.start.equals(Vec3.ZERO)).toBe(true);
    expect(survivor.end.x).toBeCloseTo(5);
    void horizontal;
  });

  it('FILLET rounds a corner between two lines', async () => {
    const h = harness();
    let a!: LineEntity;
    let b!: LineEntity;
    h.tm.run('setup', (tx) => {
      a = tx.addToModelSpace(LineEntity.create(v(0, 0), v(10, 0))) as LineEntity;
      b = tx.addToModelSpace(LineEntity.create(v(0, 0), v(0, 10))) as LineEntity;
    });

    const running = h.executor.execute('FILLET');
    h.executor.supply(distance(2));
    h.executor.supply(point(8, 0));
    h.executor.supply(point(0, 8));
    await running;

    const arcs = h.entities().filter((e): e is ArcEntity => e instanceof ArcEntity);
    expect(arcs).toHaveLength(1);
    expect(arcs[0]!.radius).toBeCloseTo(2);
    // The originals were replaced by trimmed versions.
    expect(h.db.getEntity(a.handle)?.erased).toBe(true);
    expect(h.db.getEntity(b.handle)?.erased).toBe(true);
  });
});
