#!/usr/bin/env python3
"""Reject retired module roots, target identities, and compatibility surfaces."""
from __future__ import annotations

import argparse
import os
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, load_policy


LEGACY_ROOTS = (
    "AI", "Adapters", "Animation", "AnimationSceneAdapters", "Asset",
    "AssetContracts", "AssetImport", "AssetPipeline", "Audio", "BackendFactory",
    "Camera", "Core", "Debug", "DebugTools", "ECS", "Engine", "Geometry",
    "HAL", "Input", "Networking", "Particle", "Physics",
    "PhysicsSceneAdapters", "Picking", "Platform", "Render", "RenderContracts",
    "RenderExtraction", "RenderGraph", "Resource", "ResourceRuntime",
    "ResourceSceneAdapters", "ResourceSource", "RHI", "RHI_BackendFactory",
    "RHI_DX11", "RHI_DX12", "RHI_Metal", "RHI_OpenGL", "RHI_Vulkan",
    "Runtime", "Scene", "SceneAdapters", "SceneTypes", "Scripting",
    "ShaderCompiler", "ShaderContracts", "ShaderRuntime", "Spatial", "Terrain",
    "Tools", "UI", "Water", "World",
)

LEGACY_TARGETS = frozenset({
    "RVX_Debug", "RVX::Debug",
    "RVX_Resource", "RVX::Resource",
    "RVX_Runtime", "RVX::Runtime",
    "RVX_Tools", "RVX::Tools",
})

LEGACY_MODULE_IDS = frozenset({"Debug", "Resource", "Runtime", "Tools"})

LEGACY_COMPATIBILITY_FILES = (
    "Source/Runtime/Core/Include/Core/Math.h",
)

LEGACY_CONTENT_PATTERNS = (
    "RVX_MODEL_" "PREBAKE",
    "RVX_GLTF_MODEL_" "WITH_ANIMATION_V2",
)

SCANNED_TEXT_SUFFIXES = frozenset({
    ".c", ".cc", ".cmake", ".cpp", ".h", ".hpp", ".json", ".md",
    ".py", ".ps1", ".txt", ".yaml", ".yml",
})

IGNORED_SCAN_DIRECTORIES = frozenset({
    ".claude", ".codex-validation", ".git", ".vs", "build", "out",
})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()


def first_material_path(root: Path) -> Path | None:
    if not root.is_dir():
        return None
    for directory, _, files in os.walk(root, followlinks=False):
        if files:
            return Path(directory) / sorted(files)[0]
    return None


def main() -> int:
    args = parse_args()
    root = resolve_repo_root(args.root)
    policy = load_policy(root, args)
    failures: list[str] = []

    for legacy_root in LEGACY_ROOTS:
        material = first_material_path(root / legacy_root)
        if material is not None:
            failures.append(
                f"retired top-level module root is material: {material.relative_to(root).as_posix()}"
            )

    for relative in LEGACY_COMPATIBILITY_FILES:
        if (root / relative).exists():
            failures.append(f"retired compatibility file exists: {relative}")

    for directory, subdirectories, files in os.walk(root, followlinks=False):
        subdirectories[:] = [
            name for name in subdirectories
            if name not in IGNORED_SCAN_DIRECTORIES
        ]
        for filename in files:
            path = Path(directory) / filename
            if path.suffix.lower() not in SCANNED_TEXT_SUFFIXES:
                continue
            try:
                content = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for pattern in LEGACY_CONTENT_PATTERNS:
                if pattern in content:
                    failures.append(
                        "retired content marker exists: "
                        f"{path.relative_to(root).as_posix()}: {pattern}"
                    )

    old_modules = sorted(LEGACY_MODULE_IDS.intersection(policy.modules))
    if old_modules:
        failures.append("retired manifest module ids exist: " + ", ".join(old_modules))

    old_targets = sorted(LEGACY_TARGETS.intersection(policy.target_to_module))
    if old_targets:
        failures.append("retired target identities exist: " + ", ".join(old_targets))

    if failures:
        print("Legacy architecture residue failures:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("Legacy architecture residue check passed.")
    print(
        f"Validated {len(LEGACY_ROOTS)} retired roots, {len(policy.targets)} targets, "
        f"and {len(LEGACY_CONTENT_PATTERNS)} retired content markers."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
