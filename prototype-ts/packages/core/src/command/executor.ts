import type { Vec3 } from '../math/vec3.js';
import type { Database } from '../db/database.js';
import type { Entity } from '../db/entity.js';
import type { Handle } from '../db/handle.js';
import type { Transaction, TransactionManager } from '../db/transaction.js';
import {
  CommandTransactionMode,
  type CommandContext,
  type CommandDefinition,
  type CommandInstance,
  type InputRequest,
  type InputValue,
  type StepResult,
} from './command.js';
import type { CommandRegistry } from './registry.js';

/** Observable state of the executor, for driving the UI. */
export interface ExecutorState {
  /** The command currently running, if any. */
  readonly activeCommand: CommandDefinition | null;
  /** What it is asking for, if anything. */
  readonly request: InputRequest | null;
  /** Preview geometry to draw over the document. */
  readonly preview: readonly Entity[];
  /** Command-line history, newest last. */
  readonly history: readonly HistoryEntry[];
  /** The last command run, for Enter-to-repeat. */
  readonly lastCommand: CommandDefinition | null;
}

export interface HistoryEntry {
  readonly kind: 'command' | 'prompt' | 'input' | 'info' | 'warning' | 'error';
  readonly text: string;
  readonly timestamp: number;
}

export type ExecutorListener = (state: ExecutorState) => void;

/** How the executor obtains the current selection and reports changes to it. */
export interface SelectionBridge {
  get(): readonly Handle[];
  set(handles: readonly Handle[]): void;
  clear(): void;
}

/**
 * Drives command execution.
 *
 * Owns the loop that a CAD command lives inside: start it, show its prompt,
 * feed it input, repeat until it finishes, and guarantee that however it ends
 * — completion, Escape, an exception, or another command interrupting it — the
 * database is left consistent and the transaction is either committed once or
 * fully rolled back.
 *
 * Keeping this in `@cad/core`, with no UI dependency, means the same executor
 * runs commands from the command line, from a menu, from a script, from a
 * macro and from a test.
 */
export class CommandExecutor {
  private readonly db: Database;
  private readonly transactions: TransactionManager;
  private readonly registry: CommandRegistry;
  private readonly selection: SelectionBridge;

  private active: ActiveCommand | null = null;

  private currentRequest: InputRequest | null = null;
  private previewEntities: readonly Entity[] = [];
  private historyEntries: HistoryEntry[] = [];
  private lastCommandRun: CommandDefinition | null = null;

  private readonly listeners = new Set<ExecutorListener>();

  /** Cap on retained history lines. */
  maxHistory = 500;

  constructor(
    db: Database,
    transactions: TransactionManager,
    registry: CommandRegistry,
    selection: SelectionBridge,
  ) {
    this.db = db;
    this.transactions = transactions;
    this.registry = registry;
    this.selection = selection;
  }

  get state(): ExecutorState {
    return {
      activeCommand: this.active?.definition ?? null,
      request: this.currentRequest,
      preview: this.previewEntities,
      history: this.historyEntries,
      lastCommand: this.lastCommandRun,
    };
  }

  get isRunning(): boolean {
    return this.active !== null;
  }

  /**
   * Run a command by name or alias.
   *
   * Resolves when the command finishes. A command awaiting input keeps the
   * promise pending, which is what lets a caller `await execute('LINE')` and
   * receive the outcome after the user has drawn the line.
   */
  async execute(nameOrAlias: string): Promise<CommandOutcome> {
    const command = this.registry.lookup(nameOrAlias);
    if (command === undefined) {
      const suggestion = this.registry.autocomplete(nameOrAlias);
      const hint = suggestion === null ? '' : ` Did you mean ${suggestion}?`;
      this.push('error', `Unknown command: ${nameOrAlias}.${hint}`);
      return { status: 'unknown', message: `Unknown command: ${nameOrAlias}` };
    }
    return this.run(command);
  }

  /** Run a specific command definition. */
  async run(definition: CommandDefinition): Promise<CommandOutcome> {
    // Starting a command while one is running cancels the first. This matches
    // other CAD software and is far less surprising than queueing or refusing.
    if (this.active !== null) {
      this.cancel('Interrupted by another command');
    }

    const selection = this.selection.get();
    if (definition.isAvailable?.(this.db, selection) === false) {
      this.push('warning', `${definition.name} is not available right now.`);
      return { status: 'unavailable', message: `${definition.name} is not available` };
    }

    this.push('command', definition.name);

    const mode = definition.transactionMode ?? CommandTransactionMode.Single;

    // A command that awaits input outlives this function, so its transaction
    // is opened with an explicit lifetime and closed by `finishActive` —
    // which every termination path routes through. A synchronous `run` scope
    // would commit while the command was still waiting for its first point.
    const tx =
      mode === CommandTransactionMode.Single ? this.transactions.begin(definition.name) : null;

    return new Promise<CommandOutcome>((resolveOutcome) => {
      const context = new MutableContext(this.db, this.transactions, tx, selection, this);
      const instance = definition.create();

      this.active = { definition, instance, context, finish: resolveOutcome };

      let firstStep: StepResult;
      try {
        firstStep = instance.start(context);
      } catch (error) {
        this.handleThrow(error);
        return;
      }
      this.applyStep(firstStep);
    });
  }

  /**
   * Supply input to the running command.
   *
   * Every user interaction — a point picked on canvas, text typed at the
   * command line, an Escape — arrives here. Having one entry point is what
   * makes macro playback and scripting identical to interactive use.
   */
  supply(input: InputValue): void {
    const active = this.active;
    if (active === null) return;

    if (input.kind === 'cancel') {
      this.handleCancelInput(active);
      return;
    }

    this.logInput(input);

    let result: StepResult;
    try {
      result = active.instance.supply(input, active.context);
    } catch (error) {
      this.handleThrow(error);
      return;
    }
    this.applyStep(result);
  }

  /**
   * Handle Escape.
   *
   * The command is asked first, because "cancel" does not always mean "discard
   * everything". A repeating command such as OFFSET or POINT has already
   * committed real work by the time the user presses Escape to stop it, and
   * throwing that away would be wrong — other CAD software keeps it, and so does this.
   *
   * The command cannot refuse to stop: if it responds with another prompt, it
   * is cancelled regardless. Escape must always be an exit.
   */
  private handleCancelInput(active: ActiveCommand): void {
    let result: StepResult;
    try {
      result = active.instance.supply({ kind: 'cancel' }, active.context);
    } catch (error) {
      this.handleThrow(error);
      return;
    }

    if (result.kind === 'request') {
      this.cancel();
      return;
    }
    if (result.kind === 'cancelled') {
      this.push('info', '*Cancel*');
    }
    this.applyStep(result);
  }

  /** Report cursor movement, for live preview. Never consumes input. */
  moveCursor(world: Vec3): void {
    const active = this.active;
    if (active === null) return;
    active.context.setCursor(world);
    const preview = active.instance.onCursorMove?.(world, active.context);
    if (preview !== undefined && preview !== null) {
      this.previewEntities = preview;
      this.notify();
    }
  }

  /** Cancel the running command, rolling back everything it did. */
  cancel(reason = 'Cancelled'): void {
    if (this.active === null) return;
    this.finishActive({ status: 'cancelled', message: reason }, 'abort');
    this.push('info', '*Cancel*');
  }

  /** Re-run the last command, for Enter/Space repeat. */
  async repeatLast(): Promise<CommandOutcome | null> {
    const last = this.lastCommandRun;
    if (last === null || last.repeatable === false) return null;
    return this.run(last);
  }

  private applyStep(result: StepResult): void {
    switch (result.kind) {
      case 'request':
        this.currentRequest = result.request;
        this.push('prompt', result.request.prompt);
        this.notify();
        break;
      case 'done':
        if (result.message !== undefined) this.push('info', result.message);
        this.finishActive({ status: 'completed', message: result.message }, 'commit');
        break;
      case 'cancelled':
        this.finishActive(
          { status: 'cancelled', message: result.message ?? 'Cancelled' },
          'abort',
        );
        break;
      case 'failed':
        this.push('error', result.message);
        this.finishActive({ status: 'failed', message: result.message }, 'abort');
        break;
    }
  }

  private handleThrow(error: unknown): void {
    const message = error instanceof Error ? error.message : String(error);
    this.push('error', `Command failed: ${message}`);
    this.finishActive({ status: 'failed', message }, 'abort');
  }

  /**
   * End the active command exactly once, committing or rolling back.
   *
   * Every termination path routes through here, which is what makes
   * "cancellation must never corrupt the drawing" a structural property
   * rather than something each command has to remember.
   */
  private finishActive(outcome: CommandOutcome, disposition: 'commit' | 'abort'): void {
    const active = this.active;
    if (active === null) return;

    this.active = null;
    this.currentRequest = null;
    this.previewEntities = [];

    try {
      active.instance.cleanup?.(active.context);
    } catch {
      // Cleanup must never mask the real outcome.
    }

    const tx = active.context.transactionOrNull;
    if (tx !== null) {
      if (disposition === 'abort') {
        this.transactions.abort(tx);
      } else {
        this.transactions.commit(tx);
      }
    }

    if (outcome.status === 'completed') {
      this.lastCommandRun = active.definition;
    }

    active.finish(outcome);
    this.notify();
  }

  private logInput(input: InputValue): void {
    switch (input.kind) {
      case 'point':
        this.push('input', formatPoint(input.value));
        break;
      case 'distance':
      case 'angle':
      case 'number':
        this.push('input', String(input.value));
        break;
      case 'string':
      case 'keyword':
        this.push('input', input.value);
        break;
      case 'selection':
        this.push('input', `${input.value.length} selected`);
        break;
      case 'entity':
        this.push('input', 'entity picked');
        break;
      case 'empty':
        this.push('input', '');
        break;
      case 'cancel':
        break;
    }
  }

  /** @internal Used by the command context. */
  push(kind: HistoryEntry['kind'], text: string): void {
    this.historyEntries = [
      ...this.historyEntries.slice(-(this.maxHistory - 1)),
      { kind, text, timestamp: Date.now() },
    ];
    this.notify();
  }

  /** @internal */
  updatePreview(entities: readonly Entity[]): void {
    this.previewEntities = entities;
    this.notify();
  }

  /** @internal */
  get selectionBridge(): SelectionBridge {
    return this.selection;
  }

  clearHistory(): void {
    this.historyEntries = [];
    this.notify();
  }

  addListener(listener: ExecutorListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private notify(): void {
    const snapshot = this.state;
    for (const listener of this.listeners) listener(snapshot);
  }
}

export interface CommandOutcome {
  readonly status: 'completed' | 'cancelled' | 'failed' | 'unknown' | 'unavailable';
  readonly message?: string;
}

/** The currently running command and everything needed to finish it. */
interface ActiveCommand {
  readonly definition: CommandDefinition;
  readonly instance: CommandInstance;
  readonly context: MutableContext;
  /** Resolves when the command finishes, for the awaiting caller. */
  readonly finish: (result: CommandOutcome) => void;
}

/** The concrete {@link CommandContext} handed to a running command. */
class MutableContext implements CommandContext {
  readonly db: Database;
  readonly transactions: TransactionManager;
  private readonly tx: Transaction | null;
  readonly initialSelection: readonly Handle[];
  private readonly executor: CommandExecutor;
  private cursor: Vec3 | null = null;

  constructor(
    db: Database,
    transactions: TransactionManager,
    tx: Transaction | null,
    initialSelection: readonly Handle[],
    executor: CommandExecutor,
  ) {
    this.db = db;
    this.transactions = transactions;
    this.tx = tx;
    this.initialSelection = initialSelection;
    this.executor = executor;
  }

  get transaction(): Transaction {
    if (this.tx === null) {
      throw new Error(
        'This command is registered as read-only and has no transaction. ' +
          'Change its transactionMode if it needs to modify the drawing.',
      );
    }
    return this.tx;
  }

  /** @internal Nullable form, for the executor's own abort handling. */
  get transactionOrNull(): Transaction | null {
    return this.tx;
  }

  get cursorWorld(): Vec3 | null {
    return this.cursor;
  }

  setCursor(world: Vec3): void {
    this.cursor = world;
  }

  log(message: string): void {
    this.executor.push('info', message);
  }

  warn(message: string): void {
    this.executor.push('warning', message);
  }

  setPreview(entities: readonly Entity[]): void {
    this.executor.updatePreview(entities);
  }
}

function formatPoint(p: Vec3): string {
  const fmt = (n: number): string => (Number.isInteger(n) ? String(n) : n.toFixed(4));
  return p.z === 0 ? `${fmt(p.x)},${fmt(p.y)}` : `${fmt(p.x)},${fmt(p.y)},${fmt(p.z)}`;
}
