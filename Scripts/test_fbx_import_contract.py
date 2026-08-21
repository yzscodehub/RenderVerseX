#!/usr/bin/env python3
"""Regression coverage for the FBX import architecture gate."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_fbx_import_contract.py"
COMMIT = "19bdb7e7ef02eb914d5e7211a3685f50ee6d27e3"
SHA512 = (
    "66d42b62ed3e55c19cb99cd55876853bf48ce3b09b026fa54d1719e78b40ccdf"
    "d2ca44d3410fb9552f60e6d5cdf9e5d911ee4164d905250af87f90011dbab1a4"
)


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def write_fixture(root: Path) -> None:
    write(root / "CMakeLists.txt", "option(RVX_ENABLE_FBX \"FBX\" ON)\n")
    write(
        root / "vcpkg.json",
        json.dumps({"features": {"fbx": {"dependencies": ["ufbx"]}}}),
    )
    presets = []
    for name in ("win_x64_base", "linux_x64_base", "mac_arm64_base"):
        presets.append({
            "name": name,
            "cacheVariables": {
                "RVX_ENABLE_FBX": "ON",
                "VCPKG_MANIFEST_FEATURES": "fbx",
            },
        })
    for platform in ("win_x64", "linux_x64", "mac_arm64"):
        for kind in ("shipping_host_release", "shipping_release"):
            presets.append({
                "name": f"{platform}_{kind}",
                "cacheVariables": {
                    "RVX_ENABLE_FBX": "OFF",
                    "VCPKG_MANIFEST_FEATURES": "",
                },
            })
    write(root / "CMakePresets.json", json.dumps({"configurePresets": presets}))
    write(
        root / "vcpkg-ports/ufbx/vcpkg.json",
        json.dumps({"name": "ufbx", "version": "0.14.3"}),
    )
    write(
        root / "vcpkg-ports/ufbx/portfile.cmake",
        f"vcpkg_from_github(REF {COMMIT} SHA512 {SHA512})\n",
    )
    write(
        root / "vcpkg-ports/ufbx/CMakeLists.txt",
        "project(ufbx LANGUAGES C)\n"
        "add_library(ufbx STATIC ufbx.c)\n"
        "add_library(ufbx::ufbx ALIAS ufbx)\n"
        "target_compile_features(ufbx PUBLIC c_std_99)\n"
        "set_target_properties(ufbx PROPERTIES C_STANDARD_REQUIRED YES)\n",
    )
    write(
        root / "Source/Developer/AssetImport/CMakeLists.txt",
        "if(RVX_ENABLE_FBX)\n"
        "  find_package(ufbx CONFIG REQUIRED)\n"
        "  target_sources(RVX_AssetImport PRIVATE Private/FBXImporter.cpp)\n"
        "  target_link_libraries(RVX_AssetImport PRIVATE ufbx::ufbx)\n"
        "endif()\n",
    )
    write(
        root / "Source/Developer/AssetPipeline/CMakeLists.txt",
        "if(RVX_ENABLE_FBX)\nendif()\n",
    )
    write(
        root / "Source/Developer/AssetPipeline/Private/AssetPipeline.cpp",
        '#if defined(RVX_ENABLE_FBX)\n{ ".fbx", AssetType::Model },\n#endif\n',
    )
    write(
        root / "Source/Developer/AssetImport/Include/AssetImport/FBXImporter.h",
        "#pragma once\nclass FBXImporter {};\n",
    )


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class FBXImportContractTests(unittest.TestCase):
    def test_valid_contract_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            result = run_checker(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unpinned_source_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            portfile = root / "vcpkg-ports/ufbx/portfile.cmake"
            portfile.write_text("vcpkg_from_github(REF v0.14.3)\n", encoding="utf-8")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("immutable", result.stdout)
            self.assertIn("SHA512", result.stdout)

    def test_header_only_or_cpp_classification_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            cmake = root / "vcpkg-ports/ufbx/CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                .replace("LANGUAGES C", "LANGUAGES CXX")
                .replace("STATIC ufbx.c", "INTERFACE"),
                encoding="utf-8",
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("C-only", result.stdout)
            self.assertIn("real static C source", result.stdout)

    def test_shipping_feature_leak_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            path = root / "CMakePresets.json"
            presets = json.loads(path.read_text(encoding="utf-8"))
            presets["configurePresets"][3]["cacheVariables"] = {
                "RVX_ENABLE_FBX": "ON",
                "VCPKG_MANIFEST_FEATURES": "fbx",
            }
            path.write_text(json.dumps(presets), encoding="utf-8")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("shipping preset", result.stdout)

    def test_public_ufbx_and_fetchcontent_fallback_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            header = root / "Source/Developer/AssetImport/Include/AssetImport/FBXImporter.h"
            header.write_text("struct ufbx_scene;\n", encoding="utf-8")
            source_cmake = root / "Source/Developer/AssetImport/CMakeLists.txt"
            source_cmake.write_text(
                source_cmake.read_text(encoding="utf-8") +
                "FetchContent_Declare(ufbx)\n",
                encoding="utf-8",
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("public FBXImporter header", result.stdout)
            self.assertIn("FetchContent", result.stdout)


if __name__ == "__main__":
    unittest.main()
