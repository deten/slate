import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve as resolvePath } from 'node:path';
import { InsertEntity, LineEntity, XrefStatus, Vec3, v } from '@cad/core';
import { loadDrawingWithReferences, XrefLoader } from './xref-loader.js';
import { ResourceResolver, ResourceKind, ResolutionSource } from './resource-resolver.js';
import { readDxf } from '../dxf/dxf-reader.js';
import { dxf } from '../dxf/fixtures.js';

/**
 * Build a DXF declaring xrefs and containing one line, so each generated
 * drawing is distinguishable by its geometry.
 */
function drawingWith(
  refs: ReadonlyArray<{ name: string; path: string; overlay?: boolean }>,
  lineAt: number,
): string {
  const blocks: Array<[number | string, string | number]> = [];
  let handle = 0x100;
  for (const ref of refs) {
    blocks.push(
      [0, 'BLOCK'],
      [5, (handle++).toString(16).toUpperCase()],
      [2, ref.name],
      [70, ref.overlay === true ? 12 : 4],
      [1, ref.path],
      [10, 0],
      [20, 0],
      [0, 'ENDBLK'],
      [5, (handle++).toString(16).toUpperCase()],
    );
  }

  const inserts: Array<[number | string, string | number]> = [];
  for (const ref of refs) {
    inserts.push(
      [0, 'INSERT'],
      [5, (handle++).toString(16).toUpperCase()],
      [8, '0'],
      [2, ref.name],
      [10, 0],
      [20, 0],
    );
  }

  return dxf(
    [0, 'SECTION'],
    [2, 'TABLES'],
    [0, 'TABLE'],
    [2, 'LAYER'],
    [0, 'LAYER'],
    [5, '10'],
    [2, 'Walls'],
    [70, 0],
    [62, 1],
    [6, 'Continuous'],
    [0, 'ENDTAB'],
    [0, 'ENDSEC'],
    [0, 'SECTION'],
    [2, 'BLOCKS'],
    ...blocks,
    [0, 'ENDSEC'],
    [0, 'SECTION'],
    [2, 'ENTITIES'],
    [0, 'LINE'],
    [5, (handle++).toString(16).toUpperCase()],
    [8, 'Walls'],
    [10, lineAt],
    [20, 0],
    [11, lineAt + 100],
    [21, 0],
    ...inserts,
    [0, 'ENDSEC'],
    [0, 'EOF'],
  );
}

let root = '';

beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), 'cad-xref-test-'));
  await mkdir(join(root, 'refs'), { recursive: true });
  await mkdir(join(root, 'refs', 'deep'), { recursive: true });
  await mkdir(join(root, 'elsewhere'), { recursive: true });

  // host.dxf -> refs/grid.dxf -> refs/deep/detail.dxf
  await writeFile(
    join(root, 'refs', 'deep', 'detail.dxf'),
    drawingWith([], 3000),
    'utf8',
  );
  await writeFile(
    join(root, 'refs', 'grid.dxf'),
    drawingWith([{ name: 'DETAIL', path: './deep/detail.dxf' }], 2000),
    'utf8',
  );
  await writeFile(
    join(root, 'host.dxf'),
    drawingWith(
      [
        { name: 'GRID', path: './refs/grid.dxf' },
        { name: 'MISSING', path: './refs/nope.dxf' },
      ],
      1000,
    ),
    'utf8',
  );

  // A drawing whose stored path is wrong but whose filename is findable.
  await writeFile(join(root, 'moved.dxf'), drawingWith([], 500), 'utf8');
  await writeFile(
    join(root, 'stale-path.dxf'),
    drawingWith([{ name: 'MOVED', path: 'C:\\old\\machine\\moved.dxf' }], 100),
    'utf8',
  );

  // A mutually referencing pair.
  await writeFile(
    join(root, 'cycle-a.dxf'),
    drawingWith([{ name: 'B', path: './cycle-b.dxf' }], 10),
    'utf8',
  );
  await writeFile(
    join(root, 'cycle-b.dxf'),
    drawingWith([{ name: 'A', path: './cycle-a.dxf' }], 20),
    'utf8',
  );

  // An overlay nested one level down.
  await writeFile(join(root, 'site.dxf'), drawingWith([], 9000), 'utf8');
  await writeFile(
    join(root, 'refs', 'with-overlay.dxf'),
    drawingWith([{ name: 'SITE', path: '../site.dxf', overlay: true }], 8000),
    'utf8',
  );
  await writeFile(
    join(root, 'overlay-host.dxf'),
    drawingWith([{ name: 'INNER', path: './refs/with-overlay.dxf' }], 7000),
    'utf8',
  );
});

afterAll(async () => {
  await rm(root, { recursive: true, force: true }).catch(() => undefined);
});

describe('ResourceResolver', () => {
  it('resolves a path relative to the host drawing', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const result = await resolver.resolve('./refs/grid.dxf');
    expect(result.found).toBe(true);
    expect(result.source).toBe(ResolutionSource.DrawingRelative);
    expect(result.path).toBe(resolvePath(root, 'refs', 'grid.dxf'));
  });

  it('resolves an absolute path', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const absolute = resolvePath(root, 'host.dxf');
    const result = await resolver.resolve(absolute);
    expect(result.found).toBe(true);
    expect(result.source).toBe(ResolutionSource.Absolute);
  });

  it('normalizes Windows separators so paths work cross-platform', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const result = await resolver.resolve('.\\refs\\grid.dxf');
    expect(result.found).toBe(true);
  });

  it('falls back to the bare filename when the stored path is stale', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const result = await resolver.resolve('C:\\some\\old\\path\\moved.dxf');
    expect(result.found).toBe(true);
    expect(result.source).toBe(ResolutionSource.FilenameFallback);
  });

  it('searches configured search paths', async () => {
    const resolver = new ResourceResolver({
      hostDirectory: join(root, 'elsewhere'),
      searchPaths: [join(root, 'refs')],
      filenameFallback: false,
    });
    const result = await resolver.resolve('grid.dxf');
    expect(result.found).toBe(true);
    expect(result.source).toBe(ResolutionSource.SearchPath);
  });

  it('reports every location it tried when a file is missing', async () => {
    const resolver = new ResourceResolver({
      hostDirectory: root,
      searchPaths: [join(root, 'refs')],
    });
    const result = await resolver.resolve('does-not-exist.dxf');
    expect(result.found).toBe(false);
    expect(result.source).toBe(ResolutionSource.NotFound);
    expect(result.attempted.length).toBeGreaterThan(1);
  });

  it('never rewrites the stored path', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const stored = '.\\refs\\grid.dxf';
    const result = await resolver.resolve(stored);
    // Resolution must not mutate what the drawing recorded, or saving would
    // silently convert a portable relative path to an absolute one.
    expect(result.originalPath).toBe(stored);
  });

  it('derives a nested resolver rooted at the referencing drawing', async () => {
    const resolver = new ResourceResolver({ hostDirectory: root });
    const nested = resolver.forNestedDrawing(join(root, 'refs', 'grid.dxf'));
    const result = await nested.resolve('./deep/detail.dxf');
    expect(result.found).toBe(true);
  });
});

describe('XrefLoader', () => {
  it('loads a reference and merges its geometry into the host', async () => {
    const { database, xrefs } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const grid = xrefs.nodes.find((n) => n.name === 'GRID')!;
    expect(grid.status).toBe(XrefStatus.Resolved);
    expect(grid.entityCount).toBeGreaterThan(0);

    const gridBlock = database.blocks.get('GRID')!;
    expect(gridBlock.xrefStatus).toBe(XrefStatus.Resolved);
    expect(gridBlock.resolvedPath).toBe(resolvePath(root, 'refs', 'grid.dxf'));
    expect(gridBlock.entityCount).toBeGreaterThan(0);
  });

  it('opens the host drawing even when a reference is missing', async () => {
    const { database, xrefs } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const missing = xrefs.nodes.find((n) => n.name === 'MISSING')!;
    expect(missing.status).toBe(XrefStatus.NotFound);
    expect(missing.attemptedPaths!.length).toBeGreaterThan(0);

    // The host's own geometry is present and correct regardless.
    const hostLines = [...database.entitiesIn(database.modelSpace)].filter(
      (e): e is LineEntity => e instanceof LineEntity,
    );
    expect(hostLines).toHaveLength(1);
    expect(hostLines[0]!.start.equals(v(1000, 0))).toBe(true);
    expect(xrefs.failedCount).toBe(1);
    expect(xrefs.resolvedCount).toBeGreaterThanOrEqual(1);
  });

  it('loads nested references recursively', async () => {
    const { xrefs } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const detail = xrefs.nodes.find((n) => n.name === 'DETAIL');
    expect(detail).toBeDefined();
    expect(detail!.status).toBe(XrefStatus.Resolved);
    // Nested one level below the host's direct reference.
    expect(detail!.depth).toBe(1);
  });

  it('resolves a nested path relative to its own drawing, not the host', async () => {
    // detail.dxf is at refs/deep/detail.dxf and is referenced as
    // "./deep/detail.dxf" from refs/grid.dxf. Resolving that against the host
    // directory would look for root/deep/detail.dxf and fail.
    const { xrefs } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const detail = xrefs.nodes.find((n) => n.name === 'DETAIL')!;
    expect(detail.resolvedPath).toBe(resolvePath(root, 'refs', 'deep', 'detail.dxf'));
  });

  it('prefixes xref symbol records so they cannot collide with the host', async () => {
    const { database } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    // The host has its own "Walls"; the reference's becomes "GRID|Walls".
    expect(database.layers.get('Walls')).toBeDefined();
    expect(database.layers.get('GRID|Walls')).toBeDefined();
    expect(database.layers.get('GRID|Walls')!.isXrefDependent).toBe(true);
    // Editing the host's layer must not affect the reference's.
    expect(database.layers.get('Walls')!.handle).not.toBe(
      database.layers.get('GRID|Walls')!.handle,
    );
  });

  it('detects a reference cycle instead of recursing forever', async () => {
    const { xrefs } = await loadDrawingWithReferences(join(root, 'cycle-a.dxf'));
    const circular = xrefs.nodes.filter((n) => n.status === XrefStatus.Circular);
    expect(circular.length).toBeGreaterThan(0);
    expect(circular[0]!.problem).toMatch(/cycle/i);
  });

  it('reports a stale stored path that resolved by filename', async () => {
    const { xrefs, diagnostics } = await loadDrawingWithReferences(
      join(root, 'stale-path.dxf'),
    );
    const moved = xrefs.nodes.find((n) => n.name === 'MOVED')!;
    expect(moved.status).toBe(XrefStatus.Resolved);
    expect(diagnostics.some((d) => d.message.includes('out of date'))).toBe(true);
  });

  it('does not load an overlay nested inside another reference', async () => {
    const { xrefs } = await loadDrawingWithReferences(join(root, 'overlay-host.dxf'));
    const inner = xrefs.nodes.find((n) => n.name === 'INNER')!;
    expect(inner.status).toBe(XrefStatus.Resolved);

    // The overlay one level down is deliberately skipped; that is what
    // distinguishes an overlay from an attachment.
    const site = xrefs.nodes.find((n) => n.name === 'SITE')!;
    expect(site.status).toBe(XrefStatus.Unloaded);
  });

  it('loads a top-level overlay normally', async () => {
    const loader = new XrefLoader(
      readDxf(drawingWith([{ name: 'SITE', path: './site.dxf', overlay: true }], 1)).database,
    );
    const result = await loader.loadAll(join(root, 'top.dxf'));
    const site = result.nodes.find((n) => n.name === 'SITE')!;
    expect(site.status).toBe(XrefStatus.Resolved);
  });

  it('keeps the insert pointing at the xref block after loading', async () => {
    const { database } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const inserts = [...database.entitiesIn(database.modelSpace)].filter(
      (e): e is InsertEntity => e instanceof InsertEntity,
    );
    const gridBlock = database.blocks.get('GRID')!;
    expect(inserts.some((i) => i.blockHandle === gridBlock.handle)).toBe(true);
  });

  it('includes resolved reference geometry in model extents', async () => {
    const { database } = await loadDrawingWithReferences(join(root, 'host.dxf'));
    const extents = database.modelExtents;
    // The host line ends at 1100; the grid reference reaches 2100 and the
    // nested detail 3100.
    expect(extents.max.x).toBeGreaterThanOrEqual(3000);
  });

  it('reports a DWG reference clearly when no translator is configured', async () => {
    await writeFile(
      join(root, 'dwg-ref.dxf'),
      drawingWith([{ name: 'DWGREF', path: './something.dwg' }], 1),
      'utf8',
    );
    await writeFile(join(root, 'something.dwg'), 'not really a dwg', 'utf8');

    const { xrefs } = await loadDrawingWithReferences(join(root, 'dwg-ref.dxf'));
    const node = xrefs.nodes.find((n) => n.name === 'DWGREF')!;
    expect(node.status).toBe(XrefStatus.Unresolved);
    expect(node.problem).toMatch(/translator/i);
  });
});
