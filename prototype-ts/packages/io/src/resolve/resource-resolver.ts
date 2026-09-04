import { access, stat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { dirname, isAbsolute, normalize, resolve, sep, basename } from 'node:path';

/** What kind of resource is being looked for, so the right paths are searched. */
export enum ResourceKind {
  Drawing = 'drawing',
  Image = 'image',
  Font = 'font',
  Linetype = 'linetype',
  HatchPattern = 'hatchPattern',
  PlotStyle = 'plotStyle',
}

/** Where a resource was eventually found. */
export enum ResolutionSource {
  /** The stored path was absolute and existed. */
  Absolute = 'absolute',
  /** Found relative to the host drawing. */
  DrawingRelative = 'drawingRelative',
  /** Found on a configured search path. */
  SearchPath = 'searchPath',
  /** Found by filename alone after the stored path failed. */
  FilenameFallback = 'filenameFallback',
  /** Not found anywhere. */
  NotFound = 'notFound',
}

export interface ResolutionResult {
  readonly found: boolean;
  /** Absolute path, when found. */
  readonly path: string | null;
  readonly source: ResolutionSource;
  /** The path as stored in the file, always preserved unchanged. */
  readonly originalPath: string;
  /** Every location tried, for a diagnostic the user can act on. */
  readonly attempted: readonly string[];
}

export interface ResolverOptions {
  /** Directory of the drawing doing the referencing. */
  readonly hostDirectory: string;
  /** Configured search paths, in priority order. */
  readonly searchPaths?: readonly string[];
  /**
   * Fall back to searching for the bare filename when the stored path fails.
   *
   * On by default because it is overwhelmingly what a user wants: a project
   * copied from another machine has correct filenames and wrong directories.
   */
  readonly filenameFallback?: boolean;
}

/**
 * Locates the files a drawing depends on: xrefs, raster images, fonts,
 * linetype and hatch pattern definitions, plot style tables.
 *
 * Resolution order deliberately mirrors other CAD software's, because users' folder
 * structures are built around it:
 *
 *  1. The stored path, if absolute and present.
 *  2. Relative to the host drawing's own folder. This is the case that makes
 *     a project folder portable, and is the most common in practice.
 *  3. Each configured search path in order.
 *  4. The bare filename on each of the above, as a last resort.
 *
 * The stored path is never rewritten as a side effect of resolution. A drawing
 * authored with relative paths must stay relative when saved, or the project
 * stops being portable for everyone else — a genuinely damaging silent change.
 */
export class ResourceResolver {
  private readonly hostDirectory: string;
  private readonly searchPaths: string[];
  private readonly filenameFallback: boolean;

  /**
   * Cache of resolved paths. Loading a drawing with fifty xrefs, each
   * referencing the same font, would otherwise hit the filesystem repeatedly
   * for identical questions.
   */
  private readonly cache = new Map<string, ResolutionResult>();

  constructor(options: ResolverOptions) {
    this.hostDirectory = options.hostDirectory;
    this.searchPaths = [...(options.searchPaths ?? [])];
    this.filenameFallback = options.filenameFallback ?? true;
  }

  addSearchPath(path: string): void {
    if (!this.searchPaths.includes(path)) {
      this.searchPaths.push(path);
      // Previously failed lookups might now succeed.
      this.cache.clear();
    }
  }

  get configuredSearchPaths(): readonly string[] {
    return this.searchPaths;
  }

  /**
   * Resolve a stored path to a real file.
   *
   * @param storedPath the path exactly as written in the drawing.
   * @param kind used to pick extra default locations for fonts and patterns.
   */
  async resolve(storedPath: string, kind: ResourceKind = ResourceKind.Drawing): Promise<ResolutionResult> {
    const cacheKey = `${kind}:${storedPath}`;
    const cached = this.cache.get(cacheKey);
    if (cached !== undefined) return cached;

    const result = await this.resolveUncached(storedPath, kind);
    this.cache.set(cacheKey, result);
    return result;
  }

  private async resolveUncached(
    storedPath: string,
    kind: ResourceKind,
  ): Promise<ResolutionResult> {
    const attempted: string[] = [];
    const trimmed = storedPath.trim();

    if (trimmed === '') {
      return {
        found: false,
        path: null,
        source: ResolutionSource.NotFound,
        originalPath: storedPath,
        attempted,
      };
    }

    // DWG stores Windows-style paths even in files authored on other systems,
    // so separators are normalized before anything touches the filesystem.
    const normalized = normalizeSeparators(trimmed);

    // 1. An absolute stored path.
    if (isAbsolute(normalized)) {
      attempted.push(normalized);
      if (await fileExists(normalized)) {
        return this.success(normalized, ResolutionSource.Absolute, storedPath, attempted);
      }
    }

    // 2. Relative to the host drawing.
    const hostRelative = resolve(this.hostDirectory, normalized);
    attempted.push(hostRelative);
    if (await fileExists(hostRelative)) {
      return this.success(hostRelative, ResolutionSource.DrawingRelative, storedPath, attempted);
    }

    // 3. Configured search paths, plus the defaults for this resource kind.
    for (const searchPath of [...this.searchPaths, ...defaultPathsFor(kind)]) {
      const candidate = resolve(searchPath, normalized);
      attempted.push(candidate);
      if (await fileExists(candidate)) {
        return this.success(candidate, ResolutionSource.SearchPath, storedPath, attempted);
      }
    }

    // 4. The bare filename, which rescues projects moved between machines.
    if (this.filenameFallback) {
      const name = basename(normalized);
      if (name !== normalized) {
        const roots = [this.hostDirectory, ...this.searchPaths, ...defaultPathsFor(kind)];
        for (const root of roots) {
          const candidate = resolve(root, name);
          if (attempted.includes(candidate)) continue;
          attempted.push(candidate);
          if (await fileExists(candidate)) {
            return this.success(
              candidate,
              ResolutionSource.FilenameFallback,
              storedPath,
              attempted,
            );
          }
        }
      }
    }

    return {
      found: false,
      path: null,
      source: ResolutionSource.NotFound,
      originalPath: storedPath,
      attempted,
    };
  }

  private success(
    path: string,
    source: ResolutionSource,
    originalPath: string,
    attempted: readonly string[],
  ): ResolutionResult {
    return { found: true, path, source, originalPath, attempted: [...attempted] };
  }

  /** A resolver for a nested reference, rooted at the referencing drawing. */
  forNestedDrawing(drawingPath: string): ResourceResolver {
    return new ResourceResolver({
      hostDirectory: dirname(drawingPath),
      // Nested references inherit the host's search paths, and the host's own
      // folder becomes a search path so siblings remain findable.
      searchPaths: [this.hostDirectory, ...this.searchPaths],
      filenameFallback: this.filenameFallback,
    });
  }

  clearCache(): void {
    this.cache.clear();
  }
}

/**
 * Convert stored path separators to the host platform's.
 *
 * DWG files routinely contain backslash paths regardless of the authoring
 * platform, and a drawing produced on Windows must still resolve its xrefs
 * when opened on macOS.
 */
export function normalizeSeparators(path: string): string {
  const converted = path.replace(/[\\/]+/g, sep);
  return normalize(converted);
}

/**
 * Default locations searched for a given resource kind.
 *
 * Empty for now for everything but drawings: shipping a guess at where a
 * user's fonts live is worse than searching nothing, because a wrong hit is
 * harder to diagnose than a miss. These are filled from settings instead.
 */
function defaultPathsFor(_kind: ResourceKind): readonly string[] {
  return [];
}

async function fileExists(path: string): Promise<boolean> {
  try {
    const info = await stat(path);
    if (!info.isFile()) return false;
    await access(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}
