#!/usr/bin/env python3
"""Ensure policy-backed architecture gates reject incomplete inputs."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "Scripts"
MANIFEST = REPO_ROOT / "Docs/module-boundaries.json"
POLICY_GATES = (
    "check_module_boundaries.py",
    "check_module_boundary_manifest.py",
    "check_cmake_module_visibility.py",
    "check_cmake_module_include_edges.py",
    "check_cmake_module_links.py",
    "check_runtime_product_closure.py",
    "check_public_header_linkage.py",
    "check_editor_runtime_boundary.py",
    "check_pure_ecs_runtime.py",
    "check_render_contract_fields.py",
    "check_rhi_ownership_inventory.py",
)


def run_gate(script: str, *args: str) -> subprocess.CompletedProcess[str]:
    extra = ("--phase", "final") if script == "check_rhi_ownership_inventory.py" else ()
    return subprocess.run(
        [sys.executable, str(SCRIPTS_DIR / script), *args, *extra],
        cwd=REPO_ROOT, text=True, capture_output=True, check=False,
    )


class ArchitectureGateInputTests(unittest.TestCase):
    def test_all_gates_reject_missing_repository_root(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing_root = Path(temporary) / "missing-repository"
            for script in POLICY_GATES:
                with self.subTest(script=script):
                    result = run_gate(
                        script, "--root", str(missing_root),
                        "--manifest", str(missing_root / "module-boundaries.json"),
                        "--module-inventory", str(missing_root / "module-inventory.json"),
                    )
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("input error", result.stderr.lower())

    def test_all_gates_reject_missing_manifest_and_inventory(self) -> None:
        missing_manifest = REPO_ROOT / "Docs/missing-module-boundaries.json"
        missing_inventory = REPO_ROOT / "Docs/missing-module-inventory.json"
        for script in POLICY_GATES:
            with self.subTest(script=script, failure="manifest"):
                result = run_gate(
                    script, "--root", str(REPO_ROOT), "--manifest", str(missing_manifest),
                    "--module-inventory", str(missing_inventory),
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("input error", result.stderr.lower())
            with self.subTest(script=script, failure="inventory"):
                result = run_gate(
                    script, "--root", str(REPO_ROOT), "--manifest", str(MANIFEST),
                    "--module-inventory", str(missing_inventory),
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("input error", result.stderr.lower())

    def test_removed_config_alias_is_rejected(self) -> None:
        result = run_gate(
            "check_module_boundary_manifest.py", "--root", str(REPO_ROOT),
            "--manifest", str(MANIFEST), "--config", str(MANIFEST),
            "--module-inventory", str(REPO_ROOT / "Docs/missing-module-inventory.json"),
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("unrecognized arguments: --config", result.stderr)


if __name__ == "__main__":
    unittest.main()
