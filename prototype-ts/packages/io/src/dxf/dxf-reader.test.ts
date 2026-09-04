import { describe, expect, it } from 'vitest';
import {
  ArcEntity,
  Arc,
  CircleEntity,
  DrawingUnits,
  EllipseEntity,
  InsertEntity,
  LineEntity,
  MTextEntity,
  PointEntity,
  PolylineEntity,
  TextEntity,
  AttributeEntity,
  AttributeDefinitionEntity,
  BlockTableRecord,
  ViewportEntity,
  XrefType,
  XrefStatus,
  Vec3,
  v,
  radToDeg,
} from '@cad/core';
import { readDxf } from './dxf-reader.js';
import { tokenizeDxf } from './tokenizer.js';
import { UnsupportedEntity } from './entity-reader.js';
import {
  INSERT_WITH_ATTRIBUTES,
  LEGACY_POLYLINE,
  MALFORMED,
  MESSY_FORMATTING,
  MINIMAL_DRAWING,
  WITH_LAYOUTS,
  WITH_UNKNOWN_DATA,
  WITH_XREFS,
  dxf,
} from './fixtures.js';

describe('Tokenizer', () => {
  it('reads code/value pairs with typed values', () => {
    const { groups } = tokenizeDxf('0\r\nSECTION\r\n10\r\n1.5\r\n70\r\n42\r\n');
    expect(groups).toHaveLength(3);
    expect(groups[0]).toMatchObject({ code: 0, value: 'SECTION' });
    // Code 10 is a double, 70 an integer.
    expect(groups[1]!.value).toBe(1.5);
    expect(groups[2]!.value).toBe(42);
    expect(typeof groups[2]!.value).toBe('number');
  });

  it('handles CRLF, LF, lone CR and a BOM in one file', () => {
    const result = readDxf(MESSY_FORMATTING);
    const lines = [...result.database.entitiesIn(result.database.modelSpace)];
    expect(lines).toHaveLength(1);
    const line = lines[0] as LineEntity;
    expect(line.start.equals(v(1.5, 2.5))).toBe(true);
    expect(line.end.equals(v(3.5, 4.5))).toBe(true);
  });

  it('keeps handles as strings rather than parsing them as numbers', () => {
    const { groups } = tokenizeDxf('5\r\n1E\r\n');
    // "1E" would become 1 or NaN if number-parsed.
    expect(groups[0]!.value).toBe('1E');
  });

  it('preserves leading and trailing spaces inside string values', () => {
    const { groups } = tokenizeDxf('1\r\n  spaced text  \r\n');
    expect(groups[0]!.value).toBe('  spaced text  ');
  });

  it('reports a malformed group code and stays in sync', () => {
    const { groups, diagnostics } = tokenizeDxf('0\nLINE\nbroken\nvalue\n10\n5\n');
    expect(diagnostics.some((d) => d.severity === 'warning')).toBe(true);
    // The valid groups on either side are still read.
    expect(groups.map((g) => g.code)).toEqual([0, 10]);
  });
});

describe('Reading a complete drawing', () => {
  const result = readDxf(MINIMAL_DRAWING);
  const db = result.database;

  it('reports the DXF version', () => {
    expect(result.version).toBe('AC1032');
    expect(result.versionName).toBe('other CAD software');
  });

  it('reads header variables into drawing settings', () => {
    expect(db.settings.insertionUnits).toBe(DrawingUnits.Millimeters);
    expect(db.settings.globalLinetypeScale).toBe(2.5);
    expect(db.settings.currentLayerName).toBe('Walls');
  });

  it('preserves header variables it does not model', () => {
    expect(db.settings.unknownHeaderVariables.has('$EXTMIN')).toBe(true);
  });

  it('reads the layer table with colours and states', () => {
    const walls = db.layers.get('Walls');
    expect(walls).toBeDefined();
    expect(walls!.color.toRgb()).toBe(0xff0000);
    expect(walls!.lineweight).toBe(50);

    // A negative colour index means the layer is off, and the magnitude
    // remains its colour.
    const hidden = db.layers.get('Hidden');
    expect(hidden!.isOff).toBe(true);
    expect(hidden!.color.index).toBe(3);
    expect(hidden!.isVisible).toBe(false);

    const frozen = db.layers.get('Frozen');
    expect(frozen!.isFrozen).toBe(true);
  });

  it('reads linetype patterns', () => {
    const dashed = db.linetypes.get('Dashed');
    expect(dashed).toBeDefined();
    expect(dashed!.elements.map((e) => e.length)).toEqual([12.7, -6.35]);
    expect(dashed!.isContinuous).toBe(false);
    expect(db.linetypes.get('Continuous')!.isContinuous).toBe(true);
  });

  it('links a layer to its linetype by handle', () => {
    const hidden = db.layers.get('Hidden')!;
    const dashed = db.linetypes.get('Dashed')!;
    expect(hidden.linetypeHandle).toBe(dashed.handle);
  });

  it('reads lines with their layer and colour', () => {
    const entities = [...db.entitiesIn(db.modelSpace)];
    const lines = entities.filter((e): e is LineEntity => e instanceof LineEntity);
    expect(lines).toHaveLength(2);

    const first = lines[0]!;
    expect(first.start.equals(Vec3.ZERO)).toBe(true);
    expect(first.end.equals(v(5000, 0))).toBe(true);
    expect(first.layerHandle).toBe(db.layers.get('Walls')!.handle);
    expect(first.color.isByLayer).toBe(true);

    // The second carries an explicit ACI 3.
    expect(lines[1]!.color.index).toBe(3);
  });

  it('converts arc angles from degrees to radians', () => {
    const arc = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is ArcEntity => e instanceof ArcEntity,
    )!;
    expect(arc.radius).toBe(500);
    expect(radToDeg(arc.startAngle)).toBeCloseTo(0);
    expect(radToDeg(arc.endAngle)).toBeCloseTo(180);
    // Half-circle of radius 500.
    expect(arc.geometry.length).toBeCloseTo(Math.PI * 500);
  });

  it('reads a circle', () => {
    const circle = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is CircleEntity => e instanceof CircleEntity,
    )!;
    expect(circle.center.equals(v(1000, 1000))).toBe(true);
    expect(circle.radius).toBe(250);
  });

  it('reads a closed lwpolyline', () => {
    const polys = [...db.entitiesIn(db.modelSpace)].filter(
      (e): e is PolylineEntity => e instanceof PolylineEntity,
    );
    const square = polys[0]!;
    expect(square.closed).toBe(true);
    expect(square.vertices).toHaveLength(4);
    expect(square.geometry.area).toBeCloseTo(1_000_000);
  });

  it('associates a bulge with the correct vertex', () => {
    const polys = [...db.entitiesIn(db.modelSpace)].filter(
      (e): e is PolylineEntity => e instanceof PolylineEntity,
    );
    const bulged = polys[1]!;
    expect(bulged.vertices).toHaveLength(3);
    // The bulge belongs to vertex 0, not vertex 1.
    expect(bulged.vertices[0]!.bulge).toBe(1);
    expect(bulged.vertices[1]!.bulge).toBe(0);
    // Segment 0 is therefore a semicircle and segment 1 a straight line.
    expect(bulged.segments[0]).toBeInstanceOf(Arc);
    expect(bulged.segments[1]).not.toBeInstanceOf(Arc);
    expect((bulged.segments[0] as Arc).sweep).toBeCloseTo(Math.PI);
  });

  it('reads single-line text', () => {
    const text = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is TextEntity => e instanceof TextEntity && !(e instanceof AttributeEntity),
    )!;
    expect(text.contents).toBe('ROOM 101');
    expect(text.height).toBe(200);
    expect(text.styleHandle).toBe(db.textStyles.get('Standard')!.handle);
  });

  it('reads mtext and strips its formatting for display', () => {
    const mtext = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is MTextEntity => e instanceof MTextEntity,
    )!;
    expect(mtext.contents).toBe('First line\\PSecond line');
    expect(mtext.plainText).toBe('First line\nSecond line');
    expect(mtext.width).toBe(2000);
  });

  it('reads a point and an ellipse', () => {
    const entities = [...db.entitiesIn(db.modelSpace)];
    const point = entities.find((e): e is PointEntity => e instanceof PointEntity)!;
    expect(point.position.equals(v(42, 43))).toBe(true);

    const ellipse = entities.find((e): e is EllipseEntity => e instanceof EllipseEntity)!;
    expect(ellipse.ratio).toBe(0.5);
    expect(ellipse.geometry.semiMajor).toBeCloseTo(500);
    expect(ellipse.geometry.semiMinor).toBeCloseTo(250);
    expect(ellipse.geometry.isClosed).toBe(true);
  });

  it('reads a block definition with its contents', () => {
    const door = db.blocks.get('DOOR');
    expect(door).toBeDefined();
    expect(door!.entityCount).toBe(2);
    const contents = [...db.entitiesIn(door!)];
    expect(contents.some((e) => e instanceof LineEntity)).toBe(true);
    expect(contents.some((e) => e instanceof ArcEntity)).toBe(true);
  });

  it('resolves a block reference to its definition', () => {
    const insert = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is InsertEntity => e instanceof InsertEntity,
    )!;
    expect(insert.blockHandle).toBe(db.blocks.get('DOOR')!.handle);
    expect(insert.position.equals(v(3000, 0))).toBe(true);
    expect(radToDeg(insert.rotation)).toBeCloseTo(90);
  });

  it('computes sensible model extents', () => {
    const extents = db.modelExtents;
    expect(extents.isEmpty).toBe(false);
    expect(extents.max.x).toBeGreaterThanOrEqual(5000);
  });

  it('produces a referentially valid database', () => {
    const issues = db.validate();
    expect(issues.filter((i) => i.severity === 'error')).toEqual([]);
  });

  it('synthesizes a Model layout when the file omits one', () => {
    const model = db.layouts.find((l) => l.isModelSpace);
    expect(model).toBeDefined();
    expect(db.blockForLayout(model!)).toBe(db.modelSpace);
  });
});

describe('Legacy POLYLINE', () => {
  const db = readDxf(LEGACY_POLYLINE).database;

  it('attaches VERTEX children to the polyline', () => {
    const poly = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is PolylineEntity => e instanceof PolylineEntity,
    )!;
    expect(poly.isLegacyFormat).toBe(true);
    expect(poly.closed).toBe(true);
    expect(poly.vertices).toHaveLength(3);
    expect(poly.vertices[1]!.bulge).toBe(0.5);
  });

  it('resumes normal reading after SEQEND', () => {
    const entities = [...db.entitiesIn(db.modelSpace)];
    // The VERTEX records must not become entities of their own.
    expect(entities).toHaveLength(2);
    expect(entities[1]).toBeInstanceOf(LineEntity);
  });
});

describe('Block attributes', () => {
  const db = readDxf(INSERT_WITH_ATTRIBUTES).database;

  it('reads attribute definitions inside the block', () => {
    const block = db.blocks.get('TITLEBLOCK')!;
    const attdef = [...db.entitiesIn(block)].find(
      (e): e is AttributeDefinitionEntity => e instanceof AttributeDefinitionEntity,
    )!;
    expect(attdef.tag).toBe('DRAWING_NO');
    expect(attdef.prompt).toBe('Enter drawing number');
  });

  it('reads attribute values attached to the insert', () => {
    const attributes = [...db.entitiesIn(db.modelSpace)].filter(
      (e): e is AttributeEntity => e instanceof AttributeEntity,
    );
    expect(attributes).toHaveLength(2);
    const byTag = new Map(attributes.map((a) => [a.tag, a.value]));
    expect(byTag.get('DRAWING_NO')).toBe('A-101');
    expect(byTag.get('TITLE')).toBe('GROUND FLOOR PLAN');
  });

  it('links the attributes back to their insert', () => {
    const insert = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is InsertEntity => e instanceof InsertEntity,
    )!;
    expect(insert.attributeHandles).toHaveLength(2);
  });
});

describe('External references', () => {
  const db = readDxf(WITH_XREFS).database;

  it('recognizes attachments and overlays', () => {
    const grid = db.blocks.get('GRID')!;
    expect(grid.isXref).toBe(true);
    expect(grid.xrefType).toBe(XrefType.Attachment);
    expect(grid.xrefPath).toBe('./refs/grid.dwg');

    const site = db.blocks.get('SITE')!;
    expect(site.xrefType).toBe(XrefType.Overlay);
    expect(site.xrefPath).toBe('../site/topo.dwg');
  });

  it('marks references unloaded rather than missing before resolution runs', () => {
    // "Not found" is a claim the reader has not earned yet.
    expect(db.blocks.get('GRID')!.xrefStatus).toBe(XrefStatus.Unloaded);
    expect(db.unresolvedReferences).toHaveLength(0);
  });

  it('still resolves an insert that points at an xref block', () => {
    const insert = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is InsertEntity => e instanceof InsertEntity,
    )!;
    expect(insert.blockHandle).toBe(db.blocks.get('GRID')!.handle);
  });

  it('lists external references for the reference manager', () => {
    expect(db.externalReferences.map((b) => b.name).sort()).toEqual(['GRID', 'SITE']);
  });
});

describe('Preserving unrecognized data', () => {
  const result = readDxf(WITH_UNKNOWN_DATA);
  const db = result.database;

  it('keeps an unsupported entity type instead of dropping it', () => {
    const unsupported = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is UnsupportedEntity => e instanceof UnsupportedEntity,
    );
    expect(unsupported).toBeDefined();
    expect(unsupported!.dxfType).toBe('HELIX');
    // It keeps every group code it could not interpret.
    expect(unsupported!.preserved?.unknownCodes.length).toBeGreaterThan(0);
    // But it contributes nothing to extents and cannot be picked.
    expect(unsupported!.bounds.isEmpty).toBe(true);
    expect(unsupported!.distanceToPoint(Vec3.ZERO)).toBe(Infinity);
  });

  it('keeps unmodelled group codes on a supported entity', () => {
    const line = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is LineEntity => e instanceof LineEntity,
    )!;
    const codes = line.preserved?.unknownCodes.map((u) => u.code) ?? [];
    expect(codes).toContain(160);
  });

  it('keeps third-party XData verbatim', () => {
    const line = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is LineEntity => e instanceof LineEntity,
    )!;
    const xdata = line.getXData('ACME_APP');
    expect(xdata).toEqual([
      { code: 1000, value: 'custom payload' },
      { code: 1040, value: 3.5 },
    ]);
  });

  it('survives a clone/restore cycle with preserved data intact', () => {
    const line = [...db.entitiesIn(db.modelSpace)].find(
      (e): e is LineEntity => e instanceof LineEntity,
    )!;
    const copy = line.clone();
    expect(copy.getXData('ACME_APP')).toEqual(line.getXData('ACME_APP'));
    expect(copy.preserved?.unknownCodes).toEqual(line.preserved?.unknownCodes);
  });
});

describe('Layouts and paper space', () => {
  const db = readDxf(WITH_LAYOUTS).database;

  it('names layouts from the second code 1, not the page setup name', () => {
    const names = db.layouts.map((l) => l.name);
    expect(names).toContain('Model');
    expect(names).toContain('Ground Floor');
  });

  it('reads the page setup name separately from the layout name', () => {
    const sheet = db.getLayout('Ground Floor')!;
    expect(sheet.plotSettings.pageSetupName).toBe('A1 Setup');
    expect(sheet.plotSettings.plotConfigurationName).toBe('HP DesignJet');
    // A layout whose page setup name is blank must not inherit the layout name.
    expect(db.getLayout('Model')!.plotSettings.pageSetupName).toBe('');
  });

  it('reads plot settings including paper size and scale', () => {
    const sheet = db.getLayout('Ground Floor')!;
    expect(sheet.plotSettings.paperWidth).toBe(841);
    expect(sheet.plotSettings.paperHeight).toBe(594);
    expect(sheet.plotSettings.scaleDenominator).toBe(100);
    expect(sheet.plotScale).toBeCloseTo(0.01);
    // Printable area accounts for the margins.
    expect(sheet.printableArea.width).toBeCloseTo(821);
  });

  it('links each layout to its block record in both directions', () => {
    const sheet = db.getLayout('Ground Floor')!;
    const block = db.blockForLayout(sheet);
    expect(block).toBeDefined();
    expect(block!.name).toBe('*Paper_Space');
    expect(block!.layoutHandle).toBe(sheet.handle);
  });

  it('orders tabs with model space first', () => {
    expect(db.layouts[0]!.isModelSpace).toBe(true);
  });

  it('reads a viewport with its model view and scale', () => {
    const paper = db.blocks.get('*Paper_Space')!;
    const viewport = [...db.entitiesIn(paper)].find(
      (e): e is ViewportEntity => e instanceof ViewportEntity,
    )!;
    expect(viewport.center.equals(v(210, 148))).toBe(true);
    expect(viewport.viewCenter.equals(v(5000, 2500))).toBe(true);
    expect(viewport.viewHeight).toBe(2600);
    expect(viewport.isOn).toBe(true);
    // 260 paper units showing 2600 drawing units is 1:10.
    expect(viewport.scaleFactor).toBeCloseTo(0.1);
  });

  it('maps a model point into the sheet through the viewport', () => {
    const paper = db.blocks.get('*Paper_Space')!;
    const viewport = [...db.entitiesIn(paper)].find(
      (e): e is ViewportEntity => e instanceof ViewportEntity,
    )!;
    const onSheet = viewport.modelToPaper.transformPoint(v(5000, 2500));
    expect(onSheet.x).toBeCloseTo(210);
    expect(onSheet.y).toBeCloseTo(148);
  });
});

describe('Resilience to malformed input', () => {
  it('reads what it can and reports the rest', () => {
    const result = readDxf(MALFORMED);
    const entities = [...result.database.entitiesIn(result.database.modelSpace)];
    // The circle after the damaged region is still read.
    expect(entities.some((e) => e instanceof CircleEntity)).toBe(true);
    expect(result.diagnostics.length).toBeGreaterThan(0);
  });

  it('substitutes zero for an unparseable coordinate rather than shifting the point', () => {
    const result = readDxf(MALFORMED);
    const line = [...result.database.entitiesIn(result.database.modelSpace)].find(
      (e): e is LineEntity => e instanceof LineEntity,
    )!;
    // X was garbage, so it reads 0; Y and the endpoint are unaffected.
    expect(line.start.equals(Vec3.ZERO)).toBe(true);
    expect(line.end.equals(v(10, 0))).toBe(true);
  });

  it('does not throw on an empty or truncated file', () => {
    expect(() => readDxf('')).not.toThrow();
    expect(() => readDxf('0\r\nSECTION\r\n2\r\nENTITIES\r\n0\r\nLINE\r\n')).not.toThrow();
  });

  it('produces a usable database from a file with no tables at all', () => {
    const minimal = dxf(
      [0, 'SECTION'],
      [2, 'ENTITIES'],
      [0, 'LINE'],
      [8, 'SomeLayer'],
      [10, 0],
      [20, 0],
      [11, 1],
      [21, 1],
      [0, 'ENDSEC'],
      [0, 'EOF'],
    );
    const db = readDxf(minimal).database;
    // Layer 0 and the referenced layer both exist.
    expect(db.layers.get('0')).toBeDefined();
    expect(db.layers.get('SomeLayer')).toBeDefined();
    expect(db.validate().filter((i) => i.severity === 'error')).toEqual([]);
  });

  it('warns rather than failing when an insert names an undefined block', () => {
    const orphan = dxf(
      [0, 'SECTION'],
      [2, 'ENTITIES'],
      [0, 'INSERT'],
      [8, '0'],
      [2, 'NO_SUCH_BLOCK'],
      [10, 0],
      [20, 0],
      [0, 'ENDSEC'],
      [0, 'EOF'],
    );
    const result = readDxf(orphan);
    expect(result.diagnostics.some((d) => d.message.includes('NO_SUCH_BLOCK'))).toBe(true);
    // The drawing still opens.
    expect([...result.database.entitiesIn(result.database.modelSpace)]).toHaveLength(1);
  });
});

describe('Handle preservation', () => {
  it('keeps the handles the file assigned', () => {
    const db = readDxf(MINIMAL_DRAWING).database;
    // Handles are canonicalized (leading zeros stripped, uppercased) but the
    // identity is the file's own.
    expect(db.getObject('100' as never)).toBeInstanceOf(LineEntity);
    expect(db.layers.get('Walls')!.handle).toBe('11');
  });

  it('allocates new handles that cannot collide with the fileis', () => {
    const db = readDxf(MINIMAL_DRAWING).database;
    const before = db.objectCount;
    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    db.addToModelSpace(line);
    expect(db.objectCount).toBe(before + 1);
    // The new handle is beyond every handle the file used.
    expect(Number.parseInt(line.handle, 16)).toBeGreaterThan(0x10a);
  });
});
