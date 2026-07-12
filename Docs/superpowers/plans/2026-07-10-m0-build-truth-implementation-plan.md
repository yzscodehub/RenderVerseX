# M0 Build Truth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make RenderVerseX configuration, build, test discovery, architecture gates, whitespace checks, and CI evidence fail closed and reproducible before runtime architecture changes begin.

**Architecture:** M0 adds one strict input contract shared by the Python architecture gates, one CMake target that builds every executable referenced by CTest, and one cross-platform PowerShell entry point that performs clean configure, build, inventory validation, architecture baseline execution, unit/lint execution, and diff hygiene checks. The existing architecture baseline remains the focused test selector, but it will verify that every required pattern resolves to real discovered tests before running them.

**Tech Stack:** C++20, CMake 3.21+, CTest JSON v1, GoogleTest discovery, Python 3 standard library, PowerShell 7, GitHub Actions, vcpkg.

## Global Constraints

- Runtime is the primary product path; Editor must not become a correctness dependency.
- Tier 1 backends are DX12, Vulkan, and Metal; DX11 and OpenGL are Tier 2 compatibility backends.
- M0 changes build truth and validation infrastructure only; it does not change rendering behavior or module ownership.
- Missing Python, repository roots, manifests, build targets, test executables, or baseline tests must fail with a non-zero exit code.
- A backend is never declared supported from compilation alone; M0 reports build/test evidence without changing capability status.
- Preserve the existing C++20, CMake, naming, logging, include-order, and module conventions in `AGENTS.md`.
- Do not broaden M0 into M1 packet, queue, extraction, resource registry, or RHI work.
- Each task ends with a focused verification and a separate commit.

---

## Program Sequence

This plan implements M0 only. The remaining approved sequence is:

1. M0 Build Truth — this document.
2. M1 Architecture Cut — Engine-owned extraction, immutable frame packets, generational render resources, bounded queues.
3. M2 Tier 1 RHI Proof and M3 Resource Production — may proceed in parallel only after M1 freezes their shared contracts.
4. M4 Canonical Runtime Renderer — one RenderGraph path on DX12, Vulkan, and Metal.
5. M5 Runtime Hardening — recovery, memory pressure, cancellation, shutdown, soak, and leak gates.
6. M6 Runtime Release Candidate — package-only startup, release evidence, support matrix, and known limitations.

## File Responsibility Map

### New files

- `Scripts/architecture_gate_common.py` — validates common gate inputs and converts input failures into exit code 2.
- `Scripts/test_architecture_gate_inputs.py` — subprocess regression suite proving every Python architecture gate rejects missing roots and manifests.
- `Scripts/run_build_truth.ps1` — single authoritative M0 configure/build/test/diff command and report producer.
- `Docs/build-truth.md` — operator contract for local and CI build-truth execution and artifacts.

### Modified infrastructure

- `Scripts/check_module_boundaries.py`
- `Scripts/check_module_boundary_manifest.py`
- `Scripts/check_cmake_module_visibility.py`
- `Scripts/check_cmake_module_include_edges.py`
- `Scripts/check_cmake_module_links.py`
- `Scripts/check_public_header_linkage.py`
- `Scripts/check_editor_runtime_boundary.py`
- `Scripts/check_architecture_phase_gates.py`
  - All eight consume the same validated repository-root contract.
  - The five manifest-driven gates also validate the manifest path before opening it.
- `Scripts/run_architecture_baseline.ps1` — proves required test discovery before executing the focused baseline and emits JSON/JUnit evidence.
- `CMakeLists.txt` — makes the Editor an explicit optional build product instead of an unconditional Runtime dependency.
- `CMakePresets.json` — sets `RVX_BUILD_EDITOR=OFF` for the authoritative Runtime-first presets.
- `Tests/CMakeLists.txt` — requires Python, registers gate-input regression, records validation executable targets, and defines `RVXValidationInventory`.
- `.github/workflows/ci.yml` — invokes the same build-truth entry point on Windows, Linux, and macOS and retains artifacts.
- `README.md` — points contributors at the single authoritative command.

### Mechanical whitespace-only cleanup

- `Core/Include/Core/Job/JobSystem.h`
- `Geometry/Include/Geometry/Asset/Material.h`
- `Geometry/Include/Geometry/Asset/Mesh.h`
- `Geometry/Include/Geometry/Asset/Model.h`
- `Geometry/Include/Geometry/Asset/Node.h`
- `Geometry/Include/Geometry/Asset/VertexAttribute.h`
- `Geometry/Private/Asset/Material.cpp`
- `Geometry/Private/Asset/Mesh.cpp`
- `Geometry/Private/Asset/Model.cpp`
- `Geometry/Private/Asset/Node.cpp`
- `Geometry/Private/Asset/VertexAttribute.cpp`
- `Particle/Private/Particle/GPU/ParticleSorter.h`
- `Render/Include/Render/Material/MaterialBinder.h`
- `ResourceSceneAdapters/Include/ResourceSceneAdapters/ResourceSceneAdapters.h`
- `ResourceSceneAdapters/Private/ModelResourceSceneInstantiation.cpp`
- `ResourceSceneAdapters/Private/ResourceSceneAdapters.cpp`
- `Water/Private/Water/WaterSurface.h`

---

### Task 1: Make every Python architecture gate fail closed

**Files:**

- Create: `Scripts/architecture_gate_common.py`
- Create: `Scripts/test_architecture_gate_inputs.py`
- Modify: `Scripts/check_module_boundaries.py`
- Modify: `Scripts/check_module_boundary_manifest.py`
- Modify: `Scripts/check_cmake_module_visibility.py`
- Modify: `Scripts/check_cmake_module_include_edges.py`
- Modify: `Scripts/check_cmake_module_links.py`
- Modify: `Scripts/check_public_header_linkage.py`
- Modify: `Scripts/check_editor_runtime_boundary.py`
- Modify: `Scripts/check_architecture_phase_gates.py`
- Modify: `Tests/CMakeLists.txt:75-190`

**Interfaces:**

- Consumes: a user-supplied repository root and, for manifest gates, a JSON configuration path.
- Produces: `resolve_repo_root(raw_root: str) -> pathlib.Path`, `resolve_required_file(root: pathlib.Path, raw_path: str, label: str) -> pathlib.Path`, and `run_gate(main: Callable[[], int]) -> int`.
- Exit contract: `0` means the gate ran and passed, `1` means the gate ran and found violations, and `2` means required inputs were missing or invalid.

- [ ] **Step 1: Write the failing subprocess regression suite**

Create `Scripts/test_architecture_gate_inputs.py`:

```python
#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "Scripts"
ROOT_ONLY_GATES = (
    "check_cmake_module_visibility.py",
    "check_editor_runtime_boundary.py",
    "check_architecture_phase_gates.py",
)
MANIFEST_GATES = (
    "check_module_boundaries.py",
    "check_module_boundary_manifest.py",
    "check_cmake_module_include_edges.py",
    "check_cmake_module_links.py",
    "check_public_header_linkage.py",
)


def run_gate(script: str, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / script), *args],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class ArchitectureGateInputTests(unittest.TestCase):
    def test_all_gates_reject_missing_repository_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            missing_root = Path(temp_dir) / "missing-repository"
            for script in (*ROOT_ONLY_GATES, *MANIFEST_GATES):
                with self.subTest(script=script):
                    result = run_gate(script, "--root", str(missing_root))
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("architecture gate input error", result.stderr.lower())

    def test_manifest_gates_reject_missing_manifest(self) -> None:
        missing_manifest = REPO_ROOT / "Docs" / "missing-module-boundaries.json"
        for script in MANIFEST_GATES:
            with self.subTest(script=script):
                result = run_gate(
                    script,
                    "--root",
                    str(REPO_ROOT),
                    "--config",
                    str(missing_manifest),
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("architecture gate input error", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the regression suite and verify the current gates fail the contract**

Run:

```powershell
& "D:\Programs\Python\Python312\python.exe" Scripts\test_architecture_gate_inputs.py -v
```

Expected: FAIL. At minimum, `check_cmake_module_visibility.py` and `check_editor_runtime_boundary.py` currently return `0` for a missing repository root instead of `2`.

- [ ] **Step 3: Add the shared fail-closed input helper**

Create `Scripts/architecture_gate_common.py`:

```python
#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path
from typing import Callable


class GateInputError(RuntimeError):
    pass


def resolve_repo_root(raw_root: str) -> Path:
    root = Path(raw_root).resolve()
    if not root.is_dir():
        raise GateInputError(f"repository root does not exist: {root}")
    marker = root / "CMakeLists.txt"
    if not marker.is_file():
        raise GateInputError(f"repository root has no CMakeLists.txt: {root}")
    return root


def resolve_required_file(root: Path, raw_path: str, label: str) -> Path:
    path = Path(raw_path)
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    if not path.is_file():
        raise GateInputError(f"{label} does not exist: {path}")
    return path


def run_gate(main: Callable[[], int]) -> int:
    try:
        return main()
    except GateInputError as error:
        print(f"Architecture gate input error: {error}", file=sys.stderr)
        return 2
```

- [ ] **Step 4: Route all eight gates through the shared root validator**

In every gate script, import:

```python
from architecture_gate_common import resolve_repo_root, run_gate
```

Replace:

```python
root = Path(args.root).resolve()
```

with:

```python
root = resolve_repo_root(args.root)
```

Replace every gate entry point:

```python
if __name__ == "__main__":
    sys.exit(main())
```

with:

```python
if __name__ == "__main__":
    sys.exit(run_gate(main))
```

Keep `pathlib.Path` imports that are still used by scanners and dataclasses.

- [ ] **Step 5: Validate configuration files before the five manifest gates load JSON**

In each manifest-driven gate, extend the import:

```python
from architecture_gate_common import resolve_repo_root, resolve_required_file, run_gate
```

Immediately after resolving the root in `main()`, add:

```python
config_path = resolve_required_file(root, args.config, "module boundary configuration")
```

Pass `str(config_path)` to the existing `load_config` function. For functions that already accept a `Path`, pass `config_path` directly. Do not change violation semantics or scanning rules.

- [ ] **Step 6: Require Python whenever tests are enabled and register the regression**

In `Tests/CMakeLists.txt`, replace the optional Python block with a required interpreter and keep all existing architecture tests unconditionally registered:

```cmake
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
    NAME Architecture.GateInputsFailClosed
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/test_architecture_gate_inputs.py
        -v
)
set_tests_properties(Architecture.GateInputsFailClosed PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
```

Delete the `else()` branch that currently warns and disables architecture tests when Python is absent. Preserve the eight existing architecture `add_test` definitions exactly.

- [ ] **Step 7: Run focused validation and confirm controlled exit codes**

Run:

```powershell
cmake --preset win_x64_debug
& "D:\Programs\Python\Python312\python.exe" Scripts\test_architecture_gate_inputs.py -v
ctest --test-dir build\win_x64_debug -C Debug -R "Architecture.GateInputsFailClosed|Architecture.ModuleBoundaries|Architecture.CMakeModuleVisibility|Architecture.EditorRuntimeBoundary" --output-on-failure
```

Expected:

- Python suite: `2 tests` and `OK`.
- CTest: four selected tests pass.
- Direct missing-root execution returns `2` and prints `Architecture gate input error` without a traceback.

- [ ] **Step 8: Commit the fail-closed gate contract**

```powershell
git add Scripts\architecture_gate_common.py Scripts\test_architecture_gate_inputs.py Scripts\check_module_boundaries.py Scripts\check_module_boundary_manifest.py Scripts\check_cmake_module_visibility.py Scripts\check_cmake_module_include_edges.py Scripts\check_cmake_module_links.py Scripts\check_public_header_linkage.py Scripts\check_editor_runtime_boundary.py Scripts\check_architecture_phase_gates.py Tests\CMakeLists.txt
git commit -m "test: make architecture gates fail closed"
```

---

### Task 2: Make Editor optional and build a complete Runtime CTest executable inventory

**Files:**

- Modify: `CMakeLists.txt:20-31, 200-210, 225-240`
- Modify: `CMakePresets.json:10-70`
- Modify: `Tests/CMakeLists.txt:31-73`
- Modify: `Tests/CMakeLists.txt:475-500`
- Modify: `Tests/CMakeLists.txt:770-825`
- Modify: `Tests/CMakeLists.txt:1126-1142`
- Modify: `Tests/CMakeLists.txt:2149-end`

**Interfaces:**

- Consumes: `RVX_BUILD_EDITOR`, all Runtime validation executables created by `rvx_add_gtest`, manual validation executables, fixture writers, `ModelViewer`, and `RVXCook` when those targets exist.
- Produces: Runtime-first presets with `RVX_BUILD_EDITOR=OFF` and CMake target `RVXValidationInventory`, whose successful build guarantees every executable registered by the Editor-free CTest configuration has been built.
- Editor remains available through an explicit `-DRVX_BUILD_EDITOR=ON` configuration, but no Editor target or Editor-only test participates in the M0 gate.

- [ ] **Step 1: Verify the inventory target is currently absent**

Run:

```powershell
cmake --build build\win_x64_debug --config Debug --target RVXValidationInventory
```

Expected: FAIL because `RVXValidationInventory` does not exist.

- [ ] **Step 2: Make Editor construction conditional and disable it in Runtime-first presets**

Add this option beside the existing build options in the root `CMakeLists.txt`:

```cmake
option(RVX_BUILD_EDITOR "Build the Editor application and Editor-only validation" ON)
```

Replace the unconditional Editor subdirectory with:

```cmake
if(RVX_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()
```

Add the option to the configuration summary:

```cmake
message(STATUS "  Editor:       ${RVX_BUILD_EDITOR}")
```

Set the following cache entry in `win_x64_base`, `linux_x64_base`, and `mac_arm64_base` in `CMakePresets.json`:

```json
"RVX_BUILD_EDITOR": "OFF"
```

Wrap `EditorContextValidation` and its compile options in `Tests/CMakeLists.txt`:

```cmake
if(TARGET RVX::Editor)
    rvx_add_gtest(
        NAME EditorContextValidation
        SOURCES EditorContextValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Editor
            RVX::Scene
            ${RVX_ALL_BACKENDS}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/Editor/Include
        LABELS "unit;editor"
        TIMEOUT 120
    )
    target_compile_options(EditorContextValidation PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )
endif()
```

Existing `if(TARGET RVXEditor)` smoke registrations remain unchanged and therefore disappear naturally from an Editor-free CTest inventory.

- [ ] **Step 3: Verify the Runtime preset contains no Editor target or Editor tests**

Run:

```powershell
cmake --preset win_x64_debug
Select-String -LiteralPath build\win_x64_debug\CMakeCache.txt -Pattern "^RVX_BUILD_EDITOR:BOOL=OFF$"
cmake --build build\win_x64_debug --config Debug --target RVXEditor
ctest --test-dir build\win_x64_debug -C Debug -N -R "RVXEditor|EditorContextValidation"
```

Expected: the cache check matches, building `RVXEditor` fails because the target is intentionally absent, and CTest reports zero selected Editor tests.

- [ ] **Step 4: Record every configured GoogleTest executable when it is declared**

Inside `rvx_add_gtest`, immediately after `add_executable`, add:

```cmake
    set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
        ${RVX_GTEST_NAME}
    )
```

This property is global because CMake functions have their own variable scope.

- [ ] **Step 5: Record the five manually declared test executables**

Immediately after each manual `add_executable`, append its target:

```cmake
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    VisualGoldenValidation
)
```

Repeat with the exact target name for:

```cmake
ImageContentValidation
SampleReportContentValidation
ModelViewerHDRIFixtureWriter
ModelViewerCookedBCFixtureWriter
```

- [ ] **Step 6: Define the complete inventory target after all validation targets**

Append this block at the end of `Tests/CMakeLists.txt`, before shader copying or immediately after it:

```cmake
get_property(RVX_VALIDATION_EXECUTABLE_TARGETS GLOBAL
    PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
)

foreach(RVX_EXTERNAL_TEST_TARGET IN ITEMS
    ModelViewer
    RVXCook
)
    if(TARGET ${RVX_EXTERNAL_TEST_TARGET})
        list(APPEND RVX_VALIDATION_EXECUTABLE_TARGETS
            ${RVX_EXTERNAL_TEST_TARGET}
        )
    endif()
endforeach()

list(REMOVE_DUPLICATES RVX_VALIDATION_EXECUTABLE_TARGETS)

add_custom_target(RVXValidationInventory
    DEPENDS ${RVX_VALIDATION_EXECUTABLE_TARGETS}
)
```

Do not add backend targets directly. Backend validation executables already link enabled backend libraries and therefore build the required backend dependencies.

- [ ] **Step 7: Build the inventory and prove CTest no longer advertises missing Runtime executables**

Run:

```powershell
cmake --preset win_x64_debug
cmake --build build\win_x64_debug --config Debug --target RVXValidationInventory
$inventory = ctest --test-dir build\win_x64_debug -C Debug -N 2>&1
$missingExecutables = $inventory | Select-String -Pattern "NOT_BUILT|Could not find executable"
if ($missingExecutables) { throw "CTest inventory still contains missing executables" }
```

Expected:

- `CorePathValidation.exe`, `AnimationValidation.exe`, `UIValidation.exe`, and `ModelViewerCookedBCFixtureWriter.exe` exist under the Debug output directories.
- The final search returns no matches.
- `ctest -N` still reports the complete test count rather than silently dropping tests.

- [ ] **Step 8: Run the four formerly unavailable entries**

Run:

```powershell
ctest --test-dir build\win_x64_debug -C Debug -R "CorePathValidation|AnimationValidation|UIValidation|ModelViewerCookedBCMaterialFixture" --output-on-failure
```

Expected: the three GoogleTest suites are discovered under their real case names, and the cooked BC fixture writer test executes instead of reporting a missing executable. GPU-dependent follow-up tests are outside this focused command.

- [ ] **Step 9: Commit the Editor boundary and inventory target**

```powershell
git add CMakeLists.txt CMakePresets.json Tests\CMakeLists.txt
git commit -m "build: add runtime validation inventory target"
```

---

### Task 3: Remove accumulated branch whitespace debt

**Files:**

- Modify only the 17 files listed in the mechanical cleanup section of this plan.

**Interfaces:**

- Consumes: the current `master...HEAD` branch diff.
- Produces: identical C++ tokens with no trailing spaces or tabs and a clean Git whitespace check.

- [ ] **Step 1: Capture the failing branch-range whitespace check**

Run:

```powershell
git diff --check master...HEAD
```

Expected: FAIL with trailing-whitespace findings in exactly the 17 listed files.

- [ ] **Step 2: Remove trailing spaces and tabs without changing code tokens**

For each listed file, remove only matches reported by:

```powershell
rg -n "[ \t]+$" Core\Include\Core\Job\JobSystem.h Geometry\Include\Geometry\Asset Geometry\Private\Asset Particle\Private\Particle\GPU\ParticleSorter.h Render\Include\Render\Material\MaterialBinder.h ResourceSceneAdapters Water\Private\Water\WaterSurface.h
```

Use patch edits or an encoding-preserving formatter. Do not reflow comments, rename symbols, reorder includes, or apply broad formatting.

- [ ] **Step 3: Prove the branch range and working tree are whitespace-clean**

Run:

```powershell
git diff --check master...HEAD
git diff --check
rg -n "[ \t]+$" Core\Include\Core\Job\JobSystem.h Geometry\Include\Geometry\Asset Geometry\Private\Asset Particle\Private\Particle\GPU\ParticleSorter.h Render\Include\Render\Material\MaterialBinder.h ResourceSceneAdapters Water\Private\Water\WaterSurface.h
```

Expected: all three commands produce no findings.

- [ ] **Step 4: Rebuild the modules whose files changed**

Run:

```powershell
cmake --build build\win_x64_debug --config Debug --target RVX_Core RVX_Geometry RVX_Particle RVX_Render RVX_ResourceSceneAdapters RVX_Water
```

Expected: all six targets build successfully; no test behavior changes are expected because the patch is whitespace-only.

- [ ] **Step 5: Commit the mechanical cleanup separately**

```powershell
git add Core\Include\Core\Job\JobSystem.h Geometry\Include\Geometry\Asset Geometry\Private\Asset Particle\Private\Particle\GPU\ParticleSorter.h Render\Include\Render\Material\MaterialBinder.h ResourceSceneAdapters Water\Private\Water\WaterSurface.h
git diff --cached --check
git commit -m "style: remove architecture branch whitespace debt"
```

---

### Task 4: Harden the focused architecture baseline and add the authoritative M0 runner

**Files:**

- Modify: `Scripts/run_architecture_baseline.ps1`
- Create: `Scripts/run_build_truth.ps1`

**Interfaces:**

- Consumes: a configured build directory, configuration name, configure/build/test presets, and a Git base reference.
- Produces: `ArchitectureBaseline.json`, `ArchitectureBaseline.junit.xml`, `BuildTruth.json`, command logs, and a non-zero exit for any missing requirement or failed command.
- `run_architecture_baseline.ps1` remains callable on its own for fast focused checks.
- `run_build_truth.ps1` is the only command used for an M0 clean gate.

- [ ] **Step 1: Preserve the existing baseline patterns as explicit requirements**

In `Scripts/run_architecture_baseline.ps1`, replace the joined regex declaration with this array and derive the combined regex from it:

```powershell
$baselinePatterns = @(
    "Architecture\.GateInputsFailClosed",
    "Architecture\.ModuleBoundaries",
    "Architecture\.ModuleBoundaryManifest",
    "Architecture\.CMakeModuleVisibility",
    "Architecture\.CMakeModuleIncludeEdges",
    "Architecture\.CMakeModuleLinks",
    "Architecture\.PublicHeaderLinkage",
    "Architecture\.EditorRuntimeBoundary",
    "Architecture\.PhaseGates",
    "CoreDebugConfigValidation\.DebugConfigurationDefinesRVXDebug",
    "AudioResourceValidation\.",
    "JobGraphValidation\.",
    "PhysicsWorldIntegrationValidation\.(BackendQueryAndShapeStubsReportExplicitMisses|UnsupportedColliderTypesDoNotCreateFallbackShapesOrQueryHits)",
    "RHIContractValidation\.",
    "SampleCLIValidation\.",
    "RenderPassValidationFixture\.(PostProcessStackEvaluateEffectsCountsRuntimeSupportedEffects|PostProcessFrameInputContractReportsMissingVelocityDepthAndHistory)",
    "RenderPostProcessStackValidation\.(RenderVisualQualityPresetAppliesExplicitEffectPolicy|SceneRendererFrameDiagnosticsExposePostProcessEffectPlans|SceneRendererFrameDiagnosticsExposeFeatureExtractionStats|EvaluateEffectsReportsRequestedButUnsupportedResources|PostProcessStackReportsEffectExecutionPlanDomainsAndTargets)",
    "RenderGraphValidation\.(GraphCreation|PassChain|MemoryAliasing|ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync)",
    "RenderSceneValidation\.(RenderSceneApplyProxySnapshotPopulatesObjectsAndLights|RenderProxyBridgeBuildsPrimitiveAndLightSnapshot|RenderProxyBridgeBuildsLegacyMeshRendererProxy)",
    "ActorComponentValidation\.(ActorOwnsComponentsAndDispatchesLifecycle|WorldSpawnActorDelegatesToSceneManager|ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit)",
    "AppModeBoundaryValidation\.",
    "ParticleValidation\.FeaturePublicHeadersDoNotIncludeRenderOrRHI",
    "FeatureBoundaryValidation\.(WaterComponentPublicHeaderDoesNotExposeRenderOrRHI|WaterModuleDoesNotIncludeOrLinkRHI|WaterComponentBuildsRenderSnapshotWithoutGPUHandles|TerrainComponentPublicHeaderDoesNotExposeRenderOrRHI|TerrainModuleDoesNotIncludeOrLinkRHI|TerrainComponentBuildsRenderSnapshotWithoutGPUHandles|RenderFeatureSceneBridgeCollectsFeatureSnapshotsThroughProviderContract)",
    "ResourceInstantiationValidation\.(ModelResourceInstantiateActorUsesStaticMeshComponent|LegacyInstantiateDelegatesToActorPath|ProductionPathsDoNotCreateLegacyMeshRendererComponents|WorldLoadModelResourceReplacesSceneContent)",
    "ResourceRuntimePolicyValidation\.",
    "RenderHonestyValidationFixture\.(JsonArchiveReadPathReportsUnsupportedInsteadOfPretendingSuccess|PlaceholderAssetImportersFailInsteadOfReportingSuccess|PostProcessStubPassesAreUnsupportedAndDisabled|TerrainMaterialRenderDataExportDoesNotPretendGpuUpload|TerrainHeightmapGpuTextureUploadIsHonestWhenUnavailable|TerrainLODFallbacksExposeDeterministicStatus|TerrainPlaceholderPathsExposeHonestDiagnostics|SceneRendererLegacyCollectionFallbackIsRemoved|RenderGraphCompileDiagnosticsExposeReadBeforeWrite)",
    "SystemIntegration\.(SceneEntityAndManager|ResourceBasics)"
)
$baselineRegex = $baselinePatterns -join "|"
```

- [ ] **Step 2: Add report output and preflight parameters**

Use this parameter header and strict mode:

```powershell
param(
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$ReportPath = "",
    [switch]$ListOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
```

After resolving `$buildPath`, set default artifact paths:

```powershell
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $buildPath "BuildTruth\ArchitectureBaseline.json"
}
$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
$junitPath = Join-Path $reportDirectory "ArchitectureBaseline.junit.xml"
```

- [ ] **Step 3: Prove every baseline requirement resolves to discovered tests**

Add this helper and preflight before the existing CTest execution:

```powershell
function Get-CTestInventory([string]$Pattern) {
    $jsonLines = & ctest --test-dir $buildPath -C $Configuration -R $Pattern --show-only=json-v1
    if ($LASTEXITCODE -ne 0) {
        throw "CTest inventory query failed for pattern '$Pattern'"
    }
    return (($jsonLines -join "`n") | ConvertFrom-Json)
}

$missingPatterns = @()
foreach ($pattern in $baselinePatterns) {
    $probe = Get-CTestInventory $pattern
    if (@($probe.tests).Count -eq 0) {
        $missingPatterns += $pattern
    }
}

if ($missingPatterns.Count -ne 0) {
    throw "Architecture baseline requirements are not discovered: $($missingPatterns -join ', ')"
}

$selectedInventory = Get-CTestInventory $baselineRegex
$selectedNames = @($selectedInventory.tests | ForEach-Object { $_.name })
$unbuiltNames = @($selectedNames | Where-Object { $_ -match "_NOT_BUILT$" })
if ($unbuiltNames.Count -ne 0) {
    throw "Architecture baseline contains unbuilt tests: $($unbuiltNames -join ', ')"
}
```

For `-ListOnly`, print `$selectedNames`, print `Selected tests: <count>`, write a JSON report with status `listed`, and exit `0` without running tests.

- [ ] **Step 4: Execute the baseline with JUnit and JSON evidence**

Append `--output-junit $junitPath` to the normal CTest arguments. After execution, always write:

```powershell
$ctestExitCode = $LASTEXITCODE
$report = [ordered]@{
    schema = "RVX.BuildTruth.ArchitectureBaseline"
    schemaVersion = 1
    sourceCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
    buildDir = $buildPath.ToString()
    configuration = $Configuration
    selectedTestCount = $selectedNames.Count
    selectedTests = $selectedNames
    status = if ($ctestExitCode -eq 0) { "passed" } else { "failed" }
    ctestExitCode = $ctestExitCode
    junitPath = $junitPath
}
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
exit $ctestExitCode
```

- [ ] **Step 5: Write the authoritative clean build-truth runner**

Create `Scripts/run_build_truth.ps1` with this complete orchestration:

```powershell
param(
    [string]$ConfigurePreset = "win_x64_debug",
    [string]$BuildPreset = "win_x64_debug",
    [string]$TestPreset = "win_x64_debug_unit_lint",
    [string]$BuildDir = "build\win_x64_debug",
    [string]$Configuration = "Debug",
    [string]$BaseRef = "master",
    [switch]$Fresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
$buildRootPrefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$reportDir = Join-Path $buildPath "BuildTruth"
$reportPath = Join-Path $reportDir "BuildTruth.json"
$baselineReport = Join-Path $reportDir "ArchitectureBaseline.json"
$commands = [Collections.Generic.List[object]]::new()
$status = "failed"
$failure = ""
$testCount = 0

function Invoke-Checked([string]$Name, [scriptblock]$Command) {
    $started = Get-Date
    try {
        & $Command
        $exitCode = $LASTEXITCODE
    }
    catch {
        $exitCode = 1
        $commands.Add([ordered]@{
            name = $Name
            exitCode = $exitCode
            durationMs = [int]((Get-Date) - $started).TotalMilliseconds
        })
        throw
    }
    $commands.Add([ordered]@{
        name = $Name
        exitCode = $exitCode
        durationMs = [int]((Get-Date) - $started).TotalMilliseconds
    })
    if ($exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode"
    }
}

if (-not $buildPath.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildDir must resolve below '$buildRoot': $buildPath"
}

Push-Location $repoRoot
try {
    Invoke-Checked "verify-base-ref" {
        git rev-parse --verify "$BaseRef^{commit}" | Out-Null
    }

    if ($Fresh -and (Test-Path -LiteralPath $buildPath)) {
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }

    Invoke-Checked "configure" {
        cmake --preset $ConfigurePreset
    }
    $editorSetting = Select-String `
        -LiteralPath (Join-Path $buildPath "CMakeCache.txt") `
        -Pattern "^RVX_BUILD_EDITOR:BOOL=OFF$"
    if (-not $editorSetting) {
        throw "M0 build truth requires RVX_BUILD_EDITOR=OFF"
    }
    Invoke-Checked "build" {
        cmake --build --preset $BuildPreset
    }
    Invoke-Checked "validation-inventory-target" {
        cmake --build $buildPath --config $Configuration --target RVXValidationInventory
    }

    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $inventoryLog = Join-Path $reportDir "CTestInventory.txt"
    $inventoryStarted = Get-Date
    $inventoryLines = & ctest --test-dir $buildPath -C $Configuration -N 2>&1
    $inventoryExit = $LASTEXITCODE
    $inventoryLines | Set-Content -LiteralPath $inventoryLog -Encoding utf8
    $commands.Add([ordered]@{
        name = "ctest-inventory"
        exitCode = $inventoryExit
        durationMs = [int]((Get-Date) - $inventoryStarted).TotalMilliseconds
    })
    if ($inventoryExit -ne 0) {
        throw "CTest inventory command failed with exit code $inventoryExit"
    }
    $inventoryText = $inventoryLines -join "`n"
    if ($inventoryText -match "NOT_BUILT|Could not find executable") {
        throw "CTest inventory contains unavailable executables; see $inventoryLog"
    }
    $totalMatch = [regex]::Match($inventoryText, "Total Tests:\s+(\d+)")
    $testCount = if ($totalMatch.Success) { [int]$totalMatch.Groups[1].Value } else { 0 }
    if ($testCount -eq 0) {
        throw "CTest inventory reported zero tests"
    }

    Invoke-Checked "architecture-baseline" {
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot "run_architecture_baseline.ps1") `
            -BuildDir $BuildDir `
            -Configuration $Configuration `
            -ReportPath $baselineReport
    }

    if (-not [string]::IsNullOrWhiteSpace($TestPreset)) {
        Invoke-Checked "unit-lint" {
            ctest --preset $TestPreset
        }
    }

    Invoke-Checked "branch-diff-hygiene" {
        git diff --check "$BaseRef...HEAD"
    }
    Invoke-Checked "working-diff-hygiene" {
        git diff --check
    }
    Invoke-Checked "staged-diff-hygiene" {
        git diff --cached --check
    }
    $status = "passed"
}
catch {
    $failure = $_.Exception.Message
    throw
}
finally {
    if (Test-Path -LiteralPath $buildPath) {
        New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
        $sourceCommit = (& git rev-parse HEAD).Trim()
        $report = [ordered]@{
            schema = "RVX.BuildTruth.Report"
            schemaVersion = 1
            sourceCommit = $sourceCommit
            baseRef = $BaseRef
            configurePreset = $ConfigurePreset
            buildPreset = $BuildPreset
            testPreset = $TestPreset
            buildDir = $buildPath
            configuration = $Configuration
            fresh = [bool]$Fresh
            ctestInventoryCount = $testCount
            architectureBaselineReport = $baselineReport
            status = $status
            failure = $failure
            commands = $commands
        }
        $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
    }
    Pop-Location
}

Write-Host "Build truth passed. Report: $reportPath"
```

Keep the script orchestration-only. Do not add backend capability claims, visual goldens, package loading, or performance thresholds in M0.

- [ ] **Step 6: Verify focused listing and the non-fresh authoritative path**

Run:

```powershell
Scripts\run_architecture_baseline.ps1 -BuildDir build\win_x64_debug -Configuration Debug -ListOnly
Scripts\run_build_truth.ps1 -BuildDir build\win_x64_debug -Configuration Debug -BaseRef master
```

Expected:

- Every baseline pattern discovers at least one real test.
- `CMakeCache.txt` records `RVX_BUILD_EDITOR:BOOL=OFF`; no Editor target or Editor-only test is part of the gate.
- No selected test ends in `_NOT_BUILT`.
- `BuildTruth.json`, `ArchitectureBaseline.json`, `ArchitectureBaseline.junit.xml`, and `CTestInventory.txt` exist under `build/win_x64_debug/BuildTruth`.
- Both scripts return `0` only when every invoked command succeeds.

- [ ] **Step 7: Verify the runner refuses unsafe cleanup paths**

Run:

```powershell
Scripts\run_build_truth.ps1 -BuildDir . -Fresh
```

Expected: FAIL before deletion with `BuildDir must resolve below` and the repository remains untouched.

- [ ] **Step 8: Commit the baseline and runner**

```powershell
git add Scripts\run_architecture_baseline.ps1 Scripts\run_build_truth.ps1
git diff --cached --check
git commit -m "build: add authoritative build truth gate"
```

---

### Task 5: Make CI and contributor documentation use the authoritative gate

**Files:**

- Modify: `.github/workflows/ci.yml`
- Create: `Docs/build-truth.md`
- Modify: `README.md`

**Interfaces:**

- Consumes: `Scripts/run_build_truth.ps1` and the existing configure/build/test presets.
- Produces: identical Windows, Linux, and macOS M0 entry points plus retained `BuildTruth` artifacts.

- [ ] **Step 1: Document the operator contract**

Create `Docs/build-truth.md`:

````markdown
# Build Truth Gate

The M0 build-truth gate is the authoritative precondition for architecture work.
It configures the selected preset, builds the project and complete validation
inventory, rejects unavailable CTest executables, runs the focused architecture
baseline and unit/lint preset, checks branch and working-tree whitespace, and
writes machine-readable evidence below the build directory.

The authoritative M0 presets set `RVX_BUILD_EDITOR=OFF`. Editor remains an
optional separate build product and is not a Runtime correctness dependency.

Windows Debug from an empty build directory:

```powershell
Scripts\run_build_truth.ps1 `
    -ConfigurePreset win_x64_debug `
    -BuildPreset win_x64_debug `
    -TestPreset win_x64_debug_unit_lint `
    -BuildDir build\win_x64_debug `
    -Configuration Debug `
    -BaseRef master `
    -Fresh
```

Linux and macOS use the matching presets and build directories. The same script
runs under PowerShell 7 (`pwsh`) on all CI hosts.

Successful execution produces:

- `BuildTruth/BuildTruth.json`
- `BuildTruth/ArchitectureBaseline.json`
- `BuildTruth/ArchitectureBaseline.junit.xml`
- `BuildTruth/CTestInventory.txt`

Exit code `0` means every configured command and evidence check passed. Missing
Python, roots, manifests, targets, test executables, baseline patterns, Git base
references, or whitespace cleanliness produce a non-zero exit.

The gate proves build and validation truth. It does not by itself certify GPU
behavior or Tier 1 backend production readiness.
````

- [ ] **Step 2: Point the README build section at the authoritative command**

Add this short block after the existing build/test commands in `README.md`:

```markdown
### Architecture preflight

Before architecture implementation work, run the clean M0 gate documented in
[`Docs/build-truth.md`](Docs/build-truth.md). It is stricter than an ordinary
incremental build because it rejects missing tests and writes reviewable evidence.
```

- [ ] **Step 3: Make every CI job fetch the base ref and run the same script**

In each job's `actions/checkout@v4` step, add:

```yaml
with:
  fetch-depth: 0
```

Replace the separate configure, build, and unit/lint steps in the Windows job with:

```yaml
- name: Build truth (Windows Debug)
  shell: pwsh
  run: >-
    ./Scripts/run_build_truth.ps1
    -ConfigurePreset win_x64_debug
    -BuildPreset win_x64_debug
    -TestPreset win_x64_debug_unit_lint
    -BuildDir build/win_x64_debug
    -Configuration Debug
    -BaseRef origin/master
    -Fresh

- name: Upload build-truth evidence
  if: always()
  uses: actions/upload-artifact@v4
  with:
    name: build-truth-windows-debug
    path: build/win_x64_debug/BuildTruth
```

Use the same two-step structure for Linux with `linux_x64_debug`, `build/linux_x64_debug`, and artifact name `build-truth-linux-debug`. Use `mac_arm64_debug`, `build/mac_arm64_debug`, and artifact name `build-truth-macos-arm64-debug` for macOS. Keep platform dependency installation and vcpkg cache steps unchanged.

- [ ] **Step 4: Run the final clean Windows M0 gate from an empty build directory**

Run:

```powershell
Scripts\run_build_truth.ps1 `
    -ConfigurePreset win_x64_debug `
    -BuildPreset win_x64_debug `
    -TestPreset win_x64_debug_unit_lint `
    -BuildDir build\win_x64_debug `
    -Configuration Debug `
    -BaseRef master `
    -Fresh
```

Expected:

- Fresh configure succeeds with Python and all enabled dependencies found.
- `RVX_BUILD_EDITOR` is `OFF`, and no Editor target or Editor-only test enters the M0 gate.
- Full Debug build and `RVXValidationInventory` succeed.
- CTest inventory has no `_NOT_BUILT` entries or missing executable messages.
- Architecture baseline passes with every required pattern discovered.
- Unit/lint preset passes.
- Branch, working, and staged diff checks are clean.
- `BuildTruth.json` reports `status: passed` and a positive CTest inventory count.

- [ ] **Step 5: Validate workflow syntax and documentation scope**

Run:

```powershell
git diff --check
rg -n "fetch-depth: 0|run_build_truth.ps1|upload-artifact@v4" .github\workflows\ci.yml
rg -n "BuildTruth.json|ArchitectureBaseline.json|does not by itself certify" Docs\build-truth.md
```

Expected: no whitespace errors; all three CI jobs call the authoritative script and upload evidence; the documentation explicitly avoids claiming GPU certification.

- [ ] **Step 6: Commit CI and documentation**

```powershell
git add .github\workflows\ci.yml Docs\build-truth.md README.md
git diff --cached --check
git commit -m "ci: enforce build truth across platforms"
```

---

## M0 Final Review Gate

After all five task commits:

- [ ] Run `git status --short` and require a clean worktree.
- [ ] Run `git diff --check master...HEAD` and require no output.
- [ ] Run `Scripts/run_build_truth.ps1 ... -Fresh` with the exact Windows command in Task 5.
- [ ] Open `build/win_x64_debug/BuildTruth/BuildTruth.json` and confirm the source commit matches `git rev-parse HEAD`.
- [ ] Confirm `CTestInventory.txt` contains a positive total and no missing executable text.
- [ ] Confirm `ArchitectureBaseline.json` reports `passed` and its JUnit artifact exists.
- [ ] Review the five commits independently; reject any runtime/rendering behavior change as out of scope.
- [ ] Record M0's commit IDs and artifact paths in the review handoff.

M0 is complete only after this gate passes. The next planning action is a separate M1 Architecture Cut implementation plan; no M1 code belongs in this plan.
