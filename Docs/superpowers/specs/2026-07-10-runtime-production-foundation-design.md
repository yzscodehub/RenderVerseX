# RenderVerseX Runtime Production Foundation Design

Date: 2026-07-10

Branch baseline: `codex/architecture-implementation` at `0c2caefc`

Status: Design approved; written specification awaiting final user review

## 1. Decision

RenderVerseX will move toward a production-grade runtime engine through a core-first vertical program. The primary supported graphics backends are DX12, Vulkan, and Metal. DX11 and OpenGL remain secondary compatibility backends. The Editor is deferred and is not a correctness dependency for any milestone in this program.

The program prioritizes a small, shippable runtime path over broad feature count:

1. Make the build and validation results truthful.
2. Establish strict update-thread, render-thread, resource, and RHI ownership.
3. Prove the required RHI contract on real DX12, Vulkan, and Metal devices.
4. Make cooked resources, asynchronous loading, upload, residency, and eviction production-capable.
5. Ship one canonical cross-backend renderer through RenderGraph.
6. Harden recovery, shutdown, memory pressure, and first-run behavior.
7. Promote the runtime to a release candidate with reproducible artifacts and published support evidence.

RHI consistency is part of every milestone that introduces GPU behavior. It is not a separate, open-ended parity project.

## 2. Current Branch Evidence

The architecture branch already provides a useful foundation:

- `RenderContracts`, `RenderExtraction`, and `ResourceSceneAdapters` make important dependency seams explicit.
- Module-boundary, capability, runtime resource policy, render honesty, and architecture baseline gates exist.
- RenderGraph, multiple RHI backends, render passes, pipeline caching, runtime resource policies, cooked/package concepts, and diagnostic schemas already exist.
- The architecture baseline currently selects 107 tests: 106 pass, one is skipped because the environment cannot create the required symlink, and none fail.
- The complete CTest inventory is not yet clean: four tests are unavailable because three targets are not built and one fixture writer executable is missing.
- The branch diff currently contains whitespace errors, so repository hygiene does not yet meet a release gate.

The most important remaining structural gaps are:

- `Render` publicly links `RenderExtraction`, while `RenderExtraction` publicly links `Resource`, `Scene`, and `World`. This preserves an indirect dependency from GPU execution toward gameplay and resource implementation.
- `RenderProxySnapshot` contains raw `IRenderMeshUploadSource*` and `IRenderMaterialSource*` pointers. Those pointers do not provide safe cross-thread lifetime or generation semantics.
- Capability reports describe many behaviors, but Tier 1 production support is not yet proven by equivalent real-device behavior tests on Windows, Linux, and macOS.
- Resource policy and diagnostics exist, but the complete asynchronous cooked-package-to-GPU lifecycle is not yet the only shipping path.
- The default renderer and feature implementations are broad, while several advanced paths are explicitly unsupported or incomplete. Breadth must not be mistaken for production readiness.
- Large coordinator files, especially `SceneRenderer`, combine orchestration, policy, diagnostics, and pass-specific behavior. They should be decomposed only where the milestones create a clear ownership boundary.

This design builds on the implemented framework. It does not replace the existing engine with a new architecture.

## 3. Goals and Non-Goals

### 3.1 Goals

- Start, run, render, recover where possible, and shut down without the Editor.
- Load only cooked artifacts or runtime packages in shipping modes.
- Keep World, Scene, and Resource implementation details off the render thread.
- Transfer frame data through immutable values and stable generational IDs.
- Make Render the sole owner of production GPU execution and RHI object lifetime.
- Use RenderGraph for all GPU work in the canonical runtime renderer.
- Establish equivalent required behavior across DX12, Vulkan, and Metal.
- Make every supported, degraded, skipped, and unsupported outcome observable through structured status and concise diagnostics.
- Build reproducible validation and release evidence for each Tier 1 backend.

### 3.2 Non-Goals

- Editor implementation or Editor-first workflows.
- Full DX11 or OpenGL parity with Tier 1 backends.
- Advanced effects such as production SSR, depth of field, motion blur, volumetrics, or a complete decal pipeline.
- Ray tracing, full bindless rendering, mesh shading, or variable-rate shading as baseline requirements.
- Replacing the current World, Scene, Resource, RenderGraph, or RHI modules wholesale.
- Copying Unreal Engine's UObject/editor ecosystem or Unity's complete asset authoring model.
- Adding diagnostics that have no runtime decision, automated gate, or support consumer.

## 4. Commercial-Engine Reference Principles

The architecture borrows proven principles, not implementation scale.

From Unreal Engine:

- Treat update-thread objects and render-thread state as different ownership domains.
- Publish render proxies or snapshots instead of allowing the render thread to dereference gameplay objects.
- Keep a render-scene representation owned by the renderer.
- Build GPU work as an explicit dependency graph.
- Separate logical asset identity from asynchronous load state and physical residency.
- Precache or asynchronously create PSOs, and make cache misses an observable runtime condition.

From Unity's Scriptable Render Pipeline and runtime asset guidance:

- Declare pass reads and writes so resource lifetime and synchronization can be derived and validated.
- Separate runtime asset consumption from source authoring and import workflows.
- Prefer a small, explicit render pipeline whose behavior can be tested across platforms.

RenderVerseX deliberately remains lighter: ordinary C++ types, explicit subsystem composition, narrow interfaces, and versioned data contracts are preferred over reflection-heavy object graphs.

## 5. Target Module and Ownership Boundaries

### 5.1 Composition

`RuntimeHost` is a composition role, not necessarily a new library. A sample, game executable, or future server/client host constructs `Engine`, selects an application mode and backend, installs platform services, and drives the main loop.

`Engine` owns subsystem composition and the update-to-render handoff. It initializes World, Resource, RenderExtraction, and Render in dependency order. It also coordinates orderly shutdown in the reverse direction.

### 5.2 Update and extraction domain

World, Scene, feature modules, and Resource own mutable CPU-side state. `RenderExtraction` reads that state at a defined frame boundary and produces a complete immutable `RenderFramePacket`.

`Engine`, not `Render`, invokes extraction. Therefore:

- `RenderExtraction` may depend on `RenderContracts`, World, Scene, and Resource.
- `Engine` may depend on both `RenderExtraction` and Render.
- Render must not link Scene, World, Resource, ResourceSceneAdapters, or RenderExtraction.
- Render consumes only `RenderContracts`, Runtime/Core services, ShaderCompiler, Spatial where needed, and RHI.

### 5.3 Render domain

The render thread exclusively owns:

- the accepted render-frame queue state;
- `RenderScene` and its proxy generations;
- `RenderResourceRegistry` and all strong RHI resource references;
- pass registration and the canonical pass pipeline;
- RenderGraph compilation, validation, execution, and retirement;
- command submission, presentation, and deferred GPU destruction.

Parallel command recording may use Render-owned worker jobs, but those jobs consume immutable render data and never access World, Scene, or Resource objects.

### 5.4 Resource-to-render gateway

Resource does not create or retain RHI objects. A narrow gateway contract, defined in `RenderContracts` and implemented by Render, provides:

- reservation of a `RenderResourceId` and generation;
- non-blocking enqueue of an immutable `ResourceUploadRequest`;
- release or eviction requests by stable ID;
- read-only status lookup suitable for Resource state transitions and diagnostics.

Engine injects this gateway into Resource. This keeps Resource independent of Render implementation while allowing the render thread to own GPU realization.

## 6. Core Data Contracts

### 6.1 Stable identities

The production handoff uses two identity classes:

- `AssetId`: stable logical identity for cooked/package content. It survives device recreation and is used to locate a rehydration recipe.
- `RenderResourceId`: runtime slot identity paired with a generation. It prevents stale frame packets from binding a reused registry slot.

A render resource reference is valid only when both slot and generation match the registry entry. Zero or the existing engine invalid sentinel represents no resource. Backend handles, C++ object addresses, and ownership-bearing smart pointers are forbidden in cross-thread contracts.

### 6.2 RenderFramePacket

The first production packet is a complete immutable frame snapshot, not a delta stream. A complete snapshot permits safe latest-frame queue replacement and makes recovery from a skipped update deterministic.

The packet contains:

- schema version, frame sequence, world revision, and extraction completion status;
- view and camera values;
- primitive values: transform, normal transform, bounds, visibility, layer, shadow flags, sort key, and stable mesh/material references;
- light and environment values;
- skinning palette values or an index/offset into immutable storage owned by the same packet;
- feature packets registered through versioned RenderContracts extensions;
- diagnostic counters required to validate packet completeness.

The packet never contains:

- a `World*`, `Scene*`, component pointer, or Resource object pointer;
- `IRHIDevice*`, RHI resource references, descriptors, command contexts, or backend handles;
- callbacks into the update domain;
- mutable containers shared with extraction producers.

`RenderProxySnapshot` is migrated to these rules. Raw mesh and material source pointers are replaced by generational render-resource references. Delta commands may be introduced later only after equivalence, ordering, loss recovery, and queue-drop tests prove that they preserve the complete-snapshot contract.

### 6.3 ResourceUploadRequest

An upload request contains:

- request sequence and cancellation token;
- `AssetId`, reserved `RenderResourceId`, and generation;
- resource kind and immutable creation description;
- immutable CPU payload blocks or ownership-transferred staging data;
- dependency IDs and required readiness policy;
- byte cost, priority, and diagnostic provenance;
- optional rehydration recipe key.

The request contains no Resource object pointer and no RHI object. Render validates the generation and description before creating GPU state. Completion is published as a small status record keyed by request and resource identity.

### 6.4 Versioning

Every cross-module packet has a schema ID and monotonically increasing schema version. Producers and consumers reject unsupported versions before mutating render state. Within the repository, schema changes update both producer and consumer atomically. Persisted cooked manifests and cache artifacts additionally carry toolchain and platform versions.

## 7. Threading, Queues, and Frame Flow

### 7.1 Frame flow

The canonical frame lifecycle is:

`Simulate -> Extract -> Queue -> BuildGraph -> Execute -> Present -> Retire`

1. The update thread advances World and CPU feature state.
2. Engine invokes RenderExtraction at a stable frame boundary.
3. Extraction validates and seals a complete `RenderFramePacket`.
4. Engine attempts to publish the packet to the bounded frame queue.
5. The render thread accepts the newest complete packet and updates RenderScene.
6. Render builds and validates the RenderGraph.
7. RHI executes, presents, signals a monotonic retirement fence, and retires deferred resources.

### 7.2 Bounded frame queue

The frame queue is bounded, with a default capacity of three complete packets and a supported configuration range of two to four. Publishing never waits indefinitely.

Because packets are complete snapshots, the default overload policy is latest-complete-wins: the oldest packet that has not begun render consumption may be replaced by the newest complete packet. The queue records dropped frame sequence numbers. A packet already acquired by Render is never mutated or replaced.

An incomplete or invalid extraction is not published. Render continues with the last accepted scene state and either presents the previous valid output or a deterministic clear frame according to the failure policy.

### 7.3 Bounded upload queue

The upload queue is bounded by request count and total queued bytes. `TryEnqueueUpload` returns immediately with `Accepted`, `QueueFull`, `Cancelled`, `InvalidRequest`, or `ShuttingDown`.

When the queue is full, Resource retains the CPU-ready state and retries according to priority and per-frame budget. It must not block the main thread. Render drains requests within configurable byte and time budgets so asset bursts cannot consume an unbounded frame.

### 7.4 Shutdown order

Shutdown is deterministic:

1. Stop accepting new simulation frames and resource requests.
2. Cancel cancellable IO, cook, and upload work.
3. Seal the frame and upload queues.
4. Let Render complete or reject acquired work.
5. Wait for submitted GPU work through bounded fence waits with diagnostics.
6. Retire deferred resources, destroy swapchains and the device, then tear down CPU subsystems.

No callback may target a subsystem after its shutdown phase begins.

## 8. Resource Lifecycle and Residency

The authoritative lifecycle is:

`Unloaded -> Resolving -> Loading -> CPUReady -> UploadQueued -> GPUReady -> Evicting -> Unloaded`

- `Resolving` selects a cooked artifact or mounted runtime package entry and validates path, platform, version, and content hash.
- `Loading` performs cancellable asynchronous IO, decompression, parsing, and dependency discovery.
- `CPUReady` owns immutable upload-ready data and can wait safely for upload budget.
- `UploadQueued` means Render accepted the request but has not necessarily completed GPU realization.
- `GPUReady` means the requested generation exists in `RenderResourceRegistry` and is safe for frame binding.
- `Evicting` prevents new references, waits for the last relevant GPU fence, and releases the registry generation.

Failures are recorded as structured outcomes alongside the lifecycle rather than being disguised as a successful state:

- resolve or load failure returns to `Unloaded` with a failure code;
- upload queue pressure leaves the resource in `CPUReady`;
- recoverable GPU creation failure returns to `CPUReady` with a retry/degradation reason;
- invalid or incompatible payload returns to `Unloaded` and is not retried until its content identity changes;
- cancellation returns to the last stable state without publishing a partial generation.

Shipping modes deny source asset reads. Cooked manifests are reproducible and include logical identity, dependency list, artifact hash, target platform/backend profile, shader schema, and toolchain version.

Residency is budget-driven. Resource priority, last-use frame, size, reload cost, and pin state inform eviction. Destruction is always deferred until the last GPU fence that may reference the generation has completed.

## 9. RenderGraph and Canonical Renderer

RenderGraph is the only production GPU orchestration path for the canonical renderer. Each pass declares resource reads, writes, attachment use, queue preference, and external imports before graph compilation.

The graph must provide:

- explicit texture and buffer access with subresource ranges;
- derived barriers and cross-queue synchronization;
- external resource import with initial and required final state;
- transient lifetime analysis and conservative aliasing;
- pass culling when outputs are unused and the pass has no declared side effect;
- validation before command submission;
- deterministic graph diagnostics and GPU marker names.

The canonical cross-backend baseline path is:

`Depth -> PBR Opaque -> Directional Shadow -> IBL/Sky -> ToneMap -> AA -> Present`

It uses one cooked reference scene, the same material and shader semantics, and equivalent pass topology on DX12, Vulkan, and Metal. Backend-specific code is restricted to RHI implementation and platform presentation integration. A backend may choose native resource-state or descriptor mechanisms, but it may not change visible renderer semantics silently.

Advanced passes remain disabled with explicit status until they meet the same contract and validation requirements.

## 10. Tier 1 RHI Production Baseline

DX12, Vulkan, and Metal must implement the following required behavior.

### 10.1 Device and presentation

- deterministic adapter/device identity and driver/API reporting;
- debug/validation enablement in development builds;
- swapchain creation, resize, minimize/restore, present mode or vsync policy, and out-of-date recovery;
- explicit classification of device removal/loss and a recovery entry point;
- predictable headless or offscreen behavior for validation where the platform supports it.

HDR, VRR, and multi-window support are capability-gated rather than baseline requirements.

### 10.2 Queues and synchronization

- logical graphics, compute, and copy queue types;
- a graphics queue on every Tier 1 backend;
- monotonic fence values for submit and retirement;
- GPU queue wait/signal behavior and correct cross-queue ownership transfer;
- bounded CPU waits used only for explicit synchronization, recovery, or shutdown;
- deferred destruction tied to completed fence values.

Dedicated async compute/copy hardware is optional. If queue types map to one native queue, the report must describe the mapping, and behavior must remain correct.

### 10.3 Resources and memory

- buffers, 1D/2D/3D textures, cube textures, views, and samplers required by the canonical renderer;
- upload and readback paths with correct row, slice, and alignment rules;
- resource-state transitions and subresource barriers;
- transient allocation and conservative aliasing support sufficient for RenderGraph;
- memory usage reporting where the native API exposes it;
- safe out-of-memory classification and degradation hooks.

Sparse resources, aggressive heap aliasing, and vendor memory-budget extensions remain capability-gated.

### 10.4 Descriptors, shaders, and pipelines

- one normalized descriptor/binding contract across the Tier 1 backends;
- graphics and compute PSO creation;
- shader reflection validation against descriptor layouts and vertex inputs;
- stable pipeline cache keys including shader content, render state, attachment formats, backend, adapter/driver identity where required, and schema version;
- asynchronous PSO creation and a defined pending/fallback behavior;
- persistent cache load, rejection, and rebuild diagnostics.

Bindless resources, mesh shaders, ray tracing, and variable-rate shading remain capability-gated.

### 10.5 Diagnostics

- debug markers and object names;
- timestamp queries required for frame/pass measurements;
- validation-layer or API-debug messages captured by test artifacts;
- machine-readable capability, adapter, driver, cache, and schema identity reports.

Vendor profiler integrations are optional.

### 10.6 Capability truth policy

A required Tier 1 feature may report `Supported` only when a behavior test passes on a real backend device. A native API difference normalized behind the RHI is still supported when semantics are equivalent. Silent emulation is forbidden. Explicit emulation is acceptable only for Tier 2 compatibility or for a capability-gated non-baseline feature.

DX11 and OpenGL must start, render the canonical basic path where feasible, resize, and shut down in smoke tests. They may report unsupported advanced behavior and may use explicit fallback paths. Tier 1 progress is never blocked on full Tier 2 parity.

## 11. Error Handling and Recovery

### 11.1 Recoverable asset or PSO pending

Missing GPU readiness does not block the main thread. Render uses a declared fallback resource, skips the affected draw, or renders the previous valid generation according to asset policy. Each choice increments a structured counter and records the stable asset/resource identity and reason.

### 11.2 Invalid frame or graph

Packet schema errors, stale generations, impossible resource references, or RenderGraph validation failures are rejected before submit. Render does not partially submit the frame. It preserves the previous valid output when safe or presents a deterministic failure clear, then emits a compact diagnostic artifact.

### 11.3 Swapchain changes

Resize, minimize, restore, and out-of-date events stop presentation work for the affected surface without destroying unrelated persistent resources. Swapchain-dependent graph resources are recreated after the surface becomes valid.

### 11.4 Device loss

Device loss follows a visible state machine:

`Running -> StopSubmit -> CancelOrDrain -> Recreate -> Rehydrate -> Resume`

On detection, Render stops accepting GPU submissions, seals or cancels pending uploads, captures the backend failure reason, and releases invalid device-owned state. It recreates the adapter/device and surfaces, then rehydrates persistent resources from `AssetId` recipes and rebuilds pipeline caches as needed. Frame production resumes only after a minimal valid scene is ready.

If recreation or rehydration fails repeatedly within the configured attempt budget, Engine performs an orderly exit with a machine-readable report. The first production release may support graceful termination on platforms where reliable in-process recreation cannot be proven, but it must never continue with invalid RHI objects.

### 11.5 Memory pressure

Allocation pressure first reduces transient budgets, evicts unpinned streaming resources, lowers optional quality tiers, and defers nonessential uploads. Failure after these steps produces an explicit out-of-memory outcome and orderly shutdown rather than undefined rendering.

## 12. Validation and Release Evidence

Validation is layered so a passing mock contract cannot substitute for real backend behavior.

1. **Contract:** deterministic unit/validation executables for packet schemas, generational handles, queue bounds, state transitions, graph hazards, cache keys, and capability consistency.
2. **Real backend:** DX12, Vulkan, and Metal device tests for resource creation, descriptors, barriers, queues, fences, PSOs, queries, swapchain transitions, and validation-layer cleanliness.
3. **Full frame:** the same cooked reference scene runs the canonical graph on every Tier 1 backend and produces frame reports plus visual goldens with documented tolerances.
4. **Robustness:** resize/minimize loops, out-of-date handling, device-loss injection where available, upload cancellation, queue saturation, memory pressure, deterministic shutdown, soak, and leak checks.
5. **Release:** clean configure/build/test from an empty build tree, package-only startup, reproducible manifest/cache identity, Tier 1 support matrix, Tier 2 smoke results, and known limitations.

Required CI environments are Windows for DX12 and Vulkan, Linux for Vulkan, and macOS for Metal. A backend is not declared production-ready based solely on compilation or a mock device.

Visual comparisons use the same camera, content hash, deterministic settings, and warm-up policy. Tolerances account for documented floating-point and rasterization differences without masking missing passes, wrong resources, or validation errors.

## 13. Milestone Program

This is a program design, not one monolithic implementation plan. Each milestone receives a focused implementation plan and its own exit review. M2 and M3 may proceed in parallel only after M1 is complete. Both streams consume the frozen M1 identity, queue, upload, and ownership contracts; changing those contracts requires a joint architecture review and updated cross-stream tests.

### M0 - Build Truth

Primary responsibility: make every green signal trustworthy before deeper architecture work.

Deliverables:

- build the three currently unavailable validation targets and restore the missing fixture writer;
- make Python and CMake gates fail closed when inputs, targets, or executables are missing;
- prove clean configure and Debug build from an empty build directory;
- remove branch whitespace errors and enforce diff hygiene;
- publish a single authoritative baseline command and result artifact.

Exit gate: zero unexpected `NOT_BUILT` or missing executables, clean diff check, clean configure/build, and all selected architecture tests pass or have an explicitly approved environment skip.

### M1 - Architecture Cut

Primary responsibility: create safe runtime ownership and cross-thread contracts.

Deliverables:

- move extraction invocation and ownership into Engine;
- remove Render's implementation dependency on RenderExtraction, Scene, World, and Resource;
- replace raw render-resource pointers in frame snapshots with stable generational IDs;
- introduce immutable complete `RenderFramePacket` publication;
- introduce bounded frame and upload queues with observable overload behavior;
- make RenderResourceRegistry and deferred RHI destruction render-thread-owned;
- add schema, stale-generation, queue saturation, and shutdown-order validations.

Exit gate: Runtime can simulate, extract, queue, consume, and retire frames without Render dereferencing update-domain objects; boundary gates enforce the dependency direction; queue and lifetime tests pass under saturation.

### M2 - Tier 1 RHI Proof

Primary responsibility: prove the normalized base contract on real DX12, Vulkan, and Metal devices.

Deliverables:

- shared backend conformance harness;
- real-device coverage for resource, descriptor, barrier, queue, fence, pipeline, query, and present behavior;
- evidence-backed capability reports;
- Windows, Linux, and macOS CI runners and artifacts;
- validation-layer/API-debug cleanliness gates.

Exit gate: every required baseline behavior passes on each Tier 1 backend, with no silent emulation and no backend declared supported from compilation alone.

### M3 - Resource Production

Primary responsibility: make cooked content and GPU residency a bounded, recoverable shipping pipeline.

Deliverables:

- reproducible cook/package manifest and package-only Runtime startup;
- cancellable asynchronous IO, dependency resolution, and structured failures;
- immutable upload requests, per-frame upload budgets, and completion publication;
- residency accounting, pinning, eviction, deferred destruction, and rehydration recipes;
- platform shader and PSO artifacts with stable, rejectable cache keys.

Exit gate: the reference scene loads without source assets, survives queue pressure and cancellation, reaches GPU readiness within budgets, evicts safely, and reproduces artifact identities from the same inputs.

### M4 - Canonical Runtime Renderer

Primary responsibility: prove one honest renderer end to end before expanding effects.

Deliverables:

- `Depth -> PBR Opaque -> Directional Shadow -> IBL/Sky -> ToneMap -> AA -> Present` entirely through RenderGraph;
- one cooked reference scene and deterministic camera/settings;
- equivalent shader/material semantics on all Tier 1 backends;
- frame diagnostics, GPU markers/timestamps, screenshots, and visual goldens;
- explicit disabled status for deferred advanced passes.

Exit gate: the same package renders the complete baseline graph on DX12, Vulkan, and Metal, passes graph/RHI validation, and stays within approved visual tolerances.

### M5 - Runtime Hardening

Primary responsibility: make abnormal runtime conditions deterministic and diagnosable.

Deliverables:

- resize, minimize, restore, out-of-date, and present recovery;
- device-loss recreation or proven graceful-termination policy per platform;
- memory-pressure degradation and out-of-memory handling;
- cancellation, queue drain, fence retirement, and deterministic shutdown;
- first-run PSO behavior without unbounded hitches;
- soak, leak, repeated restart, and validation-message gates.

Exit gate: fault and stress suites complete without stale object access, unbounded waits, silent corruption, or unexplained resource growth.

### M6 - Runtime Release Candidate

Primary responsibility: convert engineering proof into a supportable runtime release.

Deliverables:

- reproducible package-only startup and release builds;
- Tier 1 correctness, visual, robustness, and performance gates;
- DX11/OpenGL compatibility smoke tests;
- published capability matrix, artifact schemas, known limitations, and recovery policy;
- retained CI evidence tied to source, toolchain, content, adapter, and driver identity.

Exit gate: a clean machine can build or consume the approved artifacts, launch the packaged reference runtime, pass the release suite, and produce traceable support evidence.

## 14. Migration and Compatibility

- Existing public contracts are migrated incrementally, with compile-time failures preferred over long-lived dual semantics.
- A temporary pointer-based compatibility adapter may exist only on the update side during M1. It must resolve pointers into stable IDs before publication and must not be visible to Render.
- Existing direct GPU paths remain classified as legacy until moved into Render-owned passes. Their gate budget decreases in the milestone that touches them.
- DX11 and OpenGL keep explicit compatibility behavior. They do not force the Tier 1 contract down to the lowest common denominator.
- Existing diagnostic schemas remain versioned. Consumers reject incompatible versions rather than guessing fields.
- Large files are split when ownership moves: packet validation, queue policy, resource registry, graph orchestration, and device recovery should not remain embedded as unrelated sections of `SceneRenderer`.

## 15. Risks and Trade-offs

- **Complete snapshots cost CPU memory and copy bandwidth.** They are chosen for M1 because they make queue replacement, stale-data rejection, and recovery simple. Delta packets are a later measured optimization.
- **Strict Render ownership adds handoff latency.** Bounded queues and latest-complete-wins keep it predictable while eliminating unsafe cross-thread access.
- **Real-device Tier 1 testing increases CI cost.** That cost is necessary because compilation and mock devices cannot prove queue, barrier, descriptor, presentation, or loss behavior.
- **Package-only runtime slows ad hoc source iteration.** Editor/source workflows are deferred; shipping correctness takes priority. Development hosts may retain an explicit non-shipping source mode.
- **A narrow canonical renderer delays feature breadth.** It creates a trustworthy reference path and prevents incomplete effects from consuming production capacity.
- **Device recreation differs by platform and driver.** The contract permits a documented graceful-termination fallback until in-process recovery is proven, but never permits continued use of invalid device state.

## 16. Acceptance Criteria for This Design

The design is implemented when all of the following are true:

- Editor is absent from runtime startup, correctness, validation, and release gates.
- Render has no implementation dependency on Scene, World, Resource, or RenderExtraction.
- Cross-thread frame and upload contracts contain values, immutable payload ownership, stable IDs, and generations only.
- Frame and upload queues are bounded, non-blocking under overload, and observable.
- Render exclusively owns RHI resources, graph execution, submission, presentation, and deferred GPU destruction.
- Shipping runtime loads the canonical scene only from reproducible cooked/package artifacts.
- DX12, Vulkan, and Metal pass the required real-device RHI and full-frame validation ladder.
- DX11 and OpenGL pass their documented compatibility smoke scope without being reported as Tier 1.
- The canonical RenderGraph renderer produces validated and visually acceptable output across Tier 1 backends.
- Recovery, memory pressure, cancellation, and shutdown have deterministic tested outcomes.
- The release candidate publishes evidence-backed capabilities and known limitations without fake success.

## 17. Implementation Planning Boundary

After written-spec approval, planning begins with M0 only. M1 receives its own plan after M0's exit gate. After M1, M2 is decomposed into a shared conformance-harness plan plus backend closure plans, while M3 receives a separate resource-lifecycle plan. M4, M5, and M6 each remain independently reviewable plans.

This decomposition is mandatory: no implementation plan may combine all seven milestones into one patch series, and no later milestone may bypass an earlier exit gate except for the explicitly allowed M2/M3 parallelism after M1.

## 18. Reference Material

- Unreal Engine, Threaded Rendering: <https://dev.epicgames.com/documentation/unreal-engine/threaded-rendering-in-unreal-engine>
- Unreal Engine, Graphics Programming Overview: <https://dev.epicgames.com/documentation/en-us/unreal-engine/graphics-programming-overview-for-unreal-engine>
- Unreal Engine, Render Dependency Graph: <https://dev.epicgames.com/documentation/unreal-engine/render-dependency-graph-in-unreal-engine>
- Unreal Engine, Asset Management and Asynchronous Asset Loading: <https://dev.epicgames.com/documentation/unreal-engine/asset-management-in-unreal-engine>
- Unreal Engine, PSO Precaching: <https://dev.epicgames.com/documentation/unreal-engine/pso-precaching-for-unreal-engine>
- Unity, RenderGraph Fundamentals: <https://docs.unity3d.com/Packages/com.unity.render-pipelines.core@10.7/manual/render-graph-fundamentals.html>
- Unity, Runtime Asset Management: <https://docs.unity3d.com/Manual/assets-managing-introduction.html>
