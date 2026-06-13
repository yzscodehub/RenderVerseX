# RQ30 - CPU Billboard Particle Render Bridge

Date: 2026-06-13
Program: Render Quality & Verification Program v2
Previous stage: RQ29 - DefaultLit Local Light Frame Resources

## Stage Decision

Make the Particle module internally honest and renderable by completing a minimal CPU-simulated billboard particle path:

`ParticleSubsystem / ParticleSystemInstance`
-> `CPUParticleSimulator`
-> GPU particle/alive buffers
-> `ParticleRenderer` billboard pipeline and descriptor set
-> `ParticlePass` color target draw.

This stage does not attempt a full Niagara-class feature set. It removes the current hard-coded unsupported state for the CPU billboard path and proves that a simple particle system can emit, simulate, upload, and issue a draw through the render pass.

Before implementation starts, read this document and execute only the tasks in this stage. Spark plan review must pass first. After implementation, Spark code review must pass before commit.

## Current Evidence

- `RenderHonestyValidation` currently asserts that particle simulation and rendering are disconnected:
  - `ParticleRenderingAndSimulationExposeDisconnectedState`
  - `ParticleRendererDrawsReturnFalseWhenUnsupported`
  - `ParticlePassUnsupportedSetupDeclaresNoGraphResources`
- `ParticleSubsystem::CreateInstance()` explicitly calls `SetSimulationUnsupported("ParticleSubsystem does not connect GPU/CPU simulators to instances yet")`.
- `ParticleSystemInstance::UpdateEmission()` has the simulator emission call commented out.
- `ParticleSystemInstance::Simulate()` has the simulator simulation and alive-count update commented out.
- `CPUParticleSimulator` already owns uploadable `GPUParticle`, alive-index, and indirect draw buffers and `PrepareRender()` uploads them.
- `IEmitter::GetEmitParams()` already produces `EmitterGPUData` for emitter shapes.
- `ParticleRenderer::Initialize()` creates quad buffers and render constants but leaves `m_renderingSupported = false` with reason `"Particle render pipelines are not implemented"`.
- `ParticleRenderer::CreatePipelineIfNeeded()` is a TODO that always returns `nullptr`.
- `ParticleBillboard.hlsl` exists, but its current register layout (`b0`, `t0`, `t1`, `t2`, `s0`) collides inside the current RHI descriptor-set model because layout entries cannot share the same binding number across CBV/SRV/sampler classes.
- `ParticleBillboard.hlsl` also unconditionally includes `SoftParticle.hlsli`, whose depth resources declare additional `t1/s1/b2` bindings. Since soft particles are out of scope for RQ30, this include/call path must be removed or compiled out before the billboard descriptor layout can be trusted.
- `ParticleRenderer::CreateQuadBuffers()` currently uploads `uint32` quad indices but `DrawParticles()` binds the index buffer as `RHIFormat::R16_UINT`, so a real draw can submit while reading the wrong index element size.
- `ParticlePass` already groups visible instances, has priority after transparent rendering, and declares color/depth resources only when `IsEnabled()` becomes true.
- `ParticleSubsystem::Initialize()` currently has no real device acquisition path; the comment that would pull a `RenderSubsystem` device is disabled. Tests and samples need an explicit device injection path until engine-level subsystem wiring is added.
- `ParticleSystemInstance` has `SetSimulationUnsupported()` but no API to attach an actual simulator, so the subsystem cannot currently connect a CPU simulator without new instance API.

## Scope

1. Connect CPU simulation for particle instances.
   - Add a `ParticleSystemInstance::SetSimulator(std::unique_ptr<IParticleSimulator>, const char* backendName)` or equivalent attach API that clears the unsupported reason when the simulator is initialized.
   - Add an explicit `ParticleSubsystem::SetDeviceForTesting(IRHIDevice*)` or initialization/config path for this stage so tests can provide a device without relying on the currently-commented engine dependency.
   - Add a supported CPU simulation path to `ParticleSubsystem::CreateInstance()` when an RHI device is present.
   - Initialize `CPUParticleSimulator` with the instance max particle count.
   - Keep GPU simulation explicitly unsupported for this stage.
   - `ParticleSystemInstance::Clear()` must call `m_simulator->Clear()`.
   - `ParticleSystemInstance::UpdateEmission()` must call `IEmitter::GetEmitParams()`, compose emitter transform with the instance transform, and call `m_simulator->Emit()`.
   - `ParticleSystemInstance::Simulate()` must call `m_simulator->Simulate()` with basic `SimulationGPUData`, then update `m_aliveCount`.
2. Make CPU billboard rendering supported.
   - Replace the hard-coded unsupported renderer state with a capability result based on real resources.
   - Add `ParticleRendererConfig` or explicit initialization parameters containing:
     - shader directory or compiled shader injection path
     - color target format
     - depth format
     - sample count
     - reverse-Z/depth-test policy if needed
   - Add the required `ShaderCompiler` dependency to the Particle module, or explicitly inject precompiled shaders in tests. The implementation must not rely on an unstated global shader manager.
   - Compile/create a billboard particle pipeline for `ParticleRenderMode::Billboard`.
   - Support at least `AlphaBlend` and `Additive` blend modes.
   - Unsupported modes (`Mesh`, `Trail`, `StretchedBillboard`, unsupported blend variants if any) must remain visibly unsupported for this stage, not silently claim success.
3. Fix particle shader descriptor bindings for the RHI contract.
   - Move `ParticleBillboard.hlsl` to unique set-0 binding numbers, for example:
     - `b0` render constants
     - `t1` particle buffer
     - `t2` alive index buffer
     - `t3` particle texture/fallback white texture
     - `s4` sampler
   - Point/alive buffers must use `ShaderResourceBuffer` layout semantics, not UAV-style `StorageBuffer`.
   - Since soft particles are out of scope, remove or macro-guard the `SoftParticle.hlsli` include and `ComputeSoftParticleFade()` call in the default RQ30 billboard shader path.
   - Add a source/reflection guard proving no duplicate set-0 binding numbers remain in the particle billboard layout.
4. Add renderer descriptor resources.
   - Create a particle descriptor set layout and descriptor set for the billboard path.
   - Bind render constants, particle buffer, alive index buffer, fallback white texture, and sampler before drawing.
   - Use fallback texture/sampler when the particle system has no material/texture resource.
   - Update descriptor bindings per draw or per instance so each instance uses its current simulator buffers.
   - Fix quad index buffer element type and binding format so they match (`uint16` with `R16_UINT`, or `uint32` with `R32_UINT`).
5. Keep `ParticlePass` honest.
   - Once the renderer and visible CPU billboard instances are available, `ParticlePass::Setup()` must declare color target read/write and optional depth read.
   - `ParticlePass::Execute()` must resolve `view.colorTarget` and optional `view.depthTarget` to actual target views, create an `RHIRenderPassDesc`, call `BeginRenderPass()`, set viewport/scissor, call `PrepareRender()`, draw CPU billboard instances, and call `EndRenderPass()`.
   - Draw commands must happen inside the render pass attachment scope, not merely after RenderGraph dependency declaration.
   - Failed per-instance draw should be visible in return/log/test state, not silently ignored as success.
6. Update tests from unsupported-stub expectations to CPU billboard support expectations.

## Out of Scope

- GPU compute emission/simulation.
- GPU indirect draw as the required path.
- Mesh particles.
- Trail renderer completion.
- Stretched billboard velocity stretching.
- Soft-particle depth sampling quality and depth texture descriptor binding.
- Texture asset loading for particle materials.
- Engine-level auto-registration of `ParticlePass` into the main `SceneRenderer`/ModelViewer frame. That should be the next stage after the module path is renderable.
- 3A particle features such as flipbook animation, light emission, vector fields, collision against scene depth, ribbon trails, motion-vector output, or volumetric particles.

## Expected Files

- `Docs/superpowers/specs/2026-06-13-rq30-cpu-billboard-particle-render-bridge-plan.md`
- `Particle/CMakeLists.txt`
- `Particle/Include/Particle/ParticleSystemInstance.h`
- `Particle/Private/ParticleSystemInstance.cpp`
- `Particle/Private/ParticleSubsystem.cpp`
- `Particle/Include/Particle/Rendering/ParticleRenderer.h`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Particle/Private/Rendering/ParticlePass.cpp`
- `Particle/Shaders/ParticleBillboard.hlsl`
- `Particle/Shaders/Include/SoftParticle.hlsli` only if macro-guarding shared declarations is cleaner than removing the include from billboard
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/RenderPassValidation/main.cpp` or a new focused `ParticleValidation` target if linking `Particle` into `RenderPassValidation` is too broad
- `Docs/superpowers/specs/phase-log.md`

## Required Tests

- `RenderHonestyValidation`
  - Particle instances created through `ParticleSubsystem` with a device get a CPU simulator and no longer report the old disconnected-simulation reason.
  - `ParticleSubsystem` exposes an explicit test/device injection path until engine-level RenderSubsystem dependency wiring is implemented.
  - `ParticleSystemInstance::SetSimulator()` marks simulation supported and `Clear()` clears the attached simulator.
  - CPU particle simulation emits particles from a simple emitter and updates `aliveCount`.
  - `CPUParticleSimulator::PrepareRender()` exposes non-null particle/alive buffers after emission.
  - `ParticleRenderer` no longer reports `"Particle render pipelines are not implemented"` when the billboard pipeline resources can be created.
  - `ParticleRendererConfig` validation rejects missing shader directory, unknown render target format, or missing device with visible reasons.
  - `ParticleBillboard.hlsl` source guardrails prove unique binding numbers, `StructuredBuffer` declarations, and no unconditional soft-particle resource declarations in the RQ30 path.
  - Unsupported GPU simulation remains visibly unsupported until compute pipelines are connected.
- `RenderPassValidation` or a focused particle validation
  - `ParticlePass::Setup()` declares color target read/write when given a visible CPU billboard instance and supported renderer.
  - Empty/no-alive particle pass declares no graph work.
  - `ParticlePass::Execute()` calls the renderer for CPU billboard instances.
  - `ParticlePass::Execute()` begins a render pass using the resolved graph color target and optional depth target before any particle draw.
  - The fake command context records `BeginRenderPass`, viewport/scissor setup, `SetDescriptorSet(0)`, `SetPipeline()`, `DrawIndexed()`, and `EndRenderPass` in the expected order.
  - Render pass attachment formats match the `ParticleRendererConfig` color/depth formats used to create the pipeline.
  - `SetIndexBuffer()` records an index format that matches the quad index buffer element type.
  - The captured descriptor set contains render constants, particle buffer, alive-index buffer, fallback texture view, and sampler.
  - `DrawIndexed` instance count equals the simulator alive count.
  - Unsupported render modes/blends do not report successful draws.
- Regression gates:
  - `RenderHonestyValidation`
  - `RenderPassValidation`
  - `RenderGraphValidation`
  - `RenderSceneValidation`
  - `PipelineCacheValidation`
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`

## Validation Commands

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target RenderHonestyValidation RenderPassValidation RenderGraphValidation RenderSceneValidation PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir $B -C Debug --output-on-failure -R "RenderHonestyValidation|RenderPassValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|RenderSceneValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

If a new focused `ParticleValidation` target is introduced instead of extending `RenderPassValidation`, add it to the build command and the focused ctest regex before implementation is considered complete.

## Risks

- Shader binding collision: the existing particle shader register layout and soft-particle include are not valid for the current RHI descriptor-set model. This must be fixed before pipeline creation is considered complete.
- Index format mismatch: the existing quad indices are `uint32` but are bound as `R16_UINT`. The index data type and RHI binding format must match before any draw is considered valid.
- Pipeline format ownership: `ParticleRenderer` must take explicit render target/depth formats through config; guessing formats can create a pipeline that does not match the active target.
- Dependency creep: adding ShaderCompiler to Particle is acceptable for this stage only if it stays local to particle pipeline creation and does not make Render depend on Particle.
- Over-scoping: trying to complete GPU simulation, soft particles, trails, and engine-level registration in the same stage would make the first honest path too risky.
- Fake success: returning true from `DrawParticles()` without a real pipeline, descriptor set, particle buffers, and draw command would recreate the same problem this stage is meant to remove.
- Module boundary: `Render` cannot depend on `Particle`; engine-level pass registration must happen outside `Render` in a follow-up.
- Render target binding: declaring RenderGraph read/write dependencies is not enough. `ParticlePass::Execute()` must bind the resolved color/depth attachments in a real render pass before issuing draw calls.

## Acceptance Criteria

- A simple particle system can create a CPU simulator, emit particles, simulate them, upload render buffers, and reach a supported billboard renderer path.
- Particle renderer initialization is driven by an explicit config that includes shader location and render target/depth formats.
- `ParticleRenderer::DrawParticles()` issues a real draw for CPU billboard particles when pipeline/descriptors/resources are available.
- The quad index buffer data type matches the `SetIndexBuffer()` format used for particle draws.
- The particle billboard shader has unique descriptor binding numbers and no unconditional out-of-scope soft-particle resources.
- `ParticlePass` declares graph work and executes draws inside a real render pass over the resolved color/depth targets for visible CPU billboard instances.
- Old disconnected particle honesty tests are replaced with support-path tests plus explicit unsupported tests for out-of-scope modes.
- Focused and regression validation commands pass.
- Spark plan review: PASS (Dalton/Spark, gpt-5.5 xhigh standard, 2026-06-13).
- Spark code review: pending.
- Implementation commit: pending.
- Phase-log commit: pending.
