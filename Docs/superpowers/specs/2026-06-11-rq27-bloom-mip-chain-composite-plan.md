# RQ27 - Bloom Mip-Chain Composite Path

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ26 - ModelViewer Bloom Runtime Controls

## 1. Stage Decision

Replace the current single-pass Bloom approximation with a small multi-pass
Bloom pyramid: extract/downsample bright HDR highlights, copy scene color to the
Bloom output, then additively composite multiple blurred levels back into that
output. This moves Bloom from a local wide kernel toward a production-style
post-process structure while keeping the implementation bounded and verifiable.

RQ27 uses a fixed 3-level transient pyramid. Dynamic quality presets, compute
downsample, and full artist-facing Bloom quality controls are deferred.

## 2. Current State

- RQ25 widened the single-pass Bloom shader but Bloom still samples only around
  the current pixel.
- RQ26 exposed Bloom intensity/threshold/radius in ModelViewer, making the Bloom
  path inspectable.
- `BloomPass::AddToGraph()` currently schedules one fullscreen graphics pass.
- `PipelineCache` exposes one opaque Bloom graphics pipeline using the shared
  post-process descriptor layout (`b0`, `t1`, `s2`).
- RHI supports additive blend states and the pipeline hash includes blend state.
- RenderGraph supports transient textures and sequential graphics passes.
- `ResourceViewCache` can create arbitrary texture views through
  `GetTextureView()`, but RQ27 can use separate transient textures per pyramid
  level instead of subresource views.

## 3. Scope

1. Keep the shared post-process descriptor layout:
   - `b0`: Bloom constants
   - `t1`: one input texture
   - `s2`: sampler
2. Extend the Bloom shader with explicit modes:
   - copy scene
   - extract thresholded highlights
   - downsample/blur an existing Bloom level
   - additive composite Bloom level
3. Add an additive Bloom graphics pipeline variant in `PipelineCache`:
   - same shaders/layout as Bloom;
   - additive color blending;
   - alpha preserved with `srcAlpha = Zero`, `dstAlpha = One`;
   - distinct cached state hash via the existing blend-state hash.
4. Update `BloomPass::AddToGraph()`:
   - when `bloomIntensity <= 0`, schedule only a copy-scene pass;
   - when Bloom is active, create a fixed 3-level transient pyramid at half,
     quarter, and eighth resolution (clamped to at least 1x1);
   - schedule extract, downsample, copy-scene, and additive composite passes;
   - additive composite passes use `RHILoadOp::Load` so they accumulate over the
     copied scene output;
   - retain one input texture per pass and one output render target per pass.
5. Keep public `PostProcessSettings` unchanged.
6. Preserve default visual/golden behavior: SceneRenderer defaults still use
   `bloomIntensity = 0.0f`, so the Bloom pass copies scene color only.
7. Add tests/source guards proving the new graph shape, additive pipeline state,
   shader modes, and default visual stability.

## 4. Out of Scope

- Dynamic Bloom quality presets or configurable level count.
- Compute downsample/upsample.
- Dual-input descriptor layouts.
- True separable Gaussian blur, bicubic upsample, lens dirt, anamorphic streaks,
  lens ghosts, starburst, or temporal Bloom.
- Auto exposure or luminance histogram.
- ModelViewer UI beyond RQ26 CLI controls.
- Golden recapture.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq27-bloom-mip-chain-composite-plan.md`
- `Render/Shaders/PostProcess/Bloom.hlsl`
- `Render/Include/Render/PostProcess/Bloom.h`
- `Render/Private/PostProcess/Bloom.cpp`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ27 scope.
2. Add Bloom shader mode constants and mode-dependent output paths.
3. Extend Bloom constants with a mode scalar while preserving the existing
   descriptor layout.
4. Add a `PipelineCache::GetBloomAdditivePipeline(RHIFormat)` path.
5. Build the additive pipeline from the Bloom pipeline desc with additive color
   blending and alpha preserve blend factors.
6. Refactor `BloomPass` pass scheduling into a helper that can add copy,
   extract, downsample, and additive composite fullscreen passes.
7. Create three transient pyramid textures using `RHITextureDesc::RenderTarget`.
8. Schedule active Bloom as:
   - `BloomExtract`
   - `BloomDownsample1`
   - `BloomDownsample2`
   - `BloomCopyScene`
   - `BloomComposite2`
   - `BloomComposite1`
   - `BloomComposite0`
9. Schedule zero-intensity Bloom as:
   - `BloomCopyScene`
10. Update focused tests:
   - single-pass Bloom test becomes multi-pass active Bloom test;
   - add zero-intensity copy-only test;
   - add additive pipeline creation/hash/blend-state guard.
11. Update source guards for Bloom shader modes and old single-pass wording.
12. Build and run focused validation and default visual gates.
13. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
14. Commit implementation only after Spark review PASS.
15. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `RenderPassValidation`:
  - active Bloom schedules 7 graph passes and draws 7 fullscreen triangles;
  - first 4 Bloom draws use the opaque Bloom pipeline;
  - last 3 composite draws use the additive Bloom pipeline;
  - composite render passes use `RHILoadOp::Load`;
  - downsample pyramid render areas are half, quarter, and eighth resolution;
  - zero-intensity Bloom schedules one copy-scene pass and no additive pass.
- `PipelineCacheValidation`:
  - Bloom additive pipeline uses blend enabled;
  - color blend is One + One;
  - alpha blend preserves destination alpha (`Zero`, `One`);
  - additive and opaque Bloom pipelines are distinct cached pipelines;
  - Bloom shader contains explicit mode markers for copy/extract/downsample/
    additive composite.
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
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --tonemap aces --bloom-intensity 0.75 --bloom-threshold 0.25 --bloom-radius 2.0 --backend dx11 --width 320 --height 180 --frames 4 --no-ibl --validation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 9. Risks

- RenderGraph does not expose a render-target readwrite usage. Additive
  composites declare an explicit destination read dependency plus a render-target
  write with `RHILoadOp::Load`; tests must prove pass order and load op.
- Additive blending can damage alpha if alpha factors are not preserved.
- More Bloom passes increase runtime cost; fixed 3-level pyramid keeps RQ27
  bounded until quality presets exist.
- Default visual output depends on the zero-intensity copy path exactly
  preserving scene color.

## 10. Acceptance Criteria

- Active Bloom uses a transient 3-level pyramid and additive composites.
- Zero-intensity/default Bloom copies scene color only.
- Additive Bloom pipeline is distinct, cached, and preserves destination alpha.
- Existing post-process descriptor layout remains unchanged.
- Focused tests and default visual gates pass.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Code review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Commit message: `feat(render): composite bloom mip chain`
