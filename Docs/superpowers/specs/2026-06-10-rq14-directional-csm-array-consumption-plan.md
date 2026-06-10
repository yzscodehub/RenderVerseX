# RQ14 - Directional CSM Array Consumption

Date: 2026-06-10
Program: Render Quality & Verification Program v2
Previous stage: RQ13 - Directional Shadow Receiver Normal Bias

## 1. Stage Decision

Finish the first honest Cascaded Shadow Map consumption path for the primary directional light.
`ShadowPass` already calculates multiple cascades, but the main lighting path currently binds and
samples only cascade 0. RQ14 changes the runtime contract to a single directional shadow
`Texture2DArray<float>` plus up to four cascade matrices/splits in `ViewConstants`, so `DefaultLit`
selects and samples the correct cascade per pixel.

This directly improves large-scene shadow coverage and removes the current mismatch between
"CSM support" in the pass generator and single-cascade consumption in the shader.

## 2. Current State Evidence

- `ShadowPassConfig::numCascades` defaults to 4 and `ShadowPass::CalculateCascades()` fills multiple
  `ShadowCascade` entries.
- `ShadowPass::Setup()` currently creates one independent 2D depth texture per cascade.
- `OpaquePass::Setup()` reads only `m_shadowPass->GetCascadeTextureHandles()[0]`.
- `OpaquePass::Execute()` binds only one SRV and uploads only `m_shadowPass->GetCascades()[0].viewProjection`.
- `DefaultLit.hlsl` declares `Texture2D<float> DirectionalShadowMapTexture` and samples one matrix.
- RHI backends support Texture2DArray views when array layer count is greater than one.
- `RenderGraph` supports subresource/layer usage tracking, but `RGTextureHandle::Subresource()` defaults
  to color aspect, so depth-layer handles must explicitly use `RHITextureAspect::Depth`.

## 3. Scope

1. Add a shared directional CSM limit, `RVX_MAX_DIRECTIONAL_SHADOW_CASCADES = 4`.
2. Extend `ViewData` with:
   - up to four directional shadow view-projection matrices,
   - absolute camera-forward view-depth split distances,
   - cascade count.
3. Extend `ViewConstants` / `DefaultLit.hlsl` with:
   - camera forward + cascade count,
   - `DirectionalShadowViewProjections[4]`,
   - `DirectionalShadowCascadeSplits`,
   - existing params and receiver normal bias preserved.
4. Change `ShadowPass` to create one depth `Texture2DArray` for the directional CSM and write each
   cascade to a depth layer view.
5. Keep the directional shadow SRV binding as a single texture binding, but it must be a
   Texture2DArray-compatible view. To avoid one-cascade backend mismatch, the shadow array/fallback
   should allocate at least two layers even if the requested cascade count is one.
6. Update `OpaquePass` so it reads the whole shadow array, binds a full-array depth SRV, uploads all
   cascade matrices and split distances, and sets cascade count.
7. Update `DefaultLit.hlsl` to:
   - declare `Texture2DArray<float> DirectionalShadowMapTexture`,
   - select the cascade using absolute camera-forward view depth from
     `dot(worldPos - CameraPosition, CameraForward)`,
   - sample `float3(shadowUV, cascadeIndex)`,
   - keep RQ13 receiver normal bias and existing PCF/depth bias behavior.
8. Convert `ShadowCascade::splitDepth` from its normalized storage to absolute camera-forward distances
   before uploading to `DirectionalShadowCascadeSplits`. Tests must use near/far values where normalized
   and absolute splits differ.
9. Ensure all directional shadow array graph usages use `RHITextureAspect::Depth`, including:
   - per-layer depth writes,
   - full-array shader reads,
   - export transitions from depth-write layers to shader-resource state.
10. Preserve reverse-Z fallback behavior and existing disabled-shadow behavior.
11. Add tests covering array resource declaration, layer DSVs, full-array SRV binding, view-constant
   layout/upload, shader source guard, one-cascade array compatibility, and visual stability.

## 4. Out of Scope

- Cascade blending/fade bands.
- Stabilized texel snapping beyond the existing cascade matrices.
- PCSS/contact shadows/VSM/EVSM/MSM.
- Shadow atlas packing.
- Descriptor arrays or bindless shadow maps.
- Point/spot light shadows.
- Transparent shadow receiving.
- Shadow settings UI/editor controls.
- Broad artistic retuning of shadow map size, light direction, strength, or bias.

## 5. Expected Files

- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/Passes/ShadowPass.h`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Include/Render/Passes/OpaquePass.h`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Graph/RenderGraphCompiler.cpp` if full-array depth read aspect handling needs a fix
- `Render/Private/Graph/RenderGraphExecutor.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `RHI_DX11/Private/DX11Resources.cpp` if backend array-layer DSV/SRV view creation does not preserve
  `FirstArraySlice` for single-layer array views
- `RHI_DX12/Private/DX12Resources.cpp` if backend array-layer DSV/SRV view creation does not preserve
  array-layer semantics for single-layer array views
- `Tests/CMakeLists.txt` if backend-private native descriptor validation includes are needed
- `Tests/DX11Validation/main.cpp`
- `Tests/DX12Validation/main.cpp`
- `Tests/RenderGraphValidation/main.cpp`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm` if the shadow golden changes after inspection
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

1. `PipelineCacheValidation`:
   - `ViewConstantsLayoutMatchesDefaultLitCBufferPacking` updated for cascade arrays/splits.
   - `UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention` proves all cascade matrices
     and split distances upload, including backend clip convention.
   - The split-distance test must use near/far and `ShadowCascade::splitDepth` values where normalized
     splits do not equal absolute camera-forward distances.
   - Source guard requires `Texture2DArray<float> DirectionalShadowMapTexture`,
     `DirectionalShadowViewProjections[4]`, cascade selection by camera-forward view depth, array-layer
     sampling, RQ13 receiver normal bias, and final normal-map normal call site.
   - Camera forward upload is finite-normalized before shader cascade selection.
   - Fallback resources create a Texture2DArray-compatible view and still report existing fallback reasons.
2. `RenderPassValidation`:
   - `ShadowPassSetupDeclaresCascadeDepthResourcesAndPSSMMatrices` proves one shadow texture array is
     declared with array layers >= cascade count and each cascade handle targets a depth layer.
   - `ShadowPassExecuteResolvesCascadeViewsAndDrawsOnlyShadowCasters` proves per-layer DSVs are created.
   - `OpaquePassDeclaresDirectionalShadowReadDuringSetup` proves it reads the full array, binds an SRV
     covering all cascade layers, uploads cascade count/splits/matrices, and keeps normal bias/filter bias.
   - Add or extend a one-cascade test proving the underlying shadow texture/fallback SRV remains array-compatible.
   - Add unsupported/visible behavior for `numCascades > RVX_MAX_DIRECTIONAL_SHADOW_CASCADES`.
3. `RenderGraphValidation`:
   - Add coverage for a depth texture array whose individual layers are written with `RHITextureAspect::Depth`
     and then exported/read as shader resource, proving generated/export barriers keep Depth aspect instead
     of falling back to Color.
4. Backend validation:
   - `DX11Validation.Texture2DArrayLayerViewsPreserveArraySlice` proves a single-layer view of a 2D array
     depth texture still creates native `TEXTURE2DARRAY` DSV/SRV descriptors with the requested
     `FirstArraySlice`.
   - `DX12Validation.Texture2DArrayLayerViewsCreateNativeDescriptors` proves single-layer depth-array DSV/SRV
     creation produces valid native descriptors.
5. Visual/regression:
   - `ModelViewerShadowSmoke`
   - `ShadowVisualGoldenValidation`
   - `ModelViewerSmoke`
   - `VisualGoldenValidation`
   - `RenderSceneValidation`
   - `RenderHonestyValidation`
   - `ImageCompareValidation`

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderGraphValidation RenderSceneValidation RenderHonestyValidation DX11Validation DX12Validation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderGraphValidation|DX11Validation\.Texture2DArrayLayerViewsPreserveArraySlice|DX12Validation\.Texture2DArrayLayerViewsCreateNativeDescriptors"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderHonestyValidation|DX11Validation\.Texture2DArrayLayerViewsPreserveArraySlice|DX12Validation\.Texture2DArrayLayerViewsCreateNativeDescriptors|ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- `ViewConstants` layout grows significantly; C++ and HLSL packing must be locked by tests.
- `DirectionalShadowCascadeSplits` must be stored in absolute camera-forward depth units, not the normalized
  `ShadowCascade::splitDepth` values currently stored by `ShadowPass`.
- A one-layer texture may become a native 2D SRV on DX11/DX12, which would not match a
  `Texture2DArray` shader resource. RQ14 must allocate at least two array layers for the bound shadow
  SRV/fallback.
- Even with a multi-layer texture, a single-layer DSV/SRV view can be incorrectly created as a native 2D view
  instead of a 2D-array view, losing `FirstArraySlice`. RQ14 must preserve array view semantics for
  per-cascade layer views in the backend view-desc path.
- `RGTextureHandle::Subresource()` defaults to color aspect, and RenderGraph export transitions currently
  use color aspect for subresource-tracked textures. RQ14 must construct depth ranges explicitly and fix
  export/read barrier aspect handling for depth texture arrays.
- The visual golden may change because farther shadow receivers can now sample later cascades instead
  of cascade 0. Update only the affected shadow golden after inspecting actual/diff artifacts.
- Cascade selection without blending may expose hard transition lines in large scenes. Blending is a
  follow-up, not a blocker for honest CSM consumption.

## 9. Acceptance Criteria

- Directional shadows render from a single depth Texture2DArray with one layer per active cascade.
- `DefaultLit.hlsl` samples `Texture2DArray<float>` and selects cascade by absolute camera-forward view depth.
- The frame descriptor still uses one sampled texture binding and one sampler binding.
- One-cascade configurations remain backend-compatible with the Texture2DArray shader path.
- RenderGraph barriers for CSM depth layer writes, full-array reads, and exports use `RHITextureAspect::Depth`.
- Existing reverse-Z and disabled-shadow fallback behavior remains visible and tested.
- Required validation commands pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): consume directional csm texture array`
