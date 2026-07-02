# R12 - Final ModelViewer Validation Plan

Date: 2026-06-06

Parent program: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`

## 1. Stage Source

Current stage:

- R11, lines 542-564: `GPU-Driven Optional Advanced`, priority P2, size L.
- R12, lines 566-585: `Final ModelViewer Validation`, priority P0, size S.

Decision:

- Do not start R11 in this render-first acceptance pass.
- R11 is optional advanced work for GPU-driven rendering after the production path is already green. It includes compute culling, stream compaction, indirect draw, and advanced descriptor or bindless work. Starting it now would expand the program beyond final acceptance.
- Proceed with R12 as the active stage.

## 2. Goal

Validate the completed render-first path with a Release ModelViewer run against a fixed glTF fixture, record reproducible backend and artifact details, and document the render path from asset load to final image.

## 3. Approved Scope

- Build Release `ModelViewer` and the Release validation targets needed by the final visual gate.
- Run the relevant Release CTest suite, including `ModelViewerSmoke`, `VisualGoldenValidation`, and render validation tests.
- Run a manual Release `ModelViewer --smoke` command with the fixed fixture.
- Use the existing deterministic fixture:
  - `Tests/Fixtures/ModelViewer/R7Triangle.gltf`
- Use the existing deterministic golden:
  - `Tests/Golden/ModelViewer/R7_DX11_320x180.ppm`
- Record:
  - backend
  - GPU and driver version
  - resolution
  - fixture
  - commands
  - generated screenshot and diff artifact paths
- Add a final R12 validation report documenting the runtime path:
  - glTF asset
  - resource import/instantiation
  - GPU upload
  - material binding
  - pipeline/cache path
  - render proxy path
  - RenderGraph/pass chain
  - final image and golden comparison
- Update `phase-log.md` after Spark final review.
- Commit R12 documentation and validation records.
- If Release validation exposes a gate blocker that directly affects final acceptance fidelity, a targeted RHI or test-harness fix is allowed and must be recorded in the R12 report and phase log.

## 4. Out of Scope

- Starting R11 GPU-driven work.
- Implementing GPU frustum culling, stream compaction, indirect draw, or bindless parity.
- New feature work in RHI, RenderGraph, materials, passes, particles, ECS, or editor systems.
- Changing the existing R7 fixture or golden baseline unless validation proves the current baseline is invalid.
- Reworking ModelViewer screenshot readback. The current smoke screenshot path requires DX11, so DX11 is the reproducible visual backend for this stage.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-plan.md`
- `Docs/superpowers/specs/2026-06-06-r12-final-modelviewer-validation-report.md`
- `Docs/superpowers/specs/phase-log.md`

No engine source files are expected to change in R12.

## 6. Risks

- Release targets may not all be built yet in the local multi-config build tree.
- Full Release CTest may expose unrelated non-render tests. If that happens, the failure must be recorded and triaged; R12 cannot pass unless the final ModelViewer visual gate and relevant render validation suite pass.
- The plan requests DX11 for the reproducible visual gate because `ModelViewer` screenshot capture currently rejects non-DX11 backends.
- Driver details may need to be collected from the host OS rather than from ModelViewer logs.

## 7. Build and Validation Commands

Local build tree:

```powershell
$BuildDir = "build/win_x64_debug"
```

Host GPU and driver record:

```powershell
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterCompatibility
```

Release build:

```powershell
cmake --build build/win_x64_debug --config Release
```

Full Release CTest:

```powershell
ctest --test-dir build/win_x64_debug -C Release --output-on-failure
```

Focused render acceptance CTest if full CTest needs triage, and always as an explicit final render record:

```powershell
ctest --test-dir build/win_x64_debug -C Release --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
```

Manual ModelViewer acceptance:

```powershell
build\win_x64_debug\Samples\ModelViewer\Release\ModelViewer.exe --smoke --model Tests\Fixtures\ModelViewer\R7Triangle.gltf --backend dx11 --width 320 --height 180 --frames 8 --screenshot build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --validation
```

Manual golden comparison for the R12 screenshot:

```powershell
build\win_x64_debug\Tests\Release\VisualGoldenValidation.exe --expected Tests\Golden\ModelViewer\R7_DX11_320x180.ppm --actual build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.ppm --diff build\win_x64_debug\Tests\VisualArtifacts\Release\ModelViewer\R12_DX11_320x180.diff.ppm --tolerance 0.0 --max-different-pixels 0
```

Patch hygiene:

```powershell
git diff --check
```

## 8. Visual Gate Requirement

R12 is the final visual acceptance stage. It must pass:

- `ModelViewerSmoke`
- `VisualGoldenValidation`
- Manual Release `ModelViewer --smoke`
- Manual golden comparison for the R12 screenshot

The recorded visual backend is DX11 because current smoke capture readback is DX11-only.

## 9. Acceptance Criteria

- R11 deferral is explicitly recorded.
- Release `ModelViewer` builds.
- Release visual gate passes using the fixed glTF fixture and golden image.
- Relevant Release render validation passes.
- Full Release CTest result is recorded.
- Final report records backend, GPU/driver, resolution, fixture, commands, and artifacts.
- Final report documents the render path from glTF asset to final image.
- Spark plan review and final review have no blockers.
- `phase-log.md` is updated.
- R12 is committed as its own stage.

## 10. Spark Review Placeholders

- Spark plan review: `PASS_WITH_NON_BLOCKING`.
  - Adopted in the R12 report: explicit R11 deferral rationale, explicit DX11 visual-backend exception rationale, and saved CTest/manual-validation log paths.
- Spark final review: `PASS_WITH_NON_BLOCKING` after procedural documentation status sync.
