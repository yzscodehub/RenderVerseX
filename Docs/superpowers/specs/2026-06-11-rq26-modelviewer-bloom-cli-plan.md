# RQ26 - ModelViewer Bloom Runtime Controls

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ25 - Bloom Wide-Kernel Quality Pass

## 1. Stage Decision

Expose the engine Bloom settings at the ModelViewer sample boundary so artists
and developers can inspect the RQ25 wider Bloom kernel without editing code.
This is a runtime-control and verification stage: it does not change the engine
default visual baseline and does not implement multi-mip Bloom.

Add opt-in flags:

- `--bloom-intensity <value>`
- `--bloom-threshold <value>`
- `--bloom-radius <texels>`

The default ModelViewer path remains unchanged.

## 2. Current State

- `PostProcessSettings` already has `enableBloom`, `bloomThreshold`,
  `bloomIntensity`, and `bloomRadius`.
- `SceneRenderer` default runtime settings keep Bloom enabled but set
  `bloomIntensity = 0.0f`, preserving default visual/golden behavior.
- RQ25 upgraded `Bloom.hlsl` to a wider single-pass kernel and added a shader
  zero-intensity pass-through.
- ModelViewer exposes tone mapping, exposure, gamma, and shadow quality controls
  but does not expose Bloom controls.

## 3. Scope

1. Add ModelViewer option fields:
   - `bool bloomIntensitySet`
   - `bool bloomThresholdSet`
   - `bool bloomRadiusSet`
   - `float bloomIntensity`
   - `float bloomThreshold`
   - `float bloomRadius`
2. Add help text:
   - `--bloom-intensity <value>` with range `[0.0, 16.0]`
   - `--bloom-threshold <value>` with range `[0.0, 64.0]`
   - `--bloom-radius <texels>` with range `[0.0, 16.0]`
3. Reuse the existing full-token finite `ParseFloat()` helper.
4. Add visible validation for each Bloom flag range.
5. Extend the single `SceneRenderer::ApplyPostProcessSettings()` path:
   - include Bloom flags in `applyPostProcessSettings`;
   - set `postProcessSettings.enableBloom = true` when any Bloom flag is
     explicitly provided;
   - copy explicit Bloom values into `PostProcessSettings`;
   - preserve defaults for unspecified Bloom values.
6. Extend the combined post-process log line with Bloom intensity, threshold,
   and radius.
7. Include Bloom flags in the "SceneRenderer unavailable" warning condition.
8. Add `PipelineCacheValidation` source guards for help text, fields, parsing,
   range checks, settings application, logging, and single apply path.
9. Add one non-golden ModelViewer smoke command that exercises the RQ25 Bloom
   path with `--tonemap aces --bloom-intensity --bloom-threshold --bloom-radius`.
10. Keep existing default visual/golden gates unchanged.

## 4. Out of Scope

- Multi-mip downsample/upsample Bloom.
- Bloom soft-knee public setting.
- Dirt masks, anamorphic streaks, lens ghosts, starbursts, or temporal Bloom.
- Auto exposure or luminance histograms.
- Changing default Bloom intensity or golden images.
- Adding a full UI panel; this stage is CLI only.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq26-modelviewer-bloom-cli-plan.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ26 scope.
2. Add Bloom option fields to `ModelViewerOptions`.
3. Add help text for the three flags and ranges.
4. Add parser branches using `ParseFloat()`.
5. Add range validation and visible errors.
6. Extend the existing `applyPostProcessSettings` boolean to include Bloom flags.
7. In the single post-process settings copy, enable Bloom when any Bloom flag is
   explicit and copy only provided values.
8. Update logging for explicit post-process controls.
9. Add source guards.
10. Build and run validation gates.
11. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
12. Commit implementation only after Spark review PASS.
13. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation` source guards:
  - help text contains the three Bloom flags and ranges;
  - option fields exist;
  - parser checks range errors;
  - `applyPostProcessSettings` includes Bloom flags;
  - explicit Bloom flags set `enableBloom`, `bloomIntensity`,
    `bloomThreshold`, and `bloomRadius`;
  - the combined log includes Bloom values;
  - only one `ApplyPostProcessSettings()` call remains.
- Runtime smoke:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --bloom-intensity 0.75 --bloom-threshold 0.25 --bloom-radius 2.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
```

- Expected-failure validation:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-intensity 17.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-threshold -1.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-radius 17.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation
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
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --bloom-intensity 0.75 --bloom-threshold 0.25 --bloom-radius 2.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
& build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-intensity 17.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }
& build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-threshold -1.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }
& build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --bloom-radius 17.0 --backend dx11 --width 320 --height 180 --frames 1 --no-ibl --validation; if ($LASTEXITCODE -eq 0) { exit 1 } else { exit 0 }
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Accidentally changing default visual output by setting nonzero Bloom without a
  flag.
- Bloom threshold/radius controls applying without making the effective runtime
  state visible in logs.
- CLI range choices being too broad; ranges stay conservative for this sample
  control stage.

## 10. Acceptance Criteria

- ModelViewer accepts the three Bloom flags and rejects invalid values visibly.
- Explicit Bloom settings apply through the same single
  `SceneRenderer::ApplyPostProcessSettings()` path.
- Runtime log exposes effective Bloom intensity, threshold, and radius.
- Default visual gates pass without golden changes.
- Bloom runtime smoke passes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Code review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Commit message: `feat(samples): expose bloom runtime controls`
