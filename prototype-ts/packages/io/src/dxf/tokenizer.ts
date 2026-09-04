import { GroupValueType, groupValueType } from './group-codes.js';

/** One (code, value) pair from a DXF stream. */
export interface DxfGroup {
  readonly code: number;
  readonly value: string | number | boolean;
  /** Source line number, for diagnostics that point at the real file. */
  readonly line: number;
}

/** A problem encountered while reading, reported rather than thrown. */
export interface DxfDiagnostic {
  readonly severity: 'error' | 'warning' | 'info';
  readonly message: string;
  readonly line?: number;
}

/**
 * Tokenizer for ASCII DXF.
 *
 * DXF is line-oriented: an integer group code on one line, its value on the
 * next. That looks trivial, and the naive `split('\n')` implementation is
 * exactly why so many DXF readers mangle real files. This one handles the
 * things real files actually contain:
 *
 *  - CRLF, LF and lone-CR line endings, sometimes mixed within one file;
 *  - leading and trailing whitespace around codes (other CAD software pads them);
 *  - a UTF-8 BOM;
 *  - `\r\n` inside string values, which must not be treated as a line break;
 *  - malformed pairs, which are reported and skipped rather than aborting the
 *    whole load — a professional drawing with one bad group is still worth
 *    opening.
 *
 * The tokenizer is an iterator rather than an array builder: a large DXF is
 * hundreds of megabytes and tens of millions of groups, and materializing them
 * all before parsing would multiply peak memory for no benefit.
 */
export class DxfTokenizer {
  private readonly text: string;
  private pos = 0;
  private lineNumber = 1;
  private readonly diagnostics: DxfDiagnostic[] = [];

  constructor(text: string) {
    // Strip a UTF-8 BOM, which otherwise makes the first group code unparseable.
    this.text = text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
  }

  get problems(): readonly DxfDiagnostic[] {
    return this.diagnostics;
  }

  /**
   * Read one raw line, advancing past whichever line terminator follows.
   * Returns `null` at end of input.
   */
  private readLine(): string | null {
    if (this.pos >= this.text.length) return null;

    const start = this.pos;
    let end = start;
    while (end < this.text.length) {
      const ch = this.text.charCodeAt(end);
      if (ch === 10 || ch === 13) break;
      end++;
    }

    const line = this.text.slice(start, end);

    // Consume the terminator: CRLF counts as one.
    if (end < this.text.length) {
      if (this.text.charCodeAt(end) === 13 && this.text.charCodeAt(end + 1) === 10) {
        end += 2;
      } else {
        end += 1;
      }
    }
    this.pos = end;
    this.lineNumber++;
    return line;
  }

  /**
   * Iterate the group pairs in the stream.
   *
   * A group whose code line is not an integer is skipped with a diagnostic —
   * along with its value line, since the stream would otherwise desynchronize
   * and every subsequent group would be misread.
   */
  *groups(): IterableIterator<DxfGroup> {
    for (;;) {
      const codeLine = this.readLine();
      if (codeLine === null) return;

      const trimmed = codeLine.trim();
      // Blank lines between groups appear in files written by some tools.
      if (trimmed === '') continue;

      const code = Number.parseInt(trimmed, 10);
      if (!Number.isFinite(code) || !/^-?\d+$/.test(trimmed)) {
        this.diagnostics.push({
          severity: 'warning',
          message: `Expected a group code, found ${JSON.stringify(
            trimmed.slice(0, 40),
          )}; skipping the pair`,
          line: this.lineNumber - 1,
        });
        // Drop the presumed value line to stay in sync.
        this.readLine();
        continue;
      }

      const valueLine = this.readLine();
      if (valueLine === null) {
        this.diagnostics.push({
          severity: 'warning',
          message: `Group code ${code} at end of file has no value`,
          line: this.lineNumber,
        });
        return;
      }

      const parsed = this.parseValue(code, valueLine);
      if (parsed === undefined) continue;

      yield { code, value: parsed, line: this.lineNumber - 1 };
    }
  }

  /**
   * Convert a raw value line to its typed form.
   * @returns `undefined` when the value is unusable, which is reported.
   */
  private parseValue(code: number, raw: string): string | number | boolean | undefined {
    const type = groupValueType(code);

    switch (type) {
      case GroupValueType.String:
      case GroupValueType.Handle:
      case GroupValueType.Binary:
        // String values keep their internal whitespace: a text entity's
        // contents may legitimately begin or end with spaces. Only the line
        // terminator has been removed.
        return raw;

      case GroupValueType.Double: {
        const value = Number.parseFloat(raw.trim());
        if (!Number.isFinite(value)) {
          this.diagnostics.push({
            severity: 'warning',
            message: `Group ${code}: expected a number, found ${JSON.stringify(raw.trim())}`,
            line: this.lineNumber - 1,
          });
          // Substituting 0 keeps the entity loadable; dropping the group would
          // shift a coordinate triple and misplace the geometry entirely.
          return 0;
        }
        return value;
      }

      case GroupValueType.Integer: {
        const value = Number.parseInt(raw.trim(), 10);
        if (!Number.isFinite(value)) {
          this.diagnostics.push({
            severity: 'warning',
            message: `Group ${code}: expected an integer, found ${JSON.stringify(raw.trim())}`,
            line: this.lineNumber - 1,
          });
          return 0;
        }
        return value;
      }

      case GroupValueType.Boolean:
        return raw.trim() !== '0';
    }
  }
}

/**
 * A cursor over a group stream with the lookahead a recursive-descent parser
 * needs.
 *
 * DXF parsing is fundamentally "consume groups until a code that starts the
 * next thing", which requires being able to peek without consuming. Buffering
 * the whole stream would defeat the streaming tokenizer, so this keeps a
 * one-item pushback slot instead.
 */
export class GroupCursor {
  private readonly iterator: Iterator<DxfGroup>;
  private pushedBack: DxfGroup | null = null;
  private exhausted = false;

  constructor(groups: Iterable<DxfGroup>) {
    this.iterator = groups[Symbol.iterator]();
  }

  /** Consume and return the next group, or `null` at end of stream. */
  next(): DxfGroup | null {
    if (this.pushedBack !== null) {
      const group = this.pushedBack;
      this.pushedBack = null;
      return group;
    }
    if (this.exhausted) return null;
    const result = this.iterator.next();
    if (result.done === true) {
      this.exhausted = true;
      return null;
    }
    return result.value;
  }

  /** Look at the next group without consuming it. */
  peek(): DxfGroup | null {
    if (this.pushedBack !== null) return this.pushedBack;
    const group = this.next();
    if (group !== null) this.pushedBack = group;
    return group;
  }

  /** Return a group to the stream so the next `next()` yields it again. */
  pushBack(group: DxfGroup): void {
    if (this.pushedBack !== null) {
      throw new Error('GroupCursor supports only a single group of pushback');
    }
    this.pushedBack = group;
  }

  get atEnd(): boolean {
    return this.peek() === null;
  }
}

/** Read a whole DXF into groups. Convenience for tests and small files. */
export function tokenizeDxf(text: string): {
  groups: DxfGroup[];
  diagnostics: readonly DxfDiagnostic[];
} {
  const tokenizer = new DxfTokenizer(text);
  const groups = [...tokenizer.groups()];
  return { groups, diagnostics: tokenizer.problems };
}
