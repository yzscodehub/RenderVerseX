# RQ2e - Texture Skybox Cubemap Draw Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2d, commit `0439247 feat(render): draw procedural skybox background`
Intervening bugfix: `a3b66b0 fix(samples): submit procedural IBL uploads without short-circuit`

## 1. Stage Decision

RQ2e turns `SkyboxType::Cubemap` from an honest unsupported mode into a minimum real texture skybox background draw. The renderer should bind a GPU-ready cubemap texture from `SkyboxComponent`, sample it in `Skybox.hlsl`, and draw the background through the existing fullscreen-triangle `SkyboxPass`.

This stage is intentionally narrow. It does not generate IBL textures, does not load or convert HDR equirectangular panoramas, and does not alter DefaultLit material IBL binding. The goal is to close the background-visibility gap created by RQ2d while preserving the RQ2d procedural/color paths and the `ModelViewerSmoke --no-ibl` golden.

## 2. Current State Evidence

- `SkyboxPass::SetCubemap(RHITexture*)` stores the pointer but immediately calls `ClearSkybox("Cubemap skybox drawing is not implemented")` when a cubemap is provided.
- `SceneRenderer::UpdateSkyboxPass()` routes `SkyboxType::Procedural` and `SkyboxType::Color`, but routes both `SkyboxType::Cubemap` and `SkyboxType::Equirectangular` to `SkyboxTextureDrawingNotImplemented`.
- `PipelineCache::CreateSkyboxPipelineLayout()` creates a skybox descriptor set layout with only binding 0 as a uniform buffer.
- `Render/Shaders/Skybox.hlsl` has only procedural constants and no `TextureCube` / sampler bindings.
- `ResourceViewCache::GetDefaultSRV()` already creates a view with the texture's own dimension, so uploaded cubemaps can produce cube SRVs.
- RQ2a/RQ2c already provide cubemap CPU metadata and upload support; RQ2d already provides skybox graph target resolution, depth-tested fullscreen draw, and no-depth fallback.

## 3. Scope

1. Extend the skybox shader for cubemap sampling:
   - Add `TextureCube` and `SamplerState` bindings in `space0`.
   - Preserve the current procedural/color path.
   - Add a small mode flag in existing/new skybox constants so the pixel shader can choose procedural vs cubemap.
   - Reconstruct a camera-relative sky direction from `ViewData` inverse projection/view matrices, or a minimal equivalent constant payload, so texture skybox orientation follows the camera rotation without camera translation.
   - Apply skybox exposure once in HDR scene color space.
2. Extend `PipelineCache` skybox layout:
   - Add texture SRV binding and sampler binding to the dedicated skybox descriptor set layout.
   - Keep one dedicated skybox pipeline layout and existing output-format/depth pipeline variants.
   - Update `PipelineCacheValidation` expectations for skybox descriptor layout binding count/types and shader source guardrails.
   - Fix and test skybox pipeline cache identity:
     - Skybox pipeline hashes must include the skybox vertex shader hash and skybox pixel shader hash, not the DefaultLit shader hashes.
     - The skybox descriptor layout change must be represented in the cache identity by a layout digest or by a manifest version bump.
     - Bump the PipelineCache manifest version when the hash/layout contract changes.
     - Add `PipelineCacheValidation` coverage proving skybox shader/layout changes invalidate stale manifests and produce non-zero skybox shader/pipeline hashes.
3. Extend `SkyboxPass` runtime resources:
   - Add a pass-owned linear-wrap or linear-clamp sampler for texture skyboxes.
   - Add `SetCubemap(RHITexture*)` as a real selected draw mode when the texture is non-null.
   - Resolve/create a default cubemap SRV through `ResourceViewCache`.
   - Bind constants, cubemap SRV, and sampler in the skybox descriptor set when cubemap mode is active.
   - Keep procedural/color descriptor creation valid under the expanded layout, either by binding a safe fallback texture/sampler or by using a validation-supported optional strategy. No null descriptors.
   - Keep fallback visible: if cubemap texture, SRV, sampler, pipeline layout, or constants are unavailable, skip draw with a clear reason/log.
4. Extend `SceneRenderer::UpdateSkyboxPass()`:
   - For `SkyboxType::Cubemap`, read `SkyboxComponent::GetCubemap()`.
   - If no cubemap resource is assigned, clear with `SkyboxCubemapMissing`.
   - If assigned but not resident/GPU-ready, request high-priority upload through `GPUResourceManager`, clear with `SkyboxCubemapNotReady`, and do not draw a fake procedural background.
   - If GPU-ready, resolve the `RHITexture*` and call `SkyboxPass::SetCubemap()`.
   - Keep `SkyboxType::Equirectangular` honestly unsupported as `SkyboxEquirectangularDrawingNotImplemented`.
5. Add or update tests:
   - `RenderPassValidation`:
     - cubemap mode binds constants + texture + sampler and draws a fullscreen triangle.
     - cubemap mode skips draw when SRV or sampler creation is unavailable.
     - procedural/color mode still draws under the expanded descriptor layout.
   - `PipelineCacheValidation`:
     - skybox descriptor layout includes uniform buffer, texture SRV, and sampler bindings.
     - shader source contains `TextureCube`, sampler, and a mode branch/guardrail.
   - `RenderSceneValidation` or a new focused renderer bridge test is mandatory:
     - cubemap component not ready requests upload and remains unsupported/cleared.
     - cubemap component with no assigned resource remains unsupported/cleared as `SkyboxCubemapMissing`.
     - cubemap component with a GPU-ready resource passes a non-null `RHITexture*` into `SkyboxPass::SetCubemap()` or an equivalent observable pass state.
     - equirectangular component remains honestly unsupported.
   - Existing visual gates remain active and should not require golden recapture.

## 4. Out of Scope

- Equirectangular panorama draw or conversion.
- HDRI file discovery/wiring in ModelViewer.
- CPU/GPU convolution for irradiance or prefiltered maps.
- Specular reflection quality changes in DefaultLit.
- Sky atmosphere/clouds/sun quality work.
- New golden image capture unless the existing `--no-ibl` golden unexpectedly changes.
- ECS/Object/RenderProxy refactors.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-07-rq2e-texture-skybox-cubemap-draw-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Shaders/Skybox.hlsl`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp` or a new focused renderer bridge validation target for the mandatory SceneRenderer cubemap bridge cases.

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"`
- Visual:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"`
- Regression:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ClusteredLightingValidation"`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- `SkyboxType::Cubemap` can draw a real cubemap texture background when the texture is GPU-ready.
- `SkyboxType::Cubemap` no longer falls through to `SkyboxTextureDrawingNotImplemented`.
- `SkyboxType::Equirectangular` remains visibly unsupported and does not draw a fake fallback.
- Procedural and solid-color skybox paths still draw.
- Skybox pipeline manifest/cache identity includes the skybox shader inputs and the RQ2e layout contract; stale skybox shader/layout metadata invalidates rather than silently reusing old cache state.
- SceneRenderer cubemap bridge behavior is test-covered for missing resource, not-ready upload request, ready texture handoff, and equirectangular unsupported fallback.
- `ModelViewerSmoke --no-ibl` and its visual golden remain stable.
- `ModelViewerIBLSmoke` remains green after the RQ2e shader/layout changes.
- All fallback paths are visible through unsupported reasons, logs, or test-observable state.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only the RQ2e scope above as the task list. Do not expand into HDRI loading, equirectangular conversion, IBL convolution, or material lighting quality work during this stage.

## 9. Spark Review

- Plan review: pending (`gpt-5.5`, xhigh)
- Code review: pending (`gpt-5.5`, xhigh)
- Commit message: `feat(render): draw cubemap texture skybox`
