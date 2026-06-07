# RQ2c - ModelViewer Procedural IBL Wiring Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2b, commit `cad6ef0 feat(render): bind texture IBL resources for default lit`

## 1. Stage Decision

RQ2c makes the RQ2b texture IBL path observable in the ModelViewer sample without requiring an external HDR/EXR asset. ModelViewer will create a tiny procedural IBL resource set, attach it to a `SkyboxComponent`, upload those textures immediately, and optionally assert that the renderer reported texture IBL ready during smoke mode.

This is a sample wiring and verification stage. It does not implement skybox drawing and does not add CPU/GPU HDR environment convolution.

## 2. Current State Evidence

- RQ2b added renderer-side texture IBL binding, fallback resources, `SkyboxComponent` discovery, and `SceneEnvironmentIBLStats`.
- `SceneRenderer::UpdateEnvironmentIBL()` only enables texture IBL when irradiance, prefiltered environment, and BRDF LUT resources are GPU-ready and SRVs resolve.
- ModelViewer currently loads a glTF model and renders it, but it does not create any `SkyboxComponent` or IBL resources.
- ModelViewer also does not call `RenderSubsystem::ProcessGPUUploads()` during its smoke loop. A queued IBL upload can therefore remain not-ready unless the sample uploads the procedural resources immediately or changes upload scheduling.
- The repository has no committed `.hdr` or `.exr` environment fixture.
- Existing `ModelViewerSmoke` drives `VisualGoldenValidation` with a zero-tolerance golden image. Changing that smoke output unintentionally would make the visual gate noisy.

## 3. Scope

1. Add ModelViewer options:
   - `--no-ibl`: disable sample procedural IBL and preserve the old approximate ambient path.
   - `--expect-ibl-ready`: in smoke mode, fail if `SceneRenderer::GetEnvironmentIBLStats().textureIBLEnabled` was not true on the final frame.
   - Keep procedural IBL enabled by default for normal ModelViewer runs.
2. Create small procedural CPU texture resources in ModelViewer:
   - Irradiance cubemap: one cube, low resolution, RGBA8 or RGBA16F-compatible data.
   - Prefiltered environment cubemap: one cube with a small mip chain, in the same mip-major / face-major CPU order produced by `HDRTextureLoader`.
   - Let `GPUResourceManager::PrepareTextureUpload()` repack the mipped cubemap into RQ2a's RHI upload order.
   - BRDF LUT: small 2D texture.
   - Use stable non-zero resource IDs and keep `ResourceHandle`s alive through the owning `SkyboxComponent`.
3. Attach a scene `SkyboxComponent`:
   - `SetIrradianceMap()`
   - `SetPrefilteredMap()`
   - `SetBRDFLUT()`
   - `SetExposure()` / `SetContributesToLighting(true)`
   - Do not rely on `SkyboxPass` drawing.
4. Upload procedural IBL resources immediately after the render subsystem and scene are ready:
   - Use `renderSubsystem->GetGPUResourceManager()->UploadImmediate()` for the three procedural textures.
   - Keep failure visible through logs and `--expect-ibl-ready` smoke failure.
5. Preserve the current visual golden:
   - Update existing `ModelViewerSmoke` ctest command to pass `--no-ibl`.
   - Add a separate `ModelViewerIBLSmoke` ctest that runs with procedural IBL and `--expect-ibl-ready` but does not compare against the R7 golden.
6. Add source guardrails or focused tests as practical:
   - ModelViewer help text documents the new options.
   - `Tests/CMakeLists.txt` wires the new smoke test only when DX11 + ModelViewer are available.

## 4. Out of Scope

- SkyboxPass visual/background drawing.
- External HDRI loading in ModelViewer.
- HDRTextureLoader integration into ResourceManager default loaders.
- CPU or GPU environment convolution quality changes.
- Visual golden recapture.
- RenderSubsystem upload scheduling refactor.
- Changing RQ2b descriptor layout or DefaultLit shader math.

## 5. Expected Files

- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Docs/superpowers/specs/phase-log.md`
- Optional focused validation source if a small helper becomes testable without enlarging the sample API.

## 6. Required Tests

- Build `ModelViewer`.
- Existing `ModelViewerSmoke` and `VisualGoldenValidation` continue to pass with `--no-ibl`.
- New `ModelViewerIBLSmoke` passes with procedural IBL enabled and `--expect-ibl-ready`.
- Existing RQ2b/RQ2a focused tests remain green:
  - `PipelineCacheValidation`
  - `MaterialSystemValidation`
  - `GPUUploadServiceValidation`
  - `GPUResourceManagerValidation`
- Regression:
  - `RenderGraphValidation`
  - `RenderHonestyValidation`
  - `RenderSceneValidation`
  - `RenderPassValidation`
  - `ClusteredLightingValidation`

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ModelViewer PipelineCacheValidation MaterialSystemValidation GPUUploadServiceValidation GPUResourceManagerValidation RenderPassValidation RenderSceneValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|ModelViewerIBLSmoke|VisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|ClusteredLightingValidation"
git diff --check
```

## 8. Risks

- Procedural IBL can alter existing golden output if the old smoke test is not explicitly run with `--no-ibl`.
- Immediate upload can fail for malformed cubemap mip ordering. Mitigation: keep the generated prefiltered data in the same mip-major / face-major source order expected by `GPUResourceManager`, then validate with `ModelViewerIBLSmoke`.
- Hand-authored sample resources can leak or be destroyed too early. Mitigation: store them in `ResourceHandle`s owned by `SkyboxComponent`.
- A tiny procedural IBL is not physically accurate. This stage verifies the runtime connection, not final lighting quality.

## 9. Acceptance Criteria

- Default interactive ModelViewer can create and use procedural IBL resources.
- Existing zero-tolerance ModelViewer golden remains stable via `--no-ibl`.
- `ModelViewerIBLSmoke` proves the renderer reports texture IBL ready.
- Validation commands pass.
- Spark plan review and Spark code review have no blockers.

## 10. Spark Review

Plan review: PASS (`Confucius`, `gpt-5.3-codex-spark`)

- Non-blocking notes adopted for implementation:
  - Keep new CLI flags aligned with the existing parser pattern and non-conflicting.
  - Generate cubemap mip source data in `GPUResourceManager`'s expected mip-major / face-major CPU order.
  - Upload procedural IBL resources immediately before the first render frame.
  - Keep `ModelViewerIBLSmoke` as a readiness assertion only, with no image comparison.

Code review: PASS (`Confucius`, `gpt-5.3-codex-spark`)

- Verdict: PASS.
- Blockers: none.
- Non-blocking notes:
  - `--expect-ibl-ready` is intentionally smoke-only; in interactive mode it is harmless and does not assert.
  - `ModelViewerIBLSmoke` remains a readiness assertion and does not feed `VisualGoldenValidation`.

Commit message: `feat(samples): wire procedural IBL into model viewer`
