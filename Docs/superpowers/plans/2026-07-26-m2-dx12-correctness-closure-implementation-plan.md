# M2 DX12 Correctness Closure Implementation Plan

**Status:** Complete (2026-08-01); native, visual, fallback, and dedicated GBV gates passed
**Parent:** `2026-07-26-m2-tier1-rhi-proof-master-plan.md`
**Depends on:** Shared RHI conformance Tasks CH1-CH5
**Scope:** Native DX12 translation and the current ModelViewer regression

## Observed Red Baseline

The bounded local `ModelViewer` run starts and shuts down normally, but its
stdout contains repeated DX12 errors:

```text
Failed to create graphics pipeline state: 0x80070057
Pipeline: GPUDrivenOpaquePipeline
InputLayout elements: 4
CreateInputLayout: expected BLENDINDICES0
CreateInputLayout: expected BLENDWEIGHT0
```

The Task 20 baseline was refreshed on
`6c93300618026ca1068acebe61d399669bf5c8e0` with the required eight-frame,
320x180 DX12 smoke. Representative counts are:

- RHI error lines: 262;
- uninitialized descriptor ranges: 19;
- `DATA_STATIC`: 80;
- before-state mismatches: 69;
- `GPUDrivenOpaquePipeline` backend failures: 8;
- `ClearRenderTargetView` optimized-clear mismatches: 8;
- graphics PSO creation failures: 8;
- missing `BLENDINDICES0` and `BLENDWEIGHT0`: 8 each.

Representative state failures include:

- `SceneDepthBuffer` previous state does not match `Common`;
- GPU-culling Visibility/VisibleInstance/IndirectDraw/DrawCount buffers are
  imported as `Common` after prior UAV/SRV/Indirect use;
- pooled post-process textures are treated as fresh even when reused in
  ShaderResource state.

These counts are diagnostic baselines, not permanent expected values. The exit
value for native errors is zero.

## Root-Cause Mapping

### Pipeline input mismatch

`DefaultLit.hlsl` passes one `VSInput` containing position, normal, UV, tangent,
bone indices, and bone weights to both `VSMain` and `VSMainGPUDriven`.
`BuildGPUDrivenDefaultLitPipelineDesc()` then removes input slots 4 and 5.
The shader entry-point signature still requires those semantics.

### Descriptor staticness and completeness

`DX12PipelineLayout::CreateRootSignature()` currently marks all SRV and UAV
ranges `D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC`. That is invalid for
GPU-culling resources whose data/state changes in the command list.

`DX12DescriptorSet` allocates a native table and writes only supplied entries.
The shared validation currently proves supplied entries are individually
valid, but does not prove that every required table slot/array element is
initialized before binding.

### State handoff loss

`SceneRenderer::PrepareGPUDrivenGraphCullInputs()` imports four persistent
output buffers as `Common` on every frame and exports them in UAV/SRV/Indirect
states.

`TransientResourcePool` stores completion and descriptor identity but not the
last realized state. Reacquired resources are reset to `Undefined` by the
graph compiler even though their native resource retains its previous state.

### Clear-value mismatch

DX12 render-target resource creation hard-codes optimized color clear
`{0,0,0,1}` while `OpaquePass` clears `{0.1,0.1,0.15,1}`. The RHI texture
description does not currently carry an optimized clear value.

## Approved Design

### Separate rigid GPU-driven shader input

Add a `GPUDrivenVSInput` containing only the four required mesh attributes and
use it in `VSMainGPUDriven`. Apply the same principle to depth-only shaders.
Direct/skinned entry points keep the bone inputs.

This immediate fix is backed by shared shader-interface preflight validation;
the engine must not depend on compiler removal of unused struct fields.

### Root-signature flags separate descriptor and data lifetime

DX12 mapping follows two independent facts:

- a completely initialized native table that is never updated in place may
  keep stable descriptors for the submitted snapshot;
- referenced CBV/SRV/UAV data uses correctness-safe Root Signature 1.1 data
  flags according to actual graph use;
- `DATA_STATIC` is used only for explicitly immutable data that cannot be
  transitioned or written during the declared lifetime;
- `DESCRIPTORS_VOLATILE` is used only if the native descriptor range itself
  follows a proven legal update model, not merely because resource data
  changes;
- samplers do not receive invalid data flags.

The public RHI does not expose these DX12 flags. Correctness is the default;
static-data optimization is a later opt-in.

### Descriptor set readiness is fail-closed

Before a GPU handle is bound:

- native allocation exists;
- every required SRV/UAV/sampler table slot has been initialized;
- every array element is populated or legally partially bound;
- layout identity matches the pipeline slot;
- the snapshot is not being mutated in place while in flight.

Failure records set name, set index, binding, array element, and reason, then
skips submission of the invalid draw/dispatch.

### Explicit access leases and scoped barriers

DX12 never guesses a starting state for RenderGraph-owned work:

- GPU-culling owners retain their export snapshots;
- pool leases retain final snapshots;
- graph dependencies use the actual realized snapshot plus access intent;
- transition, UAV/memory, and aliasing dependencies remain distinct;
- equal resource states may still emit a UAV/memory dependency;
- discard intent never replaces the actual DX12 before state;
- physical ownership uses the frozen M1 `GPUQueueDomain`;
- the DX12 debug tracker compares, but does not replace, the graph decision.

### Optional optimized clear

If `RHITextureDesc` has no optimized clear, DX12 passes `nullptr` at resource
creation. If present, DX12 validates format/kind and uses the exact value.
Scene color and reverse-Z depth owners provide a value only when stable.

## Implementation Steps

### DX1: Add red contract regressions

- [x] Pipeline validation rejects a GPU-driven shader requiring bone semantics
  with the four-element layout.
- [x] Descriptor validation rejects one missing binding and one missing array
  element.
- [x] Binding an incomplete DX12 descriptor set records a deterministic
  failure.
- [x] A mutable UAV/SRV binding does not map to `DATA_STATIC`.
- [x] A two-frame GPU-culling import starts from the prior export state.
- [x] A pooled post-process texture reacquires its prior state.
- [x] Same-state UAV write/read and write/write cases emit a memory dependency.
- [x] A discarded pooled texture retains its real before state while marking
  previous contents invalid.
- [x] An absent optimized clear creates a valid render target without a
  mismatch contract.

### DX2: Fix and validate shader/pipeline interfaces

- [x] Add rigid GPU-driven input structs to DefaultLit and DepthOnly shaders.
- [x] Preserve skinned direct-draw entry points and layouts.
- [x] Enrich DXIL reflection with semantic index/system-value data.
- [x] Normalize a compact shader interface into the DX12 shader object; keep
  full compiler reflection out of the runtime RHI object.
- [x] Run common pipeline preflight before
  `CreateGraphicsPipelineState`.
- [x] Include normalized interface identity in cache hashing.
- [x] Add structured DX12 native failure details as a final fallback.

Focused exit:

- GPU-driven opaque/masked/depth PSOs create exactly once per cache key;
- invalid test PSOs never reach native creation.

### DX3: Close descriptor table initialization

- [x] Map each layout binding/array element to a deterministic table slot.
- [x] Track initialized bits for CBV/SRV/UAV and sampler ranges.
- [x] Validate completeness after construction/update.
- [x] Make `IsValid()` include allocation and completeness.
- [x] Refuse all graphics/compute/ray-tracing bindings of invalid sets.
- [x] Ensure frame, object, material, GPU-culling, and ray-tracing sets provide
  fallbacks for every required binding.
- [x] Keep replacement snapshots in the existing retirement flow.

Focused exit:

- no `Set*RootDescriptorTable` call receives an uninitialized required range;
- descriptor tests cover sparse binding numbers and arrays.

### DX4: Correct root-signature descriptor/data flags

- [x] Translate complete snapshot lifetime and resource-data volatility
  independently to root descriptors/ranges.
- [x] Default mutable SRV/UAV data to legal data-volatility semantics without
  unnecessarily making stable descriptor tables volatile.
- [x] Keep constant-buffer root descriptors valid for their update pattern.
- [x] Add structural and native tests for the realized root signature.
- [x] Do not globally disable Root Signature 1.1 or native validation.

Focused exit:

- GPU-culling UAV-to-indirect and depth SRV-to-write transitions do not produce
  `DATA_STATIC` errors.

### DX5: Close scoped dependencies and persistent/pooled access handoff

- [x] Translate shared dependencies to transition, UAV/memory, and aliasing
  barriers.
- [x] Do not skip a dependency solely because before/after states match.
- [x] Add access/content snapshots to GPU-culling owned buffers.
- [x] Feed snapshot exports back after graph execution.
- [x] Add state-bearing transient texture/buffer leases.
- [x] Persist subresource/range state where a resource uses partial tracking.
- [x] Preserve completion-safety before pool reuse.
- [x] Verify swapchain buffers remain Present-owned and depth state remains
  SceneRenderer-owned.
- [x] Make upload/presentation and other non-graph paths commit their final
  snapshots at the ownership boundary.
- [x] Remove compatibility `Common` imports from production GPU-culling paths.

Focused exit:

- first, second, resize, and pool-reuse frames have no DX12 before-state
  mismatch;
- same-state UAV hazards are ordered;
- graph diagnostics report supplied and realized access, content-validity, and
  physical-domain state.

### DX6: Close RenderPass clear contract

- [x] Add optional optimized clear translation in normal and placed textures.
- [x] Include it in texture descriptor equality/hash.
- [x] Set or omit Scene color/PostProcess clear hints deliberately.
- [x] Use the configured reverse-Z depth clear value.
- [x] Validate pass attachment format/sample count against PSO preflight.

Focused exit:

- no clear-value mismatch warnings in the bounded sample;
- changing a clear hint changes the pooled resource identity.

### DX7: Make GPU-driven fallback observable

The direct-draw fallback already exists in `OpaquePass` and `DepthPrepass`.
Preserve it and make the selection honest:

- [x] Record `Requested`, `CullingReady`, `PipelineReady`, `Eligible`,
  `Submitted`, and fallback reason separately.
- [x] Do not mark eligible before all batches, material sets, buffers, and PSOs
  are ready.
- [x] If indirect preparation fails, keep the unfiltered direct draw list.
- [x] Add a forced-PSO-failure test that produces direct draws and zero indirect
  draws.
- [x] Add a successful indirect test that produces no duplicate direct draws.

### DX8: Native and integration gates

Run:

```powershell
cmake --build --preset win_x64_debug `
  --target RHIContractValidation `
           PipelineCacheValidation `
           RenderGraphValidation `
           GPUDrivenValidation `
           RenderPassValidation `
           DX12Validation `
           RHIConformanceValidation `
           ModelViewer -- /m:1

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(RHIContractValidation|PipelineCacheValidationFixture|RenderGraphValidation|GPUDrivenValidationFixture|RenderPassValidation|RenderPassValidationFixture|RenderPassStatusValidation|RenderPostProcessStackValidation|SceneRendererDiagnosticsValidation|SceneRendererExternalTargetValidation|DX12Validation|RHIConformanceValidation)\." `
  --output-on-failure

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(ModelViewerGPUDrivenSmoke|ModelViewerGPUDrivenGBVSmoke|GPUDrivenVisualGoldenValidation|ModelViewerGPUDrivenDisabledSmoke)$" `
  --output-on-failure
```

Then run the architecture baseline and Fresh Build Truth. Run bounded
GPU-Based Validation on the focused descriptor/indirect/dependency fixtures in
a dedicated or nightly gate; do not make its cost part of every local smoke.

## Primary File Scope

- `Render/Shaders/DefaultLit.hlsl`
- `Render/Shaders/DepthOnly.hlsl`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Render/Include/Render/GPUDriven/GPUCulling.h`
- `Render/Private/GPUDriven/GPUCulling.cpp`
- shared RHI/ShaderCompiler/RenderGraph files from the conformance plan
- `RHI_DX12/Private/DX12Pipeline.*`
- `RHI_DX12/Private/DX12CommandContext.*`
- `RHI_DX12/Private/DX12Resources.*`
- `RHI_DX12/Private/DX12DescriptorHeap.*` only if allocation tracking requires
  it
- focused existing tests and `Tests/CMakeLists.txt`

## Non-Goals

- suppressing DX12 messages;
- making all descriptors volatile without shared semantics and tests;
- adding bindless rendering;
- changing the M1 Render Thread ownership model;
- declaring the full renderer production-ready from the R7 triangle;
- performance optimization before the zero-error gate.

## Exit Gate

DX12 closure is complete when:

- shared required conformance cases pass on the actual DX12 adapter;
- DX12 Debug Layer error count is zero;
- unexpected DX12 Debug Layer warning count is zero;
- GPU-driven PSOs create successfully;
- GPU-driven smoke submits indirect draws and passes its golden;
- forced unavailability deterministically uses direct draws;
- repeated frames, resize, and transient reuse have no resource-state errors;
- no native validation suppression is required.

## Completion Evidence

- Clean configure and full build completed with DX11, DX12, Vulkan, and OpenGL
  enabled; the full unit/lint inventory passed 1386/1386 tests, with three
  platform-dependent tests skipped.
- The architecture baseline passed 171/171 selected tests, with one
  platform-dependent symlink test skipped.
- `ModelViewerGPUDrivenSmoke`, the zero-tolerance visual golden comparison,
  `ModelViewerGPUDrivenDisabledSmoke`, and the dedicated one-frame
  `ModelViewerGPUDrivenGBVSmoke` all passed on the native DX12 adapter.
- The combined integration log contains zero RHI/DX12 errors or warnings,
  zero `DATA_STATIC` diagnostics, zero state mismatches, zero optimized-clear
  mismatches, and zero uninitialized-descriptor diagnostics.
- The enabled smoke recorded one indirect batch; the disabled smoke recorded
  two direct draws with fallback reason `Disabled`.
