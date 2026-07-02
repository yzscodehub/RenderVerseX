# RQ23 - Tone Mapping Camera EV100 Exposure Model

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ22 - ModelViewer Tone Mapping Exposure/Gamma CLI

## 1. Stage Decision

Add an engine-level stop-based exposure model to the tone mapping settings. RQ22
exposed the existing linear exposure multiplier in ModelViewer; RQ23 makes the
renderer capable of representing camera-style EV100 exposure and exposure
compensation in `PostProcessSettings`.

This is a framework stage, not an auto-exposure stage. The shader still receives
the same linear `Exposure` constant, but `ToneMappingPass::Configure()` can now
resolve that constant from either:

- `ManualMultiplier`: the existing `PostProcessSettings::exposure` scalar.
- `CameraEV100`: `2^(exposureCompensationEV - cameraEV100)`.

The default path must remain unchanged: `PostProcessSettings` defaults to
`ManualMultiplier` with `exposure = 1.0f`, and SceneRenderer's default visual
baseline remains stable.

## 2. Current State

- `PostProcessSettings` has `exposure = 1.0f`, `gamma = 2.2f`, and
  `toneMappingOperator`.
- `ToneMappingPass::Configure()` copies `settings.exposure` directly to
  `m_exposure`.
- `ToneMapping.hlsl` multiplies HDR scene color by the linear `Exposure` constant
  before the operator.
- ModelViewer can opt into `--post-exposure`, but that is only a sample-level
  linear multiplier and is not a camera exposure model.
- There is no engine representation for EV100 or exposure compensation, so
  future auto exposure/camera physical settings have no shared contract.

## 3. Scope

1. Add `enum class ToneMappingExposureMode : uint8` to `ToneMappingTypes.h`:
   - `ManualMultiplier`
   - `CameraEV100`
2. Extend `PostProcessSettings` with:
   - `ToneMappingExposureMode exposureMode = ToneMappingExposureMode::ManualMultiplier`
   - `float cameraEV100 = 0.0f`
   - `float exposureCompensationEV = 0.0f`
3. Add a local resolver in `ToneMapping.cpp`:
   - manual mode: finite `settings.exposure`, clamped to `[0.0, 65536.0]`, fallback `1.0`.
   - camera EV mode: finite `cameraEV100` and `exposureCompensationEV`; compute
     `pow(2.0, clamp(exposureCompensationEV - cameraEV100, -16.0, 16.0))`;
     fallback `1.0` if invalid.
4. Update `ToneMappingPass::Configure()` to use the resolver for `m_exposure`
   while preserving `m_gamma` and `m_operator` behavior.
5. Add focused `RenderPassValidation` behavior tests:
   - default/manual settings still resolve to exposure `1.0`.
   - manual multiplier still applies explicit scalar exposure.
   - manual multiplier sanitizes NaN/Inf to `1.0`, negative values to `0.0`,
     and high values to `65536.0`.
   - camera EV100 mode resolves stops correctly, e.g. EV100 `2.0`, compensation
     `1.0` -> exposure `0.5`, using tolerant float comparisons.
   - invalid EV inputs fall back to `1.0`.
6. Add `PipelineCacheValidation` source guards for the enum, settings fields,
   resolver, clamp/fallback behavior, and `Configure()` assignment.
7. Keep ModelViewer CLI, shader math, default visuals, and golden images unchanged.

## 4. Out of Scope

- Auto exposure, histogram/luminance passes, eye adaptation, temporal smoothing,
  camera aperture/shutter/ISO, or physical light unit calibration.
- ModelViewer `--camera-ev100` / `--exposure-compensation` CLI.
- Changing default exposure/gamma/operator.
- Shader constant layout changes.
- Golden recapture.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq23-tonemapping-camera-ev100-exposure-plan.md`
- `Render/Include/Render/PostProcess/ToneMappingTypes.h`
- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ23 scope.
2. Add `ToneMappingExposureMode` to `ToneMappingTypes.h`.
3. Add exposure mode, EV100, and compensation fields to `PostProcessSettings`.
4. Add the local resolver in `ToneMapping.cpp` and include required standard
   headers.
5. Update `ToneMappingPass::Configure()` to assign `m_exposure` from the resolver.
6. Add behavior tests and source guards.
7. Build and run validation gates.
8. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
9. Commit implementation only after Spark review PASS.
10. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `RenderPassValidation`:
  - default/manual exposure resolves to `1.0`.
  - explicit manual multiplier remains supported.
  - manual exposure sanitizes NaN/Inf/negative/high values.
  - EV100 exposure resolves via stop math.
  - invalid EV inputs fall back to `1.0`.
- `PipelineCacheValidation` source guards:
  - `enum class ToneMappingExposureMode : uint8` exists in `ToneMappingTypes.h`.
  - `PostProcessSettings` owns `exposureMode`, `cameraEV100`, and
    `exposureCompensationEV` with stable defaults.
  - `ToneMapping.cpp` computes `exposureCompensationEV - cameraEV100`, clamps the
    stop range, uses `std::pow(2.0f, ...)`, and falls back to `1.0f` for invalid
    inputs.
- Default visual gate remains unchanged:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerShadowSmoke`
  - `ShadowVisualGoldenValidation`
  - `ImageCompareValidation`

## 8. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderPassValidation PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|PipelineCacheValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Default visual drift if `exposureMode` defaults to `CameraEV100` or if manual
  exposure default changes.
- Overclaiming physical accuracy: this stage adds a camera-style stop model, not
  a full calibrated physical camera pipeline.
- Hidden invalid values propagating to the GPU; the resolver must sanitize NaN/Inf.
- Tests that inspect only source text would miss resolver behavior; this stage
  needs direct pass behavior tests.

## 10. Acceptance Criteria

- `PostProcessSettings` can represent manual multiplier and Camera EV100 exposure.
- `ToneMappingPass::Configure()` resolves both modes into the existing linear
  shader exposure constant.
- Defaults remain visually stable.
- Focused behavior tests and source guards pass.
- Default visual gates pass without golden changes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh; non-blocking suggestions adopted)
- Code review: pending
- Commit message: `feat(render): add camera ev exposure mode`
