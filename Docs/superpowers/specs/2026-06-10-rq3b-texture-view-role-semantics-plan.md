# RQ3b - Texture View Role Semantics Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ3a
Previous stage: RQ3a - Directional Shadow Sampling Bridge

## 1. Stage Decision

RQ3a connected directional shadow sampling, but it exposed a lower-level rendering infrastructure problem: `RHITextureViewDesc` does not say whether a view is an SRV, RTV, DSV, or UAV. `ResourceViewCache` therefore keys views only by texture, format, dimension, and subresource range. This lets semantically different views alias when their visible descriptor fields match, especially depth DSV vs depth SRV paths.

RQ3b makes texture view role an explicit RHI and cache concept. This is a foundation stage for reliable shadows, post-process targets, deferred/G-buffer work, and future multi-backend validation. It does not improve shadow filtering or visual quality directly; it removes a class of false-success binding bugs.

## 2. Current Engine Evidence

- `RHITextureViewDesc` contains `format`, `dimension`, `subresourceRange`, and `debugName`, but no view role.
- `ResourceViewCache::TextureViewKey` excludes `debugName` correctly, but also cannot include SRV/RTV/DSV/UAV role because no such field exists.
- `ResourceViewCache::GetDefaultSRV()`, `GetDefaultRTV()`, `GetDefaultDSV()`, and `GetDefaultUAV()` differ mostly by debug name and subresource range; debug name is intentionally not part of cache identity.
- `ShadowPass` creates cascade DSVs through `GetDefaultDSV()`.
- `OpaquePass` requests a `DirectionalShadowSRV`, but without a role field it can collide with a pre-existing depth view when format/aspect/range match.
- DX11 and DX12 `TextureView` constructors currently create all possible native handles allowed by the underlying texture usage, not only the view role being requested.
- Vulkan creates one `VkImageView`, but its aspect mask is inferred from texture usage rather than the requested view role/aspect.
- Metal creates a texture view by format only; the RHI still needs role identity for cache correctness even if the native object is the same kind of handle.

## 3. Scope

1. Add explicit texture view role to RHI.
   - Add a small enum, for example `RHITextureViewType : uint8`.
   - Values: `ShaderResource`, `RenderTarget`, `DepthStencil`, `UnorderedAccess`.
   - Add `type` to `RHITextureViewDesc`, defaulting to `ShaderResource` for compatibility with existing SRV-style call sites.

2. Make ResourceViewCache role-aware.
   - Include `RHITextureViewDesc::type` in `TextureViewKey` equality and hashing.
   - Set explicit type in `GetDefaultSRV/RTV/DSV/UAV`.
   - Keep `debugName` excluded from identity.
   - Add tests proving SRV and DSV for the same depth texture do not alias.

3. Make shadow sampling request a real SRV.
   - In `OpaquePass`, set `shadowViewDesc.type = ShaderResource`.
   - Keep explicit depth aspect for depth textures.
   - Update the missing-shadow-SRV test so it fails only shader-resource shadow views, not the DSV path.

4. Create only the requested native view role in backends.
   - DX11: create only SRV/RTV/DSV/UAV matching `desc.type`.
   - DX12: allocate only the matching descriptor heap handle for the requested role, and update descriptor-set binding so sampled textures copy SRV handles while storage textures copy UAV handles.
   - Vulkan: create a `VkImageView` with aspect mask derived from `desc.subresourceRange.aspect` and requested type, not just texture usage.
   - Metal: preserve native texture view behavior but carry the descriptor role through RHI/cache identity.
   - OpenGL: propagate role through `OpenGLTextureView` and swapchain/manual view descriptors even when native GL texture-view creation does not need a separate object type.

5. Fail unsupported role requests honestly.
   - `CreateTextureView` should return `nullptr` when the requested role is incompatible with texture usage or format.
   - `ShaderResource` requires `RHITextureUsage::ShaderResource`.
   - `RenderTarget` requires `RHITextureUsage::RenderTarget`.
   - `DepthStencil` requires `RHITextureUsage::DepthStencil` and a depth/stencil format.
   - `UnorderedAccess` requires `RHITextureUsage::UnorderedAccess`.
   - Backbuffer and render-target call sites must request the correct role instead of relying on the default SRV role.

6. Patch explicit role call sites.
   - Swapchain backbuffer views must request `RenderTarget`.
   - Manually created depth views must request `DepthStencil`.
   - Manually created material/skybox/fallback sampled views must request `ShaderResource`.
   - ResourceViewCache default helpers should cover most render-graph pass code.

7. Validation and guardrails.
   - Extend `ResourceViewCacheValidation` for role identity.
   - Extend `RenderPassValidation` for shadow DSV/SRV separation and missing shadow SRV fallback.
   - Add DX12 descriptor binding coverage or validation to prove `RHIBindingType::StorageTexture` uses `DX12TextureView::GetUAVHandle()` and `SampledTexture` uses `GetSRVHandle()`.
   - Run focused RHI/render validation and ModelViewer smoke.

## 4. Out of Scope

- PCF/PCSS/EVSM, cascade stabilization, atlas packing, contact shadows, point/spot shadows.
- Changing shadow compare math or visual tuning.
- Replacing `ResourceViewCache` lifetime policy.
- Reworking RenderGraph resource state tracking.
- Full descriptor/bindless redesign.
- ECS/Object/RenderProxy refactors.

## 5. Expected Files

- `RHI/Include/RHI/RHITexture.h`
- `Render/Include/Render/Graph/ResourceViewCache.h`
- `Render/Private/Graph/ResourceViewCache.cpp`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX11/Private/DX11SwapChain.cpp`
- `RHI_DX12/Private/DX12Pipeline.h`
- `RHI_DX12/Private/DX12Pipeline.cpp`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_DX12/Private/DX12SwapChain.cpp`
- `RHI_Vulkan/Private/VulkanResources.cpp`
- `RHI_Vulkan/Private/VulkanSwapChain.cpp`
- `RHI_Metal/Private/MetalDevice.mm`
- `RHI_Metal/Private/MetalSwapChain.mm`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`
- `RHI_OpenGL/Private/OpenGLSwapChain.cpp`
- Swapchain/manual view call sites as needed:
  - `Render/Private/Renderer/SceneRenderer.cpp`
  - `Render/Private/Passes/OpaquePass.cpp`
  - `Render/Private/Passes/SkyboxPass.cpp`
  - `Render/Private/Material/MaterialSystem.cpp`
  - `Render/Private/PipelineCache.cpp`
- `Tests/ResourceViewCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- `ResourceViewCacheValidation`: key identity differs by view type and still ignores `debugName`.
- `ResourceViewCacheValidation`: two descriptors with the same texture, format, dimension, subresource range, and aspect but different `RHITextureViewDesc::type` must produce different keys and different cached views.
- `ResourceViewCacheValidation`: `GetDefaultDSV(depth)` and `GetDefaultSRV(depth)` create two cached views and then hit their own cache entries on repeated calls.
- `RenderPassValidation`: `OpaquePassReportsMissingShadowSRVWhenRequestedReadCannotResolveView` must simulate SRV failure without failing ShadowPass DSV creation.
- Existing RQ3a `OpaquePassDeclaresDirectionalShadowReadDuringSetup` must keep passing.
- Existing PipelineCache shadow descriptor/frame-constant tests must keep passing.
- DX12 descriptor validation: storage texture bindings must copy UAV handles, sampled texture bindings must copy SRV handles.
- Backend role validation: unsupported role requests return null rather than producing a non-null RHI view with a missing native handle.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ResourceViewCacheValidation RenderPassValidation PipelineCacheValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ResourceViewCacheValidation|RenderPassValidation|PipelineCacheValidation"
cmake --build $B --config Debug --target DX11Validation DX12Validation VulkanValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "DX11Validation|DX12Validation|VulkanValidation|ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ3b_DamagedHelmet.ppm --validation --expect-ibl-ready --expect-procedural-ibl-quality
git diff --check
```

If a backend validation target is unavailable on the current machine, record it as skipped with the concrete reason in `phase-log.md`.

## 8. Risks

- Swapchain backbuffer views may rely on default `RHITextureViewDesc`; those call sites must request `RenderTarget`, including OpenGL proxy backbuffers.
- DX11/DX12 command contexts may receive a view with the wrong role and then find a null native handle; tests should catch the expected pass paths.
- DX12 descriptor tables already distinguish SRV vs UAV ranges; descriptor updates must choose texture view handles from the descriptor layout entry, not from texture view existence alone.
- Vulkan depth-stencil aspect selection is easy to get wrong: shader depth SRV should not automatically include stencil, while DSV can include depth-stencil when requested.
- `RHITextureViewDesc::type` defaulting to `ShaderResource` preserves compatibility but can hide missed explicit call sites; RQ3b should fix known render-target/depth manual sites.

## 9. Acceptance Criteria

- Texture view role is part of RHI view description and ResourceViewCache identity.
- SRV/RTV/DSV/UAV default helper views no longer alias solely because format/range match.
- DX11/DX12 create only the requested native view descriptor for a `RHITextureView`.
- DX12 descriptor-set updates bind SRV handles for sampled textures and UAV handles for storage textures.
- OpenGL role propagation and swapchain backbuffer role call sites are covered.
- Vulkan aspect mask respects requested depth/color semantics.
- RQ3a shadow fallback tests distinguish missing SRV from DSV creation.
- Required validation commands pass or have an explicit, justified skip.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ3b scope as the task list. Do not expand into shadow quality algorithms, visual tuning, RenderGraph state tracking rewrites, or broad RHI descriptor/bindless redesign.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; initial BLOCKED items resolved: DX12 SRV/UAV descriptor binding scope, OpenGL coverage, and strict same-descriptor/different-type cache identity test)
- Code review: PASS (`gpt-5.5`, xhigh; initial BLOCKED item resolved: DX12 descriptor-set creation now fails when initial bindings cannot be applied, with creation-time binding coverage)
- Commit message: `feat(rhi): make texture view roles explicit`
