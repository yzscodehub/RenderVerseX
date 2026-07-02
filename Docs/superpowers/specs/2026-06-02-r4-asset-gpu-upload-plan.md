# R4 Asset GPU Upload Implementation Plan

**Date:** 2026-06-02
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `9. R4 - Asset GPU Upload`
**Sub-stage:** R4 - Asset GPU Upload
**Prerequisite:** R3b committed as `d99214b`

---

## 1. Stage Goal

R4 ensures render assets become real, stable GPU resources through the existing upload infrastructure:

- glTF mesh resources upload their vertex/index streams into real RHI buffers and expose stable `ResourceId` lookup.
- Texture resources upload to real RHI textures with correct format/data handling for ModelViewer-relevant 2D textures.
- Material texture handles can resolve to resident GPU texture views through `GPUResourceManager` and `ResourceViewCache`.
- GPUResourceManager cache identity is stable by `ResourceId`, and replacement/eviction invalidates dependent texture views before releasing old texture objects.
- Upload completion/failure states remain honest and test-visible.

R4 is not a material binding stage. R5 owns material compile/bind behavior and SceneRenderer material binding integration.

Plan review gate:

- Spark plan review should judge whether this plan targets current R4 gaps without drifting into R5 material binding or R8 RenderProxy work.
- Current code already has substantial GPU upload infrastructure. R4 should harden and complete the missing identity/format/material-resolve pieces instead of replacing the whole system.

Visual gate: N/A for R4. R7 owns golden-image baselines, and R12 owns final ModelViewer acceptance.

---

## 2. Current Code Observations

- `GPUUploadService` already supports staged buffer upload and staged 2D single-mip texture upload with fence tracking.
- `GPUResourceManager::UploadMesh()` already uploads position/index buffers and optional normal/uv/tangent buffers through `GPUUploadService`.
- `GPUResourceManager::UploadTexture()` maps `TextureResource` metadata to an RHI texture description and calls `GPUUploadService`.
- `TextureLoader::DecodeImage()` forces decoded image data to RGBA, so ordinary external/embedded glTF textures normally become `TextureFormat::RGBA8`.
- `TextureResource` still supports `RGB8`; `GPUResourceManager::UploadTexture()` maps `RGB8` to an `RGBA8` RHI format without expanding 3-channel data. That can make valid RGB data fail the upload service size check.
- `GPUUploadService::TryUploadTextureStaged()` only supports 2D, one mip, one array layer, uncompressed non-depth formats. Unsupported texture layouts should remain honest failures for R4.
- `MaterialSystem::ResolveTextureView()` already checks `GPUResourceManager::IsGPUReady(textureId)` and gets SRVs through `ResourceViewCache`, but `MaterialSystemValidation` currently only tests alpha-mode classification.
- `ResourceViewCache` has generation and `InvalidateTexture()` support. `GPUResourceManager` already exposes `SetTextureInvalidatedCallback()` and calls it on eviction/replacement.

---

## 3. Approved Scope

### R4.1 - GPUResourceManager texture upload preparation

Files:

- `Render/Include/Render/GPUResourceManager.h`
- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`

Tasks:

- Add a small internal texture upload preparation path that validates metadata and prepares source bytes for `GPUUploadService`.
- Keep ModelViewer-relevant support focused on 2D, single mip, single array-layer textures.
- Map supported `TextureFormat` values to RHI formats with explicit source bytes-per-pixel expectations.
- Expand `TextureFormat::RGB8` CPU data to RGBA8 before upload, filling alpha with 255.
- Reject unsupported or inconsistent texture metadata/data sizes without creating placeholder GPU textures.
- Ensure failure sets `GPUResourceState::Failed` and does not leave partially resident texture cache entries.

Acceptance:

- RGBA8 texture resources upload successfully.
- RGB8 texture resources upload successfully through explicit RGBA expansion.
- Unsupported compressed, cubemap, array, 3D, or mip-chain layouts fail visibly unless the current upload service can truly handle them.

### R4.2 - Stable GPU cache identity and replacement behavior

Files:

- `Render/Private/GPUResourceManager.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`

Tasks:

- Keep cache identity keyed by `ResourceId`.
- Re-uploading an already resident resource with the same `ResourceId` should not create duplicate GPU resources through normal request/immediate paths.
- Replacing a resident texture entry for the same `ResourceId` should invalidate dependent views before dropping the old `RHITexture`.
- Memory usage should only count resident resources once and should be updated when resources complete, fail, replace, or evict.
- Pending upload IDs must be abandoned when a resource fails, is replaced, or is evicted.

Acceptance:

- Tests can assert duplicate upload avoidance, replacement invalidation, memory accounting, and failed upload state.

### R4.3 - Material texture reference GPU resolution

Files:

- `Render/Private/Material/MaterialSystem.cpp` only if visibility/diagnostics fixes are required
- `Tests/MaterialSystemValidation/main.cpp`

Tasks:

- Add validation coverage showing a `MaterialResource` texture handle resolves to a resident GPU texture view via `GPUResourceManager` and `ResourceViewCache`.
- Add validation coverage showing a non-resident or failed texture handle falls back to the default material texture without claiming success.
- Keep descriptor creation through the existing `MaterialSystem` path, but do not redesign binding semantics.

Acceptance:

- Material texture references are demonstrably connected to GPU-resident texture resources.
- Missing/non-resident texture resources remain fallback-visible and do not break descriptor creation.

### R4.4 - Upload service synchronization protection

Files:

- `Render/Private/GPUUploadService.cpp` only if tests expose a bug
- `Tests/GPUUploadServiceValidation/main.cpp`

Tasks:

- Ensure staged uploads are not marked complete until fence completion or explicit WaitIdle fallback.
- Ensure abandoned uploads do not leak into the completed upload set.
- Keep `SubmitCommandContext()` returned fence values as the source of truth.

Acceptance:

- Existing `GPUUploadServiceValidation` coverage remains green; add tests only if a newly found gap is real.

---

## 4. Out of Scope

- Full material binder/template compile wiring. That is R5a/R5b.
- SceneRenderer material state binding beyond proving texture handles can resolve to GPU views.
- Compressed texture GPU uploads, mip-chain uploads, cubemap uploads, texture arrays, or 3D textures unless the current upload service truly supports them.
- Asset database/editor import workflow changes beyond preserving stable `ResourceId` identity already produced by loaders.
- RenderProxy work.
- Visual golden or ModelViewer final validation.

---

## 5. Implementation Order

1. Backfill the R3b commit hash in `phase-log.md`.
2. Add texture upload preparation/validation helpers in `GPUResourceManager`.
3. Add or adjust `GPUResourceManagerValidation` tests for RGB8 expansion, unsupported layout failure, duplicate identity, replacement invalidation, and memory accounting.
4. Add `MaterialSystemValidation` fake RHI/GPUResourceManager/ResourceViewCache coverage for resident texture resolve and fallback on non-resident texture.
5. Build and run required validation.
6. Run Spark code review.
7. Update `phase-log.md`.
8. Commit R4 before moving to R5a.

---

## 6. Validation Commands

Expected commands:

```powershell
cmake --build build/win_x64_debug --config Debug --target GPUUploadServiceValidation GPUResourceManagerValidation ResourceInstantiationValidation RenderHonestyValidation MaterialSystemValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "GPUUploadServiceValidation|GPUResourceManagerValidation|ResourceInstantiationValidation|RenderHonestyValidation|MaterialSystemValidation"
git diff --check
```

Optional if implementation touches backend-sensitive upload behavior:

```powershell
cmake --build build/win_x64_debug --config Debug --target DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "DX12Validation|VulkanValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- Mesh resources remain uploadable to real RHI buffers and covered by validation.
- Texture resources upload to real RHI textures for supported ModelViewer-relevant formats.
- RGB8 CPU texture data is handled honestly rather than failing due to an implicit RGBA format mismatch.
- Unsupported texture layouts fail visibly without placeholder GPU resources.
- Material texture handles can resolve to GPU-resident texture views through the existing MaterialSystem path.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R4 evidence.
- R4 commit is created before R5a starts.
