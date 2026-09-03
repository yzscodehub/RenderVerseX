#!/usr/bin/env python3
"""Validate the strict cooked-only GFX24/GFX25 Sample product closure."""

from __future__ import annotations

import argparse
from collections import deque

from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import Policy, add_policy_arguments, load_policy, unwrap_cmake_value


PRODUCT_TARGET = "RenderVerseSamplesCooked"
COOKED_COMPOSITION = "RVX_SampleCookedComposition"
DEVELOPMENT_COMPOSITION = "RVX_SampleDevelopmentComposition"
REQUIRED_SCENARIOS = frozenset({
    "RVX_Gfx24SampleScenarios",
    "RVX_Gfx25SampleScenarios",
})
FORBIDDEN = frozenset({
    "RVX_AssetImport",
    "RVX_ShaderCompiler",
    "RVX_EngineDevelopmentComposition",
    DEVELOPMENT_COMPOSITION,
})


def _target_maps(policy: Policy):
    by_name = {target.name: target for target in policy.targets}
    references: dict[str, str] = {}
    for target in policy.targets:
        references[target.name] = target.name
        for alias in target.aliases:
            references[alias] = target.name
    return by_name, references


def _project_dependencies(target, references: dict[str, str]) -> set[str]:
    dependencies: set[str] = set()
    for value in (*target.link_libraries, *target.interface_link_libraries):
        for atom in unwrap_cmake_value(value):
            canonical = references.get(atom)
            if canonical is not None:
                dependencies.add(canonical)
    return dependencies


def validate_sample_cooked_product_closure(policy: Policy) -> list[str]:
    """Return deterministic findings for a non-cooked Sample product graph."""
    findings: list[str] = []
    by_name, references = _target_maps(policy)
    product = by_name.get(PRODUCT_TARGET)
    if product is None:
        return [f"missing cooked Sample product target {PRODUCT_TARGET}"]
    if product.type != "EXECUTABLE":
        findings.append(f"{PRODUCT_TARGET}: must be an EXECUTABLE, got {product.type}")
    if product.module != "Samples":
        findings.append(
            f"{PRODUCT_TARGET}: must be registered to Samples, got {product.module}"
        )

    graph = {
        name: _project_dependencies(target, references)
        for name, target in by_name.items()
    }
    queue = deque([(PRODUCT_TARGET, (PRODUCT_TARGET,))])
    visited: set[str] = set()
    reached: set[str] = set()
    while queue:
        current, path = queue.popleft()
        if current in visited:
            continue
        visited.add(current)
        reached.add(current)
        if current in FORBIDDEN:
            findings.append(
                f"{PRODUCT_TARGET}: cooked closure reaches forbidden {current} via "
                + " -> ".join(path)
            )
            continue
        target = by_name.get(current)
        if target is not None:
            module = policy.modules.get(target.module)
            if module is None:
                findings.append(
                    f"{PRODUCT_TARGET}: cooked closure reaches target {current} "
                    f"with unknown module {target.module}"
                )
            elif module.domain in {"developer", "editor"}:
                findings.append(
                    f"{PRODUCT_TARGET}: cooked closure reaches {module.domain} "
                    f"module target {current} via " + " -> ".join(path)
                )
        for destination in sorted(graph.get(current, ())):
            queue.append((destination, (*path, destination)))

    if COOKED_COMPOSITION not in reached:
        findings.append(
            f"{PRODUCT_TARGET}: must reach {COOKED_COMPOSITION} exactly through its link closure"
        )
    for scenario in sorted(REQUIRED_SCENARIOS):
        if scenario not in reached:
            findings.append(
                f"{PRODUCT_TARGET}: must reach required {scenario} scenario target"
            )
    if DEVELOPMENT_COMPOSITION in reached and COOKED_COMPOSITION in reached:
        findings.append(
            f"{PRODUCT_TARGET}: both development and cooked Sample compositions are reachable"
        )

    common = by_name.get("RVX_SampleCommon")
    if common is None:
        findings.append("missing shared Sample target RVX_SampleCommon")
    else:
        direct_common = _project_dependencies(common, references)
        forbidden_common = sorted(direct_common.intersection(FORBIDDEN))
        for dependency in forbidden_common:
            findings.append(
                f"RVX_SampleCommon: shared target directly links forbidden {dependency}"
            )

    runner_source = policy.root / "Samples/Common/Private/SampleRunner.cpp"
    environment_source = policy.root / "Samples/Common/Private/SampleEnvironmentLoader.cpp"
    for source in (runner_source, environment_source):
        if not source.is_file():
            findings.append(f"missing Sample source contract file {source}")
            continue
        text = source.read_text(encoding="utf-8")
        for forbidden_include in (
            '#include "AssetImport/',
            '#include "ShaderCompiler/',
            '#include "EngineDevelopmentComposition/',
        ):
            if forbidden_include in text:
                findings.append(
                    f"{source.relative_to(policy.root)}: directly includes {forbidden_include}"
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
    findings = validate_sample_cooked_product_closure(policy)
    if findings:
        print("Cooked Sample product closure violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Cooked Sample product closure passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
