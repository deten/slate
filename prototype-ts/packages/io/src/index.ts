/**
 * `@cad/io` — file format readers, writers and resource resolution.
 *
 * Depends on `@cad/core` and on Node's filesystem, but never on the UI. That
 * keeps loading testable headlessly and makes a batch converter or a
 * server-side renderer possible without restructuring.
 */

export {
  DxfTokenizer,
  GroupCursor,
  tokenizeDxf,
  type DxfGroup,
  type DxfDiagnostic,
} from './dxf/tokenizer.js';
export { DxfRecord } from './dxf/record.js';
export {
  GroupValueType,
  groupValueType,
  isXDataCode,
  isPointXCode,
  dxfVersionName,
  DXF_VERSIONS,
  SECTION,
} from './dxf/group-codes.js';
export {
  DxfReader,
  readDxf,
  type DxfReadResult,
} from './dxf/dxf-reader.js';
export {
  ENTITY_READERS,
  UnsupportedEntity,
  registerEntityReader,
  readEntity,
  decodeDxfText,
  attachPolylineVertices,
  type EntityReader,
  type ReadContext,
} from './dxf/entity-reader.js';

// DWG translation
export {
  TranslatorRegistry,
  TranslationError,
  TranslationFailure,
  isDwgPath,
  isDxfPath,
  type DwgTranslator,
  type DwgVersion,
  type TranslationOptions,
  type TranslatorStatus,
} from './dwg/translator.js';
export { OdaFileConverter } from './dwg/oda-converter.js';

// Resource resolution and external references
export {
  ResourceResolver,
  ResourceKind,
  ResolutionSource,
  normalizeSeparators,
  type ResolutionResult,
  type ResolverOptions,
} from './resolve/resource-resolver.js';
export {
  XrefLoader,
  loadDrawingWithReferences,
  type XrefNode,
  type XrefLoadResult,
  type XrefLoadOptions,
} from './resolve/xref-loader.js';
