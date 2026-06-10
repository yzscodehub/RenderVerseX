# RQ20 - Tone Mapping Runtime Operator Settings

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent reference: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`

## 1. Stage Decision

Promote the tone mapping operator from an out-of-band `ToneMappingPass` setter into
the shared `PostProcessSettings` contract. This is a small framework stage that
prepares the renderer for cinematic output controls without changing the default
visual baseline.

The stage keeps ModelViewer's default smoke/golden output stable by explicitly
requesting `ToneMappingOperator::None` in `SceneRenderer`'s default runtime
post-process settings. Future stages may expose ACES/Neutral/Reinhard selection
to samples or turn on a filmic default after a golden recapture.

The standalone/shared settings default must remain filmic-ready:
`PostProcessSettings::toneMappingOperator = ToneMappingOperator::ACES`. The
runtime renderer default is a deliberate override to `None` so the current
visual baseline does not move in this stage.

## 2. Current State

- `ToneMappingOperator` already supports `Reinhard`, `ReinhardExtended`, `ACES`,
  `Uncharted2`, `Neutral`, and `None` in `Render/PostProcess/ToneMapping.h`.
- `ToneMappingPass` owns `m_operator` and exposes `SetOperator()` / `GetOperator()`,
  but `ToneMappingPass::Configure()` only applies `enableToneMapping`, `exposure`,
  and `gamma`.
- `PostProcessSettings` has `enableToneMapping`, `exposure`, and `gamma`, but no
  operator field.
- `SceneRenderer::SetupDefaultPostProcess()` applies default settings and then
  manually calls `m_toneMappingPostProcess->SetOperator(ToneMappingOperator::None)`.
- This creates a split control path: settings drive most post-process state, while
  the tonemap curve is a hidden renderer-side override.

## 3. Scope

1. Add `ToneMappingOperator toneMappingOperator` to `PostProcessSettings`.
2. Create `Render/PostProcess/ToneMappingTypes.h`, move `ToneMappingOperator`
   there, and include that small shared header from both `PostProcessStack.h`
   and `ToneMapping.h`. `PostProcessStack.h` must not include `ToneMapping.h`.
3. Update `ToneMappingPass::Configure()` to apply `settings.toneMappingOperator`.
4. Move the default runtime pass-through decision into
   `MakeDefaultRuntimePostProcessSettings()` by setting
   `settings.toneMappingOperator = ToneMappingOperator::None`.
5. Remove the renderer-side out-of-band default call to
   `m_toneMappingPostProcess->SetOperator(ToneMappingOperator::None)` if the
   settings path fully replaces it.
6. Add focused validation coverage:
   - `ToneMappingPass::Configure()` changes `GetOperator()`.
   - Shared `PostProcessSettings` default operator is `ToneMappingOperator::ACES`.
   - Source guard that `PostProcessSettings` owns the operator field.
   - Source guard that the SceneRenderer runtime default requests `None`.
   - Source guard scoped to `SetupDefaultPostProcess()` that the old out-of-band
     default override is not present there.
7. Keep default ModelViewer and golden output unchanged.

## 4. Out of Scope

- Changing ModelViewer CLI or UI to expose tonemap operator selection.
- Changing the default runtime operator from `None` to `ACES`.
- Recapturing golden images.
- Changing the HLSL tone mapping math.
- Adding AgX, auto exposure, color grading LUTs, or camera exposure controls.
- Reordering the post-process stack.

## 5. Expected Files

- `Render/Include/Render/PostProcess/PostProcessStack.h`
- `Render/Include/Render/PostProcess/ToneMapping.h`
- `Render/Include/Render/PostProcess/ToneMappingTypes.h`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ20 scope.
2. Create `ToneMappingTypes.h` and move `ToneMappingOperator` out of
   `ToneMapping.h` into that file.
3. Include `ToneMappingTypes.h` from `PostProcessStack.h` and `ToneMapping.h`;
   do not include `ToneMapping.h` from `PostProcessStack.h`.
4. Add `toneMappingOperator` to `PostProcessSettings`, initialized to
   `ToneMappingOperator::ACES`.
5. Update `ToneMappingPass::Configure()` to assign `m_operator` from settings.
6. Update `SceneRenderer` default runtime settings to set
   `ToneMappingOperator::None`.
7. Remove the manual default `SetOperator(None)` call from
   `SetupDefaultPostProcess()`.
8. Add focused tests/source guards.
9. Build and run the validation gates.
10. Send the implementation diff to Spark `gpt-5.5` xhigh for code review.
11. Commit implementation only after Spark review PASS.
12. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `RenderPassValidation`: verify `PostProcessSettings` defaults
  `toneMappingOperator` to `ToneMappingOperator::ACES`.
- `RenderPassValidation`: verify `ToneMappingPass::Configure()` applies
  a non-default `settings.toneMappingOperator` and exposes it through
  `GetOperator()`.
- `PipelineCacheValidation`: source guards for:
  - `PostProcessSettings` contains `toneMappingOperator`.
  - `ToneMappingOperator` lives in `ToneMappingTypes.h` and is included by both
    settings and pass headers.
  - `ToneMappingPass::Configure()` assigns `m_operator`.
  - `MakeDefaultRuntimePostProcessSettings()` sets
    `ToneMappingOperator::None`.
  - `SetupDefaultPostProcess()` does not contain the old manual default
    `SetOperator(ToneMappingOperator::None)` override. This guard must inspect
    the `SetupDefaultPostProcess()` function body specifically, not the whole
    repository.
- Visual gate remains unchanged:
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

- Header dependency cycle if `PostProcessStack.h` includes `ToneMapping.h` while
  `ToneMapping.h` already includes `PostProcessStack.h`. This is avoided by
  moving `ToneMappingOperator` into `ToneMappingTypes.h`.
- Default visual drift if the default operator accidentally becomes `ACES`.
- Standalone/default settings drift if `PostProcessSettings` no longer defaults
  to `ACES`, which would make direct `ToneMappingPass` use less filmic-ready.
- Tests that only inspect the shader would miss the runtime settings split; this
  stage needs pass-level and source-level guards.

## 10. Acceptance Criteria

- Tone mapping operator is part of `PostProcessSettings`.
- `ToneMappingOperator` is declared in `ToneMappingTypes.h` and reused by both
  `PostProcessSettings` and `ToneMappingPass` without a header cycle.
- `PostProcessSettings` defaults to `ToneMappingOperator::ACES`.
- `ToneMappingPass::Configure()` applies operator, exposure, and gamma from one
  settings contract.
- SceneRenderer's default pass-through tonemap decision is encoded in default
  runtime settings, not as an extra hidden override.
- Existing default visual gates pass without golden changes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Cicero, gpt-5.5 xhigh; blocker fixes adopted)
- Code review: pending
- Commit message: `feat(render): route tonemap operator through settings`
