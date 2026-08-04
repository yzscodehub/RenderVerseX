# Render Policy and Draw Packet Remaining Execution TODO

**Status:** Tasks 0-8, all Task 9 slices through Task 9B-6B2b, Tasks
10A/10B/10C and Tasks 11A/11B/11C are complete and reviewed; Task 11D-D1a and
Task 11D-D1b-a are complete, and Task 11D-D1b-b is next
**Baseline commit:** `80838c04 feat(render): add mesh pass preparation`
**Scope:** Engine core and framework; Editor excluded
**Primary backends:** DX12, Vulkan, Metal
**Compatibility backends:** DX11, OpenGL

This checklist operationalizes Tasks 5-16 from
`2026-08-02-render-policy-draw-packet-implementation-plan.md`. The design plan
remains authoritative for architecture and acceptance semantics; this file is
the execution ledger used to implement, review, validate, and commit each
remaining stage.

## 1. Execution Rules

- [ ] Keep forced Direct as the correctness and visual reference until every
  replacement path passes its task-local parity gate.
- [ ] Resolve expected policy, availability, and fallback before RenderGraph
  compilation; an unexpected recording failure is reported and never triggers
  same-frame double submission.
- [ ] Keep one immutable plan per view. Do not store graph handles or plan data
  in shared mutable pass state.
- [ ] Treat packet identity, packet partitioning, resource generation, and
  submission lifetime as explicit contracts rather than inferred conventions.
- [ ] Do not enable HZB occlusion, async-compute submission, bindless material
  tables, mesh shaders, deferred rendering, virtualized geometry, or Editor
  work inside this plan.
- [ ] Do not promote `Auto` by changing a default. Qualification evidence lands
  first; the unchanged resolver derives the promoted result.
- [ ] Run DX11/OpenGL compile and Direct smoke guards after shared Render/RHI
  changes. Task 14 is the formal compatibility closure, not the first time the
  compatibility backends are checked.
- [ ] Add diagnostics incrementally from Task 5 onward. Task 16 closes and
  versions the schema; it does not retrofit observability at the end.
- [ ] Keep implementation delegation bounded. The primary agent owns
  architecture, acceptance, integration review, and the final commit scope.
- [ ] Use one reviewed commit per independently reversible slice. Do not combine
  a failed gate with the next stage to hide the failure.

## 2. Critical Path and Parallel Boundaries

```text
M1 foundation closure
  Task 5 policy resolver and frame-plan compiler
    -> Task 6 Direct Depth/Opaque DrawPacket migration

M2 DX12 Tier 1
  Task 7 hybrid partition and submission
    -> Task 8 candidate visibility separation
    -> Task 9 frame-owned pass contexts
    -> Task 10 formal submission strategies and DX12 Tier 1 closure

After Task 10 renderer-facing contracts are frozen
  +-> Task 11 DX12 GPU Scene / Tier 2
  +-> Task 12 Vulkan Tier 1 strategy
  +-> Task 13 Metal ICB strategy

M6 shipping closure
  Task 14 compatibility closure
    -> Task 16A qualification schema and evidence ingestion
    -> Task 15 measured Auto policy
    -> Task 16B diagnostics, Samples, and promotion
```

Tasks 11-13 may use separate platform worktrees and agents after Task 10, but
their ownership must remain disjoint: Task 11 owns GPU Scene files, Task 12 owns
Vulkan backend files, and Task 13 owns Metal backend files. Any requirement to
change shared plan types, `IRenderSubmissionStrategy`, semantic RHI execution
contracts, or shared shaders stops that lane and returns to a primary-owned
contract revision, cross-backend compile/conformance run, and explicit
re-freeze. Integration order is evidence-driven rather than an artificial
Task 11/12/13 serialization.

## 3. M1 Closure - Draw Architecture Foundation

### Task 5 - RenderPolicyResolver and Per-View Frame Plan

#### 5A. Freeze plan input/output contracts

- [x] Inventory every current policy, backend, qualification, pipeline,
  resource-residency, and pass-permission probe in `SceneRenderer`,
  `DepthPrepass`, `OpaquePass`, and `GPUDrivenPolicy`.
- [x] Define value-only resolver input facts: validated request, renderer/view
  constraints, semantic device capabilities, qualification snapshot,
  pipeline/shader readiness, resource readiness, pass permission, and workload
  counts.
- [x] Define immutable per-view output values: selected tier, visibility mode,
  submission strategy, pass plans, Direct/GPU/Skip packet references, stable
  fallback reasons, and partition counts.
- [x] Keep Task 5 packet references frame-local and deterministic; do not claim
  Task 7 exactly-once packet identity before the packet-ID contract lands.
- [x] Define stable enum values, names, comparison, validation, and diagnostic
  projection for every new plan value.
- [x] Prove resolver and compiler values own no scene, registry, RHI,
  descriptor, RenderGraph, or pass-object pointers.

#### 5B. Implement deterministic policy resolution

- [x] Add `RenderPolicyResolver` with the documented fail-closed resolution
  order.
- [x] Wrap the existing GPU-driven policy and qualification records; do not
  delete proven behavior in the first slice.
- [x] Preserve ForceDisabled determinism.
- [x] Preserve ForceEnabled qualification bypass while still enforcing device,
  pipeline, shader, and resource requirements.
- [x] Implement a conservative deterministic `Auto` benefit stub. Do not add
  timing history or hysteresis before Task 15.
- [x] Make missing or malformed facts select Direct/Skip with an explicit
  reason; never silently reinterpret an invalid value as enabled.

#### 5C. Compile and integrate one plan per view

- [x] Add `RenderFramePlanCompiler` and compile after pass-packet preparation
  but before RenderGraph construction.
- [x] Resolve current Depth/Opaque packet eligibility into planned lanes without
  changing command recording yet.
- [x] Carry Transparent as ordered Direct and Shadow as Direct-only for this
  stage.
- [x] Move predictable backend/qualification/pipeline/residency decisions out
  of pass execution; retain only validation of unexpected runtime failures.
- [x] Store frame plans in frame/view-owned lifetime and reset them on every
  accepted frame, rejected frame, resize, and view replacement.
- [x] Export selected policy plus per-pass relevant/GPU/Direct/Skip counts and
  reason counts through public render diagnostics.

#### 5D. Validate and commit Task 5

- [x] Add resolver matrix tests for Auto, ForceEnabled, ForceDisabled, invalid
  mode, missing capability, unqualified backend, pipeline failure, pending
  resource, and pass-disabled cases.
- [x] Add determinism tests that permute identical input construction order and
  compare the complete plan.
- [x] Add two-view tests proving independent plans before pass-context migration.
- [x] Add source/contract checks proving pass execution no longer probes backend
  type or qualification state.
- [x] Run RenderPolicy, MeshPassProcessor, RenderScene, GPUDriven, RenderPass,
  RenderContracts, extraction, and Direct/GPU parity gates.
- [x] Record a phase-log entry and primary review.
- [x] Commit slice 5A: `feat(render): add per-view render policy plans`.
- [x] Commit slice 5B if integration is too large for one review:
  `refactor(render): compile frame policy before graph construction`.

**Stop gate:** Task 6 cannot start until identical inputs produce identical
plans, invalid inputs fail closed, and pass execution has no backend or
qualification policy branch.

### Task 6 - Direct Depth/Opaque DrawPacket Execution

#### 6A. Direct Depth migration

- [x] Add a packet-range Direct recorder that consumes the Task 5 Depth plan.
- [x] Bind packet geometry, primitive-data index, object/skinning data, and
  masked/default-material requirements without rebuilding pass classification.
- [x] Reject malformed ranges and unavailable required bindings before command
  recording.
- [x] Run legacy and packet Depth construction in temporary dual-build mode and
  compare packet/draw counts without recording twice.
- [x] Add static, masked, skinned, missing-material, invalid-submesh, and empty
  Depth fixtures.
- [x] Capture Direct reference parity before removing the legacy Depth loop.

#### 6B. Direct Opaque migration

- [x] Add packet-range Direct recording for opaque and masked lanes.
- [x] Preserve opaque-before-masked ordering, material fallback behavior,
  tangent-basis normal-map gating, shadow receiver state, and object motion
  constants.
- [x] Keep Transparent in its existing sorted Direct pass.
- [x] Run legacy and packet Opaque construction in dual-build mode and compare
  source ordinals, exact packet values, draw arguments, material handles, and
  counts.
- [x] Add multi-submesh/multi-material and skinned Direct fixtures.
- [x] Prove the packet path produces the existing Direct golden and Porsche
  output at the approved tolerance.

#### 6C. Remove temporary legacy consumers

- [x] Review dual-build evidence and remove only the replaced Depth/Opaque
  command consumers.
- [x] Retain adapters still required by Transparent, Shadow, ObjectVelocity, or
  diagnostics and mark their removal owner explicitly.
- [x] Confirm no pass reclassifies material/pass eligibility while recording.
- [x] Run M1 exit suite across DX12 Direct, Vulkan/Metal Direct on the configured
  platform matrix, plus DX11 Direct and OpenGL compile/smoke coverage. Record an
  unavailable platform as an open milestone coverage gate rather than silently
  skipping it.
- [x] Record M1 exit evidence and commit Depth and Opaque migrations as separate
  reversible commits.

Retained-adapter removal owners are explicit: Task 7 replaces whole-pass
GPU completeness gates, Task 8 replaces draw-list/source-ordinal candidate
mapping, and Task 9 replaces persistent pass setters plus captured
Transparent, Shadow, ObjectVelocity, and diagnostics execution inputs.
Windows DX12 and DX11 Direct gates pass. Vulkan and OpenGL were executed and
retain their pre-existing pipeline/shader validation gates for Tasks 12 and 14;
Metal is unavailable on the configured Windows host and remains a Task 13
platform gate.

**M1 exit gate:** Depth and Opaque render through DrawPackets on the Direct path
with exact/documented parity and no legacy duplicate submission.

## 4. M2 - Productionized DX12 Tier 1

### Task 7 - Hybrid DX12 Packet Partition

- [x] Define a stable per-frame packet ID covering view, pass, object/primitive,
  exact mesh generation/submesh, and source identity.
- [x] Validate uniqueness and deterministic regeneration for identical frames.
- [x] Compile mutually exclusive GPU, Direct, and deliberate-Skip partitions.
- [x] Add exactly-once accounting: every relevant packet ID appears in exactly
  one terminal lane.
- [x] Replace all-pass `AreGPUDriven*GroupsDrawable()` gates with validated plan
  partitions.
- [x] Record GPU lanes indirectly and Direct fallback lanes in the same
  Depth/Opaque pass with compatible load/depth semantics.
- [x] Keep Transparent Direct, Skinned Direct, and unavailable/pending behavior
  exactly as specified by the plan reason.
- [x] Define late recording failure behavior: fail/report the current lane and
  replan a later frame; never replay already-recorded packets in the same frame.
- [x] Add mixed static/skinned/special/missing/pending/transparent fixtures.
- [x] Assert zero duplicate IDs, zero unaccounted eligible IDs, and one failing
  group not forcing unrelated groups to Direct.
- [x] Run DX12 Debug Layer, GBV, repeated-frame, resize, resource-lifetime, and
  Direct/hybrid parity gates.
- [x] Commit packet identity/accounting separately from Depth/Opaque hybrid
  execution when useful for review.

**Stop gate:** No visibility redesign begins until hybrid exactly-once
accounting is green under failure injection.

### Task 8 - Candidate Visibility Separation

- [x] Define `RenderCandidateSet` as coarse scene candidates, distinct from
  final visible packet streams.
- [x] Define `IRenderVisibilityProvider` with CPU and GPU implementations.
- [x] Establish one canonical bounds, transform, frustum plane, handedness,
  clip-depth, reverse-Z, and invalid-bounds contract shared by CPU reference and
  GPU shader code.
- [x] Feed Direct plans through CPU fine visibility.
- [x] Feed GPU plans coarse candidates and perform final frustum/distance
  visibility on the GPU without CPU pre-elimination.
- [x] Preserve stable packet/source mapping through compaction.
- [x] Remove duplicate diagnostic culls and repeated production
  `GPUCulling::BeginFrame` rebuilds.
- [x] Keep HZB occlusion disabled and report that it is unavailable by design.
- [x] Add boundary-touching, non-finite, behind-camera, near/far plane,
  zero-object, and CPU/GPU parity fixtures.
- [x] Assert GPU candidate input may exceed final visible count and requires no
  pre-submission readback.
- [x] Benchmark candidate preparation to confirm no quadratic regression.

Task 8 additionally closes a review-discovered cross-frame ownership defect:
Depth and Opaque each use per-in-flight-slot instance/constants upload buffers,
descriptor sets, and input access snapshots. The selected slot is written only
after `RenderContext::BeginFrame` waits for its previous submission. Exact GPU
executed counts remain unavailable without readback and are reported separately
from submitted upper bounds and CPU reference visibility.

### Task 9 - Frame-Owned Pass Record Contexts

#### 9A. Core contract and Depth/Opaque migration

- [x] Define immutable `RenderPassRecordContext`, graph-owned frame snapshots,
  typed `RenderPassExecutionData`, and identity-gated result publication.
- [x] Evolve `IRenderPass::AddToGraph` to receive captured per-view pass data;
  keep only a value-capture compatibility adapter for unmigrated passes.
- [x] Move Depth/Opaque plan, scene/list, shadow, graph-handle, and GPU-culling
  inputs into graph-owned data and independent per-recording GPU state.
- [x] Remove production Depth/Opaque GPU-resource setter injection; standalone
  compatibility entry points remain only until Task 9B cleanup.
- [x] Make Depth/Opaque graph callbacks execute graph-owned recorder clones and
  never read persistent per-frame pass mailboxes.
- [x] Declare compute-write to indirect/count/instance reads in RenderGraph and
  keep execution on the graphics physical queue.
- [x] Add graph identity/generation to RG handles and fail closed for foreign,
  stale, incomplete GPU, invalid shadow, and missing-declaration contexts.
- [x] Seal and submission-retain every RHI object used by a GPU-culling
  recording; verify release occurs only after its completion point.
- [x] Re-run Direct/GPU parity, repeated-frame, Porsche, GBV, resize/lifetime,
  and DX11 Direct gates.

#### 9B. Remaining scene passes and binder removal

- [x] Migrate raster Shadow producer outputs and Opaque consumption to the shared
  graph-owned results channel.
- [x] Migrate RayTracedShadow per-recording handles/output/stats to graph-owned
  data; reserve history during setup, commit only on submitted work, roll back
  unsubmitted reservations, and retain imported history through completion.
- [x] Migrate ObjectVelocity scene/list/targets and identity-gated stats.
  The pass owns recording-local View/Object/Material CB + descriptor snapshots,
  immutable planned draws, explicit velocity ReadWrite/depth Read declarations,
  submission-retained attachment/descriptor resources, and the DefaultLit
  pipeline/set-layout ownership bridge required by DX12/Vulkan/Metal. Older publication
  identities cannot regress a newer diagnostic snapshot.
- [x] Migrate Transparent scene/list/targets with truthful color ReadWrite and
  depth Read dependencies.
- [x] Migrate Skybox to a typed record that owns value-copied sky state,
  graph-only targets, private constants/descriptors, and completion-retained
  pipeline/layout/view/texture resources. Legacy Setup/Execute/targets and
  ViewData recording fail closed.
- [x] Remove `RenderFrameResourceBinder`, `UpdatePassResources`, and the direct
  `ExecutePasses` bypass; route every registry pass through the immutable
  record context; remove migrated Transparent/Skybox frame-state setters and
  Skybox frame-packet setter projection.
- [x] Close typed Opaque attachment ownership: validate current graph resource
  descriptions before declaring usage, resolve color/depth views only from
  graph handles, and retain both views and parent textures through submission
  completion. Foreign, stale, forged, incomplete, and view-creation failures
  declare no attachment usage and never fall back to raw setters.
- [x] Move the selected primary directional light into the frame snapshot and
  make DefaultLit, raster Shadow, and RayTracedShadow consume that one
  value-owned selection. Keep long-lived feature enablement separate from
  per-frame shadow eligibility.
- [x] Remove the remaining Depth/Opaque/Shadow standalone frame-state
  compatibility setters after the shared light snapshot migration closes.
- [x] Add reverse-order two-graph, caller-mutation, resize/target-replacement,
  rejected-frame, empty-list, stale-context, and in-flight resource fixtures.

### Task 10 - Submission Strategy and DX12 Tier 1 Closure

#### 10A. Semantic RHI contract

- [x] Replace backend-name inference with structured indirect execution
  capabilities: fixed-count, count-buffer, first-instance, command stride,
  alignment, limits, and required states.
- [x] Add validated RHI descriptors for indexed indirect execution and count
  buffers.
- [x] Freeze renderer-facing `RenderSubmissionMode` semantics for Direct,
  fixed/count-buffer indirect, and the existing `EncodedCommandBuffer`
  extension point without pretending the Metal-specific RHI object exists yet.
- [x] Keep the old boolean capability only as a temporary projection with a
  named removal task.
- [x] Add zero/one/max/overflow/count-clamp/alignment/state conformance tests.

#### 10B. Strategy interface and DX12 implementation

- [x] Define `IRenderSubmissionStrategy` with Direct and IndirectCount
  implementations plus a typed backend-native encoded-command-buffer extension
  boundary for Task 13.
- [x] Make renderer grouping/visibility independent of backend command APIs.
- [x] Cache DX12 command signatures by semantic command layout.
- [x] Validate command/count buffer ranges before `ExecuteIndirect`.
- [x] Rebind state invalidated by indirect execution according to the RHI
  contract.
- [x] Route both DX12 Direct and Tier 1 through the same strategy interface.
- [x] Remove obsolete pass-local DX12 submission branches and capability probes.

#### 10C. M2 validation

- [x] Run all RHI conformance and cross-backend shared-contract tests.
- [x] Run DX12 Debug Layer, GBV, Direct/hybrid parity, resize, zero-visible,
  maximum-count, and in-flight retirement fixtures.
- [x] Compile shared contracts and run Direct smoke on every configured primary
  backend; record unavailable Metal/Vulkan hosts as open M2 coverage gates.
- [x] Emit one machine-recorded M2 exit artifact aggregating Task 7 exactly-once
  failure injection, Task 8 CPU/GPU bounds parity, Task 9 multi-view/lifetime
  negatives, and Task 10 RHI conformance.
- [x] Verify zero-count groups emit no draw and buffer overrun is impossible.
- [x] Record CPU plan/submission cost and group occupancy without making Auto
  decisions yet.
- [x] Freeze the renderer-facing strategy and semantic RHI baseline before
  Tasks 11-13 branch. Task 13 may add Metal-private ICB objects behind the
  encoded-command-buffer extension; any shared contract change requires a
  primary-owned revision and a new cross-backend freeze.

**M2 exit gate:** DX12 Tier 1 supports per-group hybrid fallback through one
formal submission interface with clean validation and parity.

## 5. M3 - Persistent GPU Scene and DX12 Tier 2

### Task 11 - GPU Scene

- [x] Freeze GPU Scene table schemas for primitive, bounds, transform,
  material, geometry, draw metadata, and generation/version data.
- [x] Define stable generation-checked indices and completion-aware free-list
  reuse.
- [x] Define publication rules: entries become visible only after dependent
  mesh/material generations are resident.
- [x] Implement persistent buffers and dirty-range upload planning.
- [x] Double-buffer or version CPU-updated data that may overlap in-flight GPU
  reads.
- [x] Handle add/remove/transform/material/mesh/reload/evict operations without
  stale GPU references.
- [ ] Move GPU visibility and command generation to stable GPU Scene indices.
- [ ] Compact visible indices and commands without synchronous CPU readback.
- [ ] Route Tier 2 output through the Task 10 submission strategies.
- [ ] Keep Tier 1 per-group binding on devices without required table-indexing
  capabilities.
- [ ] Add memory, capacity, dirty-byte, full-upload, churn, and retirement
  diagnostics.
- [ ] Add warm static, high churn, generation reuse, resource eviction,
  in-flight update, and capacity growth fixtures.
- [ ] Re-run two-view, rejected-frame, resize, and in-flight failure injection
  against persistent Tier 2 buffers and generation retirement.
- [x] Prove unchanged static scenes perform no full-scene upload after warm-up.
- [ ] Benchmark 100/1k/10k/50k candidates and record CPU/GPU costs; do not tune
  Auto until Task 15.

**M3 exit gate:** DX12 Tier 2 has incremental updates, stable lifetime, no CPU
final-visible prerequisite, and no synchronous readback.

## 6. M4/M5 - Modern Backend Strategies

### Task 12 - Vulkan Tier 1 Strategy and Qualification Candidate

- [ ] Audit physical-device availability versus actually enabled feature and
  extension chains.
- [ ] Populate semantic indirect capabilities from enabled Vulkan state.
- [ ] Implement indexed indirect count through Vulkan 1.2 core or KHR entry
  points, selected explicitly.
- [ ] Implement fixed-count or Direct fallback when count-buffer submission is
  unavailable.
- [ ] Validate indirect/count buffer usage, offset/stride alignment, count
  limits, synchronization2 stage/access mapping, and queue ownership.
- [ ] Validate shader compilation and layout conventions for Vulkan.
- [ ] Route Vulkan through the Task 10 submission strategy without DX12
  conditions in Render code.
- [ ] Add capability-negative fixtures that prove unsupported commands are
  never called.
- [ ] Run Vulkan validation layers, repeated-frame/resize, Direct/forced-GPU
  parity, and resource-lifetime gates on the hermetic asset.
- [ ] Re-run two-view, rejected-frame, resize, queue-ownership, and in-flight
  failure injection on the Vulkan strategy rather than relying only on Task 9
  infrastructure tests.
- [ ] Create an independent Vulkan qualification revision and keep it Candidate
  until every required gate is reviewed.

### Task 13 - Metal ICB Strategy and Qualification Candidate

- [ ] Confirm macOS build/CI hardware and supported Apple GPU family matrix
  before implementation acceptance.
- [ ] Define the minimum RHI indirect command buffer, execution range, reset,
  encode, and resource-use contracts.
- [ ] Query ICB render/compute and argument-buffer capability tiers from the
  active Metal device.
- [ ] Implement compute encoding/reset of ICB commands and valid execution
  ranges without CPU visibility readback.
- [ ] Explicitly declare every buffer, texture, heap, and argument resource
  referenced by ICB commands.
- [ ] Version/reuse command buffers safely across in-flight frames.
- [ ] Route Metal through the Task 10 strategy interface and select Direct on
  unsupported devices.
- [ ] Add capability-negative, reset/reuse, resource-residency, in-flight, and
  execution-range fixtures.
- [ ] Re-run two-view, rejected-frame, resize, and in-flight failure injection
  against Metal ICB encoding, resource declaration, and reuse.
- [ ] Run Metal API validation plus Direct/ICB hermetic parity on each supported
  family class.
- [ ] Create an independent Metal qualification revision and keep it Candidate
  until every required gate is reviewed.

**Backend rule:** DX12 qualification never promotes Vulkan or Metal, and Vulkan
feature availability never implies the feature was enabled.

## 7. M6 - Compatibility, Auto Policy, and Shipping Qualification

### Task 14 - DX11/OpenGL Compatibility Closure

- [ ] Formalize Direct as the default strategy for DX11 and OpenGL.
- [ ] Report fixed-count indirect/multi-draw only as a separate validated
  optimization; never emulate modern capability claims.
- [ ] Compile all shared DrawPacket, MeshPassProcessor, policy, visibility, and
  submission contracts on both compatibility backends.
- [ ] Add stable fallback reasons for unsupported Tier 1/Tier 2 requests.
- [ ] Run Direct functional fixtures for opaque, masked, transparent, skinned,
  missing material, resize, and resource reload where the backend is enabled.
- [ ] Confirm no compatibility limitation weakens the DX12/Vulkan/Metal RHI
  contracts.
- [ ] Record the selected compatibility strategy in diagnostics.

### Task 16A - Qualification Schema and Evidence Ingestion

- [ ] Version qualification by backend, submission strategy, renderer path,
  shader contract, and qualification revision.
- [ ] Derive qualification level from required/passed gate masks; disallow an
  independently editable Qualified boolean.
- [ ] Define machine-readable gate evidence, ingestion validation, artifact
  hashes, adapter/driver identity, and review provenance.
- [ ] Reconcile Porsche Stage 3C status only after verifying the recorded Task 4
  artifacts satisfy the qualification plan's machine-evidence fields; external
  developer assets still cannot close hermetic `RealAssetRegression`.
- [ ] Add a checked-in attribution-complete hermetic multi-mesh/material asset
  and record opaque, masked, texture, nested-transform, multi-batch, Direct,
  hybrid, and parity evidence.
- [ ] Expose the path/strategy qualification snapshot required by Task 15
  without promoting any backend or changing Auto behavior.

### Task 15 - Measured Auto Policy and Hysteresis

- [ ] Freeze benchmark evidence for candidate/eligible counts, group count and
  occupancy, GPU Scene dirty bytes, residency completeness, CPU planning cost,
  and delayed GPU timings.
- [ ] Define per-pass conservative thresholds by qualified strategy and adapter
  class; do not use backend name alone.
- [ ] Add hysteresis, minimum residency duration, and stable transition rules.
- [ ] Reset history after backend/adapter/driver, strategy revision,
  resolution class, scene class, or shader-contract changes.
- [ ] Keep qualification/capability/pipeline checks ahead of benefit logic.
- [ ] Keep ForceEnabled/ForceDisabled deterministic and independent of benefit
  heuristics.
- [ ] Expose the exact reason Auto chose Direct or GPU-driven for each pass.
- [ ] Add small-scene, large-static, high-churn, threshold-boundary,
  oscillation, reset, and missing-timing fixtures.
- [ ] Prove Auto performs no synchronous GPU timing readback.

### Task 16B - Diagnostics, Samples, and Promotion

- [ ] Remove the `RHICapabilities`, Render, and GPUDriven compatibility
  projections only after all diagnostics and tools consume structured indexed-
  indirect execution capabilities.
- [ ] Run the required adapter/driver matrix and encode scoped deny-list entries
  rather than global bypasses.
- [ ] Finalize public diagnostics for request, selected tier, visibility mode,
  strategy, capability/qualification snapshot, packet partitions/reasons,
  candidate/visible counts, group occupancy, command counts, fallback counts,
  timings, memory, and dirty bytes.
- [ ] Emit schema-versioned JSON evidence plus human-readable logs.
- [ ] Keep ModelViewer limited to request overrides, scene setup, captures, and
  public diagnostic assertions; remove access to renderer internals.
- [ ] Run hermetic and external real-asset suites independently for DX12,
  Vulkan, and Metal.
- [ ] Promote a backend only after all required gates are machine-recorded and
  reviewed; then verify the unchanged Auto command selects the promoted path.
- [ ] Preserve forced Direct/GPU development overrides after promotion.

**M6 exit gate:** Compatibility backends remain functional, path/strategy
qualification exists before Auto benefit selection, Auto is measured and
stable, diagnostics are complete, and every modern backend is promoted only by
its own reviewed evidence.

## 8. Continuous Supporting Workstreams

### A. Qualification asset and production asset honesty

- [ ] Reconcile the GPU-driven qualification plan with Porsche execution only
  after its machine-recorded artifacts and metadata are verified; keep Stage 3D
  hermetic evidence open regardless of external-asset success.
- [ ] Select/create the small redistributable qualification asset with complete
  attribution.
- [ ] Track import/cook limitations separately from renderer correctness; a
  successful parse with zero visible/submitted primitives is a failed smoke.
- [ ] Keep large developer assets optional and outside mandatory source checkout.
- [ ] Record source/cooked content hashes, camera framing, exposure, and output
  color contract for every parity comparison.

### B. Build and test hygiene

- [ ] Repair partial-build CTest registration so unbuilt targets and
  `*_NOT_BUILT` sentinels are not advertised as runnable tests.
- [ ] Add named CTest labels/presets for M1, M2 DX12, Vulkan, Metal,
  compatibility, visual parity, GBV/API validation, and qualification.
- [ ] Keep validation executables deterministic and independent of developer
  asset paths unless explicitly marked external/opt-in.
- [ ] Preserve generated diagnostics, captures, and caches outside commits
  unless they are reviewed hermetic fixtures or goldens.

### C. Platform and CI readiness

- [ ] Maintain Windows DX12/Vulkan validation hosts with Debug Layer/GBV and
  Vulkan validation layers.
- [ ] Establish macOS Metal validation hosts before Task 13 acceptance.
- [ ] Record adapter, driver, OS, backend capability snapshot, validation mode,
  source hash, and artifact hash in CI evidence.
- [ ] Require at least two independent DX12 vendor families before DX12 final
  promotion.

## 9. Per-Slice Review and Validation Checklist

Apply this checklist to every implementation slice:

- [ ] Requirements and out-of-scope boundaries are frozen before edits.
- [ ] A bounded implementation owner is assigned; overlapping file ownership
  is avoided.
- [ ] The primary agent reviews the actual diff rather than accepting a
  subagent conclusion directly.
- [ ] New value contracts have stable names/values, validation, deterministic
  comparison, and diagnostics.
- [ ] CPU contract and negative/failure-injection tests pass.
- [ ] Affected Render/RHI/backend targets compile.
- [ ] Relevant Direct, GPU, hybrid, multi-view, resize, and lifetime tests pass.
- [ ] Visual parity is rerun whenever command consumption, visibility,
  materials, transforms, or output behavior changes.
- [ ] Backend validation layers are clean for backend behavior changes.
- [ ] Performance claims include recorded workloads and artifacts.
- [ ] `git diff --check` passes, including separately checking new untracked
  source files before staging.
- [ ] Generated diagnostics/caches and unrelated user changes remain unstaged.
- [ ] Phase log, plan status, validation commands/results, residual risks, and
  next task are updated before commit.
- [ ] Staged scope is reviewed before creating a focused commit.

## 10. Recommended Commit Sequence

1. Task 5A - policy/plan value contracts and resolver tests.
2. Task 5B - per-view plan compiler integration and diagnostics.
3. Task 6A - Direct Depth packet execution and parity.
4. Task 6B - Direct Opaque packet execution and parity.
5. Task 6C - reviewed legacy consumer removal and M1 exit.
6. Task 7A - stable packet IDs and exactly-once partition accounting.
7. Task 7B - hybrid Depth/Opaque execution and DX12 validation.
8. Task 8 - candidate visibility provider split.
9. Task 9 - frame-owned pass contexts and multi-view proof.
10. Task 10A - semantic indirect RHI contract/conformance.
11. Task 10B - submission strategies and DX12 Tier 1 closure.
12. Task 11 - GPU Scene in independently reviewed schema/update/execution
    slices.
13. Task 12 - Vulkan capability, execution, validation, and Candidate evidence
    slices.
14. Task 13 - Metal RHI ICB, execution, validation, and Candidate evidence
    slices.
15. Task 14 - formal compatibility closure.
16. Task 16A - path/strategy qualification schema, evidence ingestion, and
    hermetic asset evidence without promotion.
17. Task 15 - measured Auto thresholds and hysteresis.
18. Task 16B - diagnostics schema, Sample cleanup, adapter matrix, and
    per-backend promotion.

## 11. Immediate Next Slice

Task 11B implementation, two independent review rounds, remediation, primary
audit, and focused regression gates are complete. Start **Task 11C** with
persistent GPU buffers, dirty-range upload planning, and real completion-aware
retirement. The completed Task 11B acceptance ledger is:

- [x] Replace the Task 11A whole-state copy with a prepared, touched-row
  transaction whose allocations finish before a `noexcept` finalize; injected
  preparation failures preserve every logical row, slot, map entry, and version.
- [x] Keep `Clear` append-only and identity-safe by tombstoning live rows while
  preserving slots/generations and a monotonic committed version.
- [x] Reject duplicate nonzero object IDs before retained RenderScene/cache
  mutation and isolate all shadow-publication failures from authoritative
  Apply, Direct, Tier 1, and draw-packet-cache behavior.
- [x] Diff normalized accepted-scene values by object ID as Add/Update/Remove/
  No-op, ignore row headers/padding/linkage, and preserve same-count refs and
  rendered-frame previous-transform semantics.
- [x] Publish only exact ready mesh/material dependency closures, use canonical
  default material rows for missing or metadata-invalid materials, reject stale
  generations, and never rebind an old accepted handle to a reused slot.
- [x] Freeze pass eligibility as Depth/Opaque/Shadow/Transparent with Masked as
  a material/pipeline variant, explicit row-major affine packing, conservative
  invalid bounds, and fail-closed invalid transforms.
- [x] Separate attempted/candidate diagnostics from committed mirror counts,
  version, and source sequence; partial mirrors remain incomplete until a new
  accepted-scene Publish actually restores excluded objects.
- [x] Keep the result a non-executable CPU shadow. `executionEligible` remains
  false; no RHI buffer, shader, submission, policy, or backend behavior changed.
- [x] Finish independent re-review with READY and no unresolved P0-P2; pass
  GPU Scene 24/24, RenderScene 17/17, cache 6/6, GPU-driven 34/34,
  submission 27/27, focused architecture 12/12, and phase gate 1/1.

Task 11C-1 implementation, independent review, remediation, and primary audit
are complete. The accepted allocator/change-journal ledger is:

- [x] Publish a value-only atomic change set for every successful non-empty
  commit, with base/committed versions and exact sorted/coalesced dirty ranges
  for all six tables; failed and empty commits preserve the previous journal.
- [x] Keep `Clear()` allocation-free and fail closed at version exhaustion while
  publishing full-table dirtiness for a representable clear.
- [x] Record retirement versions without inferring completion from frame counts,
  clocks, or CPU progress; admit reuse only through an explicit safe-version
  watermark whose real completion-token mapping belongs to Task 11C-2.
- [x] Reuse primitive/bounds/transform slots independently, but reclaim and
  reuse draw/material/geometry ranges only as one matching-count contiguous
  block with one strict non-wrapping generation advancement.
- [x] Permanently retire generation-exhausted slots/blocks and keep every old
  typed reference invalid after reuse.
- [x] Preserve prepared-allocation/noexcept-finalize atomicity and cover all 19
  actual allocation-failure checkpoints; pass GPU Scene 30/30 and independent
  review with no unresolved P0-P2.

Task 11C-2 implementation, two independent review/remediation rounds, final
re-review, and primary audit are complete. The accepted persistent-upload
ledger is:

- [x] Allocate six persistent Default-memory structured buffer tables and use
  Upload-memory staging plus declared RenderGraph copy ranges; never map a
  Default buffer.
- [x] Fully initialize every new or capacity-grown allocation, including the
  zeroed capacity tail, and export only valid ShaderResource contents.
- [x] Consume exact version-linked deltas, coalesce multiple observed updates,
  retain changes that arrive while an upload is pending, and force a full
  upload when the base-version chain is missed.
- [x] Keep warm static resident data zero-copy even while its prior use is in
  flight; select or allocate another set before overwriting GPU-visible data.
- [x] Roll back recorded-but-unsubmitted access and dirty snapshots, retain
  upload resources through the submission batch, and advance residency only
  after graph execution plus a structurally valid tracker-issued completion
  token.
- [x] Merge multi-domain upload and future-read completion tokens, fail closed
  on lost completion evidence, and confirm allocator reclamation only after the
  database accepts the computed safe-version watermark.
- [x] Reject CPU table sizes that exceed the uint32 row-index ABI and calculate
  retained persistent-buffer byte counts in uint64.
- [x] Keep the result non-executing and backend-neutral: no shader binding,
  visibility, policy, submission strategy, or fallback behavior changed.
- [x] Finish independent re-review with READY and no unresolved P0-P2; pass
  GPU Scene Upload 12/12, GPU Scene 30/30, RenderGraph 50/50, and Render
  Submission 27/27 in the primary gate.

Start **Task 11D** next. It must make stable GPU Scene indices the authoritative
Tier 2 visibility/command-generation input, compact visible indices and commands
without CPU readback, mark every consumed resident version for completion-aware
lifetime tracking, and route the resulting command streams through the Task 10
submission strategies. Tier 1 remains the per-group fallback when the required
table-indexing capabilities are unavailable.

Task 11D-D1a implementation, independent review/remediation, and primary audit
are complete. The accepted private-consumer foundation is:

- [x] Acquire one exact fully current resident buffer set only when all six
  tables are clean and `resident == covered == desired == observed`; pending,
  stale, incomplete, dirty, or device-lost state returns no lease.
- [x] Return six strong buffer references, RenderGraph handles, capacities, and
  submission-batch retention for the selected set while keeping all RHI objects
  renderer-private.
- [x] Allow one outstanding lease globally, commit its realized read access only
  after graph execution, and require that commit before a completion token can
  resolve the lease. Invalid/unsubmitted work restores prior snapshots and
  fails closed.
- [x] Remove ambiguous version-only read-use marking so completion ownership is
  attached only to the exact leased set.
- [x] Resolve an accepted object/batch packet to one live, generation-checked
  primitive/draw pair with exact contiguous-range, back-reference, geometry,
  material, pass-mask, and committed-version validation.
- [x] Cover current/pending/mismatched versions, valid/invalid/omitted-commit
  submission, rollback, multi-domain reads, tombstones, clear, reclaim, and
  generation reuse; pass Upload 16/16, GPU Scene 31/31, RenderGraph 50/50, and
  Submission 27/27 in the primary gate.
- [x] Keep `GPUResidentScene` unselected and `executionEligible == false`; this
  foundation changes no culling shader, raster behavior, Task 10 strategy,
  shared RHI contract, backend implementation, or Auto policy.

Task 11D-D1b-a implementation, two remediation rounds, independent re-review,
and primary audit are complete. The accepted non-executing foundation is:

- [x] Freeze a 40-byte, ten-`uint32` stable-ref candidate ABI with exact C++
  offsets, committed-version locking, raster ordinal/group linkage, and one
  exact pass bit.
- [x] Freeze one 240-byte constants ABI shared by normal and GPU Scene compute
  shaders. Counts and all six exact lease capacities use integer fields; the
  normal path deterministically zeros the GPU Scene capacity blocks.
- [x] Mirror all six GPU Scene row layouts in one shared HLSL include and reject
  invalid schema versions, tombstones, generations, object identities,
  contiguous draw ranges, back-references, pass masks, and indexed-draw
  semantics before emitting a command.
- [x] Seal a GPU Scene recording only against the same committed lease version,
  exact nonzero capacities, matching structured-buffer strides, and sufficient
  backing byte sizes. Upload candidate/constants inputs and retain all table,
  descriptor, shader, layout, and pipeline references in the submission batch.
- [x] Keep normal Tier 1 execution, SceneRenderer scheduling, Task 10 strategy,
  public RHI, policy selection, diagnostics, and Auto behavior unchanged.
  `GPUResidentScene` remains unselected.
- [x] Finish independent re-review with READY and no unresolved P0-P2 after
  correcting the initially omitted table-capacity constants and the actual
  GPU Scene shader's float/integer count mismatch.
- [x] Pass GPU-driven 38/38, GPU Scene upload 16/16, GPU Scene 31/31,
  RenderGraph 50/50, Render Submission 27/27, and the architecture phase gate
  in the primary audit.

Start **Task 11D-D1b-b** next: build candidates from exact accepted lookups,
acquire one current lease per recorded view, declare all candidate/table/output
RenderGraph accesses, dispatch GPU Scene frustum/compaction for Depth and Opaque,
commit or roll back the lease with graph/submission outcomes, and feed the
existing per-group Task 10 indirect-count submission. Any missing or stale
lookup/lease/readiness must preserve the existing `IndirectGrouped` inputs.
Public `GPUResidentScene` selection remains forbidden until D3/D4.
