# RQ2g - HDRTextureLoader CPU IBL Sampling Controls Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2f, commit `5911b42 feat(samples): load HDRI environments in model viewer`

## 1. Stage Decision

RQ2g improves the existing CPU-side `HDRTextureLoader` IBL generation path so its quality/cost controls are honest and test-covered. `HDRLoadOptions::convolutionSamples` should actually bound diffuse irradiance and prefiltered environment sampling, and generated IBL data should avoid NaN/Inf output for tiny smoke fixtures or low sample counts.

This stage is intentionally narrow. It does not add GPU convolution, does not change renderer descriptor layouts, does not change ModelViewer default behavior, and does not alter tonemapping or material BRDF math.

## 2. Current State Evidence

- `HDRTextureLoader::LoadIBL()` passes `options.convolutionSamples` to `GenerateIrradianceMap()`, `GeneratePrefilteredMap()`, and `GenerateBRDFLUT()`.
- `GenerateIrradianceMap()` ignores its `numSamples` argument and instead uses a fixed angular step of `0.025`, which makes the option dishonest and can make ModelViewer HDRI wiring unexpectedly expensive.
- `GeneratePrefilteredMap()` divides by `totalWeight` without guarding the zero-weight case.
- `GeneratePrefilteredMap()` computes roughness as `mip / (numMipLevels - 1)`, which is unsafe when the caller requests one mip.
- RQ2f added a deterministic tiny HDRI smoke path, but there is no CPU unit test directly validating HDR loader sampling controls or finite output.

## 3. Scope

1. Make diffuse irradiance sampling count-controlled:
   - Replace the fixed nested `sampleDelta` sweep with a deterministic sample loop driven by `numSamples`.
   - Clamp `numSamples` to at least 1.
   - Use an existing deterministic sequence such as `Hammersley()` and cosine-weighted hemisphere sampling around the cubemap texel normal.
   - Preserve the existing cubemap sampling helper behavior and output format.
2. Harden prefiltered map generation:
   - Clamp `numSamples` to at least 1.
   - Handle `numMipLevels <= 1` without division by zero.
   - Guard `totalWeight <= 0` by falling back to a direct environment sample or another finite deterministic value.
   - Ensure all output channels are finite.
3. Harden BRDF LUT generation enough for low sample counts:
   - Clamp `numSamples` to at least 1.
   - Keep the existing LUT format and resource semantics.
4. Add CPU validation coverage:
   - Add `HDRTextureLoaderValidation`.
   - Test that `GenerateIrradianceMap()` respects sampling controls with a non-uniform cubemap fixture:
     - `numSamples = 0` clamps to the same finite output as `numSamples = 1`.
     - `numSamples = 1` and `numSamples = 8` produce observably different output on at least one texel/channel, proving the sample count is not ignored.
   - Test that `GeneratePrefilteredMap()` returns the requested mip count, handles `numMipLevels = 1`, and produces finite output for low sample counts.
   - Test that `GeneratePrefilteredMap()` clamps `numSamples = 0` to finite output.
   - Test that `GenerateBRDFLUT()` returns a loaded texture with expected metadata for `numSamples = 0` and `numSamples = 1`.
   - Keep tests tiny and CPU-only; no RHI backend is required.
5. Keep RQ2f sample validation green:
   - `ModelViewerHDRISmoke` remains a smoke/ready test and should benefit from lower bounded CPU cost.
   - If the smoke exposes that `UploadImmediate()` returns before staged texture uploads become `GPUReady`, fix the blocking upload completion contract in the upload infrastructure and cover it with existing upload validation tests. Do not weaken the smoke readiness assertions.

## 4. Out of Scope

- GPU IBL convolution or compute prefiltering.
- Physically complete irradiance/prefilter integration.
- Changing DefaultLit texture IBL descriptor bindings or shader math.
- Changing `SkyboxPass` or `SceneRenderer` wiring.
- Adding production HDRI assets or visual golden recapture.
- Tonemap, exposure, bloom, or color grading changes.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-07-rq2g-hdrtextureloader-cpu-ibl-sampling-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Resource/Private/Loader/HDRTextureLoader.cpp`
- `Render/Private/GPUUploadService.cpp` only if required to preserve `UploadImmediate()` blocking semantics exposed by RQ2f smoke validation.
- `Tests/CMakeLists.txt`
- `Tests/HDRTextureLoaderValidation/main.cpp`

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target HDRTextureLoaderValidation ModelViewer`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "HDRTextureLoaderValidation|ModelViewerHDRISmoke|ModelViewerIBLSmoke"`
- Visual stability:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation"`
- Regression:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "GPUUploadServiceValidation|GPUResourceManagerValidation|MaterialSystemValidation|RenderSceneValidation|RenderPassValidation"`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- `HDRLoadOptions::convolutionSamples` is no longer ignored by diffuse irradiance generation.
- Irradiance, prefiltered environment, and BRDF LUT generation clamp zero/low sample counts into finite deterministic output.
- A non-uniform cubemap test proves irradiance output differs between `numSamples = 1` and a higher sample count.
- Prefiltered generation handles one mip without division by zero.
- `HDRTextureLoaderValidation` proves finite output and metadata for tiny CPU-only fixtures.
- RQ2f `ModelViewerHDRISmoke` and the existing visual golden remain green.
- `UploadImmediate()` does not leave staged texture uploads in a non-ready pending state after its blocking wait path.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only the RQ2g scope above as the task list. Do not expand into GPU convolution, renderer wiring, shader layout changes, production HDRI assets, or tonemapping/color quality work during this stage.

## 9. Spark Review

- Plan review: pending (`gpt-5.5`, xhigh)
- Code review: pending (`gpt-5.5`, xhigh)
- Commit message: `fix(resource): honor HDR IBL sampling controls`
