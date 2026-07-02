# RQ6 - Texture Mip Chain And Material Sampler Quality Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ5
Previous stage: RQ5 - Texture Fallback Provenance Guard

## 1. Stage Decision

Add a conservative, testable texture-quality baseline for ordinary material textures: CPU-generated mip chains for decoded 2D RGBA8 textures, GPU upload validation for those mip chains, and an explicit high-quality material sampler policy. This improves model stability and texture minification quality without changing the render architecture, ECS, RenderProxy, lighting model, or post-process stack.

This is the next highest value step after RQ4/RQ5 because the renderer now has real PBR texture gates, but ordinary `TextureLoader` output is still single-mip and `MaterialSystem` uses a single default `LinearWrap` sampler. DamagedHelmet-like glTF assets benefit directly from predictable mip upload and mip-filtered sampling.

## 2. Current Engine Evidence

- `TextureLoader::DecodeImage()` forces decoded images to RGBA (`STBI_rgb_alpha`), and `CreateTextureResource()` still sets `metadata.mipLevels = 1`.
- `TextureLoader::LoadFromMemory()` raw glTF pixel paths call `CreateTextureResource()` with RGBA data as well.
- `GPUResourceManager::PrepareTextureUpload()` already supports 2D mip chains when `metadata.mipLevels > 1` and source data is packed mip0, mip1, ... for a single layer.
- `GPUUploadService::TryUploadTextureStaged()` already walks physical layers and mip levels and creates per-subresource `CopyBufferToTexture` commands.
- `RHITextureDesc`, `RHITextureViewDesc`, and render graph internals already carry mip counts.
- `RHISamplerDesc` already exposes `mipFilter`, `anisotropyEnable`, and `maxAnisotropy`; `MaterialSystem::CreateDefaultResources()` currently creates `RHISamplerDesc::LinearWrap()` as `DefaultMaterialSampler`.
- Existing validation covers cubemap mip repacking but does not prove ordinary loader-produced 2D textures generate and upload mip chains.

## 3. Scope

1. Add CPU mip generation for ordinary decoded 2D RGBA8 textures.
   - In `TextureLoader`, generate a full mip chain for eligible textures: `floor(log2(max(width, height))) + 1`.
   - Compute each mip dimension with the same rule used by `GPUResourceManager`: `max(1, width >> mip)` and `max(1, height >> mip)`.
   - Pack data in the layout expected by `GPUResourceManager`: mip0 followed by mip1 ... mipN for a single ordinary 2D layer.
   - Preserve 1x1 textures as a single mip.
   - Use a deterministic box filter.
   - For color/sRGB textures, perform RGB downsampling in linear space with standard sRGB transfer, then encode RGB back to sRGB bytes; alpha is only averaged linearly and is not sRGB-converted.
   - For normal textures, downsample in signed normal space, renormalize, and encode back to `[0,255]`, preserving alpha as averaged. If the averaged signed normal is zero length, use flat normal `(0, 0, 1)`.
   - For data textures, average bytes linearly without sRGB conversion.

2. Make external glTF texture loading usage-aware before mip generation.
   - `LoadFromReference()` external paths must not generate mip chains from filename-guessed usage and then overwrite `TextureUsage`/sRGB metadata afterward.
   - Add a usage-aware load path or equivalent helper so external references pass `ref.usage` and `ref.isSRGB` into resource creation before mip generation.
   - Cache identity must include usage/sRGB policy when those values affect generated texture bytes. The same source path loaded once as Color/sRGB and once as Normal/Data must not reuse a resource generated with the wrong mip policy.
   - Standalone `LoadFromFile()` may keep filename-based usage inference, but it should route through the same internal creation path with the inferred usage/sRGB policy.
   - Successful cache hits must preserve the correct non-fallback provenance from RQ5.

3. Keep fallback/default textures simple.
   - White, normal, and error default fallback textures may remain single-mip. RQ6 does not change fallback visuals.

4. Honor generated mip metadata during upload.
   - Add or extend `GPUResourceManagerValidation` with an ordinary 2D mip-chain upload test that checks:
     - `createdTextureDesc.mipLevels` matches metadata.
     - staged copy emits one subresource per mip.
     - subresource indices use `EncodeTextureSubresource(mip, 0, mipLevels)`.
     - row-pitched staging contains expected bytes at the recorded mip offsets.
   - This is mostly a validation stage because upload infrastructure already exists.

5. Make the material sampler quality policy explicit.
   - Change `MaterialSystem` default material sampler creation from implicit `LinearWrap()` to an explicit material sampler descriptor:
     - min/mag/mip linear.
     - wrap addressing.
     - hard requirement: linear mip filtering is explicit and tested.
     - optional policy: anisotropy enabled with a conservative max such as 8 or 16 if current backends and fake devices accept it.
   - If backend/device sampler creation fails for anisotropic descriptor, fall back visibly to linear mip sampler and record/test that fallback path if needed.
   - Do not introduce per-texture/per-material sampler states in this stage.

6. Tests.
   - Extend `RenderHonestyValidation` or add a focused test proving `TextureLoader::LoadFromMemory()` produces a multi-mip non-fallback texture for a 4x4 RGBA input and preserves single-mip for 1x1/default fallback textures.
   - Add deterministic CPU mip tests for:
     - Color/sRGB: mip generation averages in linear space and encodes back to sRGB, not raw gamma bytes.
     - Normal: mip generation averages signed normals and renormalizes, not raw color bytes.
     - Data: mip generation averages byte channels linearly without sRGB conversion.
   - Add an external `LoadFromReference()` test proving explicit `TextureReference::usage`/`isSRGB` controls mip policy even when the filename does not contain normal/data hints.
   - Add a cache test proving the same external source path can produce separate Color/sRGB and Normal/Data resources when usage/sRGB differ.
   - Extend `GPUResourceManagerValidation` for ordinary 2D mip-chain upload.
   - Extend `MaterialSystemValidation` to assert the material sampler descriptor has linear mip filtering and the chosen anisotropy policy.
   - Keep RQ4/RQ5 material visual gates green.

7. Documentation and phase log.
   - Record the stage in `Docs/superpowers/specs/phase-log.md`.

## 4. Out of Scope

- GPU compute mip generation, streaming mips, virtual texturing, BC/compressed texture support, KTX/DDS import, texture transform support, per-material sampler variants, sampler descriptor cache redesign, bindless descriptor work, DX12/Vulkan visual expansion, post-process feature completion, or BRDF changes.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq6-texture-mip-sampler-quality-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Resource/Include/Resource/Loader/TextureLoader.h`
- `Resource/Private/Loader/TextureLoader.cpp`
- `Resource/Private/ResourceManager.cpp` if generic manager loading needs to avoid duplicate loader-policy cache aliases
- `Render/Private/Material/MaterialSystem.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp`

## 6. Required Tests

- `TextureLoader` generated mip count/data test passes for a 4x4 ordinary RGBA texture.
- `TextureLoader` deterministic color/sRGB mip test proves linear-space averaging.
- `TextureLoader` deterministic normal mip test proves signed-space averaging and renormalization.
- `TextureLoader` deterministic data mip test proves raw byte-linear averaging.
- `TextureLoader::LoadFromReference()` external path uses explicit `TextureReference` usage/sRGB before mip generation and keeps separate cache entries for distinct usage/sRGB policies.
- Generic `ResourceManager::LoadResource(path)` texture loading does not leave a stale generic-loader cache alias after the manager rewrites the resource to its path identity; `Unload(path)` removes the single generic cache entry.
- Generic `ResourceManager::LoadResource(path)` must not steal, mutate, or remove an existing TextureLoader policy-cached resource created through `LoadFromReference()` for the same source path.
- `TextureLoader` keeps 1x1/default fallback textures single-mip and fallback provenance intact.
- `GPUResourceManagerValidation` ordinary 2D mip upload test passes and asserts mip subresource order/offset bytes match the packed source layout.
- `MaterialSystemValidation` sampler quality policy test passes.
- RQ4/RQ5 gates remain green:
  - `RenderHonestyValidation`
  - `GPUResourceManagerValidation`
  - `MaterialSystemValidation`
  - `ModelViewerPBRMaterialSmoke`
  - `PBRMaterialVisualGoldenValidation`
- Focused render regressions remain green:
  - `RenderPassValidation`
  - `RenderSceneValidation`
  - `PipelineCacheValidation`
- `git diff --check` passes.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target RenderHonestyValidation GPUResourceManagerValidation MaterialSystemValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "RenderHonestyValidation|GPUResourceManagerValidation|MaterialSystemValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"
git diff --check
```

## 8. Risks

- sRGB mip generation can darken/brighten textures if averaged in gamma space. RQ6 must use linear-space averaging for `TextureUsage::Color`.
- Normal map mip generation can flatten or bias normals if averaged as raw colors. RQ6 must decode to signed normals and renormalize.
- External glTF textures can be mis-mipped if filename-based `LoadFromFile()` guessing runs before `TextureReference` usage/sRGB metadata is applied. RQ6 must make the external reference path usage-aware before resource creation and include usage/sRGB in cache identity.
- Existing fake devices may not expose or preserve anisotropic sampler descriptors. If that happens, make the explicit linear mip sampler the hard requirement and leave anisotropy as a tested optional/fallback behavior.
- More CPU data increases memory use by about one third for mipped RGBA textures. This is expected but should be reflected by `TextureResource::GetMemoryUsage()` automatically because it returns vector size.
- Generated mips change uploaded texture bytes and may subtly change ModelViewer output. RQ6 should run existing visual gates but should not recapture goldens unless the diff is intentional and inspected.

## 9. Acceptance Criteria

- Ordinary 2D decoded material textures can carry a full mip chain before GPU upload.
- External glTF texture references generate mips with their explicit usage/sRGB policy, not filename guesses, and cache keys cannot reuse a differently generated mip policy.
- GPU upload creates an RHI texture with the generated mip count and copies all mip subresources.
- Material sampler policy explicitly enables mip filtering, with anisotropy used or visibly/fallback-tested if unsupported.
- Default fallback texture provenance from RQ5 remains intact.
- RQ4/RQ5 material visual gates and focused render regressions pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ6 scope as the task list. Do not expand into streaming, compressed texture formats, per-material sampler states, post-process rewrites, or BRDF/lighting changes.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `feat(render): generate texture mips for material sampling`
