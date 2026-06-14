# RQ32 - Soft Particle Depth Fade

Date: 2026-06-14
Program: Render Quality & Verification Program v2
Previous stage: RQ31 - Particle Main-Frame Integration

## 1. Stage Decision

Implement soft-particle depth fading for the existing CPU billboard particle path.

RQ30 made CPU billboard particle simulation and drawing real. RQ31 connected that path to the production frame. RQ32 should now remove the next visible-quality lie: `ParticleSystem::softParticleConfig` exists and defaults to enabled, `ParticlePass` already has access to the scene depth handle, and `ParticleBillboard.hlsl` already contains a soft-particle hook, but `ParticleRenderer` currently discards the depth texture and forces soft particles off.

This stage is intentionally independent from ECS, GPU simulation, trails, mesh particles, and particle texture/material loading.

## 2. Execution Rules

Before implementation:

- Re-read this document and confirm the RQ32 task list, scope, and acceptance criteria.
- Run Spark plan review with model `gpt-5.5`, reasoning effort `xhigh`.
- Do not edit implementation files until Spark plan review returns PASS or all blockers are fixed in this document.

During implementation:

- Keep edits scoped to RQ32 files.
- Do not revert unrelated dirty files.
- Preserve Render -> Particle independence.
- Any fallback must be visible through status/test evidence, not silent.

After implementation:

- Run focused and regression validation listed in this document.
- Run Spark code review with model `gpt-5.5`, reasoning effort `xhigh`.
- Fix all Spark blockers before commit.
- Commit implementation first.
- Append RQ32 phase-log evidence and commit the phase log separately.

## 3. Current State Evidence

- `Particle/Private/Rendering/ParticleRenderer.cpp:268` and `:353` ignore the `depthTexture` argument.
- `Particle/Private/Rendering/ParticleRenderer.cpp:312` and `:388` force `softConfig.enabled = false`.
- `Particle/Private/Rendering/ParticleRenderer.cpp:564` creates a descriptor layout with only render constants, particle buffer, alive list, particle texture, and sampler.
- `Particle/Private/Rendering/ParticleRenderer.cpp:680` rejects soft billboard pipeline requests.
- `Particle/Shaders/ParticleBillboard.hlsl:7` and `:105` gate soft-particle code behind `RVX_PARTICLE_ENABLE_SOFT_PARTICLES`, but the production compile path does not enable that path.
- `Particle/Shaders/Include/SoftParticle.hlsli` declares conflicting legacy bindings (`t1`, `s1`, `b2`) and a separate constant buffer instead of using the existing `RenderData`.
- `Particle/Private/Rendering/ParticlePass.cpp:112` marks depth read-only as a depth attachment and `:152` resolves the depth texture, but it does not request a shader-readable depth SRV.
- `Render/Private/Renderer/SceneRenderer.cpp:896` creates the main scene depth texture as `RHITextureUsage::DepthStencil` only, while `ResourceViewCache::GetDefaultSRV()` requires `ShaderResource` usage.
- `Particle/Include/Particle/Rendering/SoftParticleConfig.h:18` and `Particle/Include/Particle/ParticleSystem.h:157` expose system-level soft particle config that is currently not honored by rendering.

## 4. Scope

1. Make scene depth sampleable.
   - Change the main `SceneRenderer` depth buffer usage to `DepthStencil | ShaderResource` or the equivalent `RHITextureDesc::DepthStencil(...)` helper.
   - Keep the existing DSV path and depth prepass/opaque/transparent behavior unchanged.
   - Add validation that the created scene depth description includes both depth-stencil and shader-resource usage.
   - When `SceneRenderer` imports the persistent scene depth texture into the RenderGraph, set an explicit export state back to `RHIResourceState::DepthWrite` so the existing `m_depthBufferState = DepthWrite` bookkeeping after execution remains true even if a particle shader-read path uses the texture during the graph.

2. Do not bind the same depth resource as DSV and SRV in the same particle render pass.
   - Current RenderGraph and RHI states are single-valued; RQ32 must not pretend they can express simultaneous read-only DSV plus shader SRV for the same texture.
   - Do not add backend read-only DSV/SRV support in RQ32.
   - Use two honest paths instead:
     - Depth-SRV path: when a shader-resource view for scene depth is available, `ParticlePass` renders particles in a color-only render pass and `ParticleBillboard.hlsl` performs shader depth occlusion plus optional soft fade.
     - Fixed-function fallback path: when depth SRV is unavailable, `ParticlePass` keeps the existing read-only DSV depth-test path, disables soft fade for those draws, and records a visible fallback reason.
   - This avoids simultaneous DSV/SRV binding while preserving hard-particle depth testing as the no-SRV fallback.

3. Split particle pipelines by depth strategy.
   - Keep the existing fixed-function depth-test billboard pipeline for the DSV fallback path.
   - Add/select a depth-SRV shader-depth pipeline for the depth-SRV path:
     - `RHIDepthStencilState::Disabled()`,
     - `depthStencilFormat = RHIFormat::Unknown`,
     - same color target format, blend mode, shaders, and descriptor layout.
   - Extend the pipeline key so shader-depth/no-depth and fixed-function-depth variants cannot collide.
   - `ParticleRenderer` must select the no-depth pipeline whenever a real depth SRV is bound for shader depth testing.
   - Add tests asserting the depth-SRV path creates/uses an unknown-depth-format pipeline with depth test/write disabled.

4. Align particle fallback depth format with production scene depth.
   - Change `ParticleRendererConfig::depthStencilFormat` default from `D24_UNORM_S8_UINT` to `PipelineCache::GetDefaultDepthStencilFormat()` if including `PipelineCache` in the public header is acceptable; otherwise use `RHIFormat::D32_FLOAT` with a comment tying it to the production default.
   - In production `ParticleSubsystem` initialization, populate the renderer config with the scene renderer/pipeline default depth format instead of relying on the old D24 default.
   - Focused tests should use `D32_FLOAT` scene depth for pass/pipeline coverage, with a separate invalid-depth-format test retained.
   - The DSV fallback pipeline depth format must match the active scene depth format.

5. Add soft-particle descriptor resources.
   - Extend the particle billboard descriptor layout with unique bindings for scene depth SRV and depth sampler.
   - Suggested binding map:
     - `b0`: `RenderData`
     - `t1`: particle buffer
     - `t2`: alive index buffer
     - `t3`: particle texture
     - `s4`: particle sampler
     - `t5`: scene depth texture
     - `s6`: depth sampler
   - Create a fallback depth texture/SRV so descriptor creation remains valid when the system has soft particles disabled or a frame has no depth texture.
   - Have `ParticlePass` resolve a real depth SRV through `ResourceViewCache::GetDefaultSRV()` and pass that view to `ParticleRenderer`.
   - Bind the real scene depth SRV only when the SRV can be acquired and the draw is using the depth-SRV path.
   - Bind the fallback depth SRV only when no real scene-depth SRV is bound. A soft-disabled system with a valid scene-depth SRV must still bind the real scene depth for shader-side hard occlusion.

6. Honor `SoftParticleConfig` honestly.
   - Enable soft fade only when:
     - the particle system is billboard mode,
     - `system->softParticleConfig.enabled` is true,
     - the pass is using the depth-SRV path,
     - a real scene-depth SRV is bound.
   - Otherwise draw normal hard-edge billboards and record/return an observable soft-particle disabled reason through renderer stats or a test-visible accessor.
   - Keep particle rendering supported if soft particles cannot be used for a specific draw; lack of depth must not disable all billboards.
   - In the depth-SRV path, use shader-side scene-depth occlusion for both hard and soft billboard draws because no fixed-function DSV is bound in that path.
   - Valid scene-depth SRV plus `softParticleConfig.enabled == false` must still use the depth-SRV path, open a color-only pass, bind the real depth SRV, upload `softParticleEnabled = 0`, and upload `sceneDepthTestEnabled = 1`.

7. Fix shader bindings and fade math.
   - Update `SoftParticle.hlsli` to use non-conflicting bindings (`t5`, `s6`) and the existing `RenderData` fields.
   - Avoid a separate `b2` constant buffer.
   - Add `nearPlane`, `farPlane`, `reverseZ`, and `sceneDepthTestEnabled` fields to `RenderGPUData` in `Particle/Include/Particle/ParticleTypes.h` and the matching HLSL `RenderData` in `Particle/Shaders/Include/ParticleCommon.hlsli`.
   - Add CPU/HLSL layout guardrails:
     - `sizeof(RenderGPUData)` remains 16-byte aligned,
     - tests/source guards prove the CPU and HLSL field names exist in both layouts.
   - Use `ViewData::nearPlane`, `farPlane`, and `ParticleRendererConfig::reverseZ` through `RenderData` so depth linearization is not hard-coded to `0.1 / 1000`.
   - Ensure `ParticleBillboard.hlsl` compiles with the soft-particle resources declared in the default path.
   - Keep hard-edge behavior when `softParticleEnabled == 0`.

8. Update `ParticlePass` depth-read semantics.
   - Determine whether a depth-SRV path is available by checking the depth handle, graph texture, texture usage, and `ResourceViewCache::GetDefaultSRV()`.
   - If depth-SRV path is available:
     - declare the depth handle with `builder.Read(depth, RHIShaderStage::Pixel)`,
     - do not call `builder.SetDepthStencil()` for the particle draw,
     - open a color-only render pass,
     - draw using the depth-sampling particle pipeline,
     - keep shader hard-depth occlusion enabled even when soft fade is disabled for a particle system.
   - If depth-SRV path is unavailable:
     - keep the existing `builder.SetDepthStencil(depth, false, false)` and read-only DSV render pass,
     - pass no real depth SRV to `ParticleRenderer`,
     - force soft fade off and record the fallback reason.
   - Do not declare both `builder.Read(depth)` and `builder.SetDepthStencil(depth, false, false)` for the same pass in RQ32.

9. Add focused tests.
   - Shader guardrails:
     - no legacy soft bindings `t1/s1/b2` inside `SoftParticle.hlsli`,
     - billboard shader declares/uses `t5/s6`,
     - no hard-coded near/far constants in `ComputeSoftParticleFade` call.
   - `RenderGPUData` layout remains 16-byte aligned and source guardrails match new CPU/HLSL depth fields.
   - Descriptor layout includes seven bindings with expected types/stages.
   - Depth-SRV draw path uses a pipeline whose `depthStencilFormat == RHIFormat::Unknown` and whose depth test/write are disabled.
   - DSV fallback path uses a pipeline whose depth format matches `PipelineCache::GetDefaultDepthStencilFormat()` / active scene depth format.
   - Soft-enabled draw with a valid depth SRV binds the real scene depth SRV and uploads `softParticleEnabled == 1`, `sceneDepthTestEnabled == 1`.
   - Soft-disabled draw with a valid depth SRV binds the real scene depth SRV, opens no DSV, and uploads `softParticleEnabled == 0`, `sceneDepthTestEnabled == 1`.
   - Missing or unavailable depth SRV draws successfully with fallback depth binding, `sceneDepthTestEnabled == 0`, `softParticleEnabled == 0`, visible fallback reason, and DSV path when a depth target exists.
   - `ParticlePass` depth-SRV path declares shader read, opens a color-only render pass, and does not bind a DSV.
   - `ParticlePass` no-SRV fallback path keeps the existing read-only DSV behavior and disables soft fade.
   - Shader/source guard proves `sceneDepthTestEnabled` performs hard occlusion independently from `softParticleEnabled`.
   - SceneRenderer depth texture source guard or focused test proves the main depth texture is shader-readable and exported back to `DepthWrite`.

## 5. Out of Scope

- GPU particle simulation.
- Particle sorting improvements beyond existing CPU/GPU hooks.
- Texture-sheet animation, particle material/texture asset loading, and flipbook blending.
- Stretched billboard, trail, mesh, and light particle rendering.
- Volumetric particles and lit particle shading.
- Any ECS or Object-model refactor.

## 6. Expected Files

- `Render/Private/Renderer/SceneRenderer.cpp`
- `Particle/Include/Particle/ParticleTypes.h`
- `Particle/Include/Particle/Rendering/ParticleRenderer.h`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Particle/Private/Rendering/ParticlePass.cpp`
- `Particle/Private/ParticleSubsystem.cpp`
- `Particle/Shaders/ParticleBillboard.hlsl`
- `Particle/Shaders/Include/SoftParticle.hlsli`
- `Particle/Shaders/Include/ParticleCommon.hlsli`
- `Tests/ParticleValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md` after implementation commit

## 7. Validation Plan

Build:

```powershell
cmake --build build\win_x64_debug --config Debug --target ParticleValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
```

Focused:

```powershell
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ParticleValidation|RenderPassValidation"
```

Regression:

```powershell
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderSceneValidation"
```

Visual gate:

```powershell
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
```

Diff hygiene:

```powershell
git diff --check
```

## 8. Risks

- Some backends may reject sampling a depth texture that was not created with shader-resource usage. RQ32 fixes the main scene depth usage and requires visible fallback when SRV acquisition fails.
- Current RenderGraph/RHI cannot safely express simultaneous DSV and SRV binding for the same texture. RQ32 explicitly avoids that by using a color-only shader-depth path when depth SRV is active and the existing DSV path only when depth SRV is unavailable.
- Color-only particle passes cannot use a graphics pipeline that declares a depth-stencil format. RQ32 must create a depth-disabled pipeline variant for shader-depth draws.
- Fallback fixed-function depth testing is invalid if the particle pipeline depth format does not match the scene depth format. RQ32 aligns defaults and production config to the scene depth format.
- The persistent scene depth texture is imported from external state each frame. RQ32 must export it back to `DepthWrite` so `SceneRenderer`'s existing state bookkeeping remains honest across frames.
- The shader currently has legacy soft-particle bindings that conflict with particle buffers. RQ32 must remove those conflicts before enabling the path.
- Hard-coded near/far values would make fade quality camera-dependent and incorrect. Use view/config values instead.
- Missing depth must degrade to hard billboards, not to skipped particles or false soft-particle success.

## 9. Acceptance Criteria

- CPU billboard particles honor `ParticleSystem::softParticleConfig` when a sampleable depth texture is available.
- Soft particles use unique, non-conflicting descriptor bindings.
- RQ32 never binds the same scene-depth texture as both DSV and SRV in the same particle render pass.
- Depth-SRV draws use a depth-disabled/unknown-depth pipeline; DSV fallback draws use a depth format matching the active scene depth.
- Missing or unavailable depth SRV falls back visibly to hard billboard particles without disabling normal particle rendering.
- Main scene depth is created shader-readable for downstream effects and exported back to `DepthWrite`.
- `ParticleValidation`, focused render pass validation, render graph/scene regression, and conditional ModelViewer visual gates pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before implementation commit.

## 10. Spark Review

- Plan review: pending
- Code review: pending
- Implementation commit: pending
- Phase-log commit: pending
