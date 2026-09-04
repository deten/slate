import { spawn } from 'node:child_process';
import { access, mkdtemp, readdir, readFile, rm, copyFile, stat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, extname, join } from 'node:path';
import {
  TranslationError,
  TranslationFailure,
  type DwgTranslator,
  type DwgVersion,
  type TranslationOptions,
  type TranslatorStatus,
} from './translator.js';

/**
 * Default install locations of the ODA File Converter, by platform.
 *
 * The converter has no registry entry or PATH presence on Windows, so the
 * well-known paths are searched. Users who install elsewhere set an explicit
 * path in settings, which always takes precedence.
 */
const DEFAULT_PATHS: Readonly<Record<string, readonly string[]>> = {
  win32: [
    'C:\\Program Files\\ODA\\ODAFileConverter\\ODAFileConverter.exe',
    'C:\\Program Files\\ODA\\ODAFileConverter 25.4.0\\ODAFileConverter.exe',
    'C:\\Program Files (x86)\\ODA\\ODAFileConverter\\ODAFileConverter.exe',
  ],
  darwin: [
    '/Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter',
    '/Applications/ODAFileConverter.app/Contents/Resources/ODAFileConverter',
  ],
  linux: [
    '/usr/bin/ODAFileConverter',
    '/usr/local/bin/ODAFileConverter',
    '/opt/ODAFileConverter/ODAFileConverter',
  ],
};

/**
 * DWG translation via the ODA File Converter.
 *
 * The converter is a free, no-account, cross-platform command-line tool from
 * the Open Design Alliance. Its interface is directory-oriented rather than
 * file-oriented:
 *
 * ```
 *   ODAFileConverter <inDir> <outDir> <outVer> <outType> <recurse> <audit> [filter]
 * ```
 *
 * That shape drives the implementation: each conversion runs in a private
 * temporary directory containing exactly one file, so that a folder of
 * hundreds of drawings is never converted wholesale as a side effect of
 * opening one of them.
 *
 * The converter also exits 0 in some failure cases and writes nothing, so
 * success is determined by the output file existing — not by the exit code.
 */
export class OdaFileConverter implements DwgTranslator {
  readonly id = 'oda-file-converter';
  readonly displayName = 'ODA File Converter';
  /** The CLI can write DWG, so round-tripping is possible once wired up. */
  readonly canWrite = true;

  /** Explicit executable path, overriding the default search. */
  private explicitPath: string | null;

  constructor(explicitPath?: string) {
    this.explicitPath = explicitPath ?? null;
  }

  setPath(path: string | null): void {
    this.explicitPath = path;
  }

  /** Locate the executable, or `null` when it cannot be found. */
  private async findExecutable(): Promise<string | null> {
    if (this.explicitPath !== null) {
      return (await isExecutable(this.explicitPath)) ? this.explicitPath : null;
    }
    const candidates = DEFAULT_PATHS[process.platform] ?? [];
    for (const candidate of candidates) {
      if (await isExecutable(candidate)) return candidate;
    }
    return null;
  }

  async probe(): Promise<TranslatorStatus> {
    const path = await this.findExecutable();
    if (path === null) {
      return {
        available: false,
        name: this.displayName,
        problem: 'The ODA File Converter executable was not found.',
        remedy:
          'Download the free ODA File Converter from opendesign.com/guestfiles/oda_file_converter ' +
          'and install it, or set its path in Settings > DWG.',
      };
    }
    return { available: true, name: this.displayName, path };
  }

  async toDxf(sourcePath: string, options: TranslationOptions = {}): Promise<string> {
    const executable = await this.findExecutable();
    if (executable === null) {
      const status = await this.probe();
      throw new TranslationError(
        TranslationFailure.TranslatorUnavailable,
        status.problem ?? 'ODA File Converter is not available',
        status.remedy ?? '',
      );
    }

    if (!(await isReadableFile(sourcePath))) {
      throw new TranslationError(
        TranslationFailure.SourceUnreadable,
        `Cannot read ${sourcePath}`,
        'Check that the file exists and that you have permission to read it.',
      );
    }

    // The converter works on directories, so give it one holding only this
    // file. Without this, opening one drawing would convert every drawing
    // sitting beside it.
    const workRoot = await mkdtemp(join(tmpdir(), 'cad-dwg-'));
    const inDir = join(workRoot, 'in');
    const outDir = join(workRoot, 'out');

    try {
      await mkdirp(inDir);
      await mkdirp(outDir);

      const stagedName = `${basename(sourcePath, extname(sourcePath))}.dwg`;
      await copyFile(sourcePath, join(inDir, stagedName));

      options.onProgress?.(`Converting ${basename(sourcePath)} with ${this.displayName}…`);

      await runProcess(
        executable,
        [
          inDir,
          outDir,
          // Output version: 2018 is the newest the converter emits and loses
          // the least. The DXF reader accepts every version regardless.
          'ACAD2018',
          // Output type: DXF, ASCII rather than binary, because the reader is
          // an ASCII reader and the size difference is not worth a second path.
          'DXF',
          // Recurse: no. The staging directory is flat by construction.
          '0',
          // Audit: yes. It repairs common corruption rather than refusing.
          '1',
        ],
        options,
      );

      const produced = await findProducedFile(outDir, '.dxf');
      if (produced === null) {
        // The converter frequently exits 0 having written nothing, so this —
        // not the exit code — is the real success test.
        throw new TranslationError(
          TranslationFailure.UnsupportedFormat,
          `${this.displayName} produced no output for ${basename(sourcePath)}.`,
          'The file may be corrupt, password-protected, or a DWG version this ' +
            'converter does not support. Try opening it in other CAD software and saving ' +
            'to an older version.',
        );
      }

      return await readFile(produced, 'utf8');
    } finally {
      // Always clean up, including on failure: these are full copies of the
      // user's drawings sitting in the temp directory.
      await rm(workRoot, { recursive: true, force: true }).catch(() => undefined);
    }
  }

  async fromDxf(dxfText: string, targetPath: string, version: DwgVersion = 'ACAD2018'): Promise<void> {
    const executable = await this.findExecutable();
    if (executable === null) {
      throw new TranslationError(
        TranslationFailure.TranslatorUnavailable,
        'ODA File Converter is not available',
        'Install the ODA File Converter to save in DWG format.',
      );
    }

    const workRoot = await mkdtemp(join(tmpdir(), 'cad-dwg-out-'));
    const inDir = join(workRoot, 'in');
    const outDir = join(workRoot, 'out');
    try {
      await mkdirp(inDir);
      await mkdirp(outDir);
      const stem = basename(targetPath, extname(targetPath));
      await writeTextFile(join(inDir, `${stem}.dxf`), dxfText);

      await runProcess(executable, [inDir, outDir, version, 'DWG', '0', '1'], {});

      const produced = await findProducedFile(outDir, '.dwg');
      if (produced === null) {
        throw new TranslationError(
          TranslationFailure.TranslatorFailed,
          'The converter produced no DWG output.',
          'Check that the drawing contains no unsupported content.',
        );
      }
      await mkdirp(dirname(targetPath));
      await copyFile(produced, targetPath);
    } finally {
      await rm(workRoot, { recursive: true, force: true }).catch(() => undefined);
    }
  }
}

// --------------------------------------------------------------- utilities

async function isExecutable(path: string): Promise<boolean> {
  try {
    await access(path, constants.X_OK);
    return true;
  } catch {
    // On Windows the X_OK check is unreliable; fall back to existence.
    try {
      const info = await stat(path);
      return info.isFile();
    } catch {
      return false;
    }
  }
}

async function isReadableFile(path: string): Promise<boolean> {
  try {
    const info = await stat(path);
    if (!info.isFile()) return false;
    await access(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

async function mkdirp(path: string): Promise<void> {
  const { mkdir } = await import('node:fs/promises');
  await mkdir(path, { recursive: true });
}

async function writeTextFile(path: string, contents: string): Promise<void> {
  const { writeFile } = await import('node:fs/promises');
  await writeFile(path, contents, 'utf8');
}

/** First file in `dir` with the given extension. */
async function findProducedFile(dir: string, extension: string): Promise<string | null> {
  try {
    const names = await readdir(dir);
    const match = names.find((n) => n.toLowerCase().endsWith(extension));
    return match === undefined ? null : join(dir, match);
  } catch {
    return null;
  }
}

/**
 * Run a process to completion.
 *
 * A timeout is mandatory rather than optional: the ODA converter is a GUI
 * application running headless, and on some systems it can hang waiting for a
 * window server rather than exiting. Without a timeout that hangs the whole
 * application on file open.
 */
function runProcess(
  executable: string,
  args: readonly string[],
  options: TranslationOptions,
): Promise<void> {
  const timeoutMs = options.timeoutMs ?? 120_000;

  return new Promise<void>((resolve, reject) => {
    const child = spawn(executable, [...args], {
      // The converter needs a display on Linux; a headless run is expected to
      // fail cleanly rather than block, which the timeout below enforces.
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
    });

    let stderr = '';
    let settled = false;

    const finish = (fn: () => void): void => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      fn();
    };

    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      finish(() =>
        reject(
          new TranslationError(
            TranslationFailure.Timeout,
            `Conversion timed out after ${timeoutMs}ms.`,
            'The converter may be waiting for a display. Try converting the ' +
              'file to DXF manually, or increase the timeout in settings.',
          ),
        ),
      );
    }, timeoutMs);

    options.signal?.addEventListener('abort', () => {
      child.kill('SIGKILL');
      finish(() =>
        reject(
          new TranslationError(
            TranslationFailure.TranslatorFailed,
            'Conversion was cancelled.',
            '',
          ),
        ),
      );
    });

    child.stderr?.on('data', (chunk: Buffer) => {
      stderr += chunk.toString();
    });

    child.on('error', (error) => {
      finish(() =>
        reject(
          new TranslationError(
            TranslationFailure.TranslatorFailed,
            `Could not run ${executable}: ${error.message}`,
            'Check that the ODA File Converter is installed correctly.',
          ),
        ),
      );
    });

    child.on('close', (code) => {
      // A non-zero exit is reported, but a zero exit is not taken as success:
      // the caller verifies that an output file actually appeared.
      if (code !== 0 && code !== null) {
        finish(() =>
          reject(
            new TranslationError(
              TranslationFailure.TranslatorFailed,
              `Converter exited with code ${code}. ${stderr.trim()}`.trim(),
              'Check that the file is a valid DWG and is not password-protected.',
            ),
          ),
        );
        return;
      }
      finish(resolve);
    });
  });
}
