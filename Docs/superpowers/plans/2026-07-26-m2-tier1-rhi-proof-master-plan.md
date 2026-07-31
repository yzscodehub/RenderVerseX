# M2 Tier 1 RHI Proof Master Implementation Plan

**Status:** In progress; best-practice review incorporated; Task 20 complete;
Task 21 CH1-CH2 complete; Task 22/CH3 complete; Task 23/CH4 next
**Branch:** `codex/architecture-implementation`
**M1 entry commit:** `6c93300618026ca1068acebe61d399669bf5c8e0`
**M1 entry CI:** `https://github.com/yzscodehub/RenderVerseX/actions/runs/30521739421`
**Program milestone:** M2 - Tier 1 RHI Proof
**Primary backends:** DX12, Vulkan, Metal
**Compatibility backends:** DX11, OpenGL

## Goal

Prove one normalized, production-oriented base RHI contract on real DX12,
Vulkan, and Metal devices before expanding renderer features.

The first integration symptom is the current DX12 `ModelViewer` run:

- Engine startup, main-loop entry, and orderly shutdown succeed;
- `GPUDrivenOpaquePipeline` creation fails because the shader input signature
  requires `BLENDINDICES0` and `BLENDWEIGHT0` while the GPU-driven input layout
  deliberately removes slots 4 and 5;
- descriptor tables declared `DATA_STATIC` are bound with uninitialized
  entries and later reference resources whose data/state changes;
- persistent GPU-culling resources are imported as `Common` on every frame
  even though the previous graph exports them as UAV/SRV/Indirect;
- pooled transient resources lose their last realized state when returned to
  `TransientResourcePool`;
- render-target optimized clear values do not match the actual pass clear.

M2 must convert these failures into shared contracts and real-device evidence.
It must not become a set of DX12-only warning suppressions.

## Planning Decomposition

The approved production-foundation design requires M2 to be decomposed into a
shared conformance plan and backend closure plans:

1. [Shared RHI conformance and contract plan](2026-07-26-m2-shared-rhi-conformance-implementation-plan.md)
2. [DX12 correctness closure plan](2026-07-26-m2-dx12-correctness-closure-implementation-plan.md)
3. [Vulkan correctness closure plan](2026-07-26-m2-vulkan-correctness-closure-implementation-plan.md)
4. [Metal correctness closure plan](2026-07-26-m2-metal-correctness-closure-implementation-plan.md)
5. [Best-practice design review](2026-07-26-m2-rhi-best-practice-design-review.md)

This master plan controls sequencing, exit evidence, and cross-plan scope. The
backend plans own native translation details.

## Prerequisite Gate

M2 implementation begins only after an M1 exit review confirms:

- the exact M1 candidate commit is pushed;
- Windows and available Linux/macOS M1 gates pass on that commit;
- the frozen Render Thread ownership, identity, queue, upload, and retirement
  contracts remain unchanged;
- any environment skip is explicit and approved rather than reported as a
  backend pass.

Planning may complete before that review. Code implementation must not bypass
it.

The prerequisite gate passed on 2026-07-30. Windows, Linux, and macOS ARM64
jobs completed successfully on the exact pushed entry commit. The reviewed
entry record and current expected-red DX12 integration evidence are stored in
`../evidence/2026-07-26-m2-task20-baseline.md`.

## Architectural Decisions

### 1. Shared semantics, native translation

RHI owns backend-neutral meaning. A backend translates that meaning to native
API objects and validation rules; it does not infer missing intent.

Examples:

- a reflected vertex input contract is expressed once, then translated to a
  DX12 input layout, Vulkan vertex attributes, or a Metal vertex descriptor;
- descriptor completeness and stability are RHI semantics, then translated to
  root-signature flags, descriptor-set policy, or argument/resource binding;
- RenderGraph state handoff is explicit, then translated to DX12 states,
  Vulkan layouts/access/stages, or Metal encoder usage/hazard behavior.

### 2. State follows resource lifetime

No new global pointer-keyed state database will become the source of truth.
Resource dependency state is handed off at ownership/lifetime boundaries:

- external persistent owners provide an import access snapshot;
- RenderGraph produces the realized export access snapshot;
- `TransientResourcePool` stores the snapshot when a lease is released and
  returns it with the next lease;
- non-graph producers such as upload, presentation, or acceleration-structure
  work explicitly commit their final snapshot;
- backend trackers validate execution but do not independently invent the
  starting state.

The snapshot separates execution scope, memory access, layout/usage, physical
`GPUQueueDomain`, and content validity. `Undefined` is not used as a combined
alias for unknown native state and discarded contents.

### 3. Dependencies are richer than resource states

`RHIResourceState` remains a compatibility projection during migration. The
production RenderGraph path compiles scoped dependencies containing source and
destination execution scopes, memory accesses, layouts/usages, ranges,
physical-domain ownership, and discard intent.

Equal before/after layouts may still require a memory dependency. Backend
translations preserve this distinction:

- DX12 transition, UAV/memory, and aliasing barriers;
- Vulkan synchronization2 stage/access/layout and queue-family ownership;
- Metal tracked hazards, encoder boundaries, resource usage, fences/events
  where needed;
- conservative compatibility projection for DX11/OpenGL.

### 4. Descriptor sets are complete snapshots

A descriptor set submitted to a command list is an immutable, complete
snapshot for the duration of its GPU use.

- required bindings must be populated;
- optional bindings use explicit fallback/null resources according to declared
  capability;
- arrays are either fully initialized or explicitly declared partially bound
  on a backend that proves the required feature;
- changes create a replacement snapshot and retire the previous snapshot by
  GPU completion.

Descriptor snapshot stability and referenced resource-data volatility are
separate semantics. A stable descriptor may legally reference resource data
that changes or transitions.

### 5. Correctness-safe defaults

Static/immutable optimizations are opt-in and validated. The base contract
defaults to semantics that permit legal resource transitions and writes.
Optimized clear values are optional; when present, they must exactly match the
resource and pass contract.

### 6. Fail closed and explain why

Pipeline, descriptor, dependency, or capability failures produce structured
diagnostics at common preflight and native boundaries. M2 does not require a
high-risk rewrite of every RHI factory return type. Unsupported,
environment-unavailable, and failed remain distinct outcomes.

### 7. Real devices are authoritative

Mock tests prove deterministic contract logic. They cannot declare a Tier 1
backend supported. DX12 Debug Layer, Vulkan validation layers, and Metal
real-device execution provide the backend evidence.

## Ordered Work Breakdown

### Task 20: M1 exit review and M2 red evidence lock

- [x] Push the exact M1 candidate only after explicit authorization.
- [x] Complete M1 Windows/Linux/macOS evidence or record approved environment
  skips.
- [x] Record a bounded DX12 `ModelViewerGPUDrivenSmoke` failure artifact.
- [x] Record validation categories and counts, without checking in volatile
  native handles or machine paths.
- [x] Confirm the existing disabled-GPU-driven path remains a usable fallback
  control.
- [x] Freeze the M2 required/optional capability matrix.

Exit:

- one reviewed M1 exit record;
- one reproducible red DX12 integration case;
- no source implementation change beyond evidence/gate plumbing.

### Task 21: Shared case catalog, thin runner, and report schema

- [x] Freeze the backend-neutral case IDs, report schema, deterministic JSON,
  capability-honesty rules, and requested-versus-realized rejection (CH1).
- [x] Add the normalized native validation-message sink and reviewed allowlist
  policy (CH2).
- [ ] Add one backend-neutral case/report library, thin executable runner, and
  versioned JSON report.
- [ ] Cover resource, view, descriptor, pipeline, barrier, queue, fence, query,
  and presentation cases.
- [ ] Distinguish `Passed`, `Failed`, `Unsupported`, and
  `EnvironmentUnavailable`.
- [ ] Capture adapter/device/driver identity, requested backend, realized
  backend, capabilities, validation messages, and test-case outcomes.
- [ ] Reject fallback to a different backend.
- [ ] Add deterministic mock tests for report and capability honesty.

Exit:

- the same case inventory can run through thin runners against every enabled
  backend without duplicating existing native fixtures;
- unavailable hardware does not produce a false pass;
- JSON reports are stable enough for CI comparison.

### Task 22: Compact shader interface and graphics pipeline contract

- [x] Extend reflection with explicit location, format, D3D semantic
  name/index where applicable, and system-value identity.
- [x] Carry only a compact immutable backend-neutral shader interface and
  stable hash into `RHIShader`; do not retain the full compiler reflection
  graph.
- [x] Validate graphics pipeline descriptors before native PSO creation:
  shader stages, vertex inputs, formats, sample count, attachment count,
  depth/RT formats, and pipeline layout.
- [x] Produce a structured mismatch report naming pipeline, shader entry point,
  missing/extra semantic, expected/actual format, and attachment mismatch.
- [x] Split GPU-driven rigid vertex input from the skinned direct-draw input;
  do not depend on compiler dead-input elimination.
- [x] Include the shader-interface identity in pipeline cache keys.
- [x] Complete a DX12/Vulkan/Metal translation design checkpoint before the
  shared contract is frozen.

Exit:

- the current `BLENDINDICES0`/`BLENDWEIGHT0` mismatch is caught in a unit test;
- `GPUDrivenOpaquePipeline` and `GPUDrivenDepthOnlyPipeline` create on DX12;
- intentionally invalid pipeline cases fail before native creation.

Task 22 completed on 2026-07-30. The shared preflight and translation contract
is covered by 10 focused tests, real DX12 GPU-driven enabled/disabled smoke
tests, and the 181-test shader/pipeline/RHI/architecture regression selection.
Metal evidence at this checkpoint is structural; Task 27 remains responsible
for macOS compilation, validation messages, and real-device closure.

### Task 23: Descriptor completeness and data-volatility contract

- [ ] Model complete immutable descriptor snapshots separately from
  referenced resource-data volatility.
- [ ] Make complete binding validation account for every array element.
- [ ] Require explicit fallback/null resources for absent optional bindings.
- [ ] Refuse to bind invalid or incomplete descriptor sets.
- [ ] Keep public RHI free of DX12 range flags and update-frequency
  optimization categories.
- [ ] Map stable descriptors and mutable resource data independently to
  correctness-safe DX12 Root Signature 1.1 flags.
- [ ] Preserve replacement-and-retirement behavior for frame, object, and
  material descriptor snapshots.
- [ ] Add validation for updates while a snapshot is in flight.
- [ ] Complete a DX12/Vulkan/Metal translation design checkpoint before the
  shared contract is frozen.

Exit:

- no uninitialized descriptor range is bound;
- resources used as UAV or transitioned in the same command list are not
  declared data-static unless their contract actually permits it;
- descriptor replacement never mutates an in-flight native range.

### Task 24: Scoped dependencies, state handoff, and transient lease closure

- [ ] Introduce scoped dependency/access snapshots with execution scope,
  memory access, layout/usage, range/subresources, content validity, and the
  M1 physical `GPUQueueDomain`.
- [ ] Preserve same-layout/state memory dependencies such as UAV-write to
  UAV-write or shader-write to shader-read.
- [ ] Separate discard intent from the actual realized before state.
- [ ] Import external resources with a supplied snapshot.
- [ ] Publish realized export snapshots after graph execution.
- [ ] Return transient texture/buffer leases to the pool with their final
  snapshot.
- [ ] Reacquire pooled resources with their stored snapshot rather than
  `Undefined`.
- [ ] Persist GPU-culling buffer states across frames.
- [ ] Keep swapchain and Scene depth state ownership explicit.
- [ ] Make uploads and other non-graph producers commit final snapshots.
- [ ] Add debug validation that planned source access/layout/domain equals the
  last realized snapshot.
- [ ] Keep legacy `RHIResourceState` overloads only as an explicit migration
  projection for compatibility backends.
- [ ] Complete a DX12/Vulkan/Metal translation design checkpoint before the
  shared contract is frozen.

Exit:

- second-frame and pool-reuse tests start from the prior final access snapshot;
- GPU-culling buffers do not re-enter as `Common`;
- post-process pooled resources do not produce before-state mismatches;
- same-state hazards and discard/reuse cases have deterministic tests.

### Task 25: DX12 native correctness closure

- [ ] Complete Tasks 22-24 DX12 translations.
- [ ] Translate scoped dependencies to transition, UAV/memory, and aliasing
  barriers without treating equal states as automatic no-work.
- [ ] Add an optional optimized clear contract to texture creation.
- [ ] Make RenderPass clear behavior match, or omit, the optimized clear value.
- [ ] Verify reverse-Z depth clear values.
- [ ] Preserve CPU direct-draw fallback when GPU-driven preparation or PSO
  creation fails.
- [ ] Expose the exact GPU-driven fallback reason in frame diagnostics.
- [ ] Run the shared conformance suite and bounded ModelViewer integration.
- [ ] Run the normal Debug Layer gate on every required DX12 fixture and
  bounded GPU-Based Validation in a dedicated/nightly gate.

Exit:

- DX12 Debug Layer reports zero unexpected warnings or errors in the required
  base suite;
- `ModelViewerGPUDrivenSmoke` reports actual indirect batches/draws;
- `ModelViewerGPUDrivenDisabledSmoke` proves direct-draw fallback;
- the fixed R7 golden passes under the approved tolerance.

### Task 26: Vulkan native correctness closure

- [ ] Translate the frozen shared contracts without changing their meaning.
- [ ] Prove descriptor completeness without relying on optional partially-bound
  features for the base tier.
- [ ] Map scoped dependencies to synchronization2 layout, access, stage, and
  queue-family ownership behavior; preserve same-layout memory dependencies.
- [ ] Run on Windows Vulkan and Linux Vulkan real devices/software ICDs
  according to the support policy.
- [ ] Run the same bounded reference integration and capture validation output.

Exit:

- required Vulkan conformance cases pass with zero unexpected
  validation-layer warnings or errors;
- capability report matches actual enabled device features;
- no DX12-specific assumption leaks into Render or RenderGraph.

### Task 27: Metal native correctness closure

- [ ] Translate the frozen shader interface to Metal vertex descriptors.
- [ ] Prove explicit complete buffer/texture/sampler binding.
- [ ] Use tracked hazard mode as the correctness baseline and map graph intent
  to resource usage, encoder boundaries, and explicit synchronization where
  cross-encoder/queue behavior requires it.
- [ ] Preserve the M1 `CAMetalLayer` main-thread attachment boundary.
- [ ] Run the shared suite and bounded reference integration on macOS hardware.

Exit:

- required Metal conformance cases pass on a real macOS device with zero
  unexpected native warnings or errors;
- pipeline and command-encoding failures are structured and fail closed;
- no compile-only result is reported as production support.

### Task 28: DX11 and OpenGL compatibility regression

- [ ] Keep them outside the Tier 1 support claim.
- [ ] Adapt shared data structures and safe defaults without forcing Tier 1-only
  features.
- [ ] Run existing DX11/OpenGL unit, smoke, and golden coverage.
- [ ] Report unsupported shared cases explicitly.

Exit:

- both compatibility backends compile and pass their documented smoke scope;
- their limitations appear in the support matrix.

### Task 29: M2 exit evidence and Build Truth

- [ ] Run clean configure/build/test from empty build trees.
- [ ] Publish per-platform conformance JSON and validation logs.
- [ ] Publish bounded integration screenshots and visual diffs.
- [ ] Publish exact source commit and capability/support matrix.
- [ ] Run the architecture baseline and M1 regression suites.
- [ ] Update `phase-log.md` only from final evidence.

Exit:

- DX12, Vulkan, and Metal pass the required real-device base contract;
- unexpected validation-layer/API-debug warning and error counts are zero;
- no silent emulation, backend substitution, or compile-only support claim;
- DX11/OpenGL results are reported as compatibility evidence only.

## Dependency Order

```text
Task 20 M1 exit / red evidence
  -> Task 21 shared case catalog / thin runner
  -> Task 22 pipeline contract + three-backend checkpoint
  -> Task 23 descriptor contract + three-backend checkpoint
  -> Task 24 dependency/state contract + three-backend checkpoint
  -> Task 25 DX12 closure
  -> Task 26 Vulkan closure
  -> Task 27 Metal closure
  -> Task 28 Tier 2 regression
  -> Task 29 M2 exit
```

DX12 remains the first full native red-to-green closure, but Tasks 22-24 are
not frozen from DX12 evidence alone. Their Vulkan/Metal structural translation
checks must pass first. Tasks 26 and 27 then provide real-device closure. Any
required shared-contract meaning change returns to Tasks 22-24 and reruns all
already-green backend checks.

## Commit and Review Boundaries

Use one reviewable commit per numbered task. Do not combine:

- shared contract changes with backend-specific warning suppression;
- pipeline fixes with descriptor or state changes;
- real-device evidence with unrelated renderer features;
- generated validation artifacts with source unless the artifact is an
  approved small golden/report fixture.

Every commit must keep unit/architecture gates green. Native red-to-green
evidence is attached to the task that owns the behavior.

## Stop Conditions

Stop and request architecture review if implementation would:

- change frozen M1 Render Thread ownership or queue topology semantics;
- add a Render dependency on Scene, World, Resource, or platform Window code;
- introduce a global raw-pointer resource-state authority;
- treat equal before/after states as proof that no dependency is required;
- conflate discarded contents with unknown native state;
- mutate descriptor memory that may still be in flight;
- hide a native validation message, disable a validation layer, or classify an
  error as an expected skip;
- require a new renderer feature to prove the base RHI contract;
- make Tier 1 behavior depend on DX12-only concepts.

## Non-Goals

- Editor implementation or Editor-specific validation;
- new lighting, post-processing, ray-tracing, meshlet, or bindless features;
- performance tuning before correctness gates are green;
- parallel command recording or multiple native queues per logical queue;
- M3 cooking/residency work;
- M4 canonical-renderer feature completion;
- M5 device-loss and soak closure beyond cases needed to make M2 tests
  deterministic.

## Final Acceptance

M2 is complete only when the shared case inventory passes on real DX12,
Vulkan, and Metal devices with zero unexpected native validation warnings or
errors, the
reports identify the actual backend and adapter, and the current DX12
GPU-driven integration failure has become a permanent regression gate rather
than a one-off local fix.
