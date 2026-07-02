# RQ19 - ModelViewer Shadow Quality Presets

**Date:** 2026-06-11  
**Parent reference:** RQ18 Directional Shadow Quality Settings API  
**Previous stage:** RQ18 Directional Shadow Quality Settings API (`17d1489`)  
**Status:** Plan draft; implementation blocked until Spark plan review PASS.

---

## 1. Stage Decision / Goal

Expose the RQ18 renderer-level directional shadow settings through ModelViewer CLI presets so users can run higher
quality shadow configurations without recompiling the engine. Defaults remain unchanged to keep existing visual goldens
stable. This stage validates the new `SceneRenderer::ApplyShadowPassConfig()` integration at the sample/runtime edge.

---

## 2. Current State Evidence

- `SceneRenderer` now owns and applies a persistent `ShadowPassConfig`.
- `ShadowPassConfig` already controls shadow resolution, cascade count, split lambda, shadow compare bias, receiver
  normal bias, Poisson PCF radius, caster raster bias fields, cascade stabilization, and blend ratio.
- ModelViewer already has a deterministic `--shadow-test-scene` and `--expect-shadow-ready` smoke path.
- ModelViewer has no CLI or preset path for changing shadow quality; users cannot exercise RQ18 from the sample without
  editing code.

---

## 3. Scope

1. Add a ModelViewer shadow quality preset enum and parser.
   - Supported values: `default`, `low`, `medium`, `high`, `ultra`.
   - `default` maps to unmodified `ShadowPassConfig` defaults.
   - Invalid values fail option parsing with a visible error.
2. Add CLI help and option storage.
   - New option: `--shadow-quality <default|low|medium|high|ultra>`.
   - The option may be used with any ModelViewer run; it only matters when directional shadows are active.
3. Map presets to conservative `ShadowPassConfig` values.
   - Keep default unchanged.
   - Low/medium/high/ultra adjust only existing RQ18/RQ15-RQ17 controls: `shadowMapSize`, `numCascades`,
     `cascadeSplitLambda`, `filterRadiusTexels`, `normalBias`, `shadowBias`, and `cascadeBlendRatio`.
   - Do not set non-zero caster raster bias by default in this stage.
   - Planned preset table:

| Preset | Map size | Cascades | Lambda | Filter radius | Shadow bias | Normal bias | Blend ratio |
|--------|----------|----------|--------|---------------|-------------|-------------|-------------|
| default | `ShadowPassConfig{}` | `ShadowPassConfig{}` | `ShadowPassConfig{}` | `ShadowPassConfig{}` | `ShadowPassConfig{}` | `ShadowPassConfig{}` | `ShadowPassConfig{}` |
| low | 1024 | 2 | 0.85 | 0.75 | 0.0050 | 0.0200 | 0.04 |
| medium | 2048 | 3 | 0.90 | 1.00 | 0.0040 | 0.0200 | 0.05 |
| high | 4096 | 4 | 0.95 | 1.50 | 0.0030 | 0.0250 | 0.06 |
| ultra | 4096 | 4 | 0.98 | 2.00 | 0.0025 | 0.0300 | 0.08 |
4. Apply the preset after engine/render subsystem initialization.
   - Get `SceneRenderer` from `RenderSubsystem`.
   - Call `ApplyShadowPassConfig(MakeShadowQualityConfig(options.shadowQualityPreset))`.
   - Log the selected preset and effective key values for smoke/debug visibility.
5. Add tests/guardrails.
   - Source guard for CLI help, parser branch, preset mapping, and `ApplyShadowPassConfig()` call.
   - Manual smoke command with `--shadow-quality high` and `--expect-shadow-ready`.

---

## 4. Out of Scope

- Changing ModelViewer default visuals or existing golden images.
- Adding CTest golden coverage for high/ultra presets.
- Editor UI, runtime settings panels, asset serialization, or per-scene saved shadow settings.
- PCSS/contact shadows, temporal shadow filtering, VSM/EVSM/MSM, shadow atlases, or shader changes.
- Non-directional shadow settings.

---

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq19-modelviewer-shadow-quality-presets-plan.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md` after implementation commit

---

## 6. Required Tests

Focused gate:

- `PipelineCacheValidation` source guard confirms:
  - `--shadow-quality` help text exists.
  - `ParseShadowQualityPreset` exists and accepts the named presets.
  - `MakeShadowQualityConfig` exists.
  - `default` returns `ShadowPassConfig{}`.
  - Presets keep `casterDepthBias`, `casterSlopeScaledDepthBias`, and `casterDepthBiasClamp` at `0.0f`.
  - ModelViewer calls `sceneRenderer->ApplyShadowPassConfig(...)`.

Runtime smoke:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf --shadow-test-scene --expect-shadow-ready --shadow-quality high --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
```

Visual/regression gate:

- Existing `ModelViewerShadowSmoke` / `ShadowVisualGoldenValidation` must still pass with defaults.

---

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation"
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf --shadow-test-scene --expect-shadow-ready --shadow-quality high --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

---

## 8. Risks

- Ultra settings can be expensive. Keep the preset available but do not make it the default or part of golden gates.
- Aggressive filter/bias values can change shadow appearance. Existing golden tests continue using default settings.
- CLI parsing must fail loudly for unknown preset names; silent fallback would make quality validation misleading.

---

## 9. Acceptance Criteria

- ModelViewer accepts `--shadow-quality default|low|medium|high|ultra`.
- Default run behavior and goldens remain unchanged.
- A high-quality shadow smoke run reaches directional shadow ready state.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

---

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(samples): add model viewer shadow quality presets`
