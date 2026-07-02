# R7 - Visual Gate Baseline Implementation Plan

Date: 2026-06-06

## Source Of Truth

- Primary plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `13. R7 - Visual Gate Baseline`
- Required outputs: fixed glTF fixture, fixed camera/time/frame count/resolution/backend/render options, screenshot output path, golden image storage, image compare threshold, failure artifacts, `VisualGoldenValidation`, and `ModelViewerSmoke`.
- Previous stage: R6a completed and committed in `c5985c2`; phase-log correction committed in `a5446ba`.

## Current Code Findings

- `Samples/ModelViewer/main.cpp` is interactive-only today: it accepts a positional model path, uses auto backend, enables auto render, and loops until the window closes.
- The sample has no fixed-frame smoke mode, no explicit backend/resolution CLI, and no screenshot/capture output.
- `Tests/Common/ImageCompare.cpp` counts a changed pixel only when the first byte of the pixel differs. A change in G/B/A can be missed by `differentPixels`.
- `Tests/CMakeLists.txt` has no `VisualGoldenValidation` target and no `ModelViewerSmoke` CTest entry.
- No repository-owned glTF/glb fixture currently exists. The sample still searches a user desktop path, which is not a valid gate fixture.
- RHI has readback buffers and `CopyTextureToBuffer`, but cross-backend row-pitch semantics are not clean enough for a multi-backend visual gate in this phase. R7 will therefore establish the first reproducible local visual gate on DX11.

## Approved Scope

1. Add a repository-owned fixed ModelViewer fixture:
   - `Tests/Fixtures/ModelViewer/R7Triangle.gltf`
   - Minimal embedded-buffer triangle with deterministic mesh/material/node data.

2. Extend ModelViewer with deterministic smoke/capture mode:
   - Add CLI options: `--smoke`, `--model`, `--frames`, `--width`, `--height`, `--backend`, `--screenshot`, `--validation`, `--no-validation`, `--help`.
   - In smoke mode, default to DX11, fixed resolution, fixed frame count, validation enabled, vsync disabled, non-resizable window, and `autoRender=false`.
   - In smoke mode, fix the camera to position `(0, 1.5, 4)`, target `(0, 0, 0)`, FOV 45 degrees, and a static scene with explicit `1.0 / 60.0` tick delta. No animation or interactive input is allowed to affect the captured frame.
   - Use `Engine::TickWithoutRender(delta)` plus explicit `BeginFrame -> Render -> queue readback -> EndFrame -> WaitIdle -> write PPM -> Present` to keep the frame loop bounded and capture the final frame before present.
   - Queue the DX11 backbuffer readback before `EndFrame`, wait for completion after submit, then write a binary PPM artifact before `Present`.
   - Fail loudly if `--screenshot` is requested on a non-DX11 backend until RHI row-pitch semantics are unified.
   - Prerequisite: the local R7 gate environment must have the DX11 backend enabled and be able to create a visible window. If not, record the phase as blocked instead of silently falling back.

3. Harden image comparison utilities:
   - Fix `CompareImages()` so any channel exceeding tolerance marks the pixel as different.
   - Preserve MSE/PSNR behavior.
   - Add an `ImageCompareValidation` gtest target covering channel-level differences, tolerance, and size mismatch behavior.

4. Add visual artifact utilities and validation target:
   - Add PPM load/save/diff helpers under `Tests/Common`.
   - Add `Tests/VisualGoldenValidation/main.cpp`.
   - `VisualGoldenValidation` compares expected and actual PPM files with a threshold, writes a diff PPM on failure, and reports paths clearly.

5. Wire CMake/CTest:
   - Build `VisualGoldenValidation`.
   - Add a `ModelViewerSmoke` CTest entry that runs `ModelViewer --smoke` against the R7 fixture and writes an actual screenshot artifact into the build tree.
   - Add a `VisualGoldenValidation` CTest entry that depends on `ModelViewerSmoke` with `set_property(TEST VisualGoldenValidation PROPERTY DEPENDS ModelViewerSmoke)` and compares the actual screenshot with the stored golden.

6. Store the first golden:
   - Run `ModelViewerSmoke` locally with DX11.
   - Store the generated baseline as `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`.
   - Keep failure artifacts in the build tree, not the source tree.

7. Update phase-log and commit R7 only after:
   - Spark plan review passes.
   - Implementation builds.
   - `ModelViewerSmoke` passes.
   - `VisualGoldenValidation` passes.
   - Relevant render regression tests still pass.
   - Spark code review passes.

## Out Of Scope

- No RenderProxy work. R8 remains the next independent phase.
- No ECS/Object refactor.
- No DX12/Vulkan golden capture in R7. That needs a separate RHI row-pitch/copy-readback contract cleanup.
- No PNG dependency. PPM is sufficient for deterministic artifact storage and diffing.
- No renderer algorithm changes beyond bounded capture plumbing in the sample.

## Validation Commands

```powershell
$BuildDir = "build/win_x64_debug"
cmake --build $BuildDir --config Debug --target ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir $BuildDir -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
cmake --build $BuildDir --config Debug --target RenderGraphValidation RenderHonestyValidation RenderSceneValidation RenderPassValidation MaterialSystemValidation
ctest --test-dir $BuildDir -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation"
git diff --check
```

## Done Criteria

- `ModelViewerSmoke` creates a deterministic actual screenshot artifact from the fixed glTF fixture.
- `VisualGoldenValidation` compares actual against stored golden with a declared threshold and writes a diff artifact on failure.
- The R7 fixture, golden, validation target, and sample CLI are committed.
- `phase-log.md` records plan review, code review, validations, artifacts, and commit hash.
