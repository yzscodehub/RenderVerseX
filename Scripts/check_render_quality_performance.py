#!/usr/bin/env python3
"""Fail-closed performance qualification for RenderQualityLab.

Performance evidence is accepted only from completion-owned frames after a
60-frame warm-up. The 16.67 ms GPU budget applies only to the conservative
maximum of the Graphics prelude-to-terminal command span and the RenderGraph
pass envelope; it is not a whole-frame, CPU-submit, Present, or multi-queue
busy-time duration. The checker intentionally reports incompatible hardware
or baseline identities as ``notComparable`` and returns a non-zero status
rather than silently treating a different adapter as a passing regression
result.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import re
from pathlib import Path
from typing import Any

from render_quality_performance_baseline import (
    BaselineContractError,
    verify_approved_baseline_record,
)


SCHEMA = "RVX.RenderQualityPerformanceReport"
SCHEMA_VERSION = 3
POLICY_SCHEMA = "RVX.RenderQualityPerformancePolicy"
POLICY_VERSION = 3
CPU_PHASES = ("extraction", "visibility", "graphCompile", "submission")
TELEMETRY_COVERAGE_SCHEMA_VERSION = 2
REQUIRED_TRACKED_UPLOAD_COVERAGE = (1 << 0) | (1 << 1) | (1 << 2)
REQUIRED_TRACKED_ESTIMATED_FOOTPRINT_COVERAGE = (
    (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3)
)
TRACKED_FOOTPRINT_FIELDS = (
    "transientPoolPhysicalEstimatedBytes",
    "importedPersistentEstimatedBytes",
    "importedHistoryEstimatedBytes",
    "resourceRegistryResidentEstimatedBytes",
)
RESOURCE_COUNT_FIELDS = frozenset(
    {
        "stableImportedBuffers",
        "stableImportedTextures",
        "residentMaterials",
        "residentMeshes",
        "residentTextures",
        "transientBuffers",
        "transientTextures",
    }
)
SHA256 = re.compile(r"^[0-9a-f]{64}$")


class ContractError(ValueError):
    """Input cannot be used as performance qualification evidence."""


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ContractError(f"cannot read {label}: {error}") from error
    except json.JSONDecodeError as error:
        raise ContractError(f"{label} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise ContractError(f"{label} root must be an object")
    return value


def _require_keys(value: dict[str, Any], required: set[str], context: str) -> None:
    missing = required - value.keys()
    unknown = value.keys() - required
    if missing:
        raise ContractError(f"{context} is missing fields: {sorted(missing)}")
    if unknown:
        raise ContractError(f"{context} contains unsupported fields: {sorted(unknown)}")


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{context} must be a non-empty string")
    return value


def _sha256(value: Any, context: str) -> str:
    result = _string(value, context)
    if not SHA256.fullmatch(result):
        raise ContractError(f"{context} must be a lowercase SHA-256 digest")
    return result


def _integer(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ContractError(f"{context} must be an integer >= {minimum}")
    return value


def _number(value: Any, context: str, minimum: float = 0.0) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise ContractError(f"{context} must be a finite number")
    result = float(value)
    if result < minimum:
        raise ContractError(f"{context} must be >= {minimum}")
    return result


def _qualification_identity(value: Any, context: str) -> dict[str, str | int]:
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    keys = {
        "sampleId",
        "pipeline",
        "quality",
        "backend",
        "view",
        "qualificationWidth",
        "qualificationHeight",
        "captureTick",
        "visualContractVersion",
    }
    _require_keys(value, keys, context)
    result: dict[str, str | int] = {}
    for key in ("sampleId", "pipeline", "quality", "backend", "view"):
        result[key] = _string(value[key], f"{context}.{key}")
    for key in ("qualificationWidth", "qualificationHeight"):
        result[key] = _integer(value[key], f"{context}.{key}", 1)
    for key in ("captureTick", "visualContractVersion"):
        result[key] = _integer(value[key], f"{context}.{key}", 0)
    return result


def _hardware_identity(value: Any, context: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    keys = {
        "backend",
        "adapterName",
        "driverVersion",
        "osBuild",
        "capabilityHash",
    }
    _require_keys(value, keys, context)
    return {key: _string(value[key], f"{context}.{key}") for key in sorted(keys)}


def _telemetry_coverage(value: Any, context: str) -> dict[str, int]:
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    _require_keys(
        value,
        {
            "schemaVersion",
            "trackedUploadCoverageMask",
            "trackedEstimatedFootprintCoverageMask",
        },
        context,
    )
    result = {
        "schemaVersion": _integer(value["schemaVersion"], f"{context}.schemaVersion", 1),
        "trackedUploadCoverageMask": _integer(
            value["trackedUploadCoverageMask"],
            f"{context}.trackedUploadCoverageMask",
            1,
        ),
        "trackedEstimatedFootprintCoverageMask": _integer(
            value["trackedEstimatedFootprintCoverageMask"],
            f"{context}.trackedEstimatedFootprintCoverageMask",
            1,
        ),
    }
    if (
        result["schemaVersion"] != TELEMETRY_COVERAGE_SCHEMA_VERSION
        or result["trackedUploadCoverageMask"] != REQUIRED_TRACKED_UPLOAD_COVERAGE
        or result["trackedEstimatedFootprintCoverageMask"]
        != REQUIRED_TRACKED_ESTIMATED_FOOTPRINT_COVERAGE
    ):
        raise ContractError(f"{context} does not declare the required tracked/estimated coverage")
    return result


def _tracked_estimated_footprint(
    value: Any, context: str, expected_coverage_mask: int
) -> dict[str, int]:
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    _require_keys(value, {"schemaVersion", "coverageMask", *TRACKED_FOOTPRINT_FIELDS}, context)
    result = {
        "schemaVersion": _integer(value["schemaVersion"], f"{context}.schemaVersion", 1),
        "coverageMask": _integer(value["coverageMask"], f"{context}.coverageMask", 1),
        **{
            name: _integer(value[name], f"{context}.{name}", 0)
            for name in TRACKED_FOOTPRINT_FIELDS
        },
    }
    if (
        result["schemaVersion"] != TELEMETRY_COVERAGE_SCHEMA_VERSION
        or result["coverageMask"] != expected_coverage_mask
    ):
        raise ContractError(f"{context} coverage does not match the report declaration")
    return result


def _resource_counts(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    _require_keys(value, {"schemaVersion", "counts"}, context)
    schema_version = _integer(value["schemaVersion"], f"{context}.schemaVersion", 1)
    counts = value["counts"]
    if schema_version != TELEMETRY_COVERAGE_SCHEMA_VERSION:
        raise ContractError(f"{context}.schemaVersion is unsupported")
    if not isinstance(counts, dict) or not counts:
        raise ContractError(f"{context}.counts must be a non-empty object")
    parsed_counts = {
        _string(name, f"{context} count name"): _integer(
            count, f"{context}.counts.{name}", 0
        )
        for name, count in counts.items()
    }
    if set(parsed_counts) != RESOURCE_COUNT_FIELDS:
        raise ContractError(
            f"{context}.counts must declare the stable persistent/history resource inventory"
        )
    return {
        "schemaVersion": schema_version,
        "counts": parsed_counts,
    }


def _sample(
    value: Any, index: int, telemetry_coverage: dict[str, int]
) -> dict[str, Any]:
    context = f"samples[{index}]"
    if not isinstance(value, dict):
        raise ContractError(f"{context} must be an object")
    _require_keys(
        value,
        {
            "frameSequence",
            "submittedFrame",
            "completedFrame",
            "renderExecutionReceiptSha256",
            "completionOwned",
            "cpuMs",
            "gpu",
            "trackedUploadBytes",
            "trackedUploadCoverageMask",
            "trackedEstimatedFootprint",
            "resourceCounts",
        },
        context,
    )
    result: dict[str, Any] = {
        "frameSequence": _integer(value["frameSequence"], f"{context}.frameSequence", 1),
        "submittedFrame": _integer(value["submittedFrame"], f"{context}.submittedFrame", 1),
        "completedFrame": _integer(value["completedFrame"], f"{context}.completedFrame", 1),
        "renderExecutionReceiptSha256": _sha256(
            value["renderExecutionReceiptSha256"],
            f"{context}.renderExecutionReceiptSha256",
        ),
        "completionOwned": value["completionOwned"],
        "trackedUploadBytes": _integer(
            value["trackedUploadBytes"], f"{context}.trackedUploadBytes", 0
        ),
        "trackedUploadCoverageMask": _integer(
            value["trackedUploadCoverageMask"],
            f"{context}.trackedUploadCoverageMask",
            1,
        ),
    }
    if result["completionOwned"] is not True:
        raise ContractError(f"{context}.completionOwned must be true")
    if result["completedFrame"] < result["submittedFrame"]:
        raise ContractError(f"{context}.completedFrame precedes submittedFrame")
    cpu = value["cpuMs"]
    if not isinstance(cpu, dict):
        raise ContractError(f"{context}.cpuMs must be an object")
    _require_keys(cpu, set(CPU_PHASES), f"{context}.cpuMs")
    result["cpuMs"] = {name: _number(cpu[name], f"{context}.cpuMs.{name}") for name in CPU_PHASES}
    gpu = value["gpu"]
    if not isinstance(gpu, dict):
        raise ContractError(f"{context}.gpu must be an object")
    _require_keys(
        gpu,
        {
            "graphicsTerminalCommandSpanMs",
            "renderGraphPassEnvelopeMs",
            "graphicsQualificationSpanMs",
            "passes",
        },
        f"{context}.gpu",
    )
    passes = gpu["passes"]
    if not isinstance(passes, list) or not passes:
        raise ContractError(f"{context}.gpu.passes must be a non-empty array")
    parsed_passes: list[dict[str, str | int | float]] = []
    for execution_index, pass_value in enumerate(passes):
        pass_context = f"{context}.gpu.passes[{execution_index}]"
        if not isinstance(pass_value, dict):
            raise ContractError(f"{pass_context} must be an object")
        _require_keys(
            pass_value,
            {"executionIndex", "name", "durationMs"},
            pass_context,
        )
        reported_execution_index = _integer(
            pass_value["executionIndex"],
            f"{pass_context}.executionIndex",
            0,
        )
        if reported_execution_index != execution_index:
            raise ContractError(
                f"{pass_context}.executionIndex must be the contiguous ordered index"
            )
        parsed_passes.append(
            {
                "executionIndex": reported_execution_index,
                "name": _string(pass_value["name"], f"{pass_context}.name"),
                "durationMs": _number(
                    pass_value["durationMs"], f"{pass_context}.durationMs"
                ),
            }
        )
    graphics_terminal_command_span = _number(
        gpu["graphicsTerminalCommandSpanMs"],
        f"{context}.gpu.graphicsTerminalCommandSpanMs",
    )
    render_graph_pass_envelope = _number(
        gpu["renderGraphPassEnvelopeMs"],
        f"{context}.gpu.renderGraphPassEnvelopeMs",
    )
    graphics_qualification_span = _number(
        gpu["graphicsQualificationSpanMs"],
        f"{context}.gpu.graphicsQualificationSpanMs",
    )
    expected_qualification_span = max(
        graphics_terminal_command_span,
        render_graph_pass_envelope,
    )
    if not math.isclose(
        graphics_qualification_span,
        expected_qualification_span,
        rel_tol=1e-9,
        abs_tol=1e-9,
    ):
        raise ContractError(
            f"{context}.gpu.graphicsQualificationSpanMs must equal the conservative maximum of the two raw Graphics spans"
        )
    result["gpu"] = {
        "graphicsTerminalCommandSpanMs": graphics_terminal_command_span,
        "renderGraphPassEnvelopeMs": render_graph_pass_envelope,
        "graphicsQualificationSpanMs": graphics_qualification_span,
        "passes": parsed_passes,
    }
    if result["trackedUploadCoverageMask"] != telemetry_coverage["trackedUploadCoverageMask"]:
        raise ContractError(f"{context}.trackedUploadCoverageMask does not match report coverage")
    result["trackedEstimatedFootprint"] = _tracked_estimated_footprint(
        value["trackedEstimatedFootprint"],
        f"{context}.trackedEstimatedFootprint",
        telemetry_coverage["trackedEstimatedFootprintCoverageMask"],
    )
    result["resourceCounts"] = _resource_counts(
        value["resourceCounts"], f"{context}.resourceCounts"
    )
    return result


def load_report(path: Path) -> dict[str, Any]:
    document = _load_json(path, "performance report")
    _require_keys(
        document,
        {
            "schema",
            "schemaVersion",
            "sourceCommit",
            "runId",
            "preset",
            "qualificationIdentity",
            "hardwareIdentity",
            "telemetryCoverage",
            "baselineIdentity",
            "completedWarmupFrames",
            "samples",
        },
        "performance report",
    )
    if document["schema"] != SCHEMA or document["schemaVersion"] != SCHEMA_VERSION:
        raise ContractError("performance report schema/version is unsupported")
    telemetry_coverage = _telemetry_coverage(
        document["telemetryCoverage"], "telemetryCoverage"
    )
    samples_document = document["samples"]
    if not isinstance(samples_document, list):
        raise ContractError("performance report samples must be an array")
    return {
        "path": str(path),
        "sourceCommit": _string(document["sourceCommit"], "sourceCommit"),
        "runId": _string(document["runId"], "runId"),
        "preset": _string(document["preset"], "preset"),
        "qualificationIdentity": _qualification_identity(document["qualificationIdentity"], "qualificationIdentity"),
        "hardwareIdentity": _hardware_identity(document["hardwareIdentity"], "hardwareIdentity"),
        "telemetryCoverage": telemetry_coverage,
        "baselineIdentity": _string(document["baselineIdentity"], "baselineIdentity"),
        "completedWarmupFrames": _integer(document["completedWarmupFrames"], "completedWarmupFrames", 0),
        "samples": [
            _sample(item, index, telemetry_coverage)
            for index, item in enumerate(samples_document)
        ],
    }


def load_policy(path: Path) -> dict[str, Any]:
    document = _load_json(path, "performance policy")
    _require_keys(
        document,
        {
            "schema",
            "schemaVersion",
            "qualificationIdentity",
            "requiredHardwareIdentity",
            "telemetryCoverage",
            "warmupCompletedFrames",
            "minimumCompletedSamples",
            "baseline",
            "thresholds",
        },
        "performance policy",
    )
    if document["schema"] != POLICY_SCHEMA or document["schemaVersion"] != POLICY_VERSION:
        raise ContractError("performance policy schema/version is unsupported")
    telemetry_coverage = _telemetry_coverage(
        document["telemetryCoverage"], "policy telemetryCoverage"
    )
    baseline = document["baseline"]
    if not isinstance(baseline, dict):
        raise ContractError("performance policy baseline must be an object")
    _require_keys(baseline, {"id", "maxRegressionPercent"}, "performance policy baseline")
    max_regression = _number(baseline["maxRegressionPercent"], "baseline.maxRegressionPercent", 0.0)
    thresholds = document["thresholds"]
    if not isinstance(thresholds, dict):
        raise ContractError("performance policy thresholds must be an object")
    _require_keys(
        thresholds,
        {
            "graphicsQualificationSpanP95Ms",
            "renderGraphPassEnvelopeP95Ms",
            "cpuP95Ms",
            "passGpuP95Ms",
            "maxTrackedUploadBytes",
            "maxTransientPoolPhysicalEstimatedBytes",
            "maxImportedPersistentEstimatedBytes",
            "minImportedHistoryEstimatedBytes",
            "maxImportedHistoryEstimatedBytes",
            "maxResourceRegistryResidentEstimatedBytes",
        },
        "performance policy thresholds",
    )
    cpu = thresholds["cpuP95Ms"]
    if not isinstance(cpu, dict):
        raise ContractError("thresholds.cpuP95Ms must be an object")
    _require_keys(cpu, set(CPU_PHASES), "thresholds.cpuP95Ms")
    passes = thresholds["passGpuP95Ms"]
    if not isinstance(passes, list) or not passes:
        raise ContractError("thresholds.passGpuP95Ms must be a non-empty array")
    parsed_pass_thresholds: list[dict[str, str | int | float]] = []
    for execution_index, pass_value in enumerate(passes):
        pass_context = f"thresholds.passGpuP95Ms[{execution_index}]"
        if not isinstance(pass_value, dict):
            raise ContractError(f"{pass_context} must be an object")
        _require_keys(
            pass_value,
            {"executionIndex", "name", "p95Ms"},
            pass_context,
        )
        reported_execution_index = _integer(
            pass_value["executionIndex"],
            f"{pass_context}.executionIndex",
            0,
        )
        if reported_execution_index != execution_index:
            raise ContractError(
                f"{pass_context}.executionIndex must be the contiguous ordered index"
            )
        parsed_pass_thresholds.append(
            {
                "executionIndex": reported_execution_index,
                "name": _string(pass_value["name"], f"{pass_context}.name"),
                "p95Ms": _number(pass_value["p95Ms"], f"{pass_context}.p95Ms"),
            }
        )
    minimum_imported_history_bytes = _integer(
        thresholds["minImportedHistoryEstimatedBytes"],
        "minImportedHistoryEstimatedBytes",
        0,
    )
    maximum_imported_history_bytes = _integer(
        thresholds["maxImportedHistoryEstimatedBytes"],
        "maxImportedHistoryEstimatedBytes",
        0,
    )
    if minimum_imported_history_bytes > maximum_imported_history_bytes:
        raise ContractError(
            "minImportedHistoryEstimatedBytes exceeds maxImportedHistoryEstimatedBytes"
        )
    return {
        "qualificationIdentity": _qualification_identity(document["qualificationIdentity"], "policy qualificationIdentity"),
        "requiredHardwareIdentity": _hardware_identity(document["requiredHardwareIdentity"], "policy requiredHardwareIdentity"),
        "telemetryCoverage": telemetry_coverage,
        "warmupCompletedFrames": _integer(document["warmupCompletedFrames"], "warmupCompletedFrames", 60),
        "minimumCompletedSamples": _integer(document["minimumCompletedSamples"], "minimumCompletedSamples", 300),
        "baseline": {"id": _string(baseline["id"], "baseline.id"), "maxRegressionPercent": max_regression},
        "thresholds": {
            "graphicsQualificationSpanP95Ms": _number(
                thresholds["graphicsQualificationSpanP95Ms"],
                "graphicsQualificationSpanP95Ms",
            ),
            "renderGraphPassEnvelopeP95Ms": _number(
                thresholds["renderGraphPassEnvelopeP95Ms"],
                "renderGraphPassEnvelopeP95Ms",
            ),
            "cpuP95Ms": {name: _number(cpu[name], f"cpuP95Ms.{name}") for name in CPU_PHASES},
            "passGpuP95Ms": parsed_pass_thresholds,
            "maxTrackedUploadBytes": _integer(
                thresholds["maxTrackedUploadBytes"], "maxTrackedUploadBytes", 0
            ),
            "maxTransientPoolPhysicalEstimatedBytes": _integer(
                thresholds["maxTransientPoolPhysicalEstimatedBytes"],
                "maxTransientPoolPhysicalEstimatedBytes",
                0,
            ),
            "maxImportedPersistentEstimatedBytes": _integer(
                thresholds["maxImportedPersistentEstimatedBytes"],
                "maxImportedPersistentEstimatedBytes",
                0,
            ),
            "minImportedHistoryEstimatedBytes": minimum_imported_history_bytes,
            "maxImportedHistoryEstimatedBytes": maximum_imported_history_bytes,
            "maxResourceRegistryResidentEstimatedBytes": _integer(
                thresholds["maxResourceRegistryResidentEstimatedBytes"],
                "maxResourceRegistryResidentEstimatedBytes",
                0,
            ),
        },
    }


def percentile_95(values: list[float | int]) -> float:
    if not values:
        raise ContractError("cannot calculate p95 from no samples")
    ordered = sorted(float(value) for value in values)
    return ordered[math.ceil(len(ordered) * 0.95) - 1]


def _validate_sample_series(report: dict[str, Any], policy: dict[str, Any]) -> None:
    if report["telemetryCoverage"] != policy["telemetryCoverage"]:
        raise ContractError("report tracked/estimated telemetry coverage does not match policy")
    if report["completedWarmupFrames"] < policy["warmupCompletedFrames"]:
        raise ContractError("report has fewer completed warmup frames than required")
    samples = report["samples"]
    if len(samples) < policy["minimumCompletedSamples"]:
        raise ContractError("report has fewer completion-owned samples than required")
    last_frame = report["completedWarmupFrames"] - 1
    last_sequence = 0
    last_submitted = 0
    observed_receipts: set[str] = set()
    reference_counts: dict[str, int] | None = None
    reference_footprint: dict[str, int] | None = None
    expected_passes: list[tuple[int, str]] | None = None
    minimum_imported_history_bytes = policy["thresholds"][
        "minImportedHistoryEstimatedBytes"
    ]
    qualification = report["qualificationIdentity"]
    requires_full_resolution_taa_history = (
        qualification["pipeline"].casefold() == "deferred"
        and qualification["quality"].casefold() == "high"
        and qualification["qualificationWidth"] == 2560
        and qualification["qualificationHeight"] == 1440
        and any(
            pass_value["name"] == "TAAResolve"
            for pass_value in samples[0]["gpu"]["passes"]
        )
    )
    required_full_resolution_taa_history_bytes = 88_473_600
    if (
        requires_full_resolution_taa_history
        and minimum_imported_history_bytes
        < required_full_resolution_taa_history_bytes
    ):
        raise ContractError(
            "Deferred/High 2560x1440 TAA policy minimum imported history is below 88473600 bytes"
        )
    for index, sample in enumerate(samples):
        if sample["completedFrame"] <= last_frame:
            raise ContractError(f"samples[{index}] completion frame is stale or before warmup")
        last_frame = sample["completedFrame"]
        if sample["frameSequence"] <= last_sequence:
            raise ContractError(f"samples[{index}] frame sequence is stale or out of order")
        last_sequence = sample["frameSequence"]
        if sample["submittedFrame"] <= last_submitted:
            raise ContractError(f"samples[{index}] submitted frame is stale or out of order")
        last_submitted = sample["submittedFrame"]
        receipt = sample["renderExecutionReceiptSha256"]
        if receipt in observed_receipts:
            raise ContractError(f"samples[{index}] reuses a render execution receipt")
        observed_receipts.add(receipt)
        if (
            sample["trackedEstimatedFootprint"]["importedHistoryEstimatedBytes"]
            < minimum_imported_history_bytes
        ):
            raise ContractError(
                f"samples[{index}] imported history estimated bytes are below policy minimum"
            )
        if reference_counts is None:
            reference_counts = sample["resourceCounts"]["counts"]
            reference_footprint = sample["trackedEstimatedFootprint"]
            expected_passes = [
                (pass_value["executionIndex"], pass_value["name"])
                for pass_value in sample["gpu"]["passes"]
            ]
        else:
            if set(sample["resourceCounts"]["counts"]) != set(reference_counts):
                raise ContractError("post-warmup resource count key set changed")
            actual_passes = [
                (pass_value["executionIndex"], pass_value["name"])
                for pass_value in sample["gpu"]["passes"]
            ]
            if actual_passes != expected_passes:
                raise ContractError("post-warmup ordered GPU pass timing schema changed")
            for name, count in sample["resourceCounts"]["counts"].items():
                if count != reference_counts[name]:
                    raise ContractError(f"post-warmup resource count changed for {name}")
            for name in TRACKED_FOOTPRINT_FIELDS:
                if sample["trackedEstimatedFootprint"][name] != reference_footprint[name]:
                    raise ContractError(
                        f"post-warmup tracked estimated footprint changed for {name}"
                    )


def summarize(report: dict[str, Any]) -> dict[str, Any]:
    samples = report["samples"]
    pass_schema = samples[0]["gpu"]["passes"]
    return {
        "graphicsQualificationSpanP95Ms": percentile_95(
            [
                sample["gpu"]["graphicsQualificationSpanMs"]
                for sample in samples
            ]
        ),
        "graphicsTerminalCommandSpanP95Ms": percentile_95(
            [
                sample["gpu"]["graphicsTerminalCommandSpanMs"]
                for sample in samples
            ]
        ),
        "renderGraphPassEnvelopeP95Ms": percentile_95(
            [sample["gpu"]["renderGraphPassEnvelopeMs"] for sample in samples]
        ),
        "cpuP95Ms": {phase: percentile_95([sample["cpuMs"][phase] for sample in samples]) for phase in CPU_PHASES},
        "passGpuP95Ms": [
            {
                "executionIndex": pass_value["executionIndex"],
                "name": pass_value["name"],
                "p95Ms": percentile_95(
                    [
                        sample["gpu"]["passes"][execution_index]["durationMs"]
                        for sample in samples
                    ]
                ),
            }
            for execution_index, pass_value in enumerate(pass_schema)
        ],
        "maxTrackedUploadBytes": max(
            sample["trackedUploadBytes"] for sample in samples
        ),
        "maxTrackedEstimatedFootprintBytes": {
            field: max(
                sample["trackedEstimatedFootprint"][field] for sample in samples
            )
            for field in TRACKED_FOOTPRINT_FIELDS
        },
        "sampleCount": len(samples),
    }


def _threshold_failures(summary: dict[str, Any], policy: dict[str, Any]) -> list[str]:
    thresholds = policy["thresholds"]
    failures: list[str] = []
    if (
        summary["graphicsQualificationSpanP95Ms"]
        > thresholds["graphicsQualificationSpanP95Ms"]
    ):
        failures.append("conservative Graphics qualification span p95 exceeds policy")
    if (
        summary["renderGraphPassEnvelopeP95Ms"]
        > thresholds["renderGraphPassEnvelopeP95Ms"]
    ):
        failures.append("RenderGraph pass envelope p95 exceeds policy")
    for phase, value in summary["cpuP95Ms"].items():
        if value > thresholds["cpuP95Ms"][phase]:
            failures.append(f"CPU {phase} p95 exceeds policy")
    summary_passes = summary["passGpuP95Ms"]
    threshold_passes = thresholds["passGpuP95Ms"]
    if [
        (pass_value["executionIndex"], pass_value["name"])
        for pass_value in summary_passes
    ] != [
        (pass_value["executionIndex"], pass_value["name"])
        for pass_value in threshold_passes
    ]:
        failures.append("ordered per-pass GPU timing schema does not match policy")
    else:
        for summary_pass, threshold_pass in zip(
            summary_passes, threshold_passes, strict=True
        ):
            if summary_pass["p95Ms"] > threshold_pass["p95Ms"]:
                failures.append(
                    "GPU pass "
                    f"{summary_pass['executionIndex']}:{summary_pass['name']} "
                    "p95 exceeds policy"
                )
    if summary["maxTrackedUploadBytes"] > thresholds["maxTrackedUploadBytes"]:
        failures.append("tracked upload bytes exceed policy")
    for field, threshold_key in (
        (
            "transientPoolPhysicalEstimatedBytes",
            "maxTransientPoolPhysicalEstimatedBytes",
        ),
        (
            "importedPersistentEstimatedBytes",
            "maxImportedPersistentEstimatedBytes",
        ),
        (
            "importedHistoryEstimatedBytes",
            "maxImportedHistoryEstimatedBytes",
        ),
        (
            "resourceRegistryResidentEstimatedBytes",
            "maxResourceRegistryResidentEstimatedBytes",
        ),
    ):
        if summary["maxTrackedEstimatedFootprintBytes"][field] > thresholds[threshold_key]:
            failures.append(f"tracked estimated footprint {field} exceeds policy")
    return failures


def _baseline_failures(current: dict[str, Any], baseline: dict[str, Any], regression_percent: float) -> list[str]:
    failures: list[str] = []
    allowance = 1.0 + regression_percent / 100.0
    for field in (
        "graphicsQualificationSpanP95Ms",
        "renderGraphPassEnvelopeP95Ms",
        "maxTrackedUploadBytes",
    ):
        if current[field] > baseline[field] * allowance:
            failures.append(f"{field} regressed beyond baseline allowance")
    for group in ("cpuP95Ms", "maxTrackedEstimatedFootprintBytes"):
        if set(current[group]) != set(baseline[group]):
            failures.append(f"{group} key set differs from baseline")
            continue
        for name, value in current[group].items():
            if value > baseline[group][name] * allowance:
                failures.append(f"{group}.{name} regressed beyond baseline allowance")

    current_passes = current["passGpuP95Ms"]
    baseline_passes = baseline["passGpuP95Ms"]
    if [
        (pass_value["executionIndex"], pass_value["name"])
        for pass_value in current_passes
    ] != [
        (pass_value["executionIndex"], pass_value["name"])
        for pass_value in baseline_passes
    ]:
        failures.append("passGpuP95Ms ordered schema differs from baseline")
    else:
        for current_pass, baseline_pass in zip(
            current_passes, baseline_passes, strict=True
        ):
            if current_pass["p95Ms"] > baseline_pass["p95Ms"] * allowance:
                failures.append(
                    "passGpuP95Ms."
                    f"{current_pass['executionIndex']}:{current_pass['name']} "
                    "regressed beyond baseline allowance"
                )
    return failures


def _write_report(path: Path | None, value: dict[str, Any]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path, help="current completion-owned performance report")
    parser.add_argument("--baseline", required=True, type=Path, help="approved report from the identical hardware identity")
    parser.add_argument("--policy", required=True, type=Path, help="performance threshold and baseline policy")
    parser.add_argument(
        "--baseline-record",
        required=True,
        type=Path,
        help="approved V1 record binding the baseline report and policy",
    )
    parser.add_argument("--expected-source-commit", required=True, help="fail if current evidence belongs to another commit")
    parser.add_argument("--expected-run-id", required=True, help="fail if current evidence belongs to another run")
    parser.add_argument("--expected-preset", required=True, help="fail if current evidence belongs to another preset")
    parser.add_argument("--out", type=Path, help="result JSON path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result: dict[str, Any] = {"schema": "RVX.RenderQualityPerformanceResult", "schemaVersion": 1, "status": "failed", "failures": []}
    try:
        current = load_report(args.report)
        baseline = load_report(args.baseline)
        policy = load_policy(args.policy)
        baseline_record = verify_approved_baseline_record(
            args.baseline_record,
            args.baseline,
            args.policy,
        )
        result["baselineRecord"] = {
            "baselineId": baseline_record["baselineId"],
            "status": baseline_record["status"],
        }
        if current["sourceCommit"] != args.expected_source_commit:
            raise ContractError("current performance evidence is stale for the expected source commit")
        if current["runId"] != args.expected_run_id:
            raise ContractError("current performance evidence belongs to another run")
        if current["preset"] != args.expected_preset:
            raise ContractError("current performance evidence belongs to another preset")
        _validate_sample_series(current, policy)
        _validate_sample_series(baseline, policy)
        result["qualificationIdentity"] = current["qualificationIdentity"]
        result["hardwareIdentity"] = current["hardwareIdentity"]
        # Identity mismatches are not a performance regression and must never
        # produce a pass.  CI can surface this state distinctly for rebaselining.
        if (
            current["qualificationIdentity"] != policy["qualificationIdentity"]
            or baseline["qualificationIdentity"] != policy["qualificationIdentity"]
            or current["hardwareIdentity"] != policy["requiredHardwareIdentity"]
            or baseline["hardwareIdentity"] != policy["requiredHardwareIdentity"]
            or current["hardwareIdentity"] != baseline["hardwareIdentity"]
            or current["baselineIdentity"] != policy["baseline"]["id"]
            or baseline["baselineIdentity"] != policy["baseline"]["id"]
        ):
            result.update({"status": "notComparable", "failures": ["hardware, baseline, or qualification identity mismatch"]})
            _write_report(args.out, result)
            print("Render quality performance is not comparable to its baseline", file=sys.stderr)
            return 2
        current_summary = summarize(current)
        baseline_summary = summarize(baseline)
        failures = _threshold_failures(current_summary, policy)
        failures.extend(_baseline_failures(current_summary, baseline_summary, policy["baseline"]["maxRegressionPercent"]))
        result.update({"current": current_summary, "baseline": baseline_summary, "failures": failures, "status": "passed" if not failures else "failed"})
        _write_report(args.out, result)
        if failures:
            print("Render quality performance qualification failed:", *failures, sep="\n  - ")
            return 1
        print("Render quality performance qualification passed")
        return 0
    except (ContractError, BaselineContractError) as error:
        result["failures"] = [str(error)]
        _write_report(args.out, result)
        print(f"Render quality performance qualification rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
