#!/usr/bin/env python3
"""Small CMake integration coverage for generated module target inventory."""
from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
INVENTORY_MODULE = (REPO_ROOT / "cmake/RVXModuleInventory.cmake").as_posix()


def write_fixture(root: Path, *, unregistered: bool = False, unregistered_alias: bool = False,
                  unknown_link: bool = False,
                  dynamic_link: bool = False,
                  dynamic_include: bool = False,
                  complex_include: bool = False,
                  particle_typo: bool = False,
                  core_infrastructure: bool = False) -> None:
    for directory in ("Core", "Child", "Editor"):
        (root / directory).mkdir()
    (root / "module.json").write_text(json.dumps({
        "schema": "RVX.ModuleBoundaryManifest", "schemaVersion": 2,
        "modules": [
            {"id": "Core", "path": "Core", "deferred": False},
            {"id": "Child", "path": "Child", "deferred": False},
            {"id": "Editor", "path": "Editor", "deferred": True},
        ],
    }), encoding="utf-8")
    (root / "Core/CMakeLists.txt").write_text(
        "add_library(RVX_Core INTERFACE)\n"
        "add_library(RVX::Core ALIAS RVX_Core)\n"
        "rvx_register_module_target(TARGET RVX_Core MODULE Core "
        "ALIASES RVX::Core" + (" INFRASTRUCTURE" if core_infrastructure else "") + ")\n",
        encoding="utf-8",
    )
    extra = "add_library(RVX_Unregistered INTERFACE)\n" if unregistered else ""
    extra_alias = "add_library(RVX::ExtraChild ALIAS RVX_Child)\n" if unregistered_alias else ""
    if dynamic_link:
        link = "$<TARGET_PROPERTY:RVX_Core,INTERFACE_LINK_LIBRARIES>"
    elif particle_typo:
        link = "Particles"
    else:
        link = "RVX::Unknown" if unknown_link else ("RVX::ExtraChild" if unregistered_alias else "RVX::Core")
    if dynamic_include:
        include_line = (
            'target_include_directories(RVX_Child INTERFACE '
            '"$<TARGET_PROPERTY:RVX_Core,INTERFACE_INCLUDE_DIRECTORIES>")\n'
        )
    elif complex_include:
        include_line = (
            'target_include_directories(RVX_Child INTERFACE '
            '"$<$<CONFIG:Debug>:${CMAKE_SOURCE_DIR}/Core/Include>")\n'
        )
    else:
        include_line = ""
    (root / "Child/CMakeLists.txt").write_text(
        "add_library(RVX_Child INTERFACE)\n"
        "add_library(RVX::Child ALIAS RVX_Child)\n"
        "rvx_register_module_target(TARGET RVX_Child MODULE Child ALIASES RVX::Child)\n"
        + extra + extra_alias,
        encoding="utf-8",
    )
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\nproject(ModuleInventoryFixture LANGUAGES NONE)\n"
        f"include(\"{INVENTORY_MODULE}\")\n"
        "add_subdirectory(Core)\nadd_subdirectory(Child)\n"
        # Deliberately add this in the parent after add_subdirectory; the
        # finalized inventory must preserve this CMP0079-wrapped link.
        f"target_link_libraries(RVX_Child INTERFACE {link})\n"
        + include_line
        + "rvx_register_disabled_module(MODULE Editor REASON \"deferred fixture\")\n"
        + "rvx_finalize_module_inventory(MANIFEST \"${CMAKE_SOURCE_DIR}/module.json\" "
        + "OUTPUT \"${CMAKE_BINARY_DIR}/Architecture/RVXModuleInventory-$<CONFIG>.json\")\n",
        encoding="utf-8",
    )


def configure(root: Path, binary: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["cmake", "-S", str(root), "-B", str(binary or root / "build"), "-DCMAKE_BUILD_TYPE=Debug"],
        text=True, capture_output=True, check=False,
    )


class ModuleInventoryCMakeTests(unittest.TestCase):
    def test_debug_inventory_captures_parent_link_alias_and_disabled_editor(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            result = configure(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            inventory = root / "build/Architecture/RVXModuleInventory-Debug.json"
            data = json.loads(inventory.read_text(encoding="utf-8"))
            self.assertEqual(data["configuration"], "Debug")
            cache = (root / "build/CMakeCache.txt").read_text(encoding="utf-8")
            if "CMAKE_CONFIGURATION_TYPES:STRING=" in cache:
                release = root / "build/Architecture/RVXModuleInventory-Release.json"
                self.assertEqual(json.loads(release.read_text(encoding="utf-8"))["configuration"], "Release")
            child = next(entry for entry in data["targets"] if entry["name"] == "RVX_Child")
            self.assertIn("RVX::Child", child["aliases"])
            self.assertTrue(any(
                "RVX::Core" in value
                for value in (*child["linkLibraries"], *child["interfaceLinkLibraries"])
            ))
            self.assertEqual(data["disabledModules"], [{"module": "Editor", "reason": "deferred fixture"}])

    def test_unregistered_target_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, unregistered=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("not registered", result.stdout + result.stderr)

    def test_unregistered_alias_used_by_graph_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, unregistered_alias=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unknown first-party target", result.stdout + result.stderr)

    def test_unknown_internal_link_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, unknown_link=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unknown first-party target", result.stdout + result.stderr)

    def test_dynamic_target_producing_link_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, dynamic_link=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("dynamic target-property generator", result.stdout + result.stderr)

    def test_dynamic_target_property_include_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, dynamic_include=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("dynamic target-property generator", result.stdout + result.stderr)

    def test_complex_include_generator_expression_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, complex_include=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("unsupported include-directory generator", result.stdout + result.stderr)

    def test_reserved_particle_target_typo_fails_at_configure_time(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, particle_typo=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Particles", result.stdout + result.stderr)

    def test_infrastructure_target_does_not_satisfy_non_deferred_module(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, core_infrastructure=True)
            result = configure(root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            output = result.stdout + result.stderr
            self.assertIn("Non-deferred module 'Core' has neither a registered", output)
            self.assertIn("disabled reason", output)

    def test_in_source_build_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            result = configure(root, root)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("out-of-source build", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
