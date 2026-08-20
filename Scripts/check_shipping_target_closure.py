#!/usr/bin/env python3
"""Validate the concrete Shipping product/consumer target closure.

Unlike the module-wide runtime policy gate, this gate starts from the two
configured Shipping roots and follows the actual CMake target inventory.  The
consumer target is the only permitted tests-domain node; every dependency
below it, and every node below the product root, must be shipping-eligible
runtime code.
"""

from __future__ import annotations

import argparse
import re
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from architecture_gate_common import resolve_repo_root, run_gate
from module_policy import (
    PROJECT_REFERENCE,
    Policy,
    Target,
    add_policy_arguments,
    load_policy,
    resolve_target_source,
    unwrap_cmake_value,
)


DEFAULT_PRODUCT_TARGET = "RVXShippingProduct"
DEFAULT_CONSUMER_TARGET = "RVXShippingConsumer"

# These are legacy source-side implementations whose names do not describe
# their domain.  Cooked variants must live in distinct translation units so a
# static linker cannot pull a source parser into the product as a side effect.
LEGACY_SOURCE_IMPLEMENTATIONS = frozenset({
    "audioloader",
    "environmentloader",
    "fbximporter",
    "gltfimporter",
    "hdrtextureloader",
    "modelloader",
    "sourceresourceloaders",
    "textureloader",
    "ufbx",
})

# Static/header-only parsers do not necessarily contribute a source file to a
# first-party target.  Their CMake declaration is therefore part of the
# Shipping closure evidence too.
DECLARATION_PARSER_TOKENS = frozenset({
    "assimp",
    "miniz",
    "stb_image",
    "tinyexr",
    "tinygltf",
    "ufbx",
})


@dataclass(frozen=True)
class TargetGraph:
    canonical: dict[str, Target]
    names: dict[str, str]


def build_target_graph(policy: Policy) -> TargetGraph:
    canonical = {target.name: target for target in policy.targets}
    names: dict[str, str] = {}
    for target in policy.targets:
        for name in (target.name, *target.aliases):
            names[name] = target.name
    return TargetGraph(canonical=canonical, names=names)


def resolve_target_dependencies(
    graph: TargetGraph,
    target: Target,
) -> tuple[set[str], set[str]]:
    """Return every possible first-party target edge and unresolved name.

    Inventory values may contain LINK_ONLY or conditional generator
    expressions.  Treat every first-party name present in such an expression
    as reachable; under-approximating a Shipping closure is unsafe.
    """

    dependencies: set[str] = set()
    unknown: set[str] = set()
    for raw in (*target.link_libraries, *target.interface_link_libraries):
        normalized = raw.strip().strip('"').replace("__RVX_GENEX_OPEN__", "$<")
        candidates = [*unwrap_cmake_value(raw), *PROJECT_REFERENCE.findall(normalized)]
        for candidate in dict.fromkeys(candidates):
            canonical = graph.names.get(candidate)
            if canonical is not None:
                dependencies.add(canonical)
                continue
            for project_name in PROJECT_REFERENCE.findall(candidate):
                if project_name not in graph.names:
                    unknown.add(project_name)
    return dependencies, unknown


def forbidden_source_reason(path: Path) -> str | None:
    folded_parts = tuple(part.casefold() for part in path.parts)
    if any(part in {"importer", "importers", "parser", "parsers"}
           for part in folded_parts):
        return "source path is owned by an importer/parser directory"

    stem = path.stem.casefold()
    if stem.startswith("cooked"):
        return None
    if stem in LEGACY_SOURCE_IMPLEMENTATIONS:
        return "legacy source loader/importer must not share a Shipping object file"
    if "importer" in stem or "parser" in stem:
        return "source name identifies an importer/parser"
    original_stem = path.stem
    has_source_token = (
        original_stem.startswith("Source")
        or re.search(r"Source(?:[A-Z_]|$)", original_stem) is not None
        or re.search(r"(?:^|[_-])source(?:[_-]|$)", original_stem, re.IGNORECASE)
        is not None
    )
    if has_source_token and ("loader" in stem or "monitor" in stem):
        return "source name identifies a source loader/change monitor"
    return None


def _contains_token(value: str, token: str) -> bool:
    return re.search(
        rf"(?<![a-z0-9_]){re.escape(token.casefold())}(?![a-z0-9_])",
        value.casefold().replace("\\", "/"),
    ) is not None


def _developer_tokens(policy: Policy) -> frozenset[str]:
    tokens: set[str] = set()
    for module in policy.modules.values():
        if module.domain not in {"developer", "editor"}:
            continue
        tokens.add(module.id.casefold())
        tokens.update(prefix.casefold() for prefix in module.public_include_prefixes)
    return frozenset(token for token in tokens if token)


def _declared_dependency_findings(
    policy: Policy,
    target: Target,
) -> list[str]:
    findings: list[str] = []
    developer_tokens = _developer_tokens(policy)
    link_items = (*target.link_libraries, *target.interface_link_libraries)
    for raw in link_items:
        for token in sorted(DECLARATION_PARSER_TOKENS):
            if _contains_token(raw, token):
                findings.append(
                    f"{target.name}: forbidden Shipping link declaration contains parser token {token}: {raw}"
                )
        for token in sorted(developer_tokens):
            if _contains_token(raw, token):
                findings.append(
                    f"{target.name}: forbidden Shipping link declaration contains developer module token {token}: {raw}"
                )

    include_items = (
        *target.include_directories,
        *target.interface_include_directories,
    )
    binary_root = policy.binary_root.resolve()
    for raw in include_items:
        for token in sorted(DECLARATION_PARSER_TOKENS):
            if _contains_token(raw, token):
                findings.append(
                    f"{target.name}: forbidden Shipping include declaration contains parser token {token}: {raw}"
                )
        if not any(_contains_token(raw, token) for token in developer_tokens):
            continue
        values = unwrap_cmake_value(raw) or [raw]
        paths_in_binary_root = True
        for value in values:
            candidate = Path(value)
            if not candidate.is_absolute():
                candidate = Path(target.source_directory) / candidate
            try:
                candidate.resolve().relative_to(binary_root)
            except ValueError:
                paths_in_binary_root = False
                break
        if paths_in_binary_root:
            continue
        matched = sorted(
            token for token in developer_tokens if _contains_token(raw, token)
        )
        findings.append(
            f"{target.name}: forbidden Shipping include declaration contains developer module token "
            + ", ".join(matched)
            + f": {raw}"
        )
    return findings


def _walk_closure(
    graph: TargetGraph,
    root: str,
) -> tuple[set[str], dict[str, tuple[str, ...]], list[str]]:
    visited: set[str] = set()
    paths: dict[str, tuple[str, ...]] = {}
    findings: list[str] = []
    queue = deque([(root, (root,))])
    while queue:
        current, path = queue.popleft()
        if current in visited:
            continue
        visited.add(current)
        paths[current] = path
        target = graph.canonical[current]
        dependencies, unknown = resolve_target_dependencies(graph, target)
        findings.extend(
            f"{current}: unresolved first-party target dependency {name}"
            for name in sorted(unknown)
        )
        for dependency in sorted(dependencies):
            if dependency not in visited:
                queue.append((dependency, (*path, dependency)))
    return visited, paths, findings


def validate_shipping_target_closure(
    policy: Policy,
    *,
    product_target: str = DEFAULT_PRODUCT_TARGET,
    consumer_target: str = DEFAULT_CONSUMER_TARGET,
) -> list[str]:
    findings: list[str] = []
    graph = build_target_graph(policy)

    resolved_roots: dict[str, str] = {}
    for role, requested in (("product", product_target), ("consumer", consumer_target)):
        canonical = graph.names.get(requested)
        if canonical is None:
            findings.append(f"required Shipping {role} target is missing: {requested}")
        else:
            resolved_roots[role] = canonical
    if findings:
        return sorted(set(findings))

    product = resolved_roots["product"]
    consumer = resolved_roots["consumer"]
    if graph.canonical[consumer].type != "EXECUTABLE":
        findings.append(
            f"{consumer}: Shipping consumer must be an EXECUTABLE, got "
            f"{graph.canonical[consumer].type}"
        )

    direct_consumer_dependencies, consumer_unknown = resolve_target_dependencies(
        graph, graph.canonical[consumer]
    )
    findings.extend(
        f"{consumer}: unresolved first-party target dependency {name}"
        for name in sorted(consumer_unknown)
    )
    if product not in direct_consumer_dependencies:
        findings.append(f"{consumer}: must link the Shipping product target {product}")
    unexpected_direct = direct_consumer_dependencies - {product}
    if unexpected_direct:
        findings.append(
            f"{consumer}: has unexpected direct first-party dependencies: "
            + ", ".join(sorted(unexpected_direct))
        )

    closures: dict[str, tuple[set[str], dict[str, tuple[str, ...]]]] = {}
    for role, root in (("product", product), ("consumer", consumer)):
        reachable, paths, walk_findings = _walk_closure(graph, root)
        findings.extend(walk_findings)
        closures[role] = (reachable, paths)

        for target_name in sorted(reachable):
            target = graph.canonical[target_name]
            path = paths[target_name]
            module = policy.modules.get(target.module)
            is_consumer_root = role == "consumer" and target_name == consumer
            if module is None:
                findings.append(
                    f"{role} closure target {target_name} has no Manifest module via "
                    + " -> ".join(path)
                )
            elif not is_consumer_root and (
                module.domain != "runtime" or not module.shipping_allowed
            ):
                findings.append(
                    f"{role} closure reaches non-shipping module {module.id} "
                    f"(domain={module.domain}, shippingAllowed={module.shipping_allowed}) via "
                    + " -> ".join(path)
                )

            for source in target.sources:
                resolved = resolve_target_source(policy, target, source)
                if resolved is None:
                    findings.append(
                        f"{target_name}: Shipping-reachable source cannot be resolved: {source}"
                    )
                    continue
                reason = forbidden_source_reason(resolved)
                if reason is not None:
                    try:
                        display = resolved.resolve().relative_to(policy.root).as_posix()
                    except ValueError:
                        display = resolved.as_posix()
                    findings.append(
                        f"{target_name}: forbidden Shipping source {display}: {reason}"
                    )
            findings.extend(_declared_dependency_findings(policy, target))

    if consumer in closures["product"][0]:
        findings.append(f"{product}: product closure must not depend on consumer {consumer}")

    return sorted(set(findings))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--product-target", default=DEFAULT_PRODUCT_TARGET)
    parser.add_argument("--consumer-target", default=DEFAULT_CONSUMER_TARGET)
    add_policy_arguments(parser)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    policy = load_policy(resolve_repo_root(args.root), args)
    findings = validate_shipping_target_closure(
        policy,
        product_target=args.product_target,
        consumer_target=args.consumer_target,
    )
    if findings:
        print("Shipping target closure violations:")
        for finding in findings:
            print("  " + finding)
        return 1
    print("Shipping target closure passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gate(main))
