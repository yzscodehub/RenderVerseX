# RQ21 - ModelViewer Tone Mapping Operator CLI

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ20 - Tone Mapping Runtime Operator Settings

## 1. Stage Decision

Expose the RQ20 tone mapping operator settings contract at the ModelViewer sample
boundary through an opt-in CLI flag. This gives artists/developers a practical
way to inspect ACES/Reinhard/Neutral/Uncharted2 output without changing the
engine default visual baseline.

The default ModelViewer behavior must remain unchanged: if `--tonemap` is not
provided, ModelViewer leaves `SceneRenderer`'s runtime default post-process
settings untouched, which currently means `ToneMappingOperator::None`.

## 2. Current State

- RQ20 moved `ToneMappingOperator` into `ToneMappingTypes.h`.
- `PostProcessSettings::toneMappingOperator` defaults to
  `ToneMappingOperator::ACES` for shared/standalone use.
- `SceneRenderer::MakeDefaultRuntimePostProcessSettings()` explicitly sets
  `ToneMappingOperator::None` to preserve existing smoke/golden output.
- ModelViewer already exposes shadow quality through a CLI preset pattern:
  parser, help text, config factory, post-initialization application, source
  guard, and optional smoke validation.
- ModelViewer does not yet expose tone mapping operator selection, so users
  cannot opt into ACES/Neutral/Reinhard from the sample without code edits.

## 3. Scope

1. Add a ModelViewer CLI flag:
   `--tonemap <default|none|reinhard|reinhard-extended|aces|uncharted2|neutral>`.
2. Add a small sample-side selection enum/helper, or equivalent option state,
   that distinguishes "default / leave renderer settings untouched" from an
   explicit operator selection.
3. Parse and validate the flag with visible failure on invalid values.
4. After `engine.Initialize()`, if an explicit non-default tonemap selection was
   requested:
   - copy `sceneRenderer->GetPostProcessSettings()`;
   - set `settings.toneMappingOperator`;
   - call `sceneRenderer->ApplyPostProcessSettings(settings)`;
   - log the selected operator.
5. If `--tonemap default` or no flag is used, do not call
   `ApplyPostProcessSettings()` for tonemap just to restate defaults.
6. Add source guard coverage in `PipelineCacheValidation` for help text, parser
   accepted values, settings copy/apply path, and no default visual change.
7. Add one non-golden ModelViewer smoke command with `--tonemap aces` to verify
   the runtime path executes.
8. Keep existing default visual/golden gates unchanged.

## 4. Out of Scope

- Changing the default ModelViewer tonemap operator from `None` to `ACES`.
- Recapturing golden images.
- Adding exposure/gamma/color grading CLI.
- Adding UI controls.
- Changing tone mapping shader math, adding AgX, or changing post-process order.
- Promoting tonemap-specific smoke to permanent CTest in this stage.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq21-modelviewer-tonemap-operator-cli-plan.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ21 scope.
2. Add a sample-side tonemap selection representation and names.
3. Add `--tonemap` help text.
4. Add parser support for:
   - `default`
   - `none`
   - `reinhard`
   - `reinhard-extended`
   - `aces`
   - `uncharted2`
   - `neutral`
5. Store the selection in `ModelViewerOptions` without changing the default path.
6. Apply the explicit operator after engine initialization via
   `SceneRenderer::GetPostProcessSettings()` and `ApplyPostProcessSettings()`.
7. Log explicit selections and warn if `SceneRenderer` is unavailable.
8. Add `PipelineCacheValidation` source guards.
9. Build and run validation gates.
10. Send the implementation diff to Spark `gpt-5.5` xhigh for code review.
11. Commit implementation only after Spark review PASS.
12. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation`: source guard that ModelViewer:
  - documents `--tonemap <default|none|reinhard|reinhard-extended|aces|uncharted2|neutral>`;
  - parses all accepted values;
  - maps explicit selections to `ToneMappingOperator`;
  - copies `sceneRenderer->GetPostProcessSettings()`;
  - calls `sceneRenderer->ApplyPostProcessSettings(postProcessSettings)`;
  - does not force a tonemap operator when the default selection is used.
- Runtime smoke:

```powershell
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
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
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- Accidentally changing default smoke/golden output by applying a tonemap setting
  even when no CLI flag is provided.
- Applying post-process settings before `SceneRenderer` has initialized its stack.
- Parser aliases drifting from help text.
- Treating sample CLI as an engine-wide quality preset system; this stage should
  remain a thin sample/runtime exposure.

## 10. Acceptance Criteria

- ModelViewer accepts `--tonemap` with the approved value set.
- Invalid `--tonemap` values fail visibly.
- Explicit non-default selections are applied through
  `SceneRenderer::ApplyPostProcessSettings()`.
- No default visual golden update is required.
- Non-golden ACES smoke passes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Cicero, gpt-5.5 xhigh)
- Code review: pending
- Commit message: `feat(samples): expose model viewer tonemap operators`
