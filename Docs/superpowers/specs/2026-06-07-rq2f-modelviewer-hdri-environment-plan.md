# RQ2f - ModelViewer HDRI Environment Wiring Plan

Date: 2026-06-07
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2e, commit `840b14e feat(render): draw cubemap texture skybox`

## 1. Stage Decision

RQ2f connects the existing CPU `HDRTextureLoader::LoadIBL()` path to the ModelViewer sample. A user-provided equirectangular `.hdr` or `.exr` file should generate an environment cubemap, irradiance cubemap, prefiltered cubemap, and BRDF LUT, upload those textures, and bind them through `SkyboxComponent` so the RQ2e cubemap skybox and RQ2b DefaultLit texture IBL paths are both exercised.

This is a wiring and verification stage on top of the current engine. It does not introduce GPU convolution, does not make equirectangular images directly drawable by `SkyboxPass`, and does not replace the existing procedural fallback used when no HDRI is requested.

## 2. Current State Evidence

- `HDRTextureLoader::LoadIBL()` already loads `.hdr` / `.exr`, converts equirectangular data to an environment cubemap, generates irradiance, prefiltered mips, and a BRDF LUT, and returns `IBLData`.
- The generated cubemap metadata is `RGBA32F`, `isCubemap = true`, and mip-major / face-major data is compatible with the RQ2a upload path.
- RQ2b binds `SkyboxComponent` irradiance, prefiltered, and BRDF LUT resources to DefaultLit when all three textures are GPU-ready.
- RQ2e lets `SkyboxType::Cubemap` draw a GPU-ready cubemap background.
- ModelViewer currently creates only tiny procedural IBL resources by default and has no CLI path for a real HDRI environment.
- `ResourceManager` does not register `HDRTextureLoader` as the default `ResourceType::Texture` loader, so ModelViewer should use a direct `HDRTextureLoader` instance for this specialized environment path.

## 3. Scope

1. Extend ModelViewer CLI and options:
   - Add `--hdri <path>` for an explicit HDRI environment file.
   - Add `--expect-skybox-ready` for smoke tests that require `SkyboxPass` to report supported on the final frame.
   - Implement `--expect-skybox-ready` by checking `SceneRenderer::GetPassChainStats().passStatuses` for the `SkyboxPass` status and reporting its `unsupportedReason` on failure.
   - Keep `--expect-ibl-ready` as the DefaultLit texture IBL readiness assertion.
   - Treat `--hdri` plus `--no-ibl` as an invalid option combination, because the user explicitly requested an environment but disabled environment wiring.
2. Add ModelViewer HDRI resource wiring:
   - Add a small `HDRIEnvironmentResources` holder for environment cubemap, irradiance, prefiltered, BRDF LUT, and prefiltered mip count.
   - Load `HDRTextureLoader::LoadIBL()` with ModelViewer-specific options.
   - Use the real option fields `cubemapResolution`, `irradianceResolution`, `prefilteredResolution`, `prefilteredMipLevels`, `brdfLUTResolution`, and `convolutionSamples`.
   - Wrap returned `TextureResource*` values in `ResourceHandle<TextureResource>` so lifetime is held by the sample and the `SkyboxComponent`.
   - Upload all four textures immediately through `GPUResourceManager::UploadImmediate()`.
   - Log and fail honestly if a requested HDRI cannot be loaded, generated, uploaded, or made GPU-ready.
3. Configure `SkyboxComponent` from HDRI:
   - If `--hdri` is present, create a `SkyboxComponent` with `SkyboxType::Cubemap`.
   - Set the environment cubemap through `SetCubemap()`.
   - Set irradiance, prefiltered, and BRDF LUT resources for material IBL.
   - Set exposure and `SetContributesToLighting(true)`.
   - Do not create the procedural IBL component when HDRI mode is active.
   - If `--hdri` is absent and IBL is enabled, preserve the existing procedural IBL path.
4. Keep smoke costs bounded:
   - Use low HDRI generation settings in smoke mode, for example cubemap 8, irradiance 4, prefiltered 8, 3 mips, BRDF LUT 8, and a small sample count.
   - Use modest preview settings for interactive mode; final high-quality environment convolution tuning remains a later stage.
5. Add deterministic HDRI smoke validation:
   - Add a tiny test fixture writer target that creates a valid Radiance RGBE `.hdr` file in the build directory.
   - Name the fixture ctest `ModelViewerHDRIFixture`.
   - Add `ModelViewerHDRISmoke` ctest that depends on `ModelViewerHDRIFixture` and runs ModelViewer with `--hdri`, `--expect-ibl-ready`, and `--expect-skybox-ready`.
   - Keep `ModelViewerSmoke --no-ibl` and `VisualGoldenValidation` unchanged so the zero-tolerance golden remains stable.

## 4. Out of Scope

- GPU-side environment convolution or compute prefilter passes.
- Direct equirectangular draw in `SkyboxPass`; HDRI files are converted to cubemap by `HDRTextureLoader`.
- Adding or bundling a production HDRI asset.
- Making HDRI the default ModelViewer environment without `--hdri`.
- ACES/AgX tonemap changes, exposure automation, bloom tuning, or material BRDF changes.
- ECS/Object/RenderProxy refactors.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-07-rq2f-modelviewer-hdri-environment-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/ModelViewerHDRIFixtureWriter/main.cpp`

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target ModelViewer ModelViewerHDRIFixtureWriter`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerHDRIFixture|ModelViewerHDRISmoke|ModelViewerIBLSmoke"`
- Visual stability:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation"`
- Regression:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|PipelineCacheValidation"`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- `ModelViewer --hdri <path>` loads and generates HDRI environment resources through `HDRTextureLoader::LoadIBL()`.
- The generated environment cubemap is uploaded and assigned to `SkyboxComponent::SetCubemap()`.
- The generated irradiance, prefiltered, and BRDF LUT resources are uploaded and assigned to the same `SkyboxComponent`.
- `--expect-ibl-ready` fails if DefaultLit texture IBL did not become ready.
- `--expect-skybox-ready` fails if `SkyboxPass` did not become supported.
- `--hdri` plus `--no-ibl` is rejected rather than silently falling back.
- Existing procedural IBL behavior remains available when `--hdri` is absent.
- `ModelViewerSmoke --no-ibl` and `VisualGoldenValidation` remain unchanged.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only the RQ2f scope above as the task list. Do not expand into GPU convolution, bundled HDRI assets, direct equirectangular skybox drawing, or tonemapping/BRDF quality changes during this stage.

## 9. Spark Review

- Plan review: pending (`gpt-5.5`, xhigh)
- Code review: pending (`gpt-5.5`, xhigh)
- Commit message: `feat(samples): load HDRI environments in model viewer`
