/**
 * `@cad/core` — the geometry kernel and document model.
 *
 * This package has no dependency on the DOM, Electron, Node's filesystem, or
 * any rendering technology. That constraint is deliberate and load-bearing:
 * it keeps the model authoritative and independently testable, and it is what
 * will allow the same core to run in a worker, on a server, or in a headless
 * batch converter without modification.
 */

// Math
export { Vec3, v } from './math/vec3.js';
export { Mat4 } from './math/mat4.js';
export { Box3 } from './math/box3.js';
export {
  TOL,
  PICK_TOL,
  makeTolerance,
  scaleTolerance,
  eq,
  eqRelative,
  isZero,
  angleEq,
  clamp,
  type Tolerance,
} from './math/tolerance.js';
export {
  TAU,
  DEG_TO_RAD,
  RAD_TO_DEG,
  degToRad,
  radToDeg,
  normalizeAngle,
  normalizeAngleSigned,
  sweepAngle,
  angleInSweep,
  angleDifference,
  snapAngle,
} from './math/angle.js';

// Geometry
export {
  Curve,
  DEFAULT_TESSELLATION,
  hasArea,
  type AreaCurve,
  type ClosestPointResult,
  type TessellationOptions,
} from './geom/curve.js';
export { LineSegment, sanitizeSplitParams } from './geom/line-segment.js';
export { Arc } from './geom/arc.js';
export { EllipseArc } from './geom/ellipse-arc.js';
export { Polyline, polylineVertex, type PolylineVertex } from './geom/polyline.js';
export {
  arbitraryAxis,
  isWorldNormal,
  ocsToWorld,
  worldToOcs,
  pointOcsToWorld,
  pointWorldToOcs,
  type CoordinateFrame,
} from './geom/ocs.js';
export {
  intersectCurves,
  intersectCurvesExtended,
  intersectLineLine,
  intersectLineArc,
  intersectArcArc,
  intersectAgainstAll,
  selfIntersections,
  type Intersection,
  type IntersectOptions,
} from './geom/intersect.js';

// Database — identity and base objects
export {
  NULL_HANDLE,
  HandleAllocator,
  toHandle,
  tryToHandle,
  isNullHandle,
  handleToNumber,
  type Handle,
} from './db/handle.js';
export {
  DbObject,
  type PreservedData,
  type XDataItem,
  type XDataTable,
} from './db/db-object.js';
export { SymbolTable, SymbolTableRecord } from './db/symbol-table.js';

// Database — properties
export {
  Color,
  ColorMethod,
  Lineweight,
  aciToRgb,
  rgbToAci,
  normalizeLineweight,
  STANDARD_LINEWEIGHTS,
  TRANSPARENCY_OPAQUE,
  ACI_BY_BLOCK,
  ACI_BY_LAYER,
  ACI_NONE,
  type Transparency,
} from './db/color.js';

// Database — symbol table records
export { Layer, LAYER_ZERO, LAYER_DEFPOINTS } from './db/layer.js';
export {
  Linetype,
  STANDARD_LINETYPES,
  LINETYPE_CONTINUOUS,
  LINETYPE_BY_LAYER,
  LINETYPE_BY_BLOCK,
  type LinetypeElement,
} from './db/linetype.js';
export { TextStyle, TEXT_STYLE_STANDARD } from './db/text-style.js';
export {
  BlockTableRecord,
  MODEL_SPACE,
  PAPER_SPACE,
  XrefType,
  XrefStatus,
} from './db/block-table-record.js';
export {
  Layout,
  PlotRotation,
  PlotType,
  defaultPlotSettings,
  type PlotSettings,
} from './db/layout.js';

// Database — entities
export {
  Entity,
  isGripEditable,
  type GripEditable,
  type GripPoint,
  type ResolvedProperties,
  type ResolutionContext,
} from './db/entity.js';
export {
  LineEntity,
  CircleEntity,
  ArcEntity,
  EllipseEntity,
  PolylineEntity,
  PointEntity,
} from './db/entities/curve-entities.js';
export {
  TextEntity,
  MTextEntity,
  TextHorizontalAlignment,
  TextVerticalAlignment,
  MTextAttachment,
  stripMTextFormatting,
  setTextMeasurer,
  measureText,
} from './db/entities/text-entities.js';
export {
  InsertEntity,
  AttributeEntity,
  AttributeDefinitionEntity,
} from './db/entities/insert-entity.js';
export { ViewportEntity } from './db/entities/viewport-entity.js';

// Database — document and history
export {
  Database,
  type DatabaseEvent,
  type DatabaseListener,
  type DatabaseIssue,
} from './db/database.js';
export {
  Transaction,
  TransactionManager,
  UndoRecord,
  ABORT,
  type Abort,
} from './db/transaction.js';
export {
  DrawingUnits,
  LinearUnitFormat,
  AngularUnitFormat,
  createDefaultSettings,
  unitConversionFactor,
  type DrawingSettings,
} from './db/settings.js';

// Geometry editing operations
export {
  offsetCurve,
  trimCurve,
  extendCurve,
  filletCurves,
  chamferLines,
  breakCurve,
  nearestPointOn,
  dividePoints,
  measurePoints,
  pointAtDistance,
  intersectionPoints,
  type TrimResult,
  type FilletResult,
  type ChamferResult,
} from './geom/operations.js';

// Object snapping
export {
  SnapEngine,
  SnapMode,
  ALL_SNAP_MODES,
  SNAP_MODE_NAMES,
  snapModeName,
  apertureInWorld,
  type SnapPoint,
  type SnapQuery,
  type SnapCandidate,
} from './snap/object-snap.js';

// Command system
export {
  CommandTransactionMode,
  step,
  type CommandDefinition,
  type CommandInstance,
  type CommandContext,
  type CommandKeyword,
  type InputRequest,
  type InputValue,
  type StepResult,
  type EntityFilter,
} from './command/command.js';
export { CommandRegistry, type CommandMatch } from './command/registry.js';
export {
  CommandExecutor,
  type CommandOutcome,
  type ExecutorState,
  type ExecutorListener,
  type HistoryEntry,
  type SelectionBridge,
} from './command/executor.js';
export { DRAW_COMMANDS, regularPolygon } from './command/draw-commands.js';
export {
  MODIFY_COMMANDS,
  entityFromCurve,
  copyProperties,
} from './command/modify-commands.js';
export { createStandardRegistry } from './command/standard-commands.js';
