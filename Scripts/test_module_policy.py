#!/usr/bin/env python3
"""Focused fail-closed tests for Manifest v2 and target inventory parsing."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from architecture_gate_common import GateInputError
from module_policy import load_policy, resolve_public_target_reference, resolve_target_reference


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "Scripts"
BACKENDS = {
    "RHI_DX11": "DX11", "RHI_DX12": "DX12", "RHI_Vulkan": "Vulkan",
    "RHI_Metal": "Metal", "RHI_OpenGL": "OpenGL",
}


def module(module_id: str, path: str, prefix: str, *, deferred: bool = False,
           allowed: list[str] | None = None) -> dict:
    return {
        "id": module_id, "path": path, "domain": "editor" if deferred else "runtime",
        "layer": "validation", "publicIncludePrefixes": [prefix],
        "allowedModules": allowed if allowed is not None else [module_id],
        "shippingAllowed": not deferred, "deferred": deferred, "owner": "unassigned",
    }


def target(module_id: str, source_directory: Path, *, aliases: list[str] | None = None,
           sources: list[str] | None = None, links: list[str] | None = None,
           interface_includes: list[str] | None = None) -> dict:
    return {
        "name": f"RVX_{module_id}", "aliases": aliases if aliases is not None else [f"RVX::{module_id}"],
        "module": module_id, "type": "STATIC_LIBRARY", "sourceDirectory": str(source_directory),
        "binaryDirectory": str(source_directory), "infrastructure": False,
        "sources": sources if sources is not None else ["Private/Test.cpp"],
        "linkLibraries": links if links is not None else [], "interfaceLinkLibraries": [],
        "includeDirectories": [], "interfaceIncludeDirectories": interface_includes if interface_includes is not None else [],
    }


def write_fixture(root: Path, *, render_prefix: str = "Render", render_allowed: list[str] | None = None,
                  render_source: str = "namespace RVX {}\n") -> tuple[Path, Path]:
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\n", encoding="utf-8")
    modules = [
        module("Core", "Source/Runtime/Core", "Core"),
        module("Render", "Source/Runtime/Render", render_prefix, allowed=render_allowed),
        *(module(module_id, f"Source/Runtime/{module_id}", prefix)
          for module_id, prefix in BACKENDS.items()),
        module("Editor", "Source/Editor", "Editor", deferred=True),
    ]
    for entry in modules:
        module_root = root / entry["path"]
        (module_root / "Private").mkdir(parents=True, exist_ok=True)
        (module_root / "Include" / entry["publicIncludePrefixes"][0]).mkdir(parents=True, exist_ok=True)
    (root / "Source/Runtime/Render/Private/Test.cpp").write_text(render_source, encoding="utf-8")
    (root / "Source/Runtime/Core/Private/Test.cpp").write_text("namespace RVX {}\n", encoding="utf-8")
    (root / "Docs").mkdir()
    binary_root = root / "build"
    binary_root.mkdir()
    manifest_path = root / "Docs/module-boundaries.json"
    manifest_path.write_text(json.dumps({
        "schema": "RVX.ModuleBoundaryManifest", "schemaVersion": 2, "modules": modules,
    }), encoding="utf-8")
    inventory_path = root / "inventory.json"
    inventory_path.write_text(json.dumps({
        "schema": "RVX.ModuleTargetInventory", "schemaVersion": 1,
        "configuration": "Debug", "sourceRoot": str(root), "binaryRoot": str(binary_root),
        "targets": [
            target("Core", root / "Source/Runtime/Core"),
            target("Render", root / "Source/Runtime/Render"),
        ],
        "disabledModules": [
            *( {"module": module_id, "reason": "not built by fixture"} for module_id in BACKENDS ),
            {"module": "Editor", "reason": "deferred product"},
        ],
    }), encoding="utf-8")
    return manifest_path, inventory_path


def load(root: Path, manifest: Path, inventory: Path):
    return load_policy(root, SimpleNamespace(
        manifest=str(manifest), config=None, module_inventory=str(inventory),
    ))


def run_gate(script: str, root: Path, manifest: Path, inventory: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPTS / script), "--root", str(root), "--manifest", str(manifest),
         "--module-inventory", str(inventory)],
        cwd=REPO_ROOT, text=True, capture_output=True, check=False,
    )


class ModulePolicyTests(unittest.TestCase):
    def test_nested_path_prefix_different_from_id_and_encoded_genex_are_supported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, render_prefix="Graphics", render_allowed=["Core", "Render"])
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][1]["linkLibraries"] = ["__RVX_GENEX_OPEN__LINK_ONLY:RVX::Core>"]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            policy = load(root, manifest, inventory)
            self.assertEqual(policy.module_for_prefix("graphics"), "Render")
            self.assertEqual(policy.module_for_path(root / "Source/Runtime/Render/Private/Test.cpp"), "Render")
            self.assertEqual(resolve_target_reference(policy, data["targets"][1]["linkLibraries"][0]), ("Core", []))
            self.assertEqual(
                resolve_public_target_reference(policy, data["targets"][1]["linkLibraries"][0]),
                (None, []),
            )
            self.assertEqual(
                resolve_target_reference(policy, "__RVX_GENEX_OPEN__TARGET_NAME_IF_EXISTS:RVX::Core>"),
                ("Core", []),
            )
            self.assertEqual(
                resolve_target_reference(policy, "__RVX_GENEX_OPEN__BOOL:RVXCook>"),
                (None, ["RVXCook"]),
            )
            self.assertEqual(
                resolve_target_reference(
                    policy,
                    "__RVX_GENEX_OPEN__IF:$<BOOL:1>,RVX::Core,Particle>",
                ),
                ("Core", ["Particle"]),
            )

    def test_generic_canonical_target_in_conditional_genex_is_policy_checked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, render_allowed=["Render"])
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][0]["name"] = "RVXShippingProduct"
            conditional = (
                "__RVX_GENEX_OPEN____RVX_GENEX_OPEN__CONFIG:Release>:"
                "RVXShippingProduct>"
            )
            data["targets"][1]["linkLibraries"] = [conditional]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            policy = load(root, manifest, inventory)
            self.assertEqual(
                resolve_target_reference(policy, conditional),
                ("Core", []),
            )
            result = run_gate("check_cmake_module_links.py", root, manifest, inventory)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("forbidden module link Render->Core", result.stdout)

    def test_unresolved_generated_source_outside_owner_and_binary_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            generated = root.parent / f"{root.name}-outside" / "Generated.cpp"
            self.assertFalse(generated.exists())
            data["targets"][1]["sources"] = [str(generated)]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            result = run_gate("check_cmake_module_visibility.py", root, manifest, inventory)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("outside manifest module path or binary tree", result.stdout)

    def test_unauditable_include_generator_expressions_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][1]["includeDirectories"] = [
                "__RVX_GENEX_OPEN__TARGET_PROPERTY:RVX_Core,INTERFACE_INCLUDE_DIRECTORIES>"
            ]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

            data["targets"][1]["includeDirectories"] = [
                "__RVX_GENEX_OPEN____RVX_GENEX_OPEN__CONFIG:Debug>:"
                f"{root.as_posix()}/Source/Runtime/Core/Include>"
            ]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_missing_bad_or_empty_inventory_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            with self.assertRaises(GateInputError):
                load(root, manifest, root / "missing.json")
            inventory.write_text("not json", encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_forged_in_source_inventory_and_invalid_infrastructure_type_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["binaryRoot"] = str(root)
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["binaryRoot"] = str(root / "build")
            data["targets"][0]["infrastructure"] = True
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            inventory.write_text(json.dumps({
                "schema": "RVX.ModuleTargetInventory", "schemaVersion": 1,
                "configuration": "Debug", "sourceRoot": str(root), "binaryRoot": str(root),
                "targets": [], "disabledModules": [],
            }), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_duplicate_alias_unknown_module_and_missing_deferred_editor_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][1]["aliases"] = ["RVX::Core"]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["targets"][1]["aliases"] = ["RVX::Render"]
            data["targets"][1]["module"] = "Unknown"
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["targets"][1]["module"] = "Render"
            data["disabledModules"] = []
            inventory.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_every_deferred_module_requires_disabled_record_and_cannot_be_active(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            (root / "Source/Developer/Future").mkdir(parents=True)
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            future = module("Future", "Source/Developer/Future", "Future", deferred=True)
            future["domain"] = "developer"
            future["layer"] = "tooling"
            manifest_data["modules"].append(future)
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

            inventory_data = json.loads(inventory.read_text(encoding="utf-8"))
            inventory_data["disabledModules"].append({"module": "Future", "reason": "deferred fixture"})
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            self.assertEqual(load(root, manifest, inventory).modules["Future"].id, "Future")

            inventory_data["targets"].append(target("Future", root / "Source/Developer/Future"))
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_wildcard_and_casefold_duplicate_prefix_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["modules"][1]["allowedModules"] = ["*"]
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_path_overlap_and_invalid_domain_or_layer_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["modules"][1]["path"] = "Source/Runtime/Core/NestedRender"
            (root / "Source/Runtime/Core/NestedRender").mkdir()
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["modules"][1]["path"] = "Source/Runtime/Render"
            data["modules"][1]["domain"] = "unknown"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["modules"][1]["domain"] = "runtime"
            data["modules"][1]["layer"] = "unknown"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)
            data["modules"][1]["allowedModules"] = ["Render"]
            data["modules"][1]["publicIncludePrefixes"] = ["core"]
            manifest.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(GateInputError):
                load(root, manifest, inventory)

    def test_forbidden_link_include_and_public_private_exposure_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(
                root, render_allowed=["Render"], render_source='#include "Core/Thing.h"\n',
            )
            (root / "Source/Runtime/Core/Include/Core/Thing.h").write_text("#pragma once\n", encoding="utf-8")
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][1]["linkLibraries"] = ["RVX::Core"]
            data["targets"][1]["interfaceIncludeDirectories"] = [
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Core/Include"
            ]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            for script, expected in (
                ("check_cmake_module_links.py", "forbidden module link"),
                ("check_module_boundaries.py", "forbidden include"),
                ("check_cmake_module_include_edges.py", "PUBLIC/INTERFACE exposes"),
            ):
                result = run_gate(script, root, manifest, inventory)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_visibility_allows_only_manifest_authorized_non_private_test_sample_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            samples_root = root / "Samples"
            tests_root = root / "Tests"
            scene = samples_root / "Scenes/Scenario.cpp"
            private = samples_root / "Private/Hidden.cpp"
            for path in (scene, private, tests_root / "Private/Test.cpp"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("namespace RVX {}\n", encoding="utf-8")
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            samples = module("Samples", "Samples", "Samples", allowed=["Samples"])
            samples["domain"] = "samples"
            samples["layer"] = "product"
            tests = module("Tests", "Tests", "Tests", allowed=["Samples", "Tests"])
            tests["domain"] = "tests"
            manifest_data["modules"].extend((samples, tests))
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            inventory_data = json.loads(inventory.read_text(encoding="utf-8"))
            inventory_data["targets"].extend((
                target("Samples", samples_root, sources=["Scenes/Scenario.cpp"]),
                target("Tests", tests_root, sources=[str(scene)]),
            ))
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")

            allowed = run_gate("check_cmake_module_visibility.py", root, manifest, inventory)
            self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)

            inventory_data["targets"][-1]["sources"] = [str(private)]
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            rejected_private = run_gate("check_cmake_module_visibility.py", root, manifest, inventory)
            self.assertNotEqual(rejected_private.returncode, 0, rejected_private.stdout + rejected_private.stderr)
            self.assertIn("source belongs to Samples", rejected_private.stdout)

            inventory_data["targets"][-1]["sources"] = [str(root / "Source/Runtime/Render/Private/Test.cpp")]
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            rejected_runtime = run_gate("check_cmake_module_visibility.py", root, manifest, inventory)
            self.assertNotEqual(rejected_runtime.returncode, 0, rejected_runtime.stdout + rejected_runtime.stderr)
            self.assertIn("source belongs to Render", rejected_runtime.stdout)

    def test_infrastructure_target_links_and_includes_remain_policy_checked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][0]["infrastructure"] = True
            data["targets"][0]["type"] = "INTERFACE_LIBRARY"
            data["targets"][0]["linkLibraries"] = ["RVX::Render"]
            data["targets"][0]["includeDirectories"] = [
                "${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include"
            ]
            data["disabledModules"].append({"module": "Core", "reason": "infra fixture"})
            inventory.write_text(json.dumps(data), encoding="utf-8")
            for script, expected in (
                ("check_cmake_module_links.py", "forbidden module link Core->Render"),
                ("check_cmake_module_include_edges.py", "forbidden include edge Core->Render"),
            ):
                result = run_gate(script, root, manifest, inventory)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_actual_target_edges_to_disabled_module_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, render_allowed=["Render", "RHI_Metal"])
            data = json.loads(inventory.read_text(encoding="utf-8"))
            metal = target("RHI_Metal", root / "Source/Runtime/RHI_Metal")
            metal["infrastructure"] = True
            metal["type"] = "INTERFACE_LIBRARY"
            data["targets"].append(metal)
            data["targets"][1]["linkLibraries"] = ["RVX::RHI_Metal"]
            data["targets"][1]["includeDirectories"] = [
                "${CMAKE_SOURCE_DIR}/Source/Runtime/RHI_Metal/Include"
            ]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            for script, expected in (
                ("check_cmake_module_links.py", "links deferred/disabled module RHI_Metal"),
                ("check_cmake_module_include_edges.py", "includes disabled module RHI_Metal"),
            ):
                result = run_gate(script, root, manifest, inventory)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_disabled_backend_source_policy_is_still_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            source = root / "Source/Runtime/RHI_Metal/Private/Backend.cpp"
            (root / "Source/Runtime/Core/Include/Core/Thing.h").write_text("#pragma once\n", encoding="utf-8")
            source.write_text('#include "Core/Thing.h"\n', encoding="utf-8")
            result = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("forbidden include RHI_Metal->Core", result.stdout)

    def test_source_include_resolves_local_private_before_prefix_and_rejects_collision(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            runtime_root = root / "Source/Runtime/Runtime"
            (runtime_root / "Private").mkdir(parents=True)
            (runtime_root / "Include/Runtime").mkdir(parents=True)
            (root / "Source/Runtime/Render/Private/Runtime").mkdir(parents=True)
            (runtime_root / "Private/Test.cpp").write_text("namespace RVX {}\n", encoding="utf-8")
            (runtime_root / "Include/Runtime/Foo.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "Source/Runtime/Render/Private/Runtime/Foo.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "Source/Runtime/Render/Private/Test.cpp").write_text('#include "Runtime/Foo.h"\n', encoding="utf-8")
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            manifest_data["modules"].append(module("Runtime", "Source/Runtime/Runtime", "Runtime"))
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            inventory_data = json.loads(inventory.read_text(encoding="utf-8"))
            inventory_data["targets"].append(target("Runtime", runtime_root))
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            result = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ambiguous project include", result.stdout)

    def test_disabled_destination_source_include_uses_manifest_allowlist_not_active_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            (root / "Source/Runtime/RHI_Metal/Include/Metal/Artifact.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "Source/Runtime/Render/Private/Test.cpp").write_text('#include "Metal/Artifact.h"\n', encoding="utf-8")
            rejected = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertNotEqual(rejected.returncode, 0, rejected.stdout + rejected.stderr)
            self.assertIn("forbidden include Render->RHI_Metal", rejected.stdout)
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            manifest_data["modules"][1]["allowedModules"] = ["Render", "RHI_Metal"]
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            allowed = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)

    def test_known_project_prefix_with_missing_header_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            (root / "Source/Runtime/Render/Private/Test.cpp").write_text('#include "Core/Typo.h"\n', encoding="utf-8")
            result = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unresolved project include: Core/Typo.h", result.stdout)

    def test_tests_domain_scans_only_registered_target_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            stale = root / "Tests/Private/Stale.cpp"
            stale.parent.mkdir(parents=True)
            stale.write_text('#include "Core/Typo.h"\n', encoding="utf-8")
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
            tests = module("Tests", "Tests", "Tests", allowed=["Tests"])
            tests["domain"] = "tests"
            manifest_data["modules"].append(tests)
            manifest.write_text(json.dumps(manifest_data), encoding="utf-8")
            inventory_data = json.loads(inventory.read_text(encoding="utf-8"))
            inventory_data["disabledModules"].append({"module": "Tests", "reason": "no target yet"})
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            unregistered = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertEqual(unregistered.returncode, 0, unregistered.stdout + unregistered.stderr)

            inventory_data["disabledModules"] = [
                entry for entry in inventory_data["disabledModules"] if entry["module"] != "Tests"
            ]
            inventory_data["targets"].append(target("Tests", root / "Tests", sources=["Private/Stale.cpp"]))
            inventory.write_text(json.dumps(inventory_data), encoding="utf-8")
            registered = run_gate("check_module_boundaries.py", root, manifest, inventory)
            self.assertNotEqual(registered.returncode, 0, registered.stdout + registered.stderr)
            self.assertIn("unresolved project include: Core/Typo.h", registered.stdout)


if __name__ == "__main__":
    unittest.main()
