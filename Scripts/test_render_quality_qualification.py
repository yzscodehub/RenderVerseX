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
import check_render_quality_series as series_contract
import render_quality_golden as golden_contract


ROOT = Path(__file__).resolve().parents[1]
BUNDLE_CHECKER = ROOT / "Scripts/check_render_quality_bundle.py"
PERFORMANCE_CHECKER = ROOT / "Scripts/check_render_quality_performance.py"
PERFORMANCE_BASELINE_TOOL = (
    ROOT / "Scripts/render_quality_performance_baseline.py"
)
SERIES_CHECKER = ROOT / "Scripts/check_render_quality_series.py"
GOLDEN_TOOL = ROOT / "Scripts/render_quality_golden.py"
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
    "backend": "dx12",
    "view": "hero",
    "captureTick": 300,
    "visualContractVersion": 1,
}
PERFORMANCE_IDENTITY = {
    **IDENTITY,
    "backend": "DX12",
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
SERIES_HARDWARE = {
    **HARDWARE,
    "backend": "dx12",
    "capabilityHash": "f" * 64,
}


def render_profile_hash(recipe: str, quality: str) -> int:
    recipe_ids = {"forward-plus": 1, "deferred": 2, "hybrid-rt": 3}
    quality_ids = {"low": 0, "medium": 1, "high": 2, "ultra": 3}
    result = 14695981039346656037
    for value in (1, recipe_ids[recipe], quality_ids[quality]):
        for byte in range(8):
            result ^= (value >> (byte * 8)) & 0xFF
            result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result or 1


def render_configuration_for(
    identity: dict[str, str | int],
    *,
    render_path: str = "direct",
    instancing_mode: str = "Auto",
    requested_recipe: str = "deferred",
    resolved_recipe: str = "deferred",
    profile_hash: int | None = None,
) -> dict[str, str | int]:
    if profile_hash is None:
        profile_hash = render_profile_hash(
            resolved_recipe, str(identity["quality"])
        )
    configuration: dict[str, str | int] = {
        "renderPath": render_path,
        "instancingMode": instancing_mode,
        "requestedRecipe": requested_recipe,
        "resolvedRecipe": resolved_recipe,
        "profileHash": profile_hash,
    }
    binding = bytearray()
    for name, value in (
        ("schema", "RVX.RenderQualityConfigurationIdentity"),
        ("schemaVersion", 1),
        ("renderPath", configuration["renderPath"]),
        ("instancingMode", configuration["instancingMode"]),
        ("requestedRecipe", configuration["requestedRecipe"]),
        ("resolvedRecipe", configuration["resolvedRecipe"]),
        ("quality", identity["quality"]),
        ("backend", identity["backend"]),
        ("profileHash", configuration["profileHash"]),
    ):
        encoded = str(value).encode("utf-8")
        binding.extend(name.encode("ascii"))
        binding.extend(b":")
        binding.extend(str(len(encoded)).encode("ascii"))
        binding.extend(b":")
        binding.extend(encoded)
        binding.extend(b"\n")
    configuration["renderConfigurationHash"] = hashlib.sha256(binding).hexdigest()
    return configuration


RENDER_CONFIGURATION = render_configuration_for(IDENTITY)
TELEMETRY_COVERAGE = {
    "schemaVersion": 2,
    "trackedUploadCoverageMask": 7,
    "trackedEstimatedFootprintCoverageMask": 15,
}


def write_json(path: Path, value: object) -> None:
    payload = json.dumps(value, indent=2, sort_keys=True) + "\n"
    path.write_bytes(payload.encode("utf-8"))


def write_bundle(
    root: Path,
    *,
    name: str,
    completed_frame: int,
    source_commit: str = "commit-a",
    run_id: str = "run-a",
    preset: str = "win_x64_debug_gfx24_qualification",
    capture_tick: int = 300,
    receipt: str = "a" * 64,
    payload_offset: float = 0.0,
    qualification_identity: dict[str, str | int] | None = None,
    render_configuration: dict[str, str | int] | None = None,
) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    bundle_identity = (
        IDENTITY if qualification_identity is None else qualification_identity
    )
    bundle_configuration = (
        render_configuration_for(bundle_identity)
        if render_configuration is None else render_configuration
    )
    attachments = []
    for ordinal, (semantic, (format_name, domain, channels)) in enumerate(SEMANTICS.items()):
        payload = struct.pack(
            "<" + "f" * (2 * 2 * channels),
            *([0.1 + ordinal * 0.01 + payload_offset] * (2 * 2 * channels)),
        )
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
        "schemaVersion": 2,
        "bundleId": name,
        "sourceCommit": source_commit,
        "qualificationIdentity": {
            **bundle_identity,
            "captureTick": capture_tick,
        },
        "renderConfigurationIdentity": bundle_configuration,
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
            "captureTicks": [300, 360, 361],
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
            "minTemporalMae": 0.0005 if semantic == "Velocity" else 0.0,
            "maxTemporalMae": 0.01,
        }]}
    write_json(path, {
        "schema": "RVX.RenderQualityVisualPolicy",
        "schemaVersion": 2,
        "qualificationIdentity": {
            key: value
            for key, value in IDENTITY.items()
            if key != "captureTick"
        },
        "attachments": attachments,
        "temporal": {
            "warmupCompletedFrames": 300,
            "goldenCaptureTick": 300,
            "adjacentCaptureTicks": [360, 361],
        },
    })
    return path


def write_visual_golden_record(
    path: Path,
    baseline: Path,
    policy: Path,
    sample_manifest: Path,
    *,
    approved: bool = True,
    hardware_mode: str = "exact-hardware",
    runner_class: str | None = None,
    render_configuration: dict[str, str | int] | None = None,
) -> Path:
    baseline_document = json.loads(baseline.read_text())
    packaged_manifest = path.parent / "golden-sample-manifest.json"
    packaged_manifest.write_bytes(sample_manifest.read_bytes())
    series_hashes = [
        hashlib.sha256(f"visual-series-{ordinal}".encode()).hexdigest()
        for ordinal in range(1, 6)
    ]
    ppm = path.parent / "contact-sheet.ppm"
    index = path.parent / "contact-sheet.json"
    golden_contract._contact_sheet(
        baseline,
        series_hashes[2],
        ppm,
        index,
    )
    runs = []
    for ordinal, digest in enumerate(series_hashes, start=1):
        runs.append({
            "ordinal": ordinal,
            "runId": "run-a" if ordinal == 3 else f"visual-run-{ordinal}",
            "seriesSha256": digest,
            "hardwareIdentity": SERIES_HARDWARE,
        })
    write_json(path, {
        "schema": "RVX.RenderQualityGoldenRecord",
        "schemaVersion": 2,
        "goldenId": "dx12-deferred-high-v3",
        "status": "approved" if approved else "candidate",
        "sourceCommit": baseline_document["sourceCommit"],
        "qualificationIdentity": {
            key: value
            for key, value in baseline_document["qualificationIdentity"].items()
            if key != "captureTick"
        },
        "renderConfigurationIdentity": (
            baseline_document["renderConfigurationIdentity"]
            if render_configuration is None else render_configuration
        ),
        "sampleManifest": {
            "path": packaged_manifest.name,
            "sha256": hashlib.sha256(packaged_manifest.read_bytes()).hexdigest(),
        },
        "hardwareMode": hardware_mode,
        "runnerClass": runner_class,
        "selectedBundle": {
            "seriesOrdinal": 3,
            "seriesSha256": series_hashes[2],
            "runId": baseline_document["execution"]["runId"],
            "preset": baseline_document["execution"]["preset"],
            "captureTick": 300,
            "bundleId": baseline_document["bundleId"],
            "bundlePath": baseline.relative_to(path.parent).as_posix(),
            "bundleSha256": hashlib.sha256(baseline.read_bytes()).hexdigest(),
            "receiptSha256": baseline_document["execution"][
                "renderExecutionReceiptSha256"
            ],
        },
        "visualPolicy": {
            "path": policy.name,
            "sha256": hashlib.sha256(policy.read_bytes()).hexdigest(),
        },
        "contactSheet": {
            "ppmPath": ppm.name,
            "ppmSha256": hashlib.sha256(ppm.read_bytes()).hexdigest(),
            "indexPath": index.name,
            "indexSha256": hashlib.sha256(index.read_bytes()).hexdigest(),
        },
        "seriesAudit": {
            "requiredSeriesCount": 5,
            "selectedSeriesOrdinal": 3,
            "runs": runs,
        },
        "approval": {
            "approvedBy": "Render Team" if approved else None,
            "approvedAtUtc": "2026-09-01T00:00:00Z" if approved else None,
            "reason": "Reviewed five same-process series" if approved else None,
        },
    })
    return path


def write_attachment_series(
    root: Path,
    sample_manifest: Path,
    *,
    run_id: str = "run-a",
    payload_base: float = 0.0,
    tick_payload_offsets: dict[int, float] | None = None,
    hardware: dict[str, str] | None = None,
    identity: dict[str, str | int] | None = None,
    capture_ticks: tuple[int, int, int] = (300, 360, 361),
    render_configuration: dict[str, str | int] | None = None,
) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    series_identity = (
        {key: value for key, value in IDENTITY.items() if key != "captureTick"}
        if identity is None else identity
    )
    series_configuration = (
        render_configuration_for(series_identity)
        if render_configuration is None else render_configuration
    )
    if (
        len(capture_ticks) != 3
        or capture_ticks[0] < 1
        or any(left >= right for left, right in zip(capture_ticks, capture_ticks[1:]))
    ):
        raise ValueError("test capture ticks must be three strictly increasing values")
    payload_offsets = {
        capture_ticks[0]: payload_base,
        capture_ticks[1]: payload_base + 0.5,
        capture_ticks[2]: payload_base + 0.501,
    }
    if tick_payload_offsets is not None:
        unknown_ticks = set(tick_payload_offsets) - set(payload_offsets)
        if unknown_ticks:
            raise ValueError(f"unsupported test capture ticks: {sorted(unknown_ticks)}")
        payload_offsets.update(tick_payload_offsets)
    tick_specs = tuple(
        (capture_tick, bundle_id, receipt, payload_offsets[capture_tick])
        for capture_tick, bundle_id, receipt in zip(
            capture_ticks,
            ("1", "2", "3"),
            ("a" * 64, "c" * 64, "d" * 64),
            strict=True,
        )
    )
    tick_objects: list[series_contract.SeriesTick] = []
    tick_documents: list[dict[str, object]] = []
    for capture_tick, bundle_id, receipt, offset in tick_specs:
        directory_name = f"tick-{capture_tick:06d}"
        bundle_path = write_bundle(
            root / directory_name,
            name=bundle_id,
            completed_frame=capture_tick,
            capture_tick=capture_tick,
            receipt=receipt,
            run_id=run_id,
            payload_offset=offset,
            qualification_identity=series_identity,
            render_configuration=series_configuration,
        )
        relative = Path(directory_name) / "bundle.json"
        bundle_sha = hashlib.sha256(bundle_path.read_bytes()).hexdigest()
        tick = series_contract.SeriesTick(
            capture_tick,
            bundle_id,
            relative,
            bundle_sha,
            receipt,
            capture_tick,
            capture_tick,
            capture_tick,
        )
        tick_objects.append(tick)
        tick_documents.append({
            "captureTick": capture_tick,
            "bundleId": bundle_id,
            "bundlePath": relative.as_posix(),
            "bundleSha256": bundle_sha,
            "receiptSha256": receipt,
            "frameSequence": capture_tick,
            "submittedFrame": capture_tick,
            "completedFrame": capture_tick,
        })
    manifest_sha = hashlib.sha256(sample_manifest.read_bytes()).hexdigest()
    ticks = tuple(tick_objects)
    series_sha = series_contract.compute_series_sha256(
        "commit-a",
        run_id,
        "win_x64_debug_gfx24_qualification",
        series_identity,
        series_configuration,
        SERIES_HARDWARE if hardware is None else hardware,
        manifest_sha,
        ticks,
    )
    series_path = root / "series.json"
    write_json(series_path, {
        "schema": "RVX.RenderQualityAttachmentSeries",
        "schemaVersion": 2,
        "sourceCommit": "commit-a",
        "runId": run_id,
        "preset": "win_x64_debug_gfx24_qualification",
        "qualificationIdentity": series_identity,
        "renderConfigurationIdentity": series_configuration,
        "hardwareIdentity": SERIES_HARDWARE if hardware is None else hardware,
        "manifestSha256": manifest_sha,
        "ticks": tick_documents,
        "seriesSha256": series_sha,
    })
    return series_path


def run_bundle(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(BUNDLE_CHECKER), *map(str, arguments)], cwd=ROOT, text=True, capture_output=True, check=False)


def run_series(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SERIES_CHECKER), *map(str, arguments)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def run_golden(*arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(GOLDEN_TOOL), *map(str, arguments)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


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
            completed_frame=300,
            capture_tick=300,
            receipt="a" * 64,
        )
        baseline = write_bundle(
            root / "baseline",
            name="baseline",
            completed_frame=300,
            capture_tick=300,
            receipt="b" * 64,
        )
        later = write_bundle(
            root / "later",
            name="later",
            completed_frame=360,
            capture_tick=360,
            receipt="c" * 64,
        )
        adjacent = write_bundle(
            root / "adjacent",
            name="adjacent",
            completed_frame=361,
            capture_tick=361,
            receipt="d" * 64,
            payload_offset=0.001,
        )
        policy = write_visual_policy(root / "policy.json")
        sample_manifest = write_sample_manifest(root / "sample-manifest.json")
        golden_record = write_visual_golden_record(
            root / "golden-record.json",
            baseline,
            policy,
            sample_manifest,
        )
        return [
            "--bundle", current,
            "--baseline", baseline,
            "--golden-record", golden_record,
            "--policy", policy,
            "--sample-manifest", sample_manifest,
            "--temporal-bundle", later,
            "--temporal-bundle", adjacent,
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

    def test_bundle_v2_configuration_missing_drift_and_forgery_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            del document["renderConfigurationIdentity"]
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("renderConfigurationIdentity", result.stderr)

            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["renderConfigurationIdentity"]["renderPath"] = "gpu-driven"
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("does not bind", result.stderr)

            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["schemaVersion"] = 1
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("schema/version", result.stderr)

            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["renderConfigurationIdentity"] = render_configuration_for(
                document["qualificationIdentity"],
                render_path="banana",
            )
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("renderPath is unsupported", result.stderr)

            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            document["renderConfigurationIdentity"] = render_configuration_for(
                document["qualificationIdentity"],
                requested_recipe="forward-plus",
                resolved_recipe="forward-plus",
            )
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("does not match qualificationIdentity.pipeline", result.stderr)

            arguments = self._valid_arguments(root)
            current = Path(arguments[1])
            document = json.loads(current.read_text())
            expected = render_profile_hash("deferred", "high")
            document["renderConfigurationIdentity"] = render_configuration_for(
                document["qualificationIdentity"],
                profile_hash=expected + 1,
            )
            write_json(current, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("profileHash does not match", result.stderr)

    def test_visual_policy_v2_uses_only_adjacent_pair_and_rejects_frozen_motion(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            temporal_indices = [
                index + 1
                for index, value in enumerate(arguments)
                if value == "--temporal-bundle"
            ]
            later_path = Path(arguments[temporal_indices[0]])
            adjacent_path = Path(arguments[temporal_indices[1]])
            write_bundle(
                later_path.parent,
                name="later",
                completed_frame=360,
                capture_tick=360,
                receipt="c" * 64,
                payload_offset=0.5,
            )
            write_bundle(
                adjacent_path.parent,
                name="adjacent",
                completed_frame=361,
                capture_tick=361,
                receipt="d" * 64,
                payload_offset=0.501,
            )
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            write_bundle(
                adjacent_path.parent,
                name="adjacent",
                completed_frame=361,
                capture_tick=361,
                receipt="d" * 64,
                payload_offset=0.5,
            )
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("motion/flicker", result.stdout + result.stderr)

            policy = Path(arguments[arguments.index("--policy") + 1])
            policy_document = json.loads(policy.read_text())
            policy_document["schemaVersion"] = 1
            write_json(policy, policy_document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("schema/version", result.stderr)

    def test_visual_policy_rejects_golden_before_declared_warmup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            policy = Path(arguments[arguments.index("--policy") + 1])
            document = json.loads(policy.read_text())
            document["temporal"]["warmupCompletedFrames"] = 301
            write_json(policy, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "goldenCaptureTick must not precede",
                result.stderr,
            )

    def test_visual_gate_requires_approved_golden_and_bound_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            record = Path(arguments[arguments.index("--golden-record") + 1])
            document = json.loads(record.read_text())
            document["status"] = "candidate"
            document["approval"] = {
                "approvedBy": None,
                "approvedAtUtc": None,
                "reason": None,
            }
            write_json(record, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("not approved", result.stderr)

            arguments = self._valid_arguments(root)
            manifest = Path(
                arguments[arguments.index("--sample-manifest") + 1]
            )
            manifest.write_text(manifest.read_text() + " ", encoding="utf-8")
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Sample manifest", result.stderr)

            arguments = self._valid_arguments(root)
            record = Path(arguments[arguments.index("--golden-record") + 1])
            document = json.loads(record.read_text())
            packaged_manifest = record.parent / document["sampleManifest"]["path"]
            packaged_manifest.write_text(
                packaged_manifest.read_text() + " ",
                encoding="utf-8",
            )
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Packaged sample manifest", result.stderr)

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
            document["execution"]["renderExecutionReceiptSha256"] = "d" * 64
            for attachment in document["attachments"]:
                attachment["provenance"]["preset"] = "win_x64_debug_gfx24_qualification"
                attachment["provenance"]["renderExecutionReceiptSha256"] = "d" * 64
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
            manifest_document["samples"][0]["captureTicks"] = [360, 300]
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

            document["qualificationIdentity"]["captureTick"] = 300
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("adjacentCaptureTicks", result.stderr)

            document["qualificationIdentity"]["captureTick"] = 360
            document["execution"]["frameSequence"] = 359
            for attachment in document["attachments"]:
                attachment["provenance"]["frameSequence"] = 359
            write_json(later, document)
            result = run_bundle(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("predates its declared captureTick", result.stderr)


class RenderQualitySeriesTests(unittest.TestCase):
    def _valid_arguments(self, root: Path) -> list[Path | str]:
        manifest = write_sample_manifest(root / "sample-manifest.json")
        series = write_attachment_series(root / "series", manifest)
        return [
            "--series", series,
            "--sample-manifest", manifest,
            "--expected-source-commit", "commit-a",
            "--expected-run-id", "run-a",
            "--expected-preset", "win_x64_debug_gfx24_qualification",
            "--expected-adapter-name", SERIES_HARDWARE["adapterName"],
            "--expected-driver-version", SERIES_HARDWARE["driverVersion"],
            "--expected-os-build", SERIES_HARDWARE["osBuild"],
        ]

    def test_valid_same_process_series_binds_all_three_bundles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            result = run_series(*arguments, "--report", root / "result.json")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / "result.json").read_text())
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["captureTicks"], [300, 360, 361])

    def test_series_path_hash_identity_and_tick_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])

            document = json.loads(series_path.read_text())
            document["seriesSha256"] = "0" * 64
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("binding mismatch", result.stderr)

            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])
            document = json.loads(series_path.read_text())
            document["ticks"][1]["bundlePath"] = "../escape/bundle.json"
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("escapes", result.stderr)

            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])
            document = json.loads(series_path.read_text())
            document["ticks"][1]["bundleId"] = document["ticks"][0]["bundleId"]
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("reuses a bundleId", result.stderr)

            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])
            document = json.loads(series_path.read_text())
            document["ticks"].pop()
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("sample manifest", result.stderr)

            arguments = self._valid_arguments(root)
            manifest = Path(arguments[3])
            manifest.write_text(manifest.read_text() + " ", encoding="utf-8")
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("manifest hash", result.stderr)

    def test_series_v2_configuration_is_required_and_frozen_across_ticks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])
            document = json.loads(series_path.read_text())
            del document["renderConfigurationIdentity"]
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("renderConfigurationIdentity", result.stderr)

            arguments = self._valid_arguments(root)
            series_path = Path(arguments[1])
            document = json.loads(series_path.read_text())
            identity = document["qualificationIdentity"]
            changed_configuration = render_configuration_for(
                identity,
                render_path="gpu-driven",
            )
            tick_document = document["ticks"][2]
            bundle_path = series_path.parent / tick_document["bundlePath"]
            bundle = json.loads(bundle_path.read_text())
            bundle["renderConfigurationIdentity"] = changed_configuration
            write_json(bundle_path, bundle)
            tick_document["bundleSha256"] = hashlib.sha256(
                bundle_path.read_bytes()
            ).hexdigest()
            ticks = tuple(
                series_contract.SeriesTick(
                    item["captureTick"],
                    item["bundleId"],
                    Path(item["bundlePath"]),
                    item["bundleSha256"],
                    item["receiptSha256"],
                    item["frameSequence"],
                    item["submittedFrame"],
                    item["completedFrame"],
                )
                for item in document["ticks"]
            )
            document["seriesSha256"] = series_contract.compute_series_sha256(
                document["sourceCommit"],
                document["runId"],
                document["preset"],
                document["qualificationIdentity"],
                document["renderConfigurationIdentity"],
                document["hardwareIdentity"],
                document["manifestSha256"],
                ticks,
            )
            write_json(series_path, document)
            result = run_series(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("metadata does not match", result.stderr)


class RenderQualityGoldenTests(unittest.TestCase):
    def _candidate(self, root: Path) -> tuple[Path, Path, Path, Path]:
        manifest = write_sample_manifest(root / "sample-manifest.json")
        policy = write_visual_policy(root / "visual-policy-source.json")
        series_paths = [
            write_attachment_series(
                root / f"series-{ordinal}",
                manifest,
                run_id=f"golden-run-{ordinal}",
                payload_base=0.01,
            )
            for ordinal in range(1, 6)
        ]
        output = root / "candidate"
        arguments: list[Path | str] = [
            "prepare",
            "--sample-manifest", manifest,
            "--visual-policy", policy,
            "--output-dir", output,
            "--golden-id", "dx12-deferred-high-v3",
            "--selected-series-ordinal", "3",
            "--hardware-mode", "exact-hardware",
        ]
        for series_path in series_paths:
            arguments.extend(["--series", series_path])
        result = run_golden(*arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (
            output / "golden-record.json",
            output / "bundle" / "bundle.json",
            output / "visual-policy.json",
            output / "sample-manifest.json",
        )

    def test_candidate_contact_sheet_and_explicit_approval(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record, bundle, policy, manifest = self._candidate(root)
            document = json.loads(record.read_text())
            self.assertEqual(document["status"], "candidate")
            self.assertEqual(
                document["seriesAudit"]["selectedSeriesOrdinal"], 3
            )
            ppm = record.parent / "contact-sheet.ppm"
            self.assertTrue(ppm.read_bytes().startswith(b"P6\n1280 360\n255\n"))
            for json_path in (
                record,
                bundle,
                policy,
                manifest,
                record.parent / "contact-sheet.json",
            ):
                self.assertNotIn(b"\r", json_path.read_bytes())

            valid = run_golden(
                "verify",
                "--record", record,
                "--bundle", bundle,
                "--policy", policy,
                "--sample-manifest", manifest,
            )
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
            rejected = run_golden(
                "verify",
                "--record", record,
                "--bundle", bundle,
                "--policy", policy,
                "--sample-manifest", manifest,
                "--require-approved",
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("not approved", rejected.stderr)

            approved_root = root / "approved"
            with mock.patch.object(
                golden_contract,
                "_assert_approval_source_is_clean",
            ):
                approved = golden_contract._approve(SimpleNamespace(
                    candidate_record=record,
                    output_dir=approved_root,
                    approved_by="Render Team",
                    reason="Reviewed seven semantic panels",
                ))
            self.assertEqual(approved, 0)
            verified = run_golden(
                "verify",
                "--record", approved_root / "golden-record.json",
                "--bundle", approved_root / "bundle" / "bundle.json",
                "--policy", approved_root / "visual-policy.json",
                "--sample-manifest", approved_root / "sample-manifest.json",
                "--require-approved",
            )
            self.assertEqual(
                verified.returncode, 0, verified.stdout + verified.stderr
            )
            for json_path in (
                approved_root / "golden-record.json",
                approved_root / "bundle" / "bundle.json",
                approved_root / "visual-policy.json",
                approved_root / "sample-manifest.json",
                approved_root / "contact-sheet.json",
            ):
                self.assertNotIn(b"\r", json_path.read_bytes())

    def test_prepare_rejects_visual_drift_in_a_nonselected_series(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = write_sample_manifest(root / "sample-manifest.json")
            policy = write_visual_policy(root / "visual-policy.json")
            series_paths = [
                write_attachment_series(
                    root / f"series-{ordinal}",
                    manifest,
                    run_id=f"golden-run-{ordinal}",
                    payload_base=0.02 if ordinal == 5 else 0.01,
                )
                for ordinal in range(1, 6)
            ]
            output = root / "candidate"
            arguments: list[Path | str] = [
                "prepare",
                "--sample-manifest", manifest,
                "--visual-policy", policy,
                "--output-dir", output,
                "--golden-id", "dx12-deferred-high-v3",
                "--selected-series-ordinal", "3",
                "--hardware-mode", "exact-hardware",
            ]
            for series_path in series_paths:
                arguments.extend(["--series", series_path])
            result = run_golden(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "series 5 failed VisualPolicy calibration",
                result.stderr,
            )
            self.assertFalse(output.exists())

    def test_prepare_rejects_equal_temporal_drift_in_a_nonselected_series(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = write_sample_manifest(root / "sample-manifest.json")
            policy = write_visual_policy(root / "visual-policy.json")
            series_paths = [
                write_attachment_series(
                    root / f"series-{ordinal}",
                    manifest,
                    run_id=f"golden-run-{ordinal}",
                    payload_base=0.01,
                    tick_payload_offsets=(
                        {360: 0.61, 361: 0.611}
                        if ordinal == 5
                        else None
                    ),
                )
                for ordinal in range(1, 6)
            ]
            output = root / "candidate"
            arguments: list[Path | str] = [
                "prepare",
                "--sample-manifest", manifest,
                "--visual-policy", policy,
                "--output-dir", output,
                "--golden-id", "dx12-deferred-high-v3",
                "--selected-series-ordinal", "3",
                "--hardware-mode", "exact-hardware",
            ]
            for series_path in series_paths:
                arguments.extend(["--series", series_path])
            result = run_golden(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("series 5 failed VisualPolicy calibration", result.stderr)
            self.assertIn("tick 360", result.stderr)
            self.assertFalse(output.exists())

    def test_prepare_rejects_frozen_temporal_pairs_in_all_series(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = write_sample_manifest(root / "sample-manifest.json")
            policy = write_visual_policy(root / "visual-policy.json")
            series_paths = [
                write_attachment_series(
                    root / f"series-{ordinal}",
                    manifest,
                    run_id=f"golden-run-{ordinal}",
                    payload_base=0.01,
                    tick_payload_offsets={360: 0.51, 361: 0.51},
                )
                for ordinal in range(1, 6)
            ]
            output = root / "candidate"
            arguments: list[Path | str] = [
                "prepare",
                "--sample-manifest", manifest,
                "--visual-policy", policy,
                "--output-dir", output,
                "--golden-id", "dx12-deferred-high-v3",
                "--selected-series-ordinal", "3",
                "--hardware-mode", "exact-hardware",
            ]
            for series_path in series_paths:
                arguments.extend(["--series", series_path])
            result = run_golden(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("series 1 failed VisualPolicy calibration", result.stderr)
            self.assertIn("temporal: temporal motion/flicker", result.stderr)
            self.assertFalse(output.exists())

    def test_contact_sheet_requires_policy_and_uses_golden_tick(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = write_sample_manifest(root / "sample-manifest.json")
            policy = write_visual_policy(root / "visual-policy.json")
            series = write_attachment_series(root / "series", manifest)
            ppm = root / "contact.ppm"
            index = root / "contact.json"
            result = run_golden(
                "contact-sheet",
                "--series", series,
                "--sample-manifest", manifest,
                "--visual-policy", policy,
                "--out-ppm", ppm,
                "--out-index", index,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(json.loads(index.read_text())["captureTick"], 300)

            missing_policy = run_golden(
                "contact-sheet",
                "--series", series,
                "--sample-manifest", manifest,
                "--out-ppm", root / "missing-policy.ppm",
                "--out-index", root / "missing-policy.json",
            )
            self.assertNotEqual(missing_policy.returncode, 0)
            self.assertIn("--visual-policy", missing_policy.stderr)

    def test_golden_hash_and_unexpanded_payload_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record, bundle, policy, manifest = self._candidate(root)
            raw = bundle.parent / "BeautyLDR.raw"
            raw.write_text(
                "version https://git-lfs.github.com/spec/v1\n"
                "oid sha256:" + "0" * 64 + "\nsize 16\n",
                encoding="utf-8",
            )
            result = run_golden(
                "verify",
                "--record", record,
                "--bundle", bundle,
                "--policy", policy,
                "--sample-manifest", manifest,
            )
            self.assertEqual(result.returncode, 1)
            self.assertTrue(
                "truncated" in result.stderr
                or "SHA-256" in result.stderr
                or "payload" in result.stderr
            )


class RenderQualityGoldenComparisonTests(unittest.TestCase):
    def _fixture(
        self,
        root: Path,
        *,
        approved: bool = True,
        hardware_mode: str = "exact-hardware",
        runner_class: str | None = None,
        payload_base: float = 0.0,
        tick_payload_offsets: dict[int, float] | None = None,
        hardware: dict[str, str] | None = None,
        identity: dict[str, str | int] | None = None,
        render_configuration: dict[str, str | int] | None = None,
    ) -> dict[str, Path]:
        root.mkdir(parents=True, exist_ok=True)
        manifest = write_sample_manifest(root / "sample-manifest.json")
        policy = write_visual_policy(root / "visual-policy.json")
        baseline = write_bundle(
            root / "baseline",
            name="approved-golden",
            completed_frame=300,
            render_configuration=render_configuration,
        )
        record = write_visual_golden_record(
            root / "golden-record.json",
            baseline,
            policy,
            manifest,
            approved=approved,
            hardware_mode=hardware_mode,
            runner_class=runner_class,
            render_configuration=render_configuration,
        )
        series = write_attachment_series(
            root / "current-series",
            manifest,
            payload_base=payload_base,
            tick_payload_offsets=tick_payload_offsets,
            hardware=hardware,
            identity=identity,
            render_configuration=render_configuration,
        )
        return {
            "record": record,
            "baseline": baseline,
            "policy": policy,
            "manifest": manifest,
            "series": series,
        }

    def _arguments(
        self,
        fixture: dict[str, Path],
        report: Path,
        *,
        required_hardware_mode: str = "exact-hardware",
        runner_class: str | None = None,
        series_manifest: Path | None = None,
    ) -> list[Path | str]:
        arguments: list[Path | str] = [
            "compare-series",
            "--series", fixture["series"],
            "--record", fixture["record"],
            "--baseline", fixture["baseline"],
            "--policy", fixture["policy"],
            "--sample-manifest", fixture["manifest"],
            "--required-hardware-mode", required_hardware_mode,
            "--expected-source-commit", "commit-a",
            "--expected-run-id", "run-a",
            "--expected-preset", "win_x64_debug_gfx24_qualification",
            "--report", report,
        ]
        if runner_class is not None:
            arguments.extend(["--runner-class", runner_class])
        if series_manifest is not None:
            arguments.extend(["--series-manifest", series_manifest])
        return arguments

    def test_approved_exact_hardware_comparison_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root)
            report = root / "comparison.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            document = json.loads(report.read_text())
            self.assertEqual(document["schema"], "RVX.RenderQualityGoldenComparisonResult")
            self.assertEqual(document["status"], "passed")
            self.assertTrue(document["comparisonPerformed"])
            self.assertEqual(document["currentHardwareIdentity"], SERIES_HARDWARE)
            self.assertEqual(document["baselineHardwareIdentity"], SERIES_HARDWARE)
            self.assertEqual(
                document["currentRenderConfigurationIdentity"],
                RENDER_CONFIGURATION,
            )
            self.assertEqual(
                document["baselineRenderConfigurationIdentity"],
                RENDER_CONFIGURATION,
            )
            self.assertEqual(len(document["visualMetrics"]), len(SEMANTICS))
            self.assertEqual(len(document["temporalMetrics"]), len(SEMANTICS))

    def test_candidate_missing_tampered_and_lfs_records_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root / "candidate", approved=False)
            report = root / "candidate-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(report.read_text())["comparisonPerformed"], False)
            self.assertIn("not approved", result.stderr)

            fixture = self._fixture(root / "missing")
            fixture["record"] = root / "missing" / "does-not-exist.json"
            report = root / "missing-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertFalse(document["comparisonPerformed"])
            self.assertIsNone(document["goldenRecord"]["sha256"])

            fixture = self._fixture(root / "tampered")
            fixture["baseline"].write_text(
                fixture["baseline"].read_text(encoding="utf-8") + " ",
                encoding="utf-8",
            )
            report = root / "tampered-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            self.assertFalse(json.loads(report.read_text())["comparisonPerformed"])
            self.assertIn("SHA-256", result.stderr)

            fixture = self._fixture(root / "lfs-pointer")
            fixture["record"].write_text(
                "version https://git-lfs.github.com/spec/v1\n"
                + "oid sha256:" + "0" * 64 + "\nsize 16\n",
                encoding="utf-8",
            )
            report = root / "lfs-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            self.assertFalse(json.loads(report.read_text())["comparisonPerformed"])
            self.assertIn("not valid JSON", result.stderr)

    def test_each_exact_hardware_field_fails_before_pixel_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for field in series_contract.HARDWARE_FIELDS:
                hardware = dict(SERIES_HARDWARE)
                identity = None
                if field == "backend":
                    hardware[field] = "vulkan"
                    identity = {
                        key: value for key, value in IDENTITY.items()
                        if key != "captureTick"
                    }
                    identity["backend"] = "vulkan"
                elif field == "capabilityHash":
                    hardware[field] = "e" * 64
                else:
                    hardware[field] = f"different-{field}"
                fixture = self._fixture(
                    root / field,
                    hardware=hardware,
                    identity=identity,
                )
                report = root / f"{field}.json"
                result = run_golden(*self._arguments(fixture, report))
                self.assertEqual(result.returncode, 1, field)
                document = json.loads(report.read_text())
                self.assertFalse(document["comparisonPerformed"], field)
                self.assertEqual(document["failureCategory"], "hardware-mismatch")
                self.assertIn(field, document["diagnostic"])

    def test_hardware_mode_and_runner_class_mismatches_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root / "mode")
            report = root / "mode-report.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                required_hardware_mode="runner-class",
                runner_class="hosted-gpu",
            ))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(report.read_text())["failureCategory"], "hardware-mode")

            fixture = self._fixture(
                root / "runner-class",
                hardware_mode="runner-class",
                runner_class="hosted-gpu",
            )
            report = root / "runner-class-report.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                required_hardware_mode="runner-class",
                runner_class="other-gpu",
            ))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(json.loads(report.read_text())["failureCategory"], "hardware-mode")

            report = root / "runner-class-valid.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                required_hardware_mode="runner-class",
                runner_class="hosted-gpu",
            ))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pixel_drift_and_frozen_temporal_fail_after_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root / "pixel", payload_base=0.02)
            report = root / "pixel-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertTrue(document["comparisonPerformed"])
            self.assertEqual(document["failureCategory"], "visual-metric")
            self.assertTrue(document["visualMetrics"])

            fixture = self._fixture(
                root / "frozen",
                tick_payload_offsets={360: 0.51, 361: 0.51},
            )
            report = root / "frozen-report.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertTrue(document["comparisonPerformed"])
            self.assertEqual(document["failureCategory"], "temporal-metric")
            self.assertTrue(document["temporalMetrics"])

    def test_runtime_configuration_drift_fails_before_pixel_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root)
            configuration = render_configuration_for(
                {key: value for key, value in IDENTITY.items() if key != "captureTick"},
                render_path="gpu-driven",
            )
            fixture["series"] = write_attachment_series(
                root / "current-series-drift",
                fixture["manifest"],
                render_configuration=configuration,
            )
            report = root / "configuration-drift.json"
            result = run_golden(*self._arguments(fixture, report))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertFalse(document["comparisonPerformed"])
            self.assertEqual(
                document["failureCategory"],
                "render-configuration-mismatch",
            )

    def test_series_manifest_eol_is_ignored_but_semantic_drift_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = self._fixture(root / "format-only")
            formatted_manifest = root / "format-only" / "source-manifest.json"
            source_document = json.loads(fixture["manifest"].read_text())
            formatted_manifest.write_bytes(
                (
                    json.dumps(source_document, indent=4, sort_keys=False)
                    .replace("\n", "\r\n")
                    + "\r\n"
                ).encode("utf-8")
            )
            fixture["series"] = write_attachment_series(
                root / "format-only" / "formatted-series",
                formatted_manifest,
            )
            report = root / "format-only.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                series_manifest=formatted_manifest,
            ))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            document = json.loads(report.read_text())
            self.assertTrue(document["comparisonPerformed"])
            self.assertNotEqual(
                document["goldenSampleManifest"]["sha256"],
                document["seriesManifest"]["sha256"],
            )

            fixture = self._fixture(root / "tick-drift")
            source_manifest = root / "tick-drift" / "source-manifest.json"
            source_document = json.loads(fixture["manifest"].read_text())
            source_document["samples"][0]["captureTicks"] = [300, 360, 362]
            write_json(source_manifest, source_document)
            fixture["series"] = write_attachment_series(
                root / "tick-drift" / "source-series",
                source_manifest,
                capture_ticks=(300, 360, 362),
            )
            report = root / "tick-drift.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                series_manifest=source_manifest,
            ))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertFalse(document["comparisonPerformed"])
            self.assertEqual(document["failureCategory"], "manifest-semantic-mismatch")

            fixture = self._fixture(root / "roi-drift")
            source_manifest = root / "roi-drift" / "source-manifest.json"
            source_document = json.loads(fixture["manifest"].read_text())
            source_document["samples"][0]["roiMetricPolicy"][
                "regionsOfInterest"
            ][0]["width"] = 0.5
            write_json(source_manifest, source_document)
            fixture["series"] = write_attachment_series(
                root / "roi-drift" / "source-series",
                source_manifest,
            )
            report = root / "roi-drift.json"
            result = run_golden(*self._arguments(
                fixture,
                report,
                series_manifest=source_manifest,
            ))
            self.assertEqual(result.returncode, 1)
            document = json.loads(report.read_text())
            self.assertFalse(document["comparisonPerformed"])
            self.assertEqual(document["failureCategory"], "manifest-semantic-mismatch")


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

    def test_baseline_packages_canonicalize_crlf_json_before_hashing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            arguments, output = self._prepare_arguments(
                root,
                metrics=[5.0, 7.0, 6.0, 9.0, 8.0],
            )
            for ordinal in range(1, 6):
                run_root = root / f"run-{ordinal}"
                report_path = run_root / "performance.json"
                report_path.write_bytes(
                    report_path.read_bytes().replace(b"\n", b"\r\n")
                )
                receipt_path = run_root / "required-suite-receipt.json"
                receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                artifact = receipt["semanticArtifacts"][0]
                artifact["sha256"] = hashlib.sha256(
                    report_path.read_bytes()
                ).hexdigest()
                artifact["size"] = report_path.stat().st_size
                write_json(receipt_path, receipt)
                receipt_path.write_bytes(
                    receipt_path.read_bytes().replace(b"\n", b"\r\n")
                )

            prepared = run_baseline_tool(*arguments)
            self.assertEqual(
                prepared.returncode, 0, prepared.stdout + prepared.stderr
            )
            candidate_verified = run_baseline_tool(
                "verify",
                "--record", output / "baseline-record.json",
                "--baseline", output / "performance.json",
                "--policy", output / "performance-policy.json",
            )
            self.assertEqual(
                candidate_verified.returncode,
                0,
                candidate_verified.stdout + candidate_verified.stderr,
            )

            approved_output = root / "approved"
            with mock.patch.object(
                performance_baseline,
                "_assert_approval_source_is_clean",
            ):
                self.assertEqual(
                    performance_baseline._approve(SimpleNamespace(
                        candidate_record=output / "baseline-record.json",
                        output_dir=approved_output,
                        approved_by="Render Team",
                        reason="Reviewed canonical JSON package",
                    )),
                    0,
                )
            for package in (output, approved_output):
                json_paths = [
                    package / "baseline-record.json",
                    package / "performance.json",
                    package / "performance-policy.json",
                ] + [
                    package / "receipts" / f"{ordinal:02d}.json"
                    for ordinal in range(1, 6)
                ]
                for path in json_paths:
                    payload = path.read_bytes()
                    self.assertFalse(payload.startswith(b"\xef\xbb\xbf"), path)
                    self.assertNotIn(b"\r", payload, path)
                    payload.decode("utf-8")

            approved_verified = run_baseline_tool(
                "verify",
                "--record", approved_output / "baseline-record.json",
                "--baseline", approved_output / "performance.json",
                "--policy", approved_output / "performance-policy.json",
                "--require-approved",
            )
            self.assertEqual(
                approved_verified.returncode,
                0,
                approved_verified.stdout + approved_verified.stderr,
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
    if sys.argv[1:] == ["--list-tests"]:
        def names(suite):
            for item in suite:
                if isinstance(item, unittest.TestSuite):
                    yield from names(item)
                else:
                    yield item.id().removeprefix(__name__ + ".")

        discovered = sorted(names(unittest.defaultTestLoader.loadTestsFromModule(
            sys.modules[__name__])))
        if not discovered or len(discovered) != len(set(discovered)):
            raise SystemExit("Unit discovery must be nonempty and unique")
        print(json.dumps(discovered))
    else:
        program = unittest.main(verbosity=2, exit=False)
        result = program.result
        raise SystemExit(0 if (
            result.testsRun > 0 and result.wasSuccessful()
            and not result.skipped and not result.expectedFailures
        ) else 1)
