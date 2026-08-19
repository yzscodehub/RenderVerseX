# Render Policy Resolver Contract

**Status:** Task 5A architecture frozen
**Date:** 2026-08-03
**Parent plan:**
`Docs/superpowers/plans/2026-08-02-render-policy-draw-packet-implementation-plan.md`

## 1. Purpose

Task 5 makes Render, rather than individual passes, the owner of predictable
render-path decisions. Task 5A freezes a pure value boundary and tests it in
isolation. Task 5B will sample live engine state, compile frame-local packet
references, and publish one immutable plan per view.

The resolver consumes a snapshot of facts. It does not discover facts by
calling an RHI device, qualification manifest, resource registry, pipeline
cache, RenderGraph, or pass object.

## 2. Current Decision Ownership Audit

| Decision or fact | Current producer/owner | Task 5 target owner |
|---|---|---|
| External Auto/Force request validity | RenderContracts frame validation | RenderContracts validates; resolver revalidates the value |
| Backend and device capability probes | GPUDrivenPolicy and GPUCulling | RHI/renderer adapter samples semantic capability facts once |
| Qualification lookup and maturity | GPUDrivenQualification manifest | Qualification owner publishes an explicit snapshot; resolver consumes it |
| GPU visibility shader/pipeline readiness | GPUCulling | GPUCulling publishes normalized readiness facts |
| Shared culling buffers and bindings | GPUCulling and SceneRenderer | Resource owners publish normalized readiness facts |
| Packet relevance and intrinsic eligibility | MeshPassProcessors | MeshPassProcessors remain the classifier |
| Pass request/support/permission | SceneRenderer and pass objects | Plan compiler samples permission facts before graph construction |
| Depth/Opaque GPU group completeness | DepthPrepass and OpaquePass | Plan compiler partitions packet references |
| Pass GPU pipeline/material readiness | DepthPrepass and OpaquePass | Pipeline/material owners publish facts before plan compilation |
| GPU/Direct/Skip policy | GPUDrivenPolicy, SceneRenderer, and passes | RenderPolicyResolver is the sole predictable-policy owner |
| Unexpected recording failure | GPUCulling and passes | Execution report; never fed back into the same plan |

The audit found repeated capability checks, a hidden global qualification
lookup, a single `pipelineReady` boolean that collapses distinct failures,
pass-specific reclassification of prepared packets, and a global mutable
enable flag shared by all views and passes. Task 5 removes those duplicated
policy decisions in stages; Task 5A introduces no runtime behavior change.

## 3. Frozen Value Boundary

### 3.1 Resolver input

The input is an owned, copyable value snapshot containing:

- the frame policy request and a frame-local view ordinal;
- renderer, view, and implementation constraints;
- semantic compute, binding, and indirect-submission capabilities;
- an explicit versioned backend qualification manifest;
- normalized shader, pipeline, shared-resource, and binding readiness;
- per-pass request, support, Direct/GPU permission, readiness, workload, group,
  and packet-partition counts.

Readiness is tri-state: `Unavailable`, `Pending`, or `Ready`. Pending shaders,
pipelines, resources, and bindings have distinct stable reasons and are not
silently treated as either ready or permanently unavailable.

The input must not own or reference:

- an `IRHIDevice`, RHI resource, pipeline, descriptor, command context, or
  RenderGraph handle;
- a scene, registry, GPUCulling instance, material system, pass object,
  callback, pointer, span, or borrowed container;
- a view matrix, HiZ texture, or other execution payload;
- timing history or hysteresis, which belong to Task 15;
- a persistent packet identity, which belongs to Task 7.

### 3.2 Resolver output

The resolver returns a value-only per-view policy resolution:

- frame sequence and view ordinal;
- requested mode, selected tier, and one stable reason;
- a canonical pass-order list of visibility/submission decisions;
- per-pass input, relevant, GPU, Direct, and Skip counts;
- copied semantic capability and qualification projections.

The resolver result is deliberately not a packet execution plan. It contains
no fabricated packet ranges. Task 5B combines the result with prepared packet
streams and produces the final execution plan.

### 3.3 Frame execution plan

`RenderFrameExecutionPlan` owns a vector of frame-local packet references. A
reference retains its compatibility pass/source fields and also owns Task 7A's
structured packet ID. The ID covers frame, view, pass, object/primitive, exact
mesh generation, logical and geometry submesh, source index, and source
ordinal; equality and ordering use the complete value rather than a lone hash.

Each reference also freezes an exact prepared-source signature containing the
full packet, draw-group key, Direct layout, disposition/reason, source ordinal,
and view depth. This signature is separate from logical identity: it detects
preparation drift before Direct or GPU command recording without making
material or pipeline state part of the persistent packet ID.

GPU, Direct, and Skip ranges index the owned vector; the lane is defined by the
field containing the range. Every pass publishes expected, terminal, unique,
duplicate, and unaccounted identity counts. Validation independently recomputes
those values and accepts only a canonical, zero-duplicate, zero-unaccounted
exactly-once partition. Task 7B may change mixed-pass lane selection, but not
this identity or accounting contract. Task 7B now preserves those compiler
partitions through recording: GPU, Direct, and deliberate-Skip ranges may
coexist, while every relevant source remains in exactly one terminal lane.

Depth and Opaque preflight the complete plan and prepared stream, then record
the planned GPU lane followed by the planned Direct lane inside one render
pass. Attachments are cleared and the pass is begun/ended exactly once. A plan
may also contain GPU work with Direct sources deliberately skipped when Direct
readiness is pending; that shape records only the GPU lane and retains the
published skip reason. A late GPU recording failure marks that lane failed,
continues only with the already-planned and preflighted Direct lane, reports the
recorded prefix honestly, and never replays GPU packets as Direct in the same
frame.

Every plan and resolver value provides full value comparison. Validation is a
non-throwing, fail-closed operation over external or compiled data.

## 4. Resolution Order

The stable order is:

1. validate request values and fact consistency;
2. apply renderer, view, and implementation constraints;
3. require semantic compute, binding, and indirect-submission capabilities;
4. validate the explicit qualification manifest and backend association;
5. require qualification for `Auto`; `ForceEnabled` bypasses only maturity;
6. require visibility shader and pipeline readiness;
7. require shared resources and bindings;
8. apply pass request, support, and Direct/GPU permission;
9. apply pass shader, pipeline, and resource readiness;
10. resolve relevant, GPU-candidate, Direct, and Skip workload counts;
11. apply the deterministic Task 5 `Auto` benefit fact;
12. emit canonical pass decisions and one per-view result.

`ForceDisabled` resolves Direct without consulting later GPU enable gates, but
still enforces pass request/support and Direct-path readiness.
`ForceEnabled` never bypasses invalid input, renderer/view constraints,
capabilities, malformed qualification data, shader/pipeline requirements, or
resource readiness.

## 5. Stable Fallback Rules

- Invalid or inconsistent facts fail closed and carry an explicit reason.
- An unresolved backend is never promoted by an indirect capability bit alone.
- If a GPU candidate cannot use the selected GPU path but its Direct path is
  ready, it is planned for Direct.
- If neither legal path is ready, it is planned for Skip with a stable reason.
- A pass may contain both GPU and Direct counts. This is a planned hybrid
  partition, not permission for a recording-time silent fallback.
- Shadow and Transparent remain Direct because their Task 5B pass facts deny
  GPU permission; the pure resolver does not hard-code pass names.
- Fixed-count indirect and encoded-command-buffer strategies remain explicit
  semantic submission modes. They are not collapsed into a DX12-specific
  indirect-count boolean.

## 6. Validation Gate

Task 5A closes only when:

- old GPUDrivenPolicy behavior remains equivalent through its compatibility
  entry point;
- exhaustive CPU tests cover request modes, qualification states, capability,
  readiness, permission, workload, and submission-strategy combinations;
- permuting semantically identical pass facts produces an identical complete
  resolution;
- invalid pass kinds, duplicates, count inconsistencies, overflow, malformed
  qualification, and invalid ranges fail closed;
- no SceneRenderer, pass command recording, qualification bit, Sample, RHI
  backend, visual output, or Auto runtime default changes.

Task 5B may begin only after an independent code review and primary review of
the Task 5A diff and validation evidence.
