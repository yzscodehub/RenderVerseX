#!/usr/bin/env python3
"""Prepare, approve, and verify fixed-GPU performance baselines.

Raw calibration runs remain build artifacts. Only an explicitly approved
baseline report, policy, and record are suitable for version control.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "RVX.RenderQualityPerformanceBaselineRecord"
SCHEMA_VERSION = 1
REPORT_SCHEMA = "RVX.RenderQualityPerformanceReport"
REPORT_VERSION = 3
POLICY_SCHEMA = "RVX.RenderQualityPerformancePolicy"
POLICY_VERSION = 3
RECEIPT_SCHEMA = "RVX.AgentHarness.RequiredSuiteReceipt"
RECEIPT_VERSION = 3
RUN_COUNT = 5
SELECTION_METRIC = "graphicsQualificationSpanP95Ms"
SELECTION_RULE = "median-p95"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


class BaselineContractError(ValueError):
    """Performance baseline evidence is incomplete or inconsistent."""


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise BaselineContractError(f"cannot read {label}: {error}") from error
    except json.JSONDecodeError as error:
        raise BaselineContractError(f"{label} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise BaselineContractError(f"{label} root must be an object")
    return value


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def _require_keys(value: dict[str, Any], keys: set[str], context: str) -> None:
    missing = keys - value.keys()
    unknown = value.keys() - keys
    if missing:
        raise BaselineContractError(f"{context} is missing fields: {sorted(missing)}")
    if unknown:
        raise BaselineContractError(
            f"{context} contains unsupported fields: {sorted(unknown)}"
        )


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise BaselineContractError(f"{context} must be a non-empty string")
    return value


def _sha(value: Any, context: str) -> str:
    digest = _string(value, context)
    if not SHA256.fullmatch(digest):
        raise BaselineContractError(f"{context} must be a lowercase SHA-256 digest")
    return digest


def _integer(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise BaselineContractError(f"{context} must be an integer >= {minimum}")
    return value


def _number(value: Any, context: str, minimum: float = 0.0) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise BaselineContractError(f"{context} must be a finite number")
    result = float(value)
    if result < minimum:
        raise BaselineContractError(f"{context} must be >= {minimum}")
    return result


def _nullable_string(value: Any, context: str) -> str | None:
    return None if value is None else _string(value, context)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _report_p95(document: dict[str, Any], field: str) -> float:
    samples = document.get("samples")
    if not isinstance(samples, list) or not samples:
        raise BaselineContractError("baseline report has no samples")
    values: list[float] = []
    for index, sample in enumerate(samples):
        gpu = sample.get("gpu") if isinstance(sample, dict) else None
        if not isinstance(gpu, dict):
            raise BaselineContractError(f"baseline sample {index} has no GPU data")
        values.append(_number(gpu.get(field), f"baseline samples[{index}].{field}"))
    values.sort()
    return values[math.ceil(len(values) * 0.95) - 1]


def _contained_path(root: Path, relative: Any, context: str) -> Path:
    candidate = Path(_string(relative, context))
    if candidate.is_absolute():
        raise BaselineContractError(f"{context} must be record-relative")
    resolved_root = root.resolve()
    resolved = (resolved_root / candidate).resolve()
    if resolved != resolved_root and resolved_root not in resolved.parents:
        raise BaselineContractError(f"{context} escapes the record directory")
    return resolved


def _source_fingerprint(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BaselineContractError(f"{context} must be an object")
    _require_keys(value, {"algorithm", "fileCount", "digest"}, context)
    return {
        "algorithm": _string(value["algorithm"], f"{context}.algorithm"),
        "fileCount": _integer(value["fileCount"], f"{context}.fileCount", 1),
        "digest": _sha(value["digest"], f"{context}.digest"),
    }


def _parse_record(record_path: Path) -> dict[str, Any]:
    document = _load_json(record_path, "performance baseline record")
    _require_keys(
        document,
        {
            "schema", "schemaVersion", "baselineId", "status", "source",
            "qualificationIdentity", "hardwareIdentity", "baseline", "policy",
            "calibration", "approval",
        },
        "performance baseline record",
    )
    if document["schema"] != SCHEMA or document["schemaVersion"] != SCHEMA_VERSION:
        raise BaselineContractError(
            "performance baseline record schema/version is unsupported"
        )
    status = document["status"]
    if status not in ("candidate", "approved"):
        raise BaselineContractError("performance baseline record status is unsupported")

    source = document["source"]
    if not isinstance(source, dict):
        raise BaselineContractError("source must be an object")
    _require_keys(source, {"commit", "fingerprint"}, "source")
    baseline = document["baseline"]
    if not isinstance(baseline, dict):
        raise BaselineContractError("baseline must be an object")
    _require_keys(
        baseline,
        {
            "reportPath", "reportSha256", "runId", "preset",
            "requiredSuiteReceiptPath", "requiredSuiteReceiptSha256",
        },
        "baseline",
    )
    policy = document["policy"]
    if not isinstance(policy, dict):
        raise BaselineContractError("policy must be an object")
    _require_keys(policy, {"path", "sha256"}, "policy")

    calibration = document["calibration"]
    if not isinstance(calibration, dict):
        raise BaselineContractError("calibration must be an object")
    _require_keys(
        calibration,
        {
            "requiredRunCount", "selectionMetric", "selectionRule",
            "selectedRunOrdinal", "runs",
        },
        "calibration",
    )
    if calibration["requiredRunCount"] != RUN_COUNT:
        raise BaselineContractError("calibration must contain exactly five runs")
    if calibration["selectionMetric"] != SELECTION_METRIC:
        raise BaselineContractError("calibration selection metric is unsupported")
    if calibration["selectionRule"] != SELECTION_RULE:
        raise BaselineContractError("calibration selection rule is unsupported")
    selected_ordinal = _integer(
        calibration["selectedRunOrdinal"], "calibration.selectedRunOrdinal", 1
    )
    values = calibration["runs"]
    if not isinstance(values, list) or len(values) != RUN_COUNT:
        raise BaselineContractError("calibration.runs must contain exactly five entries")
    runs: list[dict[str, Any]] = []
    run_keys = {
        "ordinal", "runId", "reportSha256", "requiredSuiteReceiptPath",
        "requiredSuiteReceiptSha256", "graphicsQualificationSpanP95Ms",
        "renderGraphPassEnvelopeP95Ms",
    }
    for index, value in enumerate(values):
        context = f"calibration.runs[{index}]"
        if not isinstance(value, dict):
            raise BaselineContractError(f"{context} must be an object")
        _require_keys(value, run_keys, context)
        ordinal = _integer(value["ordinal"], f"{context}.ordinal", 1)
        if ordinal != index + 1:
            raise BaselineContractError("calibration run ordinals must be contiguous")
        runs.append(
            {
                "ordinal": ordinal,
                "runId": _string(value["runId"], f"{context}.runId"),
                "reportSha256": _sha(
                    value["reportSha256"], f"{context}.reportSha256"
                ),
                "requiredSuiteReceiptPath": _string(
                    value["requiredSuiteReceiptPath"],
                    f"{context}.requiredSuiteReceiptPath",
                ),
                "requiredSuiteReceiptSha256": _sha(
                    value["requiredSuiteReceiptSha256"],
                    f"{context}.requiredSuiteReceiptSha256",
                ),
                "graphicsQualificationSpanP95Ms": _number(
                    value["graphicsQualificationSpanP95Ms"],
                    f"{context}.graphicsQualificationSpanP95Ms",
                ),
                "renderGraphPassEnvelopeP95Ms": _number(
                    value["renderGraphPassEnvelopeP95Ms"],
                    f"{context}.renderGraphPassEnvelopeP95Ms",
                ),
            }
        )
    if selected_ordinal not in {run["ordinal"] for run in runs}:
        raise BaselineContractError("selected calibration run does not exist")
    for field in ("runId", "reportSha256", "requiredSuiteReceiptSha256"):
        if len({run[field] for run in runs}) != RUN_COUNT:
            raise BaselineContractError(f"calibration {field} values must be unique")
    selected = runs[selected_ordinal - 1]
    deterministic_median = sorted(
        runs,
        key=lambda run: (
            run["graphicsQualificationSpanP95Ms"],
            run["runId"],
        ),
    )[RUN_COUNT // 2]
    if selected["ordinal"] != deterministic_median["ordinal"]:
        raise BaselineContractError("selected run is not the deterministic p95 median")
    if len({run["requiredSuiteReceiptPath"] for run in runs}) != RUN_COUNT:
        raise BaselineContractError("calibration receipt paths must be unique")

    approval = document["approval"]
    if not isinstance(approval, dict):
        raise BaselineContractError("approval must be an object")
    _require_keys(approval, {"approvedBy", "approvedAtUtc", "reason"}, "approval")
    return {
        "baselineId": _string(document["baselineId"], "baselineId"),
        "status": status,
        "source": {
            "commit": _string(source["commit"], "source.commit"),
            "fingerprint": _source_fingerprint(
                source["fingerprint"], "source.fingerprint"
            ),
        },
        "qualificationIdentity": document["qualificationIdentity"],
        "hardwareIdentity": document["hardwareIdentity"],
        "baseline": {
            "reportPath": _string(baseline["reportPath"], "baseline.reportPath"),
            "reportSha256": _sha(
                baseline["reportSha256"], "baseline.reportSha256"
            ),
            "runId": _string(baseline["runId"], "baseline.runId"),
            "preset": _string(baseline["preset"], "baseline.preset"),
            "requiredSuiteReceiptPath": _string(
                baseline["requiredSuiteReceiptPath"],
                "baseline.requiredSuiteReceiptPath",
            ),
            "requiredSuiteReceiptSha256": _sha(
                baseline["requiredSuiteReceiptSha256"],
                "baseline.requiredSuiteReceiptSha256",
            ),
        },
        "policy": {
            "path": _string(policy["path"], "policy.path"),
            "sha256": _sha(policy["sha256"], "policy.sha256"),
        },
        "calibration": {
            "requiredRunCount": RUN_COUNT,
            "selectionMetric": SELECTION_METRIC,
            "selectionRule": SELECTION_RULE,
            "selectedRunOrdinal": selected_ordinal,
            "runs": runs,
        },
        "approval": {
            key: _nullable_string(approval[key], f"approval.{key}")
            for key in ("approvedBy", "approvedAtUtc", "reason")
        },
    }


def _verify_bundled_receipt(
    root: Path,
    run: dict[str, Any],
    source: dict[str, Any],
) -> None:
    receipt_path = _contained_path(
        root,
        run["requiredSuiteReceiptPath"],
        "calibration requiredSuiteReceiptPath",
    )
    if not receipt_path.is_file():
        raise BaselineContractError("bundled RequiredSuite receipt is missing")
    if sha256_file(receipt_path) != run["requiredSuiteReceiptSha256"]:
        raise BaselineContractError("bundled RequiredSuite receipt hash mismatch")
    receipt = _load_json(receipt_path, "bundled RequiredSuite receipt")
    if (
        receipt.get("schema") != RECEIPT_SCHEMA
        or receipt.get("schemaVersion") != RECEIPT_VERSION
        or receipt.get("runId") != run["runId"]
        or receipt.get("requiredLabel")
        != "suite.gfx24-performance-calibration"
        or receipt.get("selectedTests") != [
            "RenderVerseSamplesCookedDX12Gfx24DeferredHighPerformanceCalibration"
        ]
    ):
        raise BaselineContractError("bundled RequiredSuite receipt identity mismatch")
    receipt_source = receipt.get("source")
    if not isinstance(receipt_source, dict):
        raise BaselineContractError("bundled receipt has no source identity")
    before = receipt_source.get("fingerprintBefore")
    after = receipt_source.get("fingerprintAfter")
    if (
        receipt_source.get("commit") != source["commit"]
        or before != after
        or _source_fingerprint(before, "bundled receipt fingerprint")
        != source["fingerprint"]
    ):
        raise BaselineContractError("bundled receipt source identity mismatch")
    junit = receipt.get("junit")
    outcome = junit.get("outcome") if isinstance(junit, dict) else None
    zero_fields = (
        "failureCount", "errorCount", "skippedCount", "notRunCount",
        "disabledSuiteCount",
    )
    if (
        not isinstance(outcome, dict)
        or outcome.get("testCaseCount") != 1
        or any(outcome.get(name) != 0 for name in zero_fields)
    ):
        raise BaselineContractError("bundled receipt JUnit outcome is not 1/1")
    semantic = receipt.get("semanticArtifacts")
    if not isinstance(semantic, list) or not any(
        isinstance(item, dict)
        and item.get("sha256") == run["reportSha256"]
        for item in semantic
    ):
        raise BaselineContractError(
            "bundled receipt does not bind its calibration report hash"
        )


def verify_baseline_record(
    record_path: Path,
    baseline_path: Path,
    policy_path: Path,
    *,
    require_approved: bool,
) -> dict[str, Any]:
    record_path = record_path.resolve()
    baseline_path = baseline_path.resolve()
    policy_path = policy_path.resolve()
    record = _parse_record(record_path)
    root = record_path.parent
    if _contained_path(
        root, record["baseline"]["reportPath"], "baseline.reportPath"
    ) != baseline_path:
        raise BaselineContractError("baseline report path does not match record")
    if _contained_path(root, record["policy"]["path"], "policy.path") != policy_path:
        raise BaselineContractError("performance policy path does not match record")
    if sha256_file(baseline_path) != record["baseline"]["reportSha256"]:
        raise BaselineContractError("baseline report hash does not match record")
    if sha256_file(policy_path) != record["policy"]["sha256"]:
        raise BaselineContractError("performance policy hash does not match record")

    report = _load_json(baseline_path, "baseline report")
    policy = _load_json(policy_path, "performance policy")
    if report.get("schema") != REPORT_SCHEMA or report.get("schemaVersion") != REPORT_VERSION:
        raise BaselineContractError("baseline report must be V3")
    if policy.get("schema") != POLICY_SCHEMA or policy.get("schemaVersion") != POLICY_VERSION:
        raise BaselineContractError("performance policy must be V3")
    if (
        report.get("sourceCommit") != record["source"]["commit"]
        or report.get("runId") != record["baseline"]["runId"]
        or report.get("preset") != record["baseline"]["preset"]
        or report.get("baselineIdentity") != record["baselineId"]
        or report.get("qualificationIdentity") != record["qualificationIdentity"]
        or report.get("hardwareIdentity") != record["hardwareIdentity"]
    ):
        raise BaselineContractError("baseline report identity does not match record")
    policy_baseline = policy.get("baseline")
    if (
        not isinstance(policy_baseline, dict)
        or policy_baseline.get("id") != record["baselineId"]
        or policy.get("qualificationIdentity") != record["qualificationIdentity"]
        or policy.get("requiredHardwareIdentity") != record["hardwareIdentity"]
    ):
        raise BaselineContractError("performance policy identity does not match record")
    selected = record["calibration"]["runs"][
        record["calibration"]["selectedRunOrdinal"] - 1
    ]
    if not math.isclose(
        selected["graphicsQualificationSpanP95Ms"],
        _report_p95(report, "graphicsQualificationSpanMs"),
        rel_tol=0.0,
        abs_tol=1e-12,
    ) or not math.isclose(
        selected["renderGraphPassEnvelopeP95Ms"],
        _report_p95(report, "renderGraphPassEnvelopeMs"),
        rel_tol=0.0,
        abs_tol=1e-12,
    ):
        raise BaselineContractError("selected run metrics do not match baseline report")
    if (
        selected["runId"] != record["baseline"]["runId"]
        or selected["reportSha256"] != record["baseline"]["reportSha256"]
        or selected["requiredSuiteReceiptPath"]
        != record["baseline"]["requiredSuiteReceiptPath"]
        or selected["requiredSuiteReceiptSha256"]
        != record["baseline"]["requiredSuiteReceiptSha256"]
    ):
        raise BaselineContractError("selected run does not bind the baseline")
    for run in record["calibration"]["runs"]:
        _verify_bundled_receipt(root, run, record["source"])

    approval = record["approval"]
    if record["status"] == "approved":
        if not all(approval.values()):
            raise BaselineContractError("approved baseline has incomplete approval")
        try:
            approved_at = datetime.fromisoformat(
                str(approval["approvedAtUtc"]).replace("Z", "+00:00")
            )
        except ValueError as error:
            raise BaselineContractError("approval timestamp is invalid") from error
        if approved_at.tzinfo is None:
            raise BaselineContractError("approval timestamp needs a timezone")
    elif any(value is not None for value in approval.values()):
        raise BaselineContractError("candidate baseline cannot contain approval")
    if require_approved and record["status"] != "approved":
        raise BaselineContractError(
            "performance baseline record is a candidate and is not approved"
        )
    return record


def verify_approved_baseline_record(
    record_path: Path, baseline_path: Path, policy_path: Path
) -> dict[str, Any]:
    return verify_baseline_record(
        record_path, baseline_path, policy_path, require_approved=True
    )


def _validate_receipt(
    receipt_path: Path,
    report_path: Path,
    report: dict[str, Any],
) -> tuple[dict[str, Any], str, str]:
    receipt = _load_json(receipt_path, "required-suite receipt")
    if (
        receipt.get("schema") != RECEIPT_SCHEMA
        or receipt.get("schemaVersion") != RECEIPT_VERSION
    ):
        raise BaselineContractError("required-suite receipt version is unsupported")
    if receipt.get("runId") != report["runId"]:
        raise BaselineContractError("receipt run id does not match report")
    if receipt.get("requiredLabel") != "suite.gfx24-performance-calibration":
        raise BaselineContractError("receipt has the wrong suite label")
    if receipt.get("selectedTests") != [
        "RenderVerseSamplesCookedDX12Gfx24DeferredHighPerformanceCalibration"
    ]:
        raise BaselineContractError("receipt must bind the one calibration test")
    source = receipt.get("source")
    if not isinstance(source, dict) or source.get("commit") != report["sourceCommit"]:
        raise BaselineContractError("receipt source does not match report")
    before = source.get("fingerprintBefore")
    after = source.get("fingerprintAfter")
    if before != after:
        raise BaselineContractError("source changed during calibration")
    fingerprint = _source_fingerprint(before, "receipt source fingerprint")
    junit = receipt.get("junit")
    outcome = junit.get("outcome") if isinstance(junit, dict) else None
    if not isinstance(outcome, dict):
        raise BaselineContractError("receipt has no JUnit outcome")
    zero_fields = (
        "failureCount", "errorCount", "skippedCount", "notRunCount",
        "disabledSuiteCount",
    )
    if outcome.get("testCaseCount") != 1 or any(
        outcome.get(name) != 0 for name in zero_fields
    ):
        raise BaselineContractError("receipt JUnit outcome is not 1/1")
    report_sha = sha256_file(report_path)
    semantic = receipt.get("semanticArtifacts")
    if not isinstance(semantic, list) or not any(
        isinstance(item, dict) and item.get("sha256") == report_sha
        for item in semantic
    ):
        raise BaselineContractError("receipt does not bind performance report")
    immutable_path = _string(
        receipt.get("immutableReceiptPath"), "receipt.immutableReceiptPath"
    )
    return fingerprint, sha256_file(receipt_path), immutable_path


def _scaled(value: float | int, percent: float) -> float | int:
    scaled = float(value) * (1.0 + percent / 100.0)
    return math.ceil(scaled) if type(value) is int else scaled


def _assert_approval_source_is_clean(expected_commit: str) -> None:
    root = Path(__file__).resolve().parents[1]
    safe = root.as_posix()
    common = ["git", "-c", f"safe.directory={safe}", "-C", str(root)]
    commit = subprocess.run(
        [*common, "rev-parse", "HEAD"],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()
    if commit != expected_commit:
        raise BaselineContractError(
            "approval must run at the exact calibrated source commit"
        )
    status = subprocess.run(
        [*common, "status", "--porcelain=v1", "--untracked-files=all"],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    allowed_prefixes = (".agent-harness/", ".codex-validation/")
    product_changes = [
        line for line in status
        if not line[3:].replace("\\", "/").startswith(allowed_prefixes)
    ]
    if product_changes:
        raise BaselineContractError(
            "approval requires a clean product worktree at the calibrated commit"
        )


def _prepare(args: argparse.Namespace) -> int:
    import check_render_quality_performance as performance

    run_dirs = [path.resolve() for path in args.run_dir]
    if len(run_dirs) != RUN_COUNT:
        raise BaselineContractError("prepare requires exactly five --run-dir values")
    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        raise BaselineContractError("candidate output directory must be absent or empty")
    output_dir.mkdir(parents=True, exist_ok=True)

    runs: list[dict[str, Any]] = []
    reference: dict[str, Any] | None = None
    for ordinal, run_dir in enumerate(run_dirs, start=1):
        report_path = run_dir / "performance.json"
        receipt_path = run_dir / "required-suite-receipt.json"
        report = performance.load_report(report_path)
        if report["baselineIdentity"] != args.baseline_id:
            raise BaselineContractError("calibration baseline id is unexpected")
        if report["preset"] != args.expected_preset:
            raise BaselineContractError("calibration preset is unexpected")
        if report["sourceCommit"] != args.expected_source_commit:
            raise BaselineContractError("calibration source commit is unexpected")
        hardware = report["hardwareIdentity"]
        if (
            hardware["adapterName"] != args.expected_adapter_name
            or hardware["driverVersion"] != args.expected_driver_version
            or hardware["osBuild"] != args.expected_os_build
        ):
            raise BaselineContractError("calibration hardware identity is unexpected")
        validation_policy = {
            "telemetryCoverage": report["telemetryCoverage"],
            "warmupCompletedFrames": 60,
            "minimumCompletedSamples": 300,
            "thresholds": {"minImportedHistoryEstimatedBytes": 88_473_600},
        }
        performance._validate_sample_series(report, validation_policy)
        summary = performance.summarize(report)
        if summary["graphicsQualificationSpanP95Ms"] > args.graphics_budget_ms:
            raise BaselineContractError(
                f"run {ordinal} exceeds the Graphics budget"
            )
        if summary["renderGraphPassEnvelopeP95Ms"] > args.render_graph_budget_ms:
            raise BaselineContractError(
                f"run {ordinal} exceeds the RenderGraph budget"
            )
        fingerprint, receipt_sha, _ = _validate_receipt(
            receipt_path, report_path, report
        )
        if fingerprint["digest"] != args.expected_source_fingerprint:
            raise BaselineContractError("calibration source fingerprint is unexpected")
        identity = {
            "sourceCommit": report["sourceCommit"],
            "sourceFingerprint": fingerprint,
            "qualificationIdentity": report["qualificationIdentity"],
            "hardwareIdentity": report["hardwareIdentity"],
            "telemetryCoverage": report["telemetryCoverage"],
        }
        if reference is None:
            reference = identity
        elif identity != reference:
            raise BaselineContractError(
                "all runs need identical source, hardware, and qualification"
            )
        runs.append(
            {
                "ordinal": ordinal,
                "runId": report["runId"],
                "reportPath": report_path,
                "reportSha256": sha256_file(report_path),
                "inputReceiptPath": receipt_path,
                "requiredSuiteReceiptSha256": receipt_sha,
                "summary": summary,
                "report": report,
            }
        )
    receipts_dir = output_dir / "receipts"
    receipts_dir.mkdir()
    for run in runs:
        receipt_name = f"{run['ordinal']:02d}.json"
        shutil.copy2(run["inputReceiptPath"], receipts_dir / receipt_name)
        run["requiredSuiteReceiptPath"] = f"receipts/{receipt_name}"
    assert reference is not None
    selected = sorted(
        runs,
        key=lambda item: (
            item["summary"]["graphicsQualificationSpanP95Ms"],
            item["runId"],
        ),
    )[RUN_COUNT // 2]
    baseline_path = output_dir / "performance.json"
    shutil.copy2(selected["reportPath"], baseline_path)

    summary = selected["summary"]
    regression = args.max_regression_percent
    footprints = summary["maxTrackedEstimatedFootprintBytes"]
    policy = {
        "schema": POLICY_SCHEMA,
        "schemaVersion": POLICY_VERSION,
        "qualificationIdentity": reference["qualificationIdentity"],
        "requiredHardwareIdentity": reference["hardwareIdentity"],
        "telemetryCoverage": reference["telemetryCoverage"],
        "warmupCompletedFrames": 60,
        "minimumCompletedSamples": 300,
        "baseline": {
            "id": args.baseline_id,
            "maxRegressionPercent": regression,
        },
        "thresholds": {
            "graphicsQualificationSpanP95Ms": args.graphics_budget_ms,
            "renderGraphPassEnvelopeP95Ms": args.render_graph_budget_ms,
            "cpuP95Ms": {
                name: _scaled(value, regression)
                for name, value in summary["cpuP95Ms"].items()
            },
            "passGpuP95Ms": [
                {
                    **value,
                    "p95Ms": _scaled(value["p95Ms"], regression),
                }
                for value in summary["passGpuP95Ms"]
            ],
            "maxTrackedUploadBytes": _scaled(
                summary["maxTrackedUploadBytes"], regression
            ),
            "maxTransientPoolPhysicalEstimatedBytes": _scaled(
                footprints["transientPoolPhysicalEstimatedBytes"], regression
            ),
            "maxImportedPersistentEstimatedBytes": _scaled(
                footprints["importedPersistentEstimatedBytes"], regression
            ),
            "minImportedHistoryEstimatedBytes": 88_473_600,
            "maxImportedHistoryEstimatedBytes": _scaled(
                footprints["importedHistoryEstimatedBytes"], regression
            ),
            "maxResourceRegistryResidentEstimatedBytes": _scaled(
                footprints["resourceRegistryResidentEstimatedBytes"], regression
            ),
        },
    }
    policy_path = output_dir / "performance-policy.json"
    _write_json(policy_path, policy)
    record = {
        "schema": SCHEMA,
        "schemaVersion": SCHEMA_VERSION,
        "baselineId": args.baseline_id,
        "status": "candidate",
        "source": {
            "commit": reference["sourceCommit"],
            "fingerprint": reference["sourceFingerprint"],
        },
        "qualificationIdentity": reference["qualificationIdentity"],
        "hardwareIdentity": reference["hardwareIdentity"],
        "baseline": {
            "reportPath": baseline_path.name,
            "reportSha256": sha256_file(baseline_path),
            "runId": selected["runId"],
            "preset": selected["report"]["preset"],
            "requiredSuiteReceiptPath": selected["requiredSuiteReceiptPath"],
            "requiredSuiteReceiptSha256": selected[
                "requiredSuiteReceiptSha256"
            ],
        },
        "policy": {
            "path": policy_path.name,
            "sha256": sha256_file(policy_path),
        },
        "calibration": {
            "requiredRunCount": RUN_COUNT,
            "selectionMetric": SELECTION_METRIC,
            "selectionRule": SELECTION_RULE,
            "selectedRunOrdinal": selected["ordinal"],
            "runs": [
                {
                    "ordinal": run["ordinal"],
                    "runId": run["runId"],
                    "reportSha256": run["reportSha256"],
                    "requiredSuiteReceiptPath": run[
                        "requiredSuiteReceiptPath"
                    ],
                    "requiredSuiteReceiptSha256": run[
                        "requiredSuiteReceiptSha256"
                    ],
                    "graphicsQualificationSpanP95Ms": run["summary"][
                        "graphicsQualificationSpanP95Ms"
                    ],
                    "renderGraphPassEnvelopeP95Ms": run["summary"][
                        "renderGraphPassEnvelopeP95Ms"
                    ],
                }
                for run in runs
            ],
        },
        "approval": {
            "approvedBy": None,
            "approvedAtUtc": None,
            "reason": None,
        },
    }
    record_path = output_dir / "baseline-record.json"
    _write_json(record_path, record)
    verify_baseline_record(
        record_path, baseline_path, policy_path, require_approved=False
    )
    print(f"Performance baseline candidate: {record_path}")
    print(
        "Selected median run "
        f"{selected['ordinal']}: "
        f"{selected['summary']['graphicsQualificationSpanP95Ms']:.6f} ms"
    )
    return 0


def _approve(args: argparse.Namespace) -> int:
    candidate_record_path = args.candidate_record.resolve()
    candidate_root = candidate_record_path.parent
    candidate = verify_baseline_record(
        candidate_record_path,
        candidate_root / "performance.json",
        candidate_root / "performance-policy.json",
        require_approved=False,
    )
    if candidate["status"] != "candidate":
        raise BaselineContractError("only a candidate can be approved")
    _assert_approval_source_is_clean(candidate["source"]["commit"])
    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        raise BaselineContractError("approval output must be absent or empty")
    output_dir.mkdir(parents=True, exist_ok=True)
    baseline_path = output_dir / "performance.json"
    policy_path = output_dir / "performance-policy.json"
    shutil.copy2(candidate_root / "performance.json", baseline_path)
    shutil.copy2(candidate_root / "performance-policy.json", policy_path)
    shutil.copytree(candidate_root / "receipts", output_dir / "receipts")
    document = _load_json(candidate_record_path, "candidate baseline record")
    document["status"] = "approved"
    document["baseline"]["reportPath"] = baseline_path.name
    document["policy"]["path"] = policy_path.name
    document["approval"] = {
        "approvedBy": args.approved_by,
        "approvedAtUtc": datetime.now(timezone.utc).isoformat().replace(
            "+00:00", "Z"
        ),
        "reason": args.reason,
    }
    record_path = output_dir / "baseline-record.json"
    _write_json(record_path, document)
    verify_approved_baseline_record(record_path, baseline_path, policy_path)
    print(f"Approved performance baseline: {record_path}")
    return 0


def _verify(args: argparse.Namespace) -> int:
    record = verify_baseline_record(
        args.record,
        args.baseline,
        args.policy,
        require_approved=args.require_approved,
    )
    print(f"Performance baseline valid: {record['baselineId']} ({record['status']})")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser(
        "prepare", help="create an unapproved five-run candidate"
    )
    prepare.add_argument("--run-dir", action="append", required=True, type=Path)
    prepare.add_argument("--output-dir", required=True, type=Path)
    prepare.add_argument("--baseline-id", required=True)
    prepare.add_argument("--expected-source-commit", required=True)
    prepare.add_argument("--expected-source-fingerprint", required=True)
    prepare.add_argument(
        "--expected-preset",
        default="win_x64_gfx24_performance_calibration_release_run",
    )
    prepare.add_argument("--graphics-budget-ms", type=float, default=16.67)
    prepare.add_argument("--render-graph-budget-ms", type=float, default=15.0)
    prepare.add_argument("--max-regression-percent", type=float, default=10.0)
    prepare.add_argument(
        "--expected-adapter-name",
        default="NVIDIA GeForce RTX 4070 Ti",
    )
    prepare.add_argument("--expected-driver-version", default="32.0.16.1062")
    prepare.add_argument("--expected-os-build", default="Windows-10.0.19045")
    prepare.set_defaults(action=_prepare)

    approve = subparsers.add_parser(
        "approve", help="promote a human-reviewed candidate"
    )
    approve.add_argument("--candidate-record", required=True, type=Path)
    approve.add_argument("--output-dir", required=True, type=Path)
    approve.add_argument("--approved-by", required=True)
    approve.add_argument("--reason", required=True)
    approve.set_defaults(action=_approve)

    verify = subparsers.add_parser("verify", help="validate a baseline record")
    verify.add_argument("--record", required=True, type=Path)
    verify.add_argument("--baseline", required=True, type=Path)
    verify.add_argument("--policy", required=True, type=Path)
    verify.add_argument("--require-approved", action="store_true")
    verify.set_defaults(action=_verify)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return args.action(args)
    except (BaselineContractError, OSError, ValueError) as error:
        print(f"Performance baseline rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
