# R12 - Final ModelViewer Validation Report

Date: 2026-06-06

Execution note: terminal artifacts were produced at 2026-06-07 02:42 Asia/Shanghai.

Parent program: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`

Plan: `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-plan.md`

## 1. Stage Decision

R11 remains deferred.

Rationale:

- R11 is P2/L optional GPU-driven work.
- Its scope is compute culling, stream compaction, indirect draw, and advanced descriptor or bindless work.
- It is not required for the final render-first ModelViewer acceptance path.
- Starting R11 before R12 would expand the program beyond final acceptance.

R12 is the active stage and is complete when Release ModelViewer, Release CTest, the visual gate, and final Spark review pass.

## 2. Reproducible Target

- Build tree: `build/win_x64_debug`
- Configuration: `Release`
- Fixture: `Tests/Fixtures/ModelViewer/R7Triangle.gltf`
- Golden image: `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Manual screenshot: `build/win_x64_debug/Tests/VisualArtifacts/Release/ModelViewer/R12_DX11_320x180.ppm`
- Resolution: 320x180
- Frames: 8
- Visual backend: DirectX 11

Backend note:

- R12's parent plan prefers DX12 or Vulkan when reproducible.
- The current `ModelViewer` screenshot readback path explicitly requires DX11.
- DX11 is therefore the reproducible visual backend for this stage.
- DX12 and Vulkan coverage is still present through Release backend and cross-backend CTest validation.
- Follow-up: add DX12/Vulkan screenshot readback once RHI row-pitch/readback semantics are unified for those backends.

Host GPU record:

- `OrayIddDriver Device`, driver `17.1.58.818`
- `NVIDIA GeForce RTX 4070 Ti`, driver `32.0.15.9621`
- ModelViewer selected GPU: `NVIDIA GeForce RTX 4070 Ti`
- DX11 feature level: `0xB100`
- Reported VRAM: 11994 MB

## 3. R12 Gate Fixes

R12 validation exposed two small gate blockers. Both were fixed before final acceptance:

- Plan deviation note: the R12 plan expected no engine source changes. The source changes below were accepted as targeted gate fixes because they were discovered by R12 validation and directly affected final acceptance fidelity.
- `RenderPassValidation` no-effect post-process test crashed because it called `PostProcessStack::Execute()` outside the fixture logger lifecycle, and the no-effect path logs a warning. The test now uses `RenderPassValidationFixture`, sets a fake device on the empty graph, and passes invalid handles to verify the intended no-work early-out.
- DX11 default material texture upload emitted `CreateBuffer E_INVALIDARG` errors in ModelViewer because `Upload + CopySrc` buffers had no D3D11 bind flags but were created as `D3D11_USAGE_DYNAMIC`. DX11 now treats upload buffers with no bind flags as CPU staging upload buffers and maps them with `D3D11_MAP_WRITE`.

Regression coverage added:

- `DX11Validation.UploadCopySourceBufferMaps`

## 4. Validation Commands

Host record:

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterCompatibility
```

Release build:

```powershell
cmake --build build\win_x64_debug --config Release
cmake --build build\win_x64_debug --config Release --target DX11Validation ModelViewer VisualGoldenValidation ImageCompareValidation
```

Targeted checks:

```powershell
build\win_x64_debug\Tests\Release\DX11Validation.exe --gtest_filter=DX11Validation.UploadCopySourceBufferMaps
build\win_x64_debug\Tests\Debug\DX11Validation.exe --gtest_filter=DX11Validation.UploadCopySourceBufferMaps
build\win_x64_debug\Tests\Release\DX11Validation.exe
```

Release CTest:

```powershell
ctest --test-dir build\win_x64_debug -C Release --output-on-failure -O build\win_x64_debug\R12Artifacts\Logs\release-full-ctest.log
```

Focused Release render CTest:

```powershell
ctest --test-dir build\win_x64_debug -C Release --output-on-failure -O build\win_x64_debug\R12Artifacts\Logs\release-focused-render-ctest.log -R "DX11Validation|ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
```

Manual ModelViewer:

```powershell
build\win_x64_debug\Samples\ModelViewer\Release\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --backend dx11 --width 320 --height 180 --frames 8 --screenshot build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --validation
```

Manual golden comparison:

```powershell
build\win_x64_debug\Tests\Release\VisualGoldenValidation.exe --expected Tests\Golden\ModelViewer\R7_DX11_320x180.ppm --actual build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --diff build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.diff.ppm --tolerance 0.0 --max-different-pixels 0
```

Patch hygiene:

```powershell
git diff --check
```

## 5. Validation Results

- Release build: PASS
- `DX11Validation.UploadCopySourceBufferMaps`: PASS in Debug and Release
- Release `DX11Validation.exe`: PASS, 20/20
- Release full CTest: PASS, 434/434
- Focused Release render CTest: PASS, 238/238
- CTest `ModelViewerSmoke`: PASS
- CTest `VisualGoldenValidation`: PASS
- Manual `ModelViewer --smoke`: PASS
- Manual golden comparison: PASS
  - MSE: 0
  - PSNR: 100
  - Different pixels: 0
- Final manual ModelViewer log: no `[error]` lines
- `git diff --check`: PASS, with only Git CRLF warnings for Windows line endings

Artifacts:

- Full Release CTest log: `build/win_x64_debug/R12Artifacts/Logs/release-full-ctest.log`
- Focused Release render CTest log: `build/win_x64_debug/R12Artifacts/Logs/release-focused-render-ctest.log`
- Manual ModelViewer log: `build/win_x64_debug/R12Artifacts/Logs/manual-modelviewer-dx11.log`
- Manual golden comparison log: `build/win_x64_debug/R12Artifacts/Logs/manual-golden-compare.log`
- Manual screenshot: `build/win_x64_debug/Tests/VisualArtifacts/Release/ModelViewer/R12_DX11_320x180.ppm`

The manual diff artifact path was requested, but no diff file was emitted because there were zero differing pixels.

## 6. Render Path

The accepted ModelViewer path is:

1. `ModelViewer` parses smoke options and selects DX11, 320x180, 8 frames, validation enabled.
2. `ResourceSubsystem` and `ResourceManager` initialize.
3. `ModelLoader` loads `Tests/Fixtures/ModelViewer/R7Triangle.gltf` as a `ModelResource`.
4. The model is instantiated into the world as a `SceneEntity` with renderable mesh data.
5. `GPUResourceManager` uploads `R7TriangleMesh` vertex and index streams to GPU buffers.
6. `MaterialSystem` initializes material constants, default material textures, samplers, and material descriptor sets. The DX11 upload path now handles CPU upload CopySrc buffers without D3D11 errors.
7. `PipelineCache` compiles and creates the default material pipelines plus depth-only, ToneMapping, and Bloom pipelines.
8. `SceneRenderer::SetupView()` builds render state through the render proxy path introduced by R8, feeding component-derived render proxies into `RenderScene`.
9. `RenderGraph` builds the frame pass chain. The final smoke frame reports `4 passes (0 culled), 7 barriers`.
10. Scene color is rendered, post-process is applied through Bloom and ToneMapping resources, and the final result is written to the DX11 backbuffer.
11. ModelViewer smoke capture copies the backbuffer into a readback buffer and writes `R12_DX11_320x180.ppm`.
12. `VisualGoldenValidation` compares the manual screenshot against the R7 golden image with zero tolerance and zero differing pixels.

## 7. Follow-Ups

- R11 GPU-driven rendering remains deferred as optional advanced work.
- DX12/Vulkan final screenshot acceptance should be added after non-DX11 ModelViewer readback is implemented.
- The Skybox pass still reports unsupported in ModelViewer logs, which is honest and expected for the current render-first program.

## 8. Spark Review

- Plan review agent: Sagan
- Plan verdict: `PASS_WITH_NON_BLOCKING`
- Final review first pass: `BLOCKED` on procedural documentation gates only.
  - No code blocker was reported.
  - Required fixes: update this report's final-review status and append the R12 `phase-log.md` entry.
- Final incremental review: `PASS_WITH_NON_BLOCKING`.
  - Procedural blockers resolved by updating this report and appending the R12 phase-log entry.
  - Final status sync applied before commit.
