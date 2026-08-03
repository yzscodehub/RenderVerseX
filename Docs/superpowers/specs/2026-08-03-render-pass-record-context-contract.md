# Render Pass Record Context and Execution Data Contract

**Status:** Task 9A implemented and reviewed; Task 9B pending
**Date:** 2026-08-03
**Scope:** Main raster pass chain; frame/view data ownership and RenderGraph
recording lifetime

## 1. Problem statement

The legacy pass interface stores frame inputs in long-lived pass objects. The
current sequence is effectively:

```text
pass->AddToGraph(graph, m_viewData) captures pass/view addresses
  -> pass Setup writes graph handles into pass members
  -> SceneRenderer UpdatePassResources mutates scene/list/target members
  -> graph Compile/Execute reads whichever member values are present then
```

This works only because one renderer currently builds and executes one graph
immediately. It is unsafe for two views, delayed execution, rejected frames, or
a second graph that reuses the same pass instance. RenderGraph handles contain
an index but no cross-graph generation, so a stale handle may alias a valid but
unrelated resource after `RenderGraph::Clear()`.

Task 9 replaces the implicit member mailbox with explicit graph-owned data.

## 2. Ownership model

Long-lived pass objects may own only:

- immutable renderer service dependencies such as PipelineCache,
  MaterialSystem, ResourceViewCache, and RenderResourceRegistry;
- user/runtime configuration whose lifetime intentionally spans frames;
- stable backend pipelines or device resources;
- last-execution diagnostic output, provided it is never read as a command
  input.

Frame/view recording data includes:

- a value snapshot of ViewData;
- frame/view sequence and the matching immutable execution plan;
- retained-scene/draw-source references valid for the current recording;
- pass-local planned packet ranges and visibility inputs;
- a sealed, independent GPU-culling state for the graph recording;
- RenderGraph texture/buffer handles;
- transient RHI references and descriptor/material binding results.

Frame/view recording data must live in `RenderPassRecordContext` and typed
`RenderPassExecutionData`. After `AddToGraph`, every value needed by setup or
execution is copied into the RenderGraph pass data owned by that graph.

## 3. Required API behavior

`IRenderPass::AddToGraph` receives a `RenderPassRecordContext`. The context is a
temporary immutable value used to construct graph pass data; callbacks must not
retain its address. The default adapter copies ViewData rather than storing
`&view`.

Each migrated pass uses typed pass data. Setup writes realized builder handles
only into that pass data. Execute consumes only:

- the typed pass data;
- the command context supplied by RenderGraph;
- an explicitly captured diagnostic output sink.

Execute must not read frame inputs or graph handles from `this`. Capturing a
pass pointer solely as a diagnostic sink is allowed only when command decisions
cannot depend on the current contents of the pass object.

Every context validates:

- nonzero and matching frame/view sequence;
- plan, packet preparation, and visibility identities;
- graph identity for every imported or created handle;
- complete required handle declarations for the selected lane;
- no handle or pass data from a rejected/previous frame.

Invalid context or missing dependency fails closed before command recording and
publishes an explicit execution-report reason.

## 4. Graph dependency rules

Depth and Opaque GPU lanes receive independent typed input slices. Their
compute-written resources are declared as reads in the consuming graphics pass:

- instance data: shader-resource read;
- instance-index stream: vertex-buffer read;
- indirect command buffer: indirect-argument read;
- draw-count buffer: indirect-argument read.

The declarations are part of the pass data that records indirect commands.
The production SceneRenderer path does not use
`SetGPUDrivenRenderGraphResources` or a per-frame GPU-enabled setter;
standalone compatibility entry points remain only through Task 9B.
RenderPolicy remains the sole source of the selected lane.

Main-chain execution remains on the graphics physical queue. Task 9 does not
enable async compute.

## 5. Migration slices

### 9A - Core contract and Depth/Opaque

**Implementation status:** Complete and independently reviewed.

- Add record-context/execution-data types and a default value-capture adapter.
- Build one immutable context after graph targets and GPU-culling graph inputs
  exist, before registering main-chain passes.
- Migrate Depth and Opaque setup/execute to typed graph pass data.
- Remove Depth/Opaque GPU resource and per-frame enable setter injection.
- Keep existing policy, hybrid ordering, material semantics, and honest report
  counts unchanged.
- Add two-view, sequential graph, repeated-frame, stale-handle, missing
  declaration, and frame-sequence mismatch fixtures.

### 9B - Remaining scene pass adapters

**Implementation status:** Pending.

- Migrate Transparent, Shadow, ObjectVelocity, and Skybox scene/list/target
  inputs to typed graph pass data.
- Resolve targets from declared graph handles instead of post-build raw view
  injection.
- Remove `RenderFrameResourceBinder` and the late `UpdatePassResources` phase.
- Preserve long-lived pass configuration and feature enablement.
- Add resize, rejected-frame, empty-list, multi-view, and target-replacement
  fixtures for the migrated passes.

Compatibility Setup/Execute entry points may exist only as explicit standalone
test adapters during 9A. They must construct the same typed execution data and
must not remain in SceneRenderer. Task 9B removes adapters whose only purpose
was the legacy binder.

## 6. Acceptance gates

- Mutating the caller's ViewData after `AddToGraph` cannot change execution.
- Building two graphs/views from one pass instance preserves each graph's own
  plan, targets, handles, and draw sources.
- Clearing/rebuilding a graph cannot make an old handle select a new resource.
- Repeated frames and frame-slot wrap do not leak previous packet ranges.
- Resize and rejected-frame paths invalidate contexts before graph execution.
- A GPU consumer without every compute-write/indirect-read declaration fails
  graph validation and records no draw.
- Depth/Opaque Direct, hybrid, zero-visible, multi-group, late-failure, DX12
  Debug Layer/GBV, Direct/GPU parity, and Porsche gates remain green.
- DX11 Direct remains green. Vulkan/Metal/OpenGL platform gaps remain explicit
  milestone gates and are not reported as Task 9 success.

Task 10 may replace the captured GPU owner/submission details with formal
submission strategies, but it must preserve this frame-owned lifetime model.
