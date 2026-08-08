#!/usr/bin/env python3
"""Enforce the final M1 render ownership and publication boundary."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate


SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".mm"}
PRODUCTION_ROOTS = (
    "Render",
    "RenderContracts",
    "Resource",
    "Engine",
    "Editor",
    "Samples",
)
FORBIDDEN_SYMBOLS = (
    "IRenderResourceSource",
    "IRenderMeshUploadSource",
    "IRenderTextureUploadSource",
    "IRenderMaterialUploadSource",
    "IRenderMaterialSource",
    "GPUResourceManager",
    "RenderService",
    "LegacySynchronousRenderBridge",
)
FORBIDDEN_RENDER_LINKS = (
    "RVX::RenderExtraction",
    "RVX_RenderExtraction",
    "RVX::ResourceSceneAdapters",
    "RVX_ResourceSceneAdapters",
    "RVX::World",
    "RVX_World",
    "RVX::Scene",
    "RVX_Scene",
    "RVX::Resource",
    "RVX_Resource",
    "RVX::Runtime",
    "RVX_Runtime",
    "RVX::HAL",
    "RVX_HAL",
)
FORBIDDEN_RENDER_INCLUDE_PREFIXES = (
    "HAL/",
    "RenderExtraction/",
    "Resource/",
    "ResourceSceneAdapters/",
    "Scene/",
    "World/",
)
FORBIDDEN_SCENE_LINKS = (
    "RVX::RenderContracts",
    "RVX_RenderContracts",
    "RVX::Render",
    "RVX_Render",
    "RVX::RHI",
    "RVX_RHI",
)
FORBIDDEN_SCENE_INCLUDE_PREFIXES = (
    "Render/",
    "RenderContracts/",
    "RHI/",
    "RHI_BackendFactory/",
    "RHI_DX11/",
    "RHI_DX12/",
    "RHI_Metal/",
    "RHI_OpenGL/",
    "RHI_Vulkan/",
)
FORBIDDEN_PRODUCT_SAMPLE_INCLUDE_PREFIXES = (
    "RHI/",
    "RHI_BackendFactory/",
    "RHI_DX11/",
    "RHI_DX12/",
    "RHI_Metal/",
    "RHI_OpenGL/",
    "RHI_Vulkan/",
    "Render/RenderGraph",
    "Runtime/Camera/Camera.h",
    "Scene/SceneManager.h",
)
REQUIRED_SUBSYSTEM_METHODS = (
    "Configure",
    "TryPublishFrame",
    "RequestResize",
    "GetDiagnosticsSnapshot",
    "GetLastRuntimeResult",
    "GetLastShutdownResult",
    "ReserveResource",
    "TryEnqueueUpload",
    "RequestRelease",
    "QueryResourceStatus",
)
FORBIDDEN_SUBSYSTEM_METHODS = (
    "BeginFrame",
    "Render",
    "EndFrame",
    "Present",
    "RenderFrame",
    "ProcessGPUUploads",
    "GetRenderContext",
    "GetSceneRenderer",
    "GetDevice",
    "GetSwapChain",
    "GetRenderGraph",
    "GetGPUResourceManager",
)
REQUIRED_VALIDATION_TARGETS = (
    "RenderContractsValidation",
    "RenderFrameExtractionValidation",
    "RenderExecutorValidation",
    "RenderResourceRuntimeValidation",
    "RenderThreadRuntimeValidation",
    "RenderLifetimeCutoverValidation",
    "RenderCallerBoundaryValidation",
    "NativeRenderLifecycleValidation",
    "RenderConcurrencyTSAN",
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8-sig")


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def iter_production_sources(root: Path):
    for module in PRODUCTION_ROOTS:
        module_root = root / module
        if not module_root.is_dir():
            continue
        for path in module_root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def find_symbols(root: Path, findings: list[Finding]) -> None:
    for path in iter_production_sources(root):
        text = read_text(path)
        for symbol in FORBIDDEN_SYMBOLS:
            for match in re.finditer(rf"\b{re.escape(symbol)}\b", text):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"legacy symbol '{symbol}' is forbidden after the M1 cut",
                    )
                )


def extract_cmake_target_block(text: str, command: str, target: str) -> str:
    pattern = re.compile(
        rf"{re.escape(command)}\s*\(\s*{re.escape(target)}\b(?P<body>.*?)\)",
        re.DOTALL,
    )
    match = pattern.search(text)
    return match.group("body") if match else ""


def check_render_links(root: Path, findings: list[Finding]) -> None:
    path = root / "Render/CMakeLists.txt"
    text = read_text(path)
    blocks = "\n".join(
        match.group("body")
        for match in re.finditer(
            r"target_link_libraries\s*\(\s*RVX_Render\b(?P<body>.*?)\)",
            text,
            re.DOTALL,
        )
    )
    if not blocks:
        findings.append(Finding(path.relative_to(root), 1, "RVX_Render link declaration is missing"))
        return
    for dependency in FORBIDDEN_RENDER_LINKS:
        offset = blocks.find(dependency)
        if offset >= 0:
            findings.append(
                Finding(
                    path.relative_to(root),
                    line_number(text, text.find(dependency)),
                    f"RVX_Render must not link forbidden module '{dependency}'",
                )
            )


def check_render_includes(root: Path, findings: list[Finding]) -> None:
    render_root = root / "Render"
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
    for path in render_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = read_text(path)
        for match in include_pattern.finditer(text):
            included = match.group(1)
            if included.startswith(FORBIDDEN_RENDER_INCLUDE_PREFIXES):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"Render source includes forbidden module header '{included}'",
                    )
                )
            if included.startswith("Runtime/Window/"):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"Render source includes forbidden Runtime window header '{included}'",
                    )
                )


def check_scene_dependency_boundary(root: Path, findings: list[Finding]) -> None:
    cmake_path = root / "Scene/CMakeLists.txt"
    cmake = read_text(cmake_path)
    link_blocks = "\n".join(
        match.group("body")
        for match in re.finditer(
            r"target_link_libraries\s*\(\s*RVX_Scene\b(?P<body>.*?)\)",
            cmake,
            re.DOTALL,
        )
    )
    if not link_blocks:
        findings.append(
            Finding(cmake_path.relative_to(root), 1, "RVX_Scene link declaration is missing")
        )
    else:
        for dependency in FORBIDDEN_SCENE_LINKS:
            if dependency in link_blocks:
                findings.append(
                    Finding(
                        cmake_path.relative_to(root),
                        line_number(cmake, cmake.find(dependency)),
                        f"RVX_Scene must not link forbidden module '{dependency}'",
                    )
                )

    scene_root = root / "Scene"
    include_pattern = re.compile(r'^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]', re.MULTILINE)
    for path in scene_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = read_text(path)
        for match in include_pattern.finditer(text):
            included = match.group(1)
            if included.startswith(FORBIDDEN_SCENE_INCLUDE_PREFIXES):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"Scene source includes forbidden rendering header '{included}'",
                    )
                )


def check_product_sample_boundary(root: Path, findings: list[Finding]) -> None:
    sample_root = root / "Samples/RenderVerseSamples"
    include_pattern = re.compile(r'^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]', re.MULTILINE)
    for path in sample_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = read_text(path)
        for match in include_pattern.finditer(text):
            included = match.group(1)
            if included.startswith(FORBIDDEN_PRODUCT_SAMPLE_INCLUDE_PREFIXES):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"product sample includes engine-owned rendering header '{included}'",
                    )
                )
        backend_match = re.search(r"\bRHIBackendType\s*::", text)
        if backend_match:
            findings.append(
                Finding(
                    path.relative_to(root),
                    line_number(text, backend_match.start()),
                    "product sample scene must not branch on the selected RHI backend",
                )
            )

    context_path = root / "Samples/Common/Include/Samples/SampleContext.h"
    context = read_text(context_path)
    for forbidden_type in (
        r"\bCamera\s*\*",
        r"\bSceneManager\s*[&*]",
        r"\bRenderFrameSettings\s*&",
    ):
        match = re.search(forbidden_type, context)
        if match:
            findings.append(
                Finding(
                    context_path.relative_to(root),
                    line_number(context, match.start()),
                    "SampleContext must expose Scene/CameraComponent facades, not legacy raw ownership",
                )
            )


def check_subsystem_surface(root: Path, findings: list[Finding]) -> None:
    path = root / "Render/Include/Render/RenderSubsystem.h"
    text = read_text(path)
    for method in REQUIRED_SUBSYSTEM_METHODS:
        if re.search(rf"\b{re.escape(method)}\s*\(", text) is None:
            findings.append(Finding(path.relative_to(root), 1, f"RenderSubsystem must expose {method}()"))
    for method in FORBIDDEN_SUBSYSTEM_METHODS:
        match = re.search(rf"\b{re.escape(method)}\s*\(", text)
        if match:
            findings.append(
                Finding(
                    path.relative_to(root),
                    line_number(text, match.start()),
                    f"RenderSubsystem legacy/live-object method {method}() is forbidden",
                )
            )


def check_scene_renderer_surface(root: Path, findings: list[Finding]) -> None:
    path = root / "Render/Include/Render/Renderer/SceneRenderer.h"
    text = read_text(path)
    patterns = (
        r"SetupView\s*\([^)]*\bWorld\s*\*",
        r"SetupView\s*\([^)]*\bSceneManager\s*\*",
        r"SetupView\s*\([^)]*\bCamera\s*[&*]",
        r"CollectFromWorld\s*\(",
        r"CollectFromSceneManager\s*\(",
    )
    for pattern in patterns:
        match = re.search(pattern, text, re.DOTALL)
        if match:
            findings.append(
                Finding(
                    path.relative_to(root),
                    line_number(text, match.start()),
                    "SceneRenderer must consume immutable frame values, not World/SceneManager/Camera entry points",
                )
            )


def check_production_executor_boundary(root: Path, findings: list[Finding]) -> None:
    forbidden = ("CreateInlineRenderExecutor", "RenderExecutorKind::InlineTest")
    implementation_allowlist = {
        Path("Render/Private/Runtime/RenderThreadRuntime.cpp"),
    }
    for module in ("Render", "Engine", "Editor", "Samples", "Resource"):
        module_root = root / module
        if not module_root.is_dir():
            continue
        for path in module_root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            if path.relative_to(root) in implementation_allowlist:
                continue
            text = read_text(path)
            for needle in forbidden:
                offset = text.find(needle)
                if offset >= 0:
                    findings.append(
                        Finding(
                            path.relative_to(root),
                            line_number(text, offset),
                            f"production source must not select test-only inline execution via '{needle}'",
                        )
                    )


def check_rhi_backend_factory_boundary(root: Path, findings: list[Finding]) -> None:
    core_cmake_path = root / "RHI/CMakeLists.txt"
    core_cmake = read_text(core_cmake_path)
    if "RHIModule.cpp" in core_cmake or "RHIBackendFactory.cpp" in core_cmake:
        findings.append(
            Finding(
                core_cmake_path.relative_to(root),
                1,
                "RVX_RHI must not compile the enabled-backend factory",
            )
        )

    device_header_path = root / "RHI/Include/RHI/RHIDevice.h"
    device_header = read_text(device_header_path)
    if "CreateRHIDevice" in device_header:
        findings.append(
            Finding(
                device_header_path.relative_to(root),
                1,
                "RHIDevice.h must remain independent of enabled-backend composition",
            )
        )

    factory_cmake_path = root / "RHI_BackendFactory/CMakeLists.txt"
    factory_header_path = (
        root
        / "RHI_BackendFactory/Include/RHI_BackendFactory/RHIBackendFactory.h"
    )
    factory_source_path = (
        root / "RHI_BackendFactory/Private/RHIBackendFactory.cpp"
    )
    for required_path in (
        factory_cmake_path,
        factory_header_path,
        factory_source_path,
    ):
        if not required_path.is_file():
            findings.append(
                Finding(
                    required_path.relative_to(root),
                    1,
                    "RHI backend factory boundary file is missing",
                )
            )
    if not all(
        path.is_file()
        for path in (factory_cmake_path, factory_header_path, factory_source_path)
    ):
        return

    factory_cmake = read_text(factory_cmake_path)
    for dependency in (
        "RVX::RHI",
        "RVX::RHI_DX11",
        "RVX::RHI_DX12",
        "RVX::RHI_Vulkan",
        "RVX::RHI_Metal",
        "RVX::RHI_OpenGL",
    ):
        if dependency not in factory_cmake:
            findings.append(
                Finding(
                    factory_cmake_path.relative_to(root),
                    1,
                    f"RHI backend factory must compose {dependency}",
                )
            )

    factory_header = read_text(factory_header_path)
    if "CreateRHIDevice" not in factory_header:
        findings.append(
            Finding(
                factory_header_path.relative_to(root),
                1,
                "RHI backend factory must own the CreateRHIDevice declaration",
            )
        )

    render_cmake_path = root / "Render/CMakeLists.txt"
    render_cmake = read_text(render_cmake_path)
    render_links = "\n".join(
        match.group("body")
        for match in re.finditer(
            r"target_link_libraries\s*\(\s*RVX_Render\b(?P<body>.*?)\)",
            render_cmake,
            re.DOTALL,
        )
    )
    if "RVX::RHI_BackendFactory" not in render_links:
        findings.append(
            Finding(
                render_cmake_path.relative_to(root),
                1,
                "RVX_Render must link the enabled-backend factory privately",
            )
        )

    tests_cmake_path = root / "Tests/CMakeLists.txt"
    tests_cmake = read_text(tests_cmake_path)
    for token in (
        "RHIBackendFactoryLinkClosureValidation",
        "Architecture.RHIBackendFactoryLinkClosure",
    ):
        if token not in tests_cmake:
            findings.append(
                Finding(
                    tests_cmake_path.relative_to(root),
                    1,
                    f"RHI backend factory link-closure gate is missing '{token}'",
                )
            )

    baseline_path = root / "Scripts/run_architecture_baseline.ps1"
    baseline = read_text(baseline_path)
    if "Architecture\\.RHIBackendFactoryLinkClosure" not in baseline:
        findings.append(
            Finding(
                baseline_path.relative_to(root),
                1,
                "architecture baseline must execute the RHI backend factory link-closure gate",
            )
        )


def check_validation_inventory(root: Path, findings: list[Finding]) -> None:
    path = root / "Tests/CMakeLists.txt"
    text = read_text(path)
    for target in REQUIRED_VALIDATION_TARGETS:
        if re.search(rf"\bNAME\s+{re.escape(target)}\b", text) is None:
            findings.append(Finding(path.relative_to(root), 1, f"required M1 validation target '{target}' is not registered"))


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    findings: list[Finding] = []
    find_symbols(root, findings)
    check_render_links(root, findings)
    check_render_includes(root, findings)
    check_scene_dependency_boundary(root, findings)
    check_product_sample_boundary(root, findings)
    check_subsystem_surface(root, findings)
    check_scene_renderer_surface(root, findings)
    check_production_executor_boundary(root, findings)
    check_rhi_backend_factory_boundary(root, findings)
    check_validation_inventory(root, findings)

    if findings:
        print(f"M1 architecture cut failed with {len(findings)} finding(s):")
        for finding in sorted(findings, key=lambda item: (item.path.as_posix(), item.line, item.message)):
            print(f"  {finding.path.as_posix()}:{finding.line}: {finding.message}")
        return 1

    print("M1 architecture cut passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
