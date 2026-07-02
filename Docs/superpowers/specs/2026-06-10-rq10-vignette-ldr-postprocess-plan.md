# RQ10 - Vignette LDR Post-Process Activation Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ9
Previous stage: RQ9 - FXAA Post-Process Activation

## 1. Stage Decision

Activate Vignette as a real fullscreen LDR post-process pass when explicitly enabled. RQ9 opened the safe
post-ToneMapping LDR slot with FXAA; RQ10 uses that slot for one small visual effect and proves the stack can host more
than one LDR pass without regressing the HDR boundary.

This stage intentionally implements Vignette as a fullscreen graphics pass following ToneMapping/Bloom/FXAA patterns.
It does not build compute post-process infrastructure, and it does not activate ColorGrading, FilmGrain,
ChromaticAberration, DOF, SSAO, SSR, TAA, or VolumetricLighting.

## 2. Current Engine Evidence

- `VignettePass::VignettePass()` marks the pass unsupported with "Vignette shader and compute pipeline are not
  implemented".
- `VignettePass::AddToGraph()` currently adds a compute graph pass whose execute lambda only contains a TODO.
- `VignettePass::GetPriority()` is `850`, which places it before `ToneMappingPass` priority `900`; Vignette is an LDR
  display-domain effect and should not run before ToneMapping.
- `PostProcessStack` currently allows only `FXAA` as an LDR pass after ToneMapping.
- `Render/Shaders/PostProcess/Effects.hlsl` already contains Vignette math, but it is compute-style (`t0/u0/s0`) and
  does not match the current fullscreen graphics post-process descriptor layout (`b0/t1/s2`, space 0).
- `SceneRenderer` runtime defaults keep `enableVignette = false`; RQ10 must not flip that default.

## 3. Scope

1. Add Vignette fullscreen shader and pipeline support.
   - Add `Render/Shaders/PostProcess/Vignette.hlsl` with fullscreen `VSMain`/`PSMain`.
   - Use the established post-process layout: constants at `b0, space0`, input texture at `t1, space0`, sampler at
     `s2, space0`.
   - Reuse the Vignette math from `Effects.hlsl` in pixel-shader form.
   - Compile/store Vignette shaders in `PipelineCache`.
   - Add `GetVignettePipeline(outputFormat)`, `GetOrCreateVignettePipeline(outputFormat)`, and
     `BuildVignettePipelineDesc(outputFormat)`.
   - Add Vignette shader and pipeline hashes to `PipelineCacheStats` and the manifest schema.
   - Fail visibly when the Vignette shader or pipeline cannot be created.

2. Implement `VignettePass` runtime resources.
   - Add `SetResources(PipelineCache*, ResourceViewCache*)`.
   - Allocate/update a constants buffer containing texture size, inverse size, intensity, smoothness, roundness, mode,
     center, aspect ratio, and color.
   - Create/reuse a linear clamp sampler.
   - Resolve input SRV and output RTV through `ResourceViewCache`.
   - Create descriptor set and draw a fullscreen triangle.
   - Retain descriptor sets across frames like Bloom/ToneMapping/FXAA.
   - Report unsupported until all required resources are available.

3. Correct post-process ordering and boundary.
   - Change Vignette priority so it runs after ToneMapping and before FXAA by default. Use a unique value such as `940`
     so it does not collide with future LDR effects such as FilmGrain.
   - Add `Vignette` to the LDR post-ToneMapping whitelist in `PostProcessStack`.
   - Keep HDR effects blocked after ToneMapping.
   - Keep LDR effects blocked before ToneMapping.

4. Wire SceneRenderer resources.
   - Add `VignettePass` to the default post-process stack resource setup.
   - Pass `PipelineCache` and `ResourceViewCache` to Vignette.
   - Do not change `SceneRenderer` runtime default `enableVignette = false`.

5. Tests and documentation.
   - Extend `PipelineCacheValidation` for missing Vignette shader, descriptor-layout source guard, pipeline creation,
     runtime output format, manifest fields, manifest invalidation, and pipeline creation failure.
   - Extend `RenderPassValidation` for unsupported-before-resources, supported-after-resources, fullscreen draw, map
     failure skip, `Bloom -> ToneMapping -> Vignette -> FXAA` valid ordering, invalid LDR-before-ToneMapping blocking,
     and SceneRenderer resource/default guards.
   - Update `RenderHonestyValidation` so Vignette is no longer in the permanent stub list, but remains unsupported before
     resources are provided.
   - Record the stage in `Docs/superpowers/specs/phase-log.md` after commit.

## 4. Out of Scope

- ColorGrading, FilmGrain, ChromaticAberration, DOF, MotionBlur, SSAO, SSR, TAA, VolumetricLighting.
- Compute post-process pipeline infrastructure.
- UI controls or automatic enabling of Vignette in ModelViewer.
- Golden recapture; default runtime setting remains off, so existing goldens should not change.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq10-vignette-ldr-postprocess-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/PostProcess/Vignette.hlsl`
- `Render/Include/Render/PostProcess/Vignette.h`
- `Render/Private/PostProcess/Vignette.cpp`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

## 6. Required Tests

- `PipelineCacheValidation` fails visibly when the Vignette shader is missing.
- `PipelineCacheValidation` proves `Vignette.hlsl` uses `b0/t1/s2`, space 0.
- `PipelineCacheValidation` creates Vignette pipelines for default and runtime LDR output formats and records a non-zero
  Vignette pipeline hash.
- `PipelineCacheValidation` manifest includes Vignette shader/pipeline hashes and invalidates when Vignette shader or
  pipeline hash changes.
- `PipelineCacheValidation` proves the manifest schema version/field count bumps for Vignette VS/PS shader hashes and
  Vignette pipeline hash.
- `RenderPassValidation` proves Vignette reports unsupported before resources and supported after resources.
- `RenderPassValidation` proves Vignette adds a live graph pass, binds the Vignette pipeline/descriptor set, and draws a
  fullscreen triangle.
- `RenderPassValidation` proves enabled Vignette with zero intensity stays in the chain as a pass-through pass, so stack
  intermediates are never left unwritten.
- `RenderPassValidation` proves Vignette skips draw when constants cannot map.
- `RenderPassValidation` proves `Bloom -> ToneMapping -> Vignette -> FXAA` is valid and uses HDR then LDR intermediates.
- `RenderPassValidation` proves Vignette before ToneMapping is invalid and schedules no graph pass.
- `RenderPassValidation` proves HDR-after-ToneMapping remains invalid, for example `ToneMapping -> Bloom` or
  `ToneMapping -> Vignette -> Bloom`, and schedules no graph pass.
- `RenderHonestyValidation` updates the old Vignette stub expectation.
- A guard proves `SceneRenderer` wires Vignette resources while keeping `settings.enableVignette = false`.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|RenderHonestyValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|MaterialSystemValidation|ClusteredLightingValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- Pipeline manifest schema grows again. Tests must prove old manifests become stale and new Vignette hashes are tracked.
- Accidentally enabling Vignette by default would change ModelViewer goldens. Source guards and visual gates must catch
  that.
- Running Vignette before ToneMapping would apply it in HDR domain and break the RQ9 boundary. Priority and stack tests
  must catch that.
- Reusing compute-oriented `Effects.hlsl` directly would require a different descriptor/UAV layout. RQ10 avoids that by
  adding a fullscreen graphics shader.

## 9. Acceptance Criteria

- Vignette is supported and executed when explicitly enabled and resources are available.
- Vignette runs after ToneMapping in LDR domain and before FXAA by priority.
- Invalid Vignette-before-ToneMapping ordering is visible and schedules no graph pass.
- Missing Vignette shader/pipeline resources fail visibly.
- `SceneRenderer` runtime default `enableVignette = false` remains unchanged.
- Focused render, honesty, and ModelViewer visual gates pass without golden recapture.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ10 scope as the task list. Do not expand into
ColorGrading, FilmGrain, ChromaticAberration, compute post-process infrastructure, TAA, SSAO, SSR, or volumetrics.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; blocker fixed: zero-intensity Vignette now pass-through)
- Commit message: `feat(render): enable vignette postprocess pass`
