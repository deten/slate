/**
 * DWG translation.
 *
 * DWG is a closed binary format. Rather than inventing a parser — which would
 * be both a multi-year effort and permanently behind other CAD software — this module
 * defines the *interface* through which DWG data enters the application, and
 * ships one implementation over the free ODA File Converter.
 *
 * The interface, not the converter, is the architectural commitment. A later
 * native binding to the ODA Drawings SDK (for direct read/write with no
 * intermediate file) must be substitutable here without any caller changing.
 *
 * See `docs/adr/0001-dwg-strategy.md` for the options considered and why
 * LibreDWG in particular was ruled out.
 */

/** Why a translation attempt failed, so the UI can say something useful. */
export enum TranslationFailure {
  /** No translator is installed or configured. */
  TranslatorUnavailable = 'translatorUnavailable',
  /** The translator exists but could not be executed. */
  TranslatorFailed = 'translatorFailed',
  /** The source file does not exist or cannot be read. */
  SourceUnreadable = 'sourceUnreadable',
  /** The file is not a DWG, or is a version the translator cannot handle. */
  UnsupportedFormat = 'unsupportedFormat',
  /** The translation timed out. */
  Timeout = 'timeout',
}

export class TranslationError extends Error {
  readonly reason: TranslationFailure;
  /** Guidance the UI can show the user verbatim. */
  readonly remedy: string;

  constructor(reason: TranslationFailure, message: string, remedy: string) {
    super(message);
    this.name = 'TranslationError';
    this.reason = reason;
    this.remedy = remedy;
  }
}

/** Availability of a translator, for reporting setup state in the UI. */
export interface TranslatorStatus {
  readonly available: boolean;
  readonly name: string;
  /** Version string, when the translator reports one. */
  readonly version?: string;
  /** Path to the executable or library backing it. */
  readonly path?: string;
  /** Why it is unavailable, when it is. */
  readonly problem?: string;
  /** What the user should do about it. */
  readonly remedy?: string;
}

/**
 * Converts DWG into something the application can read.
 *
 * Implementations translate to DXF text today. The return type is deliberately
 * DXF rather than a `Database`: it keeps translators simple and leaves all
 * interpretation in the one well-tested DXF reader, so a new translator cannot
 * introduce its own subtly different reading of the same drawing.
 */
export interface DwgTranslator {
  /** Stable identifier, e.g. `oda-file-converter`. */
  readonly id: string;

  /** Human-readable name for the settings UI. */
  readonly displayName: string;

  /** Check whether this translator can run right now. */
  probe(): Promise<TranslatorStatus>;

  /**
   * Translate a DWG file to DXF text.
   * @throws {TranslationError} with a reason the UI can act on.
   */
  toDxf(sourcePath: string, options?: TranslationOptions): Promise<string>;

  /** `true` when this translator can also write DWG. */
  readonly canWrite: boolean;

  /**
   * Write DXF text out as a DWG file. Optional: implementations that cannot
   * write throw {@link TranslationFailure.UnsupportedFormat}.
   */
  fromDxf?(dxfText: string, targetPath: string, version?: DwgVersion): Promise<void>;
}

/** DWG file format versions the converter understands. */
export type DwgVersion =
  | 'ACAD9'
  | 'ACAD10'
  | 'ACAD12'
  | 'ACAD13'
  | 'ACAD14'
  | 'ACAD2000'
  | 'ACAD2004'
  | 'ACAD2007'
  | 'ACAD2010'
  | 'ACAD2013'
  | 'ACAD2018';

export interface TranslationOptions {
  /** Give up after this many milliseconds. */
  readonly timeoutMs?: number;
  /** Report progress for long conversions. */
  readonly onProgress?: (message: string) => void;
  /** Abort an in-flight conversion. */
  readonly signal?: AbortSignal;
}

/**
 * Chooses among the available translators.
 *
 * A registry rather than a hard-coded choice so that installing a better
 * translator later — or a plugin providing one — changes behaviour without a
 * code change here. Probing is cached because it can involve spawning a
 * process, which is far too slow to repeat per file when loading a drawing
 * with many DWG xrefs.
 */
export class TranslatorRegistry {
  private readonly translators: DwgTranslator[] = [];
  private readonly probeCache = new Map<string, TranslatorStatus>();

  register(translator: DwgTranslator): void {
    this.translators.push(translator);
    this.probeCache.delete(translator.id);
  }

  get all(): readonly DwgTranslator[] {
    return this.translators;
  }

  /** Probe every registered translator, using cached results where present. */
  async probeAll(force = false): Promise<TranslatorStatus[]> {
    const results: TranslatorStatus[] = [];
    for (const translator of this.translators) {
      results.push(await this.probe(translator, force));
    }
    return results;
  }

  private async probe(translator: DwgTranslator, force: boolean): Promise<TranslatorStatus> {
    if (!force) {
      const cached = this.probeCache.get(translator.id);
      if (cached !== undefined) return cached;
    }
    let status: TranslatorStatus;
    try {
      status = await translator.probe();
    } catch (error) {
      status = {
        available: false,
        name: translator.displayName,
        problem: error instanceof Error ? error.message : String(error),
      };
    }
    this.probeCache.set(translator.id, status);
    return status;
  }

  /** The first available translator, or `null` when none can run. */
  async best(): Promise<DwgTranslator | null> {
    for (const translator of this.translators) {
      const status = await this.probe(translator, false);
      if (status.available) return translator;
    }
    return null;
  }

  /**
   * Translate using the best available translator.
   * @throws {TranslationError} when none is available, carrying setup guidance.
   */
  async toDxf(sourcePath: string, options?: TranslationOptions): Promise<string> {
    const translator = await this.best();
    if (translator === null) {
      const statuses = await this.probeAll();
      const detail =
        statuses.length === 0
          ? 'No DWG translators are registered.'
          : statuses.map((s) => `${s.name}: ${s.problem ?? 'unavailable'}`).join('; ');
      throw new TranslationError(
        TranslationFailure.TranslatorUnavailable,
        `Cannot open DWG files: no translator is available. ${detail}`,
        'Install the free ODA File Converter from opendesign.com and set its ' +
          'location in Settings > DWG, or convert the drawing to DXF first.',
      );
    }
    return translator.toDxf(sourcePath, options);
  }

  /** Forget cached probe results, e.g. after the user changes settings. */
  invalidate(): void {
    this.probeCache.clear();
  }
}

/** `true` when a path looks like a DWG by extension. */
export function isDwgPath(path: string): boolean {
  return /\.dwg$/i.test(path);
}

export function isDxfPath(path: string): boolean {
  return /\.dxf$/i.test(path);
}
