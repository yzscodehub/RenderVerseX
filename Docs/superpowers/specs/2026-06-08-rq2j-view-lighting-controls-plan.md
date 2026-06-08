# RQ2j - View Lighting Controls and IBL Ambient Floor Split Plan

Date: 2026-06-08
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2i, commit `76977a6 feat(samples): generate procedural model viewer ibl with hdr pipeline`

## 1. Current Rendering Module Analysis

Recent RQ2 stages have connected the important render quality spine:

- `SceneRenderer` owns the runtime pass chain, HDR scene color policy, post-process setup, texture IBL discovery, and proxy-preferred scene collection.
- `PipelineCache` owns `ViewConstants`, default lit pipeline layouts, post-process/skybox pipeline variants, and shader guardrails through validation tests.
- `MaterialSystem` owns material constants, texture descriptors, fallback descriptors, and live texture IBL bindings.
- `GPUResourceManager` now uploads cubemaps, mipped cubemaps, texture arrays, and glTF PBR textures with explicit residency semantics.
- `HDRTextureLoader` now supplies CPU-generated IBL maps for explicit HDRI and default ModelViewer procedural IBL.
- `ModelViewer` is now the primary visual smoke surface for DamagedHelmet, procedural skybox, texture IBL, HDRI, and golden no-IBL stability.

The next highest-value render improvements are:

1. **View lighting controls / ambient honesty**: `PipelineCache::UpdateViewConstants()` still hard-codes `LightDirection`, `DefaultLit.hlsl` hard-codes direct light color/intensity as `float3(4,4,4)`, and `DefaultLit.hlsl` always adds `ambientFloor = baseColor * 0.08`. This can mask whether texture IBL is actually carrying the scene.
2. **PBR numeric guardrails**: RQ2i proves procedural IBL metadata and readiness, but not numeric quality of BRDF LUT, irradiance, or prefilter output.
3. **Shadow minimum path**: `ShadowPass` is honest/registered, but DefaultLit does not sample production shadows yet.
4. **Exposure / bloom calibration**: HDR + tonemap exists, but bloom is effectively disabled by default and exposure is manual.
5. **Render quality visual probes**: current golden is still no-IBL stability; it does not compare model lighting quality under IBL.

RQ2j takes item 1 only. It is deliberately small because it touches the shared `ViewConstants` contract used by shaders, pipeline tests, and rendering passes.

## 2. Stage Decision

Make DefaultLit's directional light direction, directional light intensity, and ambient floor intensity explicit `ViewData -> ViewConstants -> shader` inputs.

When texture IBL is ready, the renderer should set the ambient floor to zero so real IBL controls the environment contribution. When texture IBL is unavailable or disabled, keep the legacy `0.08` ambient floor so existing no-IBL golden output remains stable.

This stage does not tune the BRDF, add shadows, add auto exposure, or recapture the no-IBL golden.

## 3. Current State Evidence

- `Render/Private/PipelineCache.cpp` writes `constants.lightDirection = Vec3(0.5f, -0.8f, 0.3f)` and leaves the following cbuffer float as padding.
- `Render/Shaders/DefaultLit.hlsl` uses `LightDirection` but hard-codes direct light color/intensity as `float3(4.0, 4.0, 4.0)`.
- `Render/Shaders/DefaultLit.hlsl` always adds `ambientFloor = baseColor.rgb * 0.08`, even when `IBLTextureParams.x > 0.5`.
- `Render/Include/Render/Renderer/ViewData.h` already carries IBL ambient and texture IBL controls but not direct light controls or ambient floor controls.
- `Tests/PipelineCacheValidation/main.cpp` already pins `ViewConstants` offsets and DefaultLit source guardrails, making it the right place to lock this contract.

## 4. Scope

1. Extend `ViewData`:
   - Add `directionalLightDirection`, defaulting to the existing `(0.5, -0.8, 0.3)` direction.
   - Add `directionalLightIntensity`, defaulting to the existing direct light value `4.0`.
   - Add `ambientFloorIntensity`, defaulting to the existing `0.08`.
2. Preserve `ViewConstants` size and offsets:
   - Rename/repurpose the current post-`lightDirection` padding float as `directionalLightIntensity`.
   - Use `IBLTextureParams.w` for `ambientFloorIntensity`.
   - Keep `sizeof(ViewConstants) == 144` and current cbuffer offsets stable.
3. Update `PipelineCache::UpdateViewConstants()`:
   - Upload the view-supplied directional light direction instead of a hard-coded value.
   - Normalize the direction with a fallback to the old default.
   - Clamp direct light intensity and ambient floor intensity to non-negative finite values.
   - Upload `ambientFloorIntensity` through `iblTextureParams.w`.
4. Update `DefaultLit.hlsl`:
   - Rename the cbuffer padding to `DirectionalLightIntensity`.
   - Use `DirectionalLightIntensity` for direct light radiance instead of `float3(4,4,4)`.
   - Compute `ambientFloor` from `IBLTextureParams.w`, not a hard-coded `0.08`.
5. Update `SceneRenderer::UpdateEnvironmentIBL()`:
   - Reset `m_viewData.ambientFloorIntensity` to `0.08` before texture IBL readiness checks.
   - Set `m_viewData.ambientFloorIntensity = 0.0` when texture IBL is fully ready.
   - Leave no-IBL and fallback paths at `0.08` to preserve the existing golden path.
6. Update validation:
   - `PipelineCacheValidation` layout test should check `directionalLightIntensity` offset remains 92.
   - `UpdateViewConstants` tests should assert default/custom light direction, direct intensity, and ambient floor upload.
   - Add a targeted guardrail that proves texture IBL-ready view data uploads `ambientFloorIntensity == 0.0`.
   - Shader source guardrail should require `DirectionalLightIntensity`, `IBLTextureParams.w`, and absence of hard-coded `float3(4.0, 4.0, 4.0)` / `ambientFloor = baseColor.rgb * 0.08`.
7. Update `phase-log.md` after implementation and review.

## 5. Out of Scope

- Changing the Cook-Torrance BRDF, Fresnel, BRDF LUT algorithm, or IBL equations.
- Shadow map rendering or DefaultLit shadow sampling.
- Bloom, exposure, tonemap operator, color grading, SSAO, SSR, TAA, or post-process ordering.
- Recapturing `ModelViewerSmoke --no-ibl` golden.
- Changing ModelViewer camera, default model, HDRI assets, or procedural IBL generation.
- Adding scene light components or a full lighting manager.

## 6. Expected Files

- `Docs/superpowers/specs/2026-06-08-rq2j-view-lighting-controls-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`

## 7. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation ModelViewer`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|ModelViewerIBLSmoke|ModelViewerSmoke|VisualGoldenValidation"`
- Manual visual check:
  - `build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ2j_DamagedHelmet.ppm --validation --expect-ibl-ready --expect-procedural-ibl-quality`
- Hygiene:
  - `git diff --check`

## 8. Acceptance Criteria

- `PipelineCache::UpdateViewConstants()` no longer hard-codes the default light direction.
- `DefaultLit.hlsl` no longer hard-codes direct light radiance as `float3(4.0, 4.0, 4.0)`.
- `DefaultLit.hlsl` no longer hard-codes ambient floor as `baseColor.rgb * 0.08`.
- Texture IBL-ready frames use `ambientFloorIntensity == 0.0`.
- Texture IBL fallback/no-IBL frames keep `ambientFloorIntensity == 0.08`.
- `ViewConstants` size and offsets stay stable.
- `ModelViewerSmoke` and `VisualGoldenValidation` still pass without recapturing the golden.
- DamagedHelmet still renders with texture IBL and the new quality gate.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 9. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ2j scope as the task list. Do not expand into shadowing, BRDF changes, bloom/exposure, or new light component systems during this stage.

## 10. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh)
- Code review: PASS (`gpt-5.5`, xhigh)
- Commit message: `feat(render): expose view lighting controls for default lit`
