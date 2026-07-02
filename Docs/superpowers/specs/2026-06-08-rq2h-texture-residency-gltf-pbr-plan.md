# RQ2h - Texture Residency and glTF PBR Color-Space Correctness Plan

Date: 2026-06-08
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2g, commit `b47435d fix(resource): honor HDR IBL sampling controls`

## 1. Stage Decision

RQ2h fixes two render-quality foundations exposed by the DamagedHelmet ModelViewer run:

- `GPUResourceManager::UploadImmediate(TextureResource*)` currently acts as an unconditional same-id refresh path, so sample/helper code that calls it repeatedly can recreate and re-upload resident textures.
- `GLTFImporter::ExtractTextures()` creates one `TextureReference` per glTF image and leaves every reference at the default `TextureUsage::Color` / `isSRGB = true`, so normal, metallic-roughness, and occlusion maps can be uploaded as sRGB textures instead of linear data.

This stage is a correctness and residency stage. It does not change BRDF math, IBL convolution, skybox quality, tonemap, exposure, bloom, or shadows.

## 2. Current State Evidence

- `Samples/ModelViewer` now defaults to `DamagedHelmet.glb` when present on the desktop and renders successfully, but runtime logs show repeated `Created texture on GPU: DamagedHelmet` messages across frames.
- `Render/Private/GPUResourceManager.cpp` has an early resident skip for `UploadImmediate(MeshResource*)`, but `UploadImmediate(TextureResource*)` always removes queued same-id uploads and calls `UploadTexture()`.
- `SceneRenderer::SetupView()` requests texture uploads with `RequestUpload()` only when non-resident; this path is already idempotent.
- ModelViewer IBL setup calls `UploadImmediate()` for procedural/HDRI textures. Repeated immediate calls should be safe and no-op for already resident resources unless an explicit refresh API is requested.
- `Resource/Private/Importer/GLTFImporter.cpp` maps material slots to `TextureInfo`, but never writes color-space usage back to `GLTFImportResult::textures`.
- `Resource/Private/Loader/TextureLoader.cpp` and `GPUResourceManager::PrepareTextureUpload()` already honor `TextureResource::IsSRGB()`, so the missing importer metadata is the upstream bug.

## 3. Scope

1. Make immediate texture upload idempotent for resident resources:
   - Add the same resident early-out behavior that meshes already have.
   - Preserve queued stale same-id removal before a blocking upload starts.
   - Keep failed/non-resident retry behavior intact.
   - Do not add a broad refresh system in this stage; if same-id refresh is needed later it should be an explicit API, not the default `UploadImmediate()` behavior.
   - Replace the existing resident texture replacement tests that currently assert same-id `UploadImmediate()` invalidates/replaces GPU data. The new contract is: resident same-id immediate upload is a no-op, returns the same `RHITexture*`, does not invoke `TextureInvalidatedCallback`, does not create another RHI texture, leaves memory unchanged, and keeps state `GPUReady`.
2. Annotate glTF texture references by material slot:
   - Base color and emissive textures stay `TextureUsage::Color`, `isSRGB = true`.
   - Normal textures become `TextureUsage::Normal`, `isSRGB = false`.
   - Metallic-roughness and occlusion textures become `TextureUsage::Data`, `isSRGB = false`.
   - If a single glTF image is referenced by incompatible slots, log a visible warning and resolve deterministically by safety precedence: `Color < Data < Normal`.
   - Compatible slot reuse is warning-free for `Color+Color` and `Data+Data`.
   - `Color+Data`, `Color+Normal`, and `Data+Normal` are incompatible; the chosen higher-precedence linear/normal usage prevents data/normal maps from being accidentally uploaded as sRGB while preserving the current image-based resource identity.
3. Add tests that would fail on the old behavior:
   - `GPUResourceManagerValidation`: resident `UploadImmediate(texture)` is a no-op and does not create a second GPU texture.
   - `GPUResourceManagerValidation`: update/replace `FailedTextureReplacementInvalidatesExistingResidentTexture` and `SuccessfulTextureReplacementInvalidatesExistingResidentTexture` with resident no-op assertions.
   - `ResourceInstantiationValidation`: importing a small temporary glTF marks baseColor/emissive as sRGB color and normal/MR/AO as linear normal/data, asserted by resolved image index from each material `TextureInfo::imageId`.
   - `ResourceInstantiationValidation`: add one shared-image conflict fixture, at minimum baseColor plus metallic-roughness, proving the warning-safe linear `Data` fallback is deterministic.
   - Existing texture upload tests for unsupported data and queued stale upload removal must remain valid.
4. Run ModelViewer after the fix:
   - Use the default DamagedHelmet path when available.
   - Capture a screenshot artifact.
   - Confirm the model still renders and repeated resident texture upload spam is reduced for the immediate-upload path.

## 4. Out of Scope

- BRDF math changes in `DefaultLit.hlsl`.
- Real production HDRI asset selection or default HDRI switching.
- GPU IBL convolution or compute prefiltering.
- Tonemap operator changes, auto exposure, bloom tuning, color grading, or shadows.
- Changing `SceneRenderer::RequestUpload()` scheduling.
- Reworking texture identity to support the same image uploaded once as sRGB and once as linear.
- Mipmap generation or sampler-state import.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-08-rq2h-texture-residency-gltf-pbr-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Private/GPUResourceManager.cpp`
- `Resource/Include/Resource/Importer/GLTFImporter.h`
- `Resource/Private/Importer/GLTFImporter.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/ResourceInstantiationValidation/main.cpp`

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target GPUResourceManagerValidation ResourceInstantiationValidation ModelViewer`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "GPUResourceManagerValidation|ResourceInstantiationValidation"`
- Visual stability:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke|ModelViewerHDRISmoke"`
- Regression:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderSceneValidation|RenderPassValidation|GPUUploadServiceValidation|HDRTextureLoaderValidation"`
- Manual visual check:
  - `build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ2h_DamagedHelmet.ppm --validation`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- Calling `UploadImmediate()` on a GPU-ready texture does not recreate or re-upload the texture and does not invalidate existing views.
- Calling `UploadImmediate()` on a queued, non-resident same-id texture still removes the queued request and performs the immediate upload or records failure honestly.
- glTF base color and emissive images import as sRGB color textures.
- glTF normal, metallic-roughness, and occlusion images import as linear textures with `TextureUsage::Normal` / `TextureUsage::Data`.
- glTF shared-image slot conflicts resolve by the documented `Color < Data < Normal` precedence and are covered by a focused fixture.
- DamagedHelmet still renders in ModelViewer after the change.
- Required validation passes.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only the RQ2h scope above as the task list. Do not expand into BRDF/IBL/shadow/tonemap work during this stage. This stage is allowed to touch existing dirty files only if they are inside the approved scope and the current contents are preserved rather than reverted.

## 9. Spark Review

- Plan review: pending (`gpt-5.5`, xhigh)
- Code review: pending (`gpt-5.5`, xhigh)
- Commit message: `fix(render): make texture uploads idempotent and import gltf pbr color space`
