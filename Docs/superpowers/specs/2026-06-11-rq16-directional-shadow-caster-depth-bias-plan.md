# RQ16 - Directional Shadow Caster Depth-Bias Pipeline

**Date:** 2026-06-11  
**Parent reference:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md` §15 R9 Render Pass Completion  
**Previous stage:** RQ15 Directional CSM Stabilization and Fade Bands (`d3532b1`)  
**Status:** Plan draft; implementation blocked until Spark plan review PASS.

---

## 1. Stage Decision / Goal

Add an explicit caster-side depth-bias path for directional shadow maps by using a shadow-specific depth-only
pipeline variant. RQ13 added receiver normal bias and RQ15 stabilized CSM sampling; RQ16 completes the next
shadow foundation layer by ensuring shadow-map rasterization itself can apply constant, slope-scaled, and clamp
bias through pipeline state.

This stage must keep caster raster bias separate from the existing receiver compare bias:

- `directionalShadowDepthBias` / `ShadowPassConfig::shadowBias` remains the shader compare bias.
- New caster bias config controls the shadow-map rasterizer state.

---

## 2. Current State Evidence

- `RHICommandContext::SetDepthBias()` exists, but DX11/DX12 implementations are no-op or documented as requiring
  pipeline state. A dynamic-only ShadowPass call would not improve the primary local DX11/DX12 visual path.
- `PipelineCache::BuildDepthOnlyPipelineDesc()` creates one shared `DepthOnlyPipeline` with `RHIRasterizerState::Default()`,
  clockwise front face, and `RHICullMode::None`.
- `DepthPrepass` and `ShadowPass` both consume `PipelineCache::GetDepthOnlyPipeline()`, so adding caster bias to the
  existing shared pipeline would accidentally affect the camera depth prepass.
- `ShadowPass::RenderCascade()` currently binds `GetDepthOnlyPipeline()` for shadow rendering and never requests
  a shadow-specific rasterizer state.

---

## 3. Scope

1. Extend `ShadowPassConfig` with independent caster raster-bias controls. Defaults are all `0.0f` so RQ16 adds
   capability without changing the default ModelViewer golden:
   - `float casterDepthBias = 0.0f`
   - `float casterSlopeScaledDepthBias = 0.0f`
   - `float casterDepthBiasClamp = 0.0f`
2. Add deterministic sanitization for these values before creating or requesting a pipeline.
   - NaN, Inf, negative values, and `-0.0f` sanitize to `0.0f`.
   - Extremely large finite values clamp to explicit conservative caps:
     - `casterDepthBias <= 10000.0f`
     - `casterSlopeScaledDepthBias <= 16.0f`
     - `casterDepthBiasClamp` sanitizes to `0.0f` until RHI exposes an explicit depth-bias-clamp capability.
   - Sanitized values, not raw config values, are used for pipeline keys, pipeline descs, and tests.
3. Add a shadow-specific depth-only pipeline path in `PipelineCache`.
   - Do not mutate the existing `DepthOnlyPipeline` used by `DepthPrepass`.
   - Prefer an explicit getter such as `GetShadowDepthPipeline(const ShadowDepthBiasState&)` or equivalent.
   - Add a purpose discriminator/key such as `PipelinePurpose::ShadowDepth` or an equivalent explicit hash salt.
     Bias values alone are not enough because zero-bias shadow and generic depth descs can otherwise hash identically.
   - Include both the purpose discriminator and sanitized caster bias values in the pipeline hash/variant key so
     different bias settings do not alias and zero-bias shadow does not become the generic depth-only pipeline by accident.
   - Keep the same depth format, layout, shaders, topology, front-face, and no-cull behavior unless tests prove a
     deliberate change is needed.
4. Update `ShadowPass::IsSupported()` and `RenderCascade()` to require/use the shadow-specific pipeline.
   - Unsupported or failed shadow-pipeline creation must remain visible through `GetUnsupportedReason()` or stats/tests.
5. Fix the Vulkan rasterizer mapping if the implementation touches Vulkan pipeline creation.
   - `depthBiasEnable` must be true when any sanitized constant, slope, or clamp bias value is non-zero.
   - Because no RHI depth-bias-clamp capability bit exists yet, nonzero caster clamp is sanitized to `0.0f` in RQ16.
6. Add tests proving:
   - The normal depth-only pipeline remains unbiased.
   - Zero-bias shadow depth pipeline is still keyed as shadow-purpose and does not alias the generic depth-only pipeline.
   - The shadow-specific pipeline desc contains the configured caster bias values.
   - Pipeline variant identity/hash changes when caster bias changes.
   - Invalid caster bias values sanitize to safe values.
   - `ShadowPass` binds the shadow-specific pipeline, not the generic depth-only pipeline.
   - `ShadowPass` does not call `RHICommandContext::SetDepthBias()` as a dynamic-state fallback.
7. Run the visual gate. If the shadow golden changes, inspect actual/diff before updating it.

---

## 4. Out of Scope

- PCSS/contact shadows, EVSM/MSM, variance shadows, or temporal shadow filtering.
- Changing CSM split math, cascade fade width, or PCF sample pattern.
- Changing receiver compare bias semantics.
- Changing `DepthPrepass` behavior.
- Backend-specific native descriptor refactors.

---

## 5. Expected Files

- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `RHI_Vulkan/Private/VulkanPipeline.cpp` if needed for slope-only/clamp correctness.
- `Tests/VulkanValidation/main.cpp` if Vulkan pipeline-state assertions are practical in the local fake/validation setup.
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm` only if visual inspection confirms an intended change.
- `Docs/superpowers/specs/phase-log.md` after implementation commit.

---

## 6. Required Tests

`PipelineCacheValidation`

- Generic `DepthOnlyPipeline` rasterizer bias remains zero.
- Zero-bias shadow depth pipeline carries a shadow-purpose key and does not alias the generic depth-only pipeline.
- Shadow depth pipeline rasterizer state reflects sanitized caster bias values.
- Shadow depth pipeline variant/hash changes when caster bias values change.
- Invalid caster bias values sanitize to safe zero/non-negative values.

`RenderPassValidation`

- `ShadowPass` declares support only when the shadow depth pipeline can be created.
- `ShadowPass::RenderCascade()` binds the shadow-specific pipeline.
- Configured caster bias values flow from `ShadowPassConfig` into the requested shadow pipeline.
- Recording command context confirms `SetDepthBias()` was not called by `ShadowPass`.

Backend validation

- If `RHI_Vulkan/Private/VulkanPipeline.cpp` changes, add or update validation/source-guard tests proving slope-only
  bias enables Vulkan depth bias and clamp handling is honest.

Visual gate

- `ModelViewerShadowSmoke`
- `ShadowVisualGoldenValidation`
- Inspect actual/diff before any golden update.

---

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

If implementation touches shared pipeline creation or backend state conversion, also run:

```powershell
cmake --build build\win_x64_debug --config Debug --target DX11Validation DX12Validation VulkanValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "DX11Validation|DX12Validation|VulkanValidation"
```

---

## 8. Risks

- DX11/DX12 dynamic depth bias is not sufficient; caster bias must be represented in pipeline state.
- Reusing the generic depth-only pipeline would leak bias into `DepthPrepass`.
- Shadow zero-bias and generic depth-only descs may hash identically unless a purpose discriminator is added.
- Excessive default caster bias can cause peter-panning. Defaults and ModelViewer config changes must be conservative.
- RQ16 defaults all caster bias values to zero; any visual change should be unexpected unless a sample explicitly opts in.
- Pipeline variant caching must include bias values or stale PSOs can silently reuse the wrong rasterizer state.
- Vulkan currently enables depth bias from constant bias only; slope-only state must not be silently ignored, and
  clamp remains sanitized to zero until an explicit backend capability exists.
- Visual golden may change slightly along shadow edges and must be inspected before updating.

---

## 9. Acceptance Criteria

- Directional shadow rendering uses a shadow-specific depth-only pipeline with explicit caster bias state.
- Generic camera depth-only pipeline remains unbiased.
- Zero-bias shadow pipeline is still a shadow-purpose variant and cannot accidentally become the generic depth-only pipeline.
- Caster raster bias is separate from receiver compare bias.
- Invalid caster bias values sanitize to safe state and are test-covered.
- ShadowPass does not rely on dynamic `SetDepthBias()` for caster bias.
- Vulkan slope-only handling is fixed and validated; clamp is explicitly out of use through sanitizer until capability exists.
- Required build/test/visual gates pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

---

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): add shadow caster depth bias pipeline`
