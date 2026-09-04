# ADR 0004 — A C ABI for the plugin boundary

**Status:** Accepted
**Date:** 2026-08-11

## Context

The brief requires that custom commands be *first-class citizens*: a command
contributed from outside must behave exactly like a built-in one, and adding
one must require no modification of the engine.

Two questions follow from that. What shape should the boundary have, and how
much of the engine should be visible through it?

## Decision

### The boundary is pure C

`src/abi/include/cad/abi/cad_abi.h` contains no C++ whatsoever: opaque handles,
fixed-width integers, error-code returns, function pointers in a versioned
table.

The alternative — exposing the core's C++ classes — was rejected. A C++
interface across a DLL boundary ties every plugin to one compiler, one standard
library, one `_ITERATOR_DEBUG_LEVEL`, one exception implementation, and breaks
on any of them changing. That is not a stable ABI; it is a build coupling
wearing one. It also excludes every language that is not C++, and a CAD product
whose plugins can only be written in C++ has ruled out most of the people who
would write them.

Specific consequences of the choice, each deliberate:

- **Errors are status codes.** An exception unwinding through a C frame is
  undefined behaviour. Every entry point from foreign code is wrapped so no
  exception can escape, and a thread-local `last_error` carries the detail.
- **No allocation crosses.** Strings the host returns go into a caller-supplied
  buffer with the standard two-call sizing pattern; strings a plugin returns are
  copied before its call returns. Neither side ever frees the other's memory,
  which is the only arrangement that survives two allocators.
- **Functions arrive in a table, not as linked exports.** Adding a function
  appends to the table. A plugin checks `struct_size` before calling anything
  newer than its own version.
- **Every struct carries `struct_size`, and zero is rejected.** Guessing a
  layout from a missing size works today and reads past the end of the struct
  the first time a field is appended.

### A plugin command *is* a `CommandInstance`

`AbiCommandInstance` adapts the C callbacks to the core's `CommandInstance`
interface, and registration produces an ordinary `CommandDefinition` in the
ordinary `CommandRegistry`.

Nothing downstream knows the difference, because there is nothing to know: the
executor, the undo system, the command line's search and autocomplete, and the
generated menus all consume the same registry. "First-class" is achieved by
having one implementation of what a command is, rather than a second tier with
its own rules. The test suite checks this the only way that means anything — by
running a plugin command through the same executor as a built-in and then
undoing it.

### The plugin does not own its transaction

A running command's transaction is handed to the plugin read-write but with no
owner recorded, so `commit_transaction` and `abort_transaction` refuse it. The
executor guarantees exactly one commit or rollback per command however it ends;
a plugin committing halfway through would break that silently, leaving the undo
stack describing a state the drawing was never in.

A plugin acting outside a command opens its own transaction and owns that one.

`CommandTransactionMode::Manual` is likewise not reachable from the ABI. The
built-in UNDO command needs it; third-party code does not.

### The registry is per-document

A `PluginHost` owns the registration tokens for one plugin, and destroying it
unregisters everything that plugin contributed. This is what makes unloading
safe — the registry cannot be left holding a function pointer into a DLL that
is no longer mapped. `PluginManager::Loaded` orders its members so the host is
destroyed before `FreeLibrary` runs; the other order produces a crash in
whatever unrelated code next opens the command palette.

### The example plugin is written in C, and is built

`examples/plugins/boltcircle` is a real drafting command — a bolt hole circle
with multi-step input, a keyword alternative, a numeric default and layer
creation — compiled as C11 and linked against the header and nothing else.

It builds as part of the normal build, on purpose. A header that claims to be
C-compatible and is only ever compiled as C++ stops being C-compatible without
anyone noticing. A plugin that could accidentally link `cad_core` would make the
boundary decorative. Both are prevented by the build rather than by discipline.

## What is deliberately not in version 1.0

- **Preview geometry.** Rubber-band preview needs entities to cross the
  boundary as owned objects. Getting that ownership model wrong is expensive to
  unpick once plugins depend on it, so it waits for a minor version. A plugin
  command works fully without it; it simply does not draw under the cursor.
- **Direct entity mutation.** A plugin can create, transform, erase and
  re-property entities, but cannot reach in and edit a polyline's vertex array.
  Every mutation the ABI offers goes through a transaction, which is what makes
  plugin work undoable by construction rather than by convention.
- **Rendering.** Nothing in the table touches the renderer. A plugin that could
  draw directly would couple third-party code to a backend that is still
  expected to change (ADR 0003).

## Consequences

- Plugins survive a compiler upgrade, a standard-library update, and a debug/
  release mismatch.
- Any language with a C FFI can write one.
- The core stays free to change internally: nothing about its layout is visible.
- The cost is a translation layer with real code in it — roughly one wrapper per
  function, each validating, unwrapping and guarding. That repetition is
  accepted rather than abstracted away, because a clever dispatcher would save
  lines exactly where clarity is worth most.
- New capability requires appending to the header and the table rather than
  simply exposing an existing class, which is a deliberate brake: everything
  that crosses is a promise that has to be kept.
