#!/usr/bin/env python3
"""Hermetic bad-fixture coverage for the GFX24 qualification contracts."""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import render_quality_performance_baseline as performance_baseline


ROOT = Path(__file__).resolve().parents[1]
BUNDLE_CHECKER = ROOT / "Scripts/check_render_quality_bundle.py"
PERFORMANCE_CHECKER = ROOT / "Scripts/check_render_quality_performance.py"
PERFORMANCE_BASELINE_TOOL = (
    ROOT / "Scripts/render_quality_performance_baseline.py"
)
SEMANTICS = {
    "BeautyLDR": ("RGBA32_FLOAT", "LDR", 4),
    "PreToneHDR": ("RGBA32_FLOAT", "HDR", 4),
    "LinearDepth": ("R32_FLOAT", "LinearDepth", 1),
    "Normal": ("RGBA32_FLOAT", "WorldNormal", 4),
    "Velocity": ("RG32_FLOAT", "Motion", 2),
    "AmbientOcclusion": ("R32_FLOAT", "LinearMask", 1),
    "ReactiveMask": ("R32_FLOAT", "LinearMask", 1),
}
IDENTITY = {
    "sampleId": "render-quality-lab",
    "pipeline": "deferred",
    "quality": "high",
    "backend": "DX12",
    "view": "hero",
    "captureTick": 120,
    "visualContractVersion": 1,
}
PERFORMANCE_IDENTITY = {
    **IDENTITY,
    "qualificationWidth": 2560,
    "qualificationHeight": 1440,
}
HARDWARE = {
    "backend": "DX12",
    "adapterName": "NVIDIA GeForce RTX 4080",
    "driverVersion": "555.1",
    "osBuild": "windows-26100",
    "capabilityHash": "rvx-rhi-capabilities-dx12-v1-5ef7bade",
}
TELEMETRY_COVERAGE = {
    "schemaVersion": 2,
    "trackedUploadCoverageMask": 7,
    "trackedEstimatedFootprintCoverageMask": 15,
}


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_bundle(
    root: Path,
    *,
    name: str,
    completed_frame: int,
    source_commit: str = "commit-a",
    run_id: str = "run-a",
    preset: str = "win_x64_debug_gfx24_qualification",
    capture_tick: int = 120,
    receipt: str = "a" * 64,
) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    attachments = []
    for ordinal, (semantic, (format_name, domain, channels)) in enumerate(SEMANTICS.items()):
        payload = struct.pack("<" + "f" * (2 * 2 * channels), *([0.1 + ordinal * 0.01] * (2 * 2 * channels)))
        payload_path = root / f"{semantic}.raw"
        payload_path.write_bytes(payload)
        attachments.append({
            "semantic": semantic,
            "dataPath": payload_path.name,
            "format": format_name,
            "colorDomain": domain,
            "width": 2,
            "height": 2,
            "channels": channels,
            "scalarType": "float32le",
            "rowPitchBytes": 2 * channels * 4,
            "payloadBytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "provenance": {
                "sourceCommit": source_commit,
                "runId": run_id,
                "preset": preset,
                "renderExecutionReceiptSha256": receipt,
                "frameSequence": completed_frame,
                "submittedFrame": completed_frame,
                "completedFrame": completed_frame,
                "sceneRevision": 4,
                "surfaceGeneration": 1,
                "configurationRevision": 3,
                "viewId": "main",
                "viewSceneRuntimeId": 5,
                "viewCameraEntityPacked": 9,
                "viewSubViewIndex": 0,
                "viewRole": "Main",
                "graphIdentity": 14,
                "graphRecordingGeneration": 3,
                "graphRecordEpoch": 18,
                "graphResourceIdentity": ordinal + 1,
                "graphResourceGeneration": 2,
                "producerIdentity": ordinal + 21,
                "resourceName": semantic,
                "graphPasses": ["QualificationCapture"],
            },
        })
    manifest = {
        "schema": "RVX.RenderQualityAttachmentBundle",
        "schemaVersion": 1,
        "bundleId": name,
        "sourceCommit": source_commit,
        "qualificationIdentity": {**IDENTITY, "captureTick": capture_tick},
        "execution": {
            "runId": run_id,
            "preset": preset,
            "renderExecutionReceiptSha256": receipt,
            "completionOwned": True,
            "frameSequence": completed_frame,
            "submittedFrame": completed_frame,
            "completedFrame": completed_frame,
            "sceneRevision": 4,
            "surfaceGeneration": 1,
            "configurationRevision": 3,
            "viewId": "main",
            "viewSceneRuntimeId": 5,
            "viewCameraEntityPacked": 9,
            "viewSubViewIndex": 0,
            "viewRole": "Main",
            "graphIdentity": 14,
            "graphRecordingGeneration": 3,
            "graphRecordEpoch": 18,
        },
        "attachments": attachments,
    }
    path = root / "bundle.json"
    write_json(path, manifest)
    return path


def write_sample_manifest(path: Path) -> Path:
    write_json(path, {
        "schema": "RVX.SampleManifest",
        "schemaVersion": 1,
        "samples": [{
            "id": "render-quality-lab",
            "captureTicks": [120, 360],
            "roiMetricPolicy": {
                "regionsOfInterest": [{
                    "id": "frame", "x": 0.0, "y": 0.0,
                    "width": 1.0, "height": 1.0,
                }],
                "metrics": [],
            },
        }],
    })
    return path


def write_visual_policy(path: Path) -> Path:
    attachments = {}
    for semantic in SEMANTICS:
        attachments[semantic] = {"rois": [{
            "id": "full",
            "normalizedRegionId": "frame",
            "x": 0,
            "y": 0,
            "width": 2,
            "height": 2,
            "temporalMode": "static" if semantic != "Velocity" else "dynamic",
            "dataRange": 1.0,
            "maxMae": 0.0,
            "maxAbs": 0.0,
            "minPsnr": 90.0,
            "minSsim": 1.0,
            "maxTemporalMae": 0.0,
        }]}
    write_json(path, {
        "schema": "RVX.RenderQualityVisualPolicy",
        "schemaVersion": 1,
        "qualificationIdentity": IDENTITY,
        "attachments": attachments,
        "temporal": {"warmupCompletedFrames": 60, "minimumSamples": 2},
    })
    return path


def run_bundle(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(BUNDLE_CHECKER), *map(str, arguments)], cwd=ROOT, text=True, capture_output=True, check=False)


def sample(
    frame: int,
    *,
    textures: int = 12,
    imported_history_bytes: int = 88_473_600,
) -> dict[str, object]:
    return {
        "frameSequence": frame + 1,
        "submittedFrame": frame,
        "completedFrame": frame,
        "renderExecutionReceiptSha256": f"{frame + 1:064x}",
        "completionOwned": True,
        "cpuMs": {"extraction": 1.0, "visibility": 1.1, "graphCompile": 0.2, "submission": 0.6},
        "gpu": {
            "graphicsTerminalCommandSpanMs": 7.0,
            "renderGraphPassEnvelopeMs": 6.5,
            "graphicsQualificationSpanMs": 7.0,
            "passes": [
                {"executionIndex": 0, "name": "Geometry", "durationMs": 2.0},
                {"executionIndex": 1, "name": "Lighting", "durationMs": 1.5},
                {"executionIndex": 2, "name": "Geometry", "durationMs": 0.5},
            ],
        },
        "trackedUploadBytes": 2048,
        "trackedUploadCoverageMask": 7,
        "trackedEstimatedFootprint": {
            "schemaVersion": 2,
            "coverageMask": 15,
            "transientPoolPhysicalEstimatedBytes": 4096,
            "importedPersistentEstimatedBytes": 4096,
            "importedHistoryEstimatedBytes": imported_history_bytes,
            "resourceRegistryResidentEstimatedBytes": 2048,
        },
        "resourceCounts": {
            "schemaVersion": 2,
            "counts": {
                "stableImportedBuffers": 1,
                "stableImportedTextures": 7,
                "residentMaterials": 6,
                "residentMeshes": 4,
                "residentTextures": textures,
                "transientBuffers": 2,
                "transientTextures": 3,
            },
        },
    }


def write_performance(path: Path, *, warmup: int = 60, count: int = 300, hardware: dict[str, str] | None = None, grow: bool = False) -> Path:
    samples = [sample(warmup + index) for index in range(count)]
    if grow and samples:
        samples[-1]["resourceCounts"]["counts"]["residentTextures"] = 13
    write_json(path, {
        "schema": "RVX.RenderQualityPerformanceReport",
        "schemaVersion": 3,
        "sourceCommit": "commit-a",
        "runId": "run-a",
        "preset": "win_x64_debug_gfx24_qualification",
        "qualificationIdentity": PERFORMANCE_IDENTITY,
        "hardwareIdentity": HARDWARE if hardware is None else hardware,
        "telemetryCoverage": TELEMETRY_COVERAGE,
        "baselineIdentity": "dx12-rtx4080-ci-v1",
        "completedWarmupFrames": warmup,
        "samples": samples,
    })
    return path


def write_performance_policy(path: Path) -> Path:
    write_json(path, {
        "schema": "RVX.RenderQualityPerformancePolicy",
        "schemaVersion": 3,
        "qualificationIdentity": PERFORMANCE_IDENTITY,
        "requiredHardwareIdentity": HARDWARE,
        "telemetryCoverage": TELEMETRY_COVERAGE,
        "warmupCompletedFrames": 60,
        "minimumCompletedSamples": 300,
        "baseline": {"id": "dx12-rtx4080-ci-v1", "maxRegressionPercent": 10.0},
        "thresholds": {
            "graphicsQualificationSpanP95Ms": 16.67,
            "renderGraphPassEnvelopeP95Ms": 15.0,
            "cpuP95Ms": {"extraction": 5.0, "visibility": 5.0, "graphCompile": 5.0, "submission": 5.0},
            "passGpuP95Ms": [
                {"executionIndex": 0, "name": "Geometry", "p95Ms": 5.0},
                {"executionIndex": 1, "name": "Lighting", "p95Ms": 5.0},
                {"executionIndex": 2, "name": "Geometry", "p95Ms": 5.0},
            ],
            "maxTrackedUploadBytes": 4096,
            "maxTransientPoolPhysicalEstimatedBytes": 8192,
            "maxImportedPersistentEstimatedBytes": 8192,
            "minImportedHistoryEstimatedBytes": 88_473_600,
            "maxImportedHistoryEstimatedBytes": 90_000_000,
            "maxResourceRegistryResidentEstimatedBytes": 4096,
        },
    })
    return path


def write_performance_baseline_record(
    path: Path,
    baseline: Path,
    policy: Path,
    *,
    approved: bool = True,
) -> Path:
    report = json.loads(baseline.read_text())
    policy_document = json.loads(policy.read_text())
    graphics_values = sorted(
        sample_value["gpu"]["graphicsQualificationSpanMs"]
        for sample_value in report["samples"]
    )
    graph_values = sorted(
        sample_value["gpu"]["renderGraphPassEnvelopeMs"]
        for sample_value in report["samples"]
    )
    index = (len(graphics_values) * 95 + 99) // 100 - 1
    selected_graphics = graphics_values[index]
    selected_graph = graph_values[index]
    report_sha = hashlib.sha256(baseline.read_bytes()).hexdigest()
    policy_sha = hashlib.sha256(policy.read_bytes()).hexdigest()
    run_metrics = [
        selected_graphics * 0.8,
        selected_graphics * 0.9,
        selected_graphics,
        selected_graphics * 1.1,
        selected_graphics * 1.2,
    ]
    runs = []
    receipts = path.parent / "receipts"
    receipts.mkdir(exist_ok=True)
    for ordinal, metric in enumerate(run_metrics, start=1):
        run = {
            "ordinal": ordinal,
            "runId": report["runId"] if ordinal == 3 else f"run-{ordinal}",
            "reportSha256": report_sha if ordinal == 3 else hashlib.sha256(
                f"report-{ordinal}".encode()
            ).hexdigest(),
            "graphicsQualificationSpanP95Ms": metric,
            "renderGraphPassEnvelopeP95Ms": (
                selected_graph if ordinal == 3 else selected_graph * metric / selected_graphics
            ),
        }
        receipt_path = receipts / f"{ordinal:02d}.json"
        write_json(receipt_path, {
            "schema": "RVX.AgentHarness.RequiredSuiteReceipt",
            "schemaVersion": 3,
            "runId": run["runId"],
            "immutableReceiptPath": (
                ".agent-harness/receipts/"
                f"RequiredSuite.receipt-{ordinal}.json"
            ),
            "source": {
                "commit": report["sourceCommit"],
                "fingerprintBefore": {
                    "algorithm": "sha256:path-state-content-v1",
                    "fileCount": 1,
                    "digest": "f" * 64,
                },
                "fingerprintAfter": {
                    "algorithm": "sha256:path-state-content-v1",
                    "fileCount": 1,
                    "digest": "f" * 64,
                },
            },
            "junit": {
                "outcome": {
                    "testCaseCount": 1,
                    "failureCount": 0,
                    "errorCount": 0,
                    "skippedCount": 0,
                    "notRunCount": 0,
                    "disabledSuiteCount": 0,
                },
            },
            "requiredLabel": "suite.gfx24-performance-calibration",
            "selectedTests": [
                "RenderVerseSamplesCookedDX12Gfx24DeferredHighPerformanceCalibration"
            ],
            "semanticArtifacts": [{
                "path": "performance.json",
                "sha256": run["reportSha256"],
                "size": 1,
            }],
        })
        run["requiredSuiteReceiptPath"] = f"receipts/{ordinal:02d}.json"
        run["requiredSuiteReceiptSha256"] = hashlib.sha256(
            receipt_path.read_bytes()
        ).hexdigest()
        runs.append(run)
    write_json(path, {
        "schema": "RVX.RenderQualityPerformanceBaselineRecord",
        "schemaVersion": 1,
        "baselineId": report["baselineIdentity"],
        "status": "approved" if approved else "candidate",
        "source": {
            "commit": report["sourceCommit"],
            "fingerprint": {
                "algorithm": "sha256:path-state-content-v1",
                "fileCount": 1,
                "digest": "f" * 64,
            },
        },
        "qualificationIdentity": report["qualificationIdentity"],
        "hardwareIdentity": report["hardwareIdentity"],
        "baseline": {
            "reportPath": baseline.name,
            "reportSha256": report_sha,
            "runId": report["runId"],
            "preset": report["preset"],
            "requiredSuiteReceiptPath": runs[2]["requiredSuiteReceiptPath"],
            "requiredSuiteReceiptSha256": runs[2]["requiredSuiteReceiptSha256"],
        },
        "policy": {"path": policy.name, "sha256": policy_sha},
        "calibration": {
            "requiredRunCount": 5,
            "selectionMetric": "graphicsQualificationSpanP95Ms",
            "selectionRule": "median-p95",
            "selectedRunOrdinal": 3,
            "runs": runs,
        },
        "approval": {
            "approvedBy": "Render Team" if approved else None,
            "approvedAtUtc": "2026-08-31T00:00:00Z" if approved else None,
            "reason": "Reviewed fixed-GPU calibration" if approved else None,
        },
    })
    return path


def run_performance(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(PERFORMANCE_CHECKER), *map(str, arguments)], cwd=ROOT, text=True, capture_output=True, check=False)


def write_calibration_run(
    root: Path,
    *,
    ordinal: int,
    graphics_ms: float,
) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    report_path = write_performance(root / "performance.json")
    report = json.loads(report_path.read_text())
    report["runId"] = f"calibration-{ordinal}"
    report["preset"] = (
        "win_x64_gfx24_performance_calibration_release_run"
    )
    for report_sample in report["samples"]:
        report_sample["gpu"]["graphicsTerminalCommandSpanMs"] = graphics_ms
        report_sample["gpu"]["renderGraphPassEnvelopeMs"] = graphics_ms - 0.5
        report_sample["gpu"]["graphicsQualificationSpanMs"] = graphics_ms
        report_sample["renderExecutionReceiptSha256"] = hashlib.sha256(
            (
                f"calibration-{ordinal}-"
                f"{report_sample['frameSequence']}"
            ).encode()
        ).hexdigest()
    write_json(report_path, report)
    report_sha = hashlib.sha256(report_path.read_bytes()).hexdigest()
    receipt_path = root / "required-suite-receipt.json"
    write_json(receipt_path, {
        "schema": "RVX.AgentHarness.RequiredSuiteReceipt",
        "schemaVersion": 3,
        "runId": report["runId"],
        "immutableReceiptPath": (
            ".agent-harness/receipts/"
            f"RequiredSuite.calibration-{ordinal}.json"
        ),
        "source": {
            "commit": report["sourceCommit"],
            "fingerprintBefore": {
                "algorithm": "sha256:path-state-content-v1",
                "fileCount": 1,
                "digest": "e" * 64,
            },
            "fingerprintAfter": {
                "algorithm": "sha256:path-state-content-v1",
                "fileCount": 1,
                "digest": "e" * 64,
            },
        },
        "junit": {
            "outcome": {
                "testCaseCount": 1,
                "failureCount": 0,
                "errorCount": 0,
                "skippedCount": 0,
                "notRunCount": 0,
                "disabledSuiteCount": 0,
            },
        },
        "requiredLabel": "suite.gfx24-performance-calibration",
        "selectedTests": [
            "RenderVerseSamplesCookedDX12Gfx24DeferredHighPerformanceCalibration"
        ],
        "semanticArtifacts": [{
            "path": str(report_path),
            "sha256": report_sha,
            "size": report_path.stat().st_size,
        }],
    })
    return root


def run_baseline_tool(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(PERFORMANCE_BASELINE_TOOL), *map(str, arguments)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


class RenderQualityBundleTests(unittest.TestCase):
    def _valid_arguments(self, root: Path) -> list[Path | str]:
        current = write_bundle(
            root / "current",
            name="current",
            completed_frame=120,
            capture_tick=120,
            receipt="a" * 64,
        )
        baseline = write_bundle(
            root / "baseline",
            name="baseline",
            completed_frame=120,
            capture_tick=120,
            receipt="b" * 64,
        )
        later = write_bundle(
            root / "later",
            name="later",
            completed_frame=360,
            capture_tick=360,
            receipt="c" * 64,
        )
        policy = write_visual_policy(root / "policy.json")
        sample_manifest = write_sample_manifest(root / "sample-manifest.json")
        return [
            "--bundle", current,
            "--baseline", baseline,
            "--policy", policy,
            "--sample-manifest", sample_manifest,
            "--temporal-bundle", later,
            "--expected-source-commit", "commit-a",
            "--expected-run-id", "run-a",
            "--expected-preset", "win_x64_debug_gfx24_qualification",
            "--expected-receipt-sha256", "a" * 64,
        ]

    def test_valid_bundle_compares_all_semantics_and_writes_heatmaps(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            result = run_bundle(*arguments, "--heatmap-dir", root / "heatmaps", "--report", root / "result.json")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((root / "heatmaps/BeautyLDR.diff.ppm").is_file())
            self.assertEqual(json.loads((root / "result.json").read_text())["status"], "passed")

    def test_missing_semantic_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"].pop()
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("exactly", result.stderr)

    def test_truncated_payload_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            payload = root / "current/BeautyLDR.raw"
            payload.write_bytes(payload.read_bytes()[:-1])
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("truncated", result.stderr)

    def test_hash_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"][0]["sha256"] = "b" * 64
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("SHA-256", result.stderr)

    def test_nan_payload_fails_after_hash_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            payload = root / "current/BeautyLDR.raw"
            data = bytearray(payload.read_bytes())
            data[:4] = struct.pack("<f", float("nan"))
            payload.write_bytes(data)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"][0]["sha256"] = hashlib.sha256(data).hexdigest()
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("NaN", result.stderr)

    def test_padded_rows_are_not_canonical_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            attachment = document["attachments"][0]
            attachment["rowPitchBytes"] += 4
            attachment["payloadBytes"] = (
                attachment["rowPitchBytes"] * attachment["height"]
            )
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("canonical tight", result.stderr)

    def test_receipt_provenance_and_stale_commit_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"][0]["provenance"]["renderExecutionReceiptSha256"] = "c" * 64
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("provenance", result.stderr)
            document = json.loads(current.read_text())
            document["attachments"][0]["provenance"]["renderExecutionReceiptSha256"] = "a" * 64
            document["sourceCommit"] = "old-commit"
            for attachment in document["attachments"]:
                attachment["provenance"]["sourceCommit"] = "old-commit"
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("stale", result.stderr)

    def test_graph_and_view_provenance_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"][0]["provenance"]["graphRecordEpoch"] = 19
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("graphRecordEpoch provenance", result.stderr)

            document = json.loads(current.read_text())
            document["attachments"][0]["provenance"]["graphRecordEpoch"] = 18
            document["attachments"][0]["provenance"]["viewId"] = "secondary"
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("viewId provenance", result.stderr)

    def test_extent_and_aggregate_payload_limits_fail_before_raw_file_reads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["attachments"][0]["width"] = 8193
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("extent exceeds", result.stderr)

            document = json.loads(current.read_text())
            document["attachments"][0]["width"] = 2
            for attachment in document["attachments"]:
                channels = attachment["channels"]
                attachment["width"] = 4096
                attachment["height"] = 4096
                attachment["rowPitchBytes"] = 4096 * channels * 4
                attachment["payloadBytes"] = attachment["rowPitchBytes"] * 4096
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("aggregate", result.stderr)

    def test_temporal_identity_receipt_and_normalized_mapping_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            later = Path(arguments[arguments.index("--temporal-bundle") + 1])
            document = json.loads(later.read_text())
            document["execution"]["preset"] = "another-preset"
            for attachment in document["attachments"]:
                attachment["provenance"]["preset"] = "another-preset"
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("temporal bundle preset", result.stderr)

            document = json.loads(later.read_text())
            document["execution"]["preset"] = "win_x64_debug_gfx24_qualification"
            document["execution"]["renderExecutionReceiptSha256"] = "a" * 64
            for attachment in document["attachments"]:
                attachment["provenance"]["preset"] = "win_x64_debug_gfx24_qualification"
                attachment["provenance"]["renderExecutionReceiptSha256"] = "a" * 64
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("reuse a render execution receipt", result.stderr)

            later = write_bundle(
                root / "later",
                name="later",
                completed_frame=360,
                capture_tick=360,
                receipt="c" * 64,
            )
            policy = Path(arguments[arguments.index("--policy") + 1])
            policy_document = json.loads(policy.read_text())
            policy_document["attachments"]["BeautyLDR"]["rois"][0]["width"] = 1
            write_json(policy, policy_document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("deterministic normalized mapping", result.stderr)

    def test_cross_tick_temporal_sequence_rejects_undeclared_or_misaligned_ticks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            sample_manifest = Path(arguments[arguments.index("--sample-manifest") + 1])
            manifest_document = json.loads(sample_manifest.read_text())
            manifest_document["samples"][0]["captureTicks"] = [360, 120]
            write_json(sample_manifest, manifest_document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("captureTicks must be strictly increasing", result.stderr)
            write_sample_manifest(sample_manifest)

            later = Path(arguments[arguments.index("--temporal-bundle") + 1])
            document = json.loads(later.read_text())
            document["qualificationIdentity"]["captureTick"] = 240
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("not declared", result.stderr)

            document["qualificationIdentity"]["captureTick"] = 120
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("capture ticks are stale", result.stderr)

            document["qualificationIdentity"]["captureTick"] = 360
            document["execution"]["frameSequence"] = 359
            for attachment in document["attachments"]:
                attachment["provenance"]["frameSequence"] = 359
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("predates its declared captureTick", result.stderr)


class RenderQualityPerformanceTests(unittest.TestCase):
    def _valid_arguments(self, root: Path) -> list[Path | str]:
        current = write_performance(root / "current.json")
        baseline = write_performance(root / "baseline.json")
        policy = write_performance_policy(root / "policy.json")
        baseline_record = write_performance_baseline_record(
            root / "baseline-record.json",
            baseline,
            policy,
        )
        return [
            "--report", current,
            "--baseline", baseline,
            "--policy", policy,
            "--expected-source-commit", "commit-a",
            "--expected-run-id", "run-a",
            "--expected-preset", "win_x64_debug_gfx24_qualification",
            "--baseline-record", baseline_record,
        ]

    def test_valid_completion_owned_series_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = run_performance(*self._valid_arguments(root), "--out", root / "result.json")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads((root / "result.json").read_text())["status"], "passed")

    def test_candidate_and_tampered_baseline_record_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            record = Path(arguments[-1])
            document = json.loads(record.read_text())
            document["status"] = "candidate"
            document["approval"] = {
                "approvedBy": None,
                "approvedAtUtc": None,
                "reason": None,
            }
            write_json(record, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("not approved", result.stderr)

            arguments = self._valid_arguments(root)
            Path(arguments[3]).write_text(
                Path(arguments[3]).read_text() + " ",
                encoding="utf-8",
            )
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("hash", result.stderr)

            arguments = self._valid_arguments(root)
            receipt = root / "receipts" / "01.json"
            receipt.write_text(receipt.read_text() + " ", encoding="utf-8")
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("receipt hash", result.stderr)

    def test_warmup_below_sixty_and_insufficient_samples_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            write_performance(Path(arguments[1]), warmup=59)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("warmup", result.stderr)
            write_performance(Path(arguments[1]), count=299)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("fewer", result.stderr)

    def test_hardware_mismatch_is_not_comparable_never_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            wrong_hardware = dict(HARDWARE)
            wrong_hardware["adapterName"] = "Different GPU"
            write_performance(Path(arguments[1]), hardware=wrong_hardware)
            result = run_performance(*arguments, "--out", root / "result.json")
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads((root / "result.json").read_text())["status"], "notComparable")

    def test_stale_commit_and_reused_receipt_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["sourceCommit"] = "old-commit"
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("stale", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][1]["renderExecutionReceiptSha256"] = (
                document["samples"][0]["renderExecutionReceiptSha256"]
            )
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("reuses", result.stderr)

    def test_submitted_frame_regression_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["samples"][1]["submittedFrame"] = document["samples"][0]["submittedFrame"]
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("submitted frame", result.stderr)

    def test_post_warmup_telemetry_growth_and_missing_field_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            write_performance(Path(arguments[1]), grow=True)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("count changed", result.stderr)
            write_performance(Path(arguments[1]))
            document = json.loads(Path(arguments[1]).read_text())
            del document["samples"][0]["cpuMs"]["visibility"]
            write_json(Path(arguments[1]), document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("missing", result.stderr)

            write_performance(Path(arguments[1]))
            document = json.loads(Path(arguments[1]).read_text())
            document["samples"][-1]["trackedEstimatedFootprint"][
                "importedHistoryEstimatedBytes"
            ] += 1
            write_json(Path(arguments[1]), document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("footprint changed", result.stderr)

            write_performance(Path(arguments[1]))
            document = json.loads(Path(arguments[1]).read_text())
            document["samples"][-1]["trackedEstimatedFootprint"][
                "importedPersistentEstimatedBytes"
            ] += 1
            write_json(Path(arguments[1]), document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("footprint changed", result.stderr)

            write_performance(Path(arguments[1]))
            document = json.loads(Path(arguments[1]).read_text())
            document["samples"][-1]["trackedEstimatedFootprint"][
                "importedPersistentEstimatedBytes"
            ] -= 1
            write_json(Path(arguments[1]), document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("footprint changed", result.stderr)

    def test_tracked_telemetry_coverage_and_legacy_fields_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            baseline = Path(arguments[3])
            document = json.loads(current.read_text())
            document["telemetryCoverage"]["trackedUploadCoverageMask"] = 3
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("coverage", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["telemetryCoverage"][
                "trackedEstimatedFootprintCoverageMask"
            ] = 7
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("coverage", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][0]["trackedEstimatedFootprint"]["coverageMask"] = 3
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("coverage", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][0]["uploadBytes"] = document["samples"][0].pop(
                "trackedUploadBytes"
            )
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("fields", result.stderr)

            write_performance(current)
            write_performance(baseline)
            policy = Path(arguments[5])
            document = json.loads(policy.read_text())
            del document["thresholds"]["maxImportedPersistentEstimatedBytes"]
            write_json(policy, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("thresholds", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            counts = document["samples"][0]["resourceCounts"]["counts"]
            counts["persistentImportedTextures"] = counts.pop(
                "stableImportedTextures"
            )
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("persistent/history", result.stderr)

            write_performance(current)
            document = json.loads(baseline.read_text())
            del document["telemetryCoverage"]
            write_json(baseline, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("telemetryCoverage", result.stderr)

            write_performance(baseline)
            write_performance(current)
            document = json.loads(current.read_text())
            footprint = document["samples"][0]["trackedEstimatedFootprint"]
            footprint["importedHistoryNamedEstimatedBytes"] = footprint.pop(
                "importedHistoryEstimatedBytes"
            )
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("fields", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            del document["samples"][0]["trackedEstimatedFootprint"][
                "importedPersistentEstimatedBytes"
            ]
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("fields", result.stderr)

    def test_v2_reports_and_policies_are_rejected_without_compatibility_readers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            baseline = Path(arguments[3])
            policy = Path(arguments[5])

            document = json.loads(current.read_text())
            document["schemaVersion"] = 2
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("schema/version", result.stderr)

            write_performance(current)
            document = json.loads(baseline.read_text())
            document["schemaVersion"] = 2
            write_json(baseline, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("schema/version", result.stderr)

            write_performance(baseline)
            document = json.loads(policy.read_text())
            document["schemaVersion"] = 2
            write_json(policy, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("schema/version", result.stderr)

    def test_imported_history_minimum_and_qualification_extent_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])

            document = json.loads(current.read_text())
            document["samples"][0]["trackedEstimatedFootprint"][
                "importedHistoryEstimatedBytes"
            ] = 0
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("below policy minimum", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][0]["trackedEstimatedFootprint"][
                "importedHistoryEstimatedBytes"
            ] = 88_473_599
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("below policy minimum", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["qualificationIdentity"]["qualificationWidth"] = 1920
            write_json(current, document)
            result = run_performance(*arguments, "--out", root / "result.json")
            self.assertEqual(result.returncode, 2)
            self.assertEqual(
                json.loads((root / "result.json").read_text())["status"],
                "notComparable",
            )

            write_performance(current)
            document = json.loads(current.read_text())
            for report_sample in document["samples"]:
                report_sample["gpu"]["passes"][2]["name"] = "TAAResolve"
            write_json(current, document)
            policy = Path(arguments[5])
            document = json.loads(policy.read_text())
            document["thresholds"]["minImportedHistoryEstimatedBytes"] = 1
            write_json(policy, document)
            write_performance_baseline_record(
                Path(arguments[-1]),
                Path(arguments[3]),
                policy,
            )
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("below 88473600", result.stderr)

    def test_legacy_gpu_fields_and_invalid_qualification_span_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            gpu = document["samples"][0]["gpu"]
            gpu["wholeFrameMs"] = gpu.pop("graphicsTerminalCommandSpanMs")
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("gpu", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][0]["gpu"]["renderGraphPassEnvelopeMs"] = 8.0
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("conservative maximum", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            for report_sample in document["samples"]:
                report_sample["gpu"]["renderGraphPassEnvelopeMs"] = 7.0
                report_sample["gpu"]["graphicsQualificationSpanMs"] = 7.0
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            gpu = document["samples"][0]["gpu"]
            gpu["passMs"] = {"Geometry": 2.0, "Lighting": 1.5}
            del gpu["passes"]
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("gpu", result.stderr)

            write_performance(current)
            document = json.loads(current.read_text())
            document["samples"][0]["gpu"]["passes"][1]["executionIndex"] = 2
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("contiguous", result.stderr)

            write_performance(current)
            policy = Path(arguments[5])
            document = json.loads(policy.read_text())
            thresholds = document["thresholds"]
            thresholds["graphicsTerminalCommandSpanP95Ms"] = thresholds.pop(
                "graphicsQualificationSpanP95Ms"
            )
            write_json(policy, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("thresholds", result.stderr)

            write_performance_policy(policy)
            document = json.loads(policy.read_text())
            document["thresholds"]["passGpuP95Ms"] = {
                "Geometry": 5.0,
                "Lighting": 5.0,
            }
            write_json(policy, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("passGpuP95Ms", result.stderr)

    def test_quantized_zero_graphics_spans_remain_valid(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            for report_sample in document["samples"]:
                report_sample["gpu"]["graphicsTerminalCommandSpanMs"] = 0.0
                report_sample["gpu"]["renderGraphPassEnvelopeMs"] = 0.0
                report_sample["gpu"]["graphicsQualificationSpanMs"] = 0.0
            write_json(current, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class RenderQualityPerformanceBaselineTests(unittest.TestCase):
    def _prepare_arguments(
        self,
        root: Path,
        *,
        metrics: list[float],
    ) -> tuple[list[Path | str], Path]:
        run_dirs = [
            write_calibration_run(
                root / f"run-{ordinal}",
                ordinal=ordinal,
                graphics_ms=metric,
            )
            for ordinal, metric in enumerate(metrics, start=1)
        ]
        output = root / "candidate"
        arguments: list[Path | str] = [
            "prepare",
            "--output-dir", output,
            "--baseline-id", "dx12-rtx4080-ci-v1",
            "--expected-source-commit", "commit-a",
            "--expected-source-fingerprint", "e" * 64,
            "--expected-adapter-name", HARDWARE["adapterName"],
            "--expected-driver-version", HARDWARE["driverVersion"],
            "--expected-os-build", HARDWARE["osBuild"],
        ]
        for run_dir in run_dirs:
            arguments.extend(["--run-dir", run_dir])
        return arguments, output

    def test_five_run_median_candidate_requires_explicit_approval(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, output = self._prepare_arguments(
                root,
                metrics=[5.0, 7.0, 6.0, 9.0, 8.0],
            )
            result = run_baseline_tool(*arguments)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            record_path = output / "baseline-record.json"
            record = json.loads(record_path.read_text())
            self.assertEqual(record["status"], "candidate")
            self.assertEqual(record["calibration"]["selectedRunOrdinal"], 2)
            self.assertEqual(
                record["calibration"]["runs"][1][
                    "graphicsQualificationSpanP95Ms"
                ],
                7.0,
            )
            rejected = run_baseline_tool(
                "verify",
                "--record", record_path,
                "--baseline", output / "performance.json",
                "--policy", output / "performance-policy.json",
                "--require-approved",
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("not approved", rejected.stderr)

            approved_output = root / "approved"
            with mock.patch.object(
                performance_baseline,
                "_assert_approval_source_is_clean",
            ):
                approved = performance_baseline._approve(SimpleNamespace(
                    candidate_record=record_path,
                    output_dir=approved_output,
                    approved_by="Render Team",
                    reason="Reviewed five fixed-GPU reports",
                ))
            self.assertEqual(approved, 0)
            verified = run_baseline_tool(
                "verify",
                "--record", approved_output / "baseline-record.json",
                "--baseline", approved_output / "performance.json",
                "--policy", approved_output / "performance-policy.json",
                "--require-approved",
            )
            self.assertEqual(
                verified.returncode, 0, verified.stdout + verified.stderr
            )

    def test_wrong_run_count_and_budget_violation_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, _ = self._prepare_arguments(
                root,
                metrics=[5.0, 6.0, 7.0, 8.0],
            )
            result = run_baseline_tool(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("exactly five", result.stderr)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, _ = self._prepare_arguments(
                root,
                metrics=[5.0, 6.0, 7.0, 8.0, 17.0],
            )
            result = run_baseline_tool(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Graphics budget", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
