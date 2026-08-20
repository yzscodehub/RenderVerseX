#!/usr/bin/env python3
"""Self-tests for the concrete Shipping target closure gate."""

from __future__ import annotations

import unittest
from pathlib import Path

from check_shipping_target_closure import validate_shipping_target_closure
from module_policy import Module, Policy, Target


ROOT = Path(".").resolve()


def make_module(
    module_id: str,
    domain: str,
    *,
    shipping: bool,
) -> Module:
    return Module(
        id=module_id,
        path=Path(module_id),
        domain=domain,
        layer="feature",
        public_include_prefixes=(module_id,),
        allowed_modules=frozenset((module_id,)),
        shipping_allowed=shipping,
        deferred=False,
        owner="test",
    )


def make_target(
    name: str,
    module: str,
    *,
    target_type: str = "STATIC_LIBRARY",
    links: tuple[str, ...] = (),
    interface_links: tuple[str, ...] = (),
    sources: tuple[str, ...] = (),
    includes: tuple[str, ...] = (),
    interface_includes: tuple[str, ...] = (),
) -> Target:
    return Target(
        name=name,
        aliases=(),
        module=module,
        type=target_type,
        source_directory=str(ROOT),
        binary_directory=str(ROOT / "build"),
        infrastructure=False,
        sources=sources,
        link_libraries=links,
        interface_link_libraries=interface_links,
        include_directories=includes,
        interface_include_directories=interface_includes,
    )


def make_policy(
    modules: tuple[Module, ...],
    targets: tuple[Target, ...],
) -> Policy:
    module_map = {module.id: module for module in modules}
    target_map: dict[str, str] = {}
    for target in targets:
        target_map[target.name] = target.module
        target_map.update({alias: target.module for alias in target.aliases})
    return Policy(
        root=ROOT,
        manifest_path=ROOT / "manifest.json",
        inventory_path=ROOT / "inventory.json",
        modules=module_map,
        prefix_to_module={module.id.casefold(): module.id for module in modules},
        targets=targets,
        target_to_module=target_map,
        disabled_modules=frozenset(),
        source_root=ROOT,
        binary_root=ROOT / "build",
    )


def valid_policy(*, runtime_sources: tuple[str, ...] = ()) -> Policy:
    engine = make_module("Engine", "runtime", shipping=True)
    tests = make_module("Tests", "tests", shipping=False)
    targets = (
        make_target("RuntimeCore", "Engine", sources=runtime_sources),
        make_target(
            "RVXShippingProduct",
            "Engine",
            links=("RuntimeCore",),
        ),
        make_target(
            "RVXShippingConsumer",
            "Tests",
            target_type="EXECUTABLE",
            links=("RVXShippingProduct",),
            sources=("Tests/ShippingConsumer/main.cpp",),
        ),
    )
    return make_policy((engine, tests), targets)


class ShippingTargetClosureTests(unittest.TestCase):
    def test_accepts_runtime_product_and_tests_domain_consumer_root(self) -> None:
        self.assertEqual(validate_shipping_target_closure(valid_policy()), [])

    def test_requires_both_exact_roots(self) -> None:
        policy = valid_policy()
        policy = make_policy(policy.modules.values(), policy.targets[:-1])
        findings = validate_shipping_target_closure(policy)
        self.assertTrue(any("consumer target is missing" in item for item in findings))

    def test_requires_real_executable_consumer_linking_only_product(self) -> None:
        policy = valid_policy()
        targets = list(policy.targets)
        targets[-1] = make_target(
            "RVXShippingConsumer",
            "Tests",
            links=("RuntimeCore",),
        )
        findings = validate_shipping_target_closure(
            make_policy(tuple(policy.modules.values()), tuple(targets))
        )
        self.assertTrue(any("must be an EXECUTABLE" in item for item in findings))
        self.assertTrue(any("must link the Shipping product" in item for item in findings))
        self.assertTrue(any("unexpected direct" in item for item in findings))

    def test_rejects_transitive_developer_target(self) -> None:
        policy = valid_policy()
        compiler = make_module("ShaderCompiler", "developer", shipping=False)
        targets = list(policy.targets)
        targets[0] = make_target(
            "RuntimeCore",
            "Engine",
            links=("$<LINK_ONLY:RVX::Compiler>",),
        )
        targets.append(Target(
            **{
                **make_target("Compiler", "ShaderCompiler").__dict__,
                "aliases": ("RVX::Compiler",),
            }
        ))
        findings = validate_shipping_target_closure(
            make_policy((*policy.modules.values(), compiler), tuple(targets))
        )
        self.assertTrue(any("non-shipping module ShaderCompiler" in item for item in findings))

    def test_rejects_editor_and_samples_targets(self) -> None:
        for module_id, domain in (("Editor", "editor"), ("Samples", "samples")):
            with self.subTest(domain=domain):
                policy = valid_policy()
                forbidden = make_module(module_id, domain, shipping=False)
                targets = list(policy.targets)
                targets[0] = make_target(
                    "RuntimeCore",
                    "Engine",
                    links=(f"RVX::{module_id}",),
                )
                targets.append(Target(
                    **{
                        **make_target(module_id, module_id).__dict__,
                        "aliases": (f"RVX::{module_id}",),
                    }
                ))
                findings = validate_shipping_target_closure(
                    make_policy((*policy.modules.values(), forbidden), tuple(targets))
                )
                self.assertTrue(any(
                    f"non-shipping module {module_id}" in item for item in findings
                ))

    def test_rejects_other_tests_target_below_consumer(self) -> None:
        policy = valid_policy()
        targets = list(policy.targets)
        targets[-1] = make_target(
            "RVXShippingConsumer",
            "Tests",
            target_type="EXECUTABLE",
            links=("RVXShippingProduct", "FullUnitSuite"),
        )
        targets.append(make_target("FullUnitSuite", "Tests"))
        findings = validate_shipping_target_closure(
            make_policy(tuple(policy.modules.values()), tuple(targets))
        )
        self.assertTrue(any("unexpected direct" in item for item in findings))
        self.assertTrue(any("non-shipping module Tests" in item for item in findings))

    def test_rejects_unresolved_first_party_dependency(self) -> None:
        policy = valid_policy()
        targets = list(policy.targets)
        targets[0] = make_target("RuntimeCore", "Engine", links=("RVX::Missing",))
        findings = validate_shipping_target_closure(
            make_policy(tuple(policy.modules.values()), tuple(targets))
        )
        self.assertTrue(any("unresolved first-party" in item for item in findings))

    def test_rejects_importer_and_legacy_mixed_loader_sources(self) -> None:
        policy = valid_policy(runtime_sources=(
            "Resource/Private/Importer/GLTFImporter.cpp",
            "Resource/Private/Loader/ModelLoader.cpp",
            "Resource/Private/Loader/CookedModelLoader.cpp",
            "Resource/Private/Parser/PackageParser.cpp",
        ))
        findings = validate_shipping_target_closure(policy)
        self.assertTrue(any("GLTFImporter.cpp" in item for item in findings))
        self.assertTrue(any("ModelLoader.cpp" in item for item in findings))
        self.assertTrue(any("PackageParser.cpp" in item for item in findings))
        self.assertFalse(any("CookedModelLoader.cpp" in item for item in findings))

    def test_resource_loader_catalog_is_not_misclassified_as_source_loader(self) -> None:
        policy = valid_policy(runtime_sources=(
            "Resource/Private/ResourceRuntime/ResourceLoaderCatalog.cpp",
        ))
        self.assertEqual(validate_shipping_target_closure(policy), [])

    def test_rejects_header_only_and_static_parser_declarations(self) -> None:
        for token in ("tinygltf", "tinyexr", "ufbx", "stb_image", "assimp", "miniz"):
            with self.subTest(token=token, declaration="link"):
                policy = valid_policy()
                targets = list(policy.targets)
                targets[0] = make_target(
                    "RuntimeCore", "Engine", links=(f"third_party::{token}",)
                )
                findings = validate_shipping_target_closure(
                    make_policy(tuple(policy.modules.values()), tuple(targets))
                )
                self.assertTrue(any(
                    f"parser token {token}" in item for item in findings
                ))
            with self.subTest(token=token, declaration="include"):
                policy = valid_policy()
                targets = list(policy.targets)
                targets[0] = make_target(
                    "RuntimeCore",
                    "Engine",
                    includes=(f"third_party/{token}/include",),
                )
                findings = validate_shipping_target_closure(
                    make_policy(tuple(policy.modules.values()), tuple(targets))
                )
                self.assertTrue(any(
                    f"parser token {token}" in item for item in findings
                ))

    def test_rejects_raw_developer_module_link_and_include_tokens(self) -> None:
        policy = valid_policy()
        developer = make_module("ShaderCompiler", "developer", shipping=False)
        targets = list(policy.targets)
        targets[0] = make_target(
            "RuntimeCore",
            "Engine",
            links=("$<LINK_ONLY:unregistered::ShaderCompiler::headers>",),
            interface_includes=(str(ROOT / "ShaderCompiler/Include"),),
        )
        findings = validate_shipping_target_closure(
            make_policy((*policy.modules.values(), developer), tuple(targets))
        )
        self.assertTrue(any("developer module token" in item for item in findings))
        self.assertTrue(any("include declaration" in item for item in findings))

    def test_allows_ordinary_generated_include_below_binary_root(self) -> None:
        policy = valid_policy()
        targets = list(policy.targets)
        targets[0] = make_target(
            "RuntimeCore",
            "Engine",
            includes=(str(ROOT / "build/generated/include"),),
            interface_includes=("$<BUILD_INTERFACE:build/generated/public>",),
        )
        self.assertEqual(
            validate_shipping_target_closure(
                make_policy(tuple(policy.modules.values()), tuple(targets))
            ),
            [],
        )


if __name__ == "__main__":
    unittest.main()
