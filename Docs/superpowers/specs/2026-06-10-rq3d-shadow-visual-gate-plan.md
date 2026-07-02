# RQ3d - Directional Shadow Visual Gate Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ3c
Previous stage: RQ3c - Directional Shadow PCF Filter Foundation

## 1. Stage Decision

RQ3c made directional shadows use a bounded 3x3 PCF shader path, but the proof is still mostly shader-source guardrails and general ModelViewer smoke. RQ3d adds a deterministic GPU visual gate that renders an actual shadow-casting scene and compares it against a stored golden image. This makes future shadow work less blind and turns the PCF path into a visible, regression-tested contract.

This is a verification-and-framework stage, not a new shadow algorithm stage. It does not add CSM cascade selection, PCSS, EVSM, reverse-Z shadows, or shadow atlas work.

## 2. Current Engine Evidence

- `ModelViewer` already supports deterministic smoke mode, fixed frame count, screenshot capture, DX11 default backend, and `VisualGoldenValidation`.
- Existing `ModelViewerSmoke` uses `R7Triangle.gltf` with `--no-ibl` and a zero-tolerance golden, but it does not create a shadow-casting directional light or a receiver/caster composition.
- `SceneRenderer` enables shadow sampling only when the collected `RenderScene` contains a directional light with `castsShadow=true`.
- `ModelResource::Instantiate()` creates a `SceneEntity` tree with `StaticMeshComponent`; those primitives cast shadows by default.
- `LightComponent` can be created manually by ModelViewer and configured as directional with `SetCastsShadow(true)`.
- `SceneRenderer::GetPipelineCache()` exposes `PipelineCache::GetLastDirectionalShadowFrameBindingResult()`, so smoke mode can assert that shadow sampling was actually enabled instead of only comparing pixels.

## 3. Scope

1. Add a deterministic shadow fixture.
   - Add `Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf`.
   - The fixture should contain a simple receiver plane and a raised caster mesh using embedded buffers, stable materials, and no external assets.
   - Keep the fixture small and text-based so it can be reviewed.

2. Add ModelViewer shadow gate options.
   - Add `--shadow-test-scene` to create a deterministic directional light suitable for the fixture.
   - Add `--expect-shadow-ready` to fail smoke mode unless `PipelineCache::GetLastDirectionalShadowFrameBindingResult().shadowSamplingEnabled` is true on the final frame.
   - The expectation must also require `fallbackReason == DirectionalShadowFallbackReason::None` and log the fallback reason on failure.
   - In `--shadow-test-scene`, use fixed camera/light settings and optionally disable IBL in CTest to keep the golden stable.
   - Do not affect the existing default ModelViewer flow or `ModelViewerSmoke`.

3. Add a shadow visual CTest gate.
   - Add a `ModelViewerShadowSmoke` CTest that runs ModelViewer with:
     - `--smoke`
     - `--model Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf`
     - `--shadow-test-scene`
     - `--expect-shadow-ready`
     - DX11, fixed resolution, fixed frames, screenshot output.
   - Add a `ShadowVisualGoldenValidation` CTest that compares the shadow screenshot against a stored golden.
   - Make `ShadowVisualGoldenValidation` depend on `ModelViewerShadowSmoke`.
   - Use a small, explicit tolerance if required by the shadow image; prefer zero tolerance if the DX11 path is stable.

4. Store the first shadow golden.
   - Capture the DX11 shadow smoke output after implementation and store it as `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm`.
   - Commit the golden only after inspecting that it contains a visible model/receiver and the smoke log reports shadow sampling ready.
   - Inspection must confirm a visible caster, a visible receiver, and a clear shadow darkening/edge, not only a non-empty frame.

5. Tests and guardrails.
   - Existing `ModelViewerSmoke`, `VisualGoldenValidation`, and `ModelViewerIBLSmoke` must continue to pass.
   - `ModelViewer --help` text must include the new options.
   - Add or extend a lightweight validation/source guardrail if needed to ensure `--expect-shadow-ready` checks the actual PipelineCache shadow binding result.

## 4. Out of Scope

- PCSS, EVSM/VSM/MSM, contact shadows, screen-space shadows, temporal shadow denoising.
- CSM cascade selection/blending, stable texel snapping, atlas packing, texture arrays.
- Reverse-Z shadow sampling.
- Changing existing R7 golden, `ModelViewerSmoke`, or default DamagedHelmet behavior.
- Adding editor UI, runtime tweak panels, or asset pipeline changes.

## 5. Expected Files

- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf`
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

- `ModelViewerShadowSmoke` passes and writes a shadow screenshot.
- `ShadowVisualGoldenValidation` passes against `RQ3d_Shadow_DX11_320x180.ppm`.
- Existing `ModelViewerSmoke`, `VisualGoldenValidation`, and `ModelViewerIBLSmoke` keep passing.
- A manual ModelViewer shadow smoke run reports `shadowSamplingEnabled=true` and `fallbackReason=None` or equivalent log evidence for `--expect-shadow-ready`.
- `git diff --check` passes.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerShadowSmoke|ShadowVisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke"
build\win_x64_debug\Samples\ModelViewer\Debug\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\ShadowPlaneCaster.gltf --shadow-test-scene --expect-shadow-ready --backend dx11 --width 320 --height 180 --frames 8 --screenshot build\win_x64_debug\Tests\VisualArtifacts\Debug\ModelViewer\RQ3d_Shadow_DX11_320x180.ppm --no-ibl --validation
build\win_x64_debug\Tests\Debug\VisualGoldenValidation.exe --expected Tests\Golden\ModelViewer\RQ3d_Shadow_DX11_320x180.ppm --actual build\win_x64_debug\Tests\VisualArtifacts\Debug\ModelViewer\RQ3d_Shadow_DX11_320x180.ppm --diff build\win_x64_debug\Tests\VisualArtifacts\Debug\ModelViewer\RQ3d_Shadow_DX11_320x180.diff.ppm --tolerance 0.0 --max-different-pixels 0
git diff --check
```

If exact zero-tolerance is unstable during local verification, document the measured difference and set the smallest explicit tolerance/count needed for DX11 Debug only.

## 8. Risks

- A golden generated from a new fixture can accidentally pass while showing no real shadow if `--expect-shadow-ready` is missing or too weak. RQ3d must assert the actual PipelineCache shadow binding result.
- The fixture/camera/light might not produce a clear shadow on first attempt. Inspect the captured image before committing the golden.
- Fixture materials must remain opaque; transparent draw lists can intentionally disable shadow frame state in the current renderer.
- The current visual compare is single-backend DX11; this is acceptable for the gate baseline, but DX12/Vulkan visual gates remain future work.
- A shadow fixture that depends on IBL or skybox colors would be less stable; CTest should use `--no-ibl`.

## 9. Acceptance Criteria

- There is a committed, deterministic ModelViewer shadow fixture and DX11 shadow golden.
- CTest has a shadow smoke test and a shadow visual golden test.
- The shadow smoke fails if the renderer does not report directional shadow sampling ready.
- Existing ModelViewer/visual gates still pass unchanged.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ3d scope as the task list. Do not expand into CSM cascade selection, PCSS/EVSM, shadow atlas work, reverse-Z shadow support, or general ModelViewer redesign.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `test(render): add directional shadow visual gate`
