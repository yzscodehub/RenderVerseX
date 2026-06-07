# RQ2a - RHI Cubemap Subresource and Upload Foundation Plan

Date: 2026-06-07

Parent program: Render Quality & Verification Program v2

Previous committed stage: RQ1, `2a2c7bd feat(render): render scene color in HDR before tonemap`

## 0. Execution Rule

This stage follows the durable phase protocol:

1. Read this document before implementation and keep implementation inside this scope.
2. Run Spark plan review before code changes.
3. Implement only after Spark returns no blocker.
4. Run the validation commands in this document.
5. Run Spark code review after implementation.
6. Fix any Spark blocker before updating `phase-log.md`.
7. Commit only this stage's files after validation and Spark review pass.

## 1. Stage Decision

RQ2a implements the texture upload and RHI subresource foundation required by real texture IBL. It does not add IBL shader sampling yet. The immediate goal is to make CPU-generated cubemap, irradiance, prefiltered cubemap mip chains, and BRDF LUT resources representable and uploadable without hidden backend-specific layer mistakes.

The central contract is:

- `RHITextureDesc::arraySize` means array element count. For `TextureCube`, it means cube count, not physical face count.
- Physical subresource layers are `arraySize * 6` for `TextureCube`, otherwise `arraySize`.
- Flat subresource indices use the existing encoding: `mipLevel + physicalLayer * mipLevels`.
- `TextureResource::TextureMetadata::arrayLayers` remains the CPU metadata physical layer count. A single cubemap has `arrayLayers == 6`.

### 1.1 Binding Indexing Contract

This stage must make the following rules true in code and tests before multi-subresource upload is enabled:

- `RHITexture::GetArraySize()` continues to return the logical RHI `arraySize`. For cubemaps this is cube count.
- New helper code reports physical layer count separately. For cubemaps, physical layer `cubeIndex * 6 + faceIndex` maps to the native API face/slice/layer.
- `RHITexture::GetSubresourceIndex(mipLevel, arraySlice)` keeps its existing signature for compatibility, but the second argument is interpreted as physical layer for flat copy/upload operations.
- `RHISubresourceRange::baseArrayLayer` and `arrayLayerCount` are interpreted as physical layers for view and barrier ranges. A single full cubemap view therefore covers six physical layers even though the RHI descriptor has `arraySize == 1`.
- `GPUResourceManager` converts CPU metadata to RHI descriptors:
  - `metadata.isCubemap == true` requires `metadata.arrayLayers % 6 == 0`.
  - `textureDesc.dimension = TextureCube`.
  - `textureDesc.arraySize = metadata.arrayLayers / 6`.
- `GPUUploadService` receives texture data in RHI flat subresource order: physical-layer-major, mip-minor. Each subresource payload is tightly packed before the upload service repacks rows into aligned staging memory.
- `GPUResourceManager::PrepareTextureUpload()` is responsible for converting loader-specific CPU layouts into that RHI flat order. For `HDRTextureLoader` mipped cubemaps, this means converting from mip-major/face-major to physical-layer-major/mip-minor.

### 1.2 Blocker Resolution Order

Implementation must proceed in this order:

1. Add helper APIs and tests for physical layer count plus flat subresource encode/decode.
2. Fix backend cubemap creation and memory-requirement paths so `TextureCube.arraySize` is cube count everywhere.
3. Fix backend copy paths to decode flat subresources into mip plus physical layer/slice.
4. Only then extend `GPUUploadService` to issue multiple `CopyBufferToTexture()` commands.
5. Only then extend `GPUResourceManager::PrepareTextureUpload()` to produce cubemap/mip data.

Steps 4 and 5 must not land without steps 1-3 in the same stage, because that would turn previously honest rejection into incorrect uploads.

## 2. Current State

- `Resource::HDRTextureLoader` can generate cubemap and cubemap mip-chain `TextureResource` data, but `GPUResourceManager::PrepareTextureUpload()` rejects all cubemap, array, and mip-chain textures.
- `GPUUploadService::TryUploadTextureStaged()` supports only `Texture2D`, one mip, one array layer.
- DX11 and OpenGL currently treat `TextureCube.arraySize` as cube count and expand to physical faces internally.
- DX12 and Vulkan currently use `TextureCube.arraySize` directly as physical layers, which makes `arraySize == 1` invalid or incomplete for a single cubemap.
- Metal currently creates `MTLTextureTypeCube` for all cubemap descriptors and does not distinguish cube arrays.
- Vulkan, Metal, and OpenGL buffer-texture copy paths do not fully decode flat subresource indices into mip plus physical layer/slice. DX11/DX12 already pass the flat subresource index to the native API.
- `HDRTextureLoader::CreateCubemapTextureWithMips()` packs CPU data mip-major then face-major, while the RHI flat subresource order is layer-major then mip-minor. GPU upload preparation must bridge that mismatch.

## 3. Scope

1. Add small RHI helper functions for texture physical layer count, subresource count, flat subresource encode, and flat subresource decode.
2. Document the `TextureCube.arraySize` cube-count contract in `RHITextureDesc`.
3. Update DX12 and Vulkan texture creation for `TextureCube` to allocate `arraySize * 6` physical layers.
4. Update Metal texture creation to use `MTLTextureTypeCube` for one cube and `MTLTextureTypeCubeArray` for multiple cubes, with `arrayLength == cube count`.
5. Keep DX11 and OpenGL cubemap creation aligned with the cube-count contract; adjust only if tests or compile checks expose inconsistencies.
6. Update duplicate memory-requirement or placed-resource creation paths so the same cube-count contract is used outside ordinary `CreateTexture()`.
7. Fix Vulkan and Metal `CopyTexture`, `CopyBufferToTexture`, and `CopyTextureToBuffer` to decode flat subresource indices into mip and physical layer/slice.
8. Fix OpenGL copy paths to decode flat subresources. `CopyTexture()` must include the decoded physical layer in the source/destination z coordinates; buffer-texture copies must use decoded mip and a layered path for array/cube targets instead of hard-coding mip zero/face zero.
9. Extend `GPUUploadService` staged texture upload to support `Texture2D` arrays/mips and `TextureCube` mips by issuing one `CopyBufferToTexture()` per flat subresource with aligned staging rows.
10. Extend `GPUResourceManager::PrepareTextureUpload()` to accept RGBA8/RG8/R8/RGBA16F/RGBA32F 2D textures, 2D arrays, and cubemaps with mips when metadata and source data are consistent.
11. Repack cubemap mip-chain data from HDR-loader order into RHI flat subresource order before calling `GPUUploadService`.
12. Add tests for subresource helpers, multi-subresource upload copy descriptors, cubemap metadata preparation, and continued honest rejection of unsupported or inconsistent data.

## 4. Out of Scope

- Sampling cubemap IBL in `DefaultLit.hlsl`.
- Binding irradiance, prefiltered environment, or BRDF LUT descriptors to material pipelines.
- Generating new GPU IBL convolution passes.
- Skybox rendering.
- Compressed texture upload.
- 3D texture upload.
- Replacing approximate IBL ambient constants.
- Visual golden changes.

## 5. Expected Files

- `RHI/Include/RHI/RHITexture.h`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_DX12/Private/DX12Device.cpp` if duplicate texture creation code must stay consistent
- `RHI_Vulkan/Private/VulkanResources.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp` if duplicate texture creation code must stay consistent
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Metal/Private/MetalResources.mm`
- `RHI_Metal/Private/MetalDevice.mm` if duplicate texture creation code must stay consistent
- `RHI_Metal/Private/MetalCommandContext.mm`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `Render/Private/GPUUploadService.cpp`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUUploadServiceValidation/main.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp` or an existing RHI validation test if a common helper test fits better there
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- RHI helper tests:
  - `TextureCube` with `arraySize == 1` reports 6 physical layers and `6 * mipLevels` subresources.
  - `Texture2D` with `arraySize == N` reports `N` physical layers.
  - Encode/decode preserves `mip + physicalLayer * mipLevels`.
  - `RHISubresourceRange::All()` resolves to the physical layer count for cubemaps.
- Upload service tests:
  - A 2D texture with multiple mips issues one copy per subresource with correct `textureSubresource`, mip dimensions, row pitch, and monotonically increasing aligned buffer offsets.
  - A cubemap with two mips and one cube issues 12 copies and creates an RHI descriptor with `dimension == TextureCube`, `arraySize == 1`, and `mipLevels == 2`.
  - Too-small source data remains an honest failure and does not create a texture.
- Resource manager tests:
  - RGBA32F cubemap metadata with `arrayLayers == 6` prepares/uploads as one RHI cube with six physical faces.
  - Cubemap mip-chain data is reordered from mip-major/face-major CPU order into RHI flat layer-major/mip-minor order.
  - Invalid cubemap metadata such as `arrayLayers != 6 * cubeCount` is rejected.
- Backend contract tests or guardrails:
  - DX12/Vulkan texture resource creation and memory-requirement helpers use physical layer count for native allocation.
  - Metal uses cube-array texture type only when `arraySize > 1`.
  - Vulkan/Metal/OpenGL copy helpers decode non-zero flat subresources instead of treating the flat value as mip level.
- Existing regression tests still pass.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target GPUUploadServiceValidation GPUResourceManagerValidation RenderPassValidation ModelViewer
ctest --test-dir $B -C Debug --output-on-failure -R "GPUUploadServiceValidation|GPUResourceManagerValidation|RenderPassValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|MaterialSystemValidation|PipelineCacheValidation|ClusteredLightingValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- Backend duplicate texture creation code can drift. Mitigation: update both resource classes and device fallback creation paths where present.
- Cubemap array-size semantics are already inconsistent. Mitigation: document the RHI contract and add helper tests so future code uses one definition.
- OpenGL layered cubemap uploads may need target-specific handling. Mitigation: use decoded mip/layer explicitly and keep unsupported paths visible rather than silently uploading face zero repeatedly.
- Repacking CPU cubemap mips can transpose faces and mips if the order is wrong. Mitigation: add byte-pattern tests that prove the output order.
- Native API names differ: D3D and Vulkan allocate physical layers, while Metal cube arrays use cube count in `arrayLength`. Mitigation: backend code must call shared helper functions for physical layer math and keep native-specific conversion local.

## 9. Acceptance Criteria

- `TextureCube.arraySize` is consistently treated as cube count by RHI helpers and backend texture creation.
- Flat subresource decode is used by Vulkan and Metal copy paths, and OpenGL no longer hard-codes mip zero for buffer-texture copies.
- `GPUUploadService` can stage-copy multi-mip and cubemap data with one copy command per flat subresource.
- `GPUResourceManager` can upload CPU-generated RGBA32F cubemap and cubemap mip resources without rejecting them as unsupported.
- Unsupported layouts still fail honestly with no empty texture creation.
- Required validation commands pass.
- Spark plan review and Spark code review report no blocker.

## 10. Spark Review

Plan review: pending

Code review: pending

Commit message: `feat(render): support cubemap texture upload foundation`
