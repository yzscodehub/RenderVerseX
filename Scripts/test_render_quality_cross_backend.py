#!/usr/bin/env python3
"""Hermetic fail-closed tests for compare_render_quality_backends.py."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import check_render_quality_series as series
import compare_render_quality_backends as comparator
from check_render_quality_bundle import _profile_hash
from test_render_quality_qualification import (
    IDENTITY, SEMANTICS, SERIES_HARDWARE, write_attachment_series,
    render_configuration_for, write_json, write_sample_manifest,
)


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "Scripts/compare_render_quality_backends.py"
REPOSITORY_POLICY = (
    ROOT / "Tests/RenderQuality/Policies/cross-backend-visual-policy-v1.json"
)
TARGETS = (("windows", "dx12"), ("linux", "vulkan"), ("macos", "metal"))
PIPELINES = ("forward-plus", "deferred")


def digest(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def policy(path: Path) -> Path:
    attachments = {}
    for semantic in SEMANTICS:
        attachments[semantic] = {"rois": [{
            "id": "frame", "normalizedRegionId": "frame", "dataRange": 1.0,
            "maxMae": 0.02, "maxAbs": 0.1, "minPsnr": 20.0, "minSsim": 0.9,
            "temporalMode": "dynamic", "minTemporalMae": 0.0005,
            "maxTemporalMae": 0.01, "maxTemporalDeltaFieldMae": 0.0015,
        }]}
    write_json(path, {
        "schema": "RVX.RenderQualityCrossBackendVisualPolicy",
        "schemaVersion": 1, "policyId": "cross-backend-test-v1",
        "rationale": "Bounded fixture thresholds exercise seven attachments, visual differences, temporal deltas, provenance, and rejected incomplete or candidate evidence.",
        "reference": {"platform": "windows", "backend": "dx12"},
        "candidates": [
            {"platform": "linux", "backend": "vulkan"},
            {"platform": "macos", "backend": "metal"},
        ],
        "qualificationIdentity": {
            "sampleId": "render-quality-lab", "quality": "high",
            "view": "hero", "visualContractVersion": 1,
        },
        "requiredPipelines": list(PIPELINES),
        "requiredCaptureTicks": {"comparisonTick": 300, "temporalTicks": [360, 361]},
        "attachments": attachments,
    })
    return path


def refresh(path: Path) -> None:
    doc = json.loads(path.read_text())
    ticks = []
    for tick in doc["ticks"]:
        bundle_path = path.parent / Path(tick["bundlePath"])
        bundle = json.loads(bundle_path.read_text())
        tick["bundleId"] = bundle["bundleId"]
        tick["bundleSha256"] = hashlib.sha256(bundle_path.read_bytes()).hexdigest()
        tick["receiptSha256"] = bundle["execution"]["renderExecutionReceiptSha256"]
        ticks.append(series.SeriesTick(
            tick["captureTick"], tick["bundleId"], Path(tick["bundlePath"]),
            tick["bundleSha256"], tick["receiptSha256"], tick["frameSequence"],
            tick["submittedFrame"], tick["completedFrame"],
        ))
    doc["seriesSha256"] = series.compute_series_sha256(
        doc["sourceCommit"], doc["runId"], doc["preset"],
        doc["qualificationIdentity"], doc["renderConfigurationIdentity"],
        doc["hardwareIdentity"],
        doc["manifestSha256"], tuple(ticks),
    )
    write_json(path, doc)


def make(
    root: Path, manifest: Path, platform: str, backend: str, pipeline: str,
    *, base: float = 0.0, offsets: dict[int, float] | None = None,
    quality: str = "high", run_id: str | None = None,
) -> Path:
    label = f"{platform}-{backend}-{pipeline}"
    identity = {
        "sampleId": IDENTITY["sampleId"], "pipeline": pipeline, "quality": quality,
        "backend": backend, "view": IDENTITY["view"],
        "visualContractVersion": IDENTITY["visualContractVersion"],
    }
    hardware = {
        **SERIES_HARDWARE, "backend": backend,
        "adapterName": f"{platform}-{backend}", "driverVersion": f"{platform}-driver",
        "osBuild": f"{platform}-build",
        "capabilityHash": digest(f"{platform}-{backend}"),
    }
    configuration = render_configuration_for(
        identity,
        requested_recipe=pipeline,
        resolved_recipe=pipeline,
        profile_hash=_profile_hash(pipeline, quality),
    )
    path = write_attachment_series(
        root / label, manifest, run_id=run_id or f"{platform}-{backend}-suite-run",
        payload_base=base, tick_payload_offsets=offsets,
        hardware=hardware, identity=identity,
        render_configuration=configuration,
    )
    doc = json.loads(path.read_text())
    for ordinal, tick in enumerate(doc["ticks"], start=1):
        bundle_path = path.parent / Path(tick["bundlePath"])
        bundle = json.loads(bundle_path.read_text())
        # Real required-suite producers scope this ordinal to one Series.
        # Reusing 1/2/3 across pipelines and backends is therefore valid.
        bundle["bundleId"] = str(ordinal)
        receipt = digest(f"{label}-{tick['captureTick']}")
        bundle["execution"]["renderExecutionReceiptSha256"] = receipt
        for attachment in bundle["attachments"]:
            attachment["provenance"]["renderExecutionReceiptSha256"] = receipt
        write_json(bundle_path, bundle)
    refresh(path)
    return path


class CrossBackendTests(unittest.TestCase):
    def test_repository_policy_requires_signed_delta_field_bounds(self) -> None:
        document = comparator._policy(REPOSITORY_POLICY)
        self.assertEqual(document["schemaVersion"], 1)
        for attachment in document["attachments"].values():
            for roi in attachment["rois"]:
                self.assertIn("maxTemporalDeltaFieldMae", roi)
                self.assertNotIn("maxTemporalDeltaMae", roi)

    def fixture(self, root: Path, **options: object) -> tuple[Path, Path, dict[tuple[str, str, str], Path]]:
        root.mkdir(parents=True, exist_ok=True)
        manifest = write_sample_manifest(root / "sample-manifest.json")
        result: dict[tuple[str, str, str], Path] = {}
        bases = options.get("bases", {})
        offsets = options.get("offsets", {})
        qualities = options.get("qualities", {})
        runs = options.get("runs", {})
        for platform, backend in TARGETS:
            for pipeline_name in PIPELINES:
                key = platform, backend, pipeline_name
                result[key] = make(
                    root / "series", manifest, *key,
                    base=bases.get(key, 0.0), offsets=offsets.get(key),
                    quality=qualities.get(key, "high"), run_id=runs.get(key),
                )
        return manifest, policy(root / "policy.json"), result

    def run_tool(self, root: Path, manifest: Path, policy_path: Path, inputs: dict[tuple[str, str, str], Path]) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
        report = root / "comparison.json"
        command = [
            sys.executable, str(TOOL), "--sample-manifest", str(manifest),
            "--policy", str(policy_path), "--expected-source-commit", "commit-a",
            "--report", str(report),
        ]
        for (platform, backend, pipeline_name), path in sorted(inputs.items()):
            command.extend(["--series", f"{platform}/{backend}/{pipeline_name}={path}"])
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
        return result, json.loads(report.read_text())

    def test_exact_and_bounded_differences_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest, policy_path, inputs = self.fixture(root)
            result, report = self.run_tool(root, manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(report["comparisonPerformed"])
            self.assertEqual(len(report["inputSeries"]), 6)
            self.assertEqual(len(report["comparisons"]), 4)
            self.assertTrue(all(item["passed"] for item in report["comparisons"]))
            bases = {
                ("linux", "vulkan", "forward-plus"): 0.01,
                ("linux", "vulkan", "deferred"): 0.01,
                ("macos", "metal", "forward-plus"): 0.015,
                ("macos", "metal", "deferred"): 0.015,
            }
            manifest, policy_path, inputs = self.fixture(root / "bounded", bases=bases)
            result, report = self.run_tool(root / "bounded", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(report["status"], "passed")

    def test_visual_identity_missing_tamper_marker_and_duplicate_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, key = Path(temporary), ("linux", "vulkan", "forward-plus")
            manifest, policy_path, inputs = self.fixture(root, bases={key: 0.1})
            result, report = self.run_tool(root, manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(report["comparisonPerformed"])
            self.assertEqual(report["failureCategory"], "visual-metric")
            manifest, policy_path, inputs = self.fixture(root / "identity", qualities={key: "medium"})
            result, report = self.run_tool(root / "identity", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(report["comparisonPerformed"])
            self.assertIn("qualification identity", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(root / "missing")
            doc = json.loads(inputs[key].read_text())
            bundle_path = inputs[key].parent / Path(doc["ticks"][0]["bundlePath"])
            bundle = json.loads(bundle_path.read_text())
            bundle["attachments"].pop()
            write_json(bundle_path, bundle)
            refresh(inputs[key])
            result, report = self.run_tool(root / "missing", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertIn("attachments", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(root / "tamper")
            doc = json.loads(inputs[key].read_text())
            bundle_path = inputs[key].parent / Path(doc["ticks"][0]["bundlePath"])
            bundle = json.loads(bundle_path.read_text())
            raw = bundle_path.parent / bundle["attachments"][0]["dataPath"]
            raw.write_bytes(raw.read_bytes() + b"x")
            result, report = self.run_tool(root / "tamper", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertIn("payload", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(root / "candidate")
            doc = json.loads(inputs[key].read_text())
            doc["status"] = "candidate"
            write_json(inputs[key], doc)
            result, report = self.run_tool(root / "candidate", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertIn("candidate/skip marker", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(
                root / "inconsistent-suite",
                runs={key: "linux-vulkan-different-suite-run"},
            )
            result, report = self.run_tool(
                root / "inconsistent-suite", manifest, policy_path, inputs
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("both pipelines", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(root / "duplicate-receipt")
            doc = json.loads(inputs[key].read_text())
            bundle_path = inputs[key].parent / Path(doc["ticks"][0]["bundlePath"])
            bundle = json.loads(bundle_path.read_text())
            reference = inputs[("windows", "dx12", "forward-plus")]
            reference_doc = json.loads(reference.read_text())
            reference_bundle_path = reference.parent / Path(
                reference_doc["ticks"][0]["bundlePath"]
            )
            reference_bundle = json.loads(reference_bundle_path.read_text())
            receipt = reference_bundle["execution"]["renderExecutionReceiptSha256"]
            bundle["execution"]["renderExecutionReceiptSha256"] = receipt
            for attachment in bundle["attachments"]:
                attachment["provenance"]["renderExecutionReceiptSha256"] = receipt
            write_json(bundle_path, bundle)
            refresh(inputs[key])
            result, report = self.run_tool(
                root / "duplicate-receipt", manifest, policy_path, inputs
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("receipt", report["diagnostic"])

    def test_temporal_freeze_drift_reverse_motion_and_skip_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root, key = Path(temporary), ("macos", "metal", "deferred")
            manifest, policy_path, inputs = self.fixture(root, offsets={key: {360: 0.5, 361: 0.5}})
            result, report = self.run_tool(root, manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(report["comparisonPerformed"])
            self.assertEqual(report["failureCategory"], "temporal-metric")
            manifest, policy_path, inputs = self.fixture(root / "drift", offsets={key: {360: 0.5, 361: 0.7}})
            result, report = self.run_tool(root / "drift", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(report["failureCategory"], "temporal-metric")
            manifest, policy_path, inputs = self.fixture(
                root / "reverse",
                offsets={key: {360: 0.501, 361: 0.5}},
            )
            result, report = self.run_tool(root / "reverse", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(report["failureCategory"], "temporal-metric")
            self.assertIn("signed delta-field", report["diagnostic"])
            manifest, policy_path, inputs = self.fixture(root / "skip")
            doc = json.loads(inputs[key].read_text())
            bundle_path = inputs[key].parent / Path(doc["ticks"][1]["bundlePath"])
            bundle = json.loads(bundle_path.read_text())
            bundle["execution"]["status"] = "skipped"
            write_json(bundle_path, bundle)
            result, report = self.run_tool(root / "skip", manifest, policy_path, inputs)
            self.assertEqual(result.returncode, 1)
            self.assertIn("candidate/skip marker", report["diagnostic"])


if __name__ == "__main__":
    unittest.main()
