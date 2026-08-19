#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "Scripts"
ROOT_ONLY_GATES = (
    "check_cmake_module_visibility.py",
    "check_editor_runtime_boundary.py",
    "check_pure_ecs_runtime.py",
)
MANIFEST_GATES = (
    "check_module_boundaries.py",
    "check_module_boundary_manifest.py",
    "check_cmake_module_include_edges.py",
    "check_cmake_module_links.py",
    "check_public_header_linkage.py",
)


def run_gate(script: str, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / script), *args],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class ArchitectureGateInputTests(unittest.TestCase):
    def test_all_gates_reject_missing_repository_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            missing_root = Path(temp_dir) / "missing-repository"
            for script in (*ROOT_ONLY_GATES, *MANIFEST_GATES):
                with self.subTest(script=script):
                    result = run_gate(script, "--root", str(missing_root))
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("architecture gate input error", result.stderr.lower())

    def test_manifest_gates_reject_missing_manifest(self) -> None:
        missing_manifest = REPO_ROOT / "Docs" / "missing-module-boundaries.json"
        for script in MANIFEST_GATES:
            with self.subTest(script=script):
                result = run_gate(
                    script,
                    "--root",
                    str(REPO_ROOT),
                    "--config",
                    str(missing_manifest),
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("architecture gate input error", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
