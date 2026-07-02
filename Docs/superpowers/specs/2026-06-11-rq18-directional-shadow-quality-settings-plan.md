# RQ18 - Directional Shadow Quality Settings API

**Date:** 2026-06-11  
**Parent reference:** RQ15-RQ17 directional shadow quality stages  
**Previous stage:** RQ17 Directional Shadow Poisson PCF Kernel (`e5a7539`)  
**Status:** Plan draft; implementation blocked until Spark plan review PASS.

---

## 1. Stage Decision / Goal

Make directional shadow quality settings a first-class `SceneRenderer` runtime configuration instead of leaving them
only inside `ShadowPassConfig`. RQ15-RQ17 made CSM stabilization, caster bias, and Poisson PCF real; RQ18 makes those
quality controls reachable, persistent, and testable from the renderer orchestration layer while preserving the current
default visual output.

This is infrastructure for later quality presets, PCSS/contact shadows, and sample/UI controls. It is not a new shadow
filtering algorithm.

---

## 2. Current State Evidence

- `ShadowPassConfig` already contains the quality controls that matter now: `shadowMapSize`, `numCascades`,
  `cascadeSplitLambda`, `shadowBias`, `normalBias`, `filterRadiusTexels`, caster raster bias fields,
  `stabilizeCascades`, and `cascadeBlendRatio`.
- `OpaquePass` already consumes `ShadowPass::GetConfig()` and uploads `shadowBias`, `filterRadiusTexels`,
  `normalBias`, and `cascadeBlendRatio` into `ViewData` before `PipelineCache::UpdateViewConstants()`.
- `SceneRenderer::SetupDefaultPasses()` creates `ShadowPass` but does not expose a renderer-level shadow-quality API.
- `SceneRenderer::PreparePassesForFrame()` resets shadow view state every frame, but the persistent quality state is
  only reachable by owning/knowing the internal `ShadowPass`.
- ModelViewer has a shadow smoke path, but no renderer-facing shadow quality hook yet.

---

## 3. Scope

1. Add a renderer-owned directional shadow quality configuration.
   - Add a `ShadowPassConfig m_shadowPassConfig` member to `SceneRenderer`.
   - Add `ApplyShadowPassConfig(const ShadowPassConfig& config)`.
   - Add `GetShadowPassConfig() const`.
   - Defaults must remain exactly `ShadowPassConfig` defaults so existing visuals and goldens do not change.
2. Ensure the renderer-owned config is applied consistently.
   - `SetupDefaultPasses()` must apply `m_shadowPassConfig` to the newly created `ShadowPass`.
   - `ApplyShadowPassConfig()` must update `m_shadowPassConfig` and forward it to the live `ShadowPass` when present.
   - Calling `ApplyShadowPassConfig()` before default passes exist must still affect the eventual `ShadowPass`.
3. Keep pass responsibilities intact.
   - `ShadowPass` remains the owner of cascade calculation and shadow-map declaration.
   - `OpaquePass` remains the owner of turning the active `ShadowPass` config into per-frame shadow constants.
   - `SceneRenderer` only owns the persistent user/runtime config and orchestration.
4. Add focused tests/guardrails.
   - Add source guards for the persistent member, public apply/get API, and live `ShadowPass` forwarding path.
   - Prove `SetupDefaultPasses()` calls `shadowPass->SetConfig(m_shadowPassConfig)` before handing it to the registry.
   - Prove `ApplyShadowPassConfig()` stores the config and forwards to `m_shadowPass` if present.
   - Keep existing `OpaquePass` test proving custom `ShadowPassConfig` values upload to view constants.
   - Do not include `SceneRenderer.h` directly in lightweight validation targets just to instantiate the setter; that
     pulls in `RenderContext`/RHI backend factory symbols and expands the target dependency surface.

---

## 4. Out of Scope

- Changing default shadow map size, cascade count, PCF radius, bias values, or visual goldens.
- Adding ModelViewer CLI flags, editor UI, quality presets, or per-scene asset serialization.
- PCSS, contact shadows, stochastic/temporal shadow filtering, VSM/EVSM/MSM, shadow atlases, or non-directional shadows.
- ViewConstants layout changes, descriptor changes, render graph topology changes, or shader changes.

---

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq18-directional-shadow-quality-settings-plan.md`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp` or `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md` after implementation commit

---

## 6. Required Tests

Focused gate:

- Source guard for `SceneRenderer::ApplyShadowPassConfig`.
- Source guard for `m_shadowPassConfig`.
- Source guard that `SetupDefaultPasses()` applies `m_shadowPassConfig` to `ShadowPass`.
- Existing `OpaquePassDeclaresDirectionalShadowReadDuringSetup` remains green, proving custom `ShadowPassConfig`
  values continue to reach uploaded directional shadow constants.

Visual/regression gate:

- `ModelViewerSmoke`
- `VisualGoldenValidation`
- `ModelViewerShadowSmoke`
- `ShadowVisualGoldenValidation`
- No golden update expected.

---

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

---

## 8. Risks

- Adding `ShadowPass.h` to `SceneRenderer.h` increases header coupling. This is acceptable for RQ18 because the public
  API intentionally exposes `ShadowPassConfig`; do not pull in heavier pass implementation details.
- If the config is only forwarded to an existing pass and not stored, pre-initialization configuration would be lost.
- If defaults change accidentally, visual goldens may change even though RQ18 is intended as no-visual-change
  infrastructure.

---

## 9. Acceptance Criteria

- `SceneRenderer` exposes and persists a directional shadow quality config.
- The config is applied to the default `ShadowPass` whether it is set before or after pass creation.
- Existing shadow upload and visual gates pass.
- Default visual output remains unchanged; no golden recapture is needed.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

---

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): expose directional shadow quality settings`
