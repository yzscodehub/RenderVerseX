# R9a Render Pass Chain Honesty Plan

Date: 2026-06-06

## Source

- Program document: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Parent stage: `15. R9 - Render Pass Completion`
- Lines checked: 485-513
- Preceding stage evidence: R8 implementation committed as `4785071`; R8 hash log committed as `86e392d`.

## Goal

Establish the R9 pass-chain honesty foundation before implementing deeper visual algorithms. The renderer must expose which production passes are requested, supported, actually added to the RenderGraph, or deliberately disabled because they are still unsupported. This directly addresses the R9 requirement that no enabled pass is a silent no-op.

## Scope

- Add a uniform render-pass status contract to `IRenderPass`:
  - requested enabled state
  - supported state
  - enabled/executable state
  - unsupported reason
- Add pass status snapshots in `RenderPassRegistry`.
- Update `SceneRenderer::BuildRenderGraph` to use the status contract:
  - add supported enabled passes to the RenderGraph
  - skip requested-but-unsupported passes visibly
  - record per-frame pass-chain stats that tests and diagnostics can inspect
  - expose the last pass-chain stats through a small `SceneRenderer` accessor
- Update existing pass classes enough to report honest state:
  - `SkyboxPass`: requested by default but unsupported until draw pipeline exists.
  - `ShadowPass`: requested only when configured, unsupported until shadow map/cascade resources and depth pipeline are genuinely ready.
  - `DepthPrepass`: requested only when enabled, unsupported if the depth-only pipeline is unavailable.
  - `OpaquePass` and `TransparentPass`: keep supported as the implemented geometry passes.
- Add validation tests for:
  - default `IRenderPass` status semantics
  - unsupported requested passes are not reported as enabled
  - registry status snapshots preserve pass order and reasons
  - Skybox/Shadow/DepthPrepass status is honest
  - unsupported requested pass behavior remains visible instead of silent
- Keep the existing post-process honesty expectations intact.
- Limit chain-honesty assertions to passes registered in the current `SceneRenderer` main chain; future/non-registered classes such as deferred-only or experimental passes are intentionally outside R9a.

## Out Of Scope

- Completing shadow map rendering, PSSM matrices, or shadow sampling.
- Implementing clustered lighting in shaders or descriptor bindings.
- Implementing ToneMapping, Bloom, TAA, or IBL visual algorithms.
- Adding new GPU render targets for HDR/post-process ping-pong.
- Changing ModelViewer visuals beyond maintaining the existing R7 visual golden gate.
- Removing legacy passes or rewriting draw submission.

## Expected Files

- `Render/Include/Render/Passes/IRenderPass.h`
- `Render/Include/Render/Passes/DepthPrepass.h`
- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Include/Render/Passes/SkyboxPass.h`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Private/Renderer/RenderPassRegistry.h`
- `Render/Private/Renderer/RenderPassRegistry.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp` if needed for shared assertions
- `Docs/superpowers/specs/phase-log.md`

## Risks

- Logging unsupported requested passes every frame could become noisy; prefer stats and bounded warning text.
- Treating a pass as unsupported could accidentally remove a pass from the RenderGraph if its status check is too strict.
- Some passes may be intentionally disabled, which must not be counted as a failure.
- Tests should not require constructing a full graphics device just to inspect pass status.

## Validation

Build:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderPassValidation RenderHonestyValidation ModelViewer VisualGoldenValidation ImageCompareValidation
cmake --build build/win_x64_debug --config Debug --target RenderGraphValidation
```

Required tests:

```powershell
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderPassValidation|RenderHonestyValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
```

Regression:

```powershell
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Spark Review

- Plan review: pending.
- Code review: pending.

## Commit Message

`fix(render): expose render pass chain status`
