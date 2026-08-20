#!/usr/bin/env python3
"""Self-tests for the cooked-only Shipping evidence gate."""

from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from architecture_gate_common import GateInputError
from check_shipping_evidence import (
    RECEIPT_SCHEMA,
    REPORT_SCHEMA,
    validate_receipt,
    validate_shipping_evidence,
)


COMMIT = "0123456789abcdef0123456789abcdef01234567"
PRESET = "win-x64-shipping-release"
BACKEND = "DX12"


class EvidenceFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.receipt_path = root / "shipping-fixture-receipt.json"
        self.report_path = root / "shipping-evidence.json"
        source_model = root / "Tests/Fixtures/ModelViewer/R7Triangle.gltf"
        shader_catalog = root / "Render/Shaders/Programs.rvxshadercatalog.json"
        source_model.parent.mkdir(parents=True, exist_ok=True)
        shader_catalog.parent.mkdir(parents=True, exist_ok=True)
        source_model.write_bytes(b"hermetic-r7-triangle\n")
        shader_catalog.write_bytes(b"{\"schema\":\"test-catalog\"}\n")
        files = {
            "Cooked/Resources/CookManifest.rvxmanifest": b"manifest-v2\n",
            "Cooked/Resources/R7Triangle.rva": b"model-v1\n",
            "Cooked/Shaders/DX12/0000000000000001.rvxshader": b"shader-a\n",
            "Cooked/Shaders/DX12/0000000000000002.rvxshader": b"shader-b\n",
        }
        artifacts: list[dict[str, object]] = []
        payload = ""
        for relative, content in sorted(files.items()):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            digest = hashlib.sha256(content).hexdigest()
            size = len(content)
            artifacts.append({"path": relative, "size": size, "sha256": digest})
            payload += f"{relative}|{size}|{digest}\n"
        self.receipt: dict[str, object] = {
            "schema": RECEIPT_SCHEMA,
            "schemaVersion": 1,
            "commit": COMMIT,
            "preset": PRESET,
            "backend": BACKEND,
            "sourceModel": "Tests/Fixtures/ModelViewer/R7Triangle.gltf",
            "sourceModelSha256": hashlib.sha256(source_model.read_bytes()).hexdigest(),
            "shaderCatalog": "Render/Shaders/Programs.rvxshadercatalog.json",
            "shaderCatalogSha256": hashlib.sha256(shader_catalog.read_bytes()).hexdigest(),
            "cookExecutableSha256": "2" * 64,
            "resourceManifest": "Cooked/Resources/CookManifest.rvxmanifest",
            "resourceEntryCount": 1,
            "shaderProgramCount": 2,
            "artifactCount": len(artifacts),
            "artifactPayloadHash": hashlib.sha256(payload.encode()).hexdigest(),
            "artifacts": artifacts,
        }
        self.write_receipt()
        self.report: dict[str, object] = {
            "schema": REPORT_SCHEMA,
            "schemaVersion": 1,
            "commit": COMMIT,
            "preset": PRESET,
            "frames": 8,
            "width": 320,
            "height": 180,
            "quality": "Low",
            "renderPath": "Direct",
            "requestedBackend": BACKEND,
            "actualBackend": BACKEND,
            "fallbackUsed": False,
            "fallbackReason": "",
            "cookedLoadSucceeded": True,
            "sourceLoadRejected": True,
            "shutdownClean": True,
            "fixtureReceiptSha256": self.receipt_hash(),
            "fixtureArtifactPayloadHash": self.receipt["artifactPayloadHash"],
        }
        self.write_report()

    def write_receipt(self) -> None:
        self.receipt_path.write_text(
            json.dumps(self.receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def receipt_hash(self) -> str:
        return hashlib.sha256(self.receipt_path.read_bytes()).hexdigest()

    def write_report(self) -> None:
        self.report_path.write_text(
            json.dumps(self.report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )


class ShippingEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.fixture = EvidenceFixture(Path(self.temporary.name))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def validate(self) -> list[str]:
        return validate_shipping_evidence(
            self.fixture.report_path,
            self.fixture.receipt_path,
            repository_root=self.fixture.root,
            expected_backend=BACKEND,
            expected_commit=COMMIT,
            expected_preset=PRESET,
        )

    def test_accepts_exact_cooked_only_evidence(self) -> None:
        receipt, paths = validate_receipt(self.fixture.receipt_path)
        self.assertEqual(receipt["shaderProgramCount"], 2)
        self.assertEqual(len(paths), 4)
        self.assertEqual(self.validate(), [])

    def test_missing_report_field_fails_closed(self) -> None:
        del self.fixture.report["sourceLoadRejected"]
        self.fixture.write_report()
        with self.assertRaisesRegex(GateInputError, "fields are invalid"):
            self.validate()

    def test_unknown_report_field_fails_closed(self) -> None:
        self.fixture.report["optionalFallback"] = False
        self.fixture.write_report()
        with self.assertRaisesRegex(GateInputError, "unknown=.*optionalFallback"):
            self.validate()

    def test_rejects_wrong_scene_shape_and_unclean_shutdown(self) -> None:
        self.fixture.report["frames"] = 7
        self.fixture.report["width"] = 1920
        self.fixture.report["renderPath"] = "GPUDriven"
        self.fixture.report["shutdownClean"] = False
        self.fixture.write_report()
        findings = self.validate()
        self.assertTrue(any("report.frames" in item for item in findings))
        self.assertTrue(any("report.width" in item for item in findings))
        self.assertTrue(any("report.renderPath" in item for item in findings))
        self.assertTrue(any("report.shutdownClean" in item for item in findings))

    def test_rejects_backend_fallback_even_when_fallback_flag_is_false(self) -> None:
        self.fixture.report["actualBackend"] = "Vulkan"
        self.fixture.write_report()
        findings = self.validate()
        self.assertTrue(any("backend fallback is forbidden" in item for item in findings))

    def test_rejects_tampered_artifact(self) -> None:
        (self.fixture.root / "Cooked/Resources/R7Triangle.rva").write_bytes(
            b"tampered\n"
        )
        with self.assertRaisesRegex(GateInputError, "does not match its receipt"):
            self.validate()

    def test_rejects_unlisted_file_in_cooked_tree(self) -> None:
        extra = self.fixture.root / "Cooked/Resources/unlisted.rva"
        extra.write_bytes(b"unlisted\n")
        with self.assertRaisesRegex(GateInputError, "does not exactly match"):
            self.validate()

    def test_rejects_receipt_hash_and_payload_identity_mismatch(self) -> None:
        self.fixture.report["fixtureReceiptSha256"] = "3" * 64
        self.fixture.report["fixtureArtifactPayloadHash"] = "4" * 64
        self.fixture.write_report()
        findings = self.validate()
        self.assertTrue(any("receipt SHA-256 mismatch" in item for item in findings))
        self.assertTrue(any("artifact payload hash mismatch" in item for item in findings))

    def test_rejects_source_acceptance_and_cooked_load_failure(self) -> None:
        self.fixture.report["sourceLoadRejected"] = False
        self.fixture.report["cookedLoadSucceeded"] = False
        self.fixture.write_report()
        findings = self.validate()
        self.assertTrue(any("sourceLoadRejected" in item for item in findings))
        self.assertTrue(any("cookedLoadSucceeded" in item for item in findings))

    def test_rejects_unsafe_or_duplicate_receipt_artifacts(self) -> None:
        original = copy.deepcopy(self.fixture.receipt)
        artifacts = self.fixture.receipt["artifacts"]
        assert isinstance(artifacts, list)
        artifacts[0]["path"] = "Cooked/../source.gltf"
        self.fixture.write_receipt()
        with self.assertRaisesRegex(GateInputError, "unsafe"):
            validate_receipt(self.fixture.receipt_path)

        self.fixture.receipt = original
        artifacts = self.fixture.receipt["artifacts"]
        assert isinstance(artifacts, list)
        artifacts[1] = copy.deepcopy(artifacts[0])
        self.fixture.write_receipt()
        with self.assertRaises(GateInputError):
            validate_receipt(self.fixture.receipt_path)

    def test_boolean_fields_do_not_accept_integer_aliases(self) -> None:
        self.fixture.report["fallbackUsed"] = 0
        self.fixture.write_report()
        with self.assertRaisesRegex(GateInputError, "must be a boolean"):
            self.validate()

    def test_repository_source_hash_drift_is_rejected(self) -> None:
        (self.fixture.root / "Tests/Fixtures/ModelViewer/R7Triangle.gltf").write_bytes(
            b"drifted source\n"
        )
        findings = self.validate()
        self.assertTrue(any("source model hash drifted" in item for item in findings))


if __name__ == "__main__":
    unittest.main()
