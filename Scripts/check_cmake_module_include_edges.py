#!/usr/bin/env python3
"""Validate concrete CMake include-directory edges and public exposure."""
from __future__ import annotations
import argparse
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, edge_allowed, include_module, load_policy

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
        for entry in target.include_directories:
            destination = include_module(policy, entry)
            if destination in policy.disabled_modules:
                findings.append(f"{target.name}: includes disabled module {destination}: {entry}")
            elif destination and not edge_allowed(policy, target.module, destination):
                findings.append(f"{target.name}: forbidden include edge {target.module}->{destination}: {entry}")
        for entry in target.interface_include_directories:
            destination = include_module(policy, entry)
            if destination in policy.disabled_modules:
                findings.append(f"{target.name}: PUBLIC/INTERFACE exposes disabled module {destination}: {entry}")
            elif destination and destination != target.module:
                findings.append(f"{target.name}: PUBLIC/INTERFACE exposes {destination}: {entry}")
    if findings:
        print("CMake module include-directory boundary violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("CMake module include-directory edges passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
