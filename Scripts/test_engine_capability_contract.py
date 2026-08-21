#!/usr/bin/env python3
"""Regression coverage for the engine capability contract gate."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
CHECKER = REPO_ROOT / "Scripts/check_engine_capability_contract.py"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def base_contract() -> dict:
    evidence = [{"path": "Tests/CMake/UnitTests.cmake", "contains": "CoreValidation"}]
    capabilities = [
        {
            "id": "core",
            "status": "integrated",
            "module": "Core",
            "runtimeComposition": "engine-required",
            "shippingSupported": True,
            "requiredEvidence": evidence,
        },
        {
            "id": "render-tools",
            "status": "partial",
            "module": "Render",
            "runtimeComposition": "runtime-optional",
            "shippingSupported": False,
            "requiredEvidence": evidence,
        },
        {
            "id": "ai",
            "status": "experimental",
            "module": "AI",
            "runtimeComposition": "not-composed",
            "shippingSupported": False,
            "requiredEvidence": evidence,
        },
        {
            "id": "editor",
            "status": "deferred",
            "module": "Editor",
            "runtimeComposition": "not-composed",
            "shippingSupported": False,
            "requiredEvidence": [],
        },
    ]
    return {
        "schema": "RVX.EngineCapabilities",
        "schemaVersion": 1,
        "product": {
            "positioning": "runtime-first cross-platform game-engine foundation",
            "cmakeMinimum": "3.25",
            "vcpkgDescription": "fixture description",
        },
        "buildContract": {
            "moduleManifest": "Docs/module-boundaries.json",
            "configurePresets": ["fixture"],
            "testPresets": ["fixture_unit"],
            "artifactContracts": [
                {
                    "id": "model",
                    "magic": "RVX_MODEL_ARTIFACT_V2",
                    "source": "Source/Runtime/ResourceRuntime/Include/Resource/Cooked/CookedModelArtifact.h",
                }
            ],
            "renderGraph": {
                "target": "RVX_RenderGraph",
                "alias": "RVX::RenderGraph",
                "manifestModule": "Render",
                "source": "Source/Runtime/Render/CMakeLists.txt",
            },
            "fbx": {"vcpkgFeature": "fbx", "shippingEnabled": False},
        },
        "documentationClasses": {
            "classificationPrecedence": [
                "currentContracts",
                "futureProposalRoots",
                "historicalEvidenceRoots",
                "historicalEvidenceGlobs",
            ],
            "currentContracts": [
                "README.md",
                "Docs/README.md",
                "Docs/engine-capabilities.json",
                "Docs/module-boundaries.json",
            ],
            "futureProposalRoots": ["Docs/RVX-NG"],
            "historicalEvidenceRoots": ["Docs/superpowers"],
            "historicalEvidenceGlobs": ["Docs/*.md"],
        },
        "capabilities": capabilities,
    }


def write_fixture(root: Path) -> None:
    contract = base_contract()
    manifest = {
        "schema": "RVX.ModuleBoundaryManifest",
        "schemaVersion": 2,
        "modules": [
            {
                "id": "Core",
                "shippingAllowed": True,
                "deferred": False,
                "allowedModules": ["Core"],
                "owner": "unassigned",
            },
            {
                "id": "Render",
                "shippingAllowed": True,
                "deferred": False,
                "allowedModules": ["Core", "Render"],
                "owner": "unassigned",
            },
            {
                "id": "AI",
                "shippingAllowed": True,
                "deferred": False,
                "allowedModules": ["AI", "Core"],
                "owner": "unassigned",
            },
            {
                "id": "Editor",
                "shippingAllowed": False,
                "deferred": True,
                "allowedModules": ["Editor"],
                "owner": "unassigned",
            },
            {
                "id": "Engine",
                "shippingAllowed": True,
                "deferred": False,
                "allowedModules": ["Core", "Render"],
                "owner": "unassigned",
            },
        ],
    }
    write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\n")
    write(
        root / "CMakePresets.json",
        json.dumps(
            {
                "configurePresets": [{"name": "fixture"}],
                "testPresets": [{"name": "fixture_unit"}],
            }
        ),
    )
    write(root / "vcpkg.json", json.dumps({"description": "fixture description", "features": {"fbx": {}}}))
    write(root / "Docs/module-boundaries.json", json.dumps(manifest))
    write(root / "Docs/engine-capabilities.json", json.dumps(contract))
    write(root / "README.md", "# Fixture\n")
    write(root / "Docs/README.md", "# Current documentation\n")
    (root / "Docs/RVX-NG").mkdir(parents=True)
    (root / "Docs/superpowers").mkdir(parents=True)
    write(root / "Tests/CMake/UnitTests.cmake", "NAME CoreValidation\n")
    write(
        root / "Source/Runtime/ResourceRuntime/Include/Resource/Cooked/CookedModelArtifact.h",
        'constexpr auto Magic = "RVX_MODEL_ARTIFACT_V2";\n',
    )
    write(
        root / "Source/Runtime/Render/CMakeLists.txt",
        "add_library(RVX_RenderGraph STATIC graph.cpp)\n"
        "add_library(RVX::RenderGraph ALIAS RVX_RenderGraph)\n"
        "rvx_register_module_target(TARGET RVX_RenderGraph MODULE Render)\n",
    )


def run_checker(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), "--root", str(root)],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def mutate_contract(root: Path, mutation) -> None:
    path = root / "Docs/engine-capabilities.json"
    value = json.loads(path.read_text(encoding="utf-8"))
    mutation(value)
    path.write_text(json.dumps(value), encoding="utf-8")


class EngineCapabilityContractTests(unittest.TestCase):
    def test_valid_contract_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            result = run_checker(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unknown_status_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            mutate_contract(root, lambda value: value["capabilities"][1].update(status="complete"))
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("invalid status", result.stdout)

    def test_unknown_module_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            mutate_contract(root, lambda value: value["capabilities"][0].update(module="Missing"))
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("unknown module", result.stdout)

    def test_public_preset_drift_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            presets = root / "CMakePresets.json"
            value = json.loads(presets.read_text(encoding="utf-8"))
            value["configurePresets"].append({"name": "unpublished"})
            presets.write_text(json.dumps(value), encoding="utf-8")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("configurePresets", result.stdout)

    def test_missing_evidence_token_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            mutate_contract(
                root,
                lambda value: value["capabilities"][0]["requiredEvidence"][0].update(
                    contains="MissingValidation"
                ),
            )
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("evidence source", result.stdout)

    def test_stale_current_api_example_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            write(root / "README.md", "ResourceManager::Get()\n")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("retired ResourceManager singleton", result.stdout)

    def test_unclassified_document_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_fixture(root)
            write(root / "Docs/unclassified.json", "{}\n")
            result = run_checker(root)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("no current/future/history class", result.stdout)


if __name__ == "__main__":
    unittest.main()
