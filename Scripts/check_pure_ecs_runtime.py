#!/usr/bin/env python3
"""Enforce the single-authority pure-ECS Runtime cutover."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate


SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".inl", ".mm"}
PRODUCTION_ROOTS = (
    "AnimationSceneAdapters",
    "Audio",
    "Engine",
    "Geometry",
    "Particle",
    "PhysicsSceneAdapters",
    "RenderExtraction",
    "ResourceSceneAdapters",
    "Samples",
    "Scene",
    "Scripting",
    "Terrain",
    "Water",
    "World",
)
FORBIDDEN_IDENTIFIERS = (
    "Actor",
    "ActorComponent",
    "ActorFactory",
    "AnimationPhysicsBridgeSubsystem",
    "AudioComponent",
    "CameraComponent",
    "ColliderComponent",
    "ComponentFactory",
    "LightComponent",
    "MeshRendererComponent",
    "MeshComponent",
    "BoneComponent",
    "NodeComponent",
    "ParticleComponent",
    "PhysicsSubsystem",
    "PrimitiveComponent",
    "RigidBodyComponent",
    "SceneComponent",
    "SceneEntity",
    "SceneManager",
    "ScriptComponent",
    "SkeletonComponent",
    "SpatialSubsystem",
    "StaticMeshComponent",
    "TerrainComponent",
    "WaterComponent",
)
FORBIDDEN_INCLUDE_PARTS = (
    "Scene/Actor",
    "Scene/Component",
    "Scene/SceneEntity",
    "Scene/SceneManager",
    "Scene/SceneRuntime.h",
    "Scene/SceneComponent",
    "Scene/PrimitiveComponent",
    "Spatial/",
    "Picking/",
    "Scripting/ScriptComponent",
)
FORBIDDEN_PATHS = (
    "Picking",
    "Spatial",
    "Samples/Basic",
    "Samples/Showcase",
    "Engine/Private/RenderRuntimeComposition.cpp",
    "Engine/Private/RenderRuntimeComposition.h",
    "Scene/Include/Scene/Actor.h",
    "Scene/Include/Scene/ActorComponent.h",
    "Scene/Include/Scene/Component.h",
    "Scene/Include/Scene/SceneEntity.h",
    "Scene/Include/Scene/SceneManager.h",
    "Scripting/Include/Scripting/ScriptComponent.h",
)
REQUIRED_PATHS = (
    "ECS/Include/ECS/Registry.h",
    "Scene/Include/Scene/ECS/SceneEcsRuntime.h",
    "Scene/Include/Scene/ECS/FrozenSceneSnapshot.h",
    "World/Include/World/World.h",
    "Engine/Private/ECS/WorldEcsRuntimeComposition.cpp",
    "Engine/Private/ECS/EcsRenderRuntimeComposition.cpp",
    "Samples/RenderVerseSamples/main.cpp",
)
REQUIRED_TEST_TARGETS = (
    "EcsEntityValidation",
    "EcsFragmentQueryValidation",
    "EcsCommandTransactionValidation",
    "EcsSchedulerValidation",
    "EcsTransformHierarchyValidation",
    "EcsCleanupValidation",
    "EcsChangeTrackingValidation",
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


def strip_cpp_comments_and_literals(text: str) -> str:
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|R\"[^\n]*?\(.*?\)[^\n]*?\"|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def iter_production_sources(root: Path):
    for module in PRODUCTION_ROOTS:
        module_root = root / module
        if not module_root.is_dir():
            continue
        for path in module_root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def check_paths(root: Path, findings: list[Finding]) -> None:
    for relative in REQUIRED_PATHS:
        if not (root / relative).is_file():
            findings.append(Finding(Path(relative), 1, "required pure-ECS Runtime file is missing"))

    for relative in FORBIDDEN_PATHS:
        path = root / relative
        if path.is_file():
            findings.append(Finding(Path(relative), 1, "legacy Runtime file still exists"))
        elif path.is_dir() and any(child.is_file() for child in path.rglob("*")):
            findings.append(Finding(Path(relative), 1, "legacy Runtime directory still contains files"))

    scene_root = root / "Scene"
    allowed_scene_roots = (
        scene_root / "Include/Scene/ECS",
        scene_root / "Private/ECS",
    )
    for path in scene_root.rglob("*"):
        if not path.is_file() or path.name == "CMakeLists.txt":
            continue
        if not any(path.is_relative_to(allowed) for allowed in allowed_scene_roots):
            findings.append(
                Finding(path.relative_to(root), 1, "Scene contains a source outside its ECS directories")
            )


def check_sources(root: Path, findings: list[Finding]) -> None:
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
    for path in iter_production_sources(root):
        text = read_text(path)
        for match in include_pattern.finditer(text):
            included = match.group(1)
            if included.startswith(FORBIDDEN_INCLUDE_PARTS):
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(text, match.start()),
                        f"legacy Runtime include is forbidden: {included}",
                    )
                )

        code = strip_cpp_comments_and_literals(text)
        for identifier in FORBIDDEN_IDENTIFIERS:
            match = re.search(rf"\b{re.escape(identifier)}\b", code)
            if match:
                findings.append(
                    Finding(
                        path.relative_to(root),
                        line_number(code, match.start()),
                        f"legacy Runtime identifier is forbidden: {identifier}",
                    )
                )


def check_build_contract(root: Path, findings: list[Finding]) -> None:
    root_cmake = root / "CMakeLists.txt"
    text = read_text(root_cmake)
    forbidden = (
        "add_subdirectory(Spatial)",
        "add_subdirectory(Picking)",
        "RVX_BUILD_LEGACY_SAMPLES",
    )
    for token in forbidden:
        offset = text.find(token)
        if offset >= 0:
            findings.append(
                Finding(root_cmake.relative_to(root), line_number(text, offset), f"forbidden CMake token: {token}")
            )

    if not re.search(r"if\s*\(\s*RVX_BUILD_EDITOR\s*\).*?message\s*\(\s*FATAL_ERROR", text, re.DOTALL):
        findings.append(
            Finding(root_cmake.relative_to(root), 1, "RVX_BUILD_EDITOR=ON must fail explicitly")
        )

    tests_cmake = root / "Tests/CMakeLists.txt"
    tests_text = read_text(tests_cmake)
    for target in REQUIRED_TEST_TARGETS:
        if not re.search(rf"\bNAME\s+{re.escape(target)}\b", tests_text):
            findings.append(
                Finding(tests_cmake.relative_to(root), 1, f"required ECS validation target is missing: {target}")
            )

    sample_cmake = root / "Samples/CMakeLists.txt"
    sample_text = read_text(sample_cmake)
    if "add_subdirectory(RenderVerseSamples)" not in sample_text:
        findings.append(
            Finding(sample_cmake.relative_to(root), 1, "RenderVerseSamples product target is missing")
        )


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    findings: list[Finding] = []

    check_paths(root, findings)
    check_sources(root, findings)
    check_build_contract(root, findings)

    if findings:
        print(f"Pure ECS Runtime gate failed with {len(findings)} finding(s):")
        for finding in findings:
            print(f"  {finding.path}:{finding.line}: {finding.message}")
        return 1

    print("Pure ECS Runtime gate passed.")
    print("Runtime has one World -> SceneEcsRuntime authority and no legacy scene object model.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
