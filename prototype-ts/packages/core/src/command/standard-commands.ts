import { CommandRegistry } from './registry.js';
import { DRAW_COMMANDS } from './draw-commands.js';
import { MODIFY_COMMANDS } from './modify-commands.js';

/**
 * Build a registry populated with the built-in commands.
 *
 * Deliberately a *function* returning a fresh registry rather than a shared
 * singleton: each open document gets its own, so a plugin registering a
 * command for one drawing cannot leak into another, and tests never share
 * state.
 *
 * The built-ins go through exactly the same `register` call a plugin would
 * use. If that path is broken, every command breaks — which is a much better
 * failure mode than plugin registration silently rotting because nothing
 * exercises it.
 */
export function createStandardRegistry(): CommandRegistry {
  const registry = new CommandRegistry();
  registry.registerAll(DRAW_COMMANDS);
  registry.registerAll(MODIFY_COMMANDS);
  return registry;
}
