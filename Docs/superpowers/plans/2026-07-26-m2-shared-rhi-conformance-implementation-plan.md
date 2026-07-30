# M2 Shared RHI Conformance and Contract Implementation Plan

**Status:** In progress; M1/Task 20 entry gate satisfied; CH1-CH2 complete;
CH3 next
**Parent:** `2026-07-26-m2-tier1-rhi-proof-master-plan.md`
**Scope:** RHI/ShaderCompiler/RenderGraph shared semantics and conformance
infrastructure

## Goal

Define the minimum production base contract once and make the same shared case
inventory run through thin DX12, Vulkan, and Metal runners. Mock devices
validate deterministic logic; real backends decide support.

## Best-Practice Correction

The initial proposal to extend only resource state snapshots is rejected as
insufficient. The current engine already loses information when RenderGraph
usage becomes an `RHIResourceState` pair:

- execution scope and memory access are conflated;
- equal before/after states are treated as no-work by several backends;
- Vulkan stage/access masks are inferred from one coarse state;
- content discard and unknown/realized native state are conflated;
- logical queue labels do not by themselves describe the M1 physical
  submission domain.

The shared production path therefore carries scoped access dependencies and a
compact lifetime handoff snapshot. `RHIResourceState` remains a compatibility
projection during migration, not the final authority.

## Contract Inventory

### Required base cases

1. Device and capability identity
2. Buffer/texture creation and legal views
3. Upload, copy, readback, and mapped-memory behavior
4. Complete descriptor layouts and descriptor snapshots
5. Graphics and compute pipeline creation
6. Vertex input and attachment compatibility
7. Render-target/depth/UAV/SRV/copy/indirect transitions
8. Graphics/compute/copy queue topology and fence progress
9. Timestamp/query behavior when reported supported
10. Surface acquire, render, present, resize, and idle
11. Deterministic destruction after GPU completion

Ray tracing, mesh shaders, sparse resources, bindless indexing, and multi-native
queue scheduling remain optional capabilities and do not gate the base tier.

## Report Contract

Add a versioned report, for example:

```cpp
enum class RHIConformanceOutcome : uint8
{
    Passed = 0,
    Failed,
    Unsupported,
    EnvironmentUnavailable,
};
```

The report must contain:

- schema ID/version;
- exact source commit supplied by the runner;
- requested and realized backend;
- OS/platform;
- adapter/device/driver identity;
- software-adapter/ICD status;
- normalized capabilities;
- each case ID, required/optional tier, outcome, duration, and reason code;
- native validation warning/error counts and bounded messages;
- surface and queue topology actually used;
- process exit outcome.

Rules:

- requested DX12 with realized DX11 is failure;
- missing Vulkan loader/ICD is `EnvironmentUnavailable`, not `Passed`;
- a reported unsupported required-base feature is failure for a Tier 1 support
  claim;
- optional cases may be `Unsupported` only when capability reporting agrees;
- output order is deterministic.

## Shared API Changes

### Compact shader interface

Extend reflection input attributes with:

- canonical explicit location;
- format;
- system-value flag.
- D3D semantic name/index when present, for DXIL validation and diagnostics.

Carry an owned, immutable `RHIShaderInterface` in the created `RHIShader`, not
the complete compiler reflection graph or pointers into temporary results.
Keep the minimum data needed for:

- vertex inputs;
- resource binding signature;
- push/root constant ranges when present;
- a stable interface hash.

Explicit location is the portable identity. D3D semantic text supplements
rather than replaces it. Shipping builds may strip diagnostic strings while
retaining the stable signature.

Add common graphics-pipeline validation for:

- required shader stages;
- reflected vertex inputs versus `RHIInputLayoutDesc`;
- duplicate semantics/locations;
- format compatibility;
- system values excluded from vertex-buffer requirements;
- render-target/depth formats and counts;
- sample count;
- pipeline-layout presence and shader-resource compatibility where reflection
  is available.

The validation result must contain stable reason codes plus a readable message.
Pipeline cache identity includes the shader-interface hash and complete
layout/attachment compatibility; it never depends on pointer or debug-name
identity.

### Descriptor completeness and data volatility

Keep two independent contracts:

1. A descriptor snapshot is completely materialized and immutable for its
   in-flight lifetime.
2. Resource data referenced by that snapshot may be read, written, or
   transitioned according to graph use.

Do not expose DX12 descriptor-range flags or a speculative update-frequency
taxonomy in public RHI. The base tier uses complete snapshots and typed
fallback resources. Optional null/partially-bound arrays require an explicit
capability.

Validation must:

- reject missing required binding elements;
- reject duplicate elements and type mismatches;
- reject a partially-bound declaration when the device lacks support;
- reject an incomplete set at `SetDescriptorSet`;
- keep dynamic offsets separate from descriptor/data stability.
- prove replacement never mutates the in-flight native snapshot.

On DX12, the backend maps snapshot lifetime and resource-data volatility
separately. A table initialized completely and never updated in place may have
stable descriptors even when referenced data is volatile. Static-data
optimizations remain opt-in after proof.

### Scoped dependency contract

Introduce compact backend-neutral concepts, with final names selected during
implementation review:

- execution/synchronization scope;
- memory access scope, preserving read versus write;
- resource usage/layout;
- source and destination M1 physical `GPUQueueDomain`;
- texture subresource or buffer range;
- content validity/discard intent;
- transition, memory/UAV, or aliasing dependency kind where needed.

RenderGraph already records access type, desired state, shader stage, and
ranges. Compilation must preserve that information into the RHI dependency
batch. A same-layout/state dependency remains legal and required for hazards
such as UAV-write to UAV-write or compute-write to graphics-read.

Backend translation is:

- DX12 transition plus UAV/memory and aliasing barriers; enhanced barriers are
  a later implementation option, not a base requirement;
- Vulkan synchronization2 stage/access/layout and queue-family ownership;
- Metal tracked-hazard baseline plus usage, encoder boundaries, and explicit
  synchronization where needed;
- conservative `RHIResourceState` projection for DX11/OpenGL.

### Lifetime access snapshot

Introduce an owned handoff snapshot that can represent:

- uniform access/layout plus texture subresource or buffer-range overrides;
- last realized M1 physical submission domain;
- whether prior contents are valid.

RenderGraph and owners then:

- import using a snapshot;
- compile scoped dependencies from the snapshot and pass declarations;
- publish a realized export snapshot;
- expose diagnostics for planned versus supplied starting access;
- allow discard only when the first use does not require old contents.

Compatibility overloads accepting one `RHIResourceState` may remain
temporarily, but new persistent and pooled owners must migrate to snapshots.
They must not silently default to `Common`.

RenderGraph is authoritative only for one graph execution. Persistent owners,
transient leases, uploads, presentation, and other non-graph producers commit
their final snapshots explicitly. No global pointer-keyed database is added.

### Optimized clear value

Add an optional clear value to `RHITextureDesc`:

- color or depth/stencil kind;
- format compatibility;
- exact value included in descriptor hashing;
- absent by default.

RenderPass load/clear values remain execution commands. The optimized value is
only a resource-creation hint and must either match intended common clears or
be omitted.

## Conformance Case Library and Thin Runner

Add a shared case catalog/report library and, when it keeps the build clearer,
a thin target tentatively named `RHIConformanceValidation`.

It accepts:

```text
--backend dx12|vulkan|metal|dx11|opengl
--report <path>
--validation
--surface
--frames <bounded count>
```

The shared library/runner has:

- backend-neutral case definitions;
- a small platform surface adapter;
- native validation-message collection through a normalized sink;
- no backend fallback;
- deterministic fixed-size fixtures;
- bounded waits and explicit timeout/failure reasons.

Do not create a second monolithic backend-test hierarchy. Existing `DX12Validation`,
`VulkanValidation`, `CrossBackendValidation`, and native lifecycle tests remain
focused thin fixtures and may reuse the shared catalog/report helpers.

Validation is layered:

1. common contract and compiler tests;
2. backend translation/structural tests;
3. native real-device validation;
4. bounded renderer integration.

## Implementation Steps

### CH1: Freeze case IDs and report schema

- [x] Add report structs and JSON serialization.
- [x] Add deterministic ordering and schema tests.
- [x] Add outcome classification tests.
- [x] Add capability-consistency tests.
- [x] Add requested-versus-realized backend rejection.

CH1 implementation record (2026-07-30):

- added the versioned `RVX.RHI.ConformanceReport` schema and frozen eleven-case
  `RVX.RHI.BaseConformance` catalog in `RHIConformance`;
- records source commit, platform, requested/realized backend, hardware versus
  software adapter classification, validation enablement, surface realization,
  process exit code, public capability report, case results, and bounded native
  messages;
- canonicalizes capabilities, cases, and messages before deterministic JSON
  export;
- fails closed on backend fallback, incomplete execution identity, non-zero
  available-run exit, validation-request mismatch, inconsistent capability
  reports, dishonest surface success, and environment/result contradictions;
- keeps Tier 1 required-case rejection distinct from honest compatibility
  `Unsupported` reporting;
- `RHIConformanceValidation`: 11/11 passed;
- combined `RHIContractValidation`, `RHIConformanceValidation`, and
  `CrossBackendValidation`: 56/56 passed;
- architecture regressions `CMakeModuleLinks`, `PublicHeaderLinkage`, and
  `M1ArchitectureCut`: 3/3 passed.

### CH2: Add validation message sink

- [x] Define normalized severity, category, native ID, and bounded text.
- [x] Collect without suppressing native messages.
- [x] Count warnings and errors separately.
- [x] Add an allowlist mechanism only for reviewed non-errors; every allowlist
  entry requires backend, native ID, justification, and expiry/review owner.
- [x] Do not allow message-text-only wildcard suppression.
- [x] Fail required gates on unexpected warnings as well as errors.

CH2 implementation record (2026-07-30):

- added a thread-safe `RHIConformanceValidationMessageSink` with fixed-size,
  deterministically ordered evidence and separate total/dropped counts;
- preserves allowlisted warnings in the report instead of suppressing them;
- caps message text at 2048 bytes and records truncation explicitly;
- applies reviewed warning exceptions only by concrete backend plus exact native
  ID; message text is never a match key and errors are never exempted;
- validates stable allowlist ID, justification, review owner, ISO expiry, and
  evaluation date; expired entries stop matching;
- publishes the evaluation date and matched allowlist entry ID for audit;
- `RHIConformanceValidation`: 15/15 passed, including concurrent producers,
  deterministic bounding, strict backend/ID matching, expiry, malformed
  metadata, error rejection, and text bounds;
- combined `RHIContractValidation`, `RHIConformanceValidation`, and
  `CrossBackendValidation`: 60/60 passed;
- architecture regressions `CMakeModuleLinks`, `PublicHeaderLinkage`, and
  `M1ArchitectureCut`: 3/3 passed.

### CH3: Add shader/pipeline preflight contract

- [ ] Enrich DXIL/DXBC/SPIR-V reflection.
- [ ] Normalize a compact immutable shader interface with explicit locations
  and a stable hash into RHI shader objects.
- [ ] Add common validation result codes.
- [ ] Unit-test missing semantic, wrong semantic index, wrong format, duplicate
  input, system values, attachment mismatch, and valid no-input shaders.
- [ ] Add DX12, Vulkan, and Metal translation-structure tests before freezing
  the shared interface.

### CH4: Add descriptor completeness contract

- [ ] Add layout semantics and device capability checks.
- [ ] Add complete-array validation.
- [ ] Add bind-time readiness validation.
- [ ] Add replacement/in-flight lifetime regressions.
- [ ] Keep descriptor-set creation atomic: failure publishes no usable set.
- [ ] Keep resource-data volatility separate from descriptor completeness.
- [ ] Add DX12, Vulkan, and Metal translation-structure tests before freezing
  the shared contract.

### CH5: Add scoped dependency and pool lease contract

- [ ] Add execution scope, memory access, layout/usage, dependency-kind,
  physical-domain, range, and content-validity value types.
- [ ] Add lifetime snapshot equality/diagnostic formatting.
- [ ] Extend barrier batches so equal layouts can still carry memory
  dependencies.
- [ ] Add RenderGraph import/export APIs.
- [ ] Change transient acquire/release to carry state.
- [ ] Make non-graph producers commit their final snapshots.
- [ ] Add two-frame texture, buffer, subresource, range, same-state hazard,
  discard, and physical-domain tests.
- [ ] Detect stale compatibility overload use in production Render paths.
- [ ] Add DX12, Vulkan, and Metal translation-structure tests before freezing
  the shared contract.

### CH6: Build shared real-device cases

- [ ] Resource/view case
- [ ] Descriptor snapshot case
- [ ] Graphics pipeline/input case
- [ ] Compute UAV/SRV case
- [ ] Copy/readback case
- [ ] Indirect argument case
- [ ] Queue/fence case
- [ ] Query case
- [ ] Surface/present/resize/idle case
- [ ] Deferred destruction case

## Primary File Scope

- `RHI/Include/RHI/RHIShader.h`
- `RHI/Include/RHI/RHIPipeline.h`
- `RHI/Include/RHI/RHIDescriptor.h`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHIQueueTopology.h`
- `RHI/Include/RHI/RHITexture.h`
- `RHI/Include/RHI/RHIResources.h` or a focused access snapshot header
- `ShaderCompiler/Include/ShaderCompiler/ShaderReflection.h`
- `ShaderCompiler/Private/ShaderReflection.cpp`
- `ShaderCompiler/Private/ShaderManager.cpp`
- `Render/Include/Render/Graph/RenderGraph.h`
- `Render/Private/Graph/RenderGraph*.cpp`
- `Render/Include/Render/Graph/TransientResourcePool.h`
- `Render/Private/Graph/TransientResourcePool.cpp`
- `Tests/RHIContractValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderGraphValidation/main.cpp`
- new `Tests/RHIConformanceValidation/` only if shared helpers cannot live in
  the existing test structure cleanly
- `Tests/CMakeLists.txt`

## Focused Validation

```powershell
cmake --build --preset win_x64_debug `
  --target RHIContractValidation `
           PipelineCacheValidation `
           RenderGraphValidation `
           RHIConformanceValidation -- /m:1

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(RHIContractValidation|PipelineCacheValidation|RenderGraphValidation|RHIConformanceValidation)\." `
  --output-on-failure
```

Backend real-device runs are owned by their closure plans.

## Exit Gate

- shared APIs express all required intent without backend-native types;
- mocks cover invalid and valid contract behavior;
- one shared case catalog produces the same report schema through every thin
  backend runner;
- no compatibility default silently uses `Common`, partially bound
  descriptors, or immutable data;
- same-state hazards, discard intent, and physical-domain ownership are
  representable without native API types;
- backend closure can proceed without changing report semantics.
