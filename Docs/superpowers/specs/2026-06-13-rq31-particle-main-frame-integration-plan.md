# RQ31 - Particle Main-Frame Integration

Date: 2026-06-13
Program: Render Quality & Verification Program v2
Previous stage: RQ30 - CPU Billboard Particle Render Bridge

## Stage Decision

Connect the RQ30 CPU billboard particle path to the production frame instead of leaving it available only to focused tests:

`Engine / ParticleSubsystem`
-> `RenderSubsystem` device and `SceneRenderer`
-> `ParticlePass` registered in the default render pass chain
-> `SceneRenderer` pre-graph prepare callback invokes `ParticleSubsystem::PrepareRender(view)`
-> visible `ParticleSystemInstance` batches drawn by the registered pass.

This stage is about frame integration and honest ownership. It does not add new particle features. Before implementation starts,
read this document and execute only the tasks in this stage. Spark plan review must pass first. After implementation, Spark code
review must pass before commit.

## Current Evidence

- RQ30 made CPU billboard particle simulation and `ParticleRenderer` draw commands real, but the path is still module-local.
- `SceneRenderer::SetupDefaultPasses()` registers `DepthPrepass`, `ShadowPass`, `OpaquePass`, `SkyboxPass`, and `TransparentPass`; it does not register `ParticlePass`.
- `SceneRenderer::BuildRenderGraph()` only adds passes already owned by `RenderPassRegistry`, sorted by `IRenderPass::GetPriority()`.
- `RenderPassRegistry::AddPass()` takes `std::unique_ptr<IRenderPass>`, calls `OnAdd(device)`, and owns the pass. A pass cannot be simultaneously owned by `ParticleSubsystem`.
- `ParticleSubsystem::Initialize()` still contains a disabled comment for acquiring the RHI device from `RenderSubsystem`; production initialization currently fails unless tests call `SetDeviceForTesting()`.
- `ParticleSubsystem::PrepareRender()` already culls instances and calls `m_renderPass->SetParticleSystems(m_visibleInstances)`, but that pass is not registered in `SceneRenderer`.
- `SceneRenderer::Render()` calls `PreparePassesForFrame()`, then clears/builds the RenderGraph. A generic pre-graph callback seam should run between those steps so feature modules can prepare pass inputs without adding a Render-to-feature dependency.
- `RenderSubsystem::Render()` calls `m_sceneRenderer->SetupView(*camera, world)` and then `m_sceneRenderer->Render()`. It must stay independent from Particle; it is not the right place for a direct `ParticleSubsystem` include/call.
- `ParticleComponent::CreateInstance()` still directly allocates `new ParticleSystemInstance(m_particleSystem)` and therefore bypasses `ParticleSubsystem`, the CPU simulator, and the visible instance list.
- `ParticleComponent::DestroyInstance()` directly deletes the instance and also bypasses `ParticleSubsystem`.
- `Engine` dependency sorting already initializes typed dependencies before dependents. `ParticleSubsystem` declares `RenderSubsystem` and `ResourceSubsystem` dependencies, so production initialization can use `GetEngine()->GetSubsystem<RenderSubsystem>()` safely when the subsystem is registered.
- `SubsystemCollection::InitializeAll()` marks a subsystem initialized after `Initialize()` returns even if that function internally early-returns. `ParticleSubsystem` needs its own render-integration readiness/status instead of relying on `ISubsystem::IsInitialized()`.
- ModelViewer currently registers `WindowSubsystem`, `ResourceSubsystem`, `InputSubsystem`, and `RenderSubsystem`; it does not register `ParticleSubsystem`.
- ModelViewer currently does not link the Particle module, so registering `ParticleSubsystem` requires a sample CMake change.

## Scope

1. Add production device and renderer acquisition to `ParticleSubsystem`.
   - In `Initialize()`, acquire `RenderSubsystem` through `GetEngine()` and use `RenderSubsystem::GetDevice()` when no explicit device was supplied.
   - Preserve an explicit test/bootstrap override, but rename or supplement `SetDeviceForTesting()` with a production-neutral API only if needed.
   - If no device is available, mark render integration as visibly unsupported and avoid creating a fake renderer/pass.
   - Do not silently report success when device, renderer, pass registration, or callback registration fails.
2. Add a Render-owned pre-graph prepare callback seam.
   - Add a generic callback API to `SceneRenderer`, for example:
     - `using PreGraphPrepareCallback = std::function<void(const ViewData&)>;`
     - `bool AddPreGraphPrepareCallback(void* owner, PreGraphPrepareCallback callback);`
     - `bool RemovePreGraphPrepareCallback(void* owner);`
   - Store callbacks by owner token and reject duplicate owners.
   - Invoke callbacks in `SceneRenderer::Render()` after `PreparePassesForFrame()` and before RenderGraph clear/build.
   - The callback seam must not know about `ParticleSubsystem`; `Render` must not include or link `Particle`.
   - Add a small test hook or focused validation path for callback ordering if production `SceneRenderer::Render()` is too expensive to instantiate in unit tests.
3. Transfer `ParticlePass` ownership into `SceneRenderer`.
   - `ParticleSubsystem` may create/configure the `ParticlePass`, but after registration `SceneRenderer` must own it through `AddPass(std::unique_ptr<IRenderPass>)`.
   - `ParticleSubsystem` keeps a non-owning `ParticlePass*` cached pointer for `PrepareRender()`.
   - Shutdown must not double-delete the pass. If registered, the pass is removed from `SceneRenderer` by name during `ParticleSubsystem::Deinitialize()`.
   - Keep `Render` independent from `Particle`; registration happens from the Particle module because Particle already links against Render.
4. Prepare particles each production frame through the callback.
   - During `ParticleSubsystem::Initialize()`, register a pre-graph callback with `SceneRenderer` after pass registration succeeds.
   - The callback calls `ParticleSubsystem::PrepareRender(view)` only if `IsRenderIntegrationReady()` is true.
   - If the subsystem is absent, Render does nothing because no callback is registered.
   - If the subsystem exists but no pass was registered, record/log an honest reason in Particle readiness/status instead of silently succeeding.
   - `ParticleSubsystem::PrepareRender()` must be null-safe when the non-owning pass pointer is unavailable.
   - During `ParticleSubsystem::Deinitialize()`, remove the pre-graph callback with the subsystem owner token before clearing readiness or resetting renderer/pass state.
   - Deinitialization order must be: unregister callback, mark render integration not ready, remove `ParticlePass` from `SceneRenderer`, then reset renderer/sorter/state. A callback must not be able to fire after the subsystem starts deinitializing.
5. Add Particle render-integration readiness/status.
   - Add explicit APIs such as:
     - `bool IsRenderIntegrationReady() const`
     - `const std::string& GetRenderIntegrationUnsupportedReason() const`
     - optional stats for `prepareFrameCount`, `skippedPrepareFrameCount`, and `registeredPass`.
   - Do not use only `ISubsystem::IsInitialized()` as proof that particle rendering is connected.
   - When device acquisition, renderer creation, pass registration, or callback registration fails, expose a concrete reason.
6. Connect `ParticleComponent` instances to `ParticleSubsystem`.
   - On instance creation, prefer `Engine::Get()->GetSubsystem<ParticleSubsystem>()` when available and render integration is ready.
   - Instances created through the subsystem must receive the CPU simulator path added in RQ30 and participate in `m_instances` / `m_visibleInstances`.
   - On destroy, return the instance through `ParticleSubsystem::DestroyInstance()`.
   - Keep a legacy direct-allocation fallback only when no render-ready subsystem exists, and make that fallback observable in component state or logs. The fallback is compatibility only, not the main path.
   - Track instance ownership explicitly, for example `SubsystemOwned` vs `LegacyFallback`, so destroy cannot route a fallback pointer into the subsystem.
   - Do not introduce a global Particle singleton.
   - Update `Particle/CMakeLists.txt` to link `RVX_Engine` as a Particle dependency for this lookup. This is `Particle -> Engine`; do not add any `Engine -> Particle` dependency.
7. Register ParticleSubsystem in ModelViewer.
   - Add `ParticleSubsystem` registration after its dependencies are registered.
   - Link the `ModelViewer` target against `Particle`.
   - This stage does not require ModelViewer to spawn a visible particle effect by default. It must initialize without the previous "No RHI device available" failure and keep existing model rendering intact.
8. Add targeted validation coverage.
   - Verify the `SceneRenderer` pre-graph callback seam rejects duplicate owners, can remove callbacks, and invokes callbacks after pass preparation and before graph build through a narrow test seam/source guard if full renderer execution is not practical.
   - Verify `ParticleSubsystem::Initialize()` has source-level or fake-renderer coverage for acquiring a device from `RenderSubsystem` without making Render depend on Particle.
   - Verify `ParticleSubsystem` registers exactly one `ParticlePass` with `SceneRenderer` and removes it during deinitialize.
   - Verify `ParticleSubsystem::PrepareRender()` feeds visible instances into the registered pass.
   - Verify the registered callback invokes particle prepare before the RenderGraph is built when a particle subsystem is render-ready.
   - Verify `ParticleComponent` creates/destroys through `ParticleSubsystem` when the subsystem is available, and the created instance has a CPU simulator.
   - Verify the fallback path is detectable when no subsystem exists.

## Out of Scope

- Soft particles and depth-fade descriptor binding.
- Texture/flipbook particle materials.
- GPU simulation, GPU sorting, indirect draw completion, or compute emission.
- Trails, mesh particles, stretched billboards, vector fields, collision, motion vectors, volumetric particles, or particle lights.
- Rendering a default particle effect in ModelViewer.
- ECS/Object/SceneEntity removal or a new component ownership model.
- Moving ParticlePass into Render or adding a Render-to-Particle dependency.
- Making `RenderSubsystem.cpp` include `ParticleSubsystem.h` or link against Particle.
- Reworking subsystem tick phases globally. RQ31 uses a generic `SceneRenderer` pre-graph prepare hook because particles need current `ViewData`.

## Expected Files

- `Docs/superpowers/specs/2026-06-13-rq31-particle-main-frame-integration-plan.md`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Particle/CMakeLists.txt`
- `Particle/Include/Particle/ParticleSubsystem.h`
- `Particle/Private/ParticleSubsystem.cpp`
- `Particle/Include/Particle/ParticleComponent.h`
- `Particle/Private/ParticleComponent.cpp`
- `Samples/ModelViewer/main.cpp`
- `Samples/ModelViewer/CMakeLists.txt`
- `Tests/CMakeLists.txt` if validation dependencies need Engine/Runtime/World additions
- `Tests/ParticleValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp` only if existing pass-chain assertions are easier to extend there
- `Docs/superpowers/specs/phase-log.md`

## Required Tests

- `ParticleValidation`
  - `ParticleSubsystem` exposes readiness/status and does not treat `ISubsystem::IsInitialized()` alone as render-ready.
  - `ParticleSubsystem` obtains or attempts to obtain a production device from `RenderSubsystem` without requiring `SetDeviceForTesting()` in the production path. If a real engine/render-subsystem test is impractical, add source guardrails plus callback/pass integration tests with fake devices.
  - `ParticleSubsystem` registers `ParticlePass` into `SceneRenderer` and does not retain owning pass state after transfer.
  - `ParticleSubsystem::Deinitialize()` removes `ParticlePass` from the renderer without double deletion.
  - `ParticleSubsystem::PrepareRender()` updates the registered pass from visible CPU billboard instances.
  - `ParticleSubsystem::Deinitialize()` removes the pre-graph callback before clearing state; invoking the renderer callback seam after deinitialize does not call back into the subsystem.
  - `ParticleComponent` creates an instance through `ParticleSubsystem` when the subsystem is available and the instance reports CPU simulation support.
  - `ParticleComponent` destroys subsystem-owned instances through `ParticleSubsystem`.
  - Component fallback without subsystem is observable and does not pretend to be the production path.
- Render integration validation
  - The `SceneRenderer` pre-graph callback seam stores callbacks by owner, rejects duplicates, invokes callbacks in the intended slot, and removes callbacks by owner.
  - The scene pass chain can contain `ParticlePass` with priority after `TransparentPass`.
  - No-particle apps keep rendering when no feature callback is registered.
- Regression gates:
  - `ParticleValidation`
  - `RenderHonestyValidation`
  - `RenderPassValidation`
  - `RenderGraphValidation`
  - `RenderSceneValidation`
  - `PipelineCacheValidation`
- Conditional visual gates when the local DX11/window path is available:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ImageCompareValidation`

## Validation Commands

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ParticleValidation RenderHonestyValidation RenderPassValidation RenderGraphValidation RenderSceneValidation PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ParticleValidation|RenderHonestyValidation|RenderPassValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|RenderSceneValidation|PipelineCacheValidation"
# Conditional local visual gate, required when the DX11/window ModelViewer path is available on this machine:
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Risks

- Pass ownership: registering `ParticlePass` into `SceneRenderer` while also keeping a `unique_ptr` in `ParticleSubsystem` would double-delete or use after free. Ownership must transfer to `SceneRenderer`; the subsystem keeps only a non-owning pointer.
- Hook timing: preparing particles before `PreparePassesForFrame()` could miss frame resources, while preparing after `BuildRenderGraph()` would miss the current graph. The callback seam must run after `PreparePassesForFrame()` and before RenderGraph clear/build.
- Module boundary: `Render` must not include or link Particle. Coordination happens by `ParticleSubsystem` registering a callback into `SceneRenderer`.
- Module dependency: `ParticleComponent` uses `Engine::Get()`, so `Particle` must explicitly depend on `RVX_Engine`. This must not be replaced with an Engine-to-Particle dependency.
- Callback lifetime: if `ParticleSubsystem::Deinitialize()` resets renderer/pass state before unregistering the callback, `SceneRenderer` could invoke a stale callback. Callback removal must happen first and be covered by a test.
- Readiness ambiguity: `SubsystemCollection` sets initialized after `Initialize()` returns. Particle rendering must use explicit readiness/status, not `ISubsystem::IsInitialized()` alone.
- Test fragility: production `RenderSubsystem` initialization may require a real window/swapchain. Tests should use existing fake-device paths where possible, validate the Render-owned callback seam directly, and use source guardrails for production dependency acquisition if a full engine test is too brittle.
- Component ownership: `ParticleComponent` fallback instances are not tracked by `ParticleSubsystem` and should not be treated as render-ready production instances.
- Over-scoping: adding visible sample particle content, soft particles, or GPU simulation in this stage would hide the core integration risk.

## Acceptance Criteria

- When `ParticleSubsystem` is registered with an initialized `RenderSubsystem`, it acquires the production RHI device without `SetDeviceForTesting()`.
- `SceneRenderer` exposes a generic pre-graph prepare callback seam that has no Particle dependency and runs after `PreparePassesForFrame()` and before RenderGraph build.
- `ParticlePass` appears in `SceneRenderer`'s pass chain once, after transparent rendering, and is owned by `SceneRenderer`.
- `ParticleSubsystem::PrepareRender()` feeds current visible instances into the registered pass through the callback before RenderGraph build.
- `ParticleSubsystem::Deinitialize()` unregisters the pre-graph callback before clearing render state, and a post-deinit callback dispatch does not touch the subsystem.
- `ParticleSubsystem` exposes explicit render-integration readiness/status and records a concrete reason when integration is unavailable.
- Non-particle scenes keep rendering because no feature callback is registered.
- `ParticleComponent` uses `ParticleSubsystem` as the production instance path and preserves a visible compatibility fallback when absent.
- ModelViewer registers `ParticleSubsystem`; conditional local smoke/golden validation passes when the DX11/window path is available.
- Focused and regression validation commands pass.
- Spark plan review: pending (`gpt-5.5 xhigh` standard).
- Spark code review: pending.
- Implementation commit: pending.
- Phase-log commit: pending.
