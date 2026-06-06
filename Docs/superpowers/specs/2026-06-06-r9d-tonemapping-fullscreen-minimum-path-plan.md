# R9d - ToneMapping Fullscreen Minimum Path Plan

Date: 2026-06-06

## Plan Source

- Parent plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `15. R9 - Render Pass Completion`
- R9 remaining items checked before this plan:
  - Tone mapping.
  - Bloom and/or TAA minimum path as selected in the phase plan.
  - IBL minimum path.
  - Pass resource declarations through RenderGraph.
  - Stub passes implement-or-disable.
- Latest completed substage: R9c, committed as `a7d9274`.

## Current Code Facts

- `SceneRenderer` currently registers depth, shadow, opaque, skybox, and transparent passes only.
- `PostProcessStack` exists but is not part of the default `SceneRenderer` pass chain.
- `ToneMappingPass` and `BloomPass` have HLSL files, but their C++ passes are marked unsupported.
- `ToneMappingPass::AddToGraph()` currently declares an input read and output render target write, but its execute callback is a TODO.
- `PostProcessStack::Execute()` reuses `sceneColor` as the intermediate output for non-last effects and has no real no-effect copy path.
- `PipelineCache` compiles and exposes DefaultLit and DepthOnly pipelines only.
- `ResourceViewCache` can create default SRV and RTV views for RenderGraph resources after graph compilation.

## Decision

R9d implements the ToneMapping minimum visual path first. Bloom, TAA, and IBL remain unsupported or out of scope until later R9 substages.

This is the smallest stage that turns one R9 post-process requirement into a real RenderGraph-declared GPU draw path without pretending the full post-process stack is complete.

R9d intentionally does not integrate `PostProcessStack` into `SceneRenderer`'s active default frame path yet. Golden images and ModelViewer output are expected to remain stable because the new ToneMapping path is validated directly, not wired into the default framechain.

## Approved Scope

- Add a post-process fullscreen pipeline foundation to `PipelineCache`:
  - Compile `PostProcess/ToneMapping.hlsl` vertex and pixel shaders.
  - Create a post-process descriptor set layout with bindings for constants, input texture, and sampler.
  - Create a post-process pipeline layout and ToneMapping graphics pipeline.
  - Expose accessors for the ToneMapping pipeline, layout, descriptor layout, and device.
  - Keep failures visible through `GetLastError()` and initialization failure.
- Make `ToneMappingPass` a supported pass only after explicit resource injection:
  - Add `SetResources(PipelineCache*, ResourceViewCache*)`.
  - Create/reuse a constants upload buffer and linear clamp sampler.
  - Build a per-execution descriptor set for the input SRV, constants, and sampler.
  - Begin a one-color render pass, bind the ToneMapping pipeline and descriptor set, set viewport/scissor, and draw a fullscreen triangle.
  - Skip visibly if any required resource, texture, view, buffer, sampler, descriptor, or pipeline is missing.
- Fix `PostProcessStack` enough for honest chaining:
  - Do not reuse `sceneColor` as the intermediate output for multi-effect chains.
  - Create transient ping-pong textures matching the scene color desc when more than one supported effect is enabled.
  - Keep no-effect execution honest as a logged no-work path unless a copy/blit path is explicitly implemented later.
- Add focused validation:
  - PipelineCache creates the ToneMapping pipeline and descriptor layout.
  - PipelineCache initialization fails visibly when ToneMapping shader creation/pipeline creation fails.
  - ToneMapping stays disabled/unsupported before resources are injected.
  - ToneMapping with resources adds a live RenderGraph pass that reads input and writes output.
  - ToneMapping execute binds pipeline, descriptor set, viewport/scissor, render pass, and issues `Draw(3)`.
  - ToneMapping descriptor set contains constants, input SRV, and sampler before the draw call.
  - PostProcessStack creates distinct transient intermediates for multi-effect chains.
  - PostProcessStack no-effect path is visibly reported as no-work instead of silently pretending a copy happened.
  - Bloom/TAA and other stubs remain unsupported and disabled when requested.

## Out of Scope

- Bloom implementation or mip-chain rendering.
- TAA resolve/history/sharpening implementation.
- IBL, skybox, BRDF LUT, irradiance, or prefiltered environment implementation.
- SceneRenderer default pass-chain integration of PostProcessStack.
- Changing ModelViewer visual output; this stage validates the ToneMapping pass directly so default framechain integration can be planned separately.
- Runtime UI/settings for post-process.
- Cross-backend shader semantic rewrites beyond what is needed for existing ShaderCompiler paths.
- Replacing DefaultLit lighting with clustered buffers.

## Implementation Steps

1. Extend `PipelineCache` state and accessors for ToneMapping shaders, descriptor set layout, pipeline layout, and pipeline.
2. Compile `PostProcess/ToneMapping.hlsl` in `CompileShaders()` and fail initialization on missing/failed shader creation.
3. Build a manual post-process descriptor layout and pipeline layout independent of DefaultLit material layouts.
4. Create a fullscreen no-input-layout ToneMapping pipeline with one color target, no depth target, and default opaque blend.
5. Extend `ToneMappingPass` with resource injection, runtime resource creation, graph declarations, and fullscreen execution.
6. Adjust `PostProcessStack::Execute()` to allocate transient intermediates for multi-pass chains using the input texture description.
7. Update honesty tests so ToneMapping is no longer categorized with unsupported stubs once resources are present.
8. Add/extend validation tests for PipelineCache, ToneMapping graph/execute behavior, and PostProcessStack chaining.
9. Run required R9 validation and broad render regression.
10. Send Spark code review, address blockers if any, update `phase-log.md`, and commit R9d.

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

- `ToneMappingPass` is no longer a silent TODO when configured with valid resources.
- An enabled ToneMapping pass declares real RenderGraph read/write usage and issues a fullscreen draw.
- PipelineCache reports missing/failed ToneMapping shader or pipeline work as visible initialization failure.
- PostProcessStack no longer aliases intermediate output back to the original scene input for multi-effect chains.
- Unsupported post-process features remain visibly unsupported; no pass silently pretends success.
- Required R9 visual gates still pass.
- R9d does not change default `SceneRenderer` output path or golden images; no framechain integration of `PostProcessStack` yet.
