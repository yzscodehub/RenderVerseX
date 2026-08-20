#!/usr/bin/env python3
"""Cross-platform integration tests for run_shipping_fixture.cmake."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from check_shipping_evidence import validate_receipt


SCRIPT_ROOT = Path(__file__).resolve().parent
FIXTURE_SCRIPT = SCRIPT_ROOT / "run_shipping_fixture.cmake"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
PRESET = "fixture-selftest"


FAKE_COOK = r'''#!/usr/bin/env python3
import json
import sys
from pathlib import Path

args = sys.argv[1:]
def value(name):
    index = args.index(name)
    return args[index + 1]

output = Path(value("--output"))
output.mkdir(parents=True, exist_ok=True)
if "--shader-catalog" in args:
    backend = value("--shader-backend")
    catalog = json.loads(Path(value("--shader-catalog")).read_text(encoding="utf-8"))
    selected = [
        entry for entry in catalog["programs"] if backend in entry["backends"]
    ]
    backend_root = output / backend
    backend_root.mkdir(parents=True, exist_ok=True)
    for index, entry in enumerate(selected, 1):
        (backend_root / f"{index:016x}.rvxshader").write_bytes(
            (entry["asset"] + "|" + entry["entry"] + "\n").encode()
        )
else:
    (output / "R7Triangle.rva").write_bytes(b"deterministic-model\n")
    manifest = Path(value("--manifest"))
    manifest.write_text(
        "RVX_COOK_MANIFEST_V2\n"
        "schema=RVX_COOK_MANIFEST\n"
        "version=2\n"
        "toolName=RVXCook\n"
        "toolVersion=2.0.0\n"
        "sourceRoot=.\n"
        "outputRoot=.\n"
        "recursive=1\n"
        "entryCount=1\n"
        "successCount=1\n"
        "failureCount=0\n"
        "entry.0.source=R7Triangle.gltf\n"
        "entry.0.output=R7Triangle.rva\n"
        "entry.0.type=Model\n"
        "entry.0.success=1\n"
        "RVX_COOK_MANIFEST_END\n",
        encoding="utf-8",
    )
'''


class ShippingFixtureScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        if shutil.which("cmake") is None:
            self.skipTest("cmake is unavailable")
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n", encoding="utf-8"
        )
        model = self.root / "Tests/Fixtures/ModelViewer/R7Triangle.gltf"
        model.parent.mkdir(parents=True)
        model.write_text("{\"asset\":{\"version\":\"2.0\"}}\n", encoding="utf-8")
        shader_root = self.root / "Render/Shaders"
        shader_root.mkdir(parents=True)
        catalog = {
            "schema": "RVX_SHADER_PROGRAM_CATALOG",
            "version": 2,
            "programs": [
                {
                    "asset": "A.hlsl",
                    "stage": "Vertex",
                    "entry": "VSMain",
                    "backends": ["DX12", "Vulkan"],
                },
                {
                    "asset": "B.hlsl",
                    "stage": "Pixel",
                    "entry": "PSMain",
                    "backends": ["DX12"],
                },
                {
                    "asset": "C.hlsl",
                    "stage": "Pixel",
                    "entry": "PSMain",
                    "backends": ["Vulkan"],
                },
            ],
        }
        (shader_root / "Programs.rvxshadercatalog.json").write_text(
            json.dumps(catalog), encoding="utf-8"
        )
        self.build_root = self.root.parent / f"{self.root.name}-build"
        self.build_root.mkdir()
        (self.build_root / "CMakeCache.txt").write_text(
            "# configured build fixture\n", encoding="utf-8"
        )
        self.fixture_root = self.build_root / "ShippingFixture"
        fake_script = self.root / "fake_rvxcook.py"
        fake_script.write_text(textwrap.dedent(FAKE_COOK), encoding="utf-8")
        if os.name == "nt":
            self.fake_cook = self.root / "fake_rvxcook.cmd"
            self.fake_cook.write_text(
                f'@"{sys.executable}" "{fake_script}" %*\n',
                encoding="utf-8",
            )
        else:
            self.fake_cook = self.root / "fake_rvxcook"
            self.fake_cook.write_text(
                f'#!/bin/sh\nexec "{sys.executable}" "{fake_script}" "$@"\n',
                encoding="utf-8",
            )
            self.fake_cook.chmod(
                self.fake_cook.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP
            )

    def tearDown(self) -> None:
        if hasattr(self, "temporary"):
            self.temporary.cleanup()
        if hasattr(self, "build_root"):
            shutil.rmtree(self.build_root, ignore_errors=True)

    def command(self, fixture_root: Path | None = None) -> list[str]:
        return [
            "cmake",
            f"-DRVX_COOK_EXECUTABLE={self.fake_cook}",
            f"-DRVX_SOURCE_ROOT={self.root}",
            f"-DRVX_BINARY_FIXTURE_ROOT={fixture_root or self.fixture_root}",
            "-DRVX_BACKEND=DX12",
            f"-DRVX_COMMIT={COMMIT}",
            f"-DRVX_PRESET={PRESET}",
            "-P",
            str(FIXTURE_SCRIPT),
        ]

    def test_generates_complete_deterministic_fixture_and_receipt(self) -> None:
        first = subprocess.run(
            self.command(), capture_output=True, text=True, check=False
        )
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        receipt_path = self.fixture_root / "shipping-fixture-receipt.json"
        receipt, paths = validate_receipt(receipt_path)
        self.assertEqual(receipt["resourceEntryCount"], 1)
        self.assertEqual(receipt["shaderProgramCount"], 2)
        self.assertIn("Cooked/Resources/R7Triangle.rva", paths)
        self.assertFalse((self.fixture_root / "Input").exists())
        first_receipt = receipt_path.read_bytes()

        (self.fixture_root / "stale-source.gltf").write_text("must disappear")
        second = subprocess.run(
            self.command(), capture_output=True, text=True, check=False
        )
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(receipt_path.read_bytes(), first_receipt)
        self.assertFalse((self.fixture_root / "stale-source.gltf").exists())

    def test_rejects_fixture_root_outside_configured_build_tree(self) -> None:
        outside = self.root.parent / f"{self.root.name}-not-a-build/fixture"
        result = subprocess.run(
            self.command(outside), capture_output=True, text=True, check=False
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("configured CMake build tree", result.stdout + result.stderr)
        shutil.rmtree(outside.parent, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
