import type { Vec3 } from '../math/vec3.js';
import type { Database } from '../db/database.js';
import type { Entity } from '../db/entity.js';
import type { Transaction, TransactionManager } from '../db/transaction.js';
import type { Handle } from '../db/handle.js';

/**
 * What a command is currently asking the user for.
 *
 * A CAD command is a state machine that alternates between prompting and
 * receiving input, and it must be interruptible at every step. Modelling the
 * request as data — rather than as an `await` inside imperative command code —
 * is what makes cancellation, undo, scripting, macro playback and command
 * preview all work through one mechanism instead of four.
 */
export type InputRequest =
  | {
      readonly kind: 'point';
      readonly prompt: string;
      /** Rubber-band origin; the UI draws from here to the cursor. */
      readonly basePoint?: Vec3;
      /** Keyword alternatives the user may type instead of picking. */
      readonly keywords?: readonly CommandKeyword[];
      /** Accepted when the user presses Enter with no input. */
      readonly defaultValue?: Vec3;
      /** Allow the user to finish here rather than requiring input. */
      readonly allowEmpty?: boolean;
    }
  | {
      readonly kind: 'distance';
      readonly prompt: string;
      /** Distance may be given by picking a second point from here. */
      readonly basePoint?: Vec3;
      readonly defaultValue?: number;
      readonly keywords?: readonly CommandKeyword[];
      readonly allowNegative?: boolean;
    }
  | {
      readonly kind: 'angle';
      readonly prompt: string;
      readonly basePoint?: Vec3;
      readonly defaultValue?: number;
      readonly keywords?: readonly CommandKeyword[];
    }
  | {
      readonly kind: 'number';
      readonly prompt: string;
      readonly defaultValue?: number;
      readonly min?: number;
      readonly max?: number;
      /** Reject non-integers. */
      readonly integerOnly?: boolean;
      readonly keywords?: readonly CommandKeyword[];
    }
  | {
      readonly kind: 'string';
      readonly prompt: string;
      readonly defaultValue?: string;
      readonly keywords?: readonly CommandKeyword[];
      readonly allowEmpty?: boolean;
    }
  | {
      readonly kind: 'selection';
      readonly prompt: string;
      /** Stop as soon as this many entities are selected. */
      readonly maxCount?: number;
      readonly minCount?: number;
      /** Restrict what may be selected. */
      readonly filter?: EntityFilter;
      readonly keywords?: readonly CommandKeyword[];
      /** Offer the current selection rather than starting empty. */
      readonly usePickfirst?: boolean;
    }
  | {
      readonly kind: 'entity';
      readonly prompt: string;
      readonly filter?: EntityFilter;
      readonly keywords?: readonly CommandKeyword[];
    }
  | {
      readonly kind: 'keyword';
      readonly prompt: string;
      readonly keywords: readonly CommandKeyword[];
      readonly defaultValue?: string;
    };

/** One selectable option within a prompt, e.g. the "Chord" in ARC. */
export interface CommandKeyword {
  /** Full word, e.g. `Center`. */
  readonly name: string;
  /**
   * The shortest unambiguous prefix, conventionally the capitalized letters,
   * e.g. `C`. Typing this is equivalent to typing the whole word.
   */
  readonly shortcut: string;
  readonly description?: string;
}

/** Predicate restricting which entities a prompt accepts. */
export type EntityFilter = (entity: Entity) => boolean;

/** A value supplied in response to an {@link InputRequest}. */
export type InputValue =
  | { readonly kind: 'point'; readonly value: Vec3 }
  | { readonly kind: 'distance'; readonly value: number }
  | { readonly kind: 'angle'; readonly value: number }
  | { readonly kind: 'number'; readonly value: number }
  | { readonly kind: 'string'; readonly value: string }
  | { readonly kind: 'selection'; readonly value: readonly Handle[] }
  | { readonly kind: 'entity'; readonly value: Handle }
  | { readonly kind: 'keyword'; readonly value: string }
  /** The user pressed Enter with no input. */
  | { readonly kind: 'empty' }
  /** The user pressed Escape. */
  | { readonly kind: 'cancel' };

/**
 * What a command step returns: either another request, or completion.
 */
export type StepResult =
  | { readonly kind: 'request'; readonly request: InputRequest }
  | { readonly kind: 'done'; readonly message?: string }
  | { readonly kind: 'cancelled'; readonly message?: string }
  | { readonly kind: 'failed'; readonly message: string };

/** Convenience constructors, so command code reads as intent. */
export const step = {
  request(request: InputRequest): StepResult {
    return { kind: 'request', request };
  },
  done(message?: string): StepResult {
    return message === undefined ? { kind: 'done' } : { kind: 'done', message };
  },
  cancelled(message?: string): StepResult {
    return message === undefined ? { kind: 'cancelled' } : { kind: 'cancelled', message };
  },
  failed(message: string): StepResult {
    return { kind: 'failed', message };
  },
} as const;

/**
 * Services a running command may use.
 *
 * Deliberately narrow. A command gets the database, a transaction, the current
 * selection and a way to talk to the user — and nothing else. It cannot reach
 * the renderer or the UI, which is what keeps commands scriptable and testable
 * headlessly, and what will let the same command objects run from a plugin API.
 */
export interface CommandContext {
  readonly db: Database;
  readonly transactions: TransactionManager;
  /** The transaction this command's changes belong to. */
  readonly transaction: Transaction;
  /** Entities selected before the command started. */
  readonly initialSelection: readonly Handle[];
  /** Write a line to the command history. */
  log(message: string): void;
  /** Report an error to the user without failing the command. */
  warn(message: string): void;
  /**
   * Preview geometry to display while the command runs, replaced on each call.
   * Cleared automatically when the command ends, however it ends.
   */
  setPreview(entities: readonly Entity[]): void;
  /** Current cursor position in world coordinates, when known. */
  readonly cursorWorld: Vec3 | null;
}

/**
 * A command's executable behaviour.
 *
 * Implemented as an explicit state machine: {@link start} returns the first
 * request, and {@link supply} is called with each response and returns the
 * next step. The instance holds the command's own working state.
 */
export interface CommandInstance {
  /** Begin. Returns the first prompt, or completes immediately. */
  start(context: CommandContext): StepResult;

  /** Handle one input and return what to do next. */
  supply(input: InputValue, context: CommandContext): StepResult;

  /**
   * Called when the command ends for any reason, including cancellation and
   * failure. Use for cleanup only; the transaction is already handled.
   */
  cleanup?(context: CommandContext): void;

  /**
   * Optional live preview as the cursor moves, without consuming input.
   * Returning entities lets a command show what it is about to create.
   */
  onCursorMove?(world: Vec3, context: CommandContext): readonly Entity[] | null;
}

/** How a command interacts with the undo system. */
export enum CommandTransactionMode {
  /** Wrapped in one transaction; the normal case. */
  Single = 'single',
  /** Makes no database changes, e.g. ZOOM. Creates no undo entry. */
  ReadOnly = 'readOnly',
  /** Manages its own transactions, e.g. UNDO itself. */
  Manual = 'manual',
}

/**
 * The registration record for a command.
 *
 * Everything the application needs to expose a command — in the command line,
 * a menu, the command palette, a keyboard shortcut, or a script — comes from
 * here. A command contributed by a plugin is indistinguishable from a built-in
 * one, which is the stated requirement.
 */
export interface CommandDefinition {
  /** Stable identifier, e.g. `cad.draw.line`. Never shown to the user. */
  readonly id: string;

  /** Primary name typed at the command line, e.g. `LINE`. */
  readonly name: string;

  /** Alternative names, e.g. `L` for LINE. */
  readonly aliases?: readonly string[];

  /** Short label for menus and the palette. */
  readonly title: string;

  /** One-line description for the palette and tooltips. */
  readonly description?: string;

  /** Grouping for the palette, e.g. `Draw`, `Modify`, `View`. */
  readonly category?: string;

  /** Longer help text. */
  readonly help?: string;

  readonly transactionMode?: CommandTransactionMode;

  /**
   * `true` when the command can act on an existing selection made before it
   * started, rather than prompting for one.
   */
  readonly supportsPickfirst?: boolean;

  /**
   * `true` when repeating with Enter or Space should re-run it. Nearly all
   * drawing commands do; ones with side effects like SAVE should not.
   */
  readonly repeatable?: boolean;

  /**
   * Whether the command may run right now. Used to grey out menu items and to
   * refuse gracefully rather than failing mid-way.
   */
  isAvailable?(db: Database, selection: readonly Handle[]): boolean;

  /** Construct a fresh instance. Called once per invocation. */
  create(): CommandInstance;
}
