# RQ3c - Directional Shadow PCF Filter Foundation Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ3b
Previous stage: RQ3b - Texture View Role Semantics

## 1. Stage Decision

RQ3a connected directional shadow sampling and RQ3b made the shadow SRV/DSV view roles honest. The next visible quality problem is that `DefaultLit` still uses a single hard depth compare for directional shadows. RQ3c replaces that one-tap hard edge with a small, deterministic PCF filter and exposes a filter radius in the CPU-side shadow configuration.

This stage is deliberately a foundation step. It improves the main lit shader shadow quality without changing RenderGraph topology, descriptor layouts, shadow atlas structure, cascade selection, or the `ViewConstants` ABI.

## 2. Current Engine Evidence

- `Render/Shaders/DefaultLit.hlsl::SampleDirectionalShadow()` performs one `SampleLevel()` and one binary compare.
- `DirectionalShadowParams.w` is already uploaded as the reciprocal map size and was reserved for shader filtering.
- `ShadowPassConfig` has map size, cascade count, split lambda, depth bias, and normal bias, but no filter-radius control.
- `OpaquePass` sets `drawView.directionalShadowInvMapSize` from `shadowConfig.shadowMapSize`.
- `PipelineCacheValidation` already verifies `ViewConstants` packing and shadow param upload, so RQ3c must avoid ABI churn unless absolutely required.
- Existing visual gates use ModelViewer/DamagedHelmet and should keep passing; RQ3c should not require recapturing the R7 triangle golden.

## 3. Scope

1. Add a CPU-side shadow filter radius.
   - Add `float filterRadiusTexels = 1.0f` to `ShadowPassConfig`.
   - Add a matching `float directionalShadowFilterRadiusTexels = 1.0f` to `ViewData`.
   - `OpaquePass` copies `shadowConfig.filterRadiusTexels` into the draw view before uploading main-view constants.

2. Keep the `ViewConstants` ABI stable.
   - Do not add fields to `ViewConstants`.
   - Reinterpret `DirectionalShadowParams.w` as the final filter step in UV units: `invShadowMapSize * filterRadiusTexels`.
   - Keep default behavior equivalent to the existing `1.0 / shadowMapSize` because default radius is 1 texel.
   - Clamp/sanitize invalid values in `PipelineCache::UpdateViewConstants()`.

3. Replace one-tap hard shadowing with bounded 3x3 PCF.
   - Add shader helpers in `DefaultLit.hlsl`, for example:
     - `CompareDirectionalShadowDepth(float2 uv, float compareDepth)`
     - `SampleDirectionalShadowPCF(float2 uv, float compareDepth, float filterStep)`
   - Use 3x3 taps around the base UV when `filterStep > 0`.
   - Average only taps whose UVs stay inside the shadow map; if no valid tap remains, return lit.
   - Preserve the existing out-of-frustum behavior and strength blend.
   - Keep manual depth compare with `SamplerState`; do not switch to `SamplerComparisonState` in this stage.

4. Tests and guardrails.
   - Extend `PipelineCacheValidation` to prove `ViewConstants` layout is unchanged.
   - Extend shadow param upload tests so `DirectionalShadowParams.w` equals `invMapSize * filterRadiusTexels`.
   - Add invalid-control coverage for negative/NaN shadow filter radius and shadow param clamping.
   - Extend the DefaultLit shader source guardrail so the shadow path contains PCF helper logic and no longer depends on a single `shadowUV` sample/binary compare.
   - Keep RQ3a/RQ3b shadow pass and texture-view role tests passing.

## 4. Out of Scope

- PCSS, EVSM/VSM/MSM, contact shadows, screen-space shadows, temporal shadow denoising.
- Cascade stabilization, cascade selection/blending in `DefaultLit`, shadow atlas packing, array texture refactors.
- Point/spot shadows.
- Reverse-Z shadow sampling support.
- Comparison samplers or backend-specific hardware PCF.
- Visual golden recapture unless an existing gate unexpectedly changes and the change is reviewed.
- ECS/Object/RenderProxy or broader RHI descriptor redesign.

## 5. Expected Files

- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- `PipelineCacheValidation.ViewConstantsLayoutMatchesDefaultLitCBufferPacking` remains unchanged in size and offsets.
- `PipelineCacheValidation.UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention` verifies `DirectionalShadowParams.w == invMapSize * filterRadiusTexels`.
- `PipelineCacheValidation.UpdateViewConstantsSanitizesInvalidLightingControls` verifies invalid shadow radius/bias/strength/step inputs are clamped or fallback safely.
- `RenderPassValidation.OpaquePassDeclaresDirectionalShadowReadDuringSetup` or a new adjacent test sets `ShadowPassConfig::filterRadiusTexels` to a non-default value, runs the existing OpaquePass shadow path, and asserts the uploaded `directionalShadowParams.w == filterRadiusTexels / shadowMapSize`. This must prove `ShadowPassConfig -> OpaquePass -> ViewData -> PipelineCache -> ViewConstants` propagation.
- `PipelineCacheValidation.DefaultLitUsesIBLAmbientViewConstants` or a new adjacent guardrail verifies:
  - `SampleDirectionalShadowPCF` exists.
  - `DirectionalShadowParams.w` is consumed as the PCF filter step.
  - the shader uses multiple PCF taps instead of a single hard `shadowUV` sample compare.
- Existing `RenderPassValidation` shadow tests keep passing, especially RQ3a shadow read/fallback behavior.
- Existing RQ3b focused RHI texture-view tests keep passing.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation ResourceViewCacheValidation DX11Validation DX12Validation VulkanValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|ResourceViewCacheValidation|DX11Validation|DX12Validation|VulkanValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ3c_DamagedHelmet.ppm --validation --expect-ibl-ready --expect-procedural-ibl-quality
git diff --check
```

## 8. Risks

- Shader source guardrails are weaker than a GPU image test for a shadowed scene. RQ3c mitigates this by keeping the change local and preserving existing ModelViewer/visual gates.
- Enlarging the filter kernel near map borders can darken edges if invalid taps are counted. The shader must average only valid in-bounds taps.
- Reusing `DirectionalShadowParams.w` avoids ABI churn but requires comments/tests to make the new meaning clear: it is a UV-space filter step, not merely raw inverse map size.
- A filter radius of zero should gracefully fall back to a one-tap compare for debugging and future quality toggles.

## 9. Acceptance Criteria

- DefaultLit directional shadows use a bounded 3x3 PCF path with the same fallback behavior as RQ3a.
- Shadow filter radius is configurable through `ShadowPassConfig` and reaches the shader through existing ViewConstants without ABI changes.
- Invalid filter settings are sanitized and test-covered.
- Existing shadow, RHI texture-view, visual, and ModelViewer validation pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ3c scope as the task list. Do not expand into PCSS/EVSM, full CSM cascade blending, reverse-Z shadow sampling, visual-golden recapture, RenderGraph rewrites, or ECS/Object refactors.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; initial BLOCKED item resolved by requiring non-default OpaquePass filter-radius propagation coverage)
- Code review: PASS (`gpt-5.5`, xhigh; no blockers, optional comment/file-list follow-ups applied and re-reviewed)
- Commit message: `feat(render): add directional shadow pcf filter`
