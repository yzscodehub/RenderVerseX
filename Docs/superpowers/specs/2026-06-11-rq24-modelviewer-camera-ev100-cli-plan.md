# RQ24 - ModelViewer Camera EV100 Exposure CLI

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ23 - Tone Mapping Camera EV100 Exposure Model

## 1. Stage Decision

Expose RQ23's engine-level camera EV100 exposure mode at the ModelViewer sample
boundary. RQ23 made EV100 representable in `PostProcessSettings`; RQ24 lets a
developer inspect that path from the sample without editing code.

Add two opt-in flags:

- `--camera-ev100 <value>`
- `--exposure-compensation <ev>`

The default ModelViewer path remains unchanged. This is a sample/runtime exposure
stage only; it does not add auto exposure, physical camera parameters, or golden
recapture.

## 2. Current State

- `PostProcessSettings` has `ToneMappingExposureMode::ManualMultiplier` and
  `ToneMappingExposureMode::CameraEV100`.
- `ToneMappingPass::Configure()` resolves Camera EV100 as
  `2^(exposureCompensationEV - cameraEV100)` into the existing linear shader
  `Exposure` constant.
- ModelViewer exposes `--post-exposure` as a manual linear exposure multiplier
  and `--display-gamma`.
- ModelViewer does not expose `cameraEV100` or `exposureCompensationEV`.

## 3. Scope

1. Add ModelViewer options:
   - `bool cameraEV100Set`
   - `bool exposureCompensationSet`
   - `float cameraEV100`
   - `float exposureCompensationEV`
2. Add help text:
   - `--camera-ev100 <value>` with range `[-16.0, 32.0]`
   - `--exposure-compensation <ev>` with range `[-16.0, 16.0]`
3. Reuse the RQ22 full-token finite `ParseFloat()` helper.
4. Add visible validation:
   - `--camera-ev100`: finite range `[-16.0, 32.0]`
   - `--exposure-compensation`: finite range `[-16.0, 16.0]`
   - `--camera-ev100` conflicts with `--post-exposure` because they select
     different exposure modes.
   - `--exposure-compensation` requires `--camera-ev100`.
5. In the existing single post-process apply path:
   - if `--camera-ev100` is set, set
     `postProcessSettings.exposureMode = ToneMappingExposureMode::CameraEV100`;
   - set `postProcessSettings.cameraEV100`;
   - set `postProcessSettings.exposureCompensationEV` to the explicit
     compensation value or `0.0f` when omitted;
   - do not assign `postProcessSettings.exposure` in this mode.
6. Keep the existing manual `--post-exposure` path as
   `ToneMappingExposureMode::ManualMultiplier`.
7. Update the combined log line to include exposure mode, EV100, and compensation
   when EV mode is applied.
8. Add `PipelineCacheValidation` source guards for help text, options, validation,
   conflict checks, mode assignment, and single apply path.
9. Add one non-golden ModelViewer smoke command that uses `--tonemap aces`,
   `--camera-ev100`, and `--exposure-compensation`.
10. Keep existing default visual/golden gates unchanged.

## 4. Out of Scope

- Auto exposure, histogram/luminance passes, eye adaptation, temporal smoothing.
- Aperture/shutter/ISO physical camera UI.
- Changing default exposure/operator/gamma.
- Shader changes or constant layout changes.
- Golden recapture.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq24-modelviewer-camera-ev100-cli-plan.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ24 scope.
2. Add EV100 option fields to `ModelViewerOptions`.
3. Add help text for the two flags and ranges.
4. Add parsing branches using `ParseFloat()`.
5. Add post-parse conflict validation:
   - `--camera-ev100` cannot combine with `--post-exposure`;
   - `--exposure-compensation` requires `--camera-ev100`.
6. Extend the existing `applyPostProcessSettings` boolean to include EV mode.
7. In the single post-process settings copy, set either manual exposure mode or
   Camera EV100 mode, never both.
8. Update logging for explicit post-process controls.
9. Add source guards.
10. Build and run validation gates.
11. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
12. Commit implementation only after Spark review PASS.
13. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation` source guards:
  - help text contains `--camera-ev100 <value>` and
    `--exposure-compensation <ev>` with ranges;
  - options fields exist;
  - parser checks range errors;
  - conflict and dependency errors are visible;
  - `postProcessSettings.exposureMode = ToneMappingExposureMode::CameraEV100`
    appears in the EV path;
  - manual `--post-exposure` explicitly uses `ManualMultiplier`;
  - only one `ApplyPostProcessSettings()` call remains.
- Runtime smoke:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --camera-ev100 2.0 --exposure-compensation 1.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
```

- Expected-failure validation:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --post-exposure 1.0 --camera-ev100 2.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --exposure-compensation 1.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation
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
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --camera-ev100 2.0 --exposure-compensation 1.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
& build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --post-exposure 1.0 --camera-ev100 2.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }
& build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --exposure-compensation 1.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Accidentally changing default visual output by enabling EV mode without a flag.
- Allowing both manual multiplier and EV mode to apply at once.
- Treating EV100 support as auto exposure; this stage is only manual EV mode.
- Log output becoming ambiguous when only some post-process fields are explicit.

## 10. Acceptance Criteria

- ModelViewer accepts `--camera-ev100` and `--exposure-compensation`.
- Invalid, out-of-range, conflicting, or dependency-missing combinations fail
  visibly.
- Explicit EV settings apply through the same single
  `SceneRenderer::ApplyPostProcessSettings()` path.
- Default visual gates pass without golden changes.
- EV100 runtime smoke passes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh; non-blocking suggestions adopted)
- Code review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Commit message: `feat(samples): expose camera ev exposure controls`
