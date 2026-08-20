#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_module_policy import write_fixture as write_policy_fixture


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_rhi_ownership_inventory.py"
HOLDER_PATH = "Source/Runtime/Render/Private/Holder.h"


def make_entry(**overrides) -> dict:
    entry = {
        "path": HOLDER_PATH, "owner": "Holder", "symbol": "m_texture",
        "policy": "OwnerSnapshot", "completionSource": "Render Thread last-submitted snapshot",
        "normalRelease": "retirement queue after completion",
        "deviceLostRelease": "explicit device-lost teardown",
    }
    entry.update(overrides)
    return entry


def write_fixture(root: Path, source: str, entries: list[dict]) -> tuple[Path, Path]:
    manifest, module_inventory = write_policy_fixture(root)
    holder = root / HOLDER_PATH
    holder.write_text(source, encoding="utf-8")
    data = json.loads(module_inventory.read_text(encoding="utf-8"))
    data["targets"][1]["sources"] = ["Private/Holder.h"]
    module_inventory.write_text(json.dumps(data), encoding="utf-8")
    scripts = root / "Scripts"
    scripts.mkdir()
    (scripts / "rhi_ownership_inventory_m1.json").write_text(
        json.dumps({"schemaVersion": 1, "entries": entries}), encoding="utf-8"
    )
    return manifest, module_inventory


def run_checker(root: Path, manifest: Path, module_inventory: Path,
                phase: str = "pre-delete") -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root), "--phase", phase,
         "--manifest", str(manifest), "--module-inventory", str(module_inventory)],
        cwd=REPO_ROOT, text=True, capture_output=True, check=False,
    )


class RHIOwnershipInventoryCheckerTests(unittest.TestCase):
    def test_complete_classified_holder_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [make_entry()])
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unclassified_holder_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [])
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("unclassified RHI owner", output.stdout + output.stderr)

    def test_rhi_parameter_in_noexcept_method_is_not_a_holder(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(
                root, "struct Holder {\n    void Observe(id<MTLCommandBuffer> commandBuffer, int operation) noexcept;\n};\n", [],
            )
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_backend_holder_is_still_classified(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [make_entry()])
            metal = root / "Source/Runtime/RHI_Metal/Private/MetalHolder.h"
            metal.write_text("struct MetalHolder { RHITextureRef m_texture; };\n", encoding="utf-8")
            ownership = root / "Scripts/rhi_ownership_inventory_m1.json"
            data = json.loads(ownership.read_text(encoding="utf-8"))
            data["entries"].append(make_entry(
                path="Source/Runtime/RHI_Metal/Private/MetalHolder.h", owner="MetalHolder",
            ))
            ownership.write_text(json.dumps(data), encoding="utf-8")
            result = run_checker(root, manifest, inventory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stale_manifest_entry_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder {};\n", [make_entry()])
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("stale holder entry", output.stdout + output.stderr)

    def test_invalid_policy_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [make_entry(policy="FrameLag")])
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("invalid lifetime policy", output.stdout + output.stderr)

    def test_missing_completion_source_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [make_entry(completionSource="")])
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("non-empty strings", output.stdout + output.stderr)

    def test_fixed_frame_retention_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(
                root, "struct Holder { RHITextureRef m_texture; };\nconstexpr auto lag = RVX_MAX_FRAME_COUNT + 1;\n", [make_entry()],
            )
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("fixed-frame retention", output.stdout + output.stderr)

    def test_residual_global_deleter_symbol_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(
                root, "struct Holder { RHITextureRef m_texture; };\nvoid Use() { DeferredDeleterRegistry::Get(); }\n", [make_entry()],
            )
            output = run_checker(root, manifest, inventory)
            self.assertNotEqual(output.returncode, 0)
            self.assertIn("global/frame-count deletion", output.stdout + output.stderr)

    def test_unknown_phase_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, inventory = write_fixture(root, "struct Holder { RHITextureRef m_texture; };\n", [make_entry()])
            output = run_checker(root, manifest, inventory, "migration")
            self.assertEqual(output.returncode, 2)
            self.assertIn("unsupported RHI ownership phase", output.stdout + output.stderr)


if __name__ == "__main__":
    unittest.main()
