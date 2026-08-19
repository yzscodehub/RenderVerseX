# M2 Metal Correctness Closure Implementation Plan

**Status:** Proposed; best-practice review incorporated
**Parent:** `2026-07-26-m2-tier1-rhi-proof-master-plan.md`
**Depends on:** Frozen shared conformance, pipeline, descriptor, and state
contracts

## Goal

Prove the shared base RHI contract on a real macOS Metal device. Compilation on
Windows or cross-generated MSL is preparation only and cannot declare support.

## Translation Decisions

### Shader and pipeline interface

- Preserve normalized vertex location/format identity through the
  SPIR-V/MSL translation path.
- Build `MTLVertexDescriptor` from the validated RHI layout.
- Validate color/depth/sample compatibility before creating the render
  pipeline state.
- Return structured compiler/pipeline errors with entry-point and attachment
  identity.

### Resource binding

- Every required buffer, texture, and sampler slot is explicitly bound.
- Optional inputs use owned fallback resources; nil binding is not treated as
  a portable default.
- Descriptor/argument snapshots remain immutable during their GPU lifetime.
- Argument buffers are optional optimization, not a base-tier requirement.

### Access, content validity, and hazards

Metal does not expose DX12-style state transitions, but RenderGraph intent
still controls:

- resource usage declaration;
- render/compute/blit encoder boundaries;
- read/write hazards;
- synchronization between command encoders/queues;
- lifetime until command-buffer completion.

Tracked hazard mode is the base-tier correctness default. The backend records
which dependency is satisfied by automatic tracking, encoder ordering, a
memory barrier, a fence/event, or command-buffer ordering. Untracked hazard
mode is a later opt-in optimization and requires explicit synchronization
proof.

Content validity remains independent of hazard state. A discardable attachment
may use an appropriate load action, but pool reuse must still retain the last
realized access/domain snapshot. The backend must not discard shared
diagnostics merely because no DX12-style transition enum is emitted.

### Surface ownership

Preserve the frozen M1 boundary:

- `CAMetalLayer` attach/detach on the application main thread;
- Render Thread consumes the published native surface and owns Metal/RHI work;
- resize/generation changes are explicit;
- no AppKit/UIKit calls migrate into Render.

## Implementation Steps

### MT1: Add translation regressions

- [ ] Normalized vertex inputs produce deterministic Metal buffer/attribute
  indices and formats.
- [ ] Missing or incompatible attributes fail before pipeline creation.
- [ ] Attachment/sample mismatch returns a stable reason.
- [ ] Required binding completeness is validated.
- [ ] Graph read/write intent produces expected encoder usage/hazard records.
- [ ] Same-usage write/read and write/write dependencies remain visible.
- [ ] Discardable and preserved attachment contents select different load
  behavior without erasing the realized access snapshot.

### MT2: Implement shader/pipeline and binding translation

- [ ] Carry the shared shader interface through MSL generation and Metal shader
  objects.
- [ ] Integrate common pipeline preflight.
- [ ] Map descriptor snapshots to direct bindings or argument buffers without
  changing shared semantics.
- [ ] Retire replaced binding snapshots on command-buffer completion.

### MT3: Implement graph dependency/hazard translation

- [ ] Consume imported/transient access/content snapshots.
- [ ] Keep tracked hazard mode as the base-tier default.
- [ ] Translate dependencies to resource usage declarations, encoder
  boundaries, memory barriers, fences/events, or command-buffer ordering as
  required by the concrete producer/consumer pair.
- [ ] Track realized encoder and physical-domain ownership.
- [ ] Preserve subresource/range diagnostics where Metal binding granularity
  permits.
- [ ] Publish final snapshots to persistent owners and the transient pool.
- [ ] Keep any untracked-hazard path disabled until focused native tests prove
  every required explicit dependency.
- [ ] Add named debug groups/markers for conformance cases.

### MT4: Run real-device offscreen conformance

- [ ] Resource/view
- [ ] Complete binding snapshot
- [ ] Graphics/compute pipeline
- [ ] Compute write then graphics read
- [ ] Blit upload/readback
- [ ] Indirect arguments where reported supported
- [ ] Fence/event and completion
- [ ] Timestamp/query where reported supported
- [ ] Deferred destruction

### MT5: Run surface and bounded integration

- [ ] Acquire/present deterministic frames through `CAMetalLayer`.
- [ ] Process resize and surface generation retirement.
- [ ] Prove idle behavior.
- [ ] Run the bounded reference scene.
- [ ] Capture conformance JSON, native diagnostics, and screenshot/diff.

## Primary File Scope

- `RHI_Metal/Private/MetalPipeline.*`
- `RHI_Metal/Private/MetalResources.*`
- `RHI_Metal/Private/MetalCommandContext.*`
- Metal device/swapchain/surface files selected by implementation
- `ShaderCompiler/Private/SPIRVCrossTranslator.*`
- `ShaderCompiler/Private/DXCCompiler_Apple.cpp`
- shared conformance tests
- Metal/native lifecycle validation targets
- macOS CI scripts/workflows

## Validation

On macOS:

```text
RHIConformanceValidation --backend metal --validation --surface --report <path>
Metal validation/native lifecycle target
bounded ModelViewer Metal smoke
```

The runner publishes actual Mac model, GPU, OS, Metal device identity, and
whether any case used an optional fallback.

## Stop Conditions

Stop and return to shared-contract review if implementation would:

- report compile-only MSL as a real-device pass;
- move `CAMetalLayer` attachment to Render Thread;
- use nil required resources as normal portable behavior;
- enable untracked hazards as the default without explicit barrier/fence
  evidence;
- serialize every frame with a device-wide wait;
- hide command-buffer or pipeline errors.

## Exit Gate

- every required shared case passes on a real macOS Metal device;
- pipeline/binding/command-buffer diagnostics contain no unexpected warnings
  or errors;
- surface, resize, idle, and bounded integration cases pass;
- no Metal change weakens DX12 or Vulkan shared semantics.
