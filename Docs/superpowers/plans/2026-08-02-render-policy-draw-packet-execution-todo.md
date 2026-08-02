# Render Policy and Draw Packet Remaining Execution TODO

**Status:** Tasks 0-4 and Task 5A/5B complete; Task 5C frame-plan compilation
is the next implementation stage
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

- [ ] Add `RenderFramePlanCompiler` and compile after pass-packet preparation
  but before RenderGraph construction.
- [ ] Resolve current Depth/Opaque packet eligibility into planned lanes without
  changing command recording yet.
- [ ] Carry Transparent as ordered Direct and Shadow as Direct-only for this
  stage.
- [ ] Move predictable backend/qualification/pipeline/residency decisions out
  of pass execution; retain only validation of unexpected runtime failures.
- [ ] Store frame plans in frame/view-owned lifetime and reset them on every
  accepted frame, rejected frame, resize, and view replacement.
- [ ] Export selected policy plus per-pass relevant/GPU/Direct/Skip counts and
  reason counts through public render diagnostics.

#### 5D. Validate and commit Task 5

- [x] Add resolver matrix tests for Auto, ForceEnabled, ForceDisabled, invalid
  mode, missing capability, unqualified backend, pipeline failure, pending
  resource, and pass-disabled cases.
- [x] Add determinism tests that permute identical input construction order and
  compare the complete plan.
- [ ] Add two-view tests proving independent plans before pass-context migration.
- [ ] Add source/contract checks proving pass execution no longer probes backend
  type or qualification state.
- [x] Run RenderPolicy, MeshPassProcessor, RenderScene, GPUDriven, RenderPass,
  RenderContracts, extraction, and Direct/GPU parity gates.
- [x] Record a phase-log entry and primary review.
- [x] Commit slice 5A: `feat(render): add per-view render policy plans`.
- [ ] Commit slice 5B if integration is too large for one review:
  `refactor(render): compile frame policy before graph construction`.

**Stop gate:** Task 6 cannot start until identical inputs produce identical
plans, invalid inputs fail closed, and pass execution has no backend or
qualification policy branch.

### Task 6 - Direct Depth/Opaque DrawPacket Execution

#### 6A. Direct Depth migration

- [ ] Add a packet-range Direct recorder that consumes the Task 5 Depth plan.
- [ ] Bind packet geometry, primitive-data index, object/skinning data, and
  masked/default-material requirements without rebuilding pass classification.
- [ ] Reject malformed ranges and unavailable required bindings before command
  recording.
- [ ] Run legacy and packet Depth construction in temporary dual-build mode and
  compare packet/draw counts without recording twice.
- [ ] Add static, masked, skinned, missing-material, invalid-submesh, and empty
  Depth fixtures.
- [ ] Capture Direct reference parity before removing the legacy Depth loop.

#### 6B. Direct Opaque migration

- [ ] Add packet-range Direct recording for opaque and masked lanes.
- [ ] Preserve opaque-before-masked ordering, material fallback behavior,
  tangent-basis normal-map gating, shadow receiver state, and object motion
  constants.
- [ ] Keep Transparent in its existing sorted Direct pass.
- [ ] Run legacy and packet Opaque construction in dual-build mode and compare
  source ordinals, exact packet values, draw arguments, material handles, and
  counts.
- [ ] Add multi-submesh/multi-material and skinned Direct fixtures.
- [ ] Prove the packet path produces the existing Direct golden and Porsche
  output at the approved tolerance.

#### 6C. Remove temporary legacy consumers

- [ ] Review dual-build evidence and remove only the replaced Depth/Opaque
  command consumers.
- [ ] Retain adapters still required by Transparent, Shadow, ObjectVelocity, or
  diagnostics and mark their removal owner explicitly.
- [ ] Confirm no pass reclassifies material/pass eligibility while recording.
- [ ] Run M1 exit suite across DX12 Direct, Vulkan/Metal Direct on the configured
  platform matrix, plus DX11 Direct and OpenGL compile/smoke coverage. Record an
  unavailable platform as an open milestone coverage gate rather than silently
  skipping it.
- [ ] Record M1 exit evidence and commit Depth and Opaque migrations as separate
  reversible commits.

**M1 exit gate:** Depth and Opaque render through DrawPackets on the Direct path
with exact/documented parity and no legacy duplicate submission.

## 4. M2 - Productionized DX12 Tier 1

### Task 7 - Hybrid DX12 Packet Partition

- [ ] Define a stable per-frame packet ID covering view, pass, object/primitive,
  exact mesh generation/submesh, and source identity.
- [ ] Validate uniqueness and deterministic regeneration for identical frames.
- [ ] Compile mutually exclusive GPU, Direct, and deliberate-Skip partitions.
- [ ] Add exactly-once accounting: every relevant packet ID appears in exactly
  one terminal lane.
- [ ] Replace all-pass `AreGPUDriven*GroupsDrawable()` gates with validated plan
  partitions.
- [ ] Record GPU lanes indirectly and Direct fallback lanes in the same
  Depth/Opaque pass with compatible load/depth semantics.
- [ ] Keep Transparent Direct, Skinned Direct, and unavailable/pending behavior
  exactly as specified by the plan reason.
- [ ] Define late recording failure behavior: fail/report the current lane and
  replan a later frame; never replay already-recorded packets in the same frame.
- [ ] Add mixed static/skinned/special/missing/pending/transparent fixtures.
- [ ] Assert zero duplicate IDs, zero unaccounted eligible IDs, and one failing
  group not forcing unrelated groups to Direct.
- [ ] Run DX12 Debug Layer, GBV, repeated-frame, resize, resource-lifetime, and
  Direct/hybrid parity gates.
- [ ] Commit packet identity/accounting separately from Depth/Opaque hybrid
  execution when useful for review.

**Stop gate:** No visibility redesign begins until hybrid exactly-once
accounting is green under failure injection.

### Task 8 - Candidate Visibility Separation

- [ ] Define `RenderCandidateSet` as coarse scene candidates, distinct from
  final visible packet streams.
- [ ] Define `IRenderVisibilityProvider` with CPU and GPU implementations.
- [ ] Establish one canonical bounds, transform, frustum plane, handedness,
  clip-depth, reverse-Z, and invalid-bounds contract shared by CPU reference and
  GPU shader code.
- [ ] Feed Direct plans through CPU fine visibility.
- [ ] Feed GPU plans coarse candidates and perform final frustum/distance
  visibility on the GPU without CPU pre-elimination.
- [ ] Preserve stable packet/source mapping through compaction.
- [ ] Remove duplicate diagnostic culls and repeated production
  `GPUCulling::BeginFrame` rebuilds.
- [ ] Keep HZB occlusion disabled and report that it is unavailable by design.
- [ ] Add boundary-touching, non-finite, behind-camera, near/far plane,
  zero-object, and CPU/GPU parity fixtures.
- [ ] Assert GPU candidate input may exceed final visible count and requires no
  pre-submission readback.
- [ ] Benchmark candidate preparation to confirm no quadratic regression.

### Task 9 - Frame-Owned Pass Record Contexts

- [ ] Define immutable `RenderPassRecordContext` and frame-owned
  `RenderPassExecutionData`.
- [ ] Evolve `IRenderPass::AddToGraph` to receive a per-view plan and captured
  pass data.
- [ ] Move graph handles, planned packet ranges, and transient RHI references
  out of persistent pass members.
- [ ] Remove Depth/Opaque GPU-resource setter injection.
- [ ] Make execute lambdas consume only captured execution data and record
  commands without policy/backend branching.
- [ ] Declare compute-write to indirect-read and instance-index dependencies in
  RenderGraph.
- [ ] Keep execution on the graphics physical queue until async compute has its
  own synchronization and performance proof.
- [ ] Add two-view, sequential-view, repeated-frame, resize, rejected-frame,
  and stale-handle fixtures.
- [ ] Add graph-negative tests for missing declarations and lifetime violations.

### Task 10 - Submission Strategy and DX12 Tier 1 Closure

#### 10A. Semantic RHI contract

- [ ] Replace backend-name inference with structured indirect execution
  capabilities: fixed-count, count-buffer, first-instance, command stride,
  alignment, limits, and required states.
- [ ] Add validated RHI descriptors for indexed indirect execution and count
  buffers.
- [ ] Freeze renderer-facing `RenderSubmissionMode` semantics for Direct,
  fixed/count-buffer indirect, and the existing `EncodedCommandBuffer`
  extension point without pretending the Metal-specific RHI object exists yet.
- [ ] Keep the old boolean capability only as a temporary projection with a
  named removal task.
- [ ] Add zero/one/max/overflow/count-clamp/alignment/state conformance tests.

#### 10B. Strategy interface and DX12 implementation

- [ ] Define `IRenderSubmissionStrategy` with Direct and IndirectCount
  implementations plus a typed backend-native encoded-command-buffer extension
  boundary for Task 13.
- [ ] Make renderer grouping/visibility independent of backend command APIs.
- [ ] Cache DX12 command signatures by semantic command layout.
- [ ] Validate command/count buffer ranges before `ExecuteIndirect`.
- [ ] Rebind state invalidated by indirect execution according to the RHI
  contract.
- [ ] Route both DX12 Direct and Tier 1 through the same strategy interface.
- [ ] Remove obsolete pass-local DX12 submission branches and capability probes.

#### 10C. M2 validation

- [ ] Run all RHI conformance and cross-backend shared-contract tests.
- [ ] Run DX12 Debug Layer, GBV, Direct/hybrid parity, resize, zero-visible,
  maximum-count, and in-flight retirement fixtures.
- [ ] Compile shared contracts and run Direct smoke on every configured primary
  backend; record unavailable Metal/Vulkan hosts as open M2 coverage gates.
- [ ] Emit one machine-recorded M2 exit artifact aggregating Task 7 exactly-once
  failure injection, Task 8 CPU/GPU bounds parity, Task 9 multi-view/lifetime
  negatives, and Task 10 RHI conformance.
- [ ] Verify zero-count groups emit no draw and buffer overrun is impossible.
- [ ] Record CPU plan/submission cost and group occupancy without making Auto
  decisions yet.
- [ ] Freeze the renderer-facing strategy and semantic RHI baseline before
  Tasks 11-13 branch. Task 13 may add Metal-private ICB objects behind the
  encoded-command-buffer extension; any shared contract change requires a
  primary-owned revision and a new cross-backend freeze.

**M2 exit gate:** DX12 Tier 1 supports per-group hybrid fallback through one
formal submission interface with clean validation and parity.

## 5. M3 - Persistent GPU Scene and DX12 Tier 2

### Task 11 - GPU Scene

- [ ] Freeze GPU Scene table schemas for primitive, bounds, transform,
  material, geometry, draw metadata, and generation/version data.
- [ ] Define stable generation-checked indices and completion-aware free-list
  reuse.
- [ ] Define publication rules: entries become visible only after dependent
  mesh/material generations are resident.
- [ ] Implement persistent buffers and dirty-range upload planning.
- [ ] Double-buffer or version CPU-updated data that may overlap in-flight GPU
  reads.
- [ ] Handle add/remove/transform/material/mesh/reload/evict operations without
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
- [ ] Prove unchanged static scenes perform no full-scene upload after warm-up.
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

Start with **Task 5A only**:

- [x] Audit current decision probes and write the ownership matrix.
- [x] Freeze resolver input facts and immutable per-view plan outputs.
- [x] Define stable reasons and plan validation.
- [x] Implement the pure resolver behind existing behavior.
- [x] Add exhaustive CPU-only resolver matrix and determinism tests.
- [x] Do not change pass command recording, visual output, qualification bits,
  or Auto defaults.
- [x] Stop for primary architecture/code review before SceneRenderer
  integration in Task 5B.
