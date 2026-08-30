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


ROOT = Path(__file__).resolve().parents[1]
BUNDLE_CHECKER = ROOT / "Scripts/check_render_quality_bundle.py"
PERFORMANCE_CHECKER = ROOT / "Scripts/check_render_quality_performance.py"
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
HARDWARE = {
    "backend": "DX12",
    "adapterName": "NVIDIA GeForce RTX 4080",
    "driverVersion": "555.1",
    "osBuild": "windows-26100",
    "capabilityHash": "rvx-rhi-capabilities-dx12-v1-5ef7bade",
}
TELEMETRY_COVERAGE = {
    "schemaVersion": 1,
    "trackedUploadCoverageMask": 7,
    "trackedEstimatedFootprintCoverageMask": 7,
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


def sample(frame: int, *, textures: int = 12) -> dict[str, object]:
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
            "schemaVersion": 1,
            "coverageMask": 7,
            "transientPoolPhysicalEstimatedBytes": 4096,
            "importedHistoryNamedEstimatedBytes": 1024,
            "resourceRegistryResidentEstimatedBytes": 2048,
        },
        "resourceCounts": {
            "schemaVersion": 1,
            "counts": {"textures": textures, "buffers": 8, "pipelines": 4, "descriptors": 20},
        },
    }


def write_performance(path: Path, *, warmup: int = 60, count: int = 300, hardware: dict[str, str] | None = None, grow: bool = False) -> Path:
    samples = [sample(warmup + index) for index in range(count)]
    if grow and samples:
        samples[-1]["resourceCounts"]["counts"]["textures"] = 13
    write_json(path, {
        "schema": "RVX.RenderQualityPerformanceReport",
        "schemaVersion": 2,
        "sourceCommit": "commit-a",
        "runId": "run-a",
        "preset": "win_x64_debug_gfx24_qualification",
        "qualificationIdentity": IDENTITY,
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
        "schemaVersion": 2,
        "qualificationIdentity": IDENTITY,
        "requiredHardwareIdentity": HARDWARE,
        "telemetryCoverage": TELEMETRY_COVERAGE,
        "warmupCompletedFrames": 60,
        "minimumCompletedSamples": 300,
        "baseline": {"id": "dx12-rtx4080-ci-v1", "maxRegressionPercent": 10.0},
        "thresholds": {
            "graphicsQualificationSpanP95Ms": 16.67,
            "cpuP95Ms": {"extraction": 5.0, "visibility": 5.0, "graphCompile": 5.0, "submission": 5.0},
            "passGpuP95Ms": [
                {"executionIndex": 0, "name": "Geometry", "p95Ms": 5.0},
                {"executionIndex": 1, "name": "Lighting", "p95Ms": 5.0},
                {"executionIndex": 2, "name": "Geometry", "p95Ms": 5.0},
            ],
            "maxTrackedUploadBytes": 4096,
            "maxTransientPoolPhysicalEstimatedBytes": 8192,
            "maxImportedHistoryNamedEstimatedBytes": 2048,
            "maxResourceRegistryResidentEstimatedBytes": 4096,
        },
    })
    return path


def run_performance(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(PERFORMANCE_CHECKER), *map(str, arguments)], cwd=ROOT, text=True, capture_output=True, check=False)


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
        return [
            "--report", current,
            "--baseline", baseline,
            "--policy", policy,
            "--expected-source-commit", "commit-a",
            "--expected-run-id", "run-a",
            "--expected-preset", "win_x64_debug_gfx24_qualification",
        ]

    def test_valid_completion_owned_series_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = run_performance(*self._valid_arguments(root), "--out", root / "result.json")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads((root / "result.json").read_text())["status"], "passed")

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
            self.assertIn("growth", result.stderr)
            write_performance(Path(arguments[1]))
            document = json.loads(Path(arguments[1]).read_text())
            del document["samples"][0]["cpuMs"]["visibility"]
            write_json(Path(arguments[1]), document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("missing", result.stderr)

    def test_tracked_telemetry_coverage_and_legacy_fields_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["telemetryCoverage"]["trackedUploadCoverageMask"] = 3
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
            baseline = Path(arguments[3])
            document = json.loads(baseline.read_text())
            del document["telemetryCoverage"]
            write_json(baseline, document)
            result = run_performance(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("telemetryCoverage", result.stderr)

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
                report_sample["gpu"]["renderGraphPassEnvelopeMs"] = 7.5
                report_sample["gpu"]["graphicsQualificationSpanMs"] = 7.5
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
