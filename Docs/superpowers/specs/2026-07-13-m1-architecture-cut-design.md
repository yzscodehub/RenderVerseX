# RenderVerseX M1 Architecture Cut Design

Date: 2026-07-13

Branch baseline: `codex/architecture-implementation` at `9ae22bad45d1acc39a26dbf7ecb7853dde143f60`

Status: Design approved in discussion; written specification self-reviewed; awaiting final user review

Parent design: `2026-07-10-runtime-production-foundation-design.md`

## 1. Decision

M1 establishes a real render-thread boundary and makes it impossible for Render to dereference update-domain objects. The production data flow becomes:

```text
World / Scene / Resource
          |
          v
  RenderExtraction  -- immutable RenderFramePacket -->  Render Thread
          ^                                                |
          |                                                v
        Engine                                      RenderGraph / RHI

ResourceSubsystem -- immutable ResourceUploadRequest --> Render Resource Gateway
                                                          |
                                                          v
                                                    Render Thread
```

The central decisions are:

1. Runtime and Shipping use one dedicated render thread. There is no separate RHI thread in M1.
2. Inline execution exists only as a test implementation of the same runtime pump. It is not a runtime fallback and is not selectable in Shipping.
3. Engine coordinates extraction, publication, initialization order, and shutdown order. RenderSubsystem owns the executor, queues, and render-thread runtime.
4. RenderExtraction produces immutable values. Render consumes no World, Scene, Resource, Camera, callback, or update-owned object pointer.
5. Render owns the RHI device, swapchain, RenderGraph execution, GPU registry, upload realization, presentation, and fence-based retirement.
6. M1 is a hard API cut. All in-repository callers are migrated and the old synchronous or pointer-bearing Render APIs are removed before M1 exits.
7. The migration is contract-first and vertical. It does not begin as a thread wrapper around the current unsafe call graph, and it does not attempt a single big-bang rewrite.

This design intentionally normalizes DX12, Vulkan, and Metal behind one render-thread owner. Those APIs permit parallel recording or command encoding, but M1 first establishes deterministic ownership. Parallel command recording and any distinct RHI thread are M2-or-later optimizations backed by measurement and explicit synchronization contracts.

## 2. Scope

### 2.1 Goals

- Introduce a real dedicated render thread for Runtime and Shipping.
- Reverse the current dependency direction so Engine invokes extraction and Render consumes only value contracts.
- Replace pointer-bearing render snapshots and upload sources with immutable packets, stable asset identity, and generational render-resource handles.
- Make frame and upload publication bounded, non-blocking, observable, and deterministic under overload.
- Keep every strong RHI reference and every RHI object's final destruction on the render thread.
- Make startup, resize, idle behavior, fatal failure, and shutdown explicit state machines with bounded waits.
- Preserve the current renderer's supported behavior while migrating Samples, tests, and the Editor adapter mechanically.
- Provide architecture, concurrency, failure-injection, and Build Truth evidence strong enough to unlock M2 and M3.

### 2.2 Non-goals

- A separate RHI thread or general parallel command recording framework.
- Real-device Tier 1 conformance closure; that is M2.
- Reproducible cooked identity and the complete package/residency lifecycle; that is M3.
- Device-loss recreation. M1 detects, classifies, stops submission, and terminates cleanly; recovery is M5.
- Editor features, Editor UI correctness, or Editor-driven runtime ownership.
- Renderer feature expansion, visual redesign, or RenderGraph feature expansion unrelated to the ownership cut.
- DX11/OpenGL feature parity with DX12/Vulkan/Metal.
- A repository-wide change to the `EngineSubsystem::Initialize()` signature.

## 3. Current-Branch Evidence

The branch contains useful seams but does not yet enforce safe runtime ownership:

- `RenderContracts`, `RenderExtraction`, and `ResourceSceneAdapters` exist, and M0 now provides an authoritative Fresh Build Truth gate.
- `Render` publicly links `RenderExtraction`; `RenderExtraction` publicly links Resource, Scene, and World. The GPU execution module therefore still reaches update-domain implementation transitively.
- `SceneRenderer` constructs extraction bridges and reads World and SceneManager directly.
- `RenderProxySnapshot` contains `IRenderMeshUploadSource*` and `IRenderMaterialSource*`; environment and skybox bridges also contain Resource/RHI pointers and callbacks.
- `RenderScene` and `RenderObject` store Resource-facing pointers.
- `Engine::Tick` synchronously calls `ProcessGPUUploads()` and then `RenderFrame(m_activeWorld)`.
- There is no render thread. Core's thread pool does not establish render ownership.
- `GPUResourceManager` combines upload queuing, realization, registry, status, and residency concerns.
- `FrameResourceManager` relies on frame-count deletion and a global deferred-deleter registry rather than a Render-owned last-use fence.
- `EngineSubsystem::Initialize()` returns `void`, while `SubsystemCollection::InitializeAll()` already catches exceptions and unwinds initialized subsystems.

M1 builds on the existing modules and validation framework. It changes ownership and contracts without replacing World, Scene, RenderGraph, or the RHI implementations wholesale.

## 4. Authoritative Ownership Model

### 4.1 Thread roles

The supported M1 roles are:

| Role | Sole responsibilities |
|---|---|
| Main/Update Thread | OS event integration, Engine tick, World/Scene mutation, Resource state transitions, extraction, packet publication, gateway production, resize requests |
| Render Thread | RHI device and surface objects, RenderScene, RenderGraph, command submission, presentation, GPU registry, upload processing, completion processing, fence retirement |
| Worker Threads | CPU-only loading, decoding, preparation, or immutable computation; no gateway publication and no RHI access |

HAL/Main Thread owns the native window and event source. Render Thread owns device/swapchain creation, resize processing, and presentation. HAL guarantees that any native handles supplied through `NativeSurfaceDesc` remain valid until Render Thread acknowledges shutdown.

### 4.2 Module dependencies

The target dependency direction is:

```text
Core
  ^
  |
RenderContracts
  ^            ^
  |            |
RenderExtraction     Render
  ^                    ^
  |                    |
World / Scene / Resource
           ^           ^
            \         /
               Engine
```

Required rules:

- RenderContracts depends only on Core and standard-library facilities permitted by Core policy.
- RenderExtraction may depend on RenderContracts, World, Scene, Resource, and update-side adapters.
- Engine depends on RenderExtraction and Render, preferably privately unless an Engine public header exposes their types.
- Render must not link or include RenderExtraction, ResourceSceneAdapters, World, Scene, or Resource.
- Resource may depend on RenderContracts for the narrow gateway but not on Render.
- RHI modules remain below Render and never call into update-domain modules.

The CMake graph and the module-boundary manifest are changed together. M1 does not accept a source-level cleanup that leaves a forbidden transitive public link.

### 4.3 Runtime composition

Engine performs composition but does not own `std::thread`, the frame mailbox, or RHI internals. The order is:

1. HAL creates the native window/surface lifetime and provides a `NativeSurfaceDesc`.
2. Engine configures RenderSubsystem before subsystem initialization.
3. RenderSubsystem starts its executor during `Initialize()`.
4. Engine injects RenderSubsystem's narrow `IRenderResourceGateway` into ResourceSubsystem.
5. Update begins only after Render reports `Running`.
6. Each update completes simulation, extraction, and non-blocking frame publication.
7. Shutdown reverses publication and resource production before stopping Render.

Editor may host the same composition path, but it receives only a mechanical packet-publication adapter in M1. Editor is not a correctness or exit-gate dependency.

## 5. Identity and Cross-Thread Type Rules

### 5.1 Asset identity

```cpp
struct AssetId
{
    uint64 value = 0;

    [[nodiscard]] bool IsValid() const noexcept { return value != 0; }
};
```

`AssetId` is the stable logical identity used in frame/resource contracts. During M1, the current Resource ID may be adapted into `AssetId` provisionally, provided the mapping is stable for the process lifetime and collision-checked. M3 must replace that adapter with reproducible cooked/package identity.

### 5.2 Render-resource identity

```cpp
struct RenderResourceHandle
{
    uint32 slot = 0;
    uint32 generation = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return slot != 0 && generation != 0;
    }
};
```

Slot zero and generation zero are invalid. A slot may be reused only after its matching generation reaches `Released`. Reuse increments the generation before publication. If increment would wrap to zero, that slot is permanently retired and capacity is not silently recycled through an ambiguous generation.

A stale generation can never bind, release, fail, or otherwise mutate a newer generation in the same slot.

### 5.3 Forbidden cross-thread fields

No published frame packet or upload request may contain:

- pointers or references to World, Scene, Camera, Component, Entity, Resource, Material, Mesh, Texture, or any update-owned object;
- RHI classes, backend handles, descriptor objects, or ownership-bearing RHI aliases;
- `Core::Ref<>`, `std::function`, callbacks, or captured lambdas;
- `std::span`, raw byte pointers, string views, or another view into external storage;
- mutable containers shared with the producer;
- an API-specific native surface handle, except in the dedicated startup/control-only `NativeSurfaceDesc` described in section 9.

Public APIs may use `std::unique_ptr<const RenderFramePacket>` and `std::shared_ptr<const ResourceUploadRequest>` to transfer or share immutable contract ownership. These ownership wrappers are not packet fields.

## 6. Immutable Render Frame Contract

### 6.1 Construction and publication

RenderExtraction constructs packets through `RenderFramePacketBuilder`. The builder owns mutable intermediate storage on the update thread. `Seal()` validates completeness and returns:

```cpp
std::unique_ptr<const RenderFramePacket>
```

There is no public default-construct-and-mutate path for `RenderFramePacket`. Once sealed, its semantic data cannot change. The mailbox takes exclusive ownership and the render thread acquires exclusive ownership. A rejected or replaced packet may be destroyed by the publishing thread; a consumed packet may be destroyed by the render thread. This is safe because packets contain CPU values only and have no thread-affine destructor. The render-thread-only final-destruction rule applies to strong RHI ownership, not to frame packets.

### 6.2 Required packet content

The M1 packet is a complete snapshot and contains:

- schema ID and schema version;
- strictly increasing frame sequence;
- world revision and extraction completeness counters;
- `RenderViewSnapshot` with numeric camera, viewport, clipping, time, and exposure values;
- primitive snapshots containing transforms, bounds, flags, sort/layer data, and render-resource handles;
- light snapshots containing numeric light state and handles only;
- skybox, environment, and IBL snapshots expressed as values and handles;
- existing Particle, Water, and Terrain feature snapshots after contract validation;
- owned arrays needed for skinning or feature data;
- extraction diagnostics needed to prove that the packet is complete.

Every collection owns its storage. A packet never aliases builder, World, Scene, or Resource memory.

### 6.3 Complete-snapshot semantics

M1 publishes complete snapshots, not deltas. This is required for safe latest-complete-wins replacement. `worldRevision` changes when the logical world is replaced or reset. The render thread treats either of these as a temporal discontinuity:

- a gap between acquired/rendered frame sequence numbers;
- a changed world revision;
- an explicit packet discontinuity flag for camera cuts or reset events.

On discontinuity, temporal history is invalidated. Previous camera/object state is derived only from the last packet actually rendered, never from a packet that was replaced or rejected.

An incomplete extraction is not published. The renderer retains its last accepted scene and records an update-side extraction failure. If no valid scene has ever been accepted, Render uses a deterministic clear.

## 7. Immutable Resource Upload Contract

### 7.1 Ownership

```cpp
using ResourceUploadRequestRef = std::shared_ptr<const ResourceUploadRequest>;
```

The request owns a value payload variant such as `MeshUploadPayload`, `TextureUploadPayload`, or `MaterialUploadPayload`. Payload vectors own their bytes. Offsets and sizes select subranges within owned vectors; they never point outside the request.

Material payloads contain `MaterialSourceData` values and texture `RenderResourceHandle`s. They do not contain material/texture source pointers.

ResourceSubsystem retains one request reference until the request reaches a terminal state. This makes the last release of large CPU payload memory occur on the update thread after status observation, not unpredictably during a render-thread queue operation. Render never stores an update-domain Resource object merely to control payload lifetime.

### 7.2 Request fields

Every request contains:

- schema ID/version and request sequence;
- `AssetId`, reserved `RenderResourceHandle`, and `RenderResourceKind`;
- immutable resource creation values;
- immutable owned payload variant;
- dependency handles and readiness policy;
- byte cost, priority, and compact diagnostic provenance.

Cancellation before Render begins upload is represented by gateway/resource state, not by a callback in the request. Once GPU submission occurs, cancellation means "do not publish this generation as usable" and still requires fence-safe retirement.

Queued-byte accounting is derived from the checked sizes of the request's owned payload containers. Any declared byte-cost field is diagnostic metadata and must match the overflow-checked derived total; callers cannot bypass queue limits by reporting a smaller cost.

### 7.3 Producer rule

The Main/Update Thread is the sole producer of gateway operations. Worker jobs may load, decode, validate, and prepare immutable payloads, but completion is marshalled to ResourceSubsystem's update-thread phase before reservation, enqueue, release, or status-driven state transition.

This retains the current `ProcessCompletedLoads` style of ownership and avoids turning the gateway into a multi-producer synchronization surface in M1.

## 8. Render Resource Gateway and Status Table

### 8.1 Narrow public interface

The gateway contract is defined in RenderContracts and implemented by RenderSubsystem:

```cpp
class IRenderResourceGateway
{
public:
    virtual ~IRenderResourceGateway() = default;

    virtual RenderResourceReserveResult ReserveResource(
        AssetId assetId,
        RenderResourceKind kind) = 0;

    virtual RenderUploadEnqueueResult TryEnqueueUpload(
        const ResourceUploadRequestRef& request) = 0;

    virtual RenderReleaseResult RequestRelease(
        RenderResourceHandle handle) = 0;

    [[nodiscard]] virtual RenderResourceStatus QueryResourceStatus(
        RenderResourceHandle handle) const = 0;
};
```

`ReserveResource` outcomes are `Reserved`, `Existing`, `InvalidAsset`, `KindMismatch`, `CapacityExceeded`, and `ShuttingDown`.

The result contains a valid handle only for `Reserved` or `Existing`; `Existing` returns the already reserved generation and its observed status. All failure outcomes contain an invalid handle.

`TryEnqueueUpload` outcomes are `Accepted`, `QueueFullByCount`, `QueueFullByBytes`, `InvalidRequest`, `StaleGeneration`, `Cancelled`, and `ShuttingDown`.

`RequestRelease` outcomes are `Accepted`, `StaleGeneration`, `AlreadyPending`, and `ShuttingDown`.

No result contains an exception, RHI object, or pointer into Render.

### 8.2 State machine

The public render-resource states are:

```text
Reserved -> UploadQueued -> Uploading -> GPUReady
                              \-------> Failed

Reserved / UploadQueued / Uploading / GPUReady / Failed
                    -- RequestRelease --> Evicting -> Released
```

`QueryResourceStatus` validates generation. A mismatch returns `StaleGeneration`; it never returns the newer generation's state. Failure status includes a stable failure code, not a free-form string as the machine-readable identity.

### 8.3 Fixed-capacity status table

`RenderResourceStatusTable` is allocated before the render thread starts. M1 defaults to 262,144 slots, including invalid slot zero, and supports an explicit configured capacity no smaller than 1,024. Capacity does not grow at runtime.

Each slot exposes one atomically packed 64-bit publication containing generation, public state, and compact failure code. Readers use acquire semantics and successful transitions publish with release semantics. The packed value is an intentionally narrow cross-thread state machine; it is not RHI registry storage.

The update thread owns the `AssetId -> RenderResourceHandle` reservation directory and free-slot allocator. It may perform only these generation-checked compare/exchange transitions:

- `Released -> Reserved` while assigning the next nonzero generation;
- `Reserved -> UploadQueued` after the upload queue accepts the matching request;
- `Reserved`, `UploadQueued`, `Uploading`, `GPUReady`, or `Failed` -> `Evicting` when a matching release request is accepted.

The render thread may perform only:

- `UploadQueued -> Uploading`;
- `Uploading -> GPUReady` or `Uploading -> Failed`;
- `Evicting -> Released` after any submitted work and last-use fence are safe.

If release races upload completion, compare/exchange decides the winner: Render must not publish `GPUReady` over `Evicting`, and an upload that observes `Evicting` retires any partial RHI state before publishing `Released`. The update allocator does not reuse the slot until it observes the matching `Released` generation. This controlled multi-writer state channel does not weaken the rule that RenderResourceRegistry and all RHI state are render-thread-only.

Strings, byte counts, asset names, and rich failure context are published through the immutable diagnostics snapshot, not stuffed into the atomic table. The table is the fast state channel; diagnostics are the support channel.

## 9. RenderSubsystem API and Lifecycle

### 9.1 Public surface

The target RenderSubsystem surface is limited to composition, publication, control, the gateway, and diagnostics:

```cpp
void Configure(const RenderRuntimeConfig& config,
               const NativeSurfaceDesc& surface);

RenderFramePublishResult TryPublishFrame(
    std::unique_ptr<const RenderFramePacket> packet);

RenderResizeResult RequestResize(const NativeSurfaceDesc& surface);

RenderDiagnosticsSnapshot GetDiagnosticsSnapshot() const;
RenderRuntimeResult GetLastRuntimeResult() const;
RenderShutdownResult GetLastShutdownResult() const;
```

It also implements `IRenderResourceGateway`.

`Configure` is valid only before subsystem initialization. `EngineSubsystem::Initialize()` remains `void`; RenderSubsystem's override calls its private `StartRenderRuntime()`. A startup failure stores the structured result and throws `RenderSubsystemInitializationError`. Existing `SubsystemCollection::InitializeAll()` catches the exception and unwinds already initialized subsystems. This avoids changing the base subsystem contract in M1 while preserving structured diagnostics.

### 9.2 APIs removed by M1 exit

The following old shapes have no compatibility path at M1 exit:

- `BeginFrame`, `Render`, `EndFrame`, `Present`, and `RenderFrame(World*)`;
- `RenderSubsystem::Render(World*, Camera*)`;
- World/SceneManager overloads of SceneRenderer view/setup APIs;
- `ProcessGPUUploads()` as a public update-thread execution call;
- `GPUResourceManager::RequestUpload/UploadImmediate(IRender*Source*)`;
- public getters exposing SceneRenderer, GPUResourceManager, device, swapchain, RenderGraph, or other render-thread-owned objects.

Tests and Samples use public contracts or explicit test harnesses. They do not retain production-only backdoors.

### 9.3 Lifecycle state machine

```text
Stopped -> Starting -> Running -> StopRequested -> Draining -> Stopped
              \-> Failed            \---------------------> Failed
```

Only RenderSubsystem transitions lifecycle state. Observers receive atomic state or immutable diagnostics; they cannot mutate it.

Startup sequence:

1. Validate configuration and `NativeSurfaceDesc` on the update thread.
2. Allocate the fixed status table, bounded queues, coalesced control slots, diagnostics buffers, and executor.
3. Start `DedicatedRenderExecutor`.
4. Render thread records its owner thread ID.
5. Render thread creates the RHI device, swapchain/surface objects, RenderContext, SceneRenderer, resource registry, upload processor, and retirement queue.
6. Render thread publishes a structured startup result through a condition variable.
7. Update thread returns from `Initialize()` only after `Running` or a terminal startup result.
8. Any partial startup failure is unwound on the render thread in reverse creation order before failure is acknowledged.

### 9.4 Executor policy

`IRenderExecutor` has two implementations:

- `DedicatedRenderExecutor`: compiled and selected for Runtime and Shipping; owns the render thread and invokes `RenderThreadRuntime::PumpOnce()`.
- `InlineRenderExecutor`: compiled only into test support; invokes the same `PumpOnce()` synchronously under an explicit render-thread test scope.

Shipping configuration cannot select, discover, or fall back to Inline execution. A dedicated-thread startup failure is fatal initialization failure.

### 9.5 Native surface contract

`NativeSurfaceDesc` is a tagged platform value used only for startup and coalesced surface control. It contains:

- platform tag;
- non-owning native handles stored as `uintptr_t` values with platform-specific validation;
- width, height, format/presentation preferences required by the existing RHI path;
- monotonically increasing surface generation.

The descriptor owns no OS object. HAL owns the referenced window/layer/view and keeps it alive until Render acknowledges shutdown. A resize/surface update with an older generation is ignored and counted.

## 10. Bounded Queues and Control Publication

### 10.1 Frame mailbox

The frame mailbox is a mutex-protected SPSC bounded deque. It avoids lock-free lifetime complexity until profiling proves a need.

- Default capacity: 3 packets.
- Supported configured range: 2 through 4 packets.
- Publication never waits for render consumption.
- When full, the oldest unacquired packet is destroyed/replaced by the newest complete packet.
- A packet already acquired by Render is never replaced.

`TryPublishFrame` returns `Accepted`, `ReplacedOlder`, `InvalidPacket`, `OutOfOrder`, or `ShuttingDown`. Replacement records both replaced and replacement sequence numbers in diagnostics. Invalid schema/completeness and non-monotonic publication are rejected before queue mutation.

Replacement moves the old packet out while holding the mailbox lock and destroys it after unlocking. Large CPU packet destruction therefore does not lengthen the mailbox critical section.

### 10.2 Upload queue

The upload queue is a mutex-protected SPSC queue bounded by both request count and retained payload bytes.

- Maximum queued requests: 1,024 by default.
- Maximum queued payload: 256 MiB by default.
- Per-render-iteration drain: at most 64 requests, 32 MiB, and 2 ms, stopping when any budget is reached.

All defaults are explicit configuration values constrained to safe nonzero ranges. They are safety limits, not performance certification; M3 profiles and tunes them.

On pressure, Resource remains CPU-ready and retries later by its priority policy. The update thread does not block, Render does not silently drop an accepted request, and queue-full outcomes are counted by cause.

Enqueue publication is atomic with the public status transition: under the queue's producer critical section, the gateway validates count and derived-byte capacity, compare/exchanges the matching `Reserved` state to `UploadQueued`, appends the request, then unlocks and wakes Render. A capacity failure leaves the state `Reserved`; the consumer cannot observe a queued request before it observes `UploadQueued`.

### 10.3 Release queue

Accepted releases are transported through a fixed SPSC ring with usable capacity equal to the number of nonzero status slots. The update thread is its sole producer and the render thread is its sole consumer. The status transition to `Evicting` happens before publication to the ring.

Normal runtime drain is bounded to 1,024 releases and 1 ms per render iteration, stopping when either limit is reached. Terminal shutdown ignores the per-iteration limit and drains the sealed queue subject to the global shutdown watchdog.

At most one release may be pending for a slot/generation: a second request observes `Evicting` and returns `AlreadyPending`. Therefore the ring is capacity-proven for every live slot and does not need a `QueueFull` public outcome. Failure to publish after a successful state transition is an internal ownership invariant violation: RenderSubsystem seals the gateway, records a runtime-fatal result, and begins terminal shutdown.

The render thread validates generation again before acting. It cancels an unstarted upload or records the last-use fence for live GPU state, then eventually publishes `Released`. A stale ring entry can never release a reused slot.

### 10.4 Coalesced control

M1 does not introduce an unbounded control-command queue. It uses fixed coalesced slots:

- newest pending surface descriptor;
- newest pending resize descriptor;
- diagnostics-refresh requested bit;
- atomic stop requested bit.

Surface and resize slots are generation-ordered. Stop is out-of-band and cannot be delayed behind frames or uploads. Replaced controls are observable counters.

## 11. Render Thread Runtime

### 11.1 Pump order

Each `RenderThreadRuntime::PumpOnce()` iteration performs:

1. Observe stop and high-priority surface control.
2. Acquire/coalesce the newest available complete frame.
3. Process release/cancellation requests, then uploads, within their count, byte, and time budgets.
4. Validate and apply the acquired packet to RenderScene.
5. Build/validate/execute RenderGraph and submit/present only for a new accepted frame or an explicit resize redraw.
6. Poll submission/upload fence completion and process resource state transitions.
7. Retire fence-safe RHI objects.
8. Publish lifecycle and diagnostics snapshots.
9. Wait on a condition variable when there is no actionable work.

The thread does not spin and does not repeatedly present when no new frame exists. Upload work may progress without presentation. A valid resize may redraw the last rendered packet; if none exists, Render performs one deterministic clear for the new surface.

### 11.2 Packet application

Schema, sequence, world revision, and handles are validated before RenderScene mutation. Application is transactional at the frame-contract level: an invalid packet does not partially update the accepted render scene.

Missing or not-yet-`GPUReady` resources use the declared fallback or skip policy. Render never waits synchronously for an asset upload during frame processing.

### 11.3 RHI callbacks

Backend completion callbacks may record an atomic/native completion signal only. They cannot mutate RenderResourceRegistry, RenderScene, upload records, diagnostics containers, or retirement storage. The render thread polls or consumes completion signals and performs all semantic transitions.

## 12. Render-Owned Resource Components

M1 splits the current monolithic responsibilities into:

- `RenderResourceStatusTable`: fixed cross-thread public state channel;
- `RenderResourceRegistry`: render-thread-only slot/generation to strong-RHI-state mapping;
- `RenderUploadProcessor`: validates accepted requests, creates resources, submits uploads, and processes completion;
- `RenderRetirementQueue`: retains objects until last-use fences complete;
- temporary `GPUResourceManager` facade during migration only.

The temporary facade is removed before M1 exits. New code targets the split components directly; the facade exists only to keep intermediate commits buildable.

### 12.1 Two-phase resource commit

Upload realization follows:

1. Validate request schema, handle generation, kind, dependencies, and payload bounds.
2. Create a pending registry entry for the exact generation.
3. Create RHI objects and submit upload work.
4. Retain partial objects and request state until the submission fence completes.
5. On success, publish `GPUReady` with release semantics.
6. On failure, publish a stable failure, transfer partial RHI objects to retirement, and never expose the pending generation as usable.

An older valid generation remains independent until its own release request and last-use fence complete. Failure of a replacement generation does not invalidate an older generation implicitly.

### 12.2 Fence-based retirement

Frame-count deletion and the global deferred-deleter path are not sufficient for production ownership. Render introduces a queue whose entries contain at least:

```cpp
struct RenderRetirementEntry
{
    uint64 lastUseFenceValue = 0;
    Ref<RefCounted> object;
    uint64 estimatedBytes = 0;
};
```

The exact type-erasure may differ, but these invariants do not:

- only Render-thread components create retirement entries;
- registry, cache, or pass code explicitly transfers its last strong RHI reference;
- an entry releases only when `completedFenceValue >= lastUseFenceValue`;
- the last strong RHI release and final destructor run on the render thread;
- no strong RHI reference crosses into a frame packet, upload request, Resource object, or update-side diagnostics object.

M1 removes RHI use of the global `DeferredDeleterRegistry`. Non-RHI uses, if any are proven, require separate ownership and are not silently redirected through Render.

## 13. Error and Failure Policy

### 13.1 Structured result model

Failures use stable enums plus context fields such as frame sequence, request sequence, `AssetId`, `RenderResourceHandle`, surface generation, backend, and native error code where available. Free-form messages are supplementary diagnostics, not test or control-flow identities.

Failures are classified as:

| Class | Examples | Required behavior |
|---|---|---|
| Expected pressure | frame replacement, upload queue full, resource pending | return explicit outcome; count; retry/fallback without blocking |
| Recoverable frame | incomplete extraction, stale handle, missing GPU readiness, invalid packet | reject/skip/fallback; no partial submit; keep last valid state when safe |
| Frame-fatal | RenderGraph validation failure, surface temporarily invalid | no partial submit; deterministic previous output/clear; diagnostic record |
| Runtime-fatal | device loss in M1, unhandled render exception, impossible ownership violation, startup/shutdown watchdog | stop submission, capture evidence, execute terminal shutdown/fatal policy |

### 13.2 Guarding ownership

`RenderThreadGuard` records the owner thread ID and asserts every RHI-owning or render-state mutation path. An update-thread guard protects extraction and gateway production assumptions. Debug builds assert immediately; release builds also return or publish structured fatal results where continued execution would be unsafe.

The render-thread entry catches all exceptions. No exception crosses the thread boundary. It converts the exception into a runtime-fatal result, stops new work, and enters the shutdown path.

### 13.3 Frame behavior

- Unsupported schema or out-of-order sequence is rejected before RenderScene mutation.
- Stale resource handles never resolve to a newer generation.
- A missing `GPUReady` resource uses an explicit fallback or skips the draw; it never causes a synchronous wait.
- RenderGraph validation failure prevents any command submission for that frame.
- Incomplete extraction remains an update-side failure and is never published.

### 13.4 Device loss

M1 classifies device removal/loss, records backend diagnostics, stops submission, and reaches a bounded terminal outcome. It may perform orderly fatal termination rather than in-process recovery. Recreate/rehydrate/resume behavior remains M5 work and is not simulated by continuing with invalid RHI objects.

## 14. Diagnostics Contract

Render builds an immutable `RenderDiagnosticsSnapshot` and atomically publishes `std::shared_ptr<const RenderDiagnosticsSnapshot>`. `GetDiagnosticsSnapshot()` loads the current immutable snapshot and returns a value copy; a concurrent reader may safely retain the prior publication while Render publishes the next one. No snapshot owns an RHI reference. It contains at least:

- executor kind, lifecycle state, render-thread identity hash, and last transition;
- backend, surface generation, extent, and resize/coalescing counts;
- last published, acquired, applied, submitted, and presented frame sequence;
- frame queue capacity, high-water mark, replacements, invalid packets, and out-of-order rejections;
- upload queue request/byte capacity, current usage, high-water marks, pressure outcomes, accepted/completed/failed counts;
- release queue capacity, current usage, high-water mark, accepted/completed/stale counts, and oldest pending generation;
- resource state counts, stale-generation attempts, fallbacks, and skipped draws;
- last submitted/completed fence values;
- retirement entry count, estimated bytes, and oldest fence value;
- last stable startup, runtime, or shutdown failure with compact context.

The snapshot owns all strings and arrays it exposes. Engine, tests, Build Truth, and future support tooling consume the snapshot without accessing render-owned live objects.

## 15. Shutdown and Watchdogs

### 15.1 Ordered shutdown

The required order is:

1. Stop simulation/load production and prevent new gateway reservations.
2. Seal frame, upload, release, and resize publication.
3. Set the out-of-band stop request and wake Render Thread.
4. Discard unacquired frames with counters.
5. Cancel uploads not yet started; finish or classify already submitted uploads.
6. Wait for the last submitted GPU fence within the shutdown budget.
7. Drain fence-safe retirement and report any object that cannot become safe.
8. Destroy registry, upload processor, SceneRenderer/RenderContext, swapchain, and device on Render Thread.
9. Publish the structured shutdown result, exit the executor loop, and join the thread.
10. Destroy World, Resource, and remaining CPU subsystems.

Engine shutdown does not enqueue one release request per live resource. Once production is sealed, Render performs authoritative registry teardown after its last-use fence. This prevents shutdown queue floods and preserves ownership.

### 15.2 Bounded waits

- Startup watchdog default: 60 seconds.
- Shutdown watchdog default: 30 seconds.
- Tests inject a fake clock or shorter explicit policy.

Runtime watchdog expiry writes the latest diagnostics artifact and enters the fatal termination policy. Render Thread is never detached, and Engine never continues after abandoning live RHI ownership.

## 16. Migration Strategy

M1 uses a contract-first vertical sequence:

1. Add value identities, schemas, result enums, and packet/upload value types in RenderContracts.
2. Add the generational handle allocator, status table contract, and narrow gateway.
3. Add bounded mailbox/control structures plus Inline and Dedicated executors around one runtime pump.
4. Start a minimal RenderThreadRuntime and move device/swapchain/RenderContext ownership into it.
5. Add fence-based RenderRetirementQueue and remove RHI dependence on global deferred deletion.
6. Split RenderResourceRegistry and RenderUploadProcessor from GPUResourceManager behind a temporary facade.
7. Build the complete `RenderFramePacketBuilder` aggregate and migrate feature snapshots.
8. Move extraction invocation to Engine and publish packets through RenderSubsystem.
9. Convert SceneRenderer/RenderScene to packet-only input and handle-only resource lookup.
10. Migrate Samples, validation executables, and the Editor's adapter mechanically.
11. Delete the facade, old pointer APIs, old synchronous frame APIs, and forbidden module links.
12. Enable architecture, concurrency, failure-injection, TSAN, smoke, and Fresh Build Truth gates.

Every intermediate commit must build or be explicitly scoped as a mechanical compile-boundary commit in the implementation plan. No long-lived deprecated dual path is accepted.

## 17. Validation Design

### 17.1 Shared executor conformance

Core scenarios are parameterized against both `InlineRenderExecutor` and `DedicatedRenderExecutor`:

- startup success and each startup failure stage;
- complete packet publication and transactional application;
- out-of-order, unsupported schema, incomplete, and stale-handle rejection;
- capacity two through four and latest-complete-wins replacement;
- upload count/byte pressure and deterministic retry;
- two-phase resource success/failure/replacement;
- resize generation coalescing and idle no-present behavior;
- stop while idle, under frame pressure, during upload, and after submission;
- bounded shutdown and complete retirement.

Dedicated-only tests additionally prove real thread separation, wakeup behavior, stress publication, and same-thread RHI construction/destruction.

Tests use `std::latch`, condition variables, barriers, fake fences, and fake clocks. Correctness tests do not coordinate through arbitrary sleeps.

### 17.2 Fake RHI fault injection

The fake RHI can fail deterministically at:

- device creation;
- swapchain creation;
- RenderContext or SceneRenderer initialization;
- the Nth resource creation;
- upload submission;
- fence signal/wait/completion;
- resize;
- present.

For each injected failure, tests verify:

- the structured failure class and stable code;
- no exception crosses the executor boundary;
- creation and destruction of every fake RHI object occur on the recorded render thread;
- live fake RHI object count reaches zero;
- no stale generation mutates a replacement;
- queues and diagnostics reach a terminal consistent state.

### 17.3 Thread sanitizer gate

Linux CI builds a deliberately small TSAN target containing only mailbox, executor, status table, registry state machine, diagnostics publication, and fake RHI. It excludes Vulkan drivers, Editor, Samples, and unrelated third-party code. This gate is required for M1 and uploads its report as Build Truth evidence.

### 17.4 Static architecture proof

M1 adds or updates:

- CMake/module-boundary checks proving Render has no forbidden dependency;
- a specialized RenderContracts field checker that parses the relevant declarations and forbids pointer/reference/callback/RHI fields while permitting ownership wrappers in public API parameters;
- runtime fake-RHI thread-ownership assertions;
- a phase gate proving removed symbols and old overloads are absent;
- schema and generation invariant validation.

A naive repository-wide pointer grep is not an acceptable contract checker because it cannot distinguish startup native handles or API ownership wrappers from packet fields.

### 17.5 Integration and platform evidence

Required M1 evidence is:

- shared Inline/Dedicated contract suites pass;
- dedicated-thread stress and fault suites pass;
- Linux TSAN target passes;
- Runtime Samples compile and execute their existing smoke scope through packet publication;
- Editor adapter receives compile-only smoke coverage;
- Windows local Fresh Build Truth passes from a clean build tree;
- Windows, Linux, and macOS CI Build Truth passes and retains artifacts;
- the final evidence records the exact source commit.

M1 does not claim real-device Tier 1 RHI support from these results. That evidence belongs to M2.

## 18. M1 Exit Criteria

M1 is complete only when all conditions are true:

1. Render has no source or link dependency on RenderExtraction, ResourceSceneAdapters, World, Scene, or Resource.
2. Engine owns extraction invocation and publishes complete immutable packets.
3. Runtime/Shipping always use DedicatedRenderExecutor; Inline cannot be selected in Shipping.
4. Render Thread is the sole owner of device, swapchain, RenderScene, RenderGraph execution, registry, uploads, submission, presentation, and RHI destruction.
5. Frame packets and upload requests contain only owned values, stable identities, and generational handles permitted by this specification.
6. Frame/upload/control publication is bounded, non-blocking, generation-aware, and observable.
7. RHI retirement is last-use-fence-based, and final RHI destruction occurs on Render Thread.
8. Startup, idle, resize, pressure, failure, and shutdown behavior match the documented state machines and watchdog policies.
9. GPUResourceManager's temporary facade and every named old synchronous/pointer API are removed.
10. Shared executor, dedicated stress, fault injection, architecture, symbol-absence, and TSAN gates pass.
11. Runtime Samples pass smoke coverage; Editor is compile-only and is not a correctness dependency.
12. Local and CI Fresh Build Truth artifacts are green and tied to the final source commit.

M2 and M3 planning begins only after this exit review. Changes to the frozen M1 identity, queue, upload, or ownership contracts require an explicit architecture review and updated concurrency evidence.

## 19. Trade-offs and Rejected Alternatives

### 19.1 Dedicated thread versus synchronous wrapper

A synchronous wrapper would reduce the initial diff but preserve unsafe ownership and make later migration harder to validate. M1 therefore introduces the actual thread while keeping Inline only for deterministic contract tests.

### 19.2 One owner versus immediate parallel recording

DX12, Vulkan, and Metal all support useful parallel command work, but they also require precise synchronization and lifetime discipline. One Render Thread first provides a provable owner and portable baseline. M2 may add per-thread command pools/allocators and immutable recording jobs without weakening ownership.

### 19.3 Complete packets versus deltas

Complete packets consume more CPU memory and copying but make queue replacement, recovery from skipped updates, and validation deterministic. Delta packets are deferred until profiling shows a need and equivalence tests can prove loss/reordering behavior.

### 19.4 Mutex queues versus lock-free queues

Bounded mutex/CV structures are easier to reason about, instrument, and test with non-trivial C++ ownership. Lock-free replacement is deferred until measurement demonstrates contention that matters.

### 19.5 Hard cut versus deprecated dual path

A dual path would allow pointer/synchronous behavior to remain the accidental truth and double the test surface. M1 migrates repository callers and deletes the old path before exit.

### 19.6 Fixed status capacity versus runtime growth

Fixed capacity prevents reallocation races and keeps status lookup stable and cheap. Capacity exhaustion is explicit and diagnosable. M3 may introduce a measured sharding or growth design only if real content proves the configured limit inadequate.

## 20. Commercial-Engine and API References

The design adopts principles, not source implementations:

- Unreal Engine keeps gameplay objects and render-thread state separate and transfers render-owned proxy/state rather than dereferencing gameplay objects from the render thread: <https://dev.epicgames.com/documentation/unreal-engine/threaded-rendering-in-unreal-engine>
- Unreal's parallel-rendering overview motivates later immutable recording work without requiring it in the initial ownership cut: <https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine>
- Unity exposes dedicated and parallel rendering-thread modes, reinforcing that execution policy must be explicit rather than an invisible fallback: <https://docs.unity3d.com/ja/6000.0/ScriptReference/Rendering.RenderingThreadingMode.html>
- Vulkan requires external synchronization for specified host access and recommends per-thread command-pool ownership for parallel recording: <https://docs.vulkan.org/guide/latest/threading.html> and <https://docs.vulkan.org/spec/latest/chapters/fundamentals.html>
- Direct3D 12 permits command-list generation from multiple threads while command-queue submission remains explicitly ordered: <https://learn.microsoft.com/en-us/windows/win32/direct3d12/design-philosophy-of-command-queues-and-command-lists>
- Metal command queues are thread-safe and support parallel command-buffer encoding, leaving room for later recording parallelism behind the same M1 owner contract: <https://developer.apple.com/documentation/metal/mtlcommandqueue> and <https://developer.apple.com/documentation/Metal/setting-up-a-command-structure>

## 21. Implementation Planning Boundary

After this written specification is approved, a separate M1 implementation plan will decompose section 16 into small, verifiable tasks with exact files, tests-first steps, review checkpoints, and rollback-safe commits.

The implementation plan may refine names and file placement when repository evidence requires it, but it may not change the approved ownership direction, hard API cut, Shipping executor policy, cross-thread forbidden types, bounded queue semantics, fence-based retirement, failure policy, or exit gates without returning to design review.
