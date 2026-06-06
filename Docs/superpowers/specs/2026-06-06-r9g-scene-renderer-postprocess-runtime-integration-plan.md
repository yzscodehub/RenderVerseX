# R9g - SceneRenderer PostProcess Runtime Integration Plan

Date: 2026-06-06

## Source Documents

- Parent plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Parent section: `15. R9 - Render Pass Completion`
- Previous phase log: `Docs/superpowers/specs/phase-log.md`, R9e/R9f notes

## Stage Decision

R9g stays in R9 instead of moving to R10 because the current `SceneRenderer` runtime path still does
not execute the production post-process chain. R9e made Bloom and ToneMapping real graph passes when
resources are injected manually, and R9f completed the IBL-approximate ambient minimum path. The
remaining R9 runtime gap is wiring the already-supported post-process passes into `SceneRenderer` so
ModelViewer can render through the declared pass chain, not only through isolated pass tests.

## Current State

- `SceneRenderer::SetupDefaultPasses()` registers depth prepass, shadow, opaque, skybox, and transparent passes.
- `SceneRenderer::BuildRenderGraph()` imports the swap-chain back buffer and assigns it directly to
  `ViewData::colorTarget` for scene passes.
- `PostProcessStack` exists and can execute enabled supported effects over an input/output texture pair.
- `BloomPass` and `ToneMappingPass` expose `SetResources(PipelineCache*, ResourceViewCache*)` and have
  validation coverage for fullscreen graph execution.
- `PostProcessStack` is not owned or executed by `SceneRenderer`.
- R9e phase-log follow-up explicitly notes that Bloom/ToneMapping resource injection still depends on callers.

## Scope

1. Add runtime post-process ownership to `SceneRenderer`.
   - Add a `PostProcessStack` member.
   - Add cached `BloomPass*` and `ToneMappingPass*` pointers for default runtime effects.
   - Initialize the stack with the RHI device after `PipelineCache` and `ResourceViewCache` are available.
   - Add only currently supported R9 effects: Bloom and ToneMapping.
   - Inject `PipelineCache` and `ResourceViewCache` into both passes during renderer initialization.

2. Add honest runtime post-process settings and stats.
   - Add `PostProcessSettings` storage on `SceneRenderer`.
   - Default runtime settings may request Bloom and ToneMapping, but must not claim unsupported effects that
     are not added to the runtime stack.
   - Expose post-process stats from the last frame, including requested/enabled effects, unsupported skipped
     effects, graph pass count, stack transient intermediate count, and whether a scene-color staging texture
     was used.
   - If post-process was requested but resources are missing, stats must make `requested > enabled` or
     `unsupportedSkipped > 0` observable instead of silently returning to the direct path.
   - Log unsupported requested post-process effects through the existing `PostProcessStack` behavior.

3. Route scene rendering through a post-process scene-color staging texture only when supported effects exist.
   - If at least one requested post-process effect is supported and enabled, create a transient scene-color
     staging texture and set `ViewData::colorTarget` to that texture before scene passes are added.
   - Execute `PostProcessStack` after the scene pass loop with `sceneColor -> backBuffer`.
   - Let `PostProcessStack` allocate its own transient ping-pong intermediate only when more than one enabled
     effect runs.
   - Keep the scene-color format equal to the swap-chain/back-buffer format for this minimum path so existing
     material and post-process pipeline render target formats remain compatible.
   - If no supported post-process effects are enabled, keep the current direct-to-back-buffer path. Do not
     create an intermediate and do not leave the frame without a final output path.
   - Preserve the current back-buffer `ImportTexture(...)` and `SetExportState(..., Present)` behavior in both
     direct and post-process paths.

4. Preserve pass honesty.
   - `SkyboxPass` may remain honestly unsupported.
   - Unsupported post-process effects remain excluded from the default runtime stack or visibly skipped.
   - Do not silently fall back from post-process to direct output when Bloom/ToneMapping were requested and
     resources are missing; stats/logging must make the fallback visible.

5. Add tests.
   - Add a `SceneRenderer` or runtime-bridge validation that proves default runtime post-process wiring injects
     Bloom/ToneMapping resources, uses a scene-color staging texture, and produces two post-process graph passes
     with one stack transient intermediate when both effects are supported.
   - Add a fallback test proving direct-to-back-buffer remains selected when no supported post-process effects
     are enabled.
   - Add a requested-but-unsupported test proving stats expose `requested > enabled` or
     `unsupportedSkipped > 0`.
   - Keep existing `PostProcessStackRunsBloomBeforeToneMappingThroughIntermediate` coverage.
   - Run `RenderPassValidation`, `RenderGraphValidation`, `PipelineCacheValidation`, `RenderSceneValidation`,
     `ModelViewerSmoke`, and `VisualGoldenValidation`.

6. Record the phase and commit.

## Out of Scope

- TAA implementation.
- Full HDR scene color format split between material pipelines and post-process output pipelines.
- Full cubemap IBL or Skybox draw integration.
- Unsupported post-process effects such as FXAA, SSAO, SSR, DOF, motion blur, color grading, vignette,
  chromatic aberration, film grain, or volumetric lighting.
- Particle rendering.
- New backend-specific post-process work beyond existing Bloom/ToneMapping pipelines.

## Acceptance Criteria

- `SceneRenderer` owns and initializes a default runtime post-process stack.
- Bloom and ToneMapping no longer require external manual resource injection for the ModelViewer runtime path.
- When at least one supported post-process effect is enabled, scene passes render to a scene-color staging
  texture and post-process writes to the swap-chain back buffer.
- When post-process is disabled or unsupported, the renderer honestly keeps the direct-to-back-buffer path.
- Runtime stats make post-process graph pass counts, unsupported requested effects, and fallback/direct behavior observable.
- Required validation and visual gate pass.
- Spark plan review and Spark code review have no blockers before moving to the next stage.

## Validation Plan

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation RenderGraphValidation PipelineCacheValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|RenderGraphValidation|PipelineCacheValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Spark Review Gate

Implementation must not start until Spark reviews this plan and returns `PASS` or
`PASS_WITH_NON_BLOCKING_SUGGESTIONS` without blockers.
