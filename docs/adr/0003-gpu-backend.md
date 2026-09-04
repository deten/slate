# ADR 0003 — Rendering abstraction and GPU backend choice

**Status:** Accepted
**Date:** 2026-08-11

## Context

The brief requires a GPU-accelerated renderer behind an abstraction, and is
explicit that the choice must be reasoned rather than assumed:

> Do not choose Vulkan or DirectX 12 simply because one sounds more technically
> impressive. Evaluate the actual requirements, performance, tooling, driver
> support, implementation complexity, and cross-platform needs before committing
> to a backend.

It is equally explicit that `cad_core` must not depend on any graphics API.

### What this renderer actually has to do

2D drafting is not a typical 3D workload, and that shapes the answer:

- **Enormous primitive counts, trivial per-primitive shading.** A large
  architectural DWG is 100k–1M entities, nearly all thin lines. The work is
  vertex throughput and draw-call submission, not pixel shading.
- **Every frame is a full redraw at a new zoom.** Chord tolerance for curve
  tessellation is view-dependent (see `chordToleranceFor`), so geometry is
  re-tessellated as the user zooms.
- **Line quality is the product.** Joins, caps, dash patterns and sub-pixel
  hairlines are what a drafter judges the application by. None of these are
  free in a GPU pipeline; all are solved problems in a mature 2D rasterizer.
- **Text.** Correct SHX and TrueType rendering with hinting is a large problem
  that no graphics API helps with.

## Options considered

| Option | Verdict |
|---|---|
| **Direct3D 12** | Best Windows tooling (PIX, RenderDoc), universal driver support on the target platform, and the only API with first-class WARP software fallback. Windows-only. |
| **Vulkan** | Cross-platform, but driver quality varies markedly on the older Intel integrated GPUs common in engineering offices. Substantially more implementation surface for the same result. |
| **Metal** | macOS only; irrelevant until macOS is a real target. |
| **Direct3D 11** | Far simpler than D3D12 for this workload, excellent drivers, and the submission overhead D3D12 exists to reduce is not this renderer's bottleneck. |

## Decision

**Define the abstraction now; defer the backend commitment until it can be
measured.** Two parts:

1. **`cad_render` exposes a backend-agnostic interface** — scene building,
   viewport transforms, spatial culling, level of detail, and a
   `RenderTarget`/`RenderBackend` pair that consumes tessellated geometry.
   `cad_core` never sees it, per the layering in ADR 0002.

2. **When a backend is implemented, it will be Direct3D 12**, on these grounds
   rather than novelty:
   - Windows is the stated primary platform and D3D12 has the best tooling and
     most uniform driver behaviour there.
   - WARP gives a correct software fallback, which matters because CAD is used
     over RDP and on machines with no usable GPU driver.
   - The abstraction keeps Vulkan available for a future Linux or
     cross-platform need, and Metal for macOS.

### Why the backend is deferred rather than written now

Writing a GPU backend before there is a scene to feed it, a viewport to drive
it, and a measurable frame to profile would be guessing at the wrong problems.
The parts that determine whether large drawings are fast are *backend
independent*:

- spatial indexing, so the renderer touches only what is on screen;
- level of detail, so sub-pixel geometry costs nothing;
- view-dependent tessellation, so curves are smooth without over-tessellating;
- batching geometry into few, large buffers rather than many small draws.

Those are built first. Whether the resulting vertex stream is submitted through
D3D12, D3D11 or Vulkan is then a measurable question rather than a guess — and
if profiling shows the bottleneck is tessellation on the CPU rather than
submission on the GPU, D3D11 may well be the right answer and D3D12 pure cost.

**This ADR is revisited with profiling data before any backend code is written.**

## The profiling, 2026-08-12

`tools/bench_scene` builds synthetic drawings shaped like real ones — polylines
and block references dominant, with text, circles and arcs mixed in, spread over
a 200 m site across twelve layers — and measures scene building at 1920×1080,
best of five. Times in milliseconds on the development machine.

| entities | view | index search | tessellate | **build total** | items | culled |
|---|---|---|---|---|---|---|
| 10,000 | extents | 0.0 | 3.5 | **9.7** | 8,730 | 1,270 |
| 10,000 | room | 0.0 | 0.0 | **0.1** | 110 | 0 |
| 100,000 | extents | 0.6 | 54.1 | **114.7** | 87,294 | 12,706 |
| 100,000 | wing | 0.2 | 29.9 | **63.5** | 48,607 | 5,961 |
| 100,000 | room | 0.0 | 0.2 | **0.7** | 1,023 | 0 |
| 500,000 | extents | 6.3 | 331.5 | **644.0** | 436,290 | 63,710 |
| 500,000 | room | 0.0 | 1.3 | **4.2** | 4,961 | 0 |

Three findings, and they settle the question.

**The spatial index does its job completely.** Zoomed in to a room, half a
million entities cost 4.2 ms and produce 4,961 items. Search itself never
exceeds 6 ms even at half a million. The ordinary working case — drafting at a
zoom where you can see what you are doing — is already fast at any drawing size
this application is likely to meet, and it is fast for structural reasons rather
than by luck.

**Zoom-extents is CPU-bound in scene building, and roughly half of that is
tessellation.** At 100,000 entities, 54 ms of the 115 ms is turning curves into
points; most of the rest is constructing 87,294 `DrawItem`s and the vectors
inside them.

**None of that is GPU work.** The measurement stops at the draw call. A D3D12
backend would replace what happens *after* the 115 ms and leave every
millisecond of it in place. Submitting the same 87,294 items through D3D12
instead of QPainter cannot make a CPU-bound scene build faster.

### Decision, revised

**The backend commitment stays deferred, and it is now deferred on evidence
rather than on argument.** The next rendering optimisation is not a GPU
backend. In order of expected value:

1. **Per-item allocation.** Roughly half the extents-case cost is `DrawItem`
   construction and its vector-of-vectors. A tessellation sink writing into a
   reused arena would address it without changing what is drawn. This is the
   real work, and it makes a GPU backend *more* effective later rather than
   less, because a GPU wants large contiguous buffers anyway.
2. **Level of detail at low zoom.** Only 13% is culled at extents. There is
   room for aggregation below a size threshold, though it changes what is
   drawn and needs care.
3. **Only then, a backend** — and the D3D11-versus-D3D12 question above should
   be re-asked at that point, because a workload that is CPU-bound before
   submission is exactly the one where D3D12's submission savings are worth
   least.

Two cheap fixes were taken while measuring: properties were being resolved
twice per entity, and the layer resolution context was being rebuilt per entity
rather than cached per layer. Together worth about 6% at extents — small, which
is itself informative, and the reason the list above starts where it does.

The property that makes all of this hold is guarded by a test rather than by a
timing assertion: `SceneBuilder` must produce a small fraction of the drawing
for a zoomed-in view. Counts are deterministic; wall-clock assertions on a
shared machine are a coin toss, and if the index ever stops narrowing the
search the count is what changes first.

## Consequences

- The renderer is usable and testable headlessly before any GPU code exists,
  because scene building and culling produce plain data.
- A software rasterizer can be added as a reference implementation, which also
  makes render output testable in CI without a GPU.
- Committing to D3D12 later costs nothing that is not already required; choosing
  it now would risk building the wrong thing carefully.
- Text rendering is explicitly *not* solved by this decision and gets its own
  treatment when the shell exists.
