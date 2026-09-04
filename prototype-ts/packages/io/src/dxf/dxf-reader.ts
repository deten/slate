import {
  BlockTableRecord,
  Color,
  Database,
  Entity,
  Layer,
  Layout,
  Linetype,
  PolylineEntity,
  TextStyle,
  Vec3,
  XrefStatus,
  XrefType,
  degToRad,
  tryToHandle,
  type Handle,
  type LinetypeElement,
} from '@cad/core';
import { DxfTokenizer, GroupCursor, type DxfDiagnostic, type DxfGroup } from './tokenizer.js';
import { DxfRecord } from './record.js';
import {
  attachPolylineVertices,
  readEntity,
  type ReadContext,
} from './entity-reader.js';
import { SECTION, dxfVersionName } from './group-codes.js';

/** Outcome of reading a DXF file. */
export interface DxfReadResult {
  readonly database: Database;
  readonly diagnostics: readonly DxfDiagnostic[];
  /** `$ACADVER` value, e.g. `AC1032`. */
  readonly version: string;
  readonly versionName: string;
  readonly entityCount: number;
}

/**
 * Reader for ASCII DXF.
 *
 * Structured as a section dispatcher over a group cursor. The design priority
 * throughout is *resilience*: a real professional drawing is large, was
 * produced by software of varying quality, and frequently contains
 * inconsistencies. A reader that throws on the first surprise is useless for
 * the stated goal of opening real files, so problems are collected as
 * diagnostics and reading continues wherever the file remains interpretable.
 */
export class DxfReader {
  private readonly db: Database;
  private readonly diagnostics: DxfDiagnostic[] = [];
  private readonly deferred: Array<() => void> = [];
  private readonly blockNamesToHandles = new Map<string, Handle>();
  private version = '';
  private entityCount = 0;

  /** Handles seen in the file, so new allocations cannot collide. */
  private readonly context: ReadContext;

  constructor() {
    // Start from a bare database, not `createEmpty`: the file supplies its own
    // symbol tables, and pre-seeding would collide with them or leave defaults
    // that were never in the original drawing.
    this.db = new Database();
    this.context = {
      db: this.db,
      diagnostics: this.diagnostics,
      deferred: this.deferred,
      blockNamesToHandles: this.blockNamesToHandles,
    };
  }

  /** Read DXF text into a database. */
  read(text: string): DxfReadResult {
    const tokenizer = new DxfTokenizer(text);
    const cursor = new GroupCursor(tokenizer.groups());

    this.db.bulkLoad(() => {
      this.ensureMinimumTables();
      this.readSections(cursor);
      // Name-based references are resolved only once every object exists.
      for (const action of this.deferred) {
        try {
          action();
        } catch (error) {
          this.diagnostics.push({
            severity: 'warning',
            message: `Deferred reference resolution failed: ${
              error instanceof Error ? error.message : String(error)
            }`,
          });
        }
      }
      this.ensureLayouts();
    });

    this.diagnostics.push(...tokenizer.problems);

    return {
      database: this.db,
      diagnostics: this.diagnostics,
      version: this.version,
      versionName: dxfVersionName(this.version),
      entityCount: this.entityCount,
    };
  }

  /**
   * Create the records a valid drawing must have, before reading.
   *
   * The file will normally define its own and those take precedence via
   * `ensureLayer`-style lookups. This only guarantees that a *malformed* file
   * — one missing its LAYER table entirely, which does occur — still produces
   * a usable database.
   */
  private ensureMinimumTables(): void {
    if (!this.db.linetypes.has('Continuous')) {
      const lt = new Linetype();
      lt.name = 'Continuous';
      lt.description = 'Solid line';
      this.db.addLinetype(lt);
    }
    if (!this.db.textStyles.has('Standard')) {
      const style = new TextStyle();
      style.name = 'Standard';
      this.db.addTextStyle(style);
    }
    if (!this.db.layers.has('0')) {
      const layer = new Layer();
      layer.name = '0';
      layer.color = Color.fromIndex(7);
      this.db.addLayer(layer);
    }
    if (!this.db.blocks.has('*Model_Space')) {
      const model = new BlockTableRecord();
      model.name = '*Model_Space';
      this.db.addBlock(model);
    }
  }

  private readSections(cursor: GroupCursor): void {
    for (;;) {
      const group = cursor.next();
      if (group === null) return;
      if (group.code !== 0) continue;

      const value = String(group.value).toUpperCase();
      if (value === 'EOF') return;
      if (value !== 'SECTION') continue;

      const nameGroup = cursor.next();
      if (nameGroup === null) return;
      if (nameGroup.code !== 2) {
        this.diagnostics.push({
          severity: 'warning',
          message: 'SECTION without a name; skipping',
          line: nameGroup.line,
        });
        continue;
      }

      const sectionName = String(nameGroup.value).toUpperCase();
      switch (sectionName) {
        case SECTION.HEADER:
          this.readHeader(cursor);
          break;
        case SECTION.TABLES:
          this.readTables(cursor);
          break;
        case SECTION.BLOCKS:
          this.readBlocks(cursor);
          break;
        case SECTION.ENTITIES:
          this.readEntities(cursor);
          break;
        case SECTION.OBJECTS:
          this.readObjects(cursor);
          break;
        default:
          // CLASSES, THUMBNAILIMAGE and ACDSDATA are skipped: they hold no
          // drawing content this application needs, and skipping is safer than
          // half-interpreting them.
          this.skipToEndSec(cursor);
          break;
      }
    }
  }

  /** Advance past everything up to and including the section's ENDSEC. */
  private skipToEndSec(cursor: GroupCursor): void {
    for (;;) {
      const group = cursor.next();
      if (group === null) return;
      if (group.code === 0 && String(group.value).toUpperCase() === 'ENDSEC') return;
    }
  }

  // ------------------------------------------------------------------ header

  /**
   * Read the HEADER section.
   *
   * Variables come as a code-9 name followed by one or more value groups whose
   * codes vary per variable. Only the ones that affect display or editing are
   * interpreted; the rest are preserved so that saving does not reset a
   * drawing's configuration.
   */
  private readHeader(cursor: GroupCursor): void {
    let currentName: string | null = null;
    let pending: DxfGroup[] = [];

    const flush = (): void => {
      if (currentName === null) return;
      this.applyHeaderVariable(currentName, pending);
      currentName = null;
      pending = [];
    };

    for (;;) {
      const group = cursor.next();
      if (group === null) return;
      if (group.code === 0 && String(group.value).toUpperCase() === 'ENDSEC') {
        flush();
        return;
      }
      if (group.code === 9) {
        flush();
        currentName = String(group.value).toUpperCase();
        continue;
      }
      if (currentName !== null) pending.push(group);
    }
  }

  private applyHeaderVariable(name: string, groups: readonly DxfGroup[]): void {
    const first = groups[0];
    const num = (fallback: number): number =>
      typeof first?.value === 'number' ? first.value : fallback;
    const str = (fallback: string): string =>
      first === undefined ? fallback : String(first.value);
    const point = (): Vec3 => {
      const x = groups.find((g) => g.code === 10)?.value;
      const y = groups.find((g) => g.code === 20)?.value;
      const z = groups.find((g) => g.code === 30)?.value;
      return new Vec3(Number(x ?? 0), Number(y ?? 0), Number(z ?? 0));
    };

    const settings = this.db.settings;
    switch (name) {
      case '$ACADVER':
        this.version = str('');
        this.db.sourceFormat = this.version;
        break;
      case '$INSUNITS':
        settings.insertionUnits = num(settings.insertionUnits);
        break;
      case '$LTSCALE':
        settings.globalLinetypeScale = num(1);
        break;
      case '$PSLTSCALE':
        settings.paperSpaceLinetypeScale = num(1);
        break;
      case '$LUNITS':
        settings.linearFormat = num(settings.linearFormat);
        break;
      case '$LUPREC':
        settings.linearPrecision = num(settings.linearPrecision);
        break;
      case '$AUNITS':
        settings.angularFormat = num(settings.angularFormat);
        break;
      case '$AUPREC':
        settings.angularPrecision = num(settings.angularPrecision);
        break;
      case '$ANGBASE':
        settings.angleBase = degToRad(num(0));
        break;
      case '$ANGDIR':
        settings.angleClockwise = num(0) !== 0;
        break;
      case '$LWDISPLAY':
        settings.displayLineweights = num(0) !== 0;
        break;
      case '$PDMODE':
        settings.pointDisplayMode = num(0);
        break;
      case '$PDSIZE':
        settings.pointDisplaySize = num(0);
        break;
      case '$LIMMIN':
        settings.limitsMin = point();
        break;
      case '$LIMMAX':
        settings.limitsMax = point();
        break;
      case '$INSBASE':
        settings.insertionBase = point();
        break;
      case '$GRIDUNIT':
        settings.gridSpacing = point();
        break;
      case '$SNAPUNIT':
        settings.snapSpacing = point();
        break;
      case '$ORTHOMODE':
        settings.orthoEnabled = num(0) !== 0;
        break;
      case '$OSMODE':
        settings.osnapModes = num(settings.osnapModes);
        break;
      case '$CLAYER':
        settings.currentLayerName = str('0');
        break;
      case '$TEXTSTYLE':
        settings.currentTextStyleName = str('Standard');
        break;
      case '$CELTYPE':
        settings.currentLinetypeName = str('ByLayer');
        break;
      case '$DIMSTYLE':
        settings.currentDimStyleName = str('Standard');
        break;
      case '$HANDSEED': {
        // Reserve every handle below the file's seed so new objects cannot
        // collide with ones the file will define later.
        const seed = tryToHandle(str('1'));
        if (seed !== null) this.db.observeHandle(seed);
        break;
      }
      default:
        // Everything else is kept verbatim rather than discarded.
        settings.unknownHeaderVariables.set(
          name,
          groups.map((g) => ({ code: g.code, value: g.value })),
        );
        break;
    }
  }

  // ------------------------------------------------------------------ tables

  private readTables(cursor: GroupCursor): void {
    for (;;) {
      const record = this.readRecord(cursor, ['ENDSEC']);
      if (record === null) return;
      if (record.type === 'ENDSEC') return;

      switch (record.type.toUpperCase()) {
        case 'LAYER':
          this.readLayer(record);
          break;
        case 'LTYPE':
          this.readLinetype(record);
          break;
        case 'STYLE':
          this.readTextStyle(record);
          break;
        default:
          // TABLE, ENDTAB, VPORT, VIEW, UCS, APPID, DIMSTYLE, BLOCK_RECORD:
          // either structural markers or types not yet modelled.
          break;
      }
    }
  }

  private readLayer(record: DxfRecord): void {
    const name = record.string(2, '');
    if (name === '') return;

    const layer = this.db.layers.get(name) ?? new Layer();
    const isNew = this.db.layers.get(name) === undefined;
    layer.name = name;

    const handle = tryToHandle(record.string(5));
    if (handle !== null && isNew) layer.handle = handle;

    const aci = record.integer(62, 7);
    // A negative ACI on a layer means the layer is switched off; the magnitude
    // remains the layer's colour, which must be kept for when it is turned on.
    layer.isOff = aci < 0;
    const trueColor = record.optionalNumber(420);
    layer.color =
      trueColor !== undefined ? Color.fromTrueColor(trueColor) : Color.fromIndex(Math.abs(aci));

    const flags = record.integer(70, 0);
    layer.isFrozen = (flags & 1) !== 0;
    layer.isFrozenInNewViewports = (flags & 2) !== 0;
    layer.isLocked = (flags & 4) !== 0;
    layer.isXrefDependent = (flags & 16) !== 0;

    const linetypeName = record.string(6, 'Continuous');
    layer.linetypeHandle = this.db.ensureLinetype(linetypeName).handle;
    layer.lineweight = record.integer(370, -3);
    // Code 290 is the plot flag; absent means plottable.
    layer.isPlottable = record.has(290) ? record.boolean(290, true) : true;
    layer.description = record.string(300, '');

    const transparency = record.optionalNumber(440);
    if (transparency !== undefined && transparency !== 0) {
      layer.transparency = 255 - (transparency & 0xff);
    }

    const unknown = record.unconsumed();
    if (unknown.length > 0) {
      layer.preserved = { unknownCodes: unknown, originalType: 'LAYER' };
    }

    if (isNew) this.db.addLayer(layer);
  }

  private readLinetype(record: DxfRecord): void {
    const name = record.string(2, '');
    if (name === '') return;

    const existing = this.db.linetypes.get(name);
    const linetype = existing ?? new Linetype();
    linetype.name = name;

    const handle = tryToHandle(record.string(5));
    if (handle !== null && existing === undefined) linetype.handle = handle;

    linetype.description = record.string(3, '');

    // Code 49 repeats once per dash element, with 74 flagging embedded content.
    const lengths = record.all(49).map(Number);
    const elements: LinetypeElement[] = lengths.map((length) => ({ length }));
    linetype.elements = elements;
    record.markConsumed(72, 73, 40, 74, 75, 340, 44, 45, 46, 50, 9);

    const unknown = record.unconsumed();
    if (unknown.length > 0) {
      linetype.preserved = { unknownCodes: unknown, originalType: 'LTYPE' };
    }

    if (existing === undefined) this.db.addLinetype(linetype);
  }

  private readTextStyle(record: DxfRecord): void {
    const name = record.string(2, '');
    if (name === '') return;

    const existing = this.db.textStyles.get(name);
    const style = existing ?? new TextStyle();
    style.name = name;

    const handle = tryToHandle(record.string(5));
    if (handle !== null && existing === undefined) style.handle = handle;

    style.height = record.number(40, 0);
    style.widthFactor = record.number(41, 1);
    style.obliqueAngle = degToRad(record.number(50, 0));
    style.fontFile = record.string(3, 'txt.shx');
    style.bigFontFile = record.string(4, '');
    // Code 1071 in the style's XData carries the TrueType family on some files.
    style.typeface = record.string(1000, '');

    const generationFlags = record.integer(71, 0);
    style.isBackwards = (generationFlags & 2) !== 0;
    style.isUpsideDown = (generationFlags & 4) !== 0;

    const flags = record.integer(70, 0);
    style.isShapeFile = (flags & 1) !== 0;
    style.isVertical = (flags & 4) !== 0;

    const unknown = record.unconsumed();
    if (unknown.length > 0) {
      style.preserved = { unknownCodes: unknown, originalType: 'STYLE' };
    }

    if (existing === undefined) this.db.addTextStyle(style);
  }

  // ------------------------------------------------------------------ blocks

  /**
   * Read the BLOCKS section.
   *
   * Each definition runs from a BLOCK record to its matching ENDBLK, with its
   * entities in between. Block definitions must be read before the ENTITIES
   * section resolves INSERTs against them — which the deferred-resolution pass
   * guarantees regardless of section order.
   */
  private readBlocks(cursor: GroupCursor): void {
    for (;;) {
      const record = this.readRecord(cursor, ['ENDSEC']);
      if (record === null || record.type === 'ENDSEC') return;
      if (record.type.toUpperCase() !== 'BLOCK') continue;

      const name = record.string(2, '');
      if (name === '') {
        this.skipToBlockEnd(cursor);
        continue;
      }

      const existing = this.db.blocks.get(name);
      const block = existing ?? new BlockTableRecord();
      block.name = name;

      const handle = tryToHandle(record.string(5));
      if (handle !== null && existing === undefined) block.handle = handle;

      block.origin = record.point(10);
      block.description = record.string(4, '');

      const flags = record.integer(70, 0);
      // Bit 4 marks an external reference; bit 8 an overlay.
      if ((flags & 4) !== 0) {
        block.xrefType = (flags & 8) !== 0 ? XrefType.Overlay : XrefType.Attachment;
        block.xrefPath = record.string(1, '');
        // Resolution is the xref loader's job; until it runs the reference is
        // simply unresolved rather than missing.
        block.xrefStatus = XrefStatus.Unloaded;
      }
      block.isXrefDependent = (flags & 16) !== 0;

      if (existing === undefined) this.db.addBlock(block);
      this.blockNamesToHandles.set(name.toUpperCase(), block.handle);

      this.readBlockEntities(cursor, block);
    }
  }

  private readBlockEntities(cursor: GroupCursor, block: BlockTableRecord): void {
    for (;;) {
      const record = this.readRecord(cursor, ['ENDBLK', 'ENDSEC']);
      if (record === null) return;
      const type = record.type.toUpperCase();
      if (type === 'ENDBLK') return;
      if (type === 'ENDSEC') {
        // Malformed: a block was left open. Push the marker back so the
        // section loop terminates properly rather than consuming the rest.
        cursor.pushBack({ code: 0, value: 'ENDSEC', line: 0 });
        return;
      }
      this.addEntityFromRecord(record, block, cursor);
    }
  }

  private skipToBlockEnd(cursor: GroupCursor): void {
    for (;;) {
      const group = cursor.next();
      if (group === null) return;
      if (group.code === 0 && String(group.value).toUpperCase() === 'ENDBLK') return;
    }
  }

  // ---------------------------------------------------------------- entities

  private readEntities(cursor: GroupCursor): void {
    const modelSpace = this.db.modelSpace;
    for (;;) {
      const record = this.readRecord(cursor, ['ENDSEC']);
      if (record === null || record.type === 'ENDSEC') return;
      this.addEntityFromRecord(record, modelSpace, cursor);
    }
  }

  /**
   * Convert a record to an entity and file it under `owner`.
   *
   * Handles the two DXF constructs where one logical entity spans several
   * records: legacy POLYLINE with trailing VERTEX records, and INSERT with
   * trailing ATTRIB records. Both terminate with SEQEND.
   */
  private addEntityFromRecord(
    record: DxfRecord,
    owner: BlockTableRecord,
    cursor: GroupCursor,
  ): void {
    const type = record.type.toUpperCase();
    // SEQEND and ENDBLK are structural and produce no entity.
    if (type === 'SEQEND' || type === 'ENDBLK' || type === 'BLOCK') return;

    const entity = readEntity(record, this.context);
    if (entity === null) return;

    if (type === 'POLYLINE' && entity instanceof PolylineEntity) {
      attachPolylineVertices(entity, this.collectUntilSeqEnd(cursor, 'VERTEX'));
    } else if (type === 'INSERT') {
      // Attributes follow the insert and belong to it. They are added as
      // entities in their own right so they can be selected and edited.
      const attributes = this.collectUntilSeqEnd(cursor, 'ATTRIB');
      for (const attrRecord of attributes) {
        const attribute = readEntity(attrRecord, this.context);
        if (attribute === null) continue;
        this.db.addEntity(attribute, owner);
        this.entityCount++;
        if ('attributeHandles' in entity) {
          (entity as { attributeHandles: Handle[] }).attributeHandles.push(attribute.handle);
        }
      }
    }

    this.db.addEntity(entity, owner);
    this.entityCount++;
  }

  /**
   * Collect records of `expectedType` until SEQEND.
   *
   * Anything else encountered is pushed back and terminates the run, so a file
   * that omits its SEQEND — which happens — does not swallow the rest of the
   * section.
   */
  private collectUntilSeqEnd(cursor: GroupCursor, expectedType: string): DxfRecord[] {
    const collected: DxfRecord[] = [];
    for (;;) {
      const peeked = cursor.peek();
      if (peeked === null) return collected;
      if (peeked.code !== 0) {
        cursor.next();
        continue;
      }
      const type = String(peeked.value).toUpperCase();
      if (type === 'SEQEND') {
        // Consume the SEQEND and its trailing groups.
        this.readRecord(cursor, []);
        return collected;
      }
      if (type !== expectedType) return collected;

      const record = this.readRecord(cursor, []);
      if (record === null) return collected;
      collected.push(record);
    }
  }

  // ----------------------------------------------------------------- objects

  /**
   * Read the OBJECTS section, which holds non-graphical objects.
   *
   * Only LAYOUT is interpreted today, because layouts determine what tabs the
   * user sees and which block record holds each sheet's geometry. Dictionaries,
   * groups, plot style tables and the rest are skipped for now; they are not
   * lost, because they were never modified.
   */
  private readObjects(cursor: GroupCursor): void {
    for (;;) {
      const record = this.readRecord(cursor, ['ENDSEC']);
      if (record === null || record.type === 'ENDSEC') return;
      if (record.type.toUpperCase() === 'LAYOUT') {
        this.readLayout(record);
      }
    }
  }

  private readLayout(record: DxfRecord): void {
    const layout = new Layout();

    // A LAYOUT embeds AcDbPlotSettings, so code 1 appears twice: the page
    // setup name from the plot settings first, then the layout's own name.
    // Reading only the first value gives every layout the page setup's name —
    // usually blank, which would leave the tab bar unlabelled.
    const code1Values = record.all(1).map(String);
    const pageSetupName = code1Values[0] ?? '';
    layout.name = code1Values[code1Values.length - 1] ?? 'Layout';
    // A file with only one code 1 has no separate page setup name.
    const hasSeparatePageSetup = code1Values.length > 1;

    const handle = tryToHandle(record.string(5));
    if (handle !== null) layout.handle = handle;

    layout.tabOrder = record.integer(71, 0);
    layout.limitsMin = record.point(10);
    layout.limitsMax = record.point(11);
    layout.insertionBase = record.point(12);
    layout.extentsMin = record.point(14);
    layout.extentsMax = record.point(15);
    layout.ucsOrigin = record.point(13);
    layout.ucsXAxis = record.point(16, Vec3.UNIT_X);
    layout.ucsYAxis = record.point(17, Vec3.UNIT_Y);

    const plot = layout.plotSettings;
    plot.pageSetupName = hasSeparatePageSetup ? pageSetupName : '';
    plot.plotConfigurationName = record.string(2, '');
    plot.paperSizeName = record.string(4, '');
    plot.paperWidth = record.number(44, 297);
    plot.paperHeight = record.number(45, 210);
    plot.marginLeft = record.number(40, 0);
    plot.marginBottom = record.number(41, 0);
    plot.marginRight = record.number(42, 0);
    plot.marginTop = record.number(43, 0);
    plot.scaleNumerator = record.number(142, 1);
    plot.scaleDenominator = record.number(143, 1);
    plot.plotStyleSheet = record.string(7, '');
    plot.originX = record.number(46, 0);
    plot.originY = record.number(47, 0);

    const plotFlags = record.integer(70, 0);
    plot.plotCentered = (plotFlags & 16) !== 0;
    plot.plotHidden = (plotFlags & 8) !== 0;
    plot.scaleToFit = (plotFlags & 32) !== 0;

    // 330 on a layout points at its block record.
    const blockHandle = tryToHandle(record.optionalString(330));
    if (blockHandle !== null) layout.blockTableRecordHandle = blockHandle;

    const unknown = record.unconsumed();
    if (unknown.length > 0) {
      layout.preserved = { unknownCodes: unknown, originalType: 'LAYOUT' };
    }

    this.db.addLayout(layout);

    // Link the block record back to its layout so space switching works.
    this.deferred.push(() => {
      const block = this.db.getTyped(layout.blockTableRecordHandle, BlockTableRecord);
      if (block !== undefined) {
        block.layoutHandle = layout.handle;
      }
    });
  }

  /**
   * Guarantee at least a Model layout.
   *
   * DXF files exported by lightweight tools frequently omit the OBJECTS
   * section entirely. Without a layout the UI has no tab to show and no space
   * to render, so one is synthesized over the model space block.
   */
  private ensureLayouts(): void {
    if (this.db.layouts.some((l) => l.isModelSpace)) return;
    const layout = new Layout();
    layout.name = 'Model';
    layout.tabOrder = -1;
    layout.blockTableRecordHandle = this.db.modelSpace.handle;
    this.db.addLayout(layout);
    this.db.modelSpace.layoutHandle = layout.handle;
  }

  // ------------------------------------------------------------------ shared

  /**
   * Read groups up to the next code-0 marker, returning them as a record.
   *
   * The terminating marker is left unconsumed unless it is in `stopTypes`, in
   * which case a record of that type is returned so the caller can react.
   */
  private readRecord(cursor: GroupCursor, stopTypes: readonly string[]): DxfRecord | null {
    // Find the code-0 that names this record.
    let typeGroup = cursor.next();
    while (typeGroup !== null && typeGroup.code !== 0) {
      typeGroup = cursor.next();
    }
    if (typeGroup === null) return null;

    const type = String(typeGroup.value);
    if (stopTypes.includes(type.toUpperCase())) {
      return new DxfRecord(type.toUpperCase(), []);
    }

    const groups: DxfGroup[] = [];
    for (;;) {
      const peeked = cursor.peek();
      if (peeked === null) break;
      if (peeked.code === 0) break;
      groups.push(cursor.next()!);
    }
    return new DxfRecord(type, groups);
  }
}

/** Read DXF text into a fresh database. */
export function readDxf(text: string): DxfReadResult {
  return new DxfReader().read(text);
}

// Re-exported for callers that only import this module.
export type { DxfDiagnostic };
export { Entity };
