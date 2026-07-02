# RQ22 - ModelViewer Tone Mapping Exposure/Gamma CLI

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ21 - ModelViewer Tone Mapping Operator CLI

## 1. Stage Decision

Expose tone mapping exposure and display gamma at the ModelViewer sample/runtime
boundary. RQ20 made `PostProcessSettings` the single contract for tone mapping
state, and RQ21 exposed the operator. RQ22 adds the two numeric controls already
consumed by `ToneMappingPass::Configure()` so visual tuning can happen from the
sample without code edits.

Use explicit flag names:

- `--post-exposure <linear>` for `PostProcessSettings::exposure`
- `--display-gamma <value>` for `PostProcessSettings::gamma`

The names are intentionally not just `--exposure` / `--gamma` to avoid confusion
with HDRI loader exposure and color-grading gamma controls. The default
ModelViewer path must remain unchanged.

## 2. Current State

- `ToneMappingPass::Configure()` applies `settings.exposure`, `settings.gamma`,
  and `settings.toneMappingOperator`.
- `ToneMapping.hlsl` multiplies HDR scene color by `Exposure`, applies the
  selected operator, and then applies one display conversion with `Gamma`.
- ModelViewer exposes `--tonemap` from RQ21, but cannot tune exposure/gamma from
  the command line.
- ModelViewer already applies explicit post-process changes after
  `engine.Initialize()` by copying `sceneRenderer->GetPostProcessSettings()` and
  calling `ApplyPostProcessSettings()`.

## 3. Scope

1. Add ModelViewer options for optional post-process exposure and display gamma:
   - `bool postExposureSet`
   - `bool displayGammaSet`
   - `float postExposure`
   - `float displayGamma`
2. Add help text:
   - `--post-exposure <linear>`
   - `--display-gamma <value>`
3. Add a float parser for finite numeric values.
4. Validate ranges with visible failure:
   - `--post-exposure`: finite `0.0 <= value <= 64.0`
   - `--display-gamma`: finite `0.1 <= value <= 10.0`
5. After engine initialization, apply all explicit post-process controls in one
   settings copy/apply block:
   - explicit `--tonemap` selection, if not `default`;
   - explicit `--post-exposure`, if set;
   - explicit `--display-gamma`, if set.
6. If no explicit post-process controls are requested, do not call
   `ApplyPostProcessSettings()` just to restate defaults.
7. Log the effective explicit post-process controls when applied.
8. Add `PipelineCacheValidation` source guards for help text, parser/range checks,
   options fields, single copy/apply path, and default no-op behavior.
9. Add a non-golden ModelViewer smoke command that combines `--tonemap aces`,
   `--post-exposure`, and `--display-gamma`.
10. Keep existing default visual/golden gates unchanged.

## 4. Out of Scope

- Changing default exposure/gamma/operator.
- Golden recapture.
- Auto exposure, camera EV, eye adaptation, histogram metering, or luminance
  calibration.
- Color grading exposure/gamma controls or LUTs.
- Shader math changes.
- UI controls.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq22-modelviewer-tonemap-exposure-gamma-cli-plan.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ22 scope.
2. Add optional exposure/gamma fields to `ModelViewerOptions`.
3. Add `--post-exposure` and `--display-gamma` usage text.
4. Add a finite `ParseFloat()` helper, keeping `ParseUInt()` unchanged.
5. Add range-checked parsing branches with clear error messages.
6. Refactor the RQ21 post-process application block so it:
   - copies settings at most once;
   - applies explicit tonemap/exposure/gamma values to the copy;
   - calls `ApplyPostProcessSettings()` only when at least one explicit
     post-process control is set.
7. Preserve shadow quality application behavior.
8. Add source guards.
9. Build and run validation gates.
10. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
11. Commit implementation only after Spark review PASS.
12. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation`: source guard that ModelViewer:
  - documents `--post-exposure <linear>` and `--display-gamma <value>`;
  - contains `postExposureSet`, `displayGammaSet`, `postExposure`, and
    `displayGamma` options;
  - parses finite floats;
  - rejects out-of-range values visibly;
  - sets `postProcessSettings.exposure` and `postProcessSettings.gamma`;
  - calls `ApplyPostProcessSettings()` only inside an explicit
    `applyPostProcessSettings` path.
- Runtime smoke:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --post-exposure 1.25 --display-gamma 2.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
```

- Default visual gate remains unchanged:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerShadowSmoke`
  - `ShadowVisualGoldenValidation`
  - `ImageCompareValidation`

## 8. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation"
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --post-exposure 1.25 --display-gamma 2.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Accidentally changing default smoke/golden output by applying settings when no
  explicit post-process controls were provided.
- Numeric parser accepting NaN/Inf or silently clamping unsafe values.
- Confusing post-process exposure with HDRI loader exposure.
- Applying settings twice when operator and numeric controls are combined.

## 10. Acceptance Criteria

- ModelViewer accepts `--post-exposure` and `--display-gamma`.
- Invalid, NaN/Inf, or out-of-range values fail visibly.
- Explicit post-process controls apply through one
  `SceneRenderer::ApplyPostProcessSettings()` path.
- Default visual gates pass without golden changes.
- Combined ACES/exposure/gamma non-golden smoke passes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Code review: pending
- Commit message: `feat(samples): expose tonemap exposure controls`
