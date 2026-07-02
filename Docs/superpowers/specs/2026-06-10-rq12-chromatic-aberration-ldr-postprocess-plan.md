# RQ12 - ChromaticAberration LDR Post-Process Activation

Date: 2026-06-10
Program: Render Quality & Verification Program v2
Previous stage: RQ11 - ColorGrading LDR Post-Process Activation

## 1. Stage Decision / Goal

Turn `ChromaticAberrationPass` from an unsupported compute TODO into a real fullscreen LDR graphics post-process pass.
This extends the verified post-ToneMapping lens-effect chain while keeping runtime visuals stable by leaving the effect
disabled by default in `SceneRenderer`.

The supported RQ12 path is a simple RGB radial channel-separation pass:

```text
Bloom -> ToneMapping -> ColorGrading -> ChromaticAberration -> Vignette -> FXAA
```

RQ12 does not implement spectral sampling, artist presets, UI controls, temporal behavior, or default visual tuning.

## 2. Current State / Evidence

- `Render/Private/PostProcess/ChromaticAberration.cpp` marks the pass unsupported with
  `"Chromatic aberration shader and compute pipeline are not implemented"` and adds a compute graph TODO.
- `Render/Include/Render/PostProcess/ChromaticAberration.h` sets priority `860`, which places the effect before
  ToneMapping even though the current post-process domain rules require lens LDR effects to run after ToneMapping.
- There is no `Render/Shaders/PostProcess/ChromaticAberration.hlsl`.
- `PipelineCache` compiles fullscreen graphics pipelines for Bloom, ToneMapping, ColorGrading, Vignette, and FXAA, but
  does not compile/cache/hash/manifest a ChromaticAberration pipeline.
- `PostProcessStack` only whitelists `ColorGrading`, `FXAA`, and `Vignette` as legal LDR post-ToneMapping effects.
- `SceneRenderer` explicitly disables `settings.enableChromaticAberration = false`, but does not add or resource-inject a
  ChromaticAberration pass.
- `RenderHonestyValidation` currently treats `ChromaticAberrationPass` as part of the unsupported stub group.

## 3. Scope

1. Add a fullscreen graphics shader.
   - New file: `Render/Shaders/PostProcess/ChromaticAberration.hlsl`.
   - Use the existing post-process descriptor convention: constants at `b0`, input texture at `t1`, sampler at `s2`,
     all in `space0`.
   - Provide `VSMain` fullscreen triangle and `PSMain`.
   - Implement simple RGB channel offsets around the screen center with optional radial falloff.
   - Use `useSpectral == false` only; spectral sampling remains unsupported in RQ12.

2. Convert `ChromaticAberrationPass` into a real LDR fullscreen pass.
   - Change priority to an LDR post-ToneMapping slot after ColorGrading and before Vignette/FXAA, for example `935`.
   - Add `SetResources(PipelineCache*, ResourceViewCache*)`.
   - Allocate/upload an aligned constant buffer and linear clamp sampler.
   - Add a graphics RenderGraph pass that reads input as SRV, writes output as render target, binds the
     ChromaticAberration pipeline, and draws a fullscreen triangle.
   - Capture configuration at graph construction time.
   - Do not skip graph emission solely because intensity is `0.0f`; an explicitly enabled zero-intensity pass must be a
     pass-through fullscreen pass so the chain remains deterministic and testable.

3. Keep unsupported requests honest.
   - Before resources are injected, `IsSupported() == false`.
   - `useSpectral == true` is visibly unsupported and schedules no graph pass.
   - Missing pipeline, missing layout, missing view cache, failed constant-buffer creation, failed sampler creation, failed
     SRV/RTV resolution, failed descriptor set creation, and failed constant mapping must not silently report success.

4. Wire PipelineCache.
   - Compile ChromaticAberration VS/PS from the new shader.
   - Add `GetChromaticAberrationPipeline()` and `GetChromaticAberrationPipeline(RHIFormat)`.
   - Add manifest fields for VS hash, PS hash, and pipeline hash; bump the manifest schema version.
   - Add render-target-format-specific pipeline creation for LDR output formats.
   - Include the new pipeline in stats/hash invalidation and visible failure reporting.

5. Wire PostProcessStack and SceneRenderer.
   - Add `ChromaticAberration` to the LDR post-ToneMapping whitelist.
   - Add the pass to `SceneRenderer::SetupDefaultPostProcess()` and inject `PipelineCache`/`ResourceViewCache`.
   - Keep runtime default `settings.enableChromaticAberration = false`, so existing ModelViewer goldens should remain
     unchanged.

6. Update validation tests.
   - Move `ChromaticAberrationPass` out of the unsupported stub group.
   - Add pass-level tests for resource requirements, supported LDR path, spectral rejection, zero-intensity pass-through,
     constant upload packing, fullscreen draw, and map-failure skip.
   - Add stack tests proving `Bloom -> ToneMapping -> ColorGrading -> ChromaticAberration -> Vignette -> FXAA` is valid
     and uses HDR then LDR intermediates.
   - Add boundary tests proving `ChromaticAberration` before ToneMapping is invalid and schedules no graph pass.
   - Add PipelineCache tests for missing shader visibility, descriptor layout guardrails, runtime format pipeline creation,
     manifest invalidation, and pipeline creation failure visibility.
   - Add SceneRenderer source guard tests proving the pass is wired while its runtime default remains disabled.

## 4. Out of Scope

- Spectral/seven-tap chromatic aberration.
- FilmGrain implementation.
- HDR-domain chromatic aberration.
- Default enabling or tuning of ChromaticAberration.
- New UI/editor settings.
- TAA, SSAO, SSR, DOF, motion blur, volumetrics, compute post-process infrastructure, or a generalized domain graph.
- ModelViewer golden recapture unless an unexpected visual diff appears. The pass is disabled by default.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq12-chromatic-aberration-ldr-postprocess-plan.md`
- `Render/Shaders/PostProcess/ChromaticAberration.hlsl`
- `Render/Include/Render/PostProcess/ChromaticAberration.h`
- `Render/Private/PostProcess/ChromaticAberration.cpp`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- `PipelineCacheValidation` fails visibly when the ChromaticAberration shader is missing.
- `PipelineCacheValidation` proves `ChromaticAberration.hlsl` uses the post-process descriptor layout (`b0/t1/s2`,
  `space0`) and fullscreen `VSMain`/`PSMain`.
- `PipelineCacheValidation` includes ChromaticAberration shader/pipeline hashes in the manifest and invalidates when they
  change.
- `PipelineCacheValidation` creates ChromaticAberration pipelines for default and runtime LDR output formats.
- `PipelineCacheValidation` reports backend pipeline creation failure at the ChromaticAberration pipeline index.
- `RenderPassValidation` proves ChromaticAberration reports unsupported before resources and supported after resources.
- `RenderPassValidation` proves explicit spectral mode is unsupported/rejected and schedules no graph pass.
- `RenderPassValidation` proves zero-intensity ChromaticAberration remains a pass-through fullscreen pass when enabled.
- `RenderPassValidation` proves uploaded constants match the HLSL cbuffer packing.
- `RenderPassValidation` proves map failure skips draw without false success.
- `RenderPassValidation` proves the expanded post-process chain order and HDR/LDR intermediate counts.
- `RenderHonestyValidation` keeps unsupported stubs honest and adds ChromaticAberration-specific resource/spectral checks.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|RenderHonestyValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation|ModelViewerIBLSmoke|ModelViewerShadowSmoke|ShadowVisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|MaterialSystemValidation|ClusteredLightingValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- **Domain/order drift:** the old priority `860` would schedule ChromaticAberration before ToneMapping. RQ12 must move it
  into the LDR chain and add boundary tests.
- **Zero-intensity false no-op:** skipping an explicitly enabled zero-intensity pass would behave differently from Vignette
  and make pass-through behavior harder to validate. The pass should still write output.
- **CBuffer packing:** RQ11 exposed how easy it is to mismatch HLSL constant packing. RQ12 must include an upload-offset
  guardrail.
- **Spectral mode honesty:** the API already exposes `useSpectral`; RQ12 must reject it visibly instead of silently running
  a lower-quality path.
- **Golden stability:** because `SceneRenderer` keeps the pass disabled by default, existing goldens should remain stable.
  Any visual diff is a blocker unless explained and recaptured intentionally.

## 9. Acceptance Criteria

- ChromaticAberration can be explicitly enabled as a real LDR fullscreen graphics pass.
- The default ModelViewer path remains visually stable because ChromaticAberration is disabled by default.
- Unsupported spectral mode is visible and schedules no graph pass.
- Pipeline cache, shader layout, manifest, post-process ordering, constant upload, and failure paths are covered by tests.
- Required validation commands pass.
- Spark plan review PASS before implementation.
- Spark code review PASS after implementation.
- RQ12 implementation and phase-log are committed as separate commits.

## 10. Execution Rule

Before implementation starts, reread this document and use only RQ12 scope as the task list. Do not expand into FilmGrain,
spectral sampling, TAA, SSAO, SSR, DOF, motion blur, volumetrics, default visual tuning, or a generalized post-process
domain graph.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh)
- Code review: PASS (`gpt-5.5`, xhigh)
- Commit message: `feat(render): enable chromatic aberration postprocess pass`
