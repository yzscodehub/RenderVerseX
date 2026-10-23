#!/usr/bin/env python3
"""Regression coverage for manifest/inventory module coverage validation."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_module_policy import target, write_fixture


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_module_boundary_manifest.py"


def run_checker(root: Path, manifest: Path, inventory: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root), "--manifest", str(manifest),
         "--module-inventory", str(inventory)],
        cwd=REPO_ROOT, text=True, capture_output=True, check=False,
    )


class ModuleBoundaryManifestTests(unittest.TestCase):
    def test_nested_manifest_path_and_active_targets_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_non_deferred_module_without_active_or_disabled_target_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"] = [entry for entry in data["targets"] if entry["module"] != "Core"]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("neither active nor disabled", result.stderr)

    def test_infrastructure_target_cannot_mask_missing_production_module(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            core = target("Core", root / "Source/Runtime/Core")
            core["infrastructure"] = True
            core["type"] = "INTERFACE_LIBRARY"
            data["targets"] = [core, data["targets"][1]]
            inventory.write_text(json.dumps(data), encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("neither active nor disabled", result.stderr)

    def test_deferred_editor_active_target_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"].append(target("Editor", root / "Source/Editor"))
            inventory.write_text(json.dumps(data), encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("module Editor is both active and disabled", result.stderr)


if __name__ == "__main__":
    unittest.main()
