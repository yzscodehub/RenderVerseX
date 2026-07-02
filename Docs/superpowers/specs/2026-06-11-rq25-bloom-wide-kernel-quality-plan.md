# RQ25 - Bloom Wide-Kernel Quality Pass

Date: 2026-06-11
Program: Render Quality & Verification Program v2
Parent stage: RQ24 - ModelViewer Camera EV100 Exposure CLI

## 1. Stage Decision

Improve the existing HDR Bloom pass from the current tiny cross-sample
approximation to a wider, normalized fullscreen bloom kernel. This is a visual
quality step for bright HDR highlights while keeping the same graphics pipeline,
descriptor layout, render graph shape, and default ModelViewer golden output.

This stage deliberately avoids a full mip-chain bloom implementation. The engine
already labels the current Bloom path as a minimum approximation; RQ25 improves
that path without turning it into a larger render graph or RHI layout change.

## 2. Current State

- `SceneRenderer` default runtime post-process settings enable bloom but force
  `bloomIntensity = 0.0f`, preserving the default visual baseline.
- `BloomPass` runs before tone mapping in the HDR domain and uses the shared
  fullscreen post-process pipeline/resources.
- `Bloom.hlsl` currently performs one center threshold sample plus four axial
  threshold samples, then composites into the scene.
- The shader comment explicitly says it is a "deliberately small one-pass bloom
  approximation" and not a mip-chain blur.
- Existing tests verify Bloom support/resource honesty, graph pass execution,
  draw order, descriptor binding, and HDR-domain ordering.

## 3. Scope

1. Keep `BloomPass` as a single fullscreen graphics pass using the existing
   descriptor layout (`b0`, `t1`, `s2`) and pipeline cache entry.
2. Update `Render/Shaders/PostProcess/Bloom.hlsl` to:
   - retain soft-threshold extraction;
   - add an explicit zero-intensity pass-through branch;
   - replace the current 5-tap cross kernel with a normalized wider kernel
     (target: 13 taps: center, first ring axial/diagonal, second ring axial);
   - keep output in HDR scene color for downstream tone mapping.
3. Keep public `PostProcessSettings` unchanged:
   - `bloomThreshold`
   - `bloomIntensity`
   - `bloomRadius`
4. Add source guards proving:
   - the shader still uses soft thresholding;
   - zero intensity returns the scene color;
   - the wider kernel has first-ring and second-ring taps;
   - the old comment no longer claims "tiny" / "minimum" behavior.
5. Run the existing Bloom/PostProcess validation gates and default visual gates.

## 4. Out of Scope

- Multi-mip downsample/upsample bloom.
- Additive blending or multi-input Bloom descriptor layouts.
- Bloom dirt masks, anamorphic streaks, lens ghosts, starburst, or temporal
  stability work.
- ModelViewer Bloom CLI controls.
- Default visual output changes or golden recapture.
- Auto exposure or luminance histogram.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq25-bloom-wide-kernel-quality-plan.md`
- `Render/Shaders/PostProcess/Bloom.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Implementation Steps

1. Before editing, re-read this document and confirm RQ25 scope.
2. Update Bloom shader comments to describe the upgraded wide-kernel path.
3. Refactor threshold sampling into a helper such as `SampleBloomThreshold()`.
4. Add a zero-intensity pass-through guard in `PSMain`.
5. Implement the normalized wider kernel using `InvTextureSize * Radius`.
6. Preserve single input texture, single output render target, and one draw call.
7. Add `PipelineCacheValidation` source guards for the shader contract.
8. Build and run focused validation.
9. Run default visual/golden gates to prove defaults are unchanged.
10. Send implementation diff to Spark `gpt-5.5` xhigh for code review.
11. Commit implementation only after Spark review PASS.
12. Update `phase-log.md` and commit the log separately.

## 7. Required Tests

- `PipelineCacheValidation` source guards:
  - Bloom shader contains `SampleBloomThreshold`;
  - Bloom shader contains `Intensity <= 0.0`;
  - Bloom shader returns scene color in the zero-intensity path;
  - Bloom shader contains `ring1` and `ring2` tap groups or equivalent labels;
  - Bloom shader no longer contains the old "tiny fullscreen-neighborhood" text.
- Existing focused tests:
  - `RenderPassValidation` Bloom and PostProcessStack tests;
  - `PipelineCacheValidation` shader/pipeline tests.
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

- Shader quality change accidentally affects default visual output if runtime
  bloom intensity is not actually zero.
- Wider sampling might make Bloom too expensive for low-end paths; this stage
  stays single pass and keeps future quality presets out of scope.
- Source guards could overfit shader text; keep them focused on visible contract
  markers rather than exact weight values.

## 10. Acceptance Criteria

- Bloom still executes as one fullscreen pass with the existing pipeline/layout.
- Nonzero Bloom uses a wider normalized thresholded kernel.
- Zero-intensity Bloom returns the scene color in shader source.
- Existing Bloom/PostProcess tests pass.
- Default visual gates pass without golden changes.
- Spark plan review and Spark code review both return PASS.

## 11. Spark Review

- Plan review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Code review: PASS (Spark/Mill, gpt-5.5 xhigh)
- Commit message: `feat(render): widen bloom quality kernel`
