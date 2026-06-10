# RQ15 - Directional CSM Stabilization and Fade Bands

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Previous stage: RQ14 - Directional CSM Array Consumption

## 1. Stage Decision

Improve directional shadow quality now that RQ14 has made the renderer consume a real CSM texture array.
RQ15 makes CSM behavior less fragile by stabilizing cascade projection in light-space texel units and adding
a small fade band at cascade boundaries. This targets two visible defects that block higher-quality outdoor
lighting: sub-texel shadow shimmer during camera movement and hard cascade seams.

This stage stays inside the current shadow architecture. It does not introduce a new shadow representation or
a new filtering algorithm.

## 2. Current State Evidence

- `ShadowPass::CalculateCascades()` builds each cascade from the camera frustum slice, computes light-space
  min/max bounds, adds padding, and immediately creates an orthographic projection.
- The current cascade projection is not snapped to the shadow-map texel grid, so a small camera movement can
  move the shadow projection by sub-texel amounts.
- `DefaultLit.hlsl::SelectDirectionalShadowCascade()` uses a hard split test and samples only one cascade.
- RQ14 now uploads all cascade matrices and split distances and binds a `Texture2DArray<float>`, so sampling
  adjacent cascades for a fade band is possible without changing descriptor shape.
- RQ14 fixed backend array-layer views; RQ15 can rely on per-layer cascade data being genuinely separate.

## 3. Scope

1. Extend `ShadowPassConfig` with conservative quality controls:
   - `stabilizeCascades`, default enabled.
   - `cascadeBlendRatio`, default small and finite, used only for transitions between active cascades.
2. Stabilize cascade light-space XY projection:
   - Use a stable square XY extent derived from the frustum-slice radius or fitted extent.
   - Snap the light-space cascade center to shadow-map texel units derived from
     `worldUnitsPerTexel = stableExtent / shadowMapSize`.
   - Preserve Z near/far fitting and existing padding behavior.
   - Keep deterministic output for identical inputs.
3. Add cascade fade distances:
   - Extend `ViewData` and `ViewConstants` with `directionalShadowCascadeFadeDistances`.
   - Compute up to three absolute camera-forward fade widths as a fraction of the cascade span immediately
     before each split and clamp the width to that span.
   - Clamp invalid/negative/non-finite values to zero.
4. Update `DefaultLit.hlsl`:
   - Keep the primary cascade selection from RQ14.
   - When the pixel is within the configured fade distance before a split, also sample the next cascade.
   - Blend visibility using a smooth or linear fade that is zero outside the fade band.
   - Preserve RQ13 receiver normal bias, existing PCF, and depth bias behavior for each cascade sample.
5. Keep descriptor layout stable:
   - Still one shadow texture binding and one sampler.
   - No descriptor array/bindless changes.
6. Add tests and source guardrails for stabilization, fade distance upload, and shader boundary blending.
7. Run the visual gate and inspect shadow actual/diff. Update golden only if the changed pixels are the intended
   stable-shadow/fade result and Spark code review has no blocker.

## 4. Out of Scope

- PCSS/contact shadows.
- VSM/EVSM/MSM or moment filtering.
- Cascade count policy changes.
- Shadow atlas packing.
- Descriptor arrays or bindless shadows.
- Terrain-scale cascaded shadow tuning.
- Point/spot light shadows.
- Transparent shadow receiving.
- Editor/UI controls for shadow settings.
- Broad artistic retuning of light direction, strength, map size, or bias.

## 5. Expected Files

- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm` if visual inspection proves an intended change
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

1. `RenderPassValidation`:
   - Cascade stabilization produces light-space XY bounds aligned to the shadow texel grid.
   - Small camera translation below one texel does not change the snapped cascade projection, while translation
     above one texel is allowed to change it.
   - Disabling `stabilizeCascades` preserves the unsnapped path.
   - `OpaquePass` uploads finite fade distances derived from active cascade splits and `cascadeBlendRatio`.
   - Disabled or missing shadow paths reset fade distances to zero.
2. `PipelineCacheValidation`:
   - `ViewConstantsLayoutMatchesDefaultLitCBufferPacking` includes the new fade-distance vector.
   - `UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention` verifies fade-distance upload and
     clamps invalid values.
   - Source guard requires adjacent cascade sampling and fade-weight logic in `DefaultLit.hlsl`, including that
     the secondary sample uses `cascadeIndex + 1` only when the pixel is inside the fade band and the next
     cascade is in range.
3. Visual/regression:
   - `ModelViewerShadowSmoke`
   - `ShadowVisualGoldenValidation`
   - `ModelViewerSmoke`
   - `VisualGoldenValidation`
   - `RenderSceneValidation`
   - `RenderHonestyValidation`
   - `RenderPassValidation`
   - `PipelineCacheValidation`
   - `ImageCompareValidation`

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderHonestyValidation|ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- Stabilized projections can change the existing shadow golden. Any golden update must be visually inspected
  and limited to the shadow fixture.
- Over-wide fade bands can soften or double-darken seams. Keep the default small and clamp against cascade span.
- HLSL constant layout changes must remain 16-byte aligned and locked by tests.
- Sampling a second cascade adds cost in the fade band. RQ15 only samples the neighbor when fade distance is
  positive and the pixel is inside the band.
- Stable square extents may trade a little resolution for reduced shimmer. This is acceptable for a first
  production-style CSM stability pass.

## 9. Acceptance Criteria

- Directional CSM cascade projections are stable under sub-texel camera translations when stabilization is enabled.
- Stabilization can be disabled through `ShadowPassConfig` and remains test-covered.
- Shader fades from one cascade to the next near split boundaries without changing descriptor shape.
- Fade distances are finite, clamped, uploaded, and reset on disabled paths.
- Required validation commands pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): stabilize directional csm transitions`
