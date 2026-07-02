# RQ28 - Directional Light Color in DefaultLit

Date: 2026-06-11

## 1. Stage Decision

Route the selected primary directional light color into the DefaultLit shader.
Today `SceneRenderer` selects a `RenderLight` direction and intensity, but
`DefaultLit.hlsl` reconstructs direct light as white `float3(intensity)`. This
loses warm/cool sunlight and makes the PBR path less faithful to scene lighting.

RQ28 is a small engine-level lighting correctness stage. It does not change the
number of lights, the shadow system, IBL, clustered lighting, or default
post-process settings.

## 2. Current Evidence

- `SceneRenderer::PreparePassesForFrame()` copies the primary directional
  `light.direction` and `light.intensity`, but not `light.color`.
- `ShadowPass::SetDirectionalLight()` already receives `light.color`, so the
  selected light has color data available.
- `ViewData` only exposes `directionalLightDirection` and
  `directionalLightIntensity`.
- `ViewConstants` and `DefaultLit.hlsl` only expose `LightDirection` and
  `DirectionalLightIntensity`.
- `DefaultLit.hlsl` passes
  `float3(DirectionalLightIntensity, DirectionalLightIntensity,
  DirectionalLightIntensity)` into `EvaluatePBR()`.
- Existing tests explicitly guard the old white-light expression, so tests must
  be updated with the new contract.

## 3. Scope

1. Add `Vec3 directionalLightColor` to `ViewData`, defaulting to white.
2. Add `Vec3 directionalLightColor` plus padding to `ViewConstants`, keeping
   HLSL and CPU cbuffer packing explicit.
3. Reset `SceneRenderer` frame defaults to white directional light color.
4. When a primary directional `RenderLight` is selected, copy `light.color` into
   `m_viewData.directionalLightColor`.
5. In `PipelineCache::UpdateViewConstants()`, sanitize/upload light color:
   any non-finite component falls back to white; finite negative components are
   clamped to `0.0`.
6. Update `DefaultLit.hlsl` to use
   `DirectionalLightColor * DirectionalLightIntensity` for direct lighting.
7. Extend validation:
   - `ViewConstantsLayoutMatchesDefaultLitCBufferPacking` offsets/size.
   - `UpdateViewConstantsUploadsCustomAndDisabledIBLAmbientValues` or a focused
     new test proves color upload and sanitization.
   - `DefaultLitUsesIBLAmbientViewConstants` source guard proves white-light
     expression is gone and color*intensity is used.
   - `SceneRendererUsesSamePrimaryDirectionalLightForDefaultLitAndShadowPass`
     source guard proves renderer copies `light.color`.

## 4. Out of Scope

- Multiple directional lights in DefaultLit.
- Point/spot light shading changes.
- Physical light units, lux/nits calibration, or exposure defaults.
- Shadow map color filtering or colored shadows.
- Clustered lighting integration.
- ModelViewer default cinematic preset.
- Main `R7_DX11_320x180.ppm` golden recapture. The shadow-specific golden may
  be recaptured if validation proves the only drift is the intended warm
  directional light color now affecting DefaultLit.

## 5. Expected Files

- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm` if the shadow golden
  changes for the intended directional light color reason
- `Docs/superpowers/specs/phase-log.md` after implementation commit

## 6. Execution Plan

1. Re-check this document before editing.
2. Send this plan to Spark/Mill (`gpt-5.5 xhigh`) for plan review.
3. If review PASSes, update `Plan review` status.
4. Add `directionalLightColor` to `ViewData`.
5. Add matching `ViewConstants` field and HLSL cbuffer field.
6. Upload sanitized color in `PipelineCache::UpdateViewConstants()`.
7. Copy selected `RenderLight::color` in `SceneRenderer::PreparePassesForFrame()`.
8. Update `DefaultLit.hlsl` direct-light color computation.
9. Update validation tests.
10. Build and run focused validation.
11. Run default visual gate.
12. If only `ShadowVisualGoldenValidation` changes, confirm the shadow test
    scene uses a non-white directional light and recapture
    `RQ3d_Shadow_DX11_320x180.ppm`.
13. Re-run default visual gate.
14. Run `git diff --check`.
15. Send code diff to Spark/Mill (`gpt-5.5 xhigh`) for code review.
16. If review PASSes, update `Code review` status and commit implementation.
17. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation`:
  - `ViewConstants` remains standard layout and HLSL-compatible.
  - `directionalLightColor` offset and total cbuffer size are pinned.
  - custom color uploads to the view constant buffer.
  - invalid/non-finite color inputs sanitize to white or non-negative values.
  - shader source contains `DirectionalLightColor * DirectionalLightIntensity`.
  - shader source no longer contains the old white-light expansion.
  - SceneRenderer source guard shows selected `RenderLight::color` is copied to
    `m_viewData.directionalLightColor`.
- Main R7 visual golden remains unchanged. Shadow golden is recaptured only
  after confirming the diff comes from the intended warm directional light
  correction, then the full visual gate must pass.

## 8. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Cbuffer layout drift can break DefaultLit on real backends; tests must pin CPU
  offsets and HLSL field order.
- The main default visual should remain stable because its default directional
  light color is white. The shadow fixture intentionally uses a warm directional
  light, so `ShadowVisualGoldenValidation` may require a deliberate recapture
  after confirming the diff is the intended light-color correction.
- Light color sanitization must not turn invalid scene data into NaNs in shader
  constants: any non-finite component falls back to white, while finite negative
  components clamp to `0.0`.

## 10. Acceptance Criteria

- DefaultLit direct lighting uses selected directional light color and intensity.
- SceneRenderer selects one primary directional light and passes color to both
  DefaultLit view constants and ShadowPass.
- ViewConstants CPU/HLSL packing is updated and covered by tests.
- Focused validation and default visual gate pass, with shadow golden recaptured
  only if the intended warm directional light correction changes it.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Code review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Commit message: `feat(render): shade directional lights with color`
