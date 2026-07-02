# RQ3a - Directional Shadow Sampling Bridge Plan

Date: 2026-06-09
Parent program: render-quality continuation after RQ2k
Previous stage: RQ2k - BRDF LUT Split-Sum Numeric Guardrails

## 1. Stage Decision

RQ2 made texture IBL visible and numerically less suspect. The next largest visible render-quality gap is shadows: the engine can declare and render directional shadow cascades, but `DefaultLit` still evaluates direct lighting with a hard-coded `shadow = 1.0`. RQ3a connects the already-existing `ShadowPass` output to the main DefaultLit path for a minimal, honest directional-shadow sample.

This stage is deliberately a bridge, not a full AAA shadow system. It should make one directional shadow map/cascade affect direct lighting, make fallback/disabled states visible and testable, and leave stable multi-cascade selection, temporal stabilization, PCSS/EVSM, contact shadows, and transparent shadows for later stages.

## 2. Current Engine Evidence

- `ShadowPass` already computes PSSM cascade matrices, creates depth textures with `DepthStencil | ShaderResource`, exports them as `ShaderResource`, and draws shadow-casting objects.
- `SceneRenderer::PreparePassesForFrame()` enables `ShadowPass` only when a directional shadow-casting light exists.
- `DefaultLit.hlsl` receives `LightDirection` and `DirectionalLightIntensity`, but calls `EvaluatePBR(..., 1.0)` and has no shadow texture or shadow matrix inputs.
- `PipelineCache` frame descriptor set currently binds only `ViewConstants` at set 0 binding 0; DefaultLit set 2 is already crowded with material and IBL resources.
- `UpdatePassResources()` runs after `BuildRenderGraph()`, so any OpaquePass shadow read dependency must be available before or during pass `Setup`, not only during pass `Execute`.
- `ResourceViewCache::GetDefaultSRV()` does not mark depth aspects; RQ3a should request an explicit depth SRV view for shadow maps instead of changing global default SRV behavior.

## 3. Scope

1. Extend view/frame constants with shadow data.
   - Add `directionalShadowViewProjection`.
   - Add `directionalShadowParams` with at least: enabled flag, depth bias, shadow strength, shadow map size or inverse size.
   - Keep HLSL and C++ packing explicitly tested.
   - `PipelineCache::UpdateViewConstants()` must apply the same backend clip-space transform to the shadow matrix that it applies to the camera view-projection matrix, including Vulkan Y convention.
   - Reverse-Z must be represented in the uploaded params and the shader compare path; RQ3a should either support it explicitly or keep shadows disabled with an assertable reason when reverse-Z is enabled.

2. Extend DefaultLit set 0 descriptor layout.
   - Add a frame-scope shadow map texture binding and shadow sampler binding.
   - Keep material set 2 unchanged; shadow is frame/light state, not per-material state.
   - Update PipelineCache reflection validation and source guardrails.

3. Add frame descriptor update path for shadows.
   - PipelineCache should own or create a safe fallback shadow binding for disabled/unavailable shadows.
   - Add an API such as `UpdateFrameShadowResources(...)` or equivalent, with stats/result fields that tests can assert.
   - If the real shadow SRV or sampler is unavailable, keep shadow sampling disabled and record a fallback reason such as `DisabledNoDirectionalLight`, `MissingShadowSRV`, `MissingSampler`, or `ReverseZUnsupported`.
   - Updating the frame descriptor is not enough: before opaque draws, the main-view `ViewConstants` must be re-uploaded after cascade 0 is known so `directionalShadowViewProjection` and enabled params match the descriptor being bound.
   - Disabled shadow sampling must still create a complete set 0 descriptor with fallback texture/sampler bindings.

4. Expose ShadowPass output to OpaquePass before RenderGraph setup.
   - Add a narrow `OpaquePass` hook to reference `ShadowPass` or an immutable per-frame shadow resource struct.
   - In `OpaquePass::Setup`, declare a RenderGraph read of the first valid directional cascade handle when shadows are enabled.
   - In `OpaquePass::Execute`, resolve an explicit depth SRV view and update both frame descriptors and main-view constants before drawing.
   - Add a test that the read is declared during `Setup`, not via `UpdatePassResources()`, because the binder runs after graph construction.
   - RQ3a is focused on OpaquePass. TransparentPass behavior must be explicit: either it inherits the same shadow-enabled frame constants deliberately, or the pass disables/re-uploads shadow params before transparent draws. Do not leave this implicit.

5. Add minimal DefaultLit sampling.
   - Use a manual depth compare path first; do not require hardware comparison samplers in RQ3a.
   - Start with cascade 0 only.
   - Multiply direct lighting by sampled shadow visibility.
   - Disable sampling outside shadow UV/depth range.
   - Honor `RenderObject::receivesShadow` in the uploaded object/material path only if a local, low-risk route already exists; otherwise document and test that RQ3a shadows are light/frame-level only and `receivesShadow` remains a later per-object shading input.

6. Unify the selected primary directional light.
   - Select one primary directional light per frame in `SceneRenderer` before `ShadowPass` setup.
   - Upload that same light direction/intensity to `ViewData`/DefaultLit and configure `ShadowPass` from the same selection.
   - If no directional light is present, retain the existing default direct light only when it is deliberately documented as fallback lighting; shadow sampling must be disabled.
   - Prefer a narrow selected-light snapshot/introspection API over brittle source-string tests.

7. Add tests and honesty checks.
   - Shader guardrail: DefaultLit must no longer pass a literal `1.0` shadow visibility into `EvaluatePBR`.
   - PipelineCacheValidation: set 0 layout includes shadow texture/sampler; `ViewConstants` packing includes shadow fields.
   - PipelineCacheValidation: DX-style and Vulkan fake-backend uploads produce expected shadow matrix conventions or an explicit disabled reason for unsupported reverse-Z.
   - SceneRenderer/guardrail test: selected primary directional light feeds both DefaultLit constants and `ShadowPass`.
   - RenderPassValidation: OpaquePass declares a read dependency on ShadowPass cascade 0 when available and records disabled/fallback when unavailable.
   - Existing visual gates must still pass unless an intentional shadowed test scene/golden is added.

## 4. Out of Scope

- Full PSSM cascade selection, blend bands, texel snapping, temporal stabilization, shimmering fixes.
- PCSS, VSM/EVSM/MSM, contact shadows, screen-space shadows.
- Point/spot shadows.
- Transparent/alpha-tested shadow correctness beyond using existing masked draw routing if already present.
- Shadow atlas packing or array texture refactor.
- Changing ModelViewer camera/model defaults or recapturing golden unless the existing visual golden intentionally changes and is reviewed.
- Hardware comparison sampler backend fixes unless manual compare proves impossible.

## 5. Expected Files

- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/OpaquePass.h`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/RenderSceneValidation/main.cpp` or `Tests/PipelineCacheValidation/main.cpp` for the primary-light selection guardrail
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- Pipeline layout/reflection test verifies set 0 binding 1 is a sampled texture and binding 2 is a sampler.
- Frame descriptor test verifies binding 1/2 are complete even when shadows are disabled and fallback resources are used.
- View constants ABI test verifies shadow matrix/params offsets and size.
- `UpdateViewConstants()` uploads disabled defaults and enabled shadow params.
- `UpdateViewConstants()` applies the same backend clip-space convention to the uploaded shadow matrix as the camera matrix, with DX-style and Vulkan fake-backend coverage.
- DefaultLit source guardrail verifies:
  - shadow texture/sampler declarations exist in set 0,
  - a shadow sampling helper exists,
  - direct lighting uses sampled shadow visibility,
  - `EvaluatePBR(..., 1.0)` is not the direct-light path.
- Render pass test verifies OpaquePass declares a read on the ShadowPass cascade handle when available.
- Primary light test verifies the same selected directional light feeds DefaultLit constants and ShadowPass configuration.
- Fallback/honesty test verifies missing shadow SRV/sampler disables shadow sampling and records an assertable reason.
- TransparentPass behavior is documented and covered by a guardrail if frame constants would otherwise carry shadow state implicitly.
- Regression: `PipelineCacheValidation`, `RenderPassValidation`, `RenderSceneValidation`, `ModelViewerSmoke`, `VisualGoldenValidation`.

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
git diff --check
```

If ModelViewer lighting changes materially, capture a manual DamagedHelmet screenshot and inspect it before code review.

## 8. Risks

- RenderGraph ordering: OpaquePass must declare shadow reads during setup, so the shadow handle must be available before OpaquePass setup.
- Constants timing: ShadowPass temporarily uploads cascade-view constants for depth rendering; OpaquePass must re-upload main-view constants with the selected cascade 0 shadow matrix before DefaultLit draws.
- Light mismatch: the same selected primary directional light must drive both direct lighting and shadow map generation.
- Backend convention mismatch: shadow projection upload and manual depth compare must account for Vulkan clip-space and reverse-Z policy.
- Descriptor completeness: set 0 shadow bindings need safe fallback resources even when sampling is disabled.
- Depth SRV compatibility: use explicit depth SRV view descriptors and keep backend-specific fixes out of scope unless the existing RHI cannot create the view.
- Visual regression: the existing golden may not include a shadow-casting scene; no visual change is expected there.

## 9. Acceptance Criteria

- A directional shadow-casting light can make DefaultLit direct lighting use a sampled shadow visibility value.
- The sampled shadow map, uploaded shadow matrix, and DefaultLit direct light all correspond to the same selected primary directional light.
- OpaquePass re-uploads main-view constants after ShadowPass cascade data is known and before opaque draws.
- Frames without an available shadow map keep direct lighting unchanged and expose the disabled/fallback reason.
- Pipeline layout, ViewConstants packing, shader source, and OpaquePass dependency behavior are covered by tests.
- Required validation commands pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ3a scope as the task list. Do not expand into full CSM stabilization, PCSS/EVSM, point/spot shadows, contact shadows, exposure, bloom, IBL, or ECS/Object refactors.

## 11. Spark Review

- Plan review: PASS after initial blockers were addressed (`gpt-5.5`, xhigh)
- Code review: PASS after missing-shadow-SRV fallback blocker was addressed (`gpt-5.5`, xhigh)
- Commit message: `feat(render): bridge directional shadow sampling`
