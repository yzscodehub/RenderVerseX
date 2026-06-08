# RQ2i - ModelViewer CPU IBL Quality Baseline Plan

Date: 2026-06-08
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2h, commit `fa1f923 fix(render): make texture uploads idempotent and import gltf pbr color space`

## 1. Stage Decision

RQ2i improves the default ModelViewer lighting baseline by replacing the tiny hand-authored procedural IBL textures with CPU-generated IBL resources built through the existing `HDRTextureLoader` algorithms.

The current default ModelViewer path proves that texture IBL is wired, but the procedural resources are intentionally tiny and synthetic: 2x2 irradiance, 4x4 prefiltered environment with 3 mips, and a 4x4 hand-written BRDF approximation. That makes DamagedHelmet render, but it is not a good quality baseline for later PBR work.

This stage keeps the same user-facing default behavior: if no `--hdri` is supplied and `--no-ibl` is not used, ModelViewer creates a procedural skybox/IBL environment. The difference is that the IBL textures are generated from a small HDR procedural equirectangular environment using the same CPU IBL path used by explicit HDRI loading.

## 2. Current State Evidence

- `Samples/ModelViewer/main.cpp` creates default procedural IBL resources with `CreateProceduralCubemap()` and `CreateProceduralBRDFLUT()`.
- Those resources are RGBA8 and very low resolution, so they exercise descriptor binding but do not meaningfully approximate a real environment.
- `Resource::HDRTextureLoader` already exposes public CPU APIs for `EquirectangularToCubemap()`, `GenerateIrradianceMap()`, `GeneratePrefilteredMap()`, and `GenerateBRDFLUT()`.
- `ModelViewerHDRISmoke` already proves the explicit HDRI path can upload and bind CPU-generated cubemap, irradiance, prefiltered, and BRDF LUT resources.
- `ModelViewerIBLSmoke` already exercises the default procedural IBL path with `--expect-ibl-ready`.
- `ModelViewerSmoke --no-ibl` is the zero-tolerance visual golden path and must remain unchanged.

## 3. Scope

1. Replace the default procedural IBL data source:
   - Generate a small procedural HDR equirectangular environment in memory.
   - Convert it to a cubemap using `HDRTextureLoader::EquirectangularToCubemap()`.
   - Generate diffuse irradiance using `HDRTextureLoader::GenerateIrradianceMap()`.
   - Generate prefiltered specular mips using `HDRTextureLoader::GeneratePrefilteredMap()`.
   - Generate BRDF LUT using `HDRTextureLoader::GenerateBRDFLUT()`.
2. Preserve ModelViewer behavior and controls:
   - Keep `--hdri` as the explicit real environment path.
   - Keep `--no-ibl` disabling all default IBL resources and preserving the golden.
   - Keep the procedural skybox draw type and existing sky colors for default runs.
   - Keep `--expect-ibl-ready` as the readiness assertion.
   - Add `--expect-procedural-ibl-quality` as a smoke-only metadata assertion for the default procedural IBL path.
3. Add bounded quality presets:
   - Smoke mode uses small CPU IBL settings so CTest stays fast: equirectangular 16x8, environment cubemap 8, irradiance 4, prefiltered 8, 4 prefiltered mips, BRDF LUT 16, and 16 convolution samples.
   - Interactive mode uses modestly higher settings for better preview quality without making startup expensive: equirectangular 64x32, environment cubemap 32, irradiance 16, prefiltered 32, 5 prefiltered mips, BRDF LUT 64, and 64 convolution samples.
4. Pack generated cubemaps into `TextureResource` objects:
   - Use RGBA32F, `isCubemap = true`, `isSRGB = false`, and `TextureUsage::Color`.
   - Preserve the mip-major/face-major layout expected by `GPUResourceManager::PrepareTextureUpload()`.
   - Keep stable procedural resource ids.
   - Use fixed ids/names/paths:
     - Irradiance: `0x4D5649424C495201`, `ModelViewerProceduralIrradiance`, `__modelviewer_procedural_irradiance__`.
     - Prefiltered: `0x4D5649424C505201`, `ModelViewerProceduralPrefiltered`, `__modelviewer_procedural_prefiltered__`.
     - BRDF LUT: `0x4D56494252444601`, `ModelViewerProceduralBRDFLUT`, `__modelviewer_procedural_brdf_lut__`.
   - Construct the `HDRTextureLoader` used for default procedural IBL with no resource cache/manager ownership, then explicitly set fixed ModelViewer ids, names, and paths on irradiance, prefiltered, and BRDF LUT resources after generation/wrapping.
5. Add regression metadata validation:
   - `--expect-procedural-ibl-quality` fails unless irradiance and prefiltered resources are RGBA32F cubemaps, BRDF LUT is generated as RGBA16F, expected smoke/interactive resolutions and mip counts are used, resource ids match fixed ModelViewer ids, and the resources are GPU-ready when readiness is expected.
   - Update `ModelViewerIBLSmoke` to pass `--expect-procedural-ibl-quality`, so the current RGBA8/4x4 hand-authored implementation would fail the new gate.
6. Add observable logging:
   - Log the generated procedural IBL dimensions, mip count, sample count, and readiness.
7. Keep IBL/background coherent:
   - Define the procedural HDR equirectangular generator from the same sky colors and sun direction family used by the procedural skybox.

## 4. Out of Scope

- Changing `DefaultLit.hlsl` BRDF, direct-light intensity, ambient floor, or IBL equations.
- Shadow maps, SSAO, SSR, bloom, tonemap, exposure, color grading, or post-process changes.
- Making an external HDRI asset the default.
- Bundling production HDRI assets.
- GPU-side IBL convolution or compute prefiltering.
- Changing `ModelViewerSmoke --no-ibl` golden output.
- Texture identity, residency, or glTF color-space work already completed in RQ2h.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-08-rq2i-modelviewer-cpu-ibl-quality-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target ModelViewer HDRTextureLoaderValidation GPUResourceManagerValidation`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerIBLSmoke|ModelViewerHDRISmoke|HDRTextureLoaderValidation|GPUResourceManagerValidation"`
- Visual stability:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation"`
- Manual visual check:
  - `build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ2i_DamagedHelmet.ppm --validation --expect-ibl-ready --expect-procedural-ibl-quality`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- Default ModelViewer procedural IBL resources are generated through `HDRTextureLoader` CPU IBL APIs.
- Procedural irradiance and prefiltered textures are RGBA32F cubemaps with GPU-ready resources.
- Procedural BRDF LUT is generated by `HDRTextureLoader::GenerateBRDFLUT()` instead of the hand-authored 4x4 approximation.
- Procedural irradiance, prefiltered, and BRDF LUT resources use fixed ModelViewer ids, names, and paths after generation.
- `--expect-procedural-ibl-quality` fails on the old RGBA8/hand-authored procedural IBL path and passes on the new CPU-generated path.
- `ModelViewerIBLSmoke` still passes and proves the default procedural IBL path is ready.
- `ModelViewerSmoke --no-ibl` and `VisualGoldenValidation` remain unchanged.
- DamagedHelmet still renders in a manual smoke screenshot.
- Required validation passes.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only the RQ2i scope above as the task list. Do not expand into shader BRDF edits, shadowing, tone mapping, or real HDRI default asset selection during this stage.

## 9. Spark Review

- Plan review: PASS after one BLOCKED revision (`gpt-5.5`, xhigh)
- Code review: PASS (`gpt-5.5`, xhigh)
- Commit message: `feat(samples): generate procedural model viewer ibl with hdr pipeline`
