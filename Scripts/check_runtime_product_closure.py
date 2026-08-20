#!/usr/bin/env python3
"""Fail closed when a runtime/shipping target can reach non-runtime code."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque

from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import (
    Policy,
    add_policy_arguments,
    load_policy,
    resolve_target_reference,
)


def validate_runtime_product_closure(policy: Policy) -> list[str]:
    findings: list[str] = []
    runtime_modules = {
        module.id for module in policy.modules.values()
        if module.domain == "runtime"
    }

    for module in policy.modules.values():
        if module.shipping_allowed and module.domain != "runtime":
            findings.append(
                f"{module.id}: shippingAllowed requires runtime domain, got {module.domain}"
            )
        if module.domain != "runtime":
            continue
        if not module.shipping_allowed:
            findings.append(f"{module.id}: runtime module is not shippingAllowed")
        for dependency in sorted(module.allowed_modules):
            destination = policy.modules[dependency]
            if destination.domain != "runtime" or not destination.shipping_allowed:
                findings.append(
                    f"{module.id}: runtime policy allows non-shipping module {dependency}"
                )

    graph: dict[str, set[str]] = defaultdict(set)
    for target in policy.targets:
        if not target.module:
            continue
        for value in (*target.link_libraries, *target.interface_link_libraries):
            destination, unknown = resolve_target_reference(policy, value)
            findings.extend(
                f"{target.name}: unknown project target dependency {name}"
                for name in unknown
            )
            if destination is not None:
                graph[target.module].add(destination)

    for root in sorted(runtime_modules):
        queue = deque([(root, (root,))])
        visited = {root}
        while queue:
            current, path = queue.popleft()
            for destination in sorted(graph.get(current, ())):
                next_path = (*path, destination)
                module = policy.modules[destination]
                if module.domain != "runtime" or not module.shipping_allowed:
                    findings.append(
                        f"{root}: runtime closure reaches {destination} via "
                        + " -> ".join(next_path)
                    )
                    continue
                if destination not in visited:
                    visited.add(destination)
                    queue.append((destination, next_path))

    return sorted(set(findings))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings = validate_runtime_product_closure(policy)
    if findings:
        print("Runtime product closure violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Runtime product closure passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
