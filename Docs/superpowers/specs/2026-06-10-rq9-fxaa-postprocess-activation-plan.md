# RQ9 - FXAA Post-Process Activation Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ8
Previous stage: RQ8 - Tangent Basis Robustness And Normal-Map Honesty

## 1. Stage Decision

Activate the existing FXAA post-process path as a real fullscreen LDR pass when explicitly enabled by the runtime.
`Render/Shaders/PostProcess/FXAA.hlsl` already exists, but `FXAAPass` is still marked unsupported and only adds a TODO
graph pass. This means the engine exposes anti-aliasing controls while the effect cannot actually execute.

RQ9 connects FXAA using the same minimum fullscreen pattern already proven by Bloom and ToneMapping, and adjusts the
post-process HDR/LDR boundary so LDR effects can run after ToneMapping. This is a visual-quality step that stays much
smaller than TAA, SSAO, SSR, or a full post-process domain system.

## 2. Current Engine Evidence

- `FXAAPass::FXAAPass()` calls `MarkUnsupported("FXAA shader and fullscreen pipeline are not implemented")`.
- `FXAAPass::AddToGraph()` currently contains only TODO comments in the execution lambda.
- `Render/Shaders/PostProcess/FXAA.hlsl` provides fullscreen `VSMain`/`PSMain`, constants, input texture, and sampler.
- `Render/Shaders/PostProcess/FXAA.hlsl` currently declares `b0/t0/s0` without explicit space, while the existing
  post-process descriptor set layout used by Bloom/ToneMapping binds `b0`, `t1`, and `s2` in space 0.
- `PipelineCache` currently compiles and owns post-process pipelines for ToneMapping and Bloom, but not FXAA.
- `SceneRenderer` runtime defaults currently set `enableFXAA = false`; RQ9 does not flip this default.
- `PostProcessStack` currently treats ToneMapping as invalid unless it is the final effect, which prevents legitimate
  LDR effects such as FXAA from running after ToneMapping.
- `PostProcessStack` currently allocates all intermediate textures from the scene-color descriptor. If FXAA runs after
  ToneMapping, the ToneMapping output intermediate must use the final output/backbuffer LDR format rather than HDR
  scene color.

## 3. Scope

1. Add FXAA PipelineCache support.
   - Align `Render/Shaders/PostProcess/FXAA.hlsl` bindings to the existing post-process layout:
     `FXAAConstants` at `b0, space0`, input texture at `t1, space0`, and sampler at `s2, space0`.
   - Compile `Render/Shaders/PostProcess/FXAA.hlsl` vertex and pixel shaders.
   - Store FXAA shader refs and compile results in `PipelineCache`.
   - Add `GetFXAAPipeline(outputFormat)`, `GetOrCreateFXAAPipeline(outputFormat)`, and
     `BuildFXAAPipelineDesc(outputFormat)`.
   - Use the existing post-process descriptor set layout and pipeline layout.
   - Track FXAA pipeline hash in `PipelineCacheStats` and `PipelineCacheManifest`.
   - Fail visibly if the FXAA shader or pipeline cannot be created, following ToneMapping/Bloom patterns.

2. Implement `FXAAPass` runtime resources.
   - Add `SetResources(PipelineCache*, ResourceViewCache*)`.
   - Allocate/update a small constant buffer with texture size, inverse texture size, edge threshold, edge threshold min,
     and subpixel quality.
   - Create/reuse a linear clamp sampler.
   - Resolve input SRV/output RTV from `ResourceViewCache`.
   - Create a descriptor set and draw a fullscreen triangle.
   - Retain descriptor sets for enough frames, as ToneMapping/Bloom do.
   - Mark the pass supported only when PipelineCache, ResourceViewCache, FXAA pipeline/layout, constant buffer, and
     sampler are available.

3. Fix the HDR/LDR post-process boundary for FXAA.
   - Allow FXAA to run after ToneMapping without triggering the ToneMapping boundary warning.
   - Keep ToneMapping after HDR-domain effects such as Bloom.
   - Allocate intermediates before ToneMapping from the HDR scene-color descriptor.
   - Allocate the ToneMapping output intermediate and later LDR intermediates from the final output descriptor.
   - Preserve the existing invalid-boundary warning for unsupported orderings where an HDR effect appears after
     ToneMapping.

4. Wire SceneRenderer resources.
   - Ensure SceneRenderer creates/configures the FXAA pass when FXAA is present in the stack.
   - Ensure FXAA receives PipelineCache and ResourceViewCache like Bloom/ToneMapping.
   - Do not change SceneRenderer's runtime default `enableFXAA = false` in this stage. FXAA should execute when an
     explicit settings path enables it.

5. Tests and documentation.
   - Extend `PipelineCacheValidation` for FXAA shader missing/pipeline creation/hash/manifest coverage.
   - Extend `RenderPassValidation` with FXAA support, draw, constant-map failure, and Bloom -> ToneMapping -> FXAA stack
     ordering/intermediate-format tests.
   - Record the stage in `Docs/superpowers/specs/phase-log.md`.

## 4. Out of Scope

- TAA, SMAA, MSAA resolve, sharpening, FXAA shader quality macro variants, UI exposure for FXAA quality presets, dynamic
  render-resolution scaling, SSAO/SSR/DOF/Volumetric implementations, and a generalized post-process domain graph.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq9-fxaa-postprocess-activation-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Shaders/PostProcess/FXAA.hlsl`
- `Render/Include/Render/PostProcess/FXAA.h`
- `Render/Private/PostProcess/FXAA.cpp`
- `Render/Private/PostProcess/PostProcessStack.cpp`
- `Render/Include/Render/PostProcess/PostProcessStack.h` if stats/domain fields are needed
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp` if SceneRenderer resource wiring needs a focused guard

## 6. Required Tests

- `PipelineCacheValidation` fails visibly when FXAA shader is missing.
- `PipelineCacheValidation` creates FXAA pipelines for default and runtime output formats and records non-zero FXAA
  pipeline hash.
- `PipelineCacheValidation` manifest invalidates when the FXAA shader/pipeline hash changes.
- `PipelineCacheValidation` or `RenderPassValidation` source/reflection guard proves `FXAA.hlsl` uses the existing
  post-process binding layout (`b0`, `t1`, `s2`, space 0).
- `RenderPassValidation` proves FXAA reports unsupported before resources and supported after resources.
- `RenderPassValidation` proves FXAA adds a live graph pass, binds the FXAA pipeline/descriptor set, and draws a
  fullscreen triangle.
- `RenderPassValidation` proves FXAA skips draw when constants cannot map.
- `RenderPassValidation` proves Bloom -> ToneMapping -> FXAA is valid, uses HDR intermediate before ToneMapping and LDR
  intermediate after ToneMapping, and draws in that order.
- `RenderPassValidation` proves ToneMapping -> Bloom remains invalid because Bloom is an HDR-domain effect after the
  HDR-to-LDR boundary.
- `RenderHonestyValidation` updates the old unsupported/stub expectation for FXAA: before resources it remains
  unsupported, after resources it must be supported and no longer belongs in the always-unsupported list.
- `RenderPassValidation` or `RenderSceneValidation` proves SceneRenderer wires FXAA resources.
- A guard proves RQ9 does not flip SceneRenderer's runtime default `enableFXAA = false`.
- Existing focused gates remain green:
  - `PipelineCacheValidation`
  - `RenderPassValidation`
  - `RenderSceneValidation`
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerPBRMaterialSmoke`
  - `PBRMaterialVisualGoldenValidation`

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation
cmake --build $B --config Debug --target RenderHonestyValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|RenderHonestyValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
git diff --check
```

## 8. Risks

- Existing visual goldens should not change because RQ9 does not flip SceneRenderer's runtime default FXAA setting. If
  they do change, treat it as a regression unless an explicit test path enabled FXAA.
- ToneMapping boundary changes can accidentally allow HDR effects after ToneMapping. Tests must distinguish known LDR
  effects such as FXAA from HDR effects such as Bloom.
- Pipeline manifest schema gains FXAA fields. Old manifests should be treated as stale/invalid and regenerated
  visibly, matching existing manifest behavior.
- FXAA should operate in LDR after ToneMapping. Running it on HDR scene color is out of scope and should be guarded by
  stack ordering/intermediate-format tests.

## 9. Acceptance Criteria

- FXAA is supported and executed when enabled and resources are available.
- FXAA runs after ToneMapping in the default post-process order.
- ToneMapping output to FXAA uses LDR output format, not HDR scene-color format.
- Missing FXAA shader/pipeline resources fail visibly.
- Existing SceneRenderer runtime default `enableFXAA = false` remains unchanged.
- Focused render, honesty, and ModelViewer visual gates pass without unplanned golden recapture.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ9 scope as the task list. Do not expand into TAA, SSAO,
SSR, SMAA, MSAA, sharpening, or a full post-process domain graph.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `feat(render): enable fxaa postprocess pass`
