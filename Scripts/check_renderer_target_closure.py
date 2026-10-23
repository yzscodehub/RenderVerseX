#!/usr/bin/env python3
"""Validate the concrete renderer target split and runtime-only closure."""

from __future__ import annotations

import argparse
from collections import deque

from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import Policy, add_policy_arguments, load_policy, unwrap_cmake_value


RENDER_TARGETS = {
    "RVX_RenderRuntimeCore": "RVX::RenderRuntimeCore",
    "RVX_RenderGraph": "RVX::RenderGraph",
    "RVX_RenderFoundation": "RVX::RenderFoundation",
    "RVX_RenderSurface": "RVX::RenderSurface",
    "RVX_RenderLighting": "RVX::RenderLighting",
    "RVX_RenderAnimationDeform": "RVX::RenderAnimationDeform",
    "RVX_RenderPostProcess": "RVX::RenderPostProcess",
    "RVX_RenderParticles": "RVX::RenderParticles",
    "RVX_RenderWater": "RVX::RenderWater",
    "RVX_RenderRayTracing": "RVX::RenderRayTracing",
    "RVX_RenderComposition": "RVX::RenderComposition",
    "RVX_Render": "RVX::Render",
}

FEATURE_TARGETS = (
    "RVX_RenderSurface",
    "RVX_RenderLighting",
    "RVX_RenderAnimationDeform",
    "RVX_RenderPostProcess",
    "RVX_RenderParticles",
    "RVX_RenderWater",
    "RVX_RenderRayTracing",
)

FEATURE_ALLOWED_PROJECT_DEPENDENCIES = frozenset({
    "RVX_RenderGraph",
    "RVX_RenderFoundation",
})


def _target_maps(policy: Policy):
    by_name = {target.name: target for target in policy.targets}
    references: dict[str, str] = {}
    for target in policy.targets:
        references[target.name] = target.name
        for alias in target.aliases:
            references[alias] = target.name
    return by_name, references


def _direct_project_dependencies(target, references: dict[str, str]) -> set[str]:
    dependencies: set[str] = set()
    for value in (*target.link_libraries, *target.interface_link_libraries):
        for atom in unwrap_cmake_value(value):
            canonical = references.get(atom)
            if canonical is not None:
                dependencies.add(canonical)
    return dependencies


def validate_renderer_target_closure(policy: Policy) -> list[str]:
    """Return deterministic findings for an invalid renderer target graph."""
    findings: list[str] = []
    by_name, references = _target_maps(policy)

    required = set(RENDER_TARGETS)
    for name, required_alias in RENDER_TARGETS.items():
        target = by_name.get(name)
        if target is None:
            findings.append(f"missing renderer target {name}")
            continue
        if target.module != "Render":
            findings.append(f"{name}: must be registered to Render, got {target.module}")
        if target.type != "STATIC_LIBRARY" or target.infrastructure:
            findings.append(
                f"{name}: must be a concrete STATIC_LIBRARY, got {target.type}"
            )
        if required_alias not in target.aliases:
            findings.append(f"{name}: missing required alias {required_alias}")
        if not target.sources:
            findings.append(f"{name}: concrete renderer target has no sources")

    if any(name not in by_name for name in required):
        return sorted(set(findings))

    direct = {
        name: _direct_project_dependencies(by_name[name], references)
        for name in by_name
    }

    for name in FEATURE_TARGETS:
        illegal = sorted(direct[name] - FEATURE_ALLOWED_PROJECT_DEPENDENCIES)
        for dependency in illegal:
            findings.append(
                f"{name}: feature target has illegal direct dependency {dependency}"
            )
        if "RVX_RenderFoundation" not in direct[name]:
            findings.append(f"{name}: feature target must depend on RVX_RenderFoundation")
        if "RVX_RenderGraph" not in direct[name]:
            findings.append(f"{name}: feature target must depend on RVX_RenderGraph")

    composition_dependencies = direct["RVX_RenderComposition"]
    for feature in FEATURE_TARGETS:
        if feature not in composition_dependencies:
            findings.append(
                f"RVX_RenderComposition: missing direct feature dependency {feature}"
            )

    render_dependencies = direct["RVX_Render"]
    if "RVX_RenderComposition" not in render_dependencies:
        findings.append("RVX_Render: thin facade must depend on RVX_RenderComposition")
    direct_features_from_facade = sorted(render_dependencies.intersection(FEATURE_TARGETS))
    for feature in direct_features_from_facade:
        findings.append(f"RVX_Render: facade must not aggregate feature target {feature}")
    if len(by_name["RVX_Render"].sources) > 3:
        findings.append(
            "RVX_Render: facade owns more than three sources and is no longer thin"
        )

    render_header = (
        policy.root / policy.modules["Render"].path / "Include/Render/Render.h"
    )
    if render_header.is_file():
        text = render_header.read_text(encoding="utf-8")
        forbidden_facade_includes = (
            "Render/Context/",
            "Render/Features/",
            "Render/Graph/",
            "Render/Lighting/",
            "Render/Passes/",
            "Render/PostProcess/",
            "Render/RayTracing/",
            "Render/Renderer/",
        )
        for include_prefix in forbidden_facade_includes:
            if f'#include "{include_prefix}' in text:
                findings.append(
                    f"Render/Render.h: thin facade exposes {include_prefix} internals"
                )

    engine = by_name.get("RVX_Engine")
    if engine is None:
        findings.append("missing Engine target RVX_Engine")
    else:
        engine_dependencies = _direct_project_dependencies(engine, references)
        for dependency in ("RVX_Render", "RVX_RenderComposition"):
            if dependency not in engine_dependencies:
                findings.append(
                    f"RVX_Engine: missing direct renderer dependency {dependency}"
                )

    renderer_source_owner: dict[str, str] = {}
    for name in RENDER_TARGETS:
        for source in by_name[name].sources:
            owner = renderer_source_owner.get(source)
            if owner is not None:
                findings.append(
                    f"renderer source {source} is owned by both {owner} and {name}"
                )
            else:
                renderer_source_owner[source] = name

    graph = {
        name: _direct_project_dependencies(target, references)
        for name, target in by_name.items()
    }
    for root in ("RVX_Render", "RVX_RenderComposition"):
        queue = deque([(root, (root,))])
        visited = {root}
        while queue:
            current, path = queue.popleft()
            for destination in sorted(graph.get(current, ())):
                next_path = (*path, destination)
                destination_target = by_name[destination]
                destination_module = policy.modules[destination_target.module]
                if (destination_module.domain != "runtime" or
                        not destination_module.shipping_allowed):
                    findings.append(
                        f"{root}: renderer closure reaches non-shipping target "
                        f"{destination} via {' -> '.join(next_path)}"
                    )
                    continue
                if destination not in visited:
                    visited.add(destination)
                    queue.append((destination, next_path))

    feature_set = set(FEATURE_TARGETS)
    for name, target in by_name.items():
        if target.module != "Render" or name == "RVX_RenderComposition":
            continue
        aggregated = sorted(direct[name].intersection(feature_set))
        if len(aggregated) > 1:
            findings.append(
                f"{name}: only RVX_RenderComposition may aggregate feature targets: "
                + ", ".join(aggregated)
            )

    return sorted(set(findings))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings = validate_renderer_target_closure(policy)
    if findings:
        print("Renderer target closure violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Renderer target closure passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
