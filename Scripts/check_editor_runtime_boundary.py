#!/usr/bin/env python3
"""Validate the manifest/inventory Editor and Runtime deferred boundary."""
from __future__ import annotations
import argparse
import re
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, load_policy, resolve_target_reference

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings: list[str] = []
    editor = policy.modules.get("Editor")
    if editor is None or not editor.deferred or editor.shipping_allowed:
        findings.append("manifest Editor must be deferred and shipping-ineligible")
    for target in policy.targets:
        if target.module == "RuntimeServices":
            for value in (*target.link_libraries, *target.interface_link_libraries):
                destination, unknown = resolve_target_reference(policy, value)
                findings.extend(f"{target.name}: unknown project dependency {name}" for name in unknown)
                if destination == "Editor":
                    findings.append(f"{target.name}: Runtime must not link Editor")
    for module_id in ("RuntimeServices", "Editor"):
        if policy.modules[module_id].deferred:
            continue
        for path in policy.source_files([module_id]):
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError:
                lines = path.read_text(encoding="utf-8-sig").splitlines()
            for number, line in enumerate(lines, 1):
                match = INCLUDE_RE.match(line)
                if not match:
                    continue
                destination = policy.module_for_prefix(match.group(1).replace("\\", "/").split("/", 1)[0])
                if destination in {"RuntimeServices", "Editor"} and destination != module_id:
                    findings.append(f"{path.relative_to(policy.root)}:{number}: {module_id} must not include {destination}")
    if findings:
        print("Editor/runtime boundary violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Editor/runtime shared-core boundary passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
