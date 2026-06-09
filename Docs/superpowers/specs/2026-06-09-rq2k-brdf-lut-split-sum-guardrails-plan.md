# RQ2k - BRDF LUT Split-Sum Numeric Guardrails Plan

Date: 2026-06-09
Parent program: Render Quality & Verification Program v2
Previous stage: RQ2j, commit `19a3f72 feat(render): expose view lighting controls for default lit`

## 1. Stage Decision

Improve the CPU-generated BRDF integration LUT used by texture IBL so it follows the standard split-sum GGX integration instead of the current simplified visibility term, and add numeric tests that prove the LUT content, not only metadata.

This is the next highest-value render-quality step because RQ2i/RQ2j made texture IBL visible and honest. If the BRDF LUT is numerically weak, metallic response and roughness-dependent reflections remain suspect even when cubemap upload, material binding, and skybox wiring all work.

## 2. Current State Evidence

- `Resource/Private/Loader/HDRTextureLoader.cpp::GenerateBRDFLUT()` currently computes `G = NdotL * NdotV`, then derives `G_Vis` from that simplified term.
- The usual split-sum LUT integrates the Smith GGX visibility term and uses `G_Vis = (G * VdotH) / (NdotH * NdotV)`.
- `Tests/HDRTextureLoaderValidation/main.cpp::BRDFLUTClampsZeroSamplesAndKeepsMetadata` only verifies texture format, size, sRGB flag, and non-empty data.
- RQ2i plan/code review explicitly noted that the procedural IBL quality gate proves metadata, fixed identity, preset shape, and GPU readiness, but not numerical IBL content quality.
- RQ2j removed hidden ambient floor from texture IBL-ready frames, so BRDF LUT quality now matters more directly to visible output.

## 3. Scope

1. Update `HDRTextureLoader::GenerateBRDFLUT()`:
   - Replace the simplified geometry term with Smith GGX split-sum visibility integration.
   - Keep zero sample count clamped to one sample.
   - Keep output metadata as RGBA16F, non-sRGB, `TextureUsage::Data`, one mip.
   - Keep existing cache/id/name/path behavior.
2. Harden half-float packing:
   - Move the local float-to-half conversion into a small helper.
   - Clamp/sanitize non-finite and negative BRDF channels to finite non-negative values before packing.
   - Preserve alpha as 1.0 and unused blue as 0.0.
3. Add numeric validation:
   - Add a test-side half-float decoder.
   - Add a reference split-sum integration helper in the test file.
   - Compare several BRDF LUT pixels against the reference within half-float tolerant bounds, including grazing/low-roughness samples that strongly separate Smith GGX from the old `G = NdotL * NdotV` term.
   - Keep explicit pre-fix failure evidence for the old simplified implementation.
   - Assert BRDF channels are finite and non-negative, alpha is near 1, and blue is near 0.
   - Keep the existing metadata/zero-sample clamp test.
4. Run visual safety gates:
   - `ModelViewerIBLSmoke` should still pass with the default procedural IBL.
   - `ModelViewerSmoke` and `VisualGoldenValidation` should remain unchanged because no-IBL golden does not use texture IBL.
   - Manual DamagedHelmet smoke should still render.

## 4. Out of Scope

- Changing `DefaultLit.hlsl` BRDF equations or texture IBL sampling.
- Changing direct light, ambient floor, tone mapping, bloom, exposure, shadows, SSAO, SSR, TAA, or color grading.
- GPU-side IBL convolution or compute prefiltering.
- Changing ModelViewer procedural sky, presets, camera, default model, or golden output.
- Recapturing visual golden.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-09-rq2k-brdf-lut-split-sum-guardrails-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Resource/Private/Loader/HDRTextureLoader.cpp`
- `Tests/HDRTextureLoaderValidation/main.cpp`

## 6. Required Tests

- Build:
  - `cmake --build build\win_x64_debug --config Debug --target HDRTextureLoaderValidation ModelViewer VisualGoldenValidation`
- Focused:
  - `ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "HDRTextureLoaderValidation|ModelViewerIBLSmoke|ModelViewerSmoke|VisualGoldenValidation"`
- Manual visual check:
  - `build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --backend dx11 --width 800 --height 450 --frames 8 --screenshot build\win_x64_debug\VisualArtifacts\Debug\ModelViewer\RQ2k_DamagedHelmet.ppm --validation --expect-ibl-ready --expect-procedural-ibl-quality`
- Hygiene:
  - `git diff --check`

## 7. Acceptance Criteria

- `GenerateBRDFLUT()` uses Smith GGX split-sum visibility integration instead of the simplified `NdotL * NdotV` visibility term.
- BRDF LUT numeric tests fail on the old simplified implementation and pass on the updated implementation.
- BRDF LUT generated data decodes to finite non-negative RG channels, blue near 0, alpha near 1.
- Existing BRDF LUT metadata and zero-sample behavior remain intact.
- ModelViewer IBL smoke and no-IBL visual golden remain green.
- DamagedHelmet still renders in a manual smoke screenshot.
- Spark plan review (`gpt-5.5`, xhigh) passes before implementation.
- Spark code review (`gpt-5.5`, xhigh) passes before commit.
- Commit is created after the stage completes.

## 8. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ2k scope as the task list. Do not expand into shader BRDF edits, shadowing, tone mapping, exposure, bloom, or GPU IBL convolution.

## 9. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh)
- Code review: PASS (`gpt-5.5`, xhigh)
- Commit message: `fix(resource): use split-sum geometry for brdf lut`
