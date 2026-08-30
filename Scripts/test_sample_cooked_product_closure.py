#!/usr/bin/env python3
"""Self-tests for the cooked RenderQualityLab Sample product closure gate."""

from __future__ import annotations

import unittest
from dataclasses import replace
from pathlib import Path

from check_sample_cooked_product_closure import (
    COOKED_COMPOSITION,
    DEVELOPMENT_COMPOSITION,
    PRODUCT_TARGET,
    REQUIRED_SCENARIO,
    validate_sample_cooked_product_closure,
)
from module_policy import Module, Policy, Target


def module(module_id: str, domain: str = "runtime") -> Module:
    return Module(
        id=module_id,
        path=Path(module_id),
        domain=domain,
        layer="product",
        public_include_prefixes=(module_id,),
        allowed_modules=frozenset({module_id}),
        shipping_allowed=domain == "runtime",
        deferred=False,
        owner="test",
    )


def target(
    name: str,
    module_id: str,
    *,
    aliases: tuple[str, ...] = (),
    links: tuple[str, ...] = (),
    interface: tuple[str, ...] = (),
    target_type: str = "STATIC_LIBRARY",
) -> Target:
    return Target(
        name=name,
        aliases=aliases,
        module=module_id,
        type=target_type,
        source_directory=".",
        binary_directory="build",
        infrastructure=False,
        sources=(f"Private/{name}.cpp",),
        link_libraries=links,
        interface_link_libraries=interface,
        include_directories=(),
        interface_include_directories=(),
    )


def policy_with(targets: tuple[Target, ...]) -> Policy:
    modules = (
        module("Samples", "samples"),
        module("Engine"),
        module("Developer", "developer"),
    )
    target_to_module: dict[str, str] = {}
    for item in targets:
        target_to_module[item.name] = item.module
        target_to_module.update({alias: item.module for alias in item.aliases})
    return Policy(
        root=Path(__file__).resolve().parents[1],
        manifest_path=Path("manifest.json"),
        inventory_path=Path("inventory.json"),
        modules={item.id: item for item in modules},
        prefix_to_module={item.id.casefold(): item.id for item in modules},
        targets=targets,
        target_to_module=target_to_module,
        disabled_modules=frozenset(),
        source_root=Path(".").resolve(),
        binary_root=Path("build").resolve(),
    )


def valid_policy() -> Policy:
    return policy_with((
        target("RVX_SampleCommon", "Samples", aliases=("RVX::SampleCommon",)),
        target(
            COOKED_COMPOSITION,
            "Samples",
            aliases=("RVX::SampleCookedComposition",),
            links=("RVX::SampleCommon",),
        ),
        target(
            DEVELOPMENT_COMPOSITION,
            "Samples",
            aliases=("RVX::SampleDevelopmentComposition",),
        ),
        target(
            REQUIRED_SCENARIO,
            "Samples",
            aliases=("RVX::Gfx24SampleScenarios",),
        ),
        target(
            PRODUCT_TARGET,
            "Samples",
            links=("RVX::SampleCookedComposition", "RVX::Gfx24SampleScenarios"),
            target_type="EXECUTABLE",
        ),
    ))


def replace_target(policy: Policy, replacement: Target) -> Policy:
    targets = tuple(
        replacement if item.name == replacement.name else item
        for item in policy.targets
    )
    return policy_with(targets)


class CookedSampleProductClosureTests(unittest.TestCase):
    def test_accepts_cooked_only_product(self) -> None:
        self.assertEqual(validate_sample_cooked_product_closure(valid_policy()), [])

    def test_rejects_development_composition_behind_link_only(self) -> None:
        policy = valid_policy()
        product = next(item for item in policy.targets if item.name == PRODUCT_TARGET)
        policy = replace_target(
            policy,
            replace(
                product,
                interface_link_libraries=(
                    "__RVX_GENEX_OPEN__LINK_ONLY:RVX::SampleDevelopmentComposition>",
                ),
            ),
        )
        findings = validate_sample_cooked_product_closure(policy)
        self.assertTrue(any(DEVELOPMENT_COMPOSITION in item for item in findings), findings)

    def test_rejects_source_importer_from_shared_helpers(self) -> None:
        policy = valid_policy()
        common = next(item for item in policy.targets if item.name == "RVX_SampleCommon")
        importer = target("RVX_AssetImport", "Developer", aliases=("RVX::AssetImport",))
        policy = policy_with((*policy.targets, importer))
        common = next(item for item in policy.targets if item.name == "RVX_SampleCommon")
        policy = replace_target(
            policy, replace(common, link_libraries=("RVX::AssetImport",))
        )
        findings = validate_sample_cooked_product_closure(policy)
        self.assertTrue(any("shared target directly links forbidden" in item for item in findings), findings)

    def test_rejects_missing_cooked_composition(self) -> None:
        policy = valid_policy()
        product = next(item for item in policy.targets if item.name == PRODUCT_TARGET)
        policy = replace_target(
            policy,
            replace(product, link_libraries=("RVX::Gfx24SampleScenarios",)),
        )
        findings = validate_sample_cooked_product_closure(policy)
        self.assertTrue(any("must reach RVX_SampleCookedComposition" in item for item in findings), findings)


if __name__ == "__main__":
    unittest.main()
