# Render Policy, Draw Packet, and GPU-Driven Architecture Implementation Plan

**Status:** Tasks 0-8, Task 9A, Tasks 9B-1 through 9B-5, Task 9B-6A, and
Task 9B-6B1 are complete and reviewed; Task 9B-6B2a shared directional-light
snapshot migration is next.
**Date:** 2026-08-02
**Scope:** Engine-core rendering architecture for DX12, Vulkan, and Metal;
DX11 and OpenGL remain compatibility paths; Editor work is out of scope

**Execution checklist:**
`Docs/superpowers/plans/2026-08-02-render-policy-draw-packet-execution-todo.md`

## 1. Objective

Move RenderVerseX from a DX12-specific GPU-culling and indirect-draw feature
to a production-oriented renderer architecture with the following properties:

1. Samples and games express rendering requests; Render owns all runtime path
   decisions and fallback behavior.
2. Immutable frame extraction remains separated from the retained render
   scene, pass-specific draw preparation, visibility, submission, and RHI.
3. Direct and GPU-driven work can coexist in the same pass without duplicate
   or missing draws.
4. DX12, Vulkan, and Metal use backend-appropriate indirect execution
   strategies behind shared semantic contracts.
5. `Auto` selects only a qualified and beneficial path. Development overrides
   remain available but never bypass mandatory device or pipeline contracts.
6. Every decision and execution result is observable through stable,
   machine-readable diagnostics and reproducible tests.

This plan does not replace the existing qualification and production-asset
plans. It provides the renderer architecture those plans will validate.

Related plans:

- `2026-08-02-gpu-driven-backend-qualification-plan.md`
- `2026-08-02-production-asset-onboarding-plan.md`

## 2. Current Baseline

The branch already provides useful foundations:

- immutable `RenderFramePacket` extraction contracts;
- a retained Render-owned `RenderScene`;
- RenderGraph resource access and barrier tracking;
- `Auto`, `ForceEnabled`, and `ForceDisabled` GPU-driven modes;
- a reviewed backend qualification gate mask;
- DX12 compute culling and material-grouped indirect drawing for Depth and
  Opaque;
- stable GPU-driven diagnostics and direct/GPU image-parity fixtures;
- external real-asset tests with deterministic camera fitting.

The current implementation is explicitly classified as **GPU-driven Tier 1**:

```text
CPU final visibility
  -> CPU material draw lists
  -> CPU mesh/material grouping
  -> GPU fine culling
  -> per-group indirect submission
```

It is not yet a GPU-resident scene. The important current limitations are:

- `SceneRenderer` performs final CPU view culling before GPU culling;
- `RenderDrawItem` remains a thin per-frame item and currently assumes one
  submesh while building material draw lists;
- GPU-driven eligibility is checked partly in `SceneRenderer` and partly in
  Depth/Opaque execution;
- one ineligible group can force an entire pass to Direct;
- Skybox compatibility setters still maintain pass status until Task 9B-6, but
  typed Skybox recording consumes only the frame snapshot and graph-owned data;
- GPU execution is intentionally hard-limited to DX12;
- Vulkan does not yet expose or implement indirect-count submission through
  the RHI contract;
- Metal needs an ICB-oriented strategy rather than a mechanical DX12 port;
- backend qualification is backend-wide instead of path- and contract-specific;
- `Auto` checks correctness readiness but has no workload benefit model.

## 3. Architectural Invariants

The implementation must preserve these rules at every task boundary.

### 3.1 Ownership

| Layer | Owns | Must not own |
|---|---|---|
| Sample/Game | Scene setup, camera, lights, runtime requests, demo assertions | RHI capability probing, draw grouping, indirect resources, fallback decisions |
| RenderContracts | Immutable extraction values and request vocabulary | RHI objects, graph handles, renderer implementation types |
| RenderScene | Retained primitive/light state, generations, change tracking | Backend commands or sample behavior |
| Policy | View/pass capability limits, requested/selected modes, planned fallback | Material-specific shader selection or command recording |
| Mesh pass processing | Pass relevance, shader/PSO key, geometry/material binding key, eligibility | Backend feature probing or graph scheduling |
| Visibility | Candidate generation, CPU/GPU visibility, visible draw streams | PSO/material binding decisions |
| Submission | Direct, indirect-count, or ICB execution strategy | Scene extraction or gameplay policy |
| RenderGraph | Dependencies, lifetimes, barriers, queue scheduling | Object eligibility or quality policy |
| RHI | Semantic capabilities, resources, commands, synchronization | Renderer policy, backend qualification, workload heuristics |

### 3.2 Request, plan, and result are separate

```text
Render request
  -> immutable execution plan
  -> graph compilation and command recording
  -> immutable execution report
```

- A request does not prove the feature was selected.
- A plan does not prove commands were submitted.
- A submitted command does not prove visual correctness.
- Execution code must not silently change policy. Expected fallback is planned
  before graph compilation; an unexpected recording failure is reported as a
  frame/path failure and disables the path on a later frame.

### 3.3 Per-view, per-pass, and per-group granularity

- Raster pipeline selection is view-level.
- Visibility and submission strategy are pass-level limits.
- Concrete eligibility is draw-group-level.
- Transparent, skinned, special-material, and not-yet-resident groups may use
  Direct while static opaque groups use GPU-driven submission in the same
  frame.

### 3.4 Direct remains the reference path

- Direct is never removed during this plan.
- Forced Direct remains the visual and behavioral reference for every backend.
- A GPU-driven path must not invent different material, transform, clipping,
  or pass semantics.

### 3.5 Backend honesty

- Missing capabilities fail closed.
- Vulkan feature availability is queried per physical device.
- Metal ICB support is represented explicitly.
- DX11/OpenGL compatibility behavior is never reported as a qualified modern
  GPU-driven path.

## 4. Target Architecture

```text
Sample / Game
  -> RenderFramePacket
  -> retained RenderScene and PrimitiveProxy updates
  -> view candidate generation
  -> RenderPolicyResolver
  -> MeshPassProcessors
  -> RenderFrameExecutionPlan
       -> DepthPassPlan
       -> OpaquePassPlan
       -> ShadowPassPlan
       -> TransparentPassPlan
  -> Visibility providers
       -> CPU visible packet stream
       -> GPU visible packet/command stream
  -> Submission strategies
       -> Direct
       -> MultiDrawIndirectCount (DX12/Vulkan)
       -> EncodedCommandBuffer (Metal ICB)
  -> RenderGraph
  -> RHI backend
  -> RenderFrameExecutionReport
```

### 4.1 GPU-driven tiers

```cpp
enum class GPUDrivenTier : uint8
{
    Direct = 0,
    IndirectGrouped,
    GPUResidentScene,
    Meshlet
};
```

| Tier | Definition | Plan status |
|---|---|---|
| Direct | CPU visibility and direct submission | Reference path |
| IndirectGrouped | CPU-prepared packets, GPU fine culling, grouped indirect execution | Current path, to be productionized |
| GPUResidentScene | Persistent GPU primitive data, coarse CPU candidates, GPU visibility/compaction | Included in this plan after Tier 1 stabilizes |
| Meshlet | Meshlet/mesh-shader geometry processing | Future extension, contract reserved only |

Meshlet rendering is not implemented by this plan.

### 4.2 Draw preparation model

```text
RenderPrimitiveProxy
  -> MeshBatch                    pass-independent
  -> MeshPassProcessor            pass-specific classification
  -> RenderDrawPacket             immutable stateless draw description
  -> RenderDrawGroup              shared state and submission unit
```

Proposed core contracts:

```cpp
struct MeshBatch
{
    RenderObjectId object;
    RenderResourceHandle mesh;
    RenderResourceHandle material;
    uint32 submeshIndex;
    PrimitiveDataIndex primitiveData;
    RenderBatchFlags flags;
};

struct RenderDrawPacket
{
    RenderPassKind pass;
    PipelineKey pipeline;
    GeometryBindingKey geometry;
    MaterialBindingKey material;
    PrimitiveDataIndex primitiveData;
    RenderDrawArguments arguments;
    RenderDrawFlags flags;
};

struct RenderDrawGroupKey
{
    RenderPassKind pass;
    PipelineKey pipeline;
    GeometryBindingKey geometry;
    MaterialBindingKey material;
    RenderSubmissionLayout layout;
};
```

Static packets are cached using resource generations and pass-contract
versions. Dynamic packets are rebuilt only for data that genuinely changes.
Transforms and other per-instance values live in indexed primitive data rather
than invalidating the entire static packet.

### 4.3 Policy and plan contracts

```cpp
enum class RenderSubmissionMode : uint8
{
    Direct = 0,
    FixedCountIndirect,
    MultiDrawIndirectCount,
    EncodedCommandBuffer
};

enum class RenderVisibilityMode : uint8
{
    Cpu = 0,
    GpuFrustum,
    GpuFrustumAndDistance,
    GpuOcclusion
};

struct RenderPassExecutionPlan
{
    RenderPassKind pass;
    RenderVisibilityMode visibility;
    RenderSubmissionMode preferredSubmission;
    RenderSubmissionMode fallbackSubmission;
    DrawPacketRange gpuEligiblePackets;
    DrawPacketRange directPackets;
    RenderPolicyReason reason;
};

struct RenderFrameExecutionPlan
{
    uint64 frameSequence;
    RenderViewPolicy viewPolicy;
    std::vector<RenderPassExecutionPlan> passes;
    RenderCapabilitySnapshot capabilities;
    RenderQualificationSnapshot qualification;
};
```

The plan becomes immutable after plan compilation. Frame-lifetime graph handles
are stored in graph pass data, not in the persistent plan or persistent pass
objects.

### 4.4 Semantic RHI capabilities

Replace a single indirect-count boolean as the renderer-facing decision source
with a structured capability record:

```cpp
enum class RHIIndirectSubmissionType : uint8
{
    None = 0,
    FixedCount,
    CountBuffer,
    IndirectCommandBuffer
};

struct RHIIndirectExecutionCapabilities
{
    RHIIndirectSubmissionType submissionType;
    bool supportsGpuCommandGeneration;
    bool supportsIndexedDraw;
    bool supportsCountBuffer;
    bool supportsFirstInstance;
    bool supportsDrawId;
    bool supportsResourceTableIndexing;
    uint32 maxCommandCount;
    uint32 commandAlignment;
};
```

Mapping target:

| Backend | Primary strategy | Compatibility strategy |
|---|---|---|
| DX12 | `ExecuteIndirect` with count buffer | Direct |
| Vulkan | `vkCmdDrawIndexedIndirectCount` when the feature is enabled | Fixed-count indirect or Direct |
| Metal | GPU-encoded `MTLIndirectCommandBuffer` | Direct |
| DX11 | Direct; optional fixed-count indirect batching | Direct |
| OpenGL | Direct or supported multi-draw fixed count | Direct |

`supportsIndirectDrawCount` remains during migration as a compatibility
projection and is removed only after all consumers use the structured record.

## 5. Implementation Tasks

Tasks are sequential unless an explicit dependency says otherwise. Each task
must land with green acceptance tests before the next behavior-changing task.

### Task 0 - Freeze and record the reference baseline

**Purpose:** Establish trustworthy pre-refactor evidence.

Work:

- build the current focused CPU, RenderPass, RenderScene, RenderContracts, and
  GPU-driven suites;
- run the registered Porsche Direct/GPU DX12 external tests with auto framing;
- record capture hashes, adapter, driver, qualification revision, packet/draw
  counts, and direct/GPU parity;
- preserve existing goldens unless an independently reviewed correctness fix
  requires a new baseline;
- store a machine-readable baseline artifact under the test artifact output,
  not in the source asset directory.

Acceptance:

- all focused CPU tests pass;
- Porsche produces visible objects and multiple material/mesh groups;
- Direct and forced GPU-driven captures pass the documented parity threshold;
- any still-failing real-asset gate is documented before architecture changes.

### Task 1 - Add vocabulary and immutable policy contracts

**Purpose:** Introduce types without changing rendering behavior.

Proposed files:

- `Render/Include/Render/Policy/RenderPolicyTypes.h`
- `Render/Include/Render/Policy/RenderFrameExecutionPlan.h`
- `Render/Include/Render/Policy/RenderPolicyDiagnostics.h`
- `Tests/RenderPolicyValidation/main.cpp`

Modify:

- `Render/CMakeLists.txt`
- `Tests/CMakeLists.txt`
- `Render/Include/Render/RenderDiagnostics.h`

Work:

- define GPU-driven tiers, visibility modes, submission modes, pass kinds, and
  stable reason enums;
- separate request, selected plan, and execution-report types;
- provide stable enum-to-name helpers;
- keep `RenderGPUDrivenMode` in RenderContracts as the external request;
- prohibit RHI pointers and RenderGraph handles in plan contracts;
- add validation for invalid enum values and default-safe construction.

Acceptance:

- no screenshot or draw-count changes;
- all existing tests pass;
- policy-contract tests cover every enum and invalid input.

### Task 2 - Introduce MeshBatch and RenderDrawPacket

**Purpose:** Establish a pass-independent batch and stateless draw description.

Proposed files:

- `Render/Include/Render/Renderer/MeshBatch.h`
- `Render/Include/Render/Renderer/RenderDrawPacket.h`
- `Render/Private/Renderer/RenderDrawPacket.cpp`
- `Tests/RenderDrawPacketValidation/main.cpp`

Modify:

- `Render/Include/Render/Renderer/RenderDrawItem.h`
- `Render/Private/Renderer/RenderDrawItem.cpp`
- `Render/Private/Renderer/RenderScene.cpp`
- `Render/CMakeLists.txt`
- `Tests/CMakeLists.txt`

Work:

- build one `MeshBatch` per actual submesh/primitive instead of assuming one;
- carry stable object, mesh, material, submesh, primitive-data, and flags;
- introduce explicit geometry/material/pipeline keys with equality and stable
  hashing;
- keep `RenderDrawItem` as a temporary compatibility adapter;
- validate missing material, missing submesh, malformed bounds, skinning, and
  transparent classification without accessing the RHI.

Acceptance:

- multi-submesh fixtures produce the expected batch count;
- opaque/masked/transparent classification matches the legacy path;
- packet keys are deterministic across repeated construction;
- the renderer still executes the legacy Direct path.

### Task 3 - Add retained packet caching and invalidation

**Purpose:** Avoid rebuilding static draw state every frame.

Proposed files:

- `Render/Include/Render/Renderer/RenderDrawPacketCache.h`
- `Render/Private/Renderer/RenderDrawPacketCache.cpp`

Work:

- retain pass-independent packet templates in a RenderScene-owned, value-only
  cache after an accepted primitive publication has passed transactional
  validation;
- address entries by the process-unique object identity and submesh ordinal,
  then require full static-signature equality across exact mesh/material
  generations, geometry, material mode, packet flags, pass-contract version,
  and an explicitly applicable shader-layout version;
- normalize per-frame primitive-data indices out of retained templates and
  patch them only into the frame-local compatibility projection; keep
  transforms, camera depth, sorting, and visibility outside the cache;
- keep skin palettes dynamic while allowing the static skinned packet template
  to remain cacheable; expose an explicit dynamic/deforming bypass for future
  geometry or binding state that can change without an exact resource-generation
  change. Particles remain on their existing feature path and never enter the
  mesh-packet cache;
- store no RHI object, descriptor, pipeline, registry strong reference, or
  completion token. Submitted resource lifetime continues through accepted
  scene references, submission stamping, registry last-use closure, and the
  existing retirement queue; stale CPU values can be erased immediately;
- add cache hit, miss, packet-build, dynamic-bypass, object-removal,
  contract-version, and stale exact-generation diagnostics.

Acceptance:

- unchanged static scene publications miss once, then reuse retained packet
  templates from both raster and ray-tracing draw-list consumers without
  increasing the packet-build count;
- material or mesh generation changes invalidate exactly the affected packets;
- explicitly dynamic/deforming inputs never populate or reuse a retained
  static entry; changing a skin palette alone keeps the packet hit because the
  palette is not packet state;
- cache entries contain values and exact handles only, and every handle used by
  a cached packet remains a member of the accepted scene resource-reference
  closure that is stamped after submission;
- a warmed cache hit performs no cache-owned entry/vector allocation. This is
  deliberately narrower than claiming allocation-free full-frame extraction
  or RenderScene publication, which still uses existing frame-local vectors.

### Task 4 - Add pass-specific MeshPassProcessors

**Purpose:** Centralize pass relevance and draw-packet construction without
making the global policy resolver understand material details.

Proposed files:

- `Render/Include/Render/Passes/MeshPassProcessor.h`
- `Render/Private/Passes/MeshPassProcessor.cpp`
- `Render/Private/Passes/DepthMeshPassProcessor.cpp`
- `Render/Private/Passes/OpaqueMeshPassProcessor.cpp`
- `Render/Private/Passes/TransparentMeshPassProcessor.cpp`
- `Render/Private/Passes/ShadowMeshPassProcessor.cpp`

Work:

- introduce pure value-only processors with no scene, registry, pipeline-cache,
  material-system, descriptor, RHI, or RenderGraph ownership. Each processor
  adapts the retained pass-independent packet template into a pass-specific
  packet, binding requirements, disposition, and exact group key;
- classify Depth, Opaque/Masked, Transparent, and Shadow independently. Depth
  and Opaque accept opaque/masked packets; Transparent accepts only transparent
  packets; Shadow accepts every packet carrying `CastsShadow`, preserving the
  current caster baseline regardless of main-view visibility;
- use explicit `GPUCandidate`, `Direct`, and `Skip` dispositions. Every relevant
  packet that is not a GPU candidate receives exactly one stable intrinsic or
  supplied-availability reason. The fixed precedence is transparent, skinned,
  special material, unsupported topology, unsupported index type, pipeline
  unavailable, geometry unavailable, resource pending, resource unavailable,
  and pass-requires-direct;
- keep missing material as an explicit default-material binding requirement,
  not as pending or rejected state. The existing packet cannot retain pending
  provenance, so callers must supply backend-neutral readiness facts; Task 4
  must never infer `ResourcePending` from an invalid handle;
- make `RenderSubmissionLayout` describe the vertex/binding/primitive-data
  layout, and make `RenderDrawGroupKey` cover pass, complete pipeline key,
  exact geometry key including submesh/index type, exact material key, and
  submission layout;
- sort GPU candidates lexicographically by the full key and then source ordinal,
  form contiguous groups in one linear pass, and never use hash iteration order
  or a hash value as the ordering key;
- generate value-only pass packet streams/group descriptions in parallel with
  the existing `RenderDrawItem` compatibility lists. Preserve opaque-before-
  masked ordering, transparent back-to-front order, and whole-scene Shadow
  enumeration;
- replace the quadratic local grouping in
  `PrepareGPUDrivenGraphCullInputs()` with the prepared/sorted key path and
  remove `OpaquePass::FindGPUDrivenGroupRepresentative()`. Existing pass
  command recording and all-or-nothing fallback remain unchanged;
- keep Transparent ordered and Direct. Keep Skinned Direct until a separately
  qualified skinned indirect path exists. Shadow preparation is Direct-only in
  this stage;
- defer final pipeline/residency resolution to Task 5, packet-range command
  consumption to Task 6, exactly-once packet IDs/partition diagnostics to Task
  7, and duplicate culling/reset cleanup to Task 8.

Acceptance:

- packet counts and direct images match the pre-migration baseline;
- no pass scans other pass-owned draw lists to find a representative item;
- grouping is deterministic, collision-safe, and O(n log n) sort plus O(n)
  grouping rather than quadratic lookup;
- every relevant rejected GPU packet has exactly one stable eligibility reason,
  and eligible plus rejected counts equal the relevant packet count;
- Transparent order, missing-material default binding, Shadow caster coverage,
  and skinned Direct behavior remain unchanged;
- Task 4 does not change submission policy, command recording, RHI/backend
  behavior, frame schema, qualification thresholds, or visual goldens.

### Task 5 - Implement RenderPolicyResolver and plan compilation

**Purpose:** Resolve engine-owned decisions once before graph construction.

**Progress:** The value contracts, pure deterministic resolver, legacy-policy
qualification injection, validation, and review gates are complete. The next
slice adds `RenderFramePlanCompiler` and integrates one immutable plan per view
without changing command recording.

Proposed files:

- `Render/Include/Render/Policy/RenderPolicyResolver.h`
- `Render/Private/Policy/RenderPolicyResolver.cpp`
- `Render/Include/Render/Policy/RenderFramePlanCompiler.h`
- `Render/Private/Policy/RenderFramePlanCompiler.cpp`

Modify:

- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Include/Render/GPUDriven/GPUDrivenPolicy.h`

Resolution order:

1. validate the external request;
2. resolve renderer/view path constraints;
3. inspect semantic device capabilities;
4. inspect path-specific qualification;
5. inspect required pipeline/shader contracts;
6. inspect resource/residency readiness;
7. apply pass-level permission;
8. partition concrete packets by eligibility;
9. apply workload-benefit policy when mode is `Auto`;
10. emit an immutable plan with explicit fallback lanes and reasons.

Work:

- wrap the existing GPU-driven policy rather than deleting it immediately;
- use a conservative deterministic benefit stub in this task; Task 15 adds
  measured thresholds, history, and hysteresis;
- keep ForceEnabled qualification bypass but preserve capability and pipeline
  checks;
- create one plan per view; do not store a single mutable global plan for all
  cameras;
- move resource/pipeline checks out of pass execution wherever they can be
  known during plan compilation;
- produce both selected-policy diagnostics and per-pass partition counts.

Acceptance:

- existing Auto/Force tests preserve behavior;
- invalid modes fail closed;
- plan compilation is deterministic for identical inputs;
- Pass execution no longer probes backend type or qualification state.

### Task 6 - Migrate Direct Depth/Opaque execution to DrawPackets

**Purpose:** Prove the new preparation path before changing submission.

Work:

- make Depth and Opaque consume Direct packet ranges from their plans;
- bind already-resolved packet state and primitive-data indices;
- keep existing Direct shaders and RHI draw calls;
- build RenderGraph resources from explicit frame/pass contexts;
- compare legacy and packet-based Direct outputs during a temporary dual-build
  validation mode;
- remove the dual-build mode when parity evidence is reviewed.

Acceptance:

- exact or documented-tolerance Direct image parity;
- equal visible primitive and submitted draw counts;
- malformed/ineligible packets fail before command recording;
- legacy Direct execution code can be removed only after parity passes.

### Task 7 - Implement hybrid DX12 Tier 1 submission

**Purpose:** Remove all-or-nothing pass fallback while preserving the proven
DX12 indirect implementation.

Work:

- partition each Depth/Opaque plan into mutually exclusive GPU and Direct
  packet ranges;
- build indirect groups only from GPU-eligible packets;
- render Direct fallback groups in the same pass with compatible depth/load
  semantics;
- keep Transparent Direct and sorted;
- keep Skinned and resource-pending groups Direct or skipped according to the
  explicit plan;
- ensure a packet ID is present in exactly one submitted or deliberately
  skipped partition;
- replace `AreGPUDriven*GroupsDrawable()` all-pass gates with plan validation;
- retain forced Direct as the reference path.

Acceptance:

- a fixture containing static, skinned, special-material, and transparent
  objects submits static opaque indirectly and the rest through expected paths;
- zero duplicate packet IDs and zero unaccounted eligible packet IDs;
- one failing group does not force unrelated groups to Direct;
- Direct and hybrid images meet parity requirements;
- DX12 Debug Layer and GPU-Based Validation report no errors.

### Task 8 - Decouple visibility from final CPU visibility

**Purpose:** Stop GPU culling an already-final CPU-visible draw list.

Proposed files:

- `Render/Include/Render/Visibility/RenderCandidateSet.h`
- `Render/Include/Render/Visibility/IRenderVisibilityProvider.h`
- `Render/Private/Visibility/CPUVisibilityProvider.cpp`
- `Render/Private/Visibility/GPUVisibilityProvider.cpp`

Work:

- separate coarse scene candidates from final visible packet streams;
- Direct plans use the CPU provider for fine visibility;
- GPU plans consume coarse candidates and perform fine frustum/distance culling
  on the GPU;
- share canonical bounds, matrix, frustum-plane, handedness, depth-range, and
  reversed-Z contracts between CPU and GPU implementations;
- treat invalid/non-finite bounds conservatively and report them explicitly;
- remove the duplicate CPU diagnostic cull and repeated `GPUCulling::BeginFrame`
  rebuilds from the production path;
- keep occlusion culling disabled until HZB ownership and temporal validity are
  defined and tested.

Acceptance:

- CPU and GPU providers agree on deterministic boundary fixtures;
- GPU input count can exceed final visible count without CPU pre-elimination;
- camera/frustum edge cases match across DX12 shader and CPU reference;
- no readback is required before submission;
- Direct fallback still uses the same canonical bounds contract.

### Task 9 - Replace mutable pass injection with frame-owned pass contexts

**Purpose:** Make graph recording safe for multiple views and later parallel
recording.

**Progress:** Task 9A is complete for the common contract plus Depth/Opaque,
Task 9B-1 is complete for raster Shadow producer/Opaque consumption,
Task 9B-2 isolates RayTracedShadow per-recording state with a
completion-aware temporal-history owner. 9B-2 preserves identity-ordered
submitted diagnostics (including rejection), projects realized full access
snapshots back to persistent history ownership, and bounds the legacy adapter
to one pending record. Task 9B-3 isolates ObjectVelocity setup/execute state
with recording-owned raster/material bindings and monotonic publication.
Task 9B-4 isolates Transparent with a value-owned ordered draw list,
graph-handle-only targets, private per-record view/object/light/cluster
bindings, and completion-aware submission retention. Task 9B-5 isolates
Skybox with a value-owned, mode-explicit `RenderSkySnapshot`, graph-only
attachments, private CB/descriptors, and retained sky/pipeline/layout
ownership. Cubemap, Procedural, SolidColor, Equirectangular tint fallback, and
Disabled behavior are packet-owned. Task 9B-6A removed the production binder
and bypass. Task 9B-6B1 then closed typed Opaque color/depth attachment
ownership, fail-closed source validation, and completion retention. Independent
and primary review passed; the shared primary-directional-light snapshot is
next.

Proposed files:

- `Render/Include/Render/Passes/RenderPassRecordContext.h`
- `Render/Include/Render/Passes/RenderPassExecutionData.h`

Work:

- evolve `IRenderPass::AddToGraph` to receive an immutable per-view plan and a
  frame-owned record context;
- store graph handles only inside frame/pass graph data;
- remove GPU-driven resource setters from Depth/Opaque;
- make execute lambdas consume only captured pass data and record commands;
- keep persistent pass objects limited to long-lived configuration and caches;
- reserve RayTracedShadow history during graph setup, commit it only after an
  actual submission token, roll it back for unsubmitted frames, and explicitly
  retain imported history resources through completion;
- declare compute-to-graphics dependencies in RenderGraph;
- continue executing on the graphics physical queue until async-compute queue
  submission is independently supported and measured.

Acceptance:

- two-view tests prove graph handles and plans cannot leak between views;
- pass execution contains no backend/policy branching;
- graph validation catches missing resource declarations;
- repeated-frame and resize tests pass without stale-handle behavior.

### Task 10 - Formalize submission strategies and complete DX12 Tier 1

**Purpose:** Separate renderer grouping/visibility from backend execution.

Proposed files:

- `Render/Include/Render/Submission/IRenderSubmissionStrategy.h`
- `Render/Private/Submission/DirectSubmissionStrategy.cpp`
- `Render/Private/Submission/IndirectCountSubmissionStrategy.cpp`
- `RHI/Include/RHI/RHIIndirectExecution.h`

Work:

- add structured indirect execution capabilities to `RHICapabilities`;
- validate command layout size, alignment, maximum count, count-buffer offset,
  first-instance behavior, and required resource states;
- keep DX12 command signatures cached by semantic layout;
- rebind any state invalidated by `ExecuteIndirect` according to the RHI
  contract;
- extend RHI conformance tests for count-buffer clamping and zero count;
- retain the old boolean capability only as a temporary projection.

Acceptance:

- DX12 Direct and Tier 1 paths use the same submission interface;
- command/count buffer overrun is impossible under validated inputs;
- zero-count groups emit no draw;
- all DX12 conformance, Debug Layer, GBV, parity, resize, and resource-lifetime
  tests pass.

### Task 11 - Add a persistent GPU Scene for Tier 2

**Purpose:** Reduce CPU scene traversal and per-frame command preparation for
large mostly-static scenes after the submission abstraction is stable.

Proposed files:

- `Render/Include/Render/GPUScene/GPUScene.h`
- `Render/Private/GPUScene/GPUScene.cpp`
- `Render/Include/Render/GPUScene/GPUSceneUpdate.h`
- `Render/Private/GPUScene/GPUSceneUploader.cpp`

Work:

- allocate stable primitive, bounds, transform, material-table, geometry-table,
  and draw-metadata indices;
- upload dirty ranges instead of rebuilding full scene buffers;
- use generation-checked handles and free-list reuse;
- double-buffer or version data whose CPU update may overlap in-flight GPU use;
- publish new entries only after required mesh/material resources are resident;
- compact visible packet/instance indices and indirect commands on the GPU;
- route Tier 2 output through the Task 10 submission strategies;
- keep per-group binding for Tier 1 hardware; add resource-table indexing only
  where the backend capability record guarantees it;
- keep HZB occlusion as a separate opt-in stage after frustum/distance parity.

Acceptance:

- DX12 Direct, Tier 1, and Tier 2 use the same submission interface;
- static scenes upload no full-scene data after warm-up;
- add/remove/update churn preserves generation correctness;
- resource eviction or re-upload cannot leave stale GPU references;
- no CPU wait is introduced for visibility or command generation;
- memory usage and dirty-upload bytes are reported per frame.

### Task 12 - Implement and qualify the Vulkan strategy

**Purpose:** Add the first non-DX12 modern backend using Vulkan semantics.

Work:

- query `drawIndirectCount` through the actual physical-device feature chain;
- populate structured capabilities from enabled rather than merely available
  features;
- implement `DrawIndexedIndirectCount` using the core Vulkan 1.2 command or the
  KHR form as appropriate;
- use fixed-count or Direct fallback when count-buffer submission is absent;
- validate buffer usage, alignment, synchronization2 access/stage mapping, and
  count limits;
- compile/validate the GPU visibility shaders for Vulkan conventions;
- create an independent Vulkan qualification record.

Acceptance:

- Vulkan validation layers remain clean;
- capability-negative fixtures never call an unsupported command;
- Direct/forced GPU visual parity passes on the same hermetic asset and camera;
- Vulkan stays Candidate until its complete gate mask is reviewed.

### Task 13 - Implement and qualify the Metal ICB strategy

**Purpose:** Use a Metal-native command-generation path rather than emulating
DX12 count-buffer semantics.

Work:

- introduce the minimum RHI indirect-command-buffer object and execution-range
  contract required by Render submission;
- query ICB rendering/compute and argument-buffer capability tiers per device;
- encode or reset commands with a compute encoder;
- explicitly declare resources referenced by ICB commands according to Metal
  residency/use-resource requirements;
- execute the valid command range without CPU visibility readback;
- validate macOS and supported Apple GPU families separately;
- create an independent Metal qualification record.

Acceptance:

- Metal API validation remains clean;
- unsupported ICB devices select Direct without pretending to support count
  buffers;
- Direct/ICB parity passes on the hermetic asset;
- command-buffer reuse and in-flight lifetime tests pass;
- Metal stays Candidate until its complete gate mask is reviewed.

### Task 14 - Preserve DX11/OpenGL compatibility behavior

**Purpose:** Keep secondary backends functional without constraining the modern
architecture.

Work:

- map both backends to Direct by default;
- permit fixed-count indirect/multi-draw only as a separately reported
  optimization where already supported and validated;
- do not require bindless tables, count buffers, GPU scene, or async compute;
- compile shared DrawPacket and MeshPassProcessor code on both backends;
- report the exact compatibility strategy in diagnostics.

Acceptance:

- Direct functional fixtures render on enabled compatibility backends;
- unsupported modern requests fall back with a stable reason;
- no DX11/OpenGL limitation weakens DX12/Vulkan/Metal contracts.

### Task 15 - Add Auto workload policy and hysteresis

**Purpose:** Make `Auto` choose a path that is both correct and economically
useful.

**Execution dependency:** Complete the Task 16A qualification schema and
machine-evidence ingestion slice before Task 15 so benefit logic consumes a
versioned backend/path/strategy qualification snapshot rather than the earlier
backend-wide projection. This slice does not promote a backend or change Auto.

Inputs:

- qualified backend/path revision;
- candidate and eligible packet counts;
- indirect group count and average group occupancy;
- dirty GPU-scene update bytes;
- resource residency completeness;
- recent CPU plan/submission cost;
- delayed GPU visibility/submission timings without synchronous readback;
- known adapter/driver deny-list entries.

Work:

- keep qualification and mandatory capability checks before performance logic;
- use per-pass thresholds rather than a single whole-frame threshold;
- start with conservative static thresholds gathered from benchmark scenes;
- add hysteresis and a minimum residency duration to prevent frame-to-frame
  oscillation;
- reset history after backend, adapter, resolution class, scene class, or major
  shader-contract changes;
- expose why Auto selected Direct or GPU-driven.

Acceptance:

- small scenes remain Direct when indirect setup costs more;
- large stable scenes select a qualified GPU-driven path;
- selection cannot oscillate within the configured stability window;
- ForceEnabled/ForceDisabled remain deterministic and ignore benefit heuristics;
- Auto introduces no synchronous GPU timing readback.

### Task 16 - Upgrade qualification, diagnostics, and Samples

**Purpose:** Close the production workflow after the architecture is stable.

**Execution split:** Task 16A (qualification schema, evidence ingestion, and
hermetic evidence) runs after Task 14 and before Task 15. Task 16B (final
diagnostics, Sample cleanup, adapter matrix, and promotion) runs after Task 15.
The task number remains unchanged because both slices own one qualification
workflow.

Work:

- key qualification by backend, submission strategy, renderer path, shader
  contract version, and qualification revision;
- retain adapter/driver constraints and scoped deny-list entries;
- generate reviewed qualification data from machine-readable CI evidence, then
  compile the approved projection into the runtime;
- report request, selected tier, visibility mode, submission strategy,
  eligibility counts/reasons, candidate/visible counts, group occupancy,
  command counts, Direct fallback counts, timings, and memory/update bytes;
- emit JSON tool artifacts in addition to human-readable logs;
- keep ModelViewer CLI limited to request overrides, scene setup, captures, and
  observable assertions;
- remove Sample access to renderer internals or backend capability decisions;
- run hermetic and external real-asset qualification suites;
- promote `Auto` only after every required gate is recorded.

Acceptance:

- DX12, Vulkan, and Metal records are independent;
- a backend/path cannot claim Qualified with a missing gate;
- ModelViewer tests assert public diagnostics only;
- the hermetic multi-mesh/material asset closes `RealAssetRegression`;
- required adapter/driver matrices close the final promotion gate;
- unchanged `Auto` commands begin using GPU-driven only after promotion.

## 6. Pass Migration Matrix

| Pass | Initial path | First GPU-driven scope | Deferred work |
|---|---|---|---|
| Depth | Direct reference | Static opaque/masked indirect groups | Skinned depth indirect |
| Opaque Forward+ | Direct reference | Static opaque/masked hybrid submission | Bindless heterogeneous materials |
| Directional Shadow | Direct | Separate GPU visibility stream after Opaque stabilizes | Cascaded reuse and skinned indirect |
| Transparent | Sorted Direct | None in initial plan | OIT or specialized particle paths |
| Object Velocity | Direct | Reuse primitive IDs and visibility after correctness proof | GPU-generated dynamic velocity lists |
| Particles | Dedicated path | Independent system | GPU particle command generation |
| UI/Debug | Direct | None | Not required |
| Ray tracing | Independent AS path | Reuse stable primitive/GPU-scene IDs only | Ray workload policy remains separate |

Forward+ remains the primary raster pipeline. Deferred rendering, mesh shaders,
virtualized geometry, and Editor visualization are not prerequisites for this
plan and must not be mixed into the initial migration.

## 7. Validation Strategy

### 7.1 CPU-only contract tests

- request and policy resolution;
- capability mapping and invalid-capability fail-closed behavior;
- packet construction and actual submesh counts;
- packet/group key determinism;
- cache invalidation and generation reuse;
- per-pass eligibility and partition completeness;
- CPU/GPU reference frustum boundary math;
- Auto thresholds and hysteresis state transitions;
- diagnostics schema completeness.

### 7.2 RHI conformance tests

- indirect command layout and alignment;
- zero, one, maximum, and overflow draw counts;
- count-buffer clamping;
- buffer state/access transitions;
- compute-write to indirect-read synchronization;
- unsupported-capability behavior;
- DX12 command signature semantics;
- Vulkan enabled-feature and command selection;
- Metal ICB creation, reset, encode, resource use, and execution range.

### 7.3 Renderer native fixtures

Required deterministic scenes:

1. one static opaque object;
2. many identical instances;
3. multiple meshes and materials;
4. mixed opaque and masked objects;
5. transparent objects intersecting opaque objects;
6. mixed static and skinned objects;
7. missing/pending mesh or material resources;
8. malformed and boundary-touching bounds;
9. zero-visible-object frame;
10. multiple views with different visibility;
11. repeated resize and history reset;
12. resource eviction/re-upload while frames are in flight;
13. large static scene with low update rate;
14. high-churn scene where Direct should remain preferable.

For every applicable fixture record:

- visible packet IDs;
- GPU and Direct partitions;
- skipped packet IDs and reasons;
- direct/indirect group and draw counts;
- backend strategy and capability snapshot;
- frame capture and parity metrics;
- validation-layer messages;
- CPU plan/submission timings and GPU timings where available;
- resource and dirty-upload bytes.

### 7.4 Visual and production gates

- deterministic Direct golden remains the reference;
- forced GPU-driven compares the identical visible/material set;
- mixed hybrid fixtures prove both lanes render in one frame;
- external Porsche remains a developer stress candidate;
- a small checked-in redistributable asset becomes hermetic qualification
  evidence;
- cross-backend comparisons use identical source/cooked hashes, camera framing,
  exposure, and color-output contracts.

## 8. Performance Gates

Correctness gates land before performance claims. After warm-up, measure at
least 100, 1,000, 10,000, and 50,000 candidate packets where supported.

Required properties:

- no per-frame rebuild of unchanged static packet state;
- no synchronous visibility or timing readback;
- no per-frame full GPU-scene upload for unchanged static scenes;
- no quadratic group lookup in production preparation paths;
- Direct remains preferred for workloads where GPU setup is not beneficial;
- Auto thresholds are based on recorded benchmark evidence rather than backend
  identity alone;
- CPU plan/submission time, GPU visibility time, draw/group occupancy, memory,
  and dirty-upload bytes are included in benchmark artifacts.

Initial refactor tasks must remain within a reviewed small regression budget
against the Direct baseline. Final numerical promotion thresholds are recorded
per benchmark and adapter class after Task 11 rather than guessed in advance.

## 9. Threading and Lifetime Rules

- Game/World extraction writes only through `RenderFramePacketBuilder`.
- The published packet is immutable.
- Render thread owns retained scene updates, packet caches, plan compilation,
  and graph construction.
- Frame plans and graph pass data use frame-owned storage.
- Graph handles never survive their RenderGraph instance.
- GPU Scene indices use generations and are not reused while referenced by an
  in-flight submission.
- Pipelines, descriptor resources, command buffers, and cached packet owners use
  existing completion tokens and retirement queues.
- Async compute is not enabled merely because a compute queue exists; graph and
  submission synchronization must first be validated and benchmarked.

## 10. Risk Controls

| Risk | Control |
|---|---|
| Duplicate or missing draws in hybrid mode | Stable packet IDs and exactly-one partition accounting |
| Cache returns stale resources | Resource generations, contract versions, retirement tokens |
| CPU/GPU visibility mismatch | One canonical bounds/frustum contract and boundary fixtures |
| Per-frame policy oscillation | Threshold hysteresis and minimum residency duration |
| Vulkan advertises but does not enable a feature | Populate capabilities from enabled feature chains |
| Metal ICB references undeclared resources | Explicit resource table/use-resource declarations and validation |
| Runtime fallback changes graph dependencies | Resolve expected fallback before graph compilation |
| Unexpected command-recording failure duplicates work | Fail/report current path; disable or replan on a later frame |
| Multi-view state leakage | Per-view immutable plans and frame-owned graph pass data |
| GPU Scene index reuse races | Generation handles plus completion-aware retirement |
| Compatibility backends distort modern RHI | Separate semantic strategies with Direct as common denominator |
| Large refactor obscures visual regression | Direct-only parity gates after every preparation-layer migration |

## 11. Milestones and Stop/Go Gates

### Milestone M0 - Trusted baseline

Contains Task 0. No architectural behavior change begins until the current
Direct/GPU and real-asset evidence is recorded or its remaining failure is
explicitly accepted as a known baseline defect.

### Milestone M1 - Draw architecture foundation

Contains Tasks 1-6. Exit condition: Depth/Opaque render through DrawPackets on
the Direct path with reference parity. No new backend work begins before M1.

### Milestone M2 - Productionized DX12 Tier 1

Contains Tasks 7-10. Exit condition: hybrid
per-group fallback, decoupled visibility inputs, frame-owned pass contexts, and
one formal submission interface with clean DX12 validation/parity.

### Milestone M3 - GPU-resident scene

Contains Task 11 and DX12 Tier 2 validation. Exit condition: dirty
incremental scene updates, no CPU final-visible prerequisite, no readback, and
stable resource lifetime under churn.

### Milestone M4 - Vulkan

Contains Task 12. Exit condition: Vulkan Candidate evidence exists for the same
renderer contract; DX12 qualification does not affect Vulkan.

### Milestone M5 - Metal

Contains Task 13. Exit condition: Metal ICB Candidate evidence exists for the
same visible/material contract through a Metal-native submission strategy.

### Milestone M6 - Shipping policy and qualification

Contains Tasks 14-16. Exit condition: compatibility backends remain functional,
Task 16A establishes path/strategy qualification before Task 15 consumes it,
Auto has measured benefit logic and hysteresis, diagnostics are complete, and
each modern backend is promoted only by its own reviewed evidence in Task 16B.

## 12. Explicit Sequencing Constraints

- Do not implement Vulkan or Metal by removing the current DX12 guard before
  semantic RHI capabilities and submission strategies exist.
- Do not introduce the persistent GPU Scene before packet identity, cache
  invalidation, and hybrid partition tests are stable.
- Do not enable GPU occlusion culling before HZB ownership, history invalidation,
  and camera-cut behavior are specified.
- Do not delete Direct or legacy parity adapters until their replacement passes
  the task-local acceptance gate.
- Do not promote `Auto` by changing a default or qualification bit before test
  evidence lands.
- Do not mix Deferred, mesh-shader, virtualized-geometry, Editor, or skeletal
  feature implementation into M1/M2.

## 13. Recommended Immediate Slice

The first implementation run should stop after **Task 1 through Task 2**:

1. add policy/tier/submission vocabulary and CPU validation;
2. add `MeshBatch` and `RenderDrawPacket` contracts;
3. construct real submesh-aware batches behind the existing Direct path;
4. keep all existing rendering and GPU-driven submission behavior unchanged;
5. run contract, RenderScene, RenderPass, GPU-driven, ModelViewer, and real-asset
   baseline tests.

This produces an independently reviewable foundation. Task 3 caching and Task 4
pass processors should start only after the new packet identity and submesh
semantics are accepted.
