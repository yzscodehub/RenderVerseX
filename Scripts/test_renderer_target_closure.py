#!/usr/bin/env python3
"""Fail-closed self-tests for the renderer target-closure gate."""

from __future__ import annotations

import unittest
from dataclasses import replace
from pathlib import Path

from check_renderer_target_closure import (
    FEATURE_TARGETS,
    RENDER_TARGETS,
    validate_renderer_target_closure,
)
from module_policy import Module, Policy, Target


def make_module(module_id: str, domain: str = "runtime") -> Module:
    return Module(
        id=module_id,
        path=Path(module_id),
        domain=domain,
        layer="render" if module_id == "Render" else "foundation",
        public_include_prefixes=(module_id,),
        allowed_modules=frozenset({module_id}),
        shipping_allowed=domain == "runtime",
        deferred=False,
        owner="test",
    )


def make_target(
    name: str,
    module: str,
    *,
    aliases: tuple[str, ...] = (),
    links: tuple[str, ...] = (),
    sources: tuple[str, ...] | None = None,
    target_type: str = "STATIC_LIBRARY",
) -> Target:
    return Target(
        name=name,
        aliases=aliases,
        module=module,
        type=target_type,
        source_directory=".",
        binary_directory="build",
        infrastructure=False,
        sources=sources if sources is not None else (f"Private/{name}.cpp",),
        link_libraries=links,
        interface_link_libraries=(),
        include_directories=(),
        interface_include_directories=(),
    )


def make_valid_policy() -> Policy:
    modules = (
        make_module("Core"),
        make_module("Render"),
        make_module("Engine"),
        make_module("ShaderCompiler", "developer"),
    )
    targets: list[Target] = [
        make_target("RVX_Core", "Core", aliases=("RVX::Core",)),
        make_target(
            "RVX_ShaderCompiler",
            "ShaderCompiler",
            aliases=("RVX::ShaderCompiler",),
        ),
    ]
    targets.extend([
        make_target(
            "RVX_RenderRuntimeCore",
            "Render",
            aliases=("RVX::RenderRuntimeCore",),
            links=("RVX::Core",),
        ),
        make_target(
            "RVX_RenderGraph",
            "Render",
            aliases=("RVX::RenderGraph",),
            links=("RVX::Core", "RVX::RenderRuntimeCore"),
        ),
        make_target(
            "RVX_RenderFoundation",
            "Render",
            aliases=("RVX::RenderFoundation",),
            links=("RVX::Core", "RVX::RenderGraph"),
        ),
    ])
    targets.extend(
        make_target(
            name,
            "Render",
            aliases=(RENDER_TARGETS[name],),
            links=("RVX::RenderFoundation", "RVX::RenderGraph"),
        )
        for name in FEATURE_TARGETS
    )
    targets.extend([
        make_target(
            "RVX_RenderComposition",
            "Render",
            aliases=("RVX::RenderComposition",),
            links=(
                "RVX::RenderFoundation",
                "RVX::RenderGraph",
                *(RENDER_TARGETS[name] for name in FEATURE_TARGETS),
            ),
        ),
        make_target(
            "RVX_Render",
            "Render",
            aliases=("RVX::Render",),
            links=("RVX::RenderComposition",),
        ),
        make_target(
            "RVX_Engine",
            "Engine",
            aliases=("RVX::Engine",),
            links=("RVX::Render", "RVX::RenderComposition"),
        ),
    ])
    module_map = {module.id: module for module in modules}
    reference_map: dict[str, str] = {}
    for target in targets:
        reference_map[target.name] = target.module
        reference_map.update({alias: target.module for alias in target.aliases})
    return Policy(
        root=Path(".").resolve(),
        manifest_path=Path("manifest.json"),
        inventory_path=Path("inventory.json"),
        modules=module_map,
        prefix_to_module={module.id.casefold(): module.id for module in modules},
        targets=tuple(targets),
        target_to_module=reference_map,
        disabled_modules=frozenset(),
        source_root=Path(".").resolve(),
        binary_root=Path("build").resolve(),
    )


def replace_target(policy: Policy, name: str, replacement: Target | None) -> Policy:
    targets = tuple(
        target for target in policy.targets
        if target.name != name
    )
    if replacement is not None:
        targets = (*targets, replacement)
    target_map: dict[str, str] = {}
    for target in targets:
        target_map[target.name] = target.module
        target_map.update({alias: target.module for alias in target.aliases})
    return replace(policy, targets=targets, target_to_module=target_map)


class RendererTargetClosureTests(unittest.TestCase):
    def test_accepts_the_required_acyclic_runtime_graph(self) -> None:
        self.assertEqual(validate_renderer_target_closure(make_valid_policy()), [])

    def test_rejects_missing_target_and_alias(self) -> None:
        policy = make_valid_policy()
        policy = replace_target(policy, "RVX_RenderWater", None)
        findings = validate_renderer_target_closure(policy)
        self.assertIn("missing renderer target RVX_RenderWater", findings)

        policy = make_valid_policy()
        surface = next(t for t in policy.targets if t.name == "RVX_RenderSurface")
        policy = replace_target(policy, surface.name, replace(surface, aliases=()))
        findings = validate_renderer_target_closure(policy)
        self.assertIn(
            "RVX_RenderSurface: missing required alias RVX::RenderSurface",
            findings,
        )

    def test_rejects_an_interface_or_empty_feature_target(self) -> None:
        policy = make_valid_policy()
        particles = next(t for t in policy.targets if t.name == "RVX_RenderParticles")
        policy = replace_target(
            policy,
            particles.name,
            replace(particles, type="INTERFACE_LIBRARY", sources=()),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertTrue(any("concrete STATIC_LIBRARY" in item for item in findings))
        self.assertTrue(any("has no sources" in item for item in findings))

    def test_rejects_feature_to_feature_dependency(self) -> None:
        policy = make_valid_policy()
        surface = next(t for t in policy.targets if t.name == "RVX_RenderSurface")
        policy = replace_target(
            policy,
            surface.name,
            replace(surface, link_libraries=(*surface.link_libraries, "RVX::RenderLighting")),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertTrue(any("illegal direct dependency RVX_RenderLighting" in item for item in findings))

    def test_rejects_interface_and_link_only_dependency_bypasses(self) -> None:
        policy = make_valid_policy()
        surface = next(t for t in policy.targets if t.name == "RVX_RenderSurface")
        policy = replace_target(
            policy,
            surface.name,
            replace(
                surface,
                interface_link_libraries=(
                    "__RVX_GENEX_OPEN__LINK_ONLY:RVX::ShaderCompiler>",
                ),
            ),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertTrue(
            any("illegal direct dependency RVX_ShaderCompiler" in item for item in findings),
            findings,
        )

    def test_rejects_incomplete_composition(self) -> None:
        policy = make_valid_policy()
        composition = next(t for t in policy.targets if t.name == "RVX_RenderComposition")
        policy = replace_target(
            policy,
            composition.name,
            replace(
                composition,
                link_libraries=tuple(
                    value for value in composition.link_libraries
                    if value != "RVX::RenderWater"
                ),
            ),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertIn(
            "RVX_RenderComposition: missing direct feature dependency RVX_RenderWater",
            findings,
        )

    def test_rejects_developer_reachability(self) -> None:
        policy = make_valid_policy()
        surface = next(t for t in policy.targets if t.name == "RVX_RenderSurface")
        policy = replace_target(
            policy,
            surface.name,
            replace(surface, link_libraries=(*surface.link_libraries, "RVX::ShaderCompiler")),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertTrue(any("non-shipping target RVX_ShaderCompiler" in item for item in findings))

    def test_rejects_duplicate_source_ownership(self) -> None:
        policy = make_valid_policy()
        surface = next(t for t in policy.targets if t.name == "RVX_RenderSurface")
        lighting = next(t for t in policy.targets if t.name == "RVX_RenderLighting")
        policy = replace_target(
            policy,
            lighting.name,
            replace(lighting, sources=surface.sources),
        )
        findings = validate_renderer_target_closure(policy)
        self.assertTrue(any("owned by both" in item for item in findings))


if __name__ == "__main__":
    unittest.main()
