#!/usr/bin/env python3
"""Validate public-header includes have PUBLIC/INTERFACE inventory links."""
from __future__ import annotations
import argparse
import re
from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import add_policy_arguments, load_policy, resolve_public_target_reference

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    public_edges: dict[str, set[str]] = {module: {module} for module in policy.modules}
    findings: list[str] = []
    for target in policy.targets:
        if target.infrastructure or not target.module:
            continue
        for value in target.interface_link_libraries:
            destination, unknown = resolve_public_target_reference(policy, value)
            findings.extend(f"{target.name}: unknown PUBLIC/INTERFACE project dependency {name}" for name in unknown)
            if destination:
                public_edges[target.module].add(destination)
    include_count = 0
    for source, path in policy.public_header_files(policy.active_module_ids()):
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            lines = path.read_text(encoding="utf-8-sig").splitlines()
        for line_number, line in enumerate(lines, 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = match.group(1).replace("\\", "/")
            destination = policy.module_for_prefix(include.split("/", 1)[0])
            if destination is None:
                continue
            include_count += 1
            if destination not in public_edges[source]:
                findings.append(f"{path.relative_to(policy.root)}:{line_number}: public include {source}->{destination} lacks PUBLIC/INTERFACE link: {include}")
    print(f"Scanned {include_count} public project include directives.")
    if findings:
        print("Public header linkage violations:")
        for finding in findings[:100]:
            print("  " + finding)
        return 1
    print("Public header linkage passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
