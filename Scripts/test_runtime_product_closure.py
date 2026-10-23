#!/usr/bin/env python3
"""Self-tests for the runtime/shipping closure gate."""

from __future__ import annotations

import unittest
from pathlib import Path

from check_runtime_product_closure import validate_runtime_product_closure
from module_policy import Module, Policy, Target


def make_module(
    module_id: str,
    domain: str,
    *,
    allowed: tuple[str, ...] = (),
    shipping: bool = True,
) -> Module:
    return Module(
        id=module_id,
        path=Path(module_id),
        domain=domain,
        layer="feature",
        public_include_prefixes=(module_id,),
        allowed_modules=frozenset((module_id, *allowed)),
        shipping_allowed=shipping,
        deferred=False,
        owner="test",
    )


def make_target(name: str, module: str, links: tuple[str, ...] = ()) -> Target:
    return Target(
        name=name,
        aliases=(f"RVX::{name}",),
        module=module,
        type="STATIC_LIBRARY",
        source_directory=".",
        binary_directory="build",
        infrastructure=False,
        sources=(),
        link_libraries=links,
        interface_link_libraries=(),
        include_directories=(),
        interface_include_directories=(),
    )


def make_policy(modules: tuple[Module, ...], targets: tuple[Target, ...]) -> Policy:
    module_map = {module.id: module for module in modules}
    target_map: dict[str, str] = {}
    for target in targets:
        target_map[target.name] = target.module
        target_map.update({alias: target.module for alias in target.aliases})
    return Policy(
        root=Path(".").resolve(),
        manifest_path=Path("manifest.json"),
        inventory_path=Path("inventory.json"),
        modules=module_map,
        prefix_to_module={module.id.casefold(): module.id for module in modules},
        targets=targets,
        target_to_module=target_map,
        disabled_modules=frozenset(),
        source_root=Path(".").resolve(),
        binary_root=Path("build").resolve(),
    )


class RuntimeProductClosureTests(unittest.TestCase):
    def test_accepts_runtime_only_closure(self) -> None:
        core = make_module("Core", "runtime", allowed=("Engine",))
        engine = make_module("Engine", "runtime", allowed=("Core",))
        policy = make_policy(
            (core, engine),
            (
                make_target("Core", "Core"),
                make_target("Engine", "Engine", ("RVX::Core",)),
            ),
        )
        self.assertEqual(validate_runtime_product_closure(policy), [])

    def test_rejects_runtime_policy_drift_to_developer(self) -> None:
        runtime = make_module("Runtime", "runtime", allowed=("Compiler",))
        compiler = make_module("Compiler", "developer", shipping=False)
        policy = make_policy(
            (runtime, compiler),
            (make_target("Runtime", "Runtime"), make_target("Compiler", "Compiler")),
        )
        findings = validate_runtime_product_closure(policy)
        self.assertTrue(any("allows non-shipping module Compiler" in item for item in findings))

    def test_rejects_transitive_developer_reachability(self) -> None:
        engine = make_module("Engine", "runtime", allowed=("Render",))
        render = make_module("Render", "runtime")
        compiler = make_module("Compiler", "developer", shipping=False)
        policy = make_policy(
            (engine, render, compiler),
            (
                make_target("Engine", "Engine", ("RVX::Render",)),
                make_target("Render", "Render", ("RVX::Compiler",)),
                make_target("Compiler", "Compiler"),
            ),
        )
        findings = validate_runtime_product_closure(policy)
        self.assertTrue(
            any("Engine -> Render -> Compiler" in item for item in findings),
            findings,
        )

    def test_rejects_non_shipping_runtime_module(self) -> None:
        runtime = make_module("Runtime", "runtime", shipping=False)
        policy = make_policy((runtime,), (make_target("Runtime", "Runtime"),))
        findings = validate_runtime_product_closure(policy)
        self.assertIn("Runtime: runtime module is not shippingAllowed", findings)


if __name__ == "__main__":
    unittest.main()
