# RQ2b - DefaultLit Texture IBL Binding Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2a, commit `970d5bd feat(render): support cubemap texture upload foundation`

## 1. Stage Decision

RQ2b connects already-generated CPU IBL textures to the runtime DefaultLit shader path. When a scene provides GPU-ready irradiance, prefiltered environment, and BRDF LUT textures, DefaultLit samples them for diffuse/specular ambient lighting. When those resources are absent, invalid, or still uploading, the renderer must visibly fall back to the existing R9f IBL-approximate ambient constants.

This is a binding and shader-consumption stage. It does not draw the skybox, generate new IBL textures, or load a new ModelViewer environment asset.

## 2. Current State Evidence

- RQ2a made cubemap, array, and mip-chain GPU upload representable and validated across RHI copy/view contracts.
- `Resource::HDRTextureLoader::LoadIBL()` can generate CPU `environmentMap`, `irradianceMap`, `prefilteredMap`, and `brdfLUT` resources.
- `Scene::SkyboxComponent` already stores cubemap/equirectangular resources plus IBL handles: prefiltered map, irradiance map, and BRDF LUT.
- `SceneRenderer::SetupView()` currently requests uploads only for visible mesh/material textures. It does not request SkyboxComponent IBL resources.
- `DefaultLit.hlsl` currently exposes only material textures in set 2 bindings 1..5 and sampler 6. IBL is approximate through `IBLDiffuseAmbient` and `IBLSpecularAmbient` view constants.
- `PipelineCache::ValidateDefaultLitLayouts()` currently requires DefaultLit set 2 bindings 0..6 only.
- `MaterialSystem` owns set 2 descriptor creation and has default 2D material textures, but no default cubemap/BRDF IBL fallback descriptors.
- `SkyboxPass` remains a placeholder and should not be expanded in this stage.

## 3. Scope

1. Extend `ViewData` and `PipelineCache::ViewConstants` by appending an IBL texture parameter vector, without reordering existing fields:
   - `x`: texture IBL enabled, 0 or 1.
   - `y`: prefiltered mip count.
   - `z`: environment exposure/intensity multiplier.
   - `w`: reserved.
2. Extend `DefaultLit.hlsl` set 2 with:
   - `TextureCube IrradianceTexture : register(t7, space2)`
   - `TextureCube PrefilteredEnvironmentTexture : register(t8, space2)`
   - `Texture2D BRDFLUTTexture : register(t9, space2)`
   - Reuse `MaterialSampler : register(s6, space2)` for IBL sampling.
3. Add shader logic that uses texture IBL only when `IBLTextureParams.x > 0.5`; otherwise preserve the R9f approximate ambient behavior.
4. Extend `PipelineCache` DefaultLit reflection validation so set 2 bindings 7, 8, and 9 are required sampled textures.
5. Extend `MaterialSystem` set 2 descriptor creation with IBL bindings:
   - Create descriptor-complete default IBL resources: black cube for irradiance, black cube for prefiltered, and a minimal BRDF LUT fallback.
   - Track current environment IBL resource handles or a cleared fallback state.
   - Include IBL texture views and view-cache generation in the material descriptor key so descriptor cache invalidation is explicit.
   - Bind resident environment texture views when all three resources are GPU-ready; bind fallback views otherwise.
6. Add a `SceneRenderer` environment collection step after `ViewData::SetupFromCamera()` and before `PipelineCache::UpdateViewConstants()`:
   - Search active scene entities for the first enabled `SkyboxComponent` with `ContributesToLighting() == true`.
   - Request high-priority upload and mark used for irradiance, prefiltered, and BRDF LUT resources.
   - Enable texture IBL only after all required resources are GPU-ready and views can be resolved.
   - Record a visible fallback reason when resources are missing, not ready, or invalid.
7. Keep fallback honest:
   - Missing SkyboxComponent: texture IBL disabled, approximate ambient remains active.
   - Partially ready resources: texture IBL disabled, upload requests issued, approximate ambient remains active.
   - Descriptor/view creation failure: texture IBL disabled and fallback reason recorded.
8. Update focused validation tests for layout, descriptor binding, fallback behavior, and shader guardrails.

## 4. Out of Scope

- Drawing or fixing `SkyboxPass`.
- Loading a new HDRI in ModelViewer.
- Generating irradiance/prefilter/BRDF LUT on the GPU.
- Changing `HDRTextureLoader` convolution algorithms.
- Replacing the tone mapping operator.
- Changing material texture binding slots 1..6.
- Adding bindless or descriptor arrays.
- Updating visual golden images unless the fallback-free test/sample path intentionally changes pixels.

## 5. Expected Files

- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp` or `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- `PipelineCacheValidation`
  - DefaultLit reflection requires set 2 bindings 7, 8, and 9 as sampled textures.
  - `ViewConstants` layout appends IBL texture params without moving existing fields.
  - `UpdateViewConstants()` uploads disabled, enabled, and custom mip/exposure values.
  - Shader source contains the cubemap and BRDF LUT bindings and keeps the approximate fallback branch.
- `MaterialSystemValidation`
  - Default material descriptor set binds IBL fallback views at bindings 7, 8, and 9.
  - GPU-ready irradiance/prefilter/BRDF resources produce descriptor bindings for their resident views.
  - Missing or not-GPU-ready environment resources use fallback views and report fallback status.
  - Changing environment resources invalidates the descriptor cache key.
- `RenderSceneValidation` or `RenderPassValidation`
  - SceneRenderer requests uploads for SkyboxComponent IBL textures.
  - Texture IBL remains disabled until all three resources are GPU-ready.
  - Missing SkyboxComponent keeps approximate ambient enabled and records a visible fallback reason.
- Shader guardrail
  - DefaultLit does not sample IBL textures unless the texture IBL enabled flag is true.
  - Binding numbers 1..6 for existing material textures and sampler remain unchanged.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation MaterialSystemValidation RenderSceneValidation RenderPassValidation ModelViewer
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|RenderSceneValidation|RenderPassValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "GPUResourceManagerValidation|GPUUploadServiceValidation|RenderGraphValidation|RenderHonestyValidation|ClusteredLightingValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- Binding IBL in material set 2 duplicates environment descriptors per material descriptor. This is acceptable for RQ2b because it avoids a larger frame descriptor refactor. A later descriptor-layout cleanup may move environment resources to a dedicated frame/environment set.
- Shader reflection may classify cube textures differently per backend compiler. Mitigation: require sampled texture layout entries through the existing reflection validation and keep binding names in shader guardrails.
- Default cubemap fallback creation depends on the RQ2a multi-subresource upload contract. Mitigation: use a tiny black cubemap and keep upload failure as initialization failure, not a silent null descriptor.
- Partially ready resources can flicker between fallback and texture IBL if descriptor cache invalidation is wrong. Mitigation: include environment view pointers plus view generation in the descriptor key and test resource changes.
- Visual output may not change in ModelViewer until a scene/sample actually provides SkyboxComponent IBL resources. This is expected; RQ2b validates runtime binding, not sample asset authoring.

## 9. Acceptance Criteria

- DefaultLit has a real texture IBL path using irradiance, prefiltered environment, and BRDF LUT resources.
- Existing approximate ambient remains the explicit fallback when texture IBL is unavailable.
- SceneRenderer can discover SkyboxComponent IBL resources, request uploads, and enable texture IBL only when all required resources are ready.
- Material descriptor sets are complete for both fallback and resident IBL resources.
- Focused validation tests and listed regression tests pass.
- Spark plan review and Spark code review have no blockers.

## 10. Spark Review

Plan review: PASS (`Carson`, `gpt-5.3-codex-spark`)

- Non-blocking notes adopted for implementation:
  - Keep set 2 binding slots 7, 8, and 9 collision-free against shader reflection.
  - Include both fallback/live state and view generation in the material descriptor cache key.

Code review: pending
Commit message: `feat(render): bind texture IBL resources for default lit`
