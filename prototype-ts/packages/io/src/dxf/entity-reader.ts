import {
  ArcEntity,
  AttributeDefinitionEntity,
  AttributeEntity,
  Box3,
  CircleEntity,
  Color,
  Database,
  DbObject,
  EllipseEntity,
  Entity,
  InsertEntity,
  LineEntity,
  Lineweight,
  MTextEntity,
  PointEntity,
  PolylineEntity,
  TextEntity,
  TextHorizontalAlignment,
  TextVerticalAlignment,
  Vec3,
  ViewportEntity,
  degToRad,
  polylineVertex,
  toHandle,
  tryToHandle,
  type Handle,
  type PolylineVertex,
} from '@cad/core';
import { DxfRecord } from './record.js';
import type { DxfDiagnostic } from './tokenizer.js';

/**
 * Context shared across entity reads within one file.
 *
 * Carries the database being populated and the deferred work that cannot be
 * done until the whole file has been read — chiefly resolving names to
 * handles, since a DXF entity may reference a layer defined later in the file.
 */
export interface ReadContext {
  readonly db: Database;
  readonly diagnostics: DxfDiagnostic[];
  /** Actions to run once every object exists. */
  readonly deferred: Array<() => void>;
  /** Maps a block name to its record handle, filled as blocks are read. */
  readonly blockNamesToHandles: Map<string, Handle>;
}

/**
 * A function that turns a DXF record into an entity.
 *
 * Readers are registered in a table rather than dispatched by a switch so that
 * support for a new entity type — including from a plugin — is an entry in a
 * map, with no change to the reader's control flow.
 */
export type EntityReader = (record: DxfRecord, context: ReadContext) => Entity | null;

/**
 * Apply the properties every entity shares.
 *
 * Called for every entity before its type-specific fields, so that a new
 * entity type automatically gets correct layer, colour and linetype handling.
 */
export function readCommonProperties(
  entity: Entity,
  record: DxfRecord,
  context: ReadContext,
): void {
  const handle = tryToHandle(record.string(5));
  if (handle !== null) entity.handle = handle;

  const ownerHandle = tryToHandle(record.optionalString(330));
  if (ownerHandle !== null) entity.ownerHandle = ownerHandle;

  // Layer is named, not handle-referenced, in DXF. Creating it on demand is
  // required: real files do reference layers absent from the LAYER table.
  const layerName = record.string(8, '0');
  entity.layerHandle = context.db.ensureLayer(layerName).handle;

  // Colour: 62 is the ACI index, 420 an optional true colour that overrides it.
  const trueColor = record.optionalNumber(420);
  const aci = record.optionalNumber(62);
  if (trueColor !== undefined) {
    entity.color = Color.fromTrueColor(trueColor);
  } else if (aci !== undefined) {
    // A negative index means the entity is on a layer that is turned off; the
    // magnitude is the real colour, so the sign is not part of the value.
    entity.color = Color.fromIndex(Math.abs(aci));
    if (aci < 0) entity.visible = false;
  } else {
    entity.color = Color.BY_LAYER;
  }

  const colorName = record.optionalString(430);
  if (colorName !== undefined && trueColor !== undefined) {
    entity.color = Color.fromTrueColor(trueColor, undefined, colorName);
  }

  const linetypeName = record.optionalString(6);
  if (linetypeName !== undefined && linetypeName.toUpperCase() !== 'BYLAYER') {
    if (linetypeName.toUpperCase() === 'BYBLOCK') {
      entity.linetypeHandle = context.db.ensureLinetype('ByBlock').handle;
    } else {
      entity.linetypeHandle = context.db.ensureLinetype(linetypeName).handle;
    }
  }

  entity.linetypeScale = record.number(48, 1);
  entity.lineweight = record.integer(370, Lineweight.ByLayer);

  // 60 is a visibility flag where 1 means invisible.
  if (record.has(60)) entity.visible = record.integer(60, 0) === 0;

  // Transparency is stored as 0x02000000 | (255 - alpha).
  const rawTransparency = record.optionalNumber(440);
  if (rawTransparency !== undefined && rawTransparency !== 0) {
    entity.transparency = 255 - (rawTransparency & 0xff);
  }

  entity.normal = record.extrusion();

  for (const [app, items] of record.xdata) {
    entity.setXData(app, items);
  }
}

/** Attach whatever the reader did not understand, so a save can restore it. */
export function preserveRemainder(entity: Entity, record: DxfRecord): void {
  const unknown = record.unconsumed();
  if (unknown.length > 0) {
    entity.preserved = { unknownCodes: unknown, originalType: record.type };
  }
}

// ---------------------------------------------------------------- geometry

const readLine: EntityReader = (record) => {
  const entity = new LineEntity();
  entity.start = record.point(10);
  entity.end = record.point(11);
  entity.thickness = record.number(39, 0);
  return entity;
};

const readCircle: EntityReader = (record) => {
  const entity = new CircleEntity();
  entity.center = record.point(10);
  entity.radius = record.number(40, 1);
  entity.thickness = record.number(39, 0);
  return entity;
};

const readArc: EntityReader = (record) => {
  const entity = new ArcEntity();
  entity.center = record.point(10);
  entity.radius = record.number(40, 1);
  // DXF stores arc angles in degrees; the kernel is radians throughout.
  entity.startAngle = degToRad(record.number(50, 0));
  entity.endAngle = degToRad(record.number(51, 360));
  entity.thickness = record.number(39, 0);
  return entity;
};

const readEllipse: EntityReader = (record) => {
  const entity = new EllipseEntity();
  entity.center = record.point(10);
  entity.majorAxis = record.point(11, Vec3.UNIT_X);
  entity.ratio = record.number(40, 1);
  // 41/42 are the start and end eccentric anomaly, already in radians.
  entity.startParam = record.number(41, 0);
  entity.endParam = record.number(42, Math.PI * 2);
  return entity;
};

const readPoint: EntityReader = (record) => {
  const entity = new PointEntity();
  entity.position = record.point(10);
  entity.angle = degToRad(record.number(50, 0));
  return entity;
};

/**
 * LWPOLYLINE — the compact modern polyline.
 *
 * Vertices are a flat run of repeated 10/20 pairs with optional per-vertex
 * bulge (42) and widths (40/41). The optional codes are the difficulty: a
 * bulge appears only for vertices that have one, so bulges cannot be zipped
 * positionally against vertices. The groups are therefore walked in file order,
 * which is the only way to associate an optional code with its vertex.
 */
const readLwPolyline: EntityReader = (record) => {
  const entity = new PolylineEntity();
  const flags = record.integer(70, 0);
  entity.closed = (flags & 1) !== 0;
  entity.constantWidth = record.number(43, 0);
  entity.thickness = record.number(39, 0);
  record.markConsumed(90, 10, 20, 30, 40, 41, 42);

  const vertices: PolylineVertex[] = [];
  let x: number | null = null;
  let y = 0;
  let bulge = 0;
  let startWidth = entity.constantWidth;
  let endWidth = entity.constantWidth;

  const flush = (): void => {
    if (x === null) return;
    vertices.push(polylineVertex(new Vec3(x, y, 0), bulge, startWidth, endWidth));
    x = null;
    bulge = 0;
    startWidth = entity.constantWidth;
    endWidth = entity.constantWidth;
  };

  for (const group of record.groups) {
    switch (group.code) {
      case 10:
        // A new X starts a new vertex, so emit the one being built.
        flush();
        x = Number(group.value);
        break;
      case 20:
        y = Number(group.value);
        break;
      case 40:
        startWidth = Number(group.value);
        break;
      case 41:
        endWidth = Number(group.value);
        break;
      case 42:
        bulge = Number(group.value);
        break;
      default:
        break;
    }
  }
  flush();

  entity.vertices = vertices;
  return entity;
};

/**
 * POLYLINE — the legacy heavyweight form.
 *
 * Its vertices are separate VERTEX entities following it in the stream,
 * terminated by SEQEND. The section reader collects those and passes them
 * through {@link attachPolylineVertices}, because an entity reader sees only
 * one record.
 */
const readPolyline: EntityReader = (record) => {
  const entity = new PolylineEntity();
  entity.isLegacyFormat = true;
  const flags = record.integer(70, 0);
  entity.closed = (flags & 1) !== 0;
  entity.constantWidth = record.number(40, 0);
  entity.thickness = record.number(39, 0);
  entity.elevation = record.point(10).z;
  return entity;
};

/** Attach VERTEX records that followed a legacy POLYLINE. */
export function attachPolylineVertices(
  entity: PolylineEntity,
  vertexRecords: readonly DxfRecord[],
): void {
  const vertices: PolylineVertex[] = [];
  for (const record of vertexRecords) {
    const flags = record.integer(70, 0);
    // Flag 16 marks a spline control point and 8 a spline-fit vertex; neither
    // is a real polyline corner and including them distorts the shape.
    if ((flags & 16) !== 0) continue;
    vertices.push(
      polylineVertex(
        record.point(10),
        record.number(42, 0),
        record.number(40, entity.constantWidth),
        record.number(41, entity.constantWidth),
      ),
    );
  }
  entity.vertices = vertices;
}

// -------------------------------------------------------------------- text

/**
 * Map the DXF alignment codes onto the entity's enums.
 *
 * Code 72 conflates horizontal alignment with the Aligned and Fit modes, and
 * 73 carries vertical alignment separately — except on ATTDEF, where 74 does.
 */
function readTextAlignment(entity: TextEntity, record: DxfRecord, verticalCode: number): void {
  const horizontal = record.integer(72, 0);
  entity.horizontalAlignment =
    horizontal >= 0 && horizontal <= 5
      ? (horizontal as TextHorizontalAlignment)
      : TextHorizontalAlignment.Left;

  const vertical = record.integer(verticalCode, 0);
  entity.verticalAlignment =
    vertical >= 0 && vertical <= 3
      ? (vertical as TextVerticalAlignment)
      : TextVerticalAlignment.Baseline;
}

function readTextCommon(entity: TextEntity, record: DxfRecord, context: ReadContext): void {
  entity.position = record.point(10);
  const alignment = record.optionalPoint(11);
  // DXF always writes code 11, often as (0,0,0) when unused. Treating that as
  // a real alignment point would drag left-justified text to the origin.
  if (alignment !== undefined && !alignment.isZero()) {
    entity.alignmentPoint = alignment;
  }
  entity.height = record.number(40, 2.5);
  entity.contents = decodeDxfText(record.string(1, ''));
  entity.rotation = degToRad(record.number(50, 0));
  entity.widthFactor = record.number(41, 1);
  entity.obliqueAngle = degToRad(record.number(51, 0));
  entity.thickness = record.number(39, 0);

  const styleName = record.string(7, 'Standard');
  entity.styleHandle = context.db.ensureTextStyle(styleName).handle;

  const generationFlags = record.integer(71, 0);
  entity.isBackwards = (generationFlags & 2) !== 0;
  entity.isUpsideDown = (generationFlags & 4) !== 0;
}

const readText: EntityReader = (record, context) => {
  const entity = new TextEntity();
  readTextCommon(entity, record, context);
  readTextAlignment(entity, record, 73);
  return entity;
};

/**
 * MTEXT — multi-line text.
 *
 * Its contents may be split across several groups: code 3 carries 250-character
 * chunks and code 1 the final one. Concatenating in the right order matters, or
 * long paragraphs are truncated or scrambled.
 */
const readMText: EntityReader = (record, context) => {
  const entity = new MTextEntity();
  entity.position = record.point(10);
  entity.height = record.number(40, 2.5);
  entity.width = record.number(41, 0);

  const chunks = record.all(3).map(String);
  const final = record.string(1, '');
  entity.contents = decodeDxfText(chunks.join('') + final);

  const attachment = record.integer(71, 1);
  if (attachment >= 1 && attachment <= 9) {
    entity.attachment = attachment;
  }
  entity.lineSpacingFactor = record.number(44, 1);

  // Direction (11) takes precedence over rotation (50) when present.
  const direction = record.optionalPoint(11);
  if (direction !== undefined && !direction.isZero()) {
    entity.directionVector = direction;
    entity.rotation = direction.angle2;
  } else {
    entity.rotation = degToRad(record.number(50, 0));
  }

  const styleName = record.string(7, 'Standard');
  entity.styleHandle = context.db.ensureTextStyle(styleName).handle;
  return entity;
};

const readAttributeDefinition: EntityReader = (record, context) => {
  const entity = new AttributeDefinitionEntity();
  readTextCommon(entity, record, context);
  readTextAlignment(entity, record, 74);
  entity.tag = record.string(2, '');
  entity.prompt = record.string(3, '');
  entity.defaultValue = entity.contents;

  const flags = record.integer(70, 0);
  entity.isInvisible = (flags & 1) !== 0;
  entity.isConstant = (flags & 2) !== 0;
  entity.isVerifiable = (flags & 4) !== 0;
  entity.isPreset = (flags & 8) !== 0;
  entity.fieldLength = record.integer(73, 0);
  return entity;
};

const readAttribute: EntityReader = (record, context) => {
  const entity = new AttributeEntity();
  readTextCommon(entity, record, context);
  readTextAlignment(entity, record, 74);
  entity.tag = record.string(2, '');

  const flags = record.integer(70, 0);
  entity.isInvisible = (flags & 1) !== 0;
  entity.isConstant = (flags & 2) !== 0;
  entity.isVerifiable = (flags & 4) !== 0;
  entity.isPreset = (flags & 8) !== 0;
  entity.fieldLength = record.integer(73, 0);
  return entity;
};

// ------------------------------------------------------------------ insert

/**
 * INSERT — a block reference.
 *
 * The block is named, not handle-referenced, and may be defined after this
 * entity in the file. Resolution is therefore deferred until the whole file is
 * read, which is the general pattern for every name-based DXF reference.
 */
const readInsert: EntityReader = (record, context) => {
  const entity = new InsertEntity();
  entity.position = record.point(10);
  entity.scale = new Vec3(
    record.number(41, 1),
    record.number(42, 1),
    record.number(43, 1),
  );
  entity.rotation = degToRad(record.number(50, 0));
  entity.columnCount = record.integer(70, 1);
  entity.rowCount = record.integer(71, 1);
  entity.columnSpacing = record.number(44, 0);
  entity.rowSpacing = record.number(45, 0);

  const blockName = record.string(2, '');
  context.deferred.push(() => {
    const handle = context.blockNamesToHandles.get(blockName.toUpperCase());
    if (handle === undefined) {
      context.diagnostics.push({
        severity: 'warning',
        message: `Block reference points at undefined block ${JSON.stringify(blockName)}`,
      });
      return;
    }
    entity.blockHandle = handle;
  });
  return entity;
};

const readViewport: EntityReader = (record) => {
  const entity = new ViewportEntity();
  entity.center = record.point(10);
  entity.width = record.number(40, 100);
  entity.height = record.number(41, 75);
  entity.viewportId = record.integer(69, 2);
  entity.viewCenter = record.point(12);
  entity.viewHeight = record.number(45, 100);
  entity.viewDirection = record.point(16, Vec3.UNIT_Z);
  entity.viewTarget = record.point(17);
  entity.twistAngle = degToRad(record.number(51, 0));
  entity.lensLength = record.number(42, 50);

  // Status field: negative means off, and 0 means inactive.
  const status = record.integer(68, 1);
  entity.isOn = status > 0;

  const flags = record.integer(90, 0);
  // Bit 0x4000 is the "locked" viewport flag.
  entity.isLocked = (flags & 0x4000) !== 0;
  entity.showsGrid = (flags & 0x80) !== 0;
  entity.showsUcsIcon = (flags & 0x2_0000) !== 0;

  // Codes 331 list layers frozen in this viewport specifically.
  for (const raw of record.all(331)) {
    const handle = tryToHandle(String(raw));
    if (handle !== null) entity.freezeLayer(handle);
  }
  return entity;
};

/**
 * A placeholder for an entity type this reader does not implement.
 *
 * The alternative — dropping the entity — would silently delete geometry from
 * a professional drawing on save. A placeholder keeps every group code, so the
 * object survives a round-trip even though it cannot yet be displayed or
 * edited. It renders as nothing and does not participate in extents.
 */
export class UnsupportedEntity extends Entity {
  dxfType = '';

  override get objectType(): string {
    return `Unsupported:${this.dxfType}`;
  }

  override get bounds(): Box3 {
    // Contributes nothing to extents: an entity whose geometry we cannot read
    // would otherwise report a bogus box and break ZOOM EXTENTS.
    return Box3.EMPTY;
  }

  override toCurves(): [] {
    return [];
  }

  override distanceToPoint(): number {
    return Infinity;
  }

  override transformBy(): void {
    // Cannot transform geometry that was never interpreted. Silently ignoring
    // is the honest behaviour: the alternative is corrupting preserved data.
  }

  override clone(): UnsupportedEntity {
    const copy = new UnsupportedEntity();
    this.copyEntityInto(copy);
    copy.dxfType = this.dxfType;
    return copy;
  }

  override restoreFrom(snapshot: DbObject): void {
    if (!(snapshot instanceof UnsupportedEntity)) {
      throw new TypeError('Snapshot type mismatch restoring an unsupported entity');
    }
    this.restoreEntityFrom(snapshot);
    this.dxfType = snapshot.dxfType;
  }
}

/**
 * Decode the escape sequences DXF uses inside string values.
 *
 * `^I` is a tab and `%%d`-style codes are legacy symbol escapes that still
 * appear constantly in older drawings — degree signs on angular dimensions,
 * diameter symbols on hole callouts. Leaving them raw shows users `%%c`
 * instead of `⌀`.
 */
export function decodeDxfText(raw: string): string {
  return raw
    .replace(/\^I/g, '\t')
    .replace(/%%[dD]/g, '°')
    .replace(/%%[cC]/g, '⌀')
    .replace(/%%[pP]/g, '±')
    .replace(/%%(\d{3})/g, (_, code: string) => String.fromCharCode(Number.parseInt(code, 10)));
}

/**
 * The registry mapping DXF type names to readers.
 *
 * Exported and mutable so that a plugin can register support for an entity
 * type without modifying this file — one of the extensibility requirements.
 */
export const ENTITY_READERS = new Map<string, EntityReader>([
  ['LINE', readLine],
  ['CIRCLE', readCircle],
  ['ARC', readArc],
  ['ELLIPSE', readEllipse],
  ['POINT', readPoint],
  ['LWPOLYLINE', readLwPolyline],
  ['POLYLINE', readPolyline],
  ['TEXT', readText],
  ['MTEXT', readMText],
  ['ATTDEF', readAttributeDefinition],
  ['ATTRIB', readAttribute],
  ['INSERT', readInsert],
  ['VIEWPORT', readViewport],
]);

/** Register a reader for an additional entity type. */
export function registerEntityReader(dxfType: string, reader: EntityReader): void {
  ENTITY_READERS.set(dxfType.toUpperCase(), reader);
}

/**
 * Read one entity record, falling back to a preserving placeholder for types
 * with no registered reader.
 */
export function readEntity(record: DxfRecord, context: ReadContext): Entity | null {
  const reader = ENTITY_READERS.get(record.type.toUpperCase());
  let entity: Entity | null;

  if (reader === undefined) {
    const placeholder = new UnsupportedEntity();
    placeholder.dxfType = record.type;
    entity = placeholder;
  } else {
    try {
      entity = reader(record, context);
    } catch (error) {
      // One malformed entity must not abort a whole drawing.
      context.diagnostics.push({
        severity: 'warning',
        message: `Failed to read ${record.type}: ${
          error instanceof Error ? error.message : String(error)
        }`,
      });
      const placeholder = new UnsupportedEntity();
      placeholder.dxfType = record.type;
      entity = placeholder;
    }
  }

  if (entity === null) return null;
  readCommonProperties(entity, record, context);
  preserveRemainder(entity, record);
  return entity;
}

export { toHandle };
