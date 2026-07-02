# R9e - Bloom Fullscreen Minimum Path Plan

Date: 2026-06-06

## Source Of Truth

- Parent plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- Parent R9 lines checked: around 485-497
- Previous phase log: `Docs/superpowers/specs/phase-log.md`, latest completed phase `R9d - ToneMapping Fullscreen Minimum Path`

R9 remaining items after R9d:

- Bloom and/or TAA minimum path as selected in the phase plan.
- IBL minimum path.
- Remaining pass resource declarations through RenderGraph.
- Stub passes implement-or-disable.

## Decision

R9e implements the Bloom minimum visual path first. TAA and IBL remain out of scope for this stage.

Reasoning:

- R9d already created a post-process fullscreen descriptor/pipeline foundation.
- `BloomPass` is currently a requested post-process effect that contains an empty RenderGraph execute body after an unsupported gate. This is exactly the kind of fake implementation R9 is meant to remove.
- TAA has wider prerequisites: jittered camera projection, motion vectors/velocity output, history textures, reprojection, and resolve/sharpening state. It should remain honest-disabled until those dependencies are planned together.
- IBL touches sky/environment resources, material lighting inputs, and skybox/BRDF LUT infrastructure. It should be a separate R9 substage.

R9e intentionally does not integrate `PostProcessStack` into `SceneRenderer`'s active default frame path. Golden images and ModelViewer output are expected to remain stable because the new Bloom path is validated directly and through stack chaining tests, not wired into the default framechain.

## Approved Scope

- Add a Bloom fullscreen shader and pipeline:
  - `Render/Shaders/PostProcess/Bloom.hlsl`
  - Compile Bloom VS/PS in `PipelineCache`.
  - Add `GetBloomPipeline()` to `PipelineCache`.
  - Reuse the existing post-process descriptor set layout: constants at `b0`, input texture at `t1`, sampler at `s2`.
  - Create a no-input-layout fullscreen graphics pipeline with depth disabled and one color target.
  - Add Bloom shader/pipeline hashes to the manifest so Bloom changes are visible and stale cache metadata is invalidated.
- Convert `BloomPass` from unsupported stub to resource-gated executable minimum path:
  - Add `SetResources(PipelineCache*, ResourceViewCache*)`.
  - Remain unsupported until valid resources and Bloom pipeline/layout are available.
  - Allocate a pass-local constant buffer and sampler lazily.
  - Declare real RenderGraph read/write usage.
  - Execute a fullscreen triangle draw.
  - If runtime resources, views, descriptors, mapping, or graph texture resolution fail, log and skip draw rather than reporting success.
- Keep Bloom honest as a minimum path:
  - The shader performs a one-pass threshold/soft-knee additive bright composite over the input scene color.
  - No mip-chain blur is claimed in R9e.
  - `radius` and `mipCount` may be carried through constants/stats for future work but must not imply a full bloom chain is complete.
- Extend validation:
  - PipelineCache creates the Bloom pipeline and follows render target format changes.
  - PipelineCache fails visibly when Bloom shader or pipeline creation fails.
  - Bloom stays disabled/unsupported before resources are injected.
  - Bloom with resources adds a live RenderGraph pass that reads input and writes output.
  - Bloom execute binds the Bloom pipeline, descriptor set, viewport/scissor, and issues `Draw(3)`.
  - Constant mapping failure skips draw and is visible.
  - Descriptor binding/register layout matches `b0/t1/s2`.
  - `PostProcessStack` can chain Bloom before ToneMapping using a transient intermediate.
  - TAA and IBL remain unsupported/disabled without silent work.

## Out Of Scope

- Multi-mip downsample/upsample Bloom chain.
- Gaussian, Kawase, or compute blur.
- Bloom dirt lens, anamorphic streaks, threshold debug views, or HDR auto exposure.
- TAA implementation or velocity/motion-vector integration.
- IBL, skybox, BRDF LUT, irradiance, or prefiltered environment implementation.
- `SceneRenderer` default output integration.
- ECS/Object refactoring or RenderProxy changes.

## Implementation Steps

1. Add `Bloom.hlsl` with a fullscreen triangle VS and a PS using explicit `register(b0, space0)`, `register(t1, space0)`, and `register(s2, space0)`.
2. Extend `PipelineCache` with Bloom shader compile results, shader refs, pipeline ref, stats hash, manifest v3 fields, and `GetBloomPipeline()`.
3. Build the Bloom pipeline with the same post-process layout and graphics state shape as ToneMapping.
4. Update `PipelineCacheValidation` for pipeline counts, descriptor integrity, missing Bloom shader, Bloom pipeline desc, render-target format, pipeline failure, and manifest invalidation.
5. Update `BloomPass` header and implementation with resource injection, lazy resources, RenderGraph declarations, constants upload, descriptor creation, render pass setup, and fullscreen draw.
6. Update `RenderHonestyValidation` so Bloom is no longer treated as an unsupported stub once configured with valid resources.
7. Update `RenderPassValidation` with Bloom direct graph/execute tests and a Bloom-to-ToneMapping `PostProcessStack` chain test.
8. Run required R9 validation and broad render regression.
9. Send Spark code review, address blockers if any, update `phase-log.md`, and commit R9e.

## Validation Commands

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderHonestyValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderHonestyValidation|RenderPassValidation|RenderGraphValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Completion Criteria

- `BloomPass` is no longer a silent TODO when configured with valid resources.
- Enabled Bloom declares real RenderGraph read/write usage and issues a fullscreen draw.
- PipelineCache reports missing/failed Bloom shader or pipeline work as visible initialization failure.
- Pipeline manifest invalidates when Bloom shader or pipeline metadata changes.
- Bloom does not claim a full mip-chain blur implementation.
- TAA and IBL remain visibly unsupported/disabled.
- Default `SceneRenderer`/ModelViewer/golden output remains unchanged in R9e.
- `RenderPassValidation`, `PipelineCacheValidation`, `RenderHonestyValidation`, `RenderGraphValidation`, `RenderSceneValidation`, `ModelViewerSmoke`, `VisualGoldenValidation`, and broad render regression pass.
