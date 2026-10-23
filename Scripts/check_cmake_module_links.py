#!/usr/bin/env python3
"""Validate concrete CMake target links against manifest v2 policy."""
from __future__ import annotations
import argparse
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, edge_allowed, load_policy, resolve_target_reference

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
        if not target.module:
            continue
        for dependency in (*target.link_libraries, *target.interface_link_libraries):
            destination, unknown = resolve_target_reference(policy, dependency)
            findings.extend(f"{target.name}: unknown project target dependency {name}" for name in unknown)
            if destination is None:
                continue
            if destination in policy.disabled_modules:
                findings.append(f"{target.name}: links deferred/disabled module {destination}")
            elif not edge_allowed(policy, target.module, destination):
                findings.append(f"{target.name}: forbidden module link {target.module}->{destination}")
    if findings:
        print("CMake module link boundary violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("CMake module links passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
