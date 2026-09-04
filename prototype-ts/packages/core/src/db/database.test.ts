import { describe, expect, it } from 'vitest';
import { v, Vec3 } from '../math/vec3.js';
import { Mat4 } from '../math/mat4.js';
import { Database } from './database.js';
import { TransactionManager, ABORT } from './transaction.js';
import { Layer, LAYER_ZERO } from './layer.js';
import { Color, Lineweight } from './color.js';
import { BlockTableRecord, MODEL_SPACE, XrefStatus, XrefType } from './block-table-record.js';
import {
  ArcEntity,
  CircleEntity,
  LineEntity,
  PointEntity,
  PolylineEntity,
} from './entities/curve-entities.js';
import { InsertEntity, AttributeEntity } from './entities/insert-entity.js';
import { TextEntity, MTextEntity, stripMTextFormatting } from './entities/text-entities.js';
import { ViewportEntity } from './entities/viewport-entity.js';
import { toHandle, HandleAllocator, NULL_HANDLE } from './handle.js';
import { LINETYPE_CONTINUOUS } from './linetype.js';

function newDb(): { db: Database; tm: TransactionManager } {
  const db = Database.createEmpty();
  return { db, tm: new TransactionManager(db) };
}

describe('Handles', () => {
  it('canonicalizes hex text and numbers', () => {
    expect(toHandle('1a')).toBe('1A');
    expect(toHandle('001F')).toBe('1F');
    expect(toHandle(255)).toBe('FF');
  });

  it('rejects malformed handles', () => {
    expect(() => toHandle('zz')).toThrow(TypeError);
    expect(() => toHandle('')).toThrow(TypeError);
    expect(() => toHandle(-1)).toThrow(TypeError);
  });

  it('never re-issues a handle it has observed', () => {
    const alloc = new HandleAllocator(1);
    alloc.observe(toHandle('FF'));
    expect(alloc.allocate()).toBe('100');
  });
});

describe('Database initialization', () => {
  it('creates the records every drawing must have', () => {
    const { db } = newDb();
    expect(db.layers.get(LAYER_ZERO)).toBeDefined();
    expect(db.linetypes.get(LINETYPE_CONTINUOUS)).toBeDefined();
    expect(db.textStyles.get('Standard')).toBeDefined();
    expect(db.blocks.get(MODEL_SPACE)).toBeDefined();
    expect(db.modelSpace.isModelSpace).toBe(true);
  });

  it('provides a model layout and a paper layout in tab order', () => {
    const { db } = newDb();
    const layouts = db.layouts;
    expect(layouts).toHaveLength(2);
    expect(layouts[0]!.isModelSpace).toBe(true);
    expect(layouts[1]!.name).toBe('Layout1');
    expect(db.blockForLayout(layouts[1]!)).toBeDefined();
  });

  it('marks reserved records so they cannot be deleted', () => {
    const { db } = newDb();
    expect(db.defaultLayer.isReserved).toBe(true);
    expect(db.modelSpace.isReserved).toBe(true);
    expect(db.linetypes.get(LINETYPE_CONTINUOUS)!.isReserved).toBe(true);
  });
});

describe('Symbol tables', () => {
  it('looks up names case-insensitively but preserves original casing', () => {
    const { db } = newDb();
    const layer = new Layer();
    layer.name = 'Walls';
    db.addLayer(layer);
    expect(db.layers.get('WALLS')).toBe(layer);
    expect(db.layers.get('walls')!.name).toBe('Walls');
  });

  it('rejects duplicate names that differ only by case', () => {
    const { db } = newDb();
    const a = new Layer();
    a.name = 'Walls';
    db.addLayer(a);
    const b = new Layer();
    b.name = 'WALLS';
    expect(() => db.addLayer(b)).toThrow(/already contains/);
  });

  it('renames while keeping the index consistent', () => {
    const { db } = newDb();
    const layer = new Layer();
    layer.name = 'Old';
    db.addLayer(layer);
    expect(db.layers.rename('Old', 'New')).toBe(true);
    expect(db.layers.get('Old')).toBeUndefined();
    expect(db.layers.get('New')).toBe(layer);
    expect(layer.name).toBe('New');
  });

  it('refuses a rename that would collide', () => {
    const { db } = newDb();
    for (const name of ['A', 'B']) {
      const l = new Layer();
      l.name = name;
      db.addLayer(l);
    }
    expect(db.layers.rename('A', 'b')).toBe(false);
  });

  it('generates unique names for importers', () => {
    const { db } = newDb();
    const l = new Layer();
    l.name = 'Grid';
    db.addLayer(l);
    expect(db.layers.uniqueName('Grid')).toBe('Grid1');
    expect(db.layers.uniqueName('Other')).toBe('Other');
  });

  it('creates layers on demand for malformed imports', () => {
    const { db } = newDb();
    const created = db.ensureLayer('Imported');
    expect(db.layers.get('Imported')).toBe(created);
    // Calling again returns the same record rather than duplicating.
    expect(db.ensureLayer('Imported')).toBe(created);
  });
});

describe('Property resolution', () => {
  it('resolves ByLayer colour from the entity layer', () => {
    const { db } = newDb();
    const layer = db.ensureLayer('Red');
    layer.color = Color.fromIndex(1);
    const line = LineEntity.create(Vec3.ZERO, v(10, 0));
    line.layerHandle = layer.handle;
    line.color = Color.BY_LAYER;
    db.addToModelSpace(line);

    const resolved = line.resolveProperties(db.resolutionContextFor(line));
    expect(resolved.rgb).toBe(0xff0000);
  });

  it('prefers an explicit entity colour over the layer', () => {
    const { db } = newDb();
    const layer = db.ensureLayer('Red');
    layer.color = Color.fromIndex(1);
    const line = LineEntity.create(Vec3.ZERO, v(10, 0));
    line.layerHandle = layer.handle;
    line.color = Color.fromRgb(0, 0x80, 0xff);
    db.addToModelSpace(line);

    expect(line.resolveProperties(db.resolutionContextFor(line)).rgb).toBe(0x0080ff);
  });

  it('resolves ByBlock from the enclosing insert', () => {
    const { db } = newDb();
    const layer = db.ensureLayer('L');
    layer.color = Color.fromIndex(1); // red

    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    line.layerHandle = layer.handle;
    line.color = Color.BY_BLOCK;

    const insert = new InsertEntity();
    insert.color = Color.fromIndex(3); // green

    const resolved = line.resolveProperties(db.resolutionContextFor(line, insert));
    expect(resolved.rgb).toBe(0x00ff00);
  });

  it('falls back to the layer for ByBlock outside any block', () => {
    const { db } = newDb();
    const layer = db.ensureLayer('L');
    layer.color = Color.fromIndex(1);
    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    line.layerHandle = layer.handle;
    line.color = Color.BY_BLOCK;
    expect(line.resolveProperties(db.resolutionContextFor(line)).rgb).toBe(0xff0000);
  });

  it('multiplies entity and global linetype scale', () => {
    const { db } = newDb();
    db.settings.globalLinetypeScale = 2;
    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    line.linetypeScale = 3;
    line.layerHandle = db.defaultLayer.handle;
    expect(line.resolveProperties(db.resolutionContextFor(line)).linetypeScale).toBe(6);
  });

  it('resolves Default lineweight to the 0.25mm system default', () => {
    const { db } = newDb();
    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    line.layerHandle = db.defaultLayer.handle;
    line.lineweight = Lineweight.ByLayer;
    db.defaultLayer.lineweight = Lineweight.Default;
    expect(line.resolveProperties(db.resolutionContextFor(line)).lineweight).toBe(25);
  });

  it('hides entities on a layer that is off or frozen but not one that is locked', () => {
    const { db } = newDb();
    const layer = db.ensureLayer('Hidden');
    const line = LineEntity.create(Vec3.ZERO, v(1, 0));
    line.layerHandle = layer.handle;

    layer.isOff = true;
    expect(line.resolveProperties(db.resolutionContextFor(line)).visible).toBe(false);
    layer.isOff = false;
    layer.isFrozen = true;
    expect(line.resolveProperties(db.resolutionContextFor(line)).visible).toBe(false);
    layer.isFrozen = false;
    layer.isLocked = true;
    expect(line.resolveProperties(db.resolutionContextFor(line)).visible).toBe(true);
    expect(layer.isEditable).toBe(false);
  });
});

describe('Entities', () => {
  it('computes bounds and curves for a line', () => {
    const line = LineEntity.create(v(1, 2), v(4, 6));
    expect(line.bounds.min.equals(v(1, 2))).toBe(true);
    expect(line.toCurves()).toHaveLength(1);
    expect(line.distanceToPoint(v(1, 2))).toBeCloseTo(0);
  });

  it('transforms a line in place', () => {
    const line = LineEntity.create(Vec3.ZERO, v(10, 0));
    line.transformBy(Mat4.translation(v(5, 5)));
    expect(line.start.equals(v(5, 5))).toBe(true);
    expect(line.end.equals(v(15, 5))).toBe(true);
  });

  it('keeps a circle circular under a uniform scale', () => {
    const circle = CircleEntity.create(Vec3.ZERO, 2);
    circle.transformBy(Mat4.scaling(3));
    expect(circle.radius).toBeCloseTo(6);
    expect(circle.requiresEllipseAfter(Mat4.scaling(v(2, 3, 1)))).toBe(true);
    expect(circle.requiresEllipseAfter(Mat4.rotationZ(1))).toBe(false);
  });

  it('offers grips that actually edit the entity', () => {
    const line = LineEntity.create(Vec3.ZERO, v(10, 0));
    const grips = line.getGripPoints();
    expect(grips).toHaveLength(3);
    // Dragging the midpoint grip translates the whole line.
    line.moveGrip(grips[1]!, v(5, 5));
    expect(line.start.equals(v(0, 5))).toBe(true);
    expect(line.end.equals(v(10, 5))).toBe(true);
  });

  it('reshapes an arc by dragging its midpoint grip', () => {
    const arc = ArcEntity.create(Vec3.ZERO, 5, 0, Math.PI);
    const grips = arc.getGripPoints();
    arc.moveGrip(grips[2]!, v(0, 3));
    // The arc now passes through (0,3) between (5,0) and (-5,0).
    expect(arc.geometry.containsPoint(v(0, 3))).toBe(true);
  });

  it('positions text by its alignment point when justified', () => {
    const text = TextEntity.create(v(0, 0), 'Hello', 2.5);
    expect(text.effectivePosition.equals(Vec3.ZERO)).toBe(true);
    text.horizontalAlignment = 1; // Center
    text.alignmentPoint = v(50, 50);
    expect(text.effectivePosition.equals(v(50, 50))).toBe(true);
  });

  it('strips MTEXT formatting codes to readable text', () => {
    expect(stripMTextFormatting('Line one\\PLine two')).toBe('Line one\nLine two');
    expect(stripMTextFormatting('{\\fArial|b1|i0;Bold}text')).toBe('Boldtext');
    expect(stripMTextFormatting('{\\C1;Red} normal')).toBe('Red normal');
    expect(stripMTextFormatting('50\\S+0.5^-0.5;mm')).toBe('50+0.5/-0.5mm');
    expect(stripMTextFormatting('A\\~B')).toBe('A B');
    expect(stripMTextFormatting('literal \\{brace\\}')).toBe('literal {brace}');
  });

  it('reports mtext line count through the stripped contents', () => {
    const mtext = MTextEntity.create(Vec3.ZERO, 'a\\Pb\\Pc', 2.5);
    expect(mtext.lines).toEqual(['a', 'b', 'c']);
    expect(mtext.bounds.height).toBeCloseTo(7.5);
  });
});

describe('Block references', () => {
  function buildBlockDb(): { db: Database; block: BlockTableRecord; insert: InsertEntity } {
    const db = Database.createEmpty();
    const block = new BlockTableRecord();
    block.name = 'Widget';
    db.addBlock(block);
    // A unit square at the origin.
    db.addEntity(PolylineEntity.fromPoints([Vec3.ZERO, v(1, 0), v(1, 1), v(0, 1)], true), block);

    const insert = InsertEntity.create(block.handle, v(10, 10));
    db.addToModelSpace(insert);
    return { db, block, insert };
  }

  it('places block contents through the insert transform', () => {
    const { db, insert } = buildBlockDb();
    db.blockExtents(db.modelSpace);
    const b = insert.bounds;
    expect(b.min.equals(v(10, 10), { point: 1e-9, angle: 1e-9 })).toBe(true);
    expect(b.max.equals(v(11, 11), { point: 1e-9, angle: 1e-9 })).toBe(true);
  });

  it('applies scale and rotation in the DWG-defined order', () => {
    const { db, insert } = buildBlockDb();
    insert.scale = v(2, 2, 1);
    insert.rotation = Math.PI / 2;
    db.blockExtents(db.modelSpace);
    const b = insert.bounds;
    // The 2x2 square rotated 90 degrees lands in -x, +y from the insertion.
    expect(b.min.x).toBeCloseTo(8);
    expect(b.max.y).toBeCloseTo(12);
  });

  it('flattens nested blocks with accumulated transforms', () => {
    const { db, block } = buildBlockDb();
    const outer = new BlockTableRecord();
    outer.name = 'Outer';
    db.addBlock(outer);
    db.addEntity(InsertEntity.create(block.handle, v(5, 0)), outer);

    const outerInsert = InsertEntity.create(outer.handle, v(100, 0));
    db.addToModelSpace(outerInsert);

    const flattened = [...db.flatten(db.modelSpace, Mat4.IDENTITY)];
    const polylines = flattened.filter((f) => f.entity instanceof PolylineEntity);
    expect(polylines).toHaveLength(2);
    // The nested one sits at 100 + 5 = 105.
    const nested = polylines.find((p) => p.transform.translation.x === 105);
    expect(nested).toBeDefined();
  });

  it('does not hang on a block that references itself', () => {
    const db = Database.createEmpty();
    const block = new BlockTableRecord();
    block.name = 'Recursive';
    db.addBlock(block);
    db.addEntity(InsertEntity.create(block.handle, Vec3.ZERO), block);
    db.addToModelSpace(InsertEntity.create(block.handle, Vec3.ZERO));

    // The guarantee is termination, not emptiness: the cycle is cut by the
    // visited set, and the surviving insert still contributes its insertion
    // point, exactly as an unresolvable xref does.
    const flattened = [...db.flatten(db.modelSpace, Mat4.IDENTITY)];
    expect(flattened.length).toBeGreaterThan(0);
    const extents = db.blockExtents(db.modelSpace);
    expect(extents.isEmpty).toBe(false);
    expect(extents.diagonal).toBeCloseTo(0);
    expect(extents.center.equals(Vec3.ZERO)).toBe(true);
  });

  it('gives a missing xref a location so it is visible in extents', () => {
    const db = Database.createEmpty();
    const xref = new BlockTableRecord();
    xref.name = 'MissingRef';
    xref.xrefType = XrefType.Attachment;
    xref.xrefStatus = XrefStatus.NotFound;
    xref.xrefPath = './missing.dwg';
    db.addBlock(xref);
    const insert = InsertEntity.create(xref.handle, v(42, 7));
    db.addToModelSpace(insert);

    expect(xref.isMissingContent).toBe(true);
    expect(db.unresolvedReferences).toHaveLength(1);
    // Bounds collapse to the insertion point rather than vanishing.
    expect(insert.bounds.min.equals(v(42, 7))).toBe(true);
  });

  it('expands a MINSERT array into one transform per cell', () => {
    const { db, insert } = buildBlockDb();
    insert.columnCount = 3;
    insert.rowCount = 2;
    insert.columnSpacing = 10;
    insert.rowSpacing = 5;
    expect([...insert.cellTransforms()]).toHaveLength(6);
    db.blockExtents(db.modelSpace);
    // Three columns 10 apart starting at x=10, each 1 wide: up to x=31.
    expect(insert.bounds.max.x).toBeCloseTo(31);
    expect(insert.bounds.max.y).toBeCloseTo(16);
  });
});

describe('Viewports', () => {
  it('derives scale from the view height and window height', () => {
    const vp = new ViewportEntity();
    vp.height = 100;
    vp.viewHeight = 10000;
    expect(vp.scaleFactor).toBeCloseTo(0.01); // 1:100
    vp.setScaleFactor(0.02);
    expect(vp.viewHeight).toBeCloseTo(5000);
  });

  it('maps model coordinates into paper space', () => {
    const vp = new ViewportEntity();
    vp.center = v(100, 100);
    vp.width = 100;
    vp.height = 100;
    vp.viewCenter = v(5000, 5000);
    vp.viewHeight = 10000;
    // The view centre lands at the window centre.
    const mapped = vp.modelToPaper.transformPoint(v(5000, 5000));
    expect(mapped.x).toBeCloseTo(100);
    expect(mapped.y).toBeCloseTo(100);
    // Round-trip through the inverse.
    const back = vp.paperToModel!.transformPoint(mapped);
    expect(back.x).toBeCloseTo(5000);
  });

  it('tracks per-viewport frozen layers independently', () => {
    const db = Database.createEmpty();
    const layer = db.ensureLayer('Dims');
    const vp = new ViewportEntity();
    expect(vp.isLayerFrozen(layer.handle)).toBe(false);
    vp.freezeLayer(layer.handle);
    expect(vp.isLayerFrozen(layer.handle)).toBe(true);
    // The layer itself remains globally visible.
    expect(layer.isVisible).toBe(true);
    vp.thawLayer(layer.handle);
    expect(vp.isLayerFrozen(layer.handle)).toBe(false);
  });

  it('does not rescale the model view when the window is moved or resized', () => {
    const vp = new ViewportEntity();
    vp.viewHeight = 1000;
    const before = vp.viewHeight;
    vp.transformBy(Mat4.translation(v(50, 0)));
    expect(vp.viewHeight).toBe(before);
  });
});

describe('Transactions and undo/redo', () => {
  it('records an addition and reverses it', () => {
    const { db, tm } = newDb();
    tm.run('Draw Line', (tx) => {
      tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(10, 0)));
    });
    expect(db.modelSpace.entityCount).toBe(1);
    expect(tm.canUndo).toBe(true);
    expect(tm.undoName).toBe('Draw Line');

    tm.undo();
    expect(db.modelSpace.entityCount).toBe(0);
    tm.redo();
    expect(db.modelSpace.entityCount).toBe(1);
  });

  it('restores modified geometry exactly', () => {
    const { db, tm } = newDb();
    let line!: LineEntity;
    tm.run('Draw', (tx) => {
      line = tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(10, 0))) as LineEntity;
    });

    tm.run('Move', (tx) => {
      tx.modifyEntity(line, (e) => e.transformBy(Mat4.translation(v(0, 20))));
    });
    expect(line.start.y).toBe(20);

    tm.undo();
    expect(line.start.y).toBe(0);
    expect(line.end.equals(v(10, 0))).toBe(true);
    tm.redo();
    expect(line.start.y).toBe(20);
  });

  it('treats a multi-object command as a single undo step', () => {
    const { db, tm } = newDb();
    tm.run('Draw Many', (tx) => {
      for (let i = 0; i < 5; i++) {
        tx.addToModelSpace(LineEntity.create(v(i, 0), v(i, 10)));
      }
    });
    expect(db.modelSpace.entityCount).toBe(5);
    expect(tm.undoDepth).toBe(1);

    tm.undo();
    expect(db.modelSpace.entityCount).toBe(0);
  });

  it('restores an erased entity to its original draw order position', () => {
    const { db, tm } = newDb();
    const handles: string[] = [];
    tm.run('Draw', (tx) => {
      for (let i = 0; i < 3; i++) {
        handles.push(tx.addToModelSpace(PointEntity.create(v(i, 0))).handle);
      }
    });

    const middle = db.getEntity(handles[1] as never)!;
    tm.run('Erase', (tx) => {
      tx.erase(middle);
    });
    expect(db.modelSpace.entityHandles).toEqual([handles[0], handles[2]]);

    tm.undo();
    expect(db.modelSpace.entityHandles).toEqual(handles);
  });

  it('rolls everything back when the body throws', () => {
    const { db, tm } = newDb();
    expect(() =>
      tm.run('Failing', (tx) => {
        tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
        tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(2, 0)));
        throw new Error('command failed midway');
      }),
    ).toThrow('command failed midway');

    // Nothing was left behind, and no undo entry was created.
    expect(db.modelSpace.entityCount).toBe(0);
    expect(tm.canUndo).toBe(false);
  });

  it('rolls back on ABORT without recording history', () => {
    const { db, tm } = newDb();
    const result = tm.run('Cancelled', (tx) => {
      tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
      return ABORT;
    });
    expect(result).toBe(ABORT);
    expect(db.modelSpace.entityCount).toBe(0);
    expect(tm.canUndo).toBe(false);
  });

  it('leaves modified objects untouched after an abort', () => {
    const { tm } = newDb();
    let line!: LineEntity;
    tm.run('Draw', (tx) => {
      line = tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(10, 0))) as LineEntity;
    });

    tm.run('Cancelled Move', (tx) => {
      tx.modifyEntity(line, (e) => e.transformBy(Mat4.translation(v(100, 100))));
      return ABORT;
    });
    expect(line.start.equals(Vec3.ZERO)).toBe(true);
    expect(line.end.equals(v(10, 0))).toBe(true);
  });

  it('joins a nested transaction into the outer one', () => {
    const { db, tm } = newDb();
    tm.run('Outer', (tx) => {
      tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
      tm.run('Inner', (inner) => {
        inner.addToModelSpace(LineEntity.create(Vec3.ZERO, v(2, 0)));
      });
    });
    expect(db.modelSpace.entityCount).toBe(2);
    // A single undo step, not two.
    expect(tm.undoDepth).toBe(1);
    tm.undo();
    expect(db.modelSpace.entityCount).toBe(0);
  });

  it('discards the redo branch once a new edit is made', () => {
    const { tm } = newDb();
    tm.run('A', (tx) => tx.addToModelSpace(PointEntity.create(Vec3.ZERO)));
    tm.undo();
    expect(tm.canRedo).toBe(true);
    tm.run('B', (tx) => tx.addToModelSpace(PointEntity.create(v(1, 1))));
    expect(tm.canRedo).toBe(false);
  });

  it('snapshots an object only once however many times it is modified', () => {
    const { tm } = newDb();
    let line!: LineEntity;
    tm.run('Draw', (tx) => {
      line = tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0))) as LineEntity;
    });
    tm.run('Move Repeatedly', (tx) => {
      for (let i = 0; i < 10; i++) {
        tx.modifyEntity(line, (e) => e.transformBy(Mat4.translation(v(1, 0))));
      }
    });
    expect(line.start.x).toBeCloseTo(10);
    tm.undo();
    // A single undo returns it all the way to the start.
    expect(line.start.x).toBeCloseTo(0);
  });

  it('survives many undo/redo cycles without drift', () => {
    const { tm } = newDb();
    let circle!: CircleEntity;
    tm.run('Draw', (tx) => {
      circle = tx.addToModelSpace(CircleEntity.create(v(5, 5), 3)) as CircleEntity;
    });
    tm.run('Scale', (tx) => {
      tx.modifyEntity(circle, (e) => e.transformBy(Mat4.scaling(2, v(5, 5))));
    });

    for (let i = 0; i < 20; i++) {
      tm.undo();
      expect(circle.radius).toBeCloseTo(3);
      tm.redo();
      expect(circle.radius).toBeCloseTo(6);
    }
    expect(circle.center.equals(v(5, 5))).toBe(true);
  });

  it('preserves XData through a clone/restore cycle', () => {
    const { tm } = newDb();
    let line!: LineEntity;
    tm.run('Draw', (tx) => {
      line = tx.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0))) as LineEntity;
      line.setXData('ACME_STRUCTURAL', [{ code: 1000, value: 'BEAM-12' }]);
    });
    tm.run('Move', (tx) => {
      tx.modifyEntity(line, (e) => e.transformBy(Mat4.translation(v(5, 0))));
    });
    tm.undo();
    // Third-party data must survive an undo untouched.
    expect(line.getXData('ACME_STRUCTURAL')).toEqual([{ code: 1000, value: 'BEAM-12' }]);
  });

  it('refuses to undo while a transaction is open', () => {
    const { tm } = newDb();
    expect(() =>
      tm.run('Bad', () => {
        tm.undo();
      }),
    ).toThrow(/while a transaction is open/);
  });

  it('caps the undo stack at the configured depth', () => {
    const { tm } = newDb();
    tm.maxUndoDepth = 5;
    for (let i = 0; i < 12; i++) {
      tm.run(`Op ${i}`, (tx) => tx.addToModelSpace(PointEntity.create(v(i, 0))));
    }
    expect(tm.undoDepth).toBe(5);
    expect(tm.history[4]).toBe('Op 11');
  });
});

describe('Erase and purge', () => {
  it('keeps erased objects retrievable but out of iteration', () => {
    const { db } = newDb();
    const line = db.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
    db.eraseObject(line.handle);
    expect([...db.entitiesIn(db.modelSpace)]).toHaveLength(0);
    // Still present for undo and for dangling references.
    expect(db.getObject(line.handle)).toBe(line);
  });

  it('purges only erased objects that nothing references', () => {
    const { db } = newDb();
    const block = new BlockTableRecord();
    block.name = 'Used';
    db.addBlock(block);
    const insert = db.addToModelSpace(InsertEntity.create(block.handle, Vec3.ZERO));

    const orphan = db.addToModelSpace(PointEntity.create(Vec3.ZERO));
    db.eraseObject(orphan.handle);
    db.eraseObject(block.handle);

    const removed = db.purgeErased();
    // The orphan goes; the block stays because the live insert references it.
    expect(removed).toBe(1);
    expect(db.getObject(orphan.handle)).toBeUndefined();
    expect(db.getObject(block.handle)).toBeDefined();
    expect(insert.blockHandle).toBe(block.handle);
  });
});

describe('Validation', () => {
  it('reports dangling references without throwing', () => {
    const { db } = newDb();
    const line = db.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
    line.layerHandle = toHandle('DEADBEEF');

    const issues = db.validate();
    expect(issues.some((i) => i.message.includes('DEADBEEF'))).toBe(true);
    expect(issues.every((i) => i.severity === 'error' || i.severity === 'warning')).toBe(true);
  });

  it('reports a clean database as having no issues', () => {
    const { db } = newDb();
    db.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
    expect(db.validate()).toHaveLength(0);
  });
});

describe('Events', () => {
  it('notifies listeners of adds, modifies and erases', () => {
    const { db } = newDb();
    const events: string[] = [];
    const off = db.addListener((e) => events.push(e.kind));

    const line = db.addToModelSpace(LineEntity.create(Vec3.ZERO, v(1, 0)));
    db.notifyModified(line.handle);
    db.eraseObject(line.handle);
    off();
    db.addToModelSpace(PointEntity.create(Vec3.ZERO));

    expect(events).toEqual(['objectAdded', 'objectModified', 'objectErased']);
  });

  it('collapses bulk-load notifications into a single reset', () => {
    const { db } = newDb();
    const events: string[] = [];
    db.addListener((e) => events.push(e.kind));

    db.bulkLoad(() => {
      for (let i = 0; i < 100; i++) {
        db.addToModelSpace(PointEntity.create(v(i, 0)));
      }
    });
    expect(events).toEqual(['reset']);
  });
});

describe('Attributes', () => {
  it('clones an attribute without recursing through the text base class', () => {
    const attr = new AttributeEntity();
    attr.tag = 'ROOM_NAME';
    attr.value = 'Office 101';
    attr.height = 3;
    attr.position = v(1, 2);

    const copy = attr.clone();
    expect(copy).toBeInstanceOf(AttributeEntity);
    expect(copy.tag).toBe('ROOM_NAME');
    expect(copy.value).toBe('Office 101');
    expect(copy.height).toBe(3);
    expect(copy.position.equals(v(1, 2))).toBe(true);
    // A genuine copy, not the same object.
    expect(copy).not.toBe(attr);
  });

  it('round-trips an attribute through restoreFrom', () => {
    const attr = new AttributeEntity();
    attr.tag = 'TAG';
    attr.value = 'before';
    const snapshot = attr.clone();
    attr.value = 'after';
    attr.restoreFrom(snapshot);
    expect(attr.value).toBe('before');
  });
});
