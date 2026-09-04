import type { CommandDefinition } from './command.js';

/** A command matched against typed text, with its match quality. */
export interface CommandMatch {
  readonly command: CommandDefinition;
  /** How the input matched, best first. */
  readonly kind: 'exact' | 'alias' | 'prefix' | 'fuzzy';
  /** Relevance for ranking, higher is better. */
  readonly score: number;
  /** Which string matched, for highlighting in the palette. */
  readonly matched: string;
}

/**
 * The catalogue of available commands.
 *
 * A registry rather than a fixed list, because the brief requires that custom
 * commands, macros and plugins be first-class and addable without modifying
 * the engine. Every consumer — the command line, the palette, menus, keyboard
 * shortcuts, and later the scripting API — reads from here, so a registered
 * command is automatically available everywhere.
 *
 * Names and aliases are case-insensitive, matching other CAD software.
 */
export class CommandRegistry {
  private readonly byId = new Map<string, CommandDefinition>();
  /** Normalized name or alias -> command id. */
  private readonly byName = new Map<string, string>();
  private readonly listeners = new Set<() => void>();

  /**
   * Register a command.
   * @throws {Error} when the id, name or an alias is already taken. Silent
   *   overwriting would let one plugin shadow another's command invisibly.
   */
  register(command: CommandDefinition): () => void {
    if (this.byId.has(command.id)) {
      throw new Error(`A command with id ${JSON.stringify(command.id)} is already registered`);
    }

    const names = [command.name, ...(command.aliases ?? [])];
    for (const name of names) {
      const key = normalize(name);
      const existing = this.byName.get(key);
      if (existing !== undefined) {
        throw new Error(
          `Command name ${JSON.stringify(name)} is already used by ${existing}`,
        );
      }
    }

    this.byId.set(command.id, command);
    for (const name of names) {
      this.byName.set(normalize(name), command.id);
    }
    this.notify();

    // Returning an unregister function is what makes plugin unloading
    // possible without the registry tracking ownership.
    return () => this.unregister(command.id);
  }

  /** Register several, rolling back if any fails, so registration is atomic. */
  registerAll(commands: readonly CommandDefinition[]): () => void {
    const disposers: Array<() => void> = [];
    try {
      for (const command of commands) {
        disposers.push(this.register(command));
      }
    } catch (error) {
      for (const dispose of disposers) dispose();
      throw error;
    }
    return () => {
      for (const dispose of disposers) dispose();
    };
  }

  unregister(id: string): boolean {
    const command = this.byId.get(id);
    if (command === undefined) return false;
    this.byId.delete(id);
    for (const [key, value] of [...this.byName]) {
      if (value === id) this.byName.delete(key);
    }
    this.notify();
    return true;
  }

  get(id: string): CommandDefinition | undefined {
    return this.byId.get(id);
  }

  /** Look up by name or alias, case-insensitively. */
  lookup(nameOrAlias: string): CommandDefinition | undefined {
    const id = this.byName.get(normalize(nameOrAlias));
    return id === undefined ? undefined : this.byId.get(id);
  }

  get all(): CommandDefinition[] {
    return [...this.byId.values()];
  }

  get size(): number {
    return this.byId.size;
  }

  /** Commands grouped by category, for building menus. */
  byCategory(): Map<string, CommandDefinition[]> {
    const groups = new Map<string, CommandDefinition[]>();
    for (const command of this.byId.values()) {
      const category = command.category ?? 'Other';
      const list = groups.get(category);
      if (list === undefined) {
        groups.set(category, [command]);
      } else {
        list.push(command);
      }
    }
    for (const list of groups.values()) {
      list.sort((a, b) => a.title.localeCompare(b.title));
    }
    return groups;
  }

  /**
   * Rank commands against typed text for the command line and palette.
   *
   * Ordering is deliberate and matters for muscle memory: an exact name always
   * wins, then an exact alias, then a prefix, then a subsequence match. A user
   * who types `L` must get LINE — because `L` is its registered alias — and
   * never some fuzzy match that happens to score higher.
   */
  search(query: string, limit = 20): CommandMatch[] {
    const trimmed = query.trim();
    if (trimmed === '') {
      return this.all
        .slice(0, limit)
        .map((command) => ({ command, kind: 'fuzzy' as const, score: 0, matched: command.title }));
    }

    const key = normalize(trimmed);
    const matches: CommandMatch[] = [];

    for (const command of this.byId.values()) {
      const names = [command.name, ...(command.aliases ?? [])];
      let best: CommandMatch | null = null;

      for (const name of names) {
        const normalized = normalize(name);
        const isPrimary = name === command.name;

        if (normalized === key) {
          // An exact alias must still outrank a prefix match on another
          // command's full name, which is why alias scores stay this high.
          const score = isPrimary ? 1000 : 900;
          const kind = isPrimary ? ('exact' as const) : ('alias' as const);
          if (best === null || score > best.score) {
            best = { command, kind, score, matched: name };
          }
          continue;
        }

        if (normalized.startsWith(key)) {
          // Shorter names are better prefix matches: typing "LI" should
          // prefer LINE over LINETYPE.
          const score = 500 - (normalized.length - key.length);
          if (best === null || score > best.score) {
            best = { command, kind: 'prefix', score, matched: name };
          }
        }
      }

      if (best === null) {
        const target = normalize(command.title);
        const subsequenceScore = fuzzyScore(key, target);
        if (subsequenceScore > 0) {
          best = { command, kind: 'fuzzy', score: subsequenceScore, matched: command.title };
        }
      }

      if (best !== null) matches.push(best);
    }

    matches.sort((a, b) => b.score - a.score || a.command.name.localeCompare(b.command.name));
    return matches.slice(0, limit);
  }

  /**
   * Autocomplete suggestion for the command line: the shortest command whose
   * name begins with what the user has typed.
   */
  autocomplete(partial: string): string | null {
    const key = normalize(partial);
    if (key === '') return null;
    let best: string | null = null;
    for (const [name, id] of this.byName) {
      if (!name.startsWith(key)) continue;
      const command = this.byId.get(id);
      if (command === undefined) continue;
      // Suggest the canonical name, not the alias that matched, so the user
      // learns the real command name.
      const suggestion = command.name;
      if (best === null || suggestion.length < best.length) best = suggestion;
    }
    return best;
  }

  addListener(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) listener();
  }
}

function normalize(name: string): string {
  return name.trim().toUpperCase();
}

/**
 * Subsequence match score: every character of `needle` must appear in
 * `haystack` in order. Consecutive matches score higher, so "ln" ranks
 * "LiNe" above a scattered match.
 */
function fuzzyScore(needle: string, haystack: string): number {
  let score = 0;
  let haystackIndex = 0;
  let consecutive = 0;

  for (const char of needle) {
    const found = haystack.indexOf(char, haystackIndex);
    if (found < 0) return 0;
    consecutive = found === haystackIndex ? consecutive + 1 : 0;
    score += 10 + consecutive * 5;
    // A match at a word boundary is more meaningful than one mid-word.
    if (found === 0 || haystack[found - 1] === ' ') score += 15;
    haystackIndex = found + 1;
  }
  // Prefer shorter targets among equally good matches.
  return Math.max(1, score - haystack.length);
}
