#!/usr/bin/env python3
"""Regression coverage for the prepare-only resource loader gate."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_resource_loader_contracts.py"
CONTRACT = """#pragma once
class IResourceLoader
{
public:
    virtual bool Supports(const int& request) const noexcept = 0;
    virtual bool Prepare(const int& context, int& bundle, int& error) = 0;
};
"""


def write_fixture(root: Path, source: str = "") -> None:
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\n")
    header = root / "Source/Runtime/ResourceRuntime/Include/ResourceRuntime/ResourceLoader.h"
    header.parent.mkdir(parents=True)
    header.write_text(CONTRACT, encoding="utf-8")
    if source:
        implementation = root / "Source/Developer/Fixture.cpp"
        implementation.parent.mkdir(parents=True)
        implementation.write_text(source, encoding="utf-8")


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class ResourceLoaderContractTests(unittest.TestCase):
    def test_prepare_only_fixture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root, "void PrepareResource();\n")
            result = run_checker(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_singleton_and_legacy_loader_entry_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                "auto& manager = ResourceManager::Get();\n"
                "IResource* Load(const char* path);\n",
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("ResourceManager singleton access", result.stdout)
            self.assertIn("legacy loader Load entry point", result.stdout)

    def test_mutable_catalog_and_fallback_capability_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(
                root,
                "void RegisterLoader();\n"
                "bool SupportsPreparedLoading() const;\n"
                "void FindOnlyByType();\n",
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("runtime loader registration", result.stdout)
            self.assertIn("prepared-loading fallback capability", result.stdout)
            self.assertIn("type-only loader selection", result.stdout)

    def test_non_noexcept_supports_contract_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            header = root / "Source/Runtime/ResourceRuntime/Include/ResourceRuntime/ResourceLoader.h"
            header.write_text(CONTRACT.replace(" const noexcept", " const"), encoding="utf-8")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("const noexcept", result.stdout)


if __name__ == "__main__":
    unittest.main()
