# RQ2d - Procedural Skybox Minimum Draw Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2c, commit `7a7fef9 feat(samples): wire procedural IBL into model viewer`

## 1. Stage Decision

RQ2d moves `SkyboxPass` from an honest unsupported placeholder to a minimum renderable procedural sky background. The pass will draw a fullscreen triangle into the scene color target using a small `Skybox.hlsl` shader and pass-local constants, but only when the scene provides an active `SkyboxComponent` with a supported background mode. This explicit scene gating preserves the existing ModelViewer golden because `ModelViewerSmoke` runs with `--no-ibl`, which also avoids creating the ModelViewer procedural skybox component.

This is a minimum draw and pipeline stage. It does not sample cubemap textures, does not load external HDRI assets, and does not implement environment convolution.

## 2. Current State Evidence

- `SceneRenderer::SetupDefaultPasses()` already creates `SkyboxPass`, passes `PipelineCache`, and registers it with the pass registry.
- `RenderFrameResourceBinder::BindScenePassResources()` already assigns color and depth render targets to `SkyboxPass`.
- `SkyboxPass::Setup()` already declares a color write and read-only depth in the RenderGraph path, but `m_drawReady` stays false and the pass reports unsupported.
- `SkyboxPass::Execute()` resolves graph-owned color RTVs but does not resolve graph-owned depth DSVs and does not bind a pipeline or issue a draw.
- `PipelineCache` currently creates DefaultLit, DepthOnly, ToneMapping, and Bloom pipelines, but has no skybox shader, descriptor layout, or pipeline accessors.
- PipelineCache manifest metadata currently tracks DefaultLit, ToneMapping, and Bloom shader/pipeline hashes only. Adding a skybox pipeline must update manifest parse/write/match logic and tests.
- `ModelViewer` now wires a `SkyboxComponent` for IBL in RQ2c, but the component does not make the background visible because `SkyboxPass` still does not draw.

## 3. Scope

1. Add a `Render/Shaders/Skybox.hlsl` minimum procedural sky shader:
   - Vertex shader emits a fullscreen triangle.
   - Vertex shader writes a depth value from constants: approximately `0.999` for forward Z and `0.001` for reverse Z, so read-only depth testing fills only background pixels.
   - Pixel shader outputs a simple horizon-to-sky gradient plus optional sun tint using constants.
   - No cubemap sampling in this stage.
2. Extend `PipelineCache` with skybox resources:
   - Compile `Skybox.hlsl` VS/PS during shader compilation.
   - Add a dedicated skybox descriptor set layout with one uniform buffer binding.
   - Add a dedicated skybox pipeline layout.
   - Add `GetSkyboxPipeline()` / `GetSkyboxPipeline(RHIFormat)` accessors.
   - Add `GetSkyboxSetLayout()` / `GetSkyboxLayout()` accessors for pass binding.
   - Add a skybox pipeline builder using fullscreen triangle, no vertex input, one render target, read-only depth, and the configured depth format.
   - Cache per-output-format skybox pipelines like ToneMapping/Bloom.
   - Add skybox vertex/pixel shader hashes and skybox pipeline hash to PipelineCache manifest metadata.
   - Update manifest read/write/match/known-field logic and fixed pipeline count/format tests for the additional pipeline.
3. Add scene-to-skybox pass gating:
   - Add a `SceneRenderer` step that finds the first active enabled `SkyboxComponent` for background drawing.
   - If no component exists, disable or clear `SkyboxPass` so it remains non-drawing and reports a visible no-skybox reason.
   - Support `SkyboxType::Procedural` in this stage.
   - Treat `SkyboxType::Color` as a procedural-solid variant by setting zenith/horizon/ground colors to the solid color.
   - Keep `SkyboxType::Cubemap` and `SkyboxType::Equirectangular` out of scope for drawing; they should not silently draw a fake texture background.
   - Update ModelViewer's procedural IBL skybox setup to set `SkyboxType::Procedural` and explicit procedural colors. The `--no-ibl` path still creates no skybox component.
4. Implement `SkyboxPass` runtime resources and draw:
   - Allocate a pass-local upload constant buffer and retain descriptor sets for a few frames.
   - Support only component-gated procedural/solid sky draw readiness for RQ2d.
   - `SetResources()` reports unsupported until `PipelineCache` is initialized and skybox pipeline/layout/set layout are available.
   - `SetProceduralSkyParams()` / `SetSolidColor()` marks the pass draw-requested only after scene gating has selected a supported component.
   - `ClearSkybox()` or equivalent returns the pass to a non-drawing state with visible reason.
   - `Setup()` continues to declare scene color write and read-only depth.
   - `Execute()` resolves both graph color RTV and graph depth DSV when graph handles are active.
   - `Execute()` binds skybox pipeline + descriptor set and draws 3 vertices.
   - If no depth target is available, draw full background without depth attachment.
5. Update validation:
   - `RenderPassValidation`:
     - uninitialized `SkyboxPass` remains requested but unsupported.
     - initialized `SkyboxPass` without a component-selected sky remains unsupported/non-drawing.
     - initialized `SkyboxPass` with procedural sky params reports supported.
     - graph execution path binds skybox pipeline, creates a skybox descriptor set, and draws a fullscreen triangle.
     - missing pipeline/layout/constant-buffer map failure remains visible and skips draw.
   - `PipelineCacheValidation`:
     - missing `Skybox.hlsl` reports a specific initialization error.
     - skybox pipeline exists after initialization.
     - skybox descriptor layout contains only uniform binding 0.
     - manifest includes and invalidates on skybox shader/pipeline hash changes.
     - source guardrail verifies fullscreen triangle and procedural gradient path.
   - `RenderHonestyValidation`:
     - update old placeholder expectation so Skybox is still unsupported without resources/scene selection and supported only after explicit procedural sky selection.
   - Existing visual gates remain active.

## 4. Out of Scope

- Cubemap skybox sampling.
- Binding `SkyboxComponent` cubemap/equirectangular textures to the pass.
- External HDRI loading in ModelViewer.
- CPU or GPU IBL convolution.
- Sky atmosphere, clouds, sun disk quality, aerial perspective, or physically based sky.
- Visual golden recapture unless the current golden path unexpectedly changes.
- RenderProxy, ECS/Object refactors, or material descriptor layout changes.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-07-rq2d-procedural-skybox-minimum-draw-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Shaders/Skybox.hlsl`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

## 6. Required Tests

- Build:
  - `PipelineCacheValidation`
  - `RenderPassValidation`
  - `RenderHonestyValidation`
  - `ModelViewer`
- Focused:
  - `PipelineCacheValidation`
  - `RenderPassValidation`
  - `RenderHonestyValidation`
- Visual:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerIBLSmoke`
- Regression:
  - `RenderGraphValidation`
  - `RenderSceneValidation`
  - `MaterialSystemValidation`
  - `GPUUploadServiceValidation`
  - `GPUResourceManagerValidation`
  - `ClusteredLightingValidation`

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderHonestyValidation ModelViewer
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderHonestyValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|RenderSceneValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ClusteredLightingValidation"
git diff --check
```

## 8. Risks

- Skybox could overwrite foreground if depth state or shader depth is wrong.
  - Mitigation: use read-only depth test and pass a forward/reverse-Z far depth constant; validate draw setup through `RenderPassValidation`.
- Existing golden could change if the old smoke path starts drawing sky behind the R7 triangle.
  - Mitigation: gate drawing on `SkyboxComponent`; `ModelViewerSmoke --no-ibl` creates no skybox component. Keep `VisualGoldenValidation` as a hard gate. If it changes, treat that as a blocker unless the stage explicitly recaptures golden after review.
- New pipeline creation can make PipelineCache initialization fail when the shader file is missing.
  - Mitigation: add a specific missing-shader test and visible error text.
- A dedicated skybox layout adds more PipelineCache surface area.
  - Mitigation: keep one uniform-buffer binding only and do not alter existing DefaultLit/material/post-process layouts.
- Manifest compatibility can break if the new skybox hashes are not serialized consistently.
  - Mitigation: update known fields, required reads, writes, match checks, and stale-hash tests in the same stage.

## 9. Acceptance Criteria

- `SkyboxPass` remains honestly unsupported before resources are assigned.
- With initialized `PipelineCache` and an explicit procedural/solid `SkyboxComponent` selection, `SkyboxPass` is supported and draws one fullscreen triangle through RenderGraph execution.
- Without a selected supported `SkyboxComponent`, `SkyboxPass` does not draw and reports a visible reason.
- Skybox pipeline and descriptor layout are complete and separately queryable.
- PipelineCache manifest includes skybox shader/pipeline hashes and invalidates on stale skybox metadata.
- Existing ModelViewer golden remains stable or a reviewed recapture is performed.
- Validation commands pass.
- Spark plan review and Spark code review have no blockers.

## 10. Spark Review

Plan review: BLOCKED by first pass (`Confucius`, `gpt-5.3-codex-spark`)

- Blockers adopted:
  - Added explicit scene/component draw gating so `ModelViewerSmoke --no-ibl` does not draw a skybox and does not perturb the existing golden.
  - Added PipelineCache manifest/hash/count update requirements for the new skybox pipeline.
  - Added `SceneRenderer` / `ModelViewer` scope for passing `SkyboxComponent` background data to `SkyboxPass`.

Second plan review: PASS (`Confucius`, `gpt-5.3-codex-spark`)

- Verdict: PASS.
- Blockers: none.
- Non-blocking notes adopted:
  - Keep the `SceneRenderer` skybox selection strategy stable: first active enabled `SkyboxComponent` in scene traversal order.
  - Add a no-selected-skybox draw test with zero pipeline binds.
  - Include skybox fields in manifest known-field/read/write/match validation.

Code review: PASS (`Confucius`, `gpt-5.3-codex-spark`)

- Verdict: PASS.
- Blockers: none.
- Non-blocking follow-ups:
  - Add a future scene-level regression proving `SkyboxType::Cubemap` / `SkyboxType::Equirectangular` components do not draw until texture skybox support is implemented.
  - Add a future direct ModelViewer state/log assertion for `--no-ibl` proving no `SkyboxComponent` is created, beyond the current visual golden protection.

Commit message: `feat(render): draw procedural skybox background`
