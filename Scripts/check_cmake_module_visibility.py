#!/usr/bin/env python3
"""Validate target ownership and source visibility from the generated inventory."""
from __future__ import annotations
import argparse
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, edge_allowed, load_policy, resolve_target_source


def is_allowed_test_sample_source(policy, target, source_module: str, path) -> bool:
    """Tests may compile public sample scenarios, never runtime/private sources."""
    if target.module != "Tests" or source_module != "Samples":
        return False
    if not edge_allowed(policy, target.module, source_module):
        return False
    return not any(part.casefold() == "private" for part in path.parts)

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings: list[str] = []
    for target in policy.targets:
        if target.infrastructure:
            continue
        if target.module not in policy.modules:
            findings.append(f"{target.name}: target has no manifest module")
            continue
        owner_path = policy.modules[target.module].path
        for source in target.sources:
            path = resolve_target_source(policy, target, source)
            if path is None:
                findings.append(f"{target.name}: source cannot be resolved from inventory: {source}")
                continue
            source_module = policy.module_for_path(path)
            if source_module is not None and source_module != target.module:
                if not is_allowed_test_sample_source(policy, target, source_module, path):
                    findings.append(f"{target.name}: source belongs to {source_module}, not {target.module}: {source}")
            elif source_module is None:
                owner_root = (policy.root / owner_path).resolve()
                binary_root = policy.binary_root.resolve()
                if not path.resolve().is_relative_to(owner_root) and not path.resolve().is_relative_to(binary_root):
                    findings.append(f"{target.name}: source is outside manifest module path or binary tree: {source}")
    if findings:
        print("CMake module visibility violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("CMake module visibility passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
