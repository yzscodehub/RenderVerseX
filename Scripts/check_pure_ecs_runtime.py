#!/usr/bin/env python3
"""Enforce the pure-ECS Runtime cutover from manifest/inventory inputs."""
from __future__ import annotations
import argparse
import re
from architecture_gate_common import GateInputError, resolve_repo_root, run_gate
from module_policy import add_policy_arguments, load_policy

FORBIDDEN_IDENTIFIERS = ("Actor", "ActorComponent", "ActorFactory", "AnimationPhysicsBridgeSubsystem", "AudioComponent", "CameraComponent", "ColliderComponent", "ComponentFactory", "LightComponent", "MeshRendererComponent", "MeshComponent", "BoneComponent", "NodeComponent", "ParticleComponent", "PhysicsSubsystem", "PrimitiveComponent", "RigidBodyComponent", "SceneComponent", "SceneEntity", "SceneManager", "ScriptComponent", "SkeletonComponent", "SpatialSubsystem", "StaticMeshComponent", "TerrainComponent", "WaterComponent")
FORBIDDEN_INCLUDE_PARTS = ("Scene/Actor", "Scene/Component", "Scene/SceneEntity", "Scene/SceneManager", "Scene/SceneRuntime.h", "Scene/SceneComponent", "Scene/PrimitiveComponent", "Spatial/", "Picking/", "Scripting/ScriptComponent")
FORBIDDEN_PATHS = ("Picking", "Spatial", "Samples/Basic", "Samples/Showcase", "Engine/Private/RenderRuntimeComposition.cpp", "Engine/Private/RenderRuntimeComposition.h", "Source/Runtime/Scene/Include/Scene/Actor.h", "Source/Runtime/Scene/Include/Scene/ActorComponent.h", "Source/Runtime/Scene/Include/Scene/Component.h", "Source/Runtime/Scene/Include/Scene/SceneEntity.h", "Source/Runtime/Scene/Include/Scene/SceneManager.h", "Source/Runtime/Scripting/Include/Scripting/ScriptComponent.h")
REQUIRED_PATHS = ("Source/Runtime/ECS/Include/ECS/Registry.h", "Source/Runtime/Scene/Include/Scene/ECS/SceneEcsRuntime.h", "Source/Runtime/Scene/Include/Scene/ECS/FrozenSceneSnapshot.h", "World/Include/World/World.h", "Engine/Private/ECS/WorldEcsRuntimeComposition.cpp", "Engine/Private/ECS/EcsRenderRuntimeComposition.cpp", "Samples/RenderVerseSamples/main.cpp")
REQUIRED_TEST_TARGETS = ("EcsEntityValidation", "EcsFragmentQueryValidation", "EcsCommandTransactionValidation", "EcsSchedulerValidation", "EcsTransformHierarchyValidation", "EcsCleanupValidation", "EcsChangeTrackingValidation")

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def text(path):
    try: return path.read_text(encoding="utf-8")
    except UnicodeDecodeError: return path.read_text(encoding="utf-8-sig")


def is_material_legacy_path(path) -> bool:
    """Empty ignored directory scaffolding is not a live legacy subsystem."""
    if path.is_file():
        return True
    return path.is_dir() and any(candidate.is_file() for candidate in path.rglob("*"))

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings: list[str] = []
    for relative in REQUIRED_PATHS:
        if not (policy.root / relative).is_file():
            findings.append(f"{relative}: required pure-ECS Runtime file is missing")
    for relative in FORBIDDEN_PATHS:
        if is_material_legacy_path(policy.root / relative):
            findings.append(f"{relative}: legacy Runtime path still exists")
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
    code_pattern = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", re.DOTALL)
    production = [
        module.id for module in policy.modules.values()
        if module.domain in {"runtime", "samples"} and not module.deferred
    ]
    production_sources = policy.source_files(production)
    if not production_sources:
        raise GateInputError("manifest production source scan is empty")
    for path in production_sources:
        source = text(path)
        for match in include_pattern.finditer(source):
            included = match.group(1)
            if included.startswith(FORBIDDEN_INCLUDE_PARTS):
                findings.append(f"{path.relative_to(policy.root)}:{source.count(chr(10), 0, match.start()) + 1}: legacy Runtime include is forbidden: {included}")
        code = code_pattern.sub(lambda match: "\n" * match.group(0).count("\n"), source)
        for identifier in FORBIDDEN_IDENTIFIERS:
            match = re.search(rf"\b{re.escape(identifier)}\b", code)
            if match:
                findings.append(f"{path.relative_to(policy.root)}:{code.count(chr(10), 0, match.start()) + 1}: legacy Runtime identifier is forbidden: {identifier}")
    known_targets = {name for target in policy.targets for name in (target.name, *target.aliases)}
    for target in REQUIRED_TEST_TARGETS:
        if target not in known_targets:
            findings.append(f"inventory: required ECS validation target is missing: {target}")
    if not any(target.module == "Samples" and not target.infrastructure for target in policy.targets):
        findings.append("inventory: RenderVerseSamples product target is missing")
    if findings:
        print(f"Pure ECS Runtime gate failed with {len(findings)} finding(s):")
        for finding in findings[:100]: print("  " + finding)
        return 1
    print("Pure ECS Runtime gate passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
