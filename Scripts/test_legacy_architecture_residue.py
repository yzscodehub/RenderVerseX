#!/usr/bin/env python3
"""Regression coverage for retired architecture residue checks."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_module_policy import write_fixture


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_legacy_architecture_residue.py"


def run_checker(root: Path, manifest: Path, inventory: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable, str(CHECKER), "--root", str(root),
            "--manifest", str(manifest), "--module-inventory", str(inventory),
        ],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class LegacyArchitectureResidueTests(unittest.TestCase):
    def test_clean_layout_and_empty_retired_directory_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            (root / "ResourceRuntime").mkdir()
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_material_retired_root_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            old_source = root / "Core/Private/Old.cpp"
            old_source.parent.mkdir(parents=True)
            old_source.write_text("// retired\n", encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("retired top-level module root is material", result.stdout)

    def test_retired_target_identity_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            data = json.loads(inventory.read_text(encoding="utf-8"))
            data["targets"][0]["name"] = "RVX_Debug"
            inventory.write_text(json.dumps(data), encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("RVX_Debug", result.stdout)

    def test_retired_compatibility_file_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            legacy = root / "Source/Runtime/Core/Include/Core/Math.h"
            legacy.parent.mkdir(parents=True, exist_ok=True)
            legacy.write_text("#pragma once\n", encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("retired compatibility file exists", result.stdout)

    def test_retired_model_artifact_marker_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root)
            legacy = root / "Source/Runtime/Fixture.cpp"
            legacy.parent.mkdir(parents=True, exist_ok=True)
            legacy.write_text(
                'constexpr auto magic = "' + "RVX_MODEL_" + 'PREBAKE_V1";\n',
                encoding="utf-8",
            )
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("retired content marker exists", result.stdout)


if __name__ == "__main__":
    unittest.main()
