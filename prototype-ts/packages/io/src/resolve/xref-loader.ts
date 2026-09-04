import { readFile } from 'node:fs/promises';
import { dirname, resolve as resolvePath } from 'node:path';
import {
  BlockTableRecord,
  Database,
  Entity,
  Layer,
  Linetype,
  TextStyle,
  XrefStatus,
  XrefType,
  type Handle,
} from '@cad/core';
import { readDxf } from '../dxf/dxf-reader.js';
import type { DxfDiagnostic } from '../dxf/tokenizer.js';
import { isDwgPath, type TranslatorRegistry } from '../dwg/translator.js';
import { ResourceKind, ResourceResolver, ResolutionSource } from './resource-resolver.js';

/** State of one node in the reference graph, for the reference manager UI. */
export interface XrefNode {
  /** Block record in the host database representing this reference. */
  readonly blockHandle: Handle;
  readonly name: string;
  /** Path exactly as stored in the referencing drawing. */
  readonly storedPath: string;
  /** Absolute path it resolved to, when found. */
  readonly resolvedPath: string | null;
  readonly status: XrefStatus;
  readonly type: XrefType;
  /** Nesting depth; 0 for a reference attached directly to the host. */
  readonly depth: number;
  /** Names of references nested inside this one. */
  readonly children: readonly string[];
  /** Entities brought in from this reference. */
  readonly entityCount: number;
  /** Why it failed, when it did. */
  readonly problem?: string;
  readonly attemptedPaths?: readonly string[];
}

export interface XrefLoadResult {
  readonly nodes: readonly XrefNode[];
  readonly diagnostics: readonly DxfDiagnostic[];
  readonly resolvedCount: number;
  readonly failedCount: number;
}

export interface XrefLoadOptions {
  /** Maximum nesting depth. Guards against pathological reference chains. */
  readonly maxDepth?: number;
  /** Translator registry, needed when a reference points at a DWG. */
  readonly translators?: TranslatorRegistry;
  readonly onProgress?: (message: string) => void;
  /** Skip overlays, which is what other CAD software does when the host is itself an xref. */
  readonly skipOverlays?: boolean;
}

/**
 * Loads external references into a host database.
 *
 * The three requirements that shape this design:
 *
 *  1. **A missing reference must never prevent the host from opening.** Every
 *     failure is recorded on the node and in the diagnostics, and loading
 *     continues. This is why nothing here throws for a resolution failure.
 *
 *  2. **Nested references must load.** An xref may itself reference others,
 *     and professional drawings nest several deep. Each level resolves
 *     relative to *its own* file, not the host's, which is why a fresh
 *     resolver is derived per drawing.
 *
 *  3. **Cycles must be detected, not survived by luck.** A references B
 *     references A occurs in real project sets, usually by accident. The
 *     visited set is keyed on the resolved absolute path, because the same
 *     file can be reached by different stored paths.
 *
 * Content is merged into the host database under the xref's block record, with
 * symbol table records renamed to the `xrefname|recordname` convention so that
 * a layer named "Walls" in a reference never collides with the host's own.
 */
export class XrefLoader {
  private readonly host: Database;
  private readonly diagnostics: DxfDiagnostic[] = [];
  private readonly nodes: XrefNode[] = [];
  /** Absolute paths currently being loaded, for cycle detection. */
  private readonly loading = new Set<string>();
  /** Absolute paths already loaded, so a diamond loads once. */
  private readonly loaded = new Map<string, Handle>();

  constructor(host: Database) {
    this.host = host;
  }

  /**
   * Resolve and load every external reference declared in the host database.
   */
  async loadAll(
    hostPath: string,
    searchPaths: readonly string[] = [],
    options: XrefLoadOptions = {},
  ): Promise<XrefLoadResult> {
    const resolver = new ResourceResolver({
      hostDirectory: dirname(resolvePath(hostPath)),
      searchPaths,
    });

    if (hostPath !== '') this.loading.add(resolvePath(hostPath));

    await this.loadInto(this.host, resolver, 0, options);

    const resolvedCount = this.nodes.filter((n) => n.status === XrefStatus.Resolved).length;
    return {
      nodes: this.nodes,
      diagnostics: this.diagnostics,
      resolvedCount,
      failedCount: this.nodes.length - resolvedCount,
    };
  }

  /** Load the references declared by one database into the host. */
  private async loadInto(
    database: Database,
    resolver: ResourceResolver,
    depth: number,
    options: XrefLoadOptions,
  ): Promise<void> {
    const maxDepth = options.maxDepth ?? 16;
    if (depth > maxDepth) {
      this.diagnostics.push({
        severity: 'warning',
        message: `External reference nesting exceeded ${maxDepth} levels; deeper references were not loaded.`,
      });
      return;
    }

    for (const block of database.externalReferences) {
      // An overlay nested inside another xref is deliberately not loaded —
      // that is exactly what distinguishes an overlay from an attachment, and
      // it is how users prevent site plans from multiplying through a set.
      if (block.xrefType === XrefType.Overlay && depth > 0 && (options.skipOverlays ?? true)) {
        this.nodes.push({
          blockHandle: block.handle,
          name: block.name,
          storedPath: block.xrefPath,
          resolvedPath: null,
          status: XrefStatus.Unloaded,
          type: block.xrefType,
          depth,
          children: [],
          entityCount: 0,
          problem: 'Overlay references are not loaded when nested inside another reference.',
        });
        continue;
      }

      await this.loadOne(block, resolver, depth, options);
    }
  }

  private async loadOne(
    block: BlockTableRecord,
    resolver: ResourceResolver,
    depth: number,
    options: XrefLoadOptions,
  ): Promise<void> {
    options.onProgress?.(`Resolving reference ${block.name}…`);

    const resolution = await resolver.resolve(block.xrefPath, ResourceKind.Drawing);

    if (!resolution.found || resolution.path === null) {
      block.xrefStatus = XrefStatus.NotFound;
      this.diagnostics.push({
        severity: 'warning',
        message: `External reference "${block.name}" not found: ${block.xrefPath}`,
      });
      this.nodes.push({
        blockHandle: block.handle,
        name: block.name,
        storedPath: block.xrefPath,
        resolvedPath: null,
        status: XrefStatus.NotFound,
        type: block.xrefType,
        depth,
        children: [],
        entityCount: 0,
        problem: `File not found. Searched ${resolution.attempted.length} location(s).`,
        attemptedPaths: resolution.attempted,
      });
      return;
    }

    const absolute = resolvePath(resolution.path);

    // Cycle: this file is already being loaded further up the chain.
    if (this.loading.has(absolute)) {
      block.xrefStatus = XrefStatus.Circular;
      this.diagnostics.push({
        severity: 'warning',
        message: `Circular external reference detected at "${block.name}" (${absolute}); it was not loaded again.`,
      });
      this.nodes.push({
        blockHandle: block.handle,
        name: block.name,
        storedPath: block.xrefPath,
        resolvedPath: absolute,
        status: XrefStatus.Circular,
        type: block.xrefType,
        depth,
        children: [],
        entityCount: 0,
        problem: 'This reference forms a cycle and was skipped to avoid infinite recursion.',
      });
      return;
    }

    let referenced: Database;
    let childDiagnostics: readonly DxfDiagnostic[] = [];
    try {
      const { database, diagnostics } = await this.readDrawing(absolute, options);
      referenced = database;
      childDiagnostics = diagnostics;
    } catch (error) {
      block.xrefStatus = XrefStatus.Unresolved;
      const message = error instanceof Error ? error.message : String(error);
      this.diagnostics.push({
        severity: 'warning',
        message: `External reference "${block.name}" could not be read: ${message}`,
      });
      this.nodes.push({
        blockHandle: block.handle,
        name: block.name,
        storedPath: block.xrefPath,
        resolvedPath: absolute,
        status: XrefStatus.Unresolved,
        type: block.xrefType,
        depth,
        children: [],
        entityCount: 0,
        problem: message,
      });
      return;
    }

    for (const d of childDiagnostics) {
      this.diagnostics.push({
        ...d,
        message: `[${block.name}] ${d.message}`,
      });
    }

    this.loading.add(absolute);
    try {
      // Load the reference's own references first, so that when its content is
      // merged the nested geometry is already present.
      const nestedResolver = resolver.forNestedDrawing(absolute);
      const nestedLoader = new XrefLoader(referenced);
      nestedLoader.loading.add(absolute);
      for (const path of this.loading) nestedLoader.loading.add(path);
      await nestedLoader.loadInto(referenced, nestedResolver, depth + 1, options);
      this.diagnostics.push(...nestedLoader.diagnostics);
      this.nodes.push(...nestedLoader.nodes);

      const entityCount = this.mergeInto(block, referenced);

      block.xrefStatus = XrefStatus.Resolved;
      block.resolvedPath = absolute;
      this.loaded.set(absolute, block.handle);

      this.nodes.push({
        blockHandle: block.handle,
        name: block.name,
        storedPath: block.xrefPath,
        resolvedPath: absolute,
        status: XrefStatus.Resolved,
        type: block.xrefType,
        depth,
        children: nestedLoader.nodes.map((n) => n.name),
        entityCount,
      });

      if (resolution.source === ResolutionSource.FilenameFallback) {
        this.diagnostics.push({
          severity: 'info',
          message:
            `External reference "${block.name}" was found by filename at ${absolute}; ` +
            `its stored path (${block.xrefPath}) is out of date.`,
        });
      }
    } finally {
      this.loading.delete(absolute);
    }
  }

  /** Read a referenced drawing, translating from DWG when necessary. */
  private async readDrawing(
    absolutePath: string,
    options: XrefLoadOptions,
  ): Promise<{ database: Database; diagnostics: readonly DxfDiagnostic[] }> {
    let dxfText: string;
    if (isDwgPath(absolutePath)) {
      if (options.translators === undefined) {
        throw new Error(
          'This reference is a DWG file and no DWG translator is configured. ' +
            'Install the ODA File Converter to load DWG references.',
        );
      }
      dxfText = await options.translators.toDxf(absolutePath, {
        onProgress: options.onProgress,
      });
    } else {
      dxfText = await readFile(absolutePath, 'utf8');
    }
    const result = readDxf(dxfText);
    result.database.filePath = absolutePath;
    return { database: result.database, diagnostics: result.diagnostics };
  }

  /**
   * Copy a referenced drawing's model space into the host, under `block`.
   *
   * Symbol table records are renamed to `xrefname|recordname`, which is
   * other CAD software's convention and the reason a reference's layers appear in the
   * host's layer manager as a distinguishable group rather than merging into —
   * and silently changing — the host's own layers.
   */
  private mergeInto(block: BlockTableRecord, source: Database): number {
    const prefix = block.name;
    const layerMap = new Map<Handle, Handle>();
    const linetypeMap = new Map<Handle, Handle>();
    const styleMap = new Map<Handle, Handle>();
    const blockMap = new Map<Handle, Handle>();

    for (const sourceLinetype of source.linetypes.records()) {
      const copy = sourceLinetype.clone() as Linetype;
      copy.handle = '0' as Handle;
      copy.name = this.host.linetypes.uniqueName(`${prefix}|${sourceLinetype.name}`);
      this.host.addLinetype(copy);
      linetypeMap.set(sourceLinetype.handle, copy.handle);
    }

    for (const sourceStyle of source.textStyles.records()) {
      const copy = sourceStyle.clone() as TextStyle;
      copy.handle = '0' as Handle;
      copy.name = this.host.textStyles.uniqueName(`${prefix}|${sourceStyle.name}`);
      this.host.addTextStyle(copy);
      styleMap.set(sourceStyle.handle, copy.handle);
    }

    for (const sourceLayer of source.layers.records()) {
      const copy = sourceLayer.clone() as Layer;
      copy.handle = '0' as Handle;
      copy.name = this.host.layers.uniqueName(`${prefix}|${sourceLayer.name}`);
      copy.isXrefDependent = true;
      copy.linetypeHandle =
        linetypeMap.get(sourceLayer.linetypeHandle) ?? this.host.defaultLayer.linetypeHandle;
      this.host.addLayer(copy);
      layerMap.set(sourceLayer.handle, copy.handle);
    }

    // Block definitions used inside the reference come across too, otherwise
    // its INSERTs would point at nothing.
    for (const sourceBlock of source.blocks.records()) {
      if (sourceBlock.isLayoutSpace) continue;
      const copy = new BlockTableRecord();
      copy.name = this.host.blocks.uniqueName(`${prefix}|${sourceBlock.name}`);
      copy.origin = sourceBlock.origin;
      copy.isXrefDependent = true;
      this.host.addBlock(copy);
      blockMap.set(sourceBlock.handle, copy.handle);
    }

    const remap = (entity: Entity): void => {
      entity.layerHandle = layerMap.get(entity.layerHandle) ?? this.host.defaultLayer.handle;
      const mappedLinetype = linetypeMap.get(entity.linetypeHandle);
      if (mappedLinetype !== undefined) entity.linetypeHandle = mappedLinetype;
      // Text styles and nested block references are remapped through the same
      // tables; the properties are named differently per entity type, so this
      // uses a structural check rather than instanceof chains.
      const withStyle = entity as { styleHandle?: Handle };
      if (withStyle.styleHandle !== undefined) {
        withStyle.styleHandle =
          styleMap.get(withStyle.styleHandle) ?? withStyle.styleHandle;
      }
      const withBlock = entity as { blockHandle?: Handle };
      if (withBlock.blockHandle !== undefined) {
        withBlock.blockHandle = blockMap.get(withBlock.blockHandle) ?? withBlock.blockHandle;
      }
    };

    let count = 0;

    // Copy the contents of each non-layout block definition.
    for (const sourceBlock of source.blocks.records()) {
      if (sourceBlock.isLayoutSpace) continue;
      const targetHandle = blockMap.get(sourceBlock.handle);
      if (targetHandle === undefined) continue;
      const target = this.host.getTyped(targetHandle, BlockTableRecord);
      if (target === undefined) continue;
      for (const entity of source.entitiesIn(sourceBlock)) {
        const copy = entity.clone();
        copy.handle = '0' as Handle;
        remap(copy);
        this.host.addEntity(copy, target);
        count++;
      }
    }

    // Copy model space into the xref's own block record. Paper space is not
    // copied: an xref contributes model geometry, never the sheets it was
    // laid out on.
    for (const entity of source.entitiesIn(source.modelSpace)) {
      const copy = entity.clone();
      copy.handle = '0' as Handle;
      remap(copy);
      this.host.addEntity(copy, block);
      count++;
    }

    this.host.invalidateBlockExtents(block.handle);
    return count;
  }
}

/**
 * Convenience: load a drawing and all its references in one call.
 */
export async function loadDrawingWithReferences(
  filePath: string,
  options: XrefLoadOptions & { searchPaths?: readonly string[] } = {},
): Promise<{
  database: Database;
  diagnostics: readonly DxfDiagnostic[];
  xrefs: XrefLoadResult;
}> {
  let dxfText: string;
  if (isDwgPath(filePath)) {
    if (options.translators === undefined) {
      throw new Error('Opening a DWG requires a configured translator.');
    }
    dxfText = await options.translators.toDxf(filePath, { onProgress: options.onProgress });
  } else {
    dxfText = await readFile(filePath, 'utf8');
  }

  const result = readDxf(dxfText);
  result.database.filePath = resolvePath(filePath);

  const loader = new XrefLoader(result.database);
  const xrefs = await loader.loadAll(filePath, options.searchPaths ?? [], options);

  return {
    database: result.database,
    diagnostics: [...result.diagnostics, ...xrefs.diagnostics],
    xrefs,
  };
}
