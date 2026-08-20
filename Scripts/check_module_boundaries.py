#!/usr/bin/env python3
"""Validate project include edges using manifest prefixes and inventory sources."""
from __future__ import annotations
import argparse
import re
from pathlib import Path
from architecture_gate_common import GateInputError, resolve_repo_root, run_gate
from module_policy import add_policy_arguments, edge_allowed, load_policy, resolve_inventory_path

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[">]')


def module_include_roots(policy, module_id: str, *, public_only: bool) -> list[Path]:
    module = policy.modules[module_id]
    roots = [policy.root / module.path / "Include"]
    for target in policy.targets:
        if target.module != module_id:
            continue
        values = target.interface_include_directories
        if not public_only:
            values = (*target.include_directories, *values)
        for value in values:
            path = resolve_inventory_path(policy, value)
            if path is not None and path.is_dir():
                roots.append(path)
    return list(dict.fromkeys(root.resolve() for root in roots if root.is_dir()))


def exact_include_prefix_module(policy, prefix: str) -> str | None:
    for module in policy.modules.values():
        if prefix in module.public_include_prefixes:
            return module.id
    return None


def resolve_include_module(policy, source: str, including, include: str) -> tuple[str | None, str | None]:
    """Resolve project includes through real local/private/public candidates.

    A basename such as ``Runtime/Foo.h`` may be a local Render private include
    rather than the public Runtime module.  A prefix alone is therefore never
    considered authoritative; conflicting existing candidates are rejected.
    """
    module = policy.modules[source]
    include_path = Path(include)
    candidates = [including.parent / include_path, policy.root / module.path / "Private" / include_path]
    candidates.extend(root / include_path for root in module_include_roots(policy, source, public_only=False))
    parts = include_path.parts
    public_module = None
    if parts:
        public_module = exact_include_prefix_module(policy, parts[0])
        if public_module is not None:
            candidates.extend(
                root / include_path
                for root in module_include_roots(policy, public_module, public_only=True)
            )
    resolved_modules: set[str] = set()
    for candidate in candidates:
        if candidate.is_file():
            owner = policy.module_for_path(candidate)
            if owner is not None:
                resolved_modules.add(owner)
    if len(resolved_modules) > 1:
        return None, f"ambiguous project include: {include} resolves to {', '.join(sorted(resolved_modules))}"
    if not resolved_modules and public_module is not None:
        return None, f"unresolved project include: {include}"
    return (next(iter(resolved_modules)) if resolved_modules else None), None

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    add_policy_arguments(parser)
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings: list[str] = []
    count = 0
    production_modules = [
        module.id for module in policy.modules.values()
        if not module.deferred and module.domain != "tests"
    ]
    # Test sources are scoped to concrete registered targets.  This keeps
    # deferred/removed Editor test scaffolding out of the runtime policy scan
    # while still checking every actually configured test source.
    sources = sorted(set(policy.source_files(production_modules)) | set(policy.target_source_files(["Tests"])))
    if not sources:
        raise GateInputError("manifest source scan is empty")
    for path in sources:
        source = policy.module_for_path(path)
        if source is None:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            lines = path.read_text(encoding="utf-8-sig").splitlines()
        for line_number, line in enumerate(lines, 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            include = match.group(1).replace("\\", "/")
            destination, error = resolve_include_module(policy, source, path, include)
            if error is not None:
                findings.append(f"{path.relative_to(policy.root)}:{line_number}: {error}")
                continue
            if destination is None:
                continue
            count += 1
            if not edge_allowed(policy, source, destination):
                findings.append(f"{path.relative_to(policy.root)}:{line_number}: forbidden include {source}->{destination}: {include}")
    print(f"Scanned {len(sources)} manifest-owned source files and {count} project includes.")
    if findings:
        print("Module boundary violations:")
        for finding in findings[:100]:
            print("  " + finding)
        return 1
    print("Module boundaries passed.")
    return 0

if __name__ == "__main__":
    raise SystemExit(run_gate(main))
