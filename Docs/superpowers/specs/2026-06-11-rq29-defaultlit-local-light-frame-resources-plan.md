# RQ29 - DefaultLit Local Light Frame Resources

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Previous stage: RQ28 - Directional Light Color

## Stage Decision

Connect the existing scene light infrastructure to the real `DefaultLit` material path so the main renderer can shade bounded point and spot lights in addition to the selected directional light.

This stage intentionally does not implement clustered tile indexing, local-light shadows, editor light tooling, or a new material shader. It turns the already-present `RenderScene` -> `LightManager` GPU light buffers into frame resources consumed by `DefaultLit`.

Before implementation starts, read this document and execute only the tasks in this stage. Spark plan review must pass first. After implementation, Spark code review must pass before commit.

## Current Evidence

- `RenderScene` already stores `RenderLight` entries with `Directional`, `Point`, and `Spot` types.
- `LightManager` already collects `RenderScene` lights, owns `LightConstants`, point-light, and spot-light GPU buffers, and can upload them.
- `Lighting.hlsli` already defines GPU-side `DirectionalLight`, `PointLight`, `SpotLight`, `EvaluatePointLight`, and `EvaluateSpotLight`.
- `PBRLit.hlsl` contains an older multi-light shader path, but it does not match the current `DefaultLit` descriptor spaces, material bindings, IBL, and shadow path.
- The active draw path is `DefaultLit.hlsl`, which currently shades only `DirectionalLightColor * DirectionalLightIntensity`.
- `DefaultFrameDescriptorSet` currently binds only view constants plus directional shadow texture/sampler.
- `OpaquePass` and `TransparentPass` both bind `DefaultLit` frame/object/material descriptor sets. Neither pass updates or binds local-light frame resources.
- DX11 shader reflection does not reliably preserve HLSL register spaces for the current `DefaultLit` contract, so its descriptor layout must be stabilized explicitly rather than silently accepting lossy reflection output.

## Scope

1. Add `DefaultLit` frame-set bindings for:
   - light constants buffer: `cbuffer LightConstants : register(b3, space0)`
   - point lights read-only structured buffer: `StructuredBuffer<PointLight> PointLights : register(t4, space0)`
   - spot lights read-only structured buffer: `StructuredBuffer<SpotLight> SpotLights : register(t5, space0)`
   These binding numbers are part of the stage contract and must be pinned by validation. Existing set 0 bindings remain unchanged: `b0` view constants, `t1` directional shadow texture, and `s2` directional shadow sampler.
   The read-only structured buffers must use an SRV-compatible RHI binding type (`ShaderResourceBuffer`), not a UAV/storage binding. DX12/DX11 bind them as SRV/t-registers; Vulkan/OpenGL may still map the RHI type to storage-buffer/SSBO descriptors as required by those APIs.
2. Add durable empty fallback resources so the frame descriptor set is complete even when a scene has no local lights or a pass has no `LightManager`.
3. Add a `PipelineCache` frame-light update API that binds the provided light buffers or the empty fallbacks and records the last binding result.
4. Wire a `LightManager` into `SceneRenderer` frame setup and pass resources:
   - collect lights from `RenderScene`
   - upload GPU buffers before scene draw passes
   - pass the manager to `OpaquePass` and `TransparentPass`
5. Update `OpaquePass` and `TransparentPass` to refresh light frame resources before binding `DefaultFrameDescriptorSet`.
6. Update `DefaultLit.hlsl` to include `Lighting.hlsli`, declare the new bindings in `space0`, and accumulate bounded point and spot light contribution using the existing PBR helper functions.
7. Add focused validation for descriptor layout, fallback binding, shader source guardrails, and pass integration.
8. Keep the DX11 `DefaultLit` descriptor layout on the explicit set 0/1/2 contract while non-DX11 backends continue to validate reflected layouts plus the new local-light bindings.

## Out of Scope

- Clustered light assignment consumption in the shader.
- Tile/cluster light list indexing.
- Point or spot shadow maps.
- Volumetric integration with local lights.
- Replacing `DefaultLit` with the old `PBRLit` shader.
- Changing the existing IBL, shadow, bloom, or tone mapping behavior.
- Visual golden recapture unless the focused visual gate proves a deterministic change in an existing fixture.

## Expected Files

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/OpaquePass.h`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Include/Render/Passes/TransparentPass.h`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Lighting/LightManager.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Render/Shaders/Include/Lighting.hlsli`
- `RHI/Include/RHI/RHIDefinitions.h`
- `RHI/Include/RHI/RHIDescriptor.h`
- `RHI_DX11/Private/DX11Pipeline.cpp`
- `RHI_DX12/Private/DX12Pipeline.cpp`
- `RHI_Vulkan/Private/VulkanCommon.h`
- `RHI_Vulkan/Private/VulkanPipeline.cpp`
- `RHI_OpenGL/Private/OpenGLDescriptor.cpp`
- `RHI_OpenGL/Private/OpenGLPipeline.cpp`
- `ShaderCompiler/Private/ShaderLayout.cpp`
- `ShaderCompiler/Private/ShaderReflection.cpp`
- `ShaderCompiler/Private/DX11SlotMapper.cpp`
- `ShaderCompiler/Private/DXCCompiler.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## Required Tests

- `PipelineCacheValidation`
  - `DefaultLit` reflection includes set 0 bindings for light constants, point lights, and spot lights with the expected binding types.
  - Point/spot light frame bindings are `ShaderResourceBuffer`/SRV-compatible bindings, not `StorageBuffer` UAV bindings.
  - DX11 builds the same `DefaultLit` set 0/1/2 contract explicitly, including local-light bindings, without relying on lossy register-space reflection.
  - `DefaultFrameDescriptorSet` creation binds fallback light buffers.
  - Updating frame-light resources binds real light buffers when provided and fallback buffers when missing.
  - Updating frame-light resources does not drop or invalidate existing set 0 bindings: `b0` view constants, `t1` directional shadow texture, and `s2` directional shadow sampler.
  - Updating directional-shadow resources does not drop or invalidate the local-light bindings.
  - `DefaultLit.hlsl` source guardrails prove local light declarations and loops are present.
  - `LightManager` point/spot GPU buffers are created with structured-buffer usage and valid strides.
- `SceneRenderer` integration guardrails
  - Source or behavior tests prove `SceneRenderer` owns and initializes a `LightManager`.
  - Per-frame preparation collects lights from `RenderScene` into `LightManager`.
  - Per-frame preparation calls `LightManager::UpdateGPUBuffers()`.
  - Pass setup passes the same `LightManager` to both `OpaquePass` and `TransparentPass`.
- `RenderPassValidation`
  - `OpaquePass` updates frame-light resources from a `LightManager` before drawing.
  - `TransparentPass` also updates frame-light resources while preserving the existing directional-shadow disabled/fallback behavior and keeping local lights bound.
- Regression gates:
  - `RenderGraphValidation`
  - `RenderHonestyValidation`
  - `RenderSceneValidation`
  - `RenderPassValidation`
  - `MaterialSystemValidation`
  - `PipelineCacheValidation`
  - `ClusteredLightingValidation`
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`

## Validation Commands

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|MaterialSystemValidation|ClusteredLightingValidation"
git diff --check
```

## Risks

- Descriptor drift: adding `StructuredBuffer` declarations changes the reflected frame layout and must be validated.
- Unbound local-light buffers: no-light scenes must still have valid fallback buffers.
- Register-space collision: local-light bindings are fixed to `b3/t4/t5, space0`; validation must reject drift away from these binding numbers or types.
- SRV/UAV mismatch: HLSL `StructuredBuffer` uses t-register SRVs, so the RHI layout must not label point/spot buffers as UAV-style storage buffers.
- DX11 reflection drift: `DefaultLit` uses an explicit descriptor contract on DX11; tests must prove the contract still contains local-light resources.
- Transparent pass drift: it must continue disabling directional shadow sampling while still binding local lights.
- Visual deltas: existing goldens may not change if fixtures have no local lights; if any deterministic fixture includes local lights, recapture only with evidence and a Spark amendment.

## Acceptance Criteria

- `DefaultLit` consumes point and spot light GPU buffers in the active material path.
- Opaque and transparent scene draws have complete frame resources for local lights.
- Empty or missing light resources fall back visibly and safely instead of leaving descriptors unbound.
- `SceneRenderer` is proven to collect, upload, and pass local light resources through the main render path.
- Frame descriptor updates preserve both existing view/shadow resources and new local-light resources.
- Focused and regression validation commands pass.
- Spark plan review: PASS (Mill/Spark, gpt-5.5 xhigh standard, 2026-06-11).
- Spark code review: PASS (Darwin/Spark, gpt-5.5 xhigh standard, 2026-06-11).
- Implementation commit: `a1c56ef feat(render): bind local lights in default lit`.
- Phase-log commit: pending.
