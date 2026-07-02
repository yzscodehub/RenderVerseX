# RQ11 - ColorGrading LDR Post-Process Activation

Date: 2026-06-10
Parent program: Render Quality & Verification Program v2
Prerequisite: RQ10 - Vignette LDR Post-Process Activation

## 1. Stage Decision / Goal

Turn `ColorGradingPass` from a compute stub into a real fullscreen graphics post-process pass in the LDR domain after
ToneMapping. This is the next practical step toward AAA-style presentation because final color shaping, contrast,
saturation, white balance, lift/gamma/gain, channel mixing, hue shift, and split toning are foundational to art-directed
image quality.

RQ11 deliberately implements the manual LDR color-grading path only. It does not build 3D LUT baking, compute
post-process infrastructure, HDR pre-tonemap grading, editor UI, automatic exposure, or default visual tuning.

## 2. Current State / Evidence

- `ColorGradingPass` exists but is marked unsupported in `Render/Private/PostProcess/ColorGrading.cpp` and its
  `AddToGraph()` body schedules a compute pass with no execution.
- `Render/Shaders/PostProcess/ColorGrading.hlsl` contains useful color math, but its resource layout is not compatible
  with the established fullscreen post-process contract (`b0/t1/s2`, space 0), and it still exposes compute and 3D LUT
  bindings.
- `PostProcessSettings::enableColorGrading` defaults to true at the settings-struct level, but
  `SceneRenderer::MakeDefaultRuntimePostProcessSettings()` explicitly sets `settings.enableColorGrading = false;`.
- Runtime post-processing currently wires Bloom, ToneMapping, Vignette, and FXAA resources. ColorGrading is not registered
  in `SceneRenderer::SetupDefaultPostProcess()`.
- RQ9 and RQ10 established the LDR post-ToneMapping pattern: real fullscreen graphics pass, PipelineCache shader/pipeline
  ownership, visible missing-resource behavior, manifest hashes, LDR/HDR boundary tests, SceneRenderer resource injection,
  and default-off runtime baseline.

## 3. Scope

### 3.1 ColorGrading shader and resource contract

- Rewrite or adapt `Render/Shaders/PostProcess/ColorGrading.hlsl` to provide `VSMain` and `PSMain` fullscreen graphics
  entries.
- Use the established post-process descriptor layout:
  - `cbuffer ColorGradingConstants : register(b0, space0)`
  - `Texture2D<float4> InputTexture : register(t1, space0)`
  - `SamplerState LinearSampler : register(s2, space0)`
- Implement manual LDR grading math in the pixel shader:
  - exposure offset in EV or brightness-equivalent offset from settings,
  - white balance temperature/tint,
  - contrast,
  - saturation,
  - hue shift,
  - lift/gamma/gain,
  - channel mixer,
  - split toning.
- Neutral settings must be pass-through and still write the output when the pass is enabled.

### 3.2 PipelineCache integration

- Add ColorGrading VS/PS compilation in `PipelineCache`.
- Add a format-aware `GetColorGradingPipeline(RHIFormat outputFormat)` and `BuildColorGradingPipelineDesc()`.
- Add `colorGradingPipelineHash` to `PipelineCacheStats`.
- Bump the pipeline manifest schema and add ColorGrading VS hash, PS hash, and pipeline hash.
- Extend manifest read/write/match/invalidations and pipeline creation failure visibility.

### 3.3 ColorGradingPass runtime implementation

- Add `SetResources(PipelineCache*, ResourceViewCache*)`.
- Allocate a constant buffer and linear clamp sampler like FXAA/Vignette.
- Capture grading config during graph build and upload captured values during execution.
- Use `RenderGraphPassType::Graphics`, read the input as SRV, write the output as render target, bind descriptor set 0,
  draw fullscreen triangle, and retain descriptor sets across frames.
- Make the supported RQ11 path explicitly LDR:
  - `ColorGradingConfig::mode` should default to `ColorGradingMode::LDR`, or `Configure()` must force the supported runtime
    settings path to LDR.
  - An explicit `SetMode(ColorGradingMode::HDR)` request is unsupported in RQ11. It must be visibly rejected before graph
    scheduling, not silently run through the LDR fullscreen shader.
  - `ColorGradingMode::None` is treated as disabled/no-work.
- Do not return early for neutral/no-op settings after the stack has included the pass. Neutral ColorGrading is a
  pass-through write.
- Keep the existing `BakeToLUT()` API honest: it may remain unsupported and return `nullptr`, but it must not report a
  successful LUT bake.
- Keep 3D LUT runtime application out of RQ11. If `useLUT` is explicitly requested, or `SetLUT(...)` receives a texture,
  the pass must become visibly unsupported/rejected and schedule no graph pass; it cannot silently ignore a requested LUT.

### 3.4 PostProcessStack boundary

- Treat `ColorGrading` as an LDR post-ToneMapping effect.
- Choose a unique priority after ToneMapping and before Vignette/FilmGrain/FXAA. Proposed priority: `930`.
- Preserve invalid boundary behavior:
  - `ColorGrading -> ToneMapping` is invalid for the LDR-only RQ11 path.
  - `ToneMapping -> ColorGrading -> Bloom` remains invalid because Bloom is HDR-domain after an LDR chain.
  - Invalid chains schedule no graph passes.

### 3.5 SceneRenderer wiring

- Add `ColorGradingPass` to the default runtime post-process stack and inject resources.
- Keep `settings.enableColorGrading = false;` in the runtime default settings.
- Existing ModelViewer goldens should not change unless a test explicitly enables ColorGrading.

## 4. Out of Scope

- 3D LUT texture binding, LUT baking, LUT import, LUT authoring, or editor UI.
- HDR pre-tonemap color grading.
- Compute post-process infrastructure or UAV ColorGrading execution.
- ACES/AgX operator changes, auto exposure, exposure histograms, bloom quality changes, TAA, SSAO, SSR, DOF,
  chromatic aberration, film grain, volumetrics, or default art-direction tuning.
- Reordering unrelated passes or changing default ModelViewer appearance.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq11-color-grading-ldr-postprocess-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/PostProcess/ColorGrading.hlsl`
- `Render/Include/Render/PostProcess/ColorGrading.h`
- `Render/Private/PostProcess/ColorGrading.cpp`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

## 6. Required Tests

- `PipelineCacheValidation` fails visibly when the ColorGrading shader is missing.
- `PipelineCacheValidation` proves `ColorGrading.hlsl` uses `b0/t1/s2`, space 0.
- `PipelineCacheValidation` creates ColorGrading pipelines for default and runtime LDR output formats and records a
  non-zero ColorGrading pipeline hash.
- `PipelineCacheValidation` manifest includes ColorGrading shader/pipeline hashes and invalidates when ColorGrading shader
  or pipeline hash changes.
- `PipelineCacheValidation` proves the manifest schema version/field count bumps for ColorGrading VS/PS shader hashes and
  ColorGrading pipeline hash.
- `RenderPassValidation` proves ColorGrading reports unsupported before resources and supported after resources.
- `RenderPassValidation` proves the default/configured supported path is LDR.
- `RenderPassValidation` proves explicit HDR mode is visibly unsupported/rejected and schedules no graph pass.
- `RenderPassValidation` proves ColorGrading adds a live graph pass, binds the ColorGrading pipeline/descriptor set, and
  draws a fullscreen triangle.
- `RenderPassValidation` proves neutral ColorGrading settings still write a pass-through output when enabled.
- `RenderPassValidation` proves uploaded `ColorGradingConstants` match the HLSL cbuffer packing, including the 16-byte
  boundary before `Lift/Gamma/Gain`.
- `RenderPassValidation` proves ColorGrading skips draw when constants cannot map.
- `RenderPassValidation` proves requested LUT mode, including `SetLUT(...)`, is visibly unsupported/rejected and schedules
  no graph pass.
- `RenderPassValidation` proves `Bloom -> ToneMapping -> ColorGrading -> Vignette -> FXAA` is valid and uses HDR then LDR
  intermediates.
- `RenderPassValidation` proves ColorGrading before ToneMapping is invalid and schedules no graph pass.
- `RenderPassValidation` proves HDR-after-LDR remains invalid, for example `ToneMapping -> ColorGrading -> Bloom`, and
  schedules no graph pass.
- `RenderHonestyValidation` updates the old ColorGrading stub expectation and keeps LUT bake honest.
- A guard proves `SceneRenderer` wires ColorGrading resources while keeping `settings.enableColorGrading = false`.

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

- Silent no-op risk: neutral settings or requested LUT must not create an unwritten transient after the stack includes the
  pass.
- Boundary risk: ColorGrading used to default to HDR mode. RQ11 is LDR-only, so the supported config path, priority, and
  boundary tests must prevent it from running before ToneMapping or silently accepting explicit HDR mode.
- Manifest churn: PipelineCache schema grows again and must reject stale manifests predictably.
- Default visual churn: SceneRenderer must wire ColorGrading without enabling it by default.
- Shader layout drift: existing ColorGrading shader uses old bindings; RQ11 must align it with `b0/t1/s2`, space 0.

## 9. Acceptance Criteria

- ColorGrading is supported and executed when explicitly enabled and resources are available.
- ColorGrading's supported path is explicitly LDR; explicit HDR mode and LUT requests are visibly unsupported/rejected.
- ColorGrading runs after ToneMapping in LDR domain and before Vignette/FXAA by priority.
- Neutral settings are pass-through but still write the graph output.
- Invalid ColorGrading-before-ToneMapping and HDR-after-LDR ordering are visible and schedule no graph pass.
- Missing ColorGrading shader/pipeline resources fail visibly.
- Runtime default `enableColorGrading = false` remains unchanged.
- Focused render, honesty, and ModelViewer visual gates pass without golden recapture.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ11 scope as the task list. Do not expand into LUT
baking, HDR grading, compute post-process infrastructure, FilmGrain, ChromaticAberration, TAA, SSAO, SSR, or volumetrics.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; blocker fixes adopted for explicit LDR mode and LUT rejection)
- Code review: PASS (`gpt-5.5`, xhigh; fixed cbuffer packing blocker and added upload-offset guardrail)
- Commit message: `feat(render): enable color grading postprocess pass`
