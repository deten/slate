# ADR 0002 — Native C++ core, Qt 6 shell, GPU rendering

**Status:** Accepted — supersedes the stack chosen implicitly in ADR 0001's context
**Date:** 2026-08-11

## Context

The brief was revised to mandate the technology architecture explicitly:

- A **native C++ core** containing the document model, geometry engine, command
  system, transactions, spatial indexing and the interoperability layer.
- **Qt 6** for the desktop shell, Windows-first, macOS a realistic future.
- A **GPU-accelerated rendering abstraction** with a DirectX 12 / Vulkan backend.
- A **stable C-compatible ABI** as the plugin boundary.
- A portable executable initially.

It also names what to avoid: *"Do not use Electron or a heavy web-tech stack as
the primary application architecture"*, and *"Do not make the CAD engine
dependent on: Electron, Chromium, React, Web Canvas, Qt, DirectX, Vulkan,
Metal."*

The work completed before that revision is a TypeScript monorepo — `@cad/core`
(geometry kernel, document model, transactions, commands), `@cad/io` (DXF
reader, DWG translator interface, xref resolution) and a Canvas2D renderer —
with 241 passing tests. Measured against the revised brief, the *language and
runtime are wrong*, and mission item 10 is explicit: fix the foundation early
rather than continuing to build on it.

## Decision

Port to a native C++20 core, and keep the TypeScript work as a reference
prototype rather than deleting it.

### Layering

```
  cad_core        pure C++20. No Qt, no graphics API, no OS UI.
     |            document model, geometry, commands, undo, snaps, spatial index
     |
  cad_io          DXF/DWG interop. Depends on cad_core only.
     |
  cad_render      rendering abstraction. Depends on cad_core.
     |              +-- backend: D3D12 / Vulkan (selected at build time)
     |
  cad_abi         C-compatible plugin boundary over cad_core.
     |
  cad_app         Qt 6 shell. Depends on everything above.
```

The dependency arrows point one way only. `cad_core` compiles and its tests run
with no Qt and no GPU present, which is both an architectural requirement and
the practical reason the core can be built now while Qt is still being sourced.

### What carries over from the prototype

The prototype is not wasted, and it is not a throwaway. What transfers is the
part that took the real effort — domain knowledge and design decisions, now
validated by a passing test suite:

- the OCS arbitrary axis algorithm and its 1/64 threshold;
- the DXF bulge sign convention, verified against the semicircle case;
- Rytz's construction for recovering ellipse axes after a shear;
- normalized `[0,1]` curve parameterization enabling generic trim/extend/offset;
- whole-object snapshot undo, and explicit transaction lifetimes for
  interactive commands;
- ByLayer/ByBlock resolution against an explicit context;
- xref resolution order, cycle detection and overlay semantics;
- the DXF group-code type table and structure-preservation strategy;
- the command state-machine model and the cancel semantics it enables.

The **test suite is the specification.** Each C++ test is ported from its
TypeScript counterpart so that behaviour is pinned, not re-derived.

### Disposition of the prototype

Moved to `prototype-ts/`, excluded from the product build, retained as
executable reference. It is deleted once the C++ port reaches parity.

## Consequences

- Real cost: the port is substantial. It is nonetheless cheaper now, at ~10k
  lines, than after the UI is built on top.
- `cad_core` has no third-party dependencies beyond the standard library, so it
  builds anywhere with a C++20 compiler.
- Qt 6 is not yet installed on this machine and is not needed to make progress;
  the core and its tests come first regardless. Qt is sourced when the shell
  work begins.
- GPU backend selection (D3D12 vs Vulkan) is deferred behind the rendering
  abstraction, per the brief's instruction to evaluate rather than assume. That
  evaluation gets its own ADR when the renderer is implemented.
- Build tooling is portable (`.tools/`), matching the portable-executable goal
  and avoiding machine-wide installs.
