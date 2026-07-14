#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts" / "check_module_boundary_manifest.py"


def write_fixture(root: Path, runtime_target_directory: str) -> None:
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.21)\n",
        encoding="utf-8",
    )
    render_directory = root / "Render"
    render_directory.mkdir(parents=True)
    (render_directory / "CMakeLists.txt").write_text(
        "add_library(RVX_Render STATIC)\n",
        encoding="utf-8",
    )
    scene_directory = root / "Scene"
    scene_directory.mkdir(parents=True)
    (scene_directory / "CMakeLists.txt").write_text(
        "add_library(RVX_Scene STATIC)\n",
        encoding="utf-8",
    )

    target_directory = root / runtime_target_directory
    target_directory.mkdir(parents=True, exist_ok=True)
    target_cmake = target_directory / "CMakeLists.txt"
    with target_cmake.open("a", encoding="utf-8") as handle:
        handle.write("add_library(RVX_RenderRuntimeCore STATIC)\n")

    docs_directory = root / "Docs"
    docs_directory.mkdir(parents=True)
    manifest = {
        "projectIncludePrefixes": ["Render", "Scene"],
        "modules": {
            "Render": {
                "path": "Render",
                "allowed": ["Render"],
            },
            "Scene": {
                "path": "Scene",
                "allowed": ["Scene"],
            },
        },
    }
    (docs_directory / "module-boundaries.json").write_text(
        json.dumps(manifest),
        encoding="utf-8",
    )


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(CHECKER),
            "--root",
            str(root),
            "--config",
            str(root / "Docs" / "module-boundaries.json"),
        ],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def write_render_test_support_fixture(root: Path, target_directory: str) -> None:
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.21)\n",
        encoding="utf-8",
    )
    for module_name in ("Tests", "Scene"):
        module_directory = root / module_name
        module_directory.mkdir(parents=True)
        (module_directory / "CMakeLists.txt").write_text(
            "add_library(RVX_RenderTestSupport STATIC)\n"
            if module_name == target_directory
            else "",
            encoding="utf-8",
        )

    docs_directory = root / "Docs"
    docs_directory.mkdir(parents=True)
    manifest = {
        "projectIncludePrefixes": ["Tests", "Scene"],
        "modules": {
            "Tests": {
                "path": "Tests",
                "allowed": ["*"],
            },
            "Scene": {
                "path": "Scene",
                "allowed": ["Scene"],
            },
        },
    }
    (docs_directory / "module-boundaries.json").write_text(
        json.dumps(manifest),
        encoding="utf-8",
    )


class ModuleBoundaryManifestTests(unittest.TestCase):
    def test_render_runtime_core_is_valid_render_submodule(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root, "Render")

            result = run_checker(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_render_runtime_core_outside_registered_module_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root, "Outside")

            result = run_checker(root)
            output = result.stdout + result.stderr

            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("outside any registered module path", output)

    def test_render_runtime_core_in_registered_wrong_module_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_fixture(root, "Scene")

            result = run_checker(root)
            output = result.stdout + result.stderr

            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("declared under Scene, not Render", output)

    def test_render_test_support_is_valid_tests_submodule(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_render_test_support_fixture(root, "Tests")

            result = run_checker(root)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_render_test_support_in_registered_wrong_module_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            write_render_test_support_fixture(root, "Scene")

            result = run_checker(root)
            output = result.stdout + result.stderr

            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn("declared under Scene, not Tests", output)


if __name__ == "__main__":
    unittest.main()
