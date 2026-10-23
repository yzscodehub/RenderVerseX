#!/usr/bin/env python3
"""Validate manifest v2 and generated module-target inventory coverage."""
from __future__ import annotations
import argparse
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, load_policy

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    active_modules = {
        target.module for target in policy.targets
        if target.module and not target.infrastructure
    }
    missing = sorted(module_id for module_id, module in policy.modules.items()
                     if not module.deferred and module_id not in active_modules
                     and module_id not in policy.disabled_modules)
    if missing:
        print("Module boundary manifest failures:")
        print("  manifest modules are neither active targets nor disabled: " + ", ".join(missing))
        return 1
    print("Module boundary manifest passed.")
    print(f"Validated {len(policy.modules)} manifest modules and {len(policy.targets)} concrete targets.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
