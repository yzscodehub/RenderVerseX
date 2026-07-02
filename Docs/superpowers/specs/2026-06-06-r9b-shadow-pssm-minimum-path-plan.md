# R9b Shadow/PSSM Minimum Resource Path Plan

**Date:** 2026-06-06  
**Status:** Spark plan review passed with non-blocking suggestions  
**Parent R-SP:** R9 - Render Pass Completion

---

## 1. Source Check

- Roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section reread before this plan: `15. R9 - Render Pass Completion`
- Source lines checked: roadmap lines 485-513
- Phase log reread: `Docs/superpowers/specs/phase-log.md`, latest committed entry `R9a: Render Pass Chain Honesty`

R9 scope includes:

- Shadow/PSSM minimum path.
- Pass resource declarations through RenderGraph.
- Stub passes implement-or-disable.
- Required validation: `RenderGraphValidation`, `RenderSceneValidation`, `VisualGoldenValidation`, `ModelViewerSmoke`.

R9a completed pass-chain honesty only. It intentionally left Shadow, Skybox, Bloom, ToneMapping, TAA, clustered lighting, and IBL unimplemented. This sub-stage implements only the Shadow/PSSM minimum resource path.

---

## 2. Goal

Make `ShadowPass` a real, supported, RenderGraph-declared minimum pass when a directional shadow-casting light exists and the depth-only pipeline is available.

The pass must:

- Be disabled by default when no shadow-casting directional light exists.
- Become requested and supported when frame light data selects a directional shadow light and required render infrastructure exists.
- Declare one transient depth texture per cascade through `RenderGraph`.
- Resolve transient cascade textures after graph compile and render depth-only geometry into them.
- Produce valid PSSM split depths and non-identity light view-projection matrices.
- Remain visually neutral for ModelViewer because shadow sampling into lighting is out of this sub-stage.

---

## 3. Scope

### Pipeline Foundation

- Add a minimal `PipelineCache` depth-only graphics pipeline.
- Reuse the existing default vertex shader, object constants, frame descriptor layout, and input layout.
- Configure the depth-only pipeline with:
  - depth test/write enabled,
  - configured depth stencil format,
  - zero color render targets,
  - no pixel shader if accepted by the existing RHI contract,
  - rasterizer depth bias sourced from shadow pass config only at render state level where available.
- If a backend proves unable to create a zero-color/no-pixel-shader pipeline, keep the failure visible and add a minimal depth-only pixel shader fallback rather than reporting support without a valid pipeline.
- Keep material pipelines unchanged.

### ShadowPass Resources

- Replace the current resource-dependent `IsSupported()` with a resource-independent support check:
  - `PipelineCache` initialized,
  - depth-only pipeline exists,
  - `GPUResourceManager` exists,
  - valid cascade count and shadow map size.
- Add cascade RenderGraph handles and runtime texture/view caches.
- In `Setup()`:
  - clamp/validate cascade count and map size,
  - calculate PSSM splits and light matrices,
  - create one `RHITextureDesc::DepthStencil` transient 2D texture per cascade,
  - declare each cascade with `RenderGraphBuilder::SetDepthStencil(...)`.
- Because R9b does not yet sample shadow maps, pass liveness is test-visible through the declared depth writes, graph compile stats, exported cascade handles/descriptors, and execution stats. The graph must not count an enabled `ShadowPass` as a silent no-op even if later lighting does not consume the textures yet.
- In `Execute()`:
  - resolve cascade textures from `view.renderGraph`,
  - get per-cascade DSVs through `view.viewCache->GetDefaultDSV(...)`,
  - begin one depth-only render pass per valid cascade,
  - bind the depth-only pipeline and frame/object descriptor sets,
  - draw only objects with `castsShadow == true` and valid GPU mesh buffers.
- Track deterministic shadow pass stats for tests:
  - configured cascade count,
  - declared cascade resource count,
  - resolved cascade view count,
  - shadow caster count,
  - draw count.

### PSSM Minimum Matrices

- Replace identity cascade matrices with a practical minimum:
  - compute camera frustum slice corners from `ViewData` near/far/FOV/aspect/camera basis,
  - fit an orthographic light projection around each slice,
  - build `lightView * lightProjection` using existing GLM helpers.
- This is not a stabilized/shimmer-free production CSM implementation.

### SceneRenderer Integration

- Add a frame preparation step before `BuildRenderGraph()` evaluates pass status.
- Select the first `RenderLight` where:
  - `type == Directional`,
  - `castsShadow == true`,
  - `intensity > 0`.
- Configure `ShadowPass` from that light before pass status evaluation.
- Disable `ShadowPass` for the frame when no eligible directional shadow light exists.
- Keep `RenderFrameResourceBinder` for render scene/draw resource binding after graph resources are created.

---

## 4. Out of Scope

- Sampling shadow maps in `OpaquePass`, shaders, materials, or clustered lighting.
- Making ModelViewer visibly darker or changing the R7 golden image.
- Point/spot shadow maps.
- Texture arrays for cascades.
- Stable texel snapping, cascade blending, PCF filtering, or atlas packing.
- Clustered lighting, IBL, ToneMapping, Bloom, TAA, SSAO, SSR, or Skybox implementation.
- Full ECS/Object migration or `SceneEntity` deletion.
- Multithreaded render proxy command queues.

---

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-06-r9b-shadow-pssm-minimum-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Include/Render/Renderer/SceneRenderer.h`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

Only add other files if a build break proves a narrow signature update is necessary.

---

## 6. Risks

- Some backends may reject a graphics pipeline with no pixel shader or zero color render targets. If that happens locally, keep backend failure visible and do not fake support.
- `RenderGraph` pass culling could remove a standalone shadow pass if the resource is not consumed later. The pass must remain test-visible either through graph stats/resources or an explicit non-culled write path.
- Cascade matrices can be numerically fragile when view planes are invalid. Clamp invalid `near/far/aspect/FOV` values and record unsupported status for impossible configurations.
- R7 visual golden should remain unchanged because shadow maps are not sampled in lighting during R9b.

---

## 7. Tests To Add Or Update

### PipelineCacheValidation

- Assert initialization creates four graphics pipelines: opaque, masked, transparent, and depth-only.
- Assert `GetDepthOnlyPipeline()` is non-null after initialization.
- Assert the depth-only pipeline description has zero color render targets, a valid depth format, writable depth state, and no pixel shader.
- Keep existing material pipeline hash/count expectations adjusted for the extra pipeline.

### RenderPassValidation

- Update built-in pass status test so `ShadowPass` remains unsupported without resources, but becomes supported when resources, pipeline cache, valid config, and a directional light are present.
- Add `ShadowPass` setup test:
  - creates the configured number of cascade resources through `RenderGraph`,
  - depth descriptors match configured size/format,
  - split depths are increasing,
  - view-projection matrices are not identity.
- Add `ShadowPass` execute test:
  - resolves DSVs through `ResourceViewCache`,
  - begins/ends one render pass per cascade,
  - draws only shadow-casting objects.
- Add disabled/no-light behavior test:
  - no eligible light keeps `ShadowPass` not requested and no graph pass is added.

### Existing Gates

- `RenderGraphValidation`
- `RenderSceneValidation`
- `VisualGoldenValidation`
- `ModelViewerSmoke`

---

## 8. Build And Validation Commands

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderGraphValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderGraphValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

---

## 9. Visual Gate

Required and expected to pass without golden changes.

- `ModelViewerSmoke`: PASS required
- `VisualGoldenValidation`: PASS required

If the visual golden changes, treat it as a blocker unless the diff is proven to be an intentional and documented visual result of a later shadow-sampling stage. R9b does not sample shadows, so no visual diff is expected.

---

## 10. Spark Review Gates

- Spark plan review model: `gpt-5.3-codex-spark`
- Spark plan review agent: `019e9ceb-72f3-7e72-8e3a-d79dd73a8db0`
- Plan review status: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Non-blocking suggestions adopted before implementation: documented RenderGraph liveness strategy, backend-sensitive no-pixel-shader fallback contract, and validation command consistency.
- Spark code review model: `gpt-5.3-codex-spark`
- Spark code review agent: `019e9d1c-54cc-7072-9185-de319163a620`
- Spark final follow-up review agent: `019e9d21-4e99-76f2-88b8-60d05747f2e1`
- Code review status: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`, no blocking findings.

Code changes started only after Spark plan review returned no blockers, and the phase is eligible for commit after validation and Spark code review returned no blocking findings.

---

## 11. Phase Log And Commit

Phase-log entry to append after validation and Spark code review:

- R-SP: `R9b: Shadow/PSSM Minimum Resource Path`
- Commit message: `Implement R9b shadow PSSM minimum path`
