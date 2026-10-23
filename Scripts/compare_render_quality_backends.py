#!/usr/bin/env python3
"""Fail-closed RenderQualityAttachmentSeries V2 cross-backend comparison."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from uuid import uuid4

from check_render_quality_bundle import (
    ContractError,
    REQUIRED_SEMANTICS,
    _metrics,
    _roi_values,
    load_bundle,
    load_sample_contract,
)
from check_render_quality_series import AttachmentSeries, load_series


SCHEMA = "RVX.RenderQualityCrossBackendVisualPolicy"
SCHEMA_VERSION = 1
RESULT_SCHEMA = "RVX.RenderQualityCrossBackendComparisonResult"
RESULT_VERSION = 1
COMMON_IDENTITY = ("sampleId", "quality", "view", "visualContractVersion")
MARKERS = {"candidate", "skip", "skipped", "disabled", "not-run"}


class CrossBackendError(ValueError):
    """Evidence is incomplete, stale, unsafe, or exceeds its policy."""


@dataclass(frozen=True)
class SeriesInput:
    platform: str
    backend: str
    pipeline: str
    path: Path

    @property
    def key(self) -> tuple[str, str, str]:
        return self.platform, self.backend, self.pipeline


def _load(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise CrossBackendError(f"cannot read {label}: {error}") from error
    except json.JSONDecodeError as error:
        raise CrossBackendError(f"{label} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise CrossBackendError(f"{label} root must be an object")
    return value


def _keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    missing, unknown = expected - value.keys(), value.keys() - expected
    if missing:
        raise CrossBackendError(f"{context} is missing fields: {sorted(missing)}")
    if unknown:
        raise CrossBackendError(f"{context} contains unsupported fields: {sorted(unknown)}")


def _str(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise CrossBackendError(f"{context} must be a non-empty string")
    return value


def _int(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise CrossBackendError(f"{context} must be an integer >= {minimum}")
    return value


def _num(value: Any, context: str, minimum: float | None = None) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise CrossBackendError(f"{context} must be a finite number")
    result = float(value)
    if minimum is not None and result < minimum:
        raise CrossBackendError(f"{context} must be >= {minimum}")
    return result


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _target(value: Any, context: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise CrossBackendError(f"{context} must be an object")
    _keys(value, {"platform", "backend"}, context)
    return {"platform": _str(value["platform"], f"{context}.platform"),
            "backend": _str(value["backend"], f"{context}.backend")}


def _policy(path: Path) -> dict[str, Any]:
    value = _load(path, "cross-backend visual policy")
    _keys(value, {
        "schema", "schemaVersion", "policyId", "rationale", "reference",
        "candidates", "qualificationIdentity", "requiredPipelines",
        "requiredCaptureTicks", "attachments",
    }, "cross-backend visual policy")
    if value["schema"] != SCHEMA or value["schemaVersion"] != SCHEMA_VERSION:
        raise CrossBackendError("cross-backend visual policy schema/version is unsupported")
    _str(value["policyId"], "policyId")
    if len(_str(value["rationale"], "rationale")) < 32:
        raise CrossBackendError("policy rationale must explain its bounded thresholds")
    reference = _target(value["reference"], "reference")
    if not isinstance(value["candidates"], list) or len(value["candidates"]) != 2:
        raise CrossBackendError("cross-backend policy requires exactly two candidate backends")
    candidates = [_target(item, f"candidates[{index}]")
                  for index, item in enumerate(value["candidates"])]
    target_keys = [(item["platform"], item["backend"])
                   for item in [reference, *candidates]]
    if len(set(target_keys)) != len(target_keys):
        raise CrossBackendError("cross-backend policy target platform/backend pairs must be unique")
    if (
        reference != {"platform": "windows", "backend": "dx12"}
        or {(item["platform"], item["backend"]) for item in candidates}
        != {("linux", "vulkan"), ("macos", "metal")}
    ):
        raise CrossBackendError(
            "GFX24 cross-backend policy must use Windows/DX12 as reference "
            "and Linux/Vulkan plus macOS/Metal as candidates"
        )

    identity = value["qualificationIdentity"]
    if not isinstance(identity, dict):
        raise CrossBackendError("qualificationIdentity must be an object")
    _keys(identity, set(COMMON_IDENTITY), "qualificationIdentity")
    value["qualificationIdentity"] = {
        field: _int(identity[field], f"qualificationIdentity.{field}", 1)
        if field == "visualContractVersion"
        else _str(identity[field], f"qualificationIdentity.{field}")
        for field in COMMON_IDENTITY
    }

    pipelines = value["requiredPipelines"]
    if (not isinstance(pipelines, list) or len(pipelines) != 2
            or any(not isinstance(item, str) or not item for item in pipelines)
            or set(pipelines) != {"forward-plus", "deferred"}):
        raise CrossBackendError(
            "requiredPipelines must contain forward-plus and deferred exactly once"
        )
    ticks = value["requiredCaptureTicks"]
    if not isinstance(ticks, dict):
        raise CrossBackendError("requiredCaptureTicks must be an object")
    _keys(ticks, {"comparisonTick", "temporalTicks"}, "requiredCaptureTicks")
    comparison = _int(ticks["comparisonTick"], "comparisonTick", 1)
    temporal = ticks["temporalTicks"]
    if (not isinstance(temporal, list) or len(temporal) != 2
            or any(type(item) is not int or item < 1 for item in temporal)
            or temporal[1] != temporal[0] + 1 or comparison >= temporal[0]):
        raise CrossBackendError("temporalTicks must be one adjacent pair after comparisonTick")

    attachments = value["attachments"]
    if not isinstance(attachments, dict) or set(attachments) != set(REQUIRED_SEMANTICS):
        raise CrossBackendError("attachments must declare exactly the seven required semantics")
    roi_fields = {
        "id", "normalizedRegionId", "dataRange", "maxMae", "maxAbs", "minPsnr",
        "minSsim", "temporalMode", "minTemporalMae", "maxTemporalMae",
        "maxTemporalDeltaFieldMae",
    }
    for semantic, attachment in attachments.items():
        if not isinstance(attachment, dict):
            raise CrossBackendError(f"attachments.{semantic} must be an object")
        _keys(attachment, {"rois"}, f"attachments.{semantic}")
        rois = attachment["rois"]
        if not isinstance(rois, list) or not rois:
            raise CrossBackendError(f"attachments.{semantic}.rois must be non-empty")
        seen: set[str] = set()
        for index, roi in enumerate(rois):
            context = f"attachments.{semantic}.rois[{index}]"
            if not isinstance(roi, dict):
                raise CrossBackendError(f"{context} must be an object")
            _keys(roi, roi_fields, context)
            identifier = _str(roi["id"], f"{context}.id")
            if identifier in seen:
                raise CrossBackendError(f"attachments.{semantic} has duplicate ROI id {identifier}")
            seen.add(identifier)
            _str(roi["normalizedRegionId"], f"{context}.normalizedRegionId")
            data_range = _num(roi["dataRange"], f"{context}.dataRange", 0.0)
            max_mae = _num(roi["maxMae"], f"{context}.maxMae", 0.0)
            max_abs = _num(roi["maxAbs"], f"{context}.maxAbs", 0.0)
            min_psnr = _num(roi["minPsnr"], f"{context}.minPsnr", 0.0)
            min_ssim = _num(roi["minSsim"], f"{context}.minSsim", 0.0)
            min_temporal = _num(roi["minTemporalMae"], f"{context}.minTemporalMae", 0.0)
            max_temporal = _num(roi["maxTemporalMae"], f"{context}.maxTemporalMae", 0.0)
            max_delta_field = _num(
                roi["maxTemporalDeltaFieldMae"],
                f"{context}.maxTemporalDeltaFieldMae",
                0.0,
            )
            if (data_range <= 0.0 or min_ssim > 1.0
                    or max_mae > data_range * 0.25
                    or max_abs > data_range * 0.75
                    or not 12.0 <= min_psnr <= 120.0
                    or min_temporal > max_temporal
                    or max_temporal > data_range * 0.25
                    or max_delta_field > data_range * 0.25):
                raise CrossBackendError(f"{context} has an unbounded or inconsistent threshold")
            if roi["temporalMode"] not in {"static", "dynamic"}:
                raise CrossBackendError(f"{context}.temporalMode must be static or dynamic")
            if roi["temporalMode"] == "static" and min_temporal != 0.0:
                raise CrossBackendError(f"{context} static ROI must have minTemporalMae == 0")
            if roi["temporalMode"] == "dynamic" and min_temporal <= 0.0:
                raise CrossBackendError(f"{context} dynamic ROI must reject frozen output")
    value["reference"], value["candidates"] = reference, candidates
    return value


def _parse_series(value: str) -> SeriesInput:
    label, separator, raw_path = value.partition("=")
    parts = label.split("/")
    if not separator or len(parts) != 3 or not raw_path:
        raise CrossBackendError("--series must be platform/backend/pipeline=path")
    platform, backend, pipeline = (_str(item, "--series label") for item in parts)
    return SeriesInput(platform, backend, pipeline, Path(raw_path).resolve())


def _marker(value: Any, context: str) -> str | None:
    if isinstance(value, dict):
        status = value.get("status")
        if isinstance(status, str) and status.casefold() in MARKERS:
            return f"{context}.status={status}"
        for name in ("candidate", "skipped", "disabled", "notRun"):
            if value.get(name) is True:
                return f"{context}.{name}=true"
        for name, child in value.items():
            found = _marker(child, f"{context}.{name}")
            if found:
                return found
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found = _marker(child, f"{context}[{index}]")
            if found:
                return found
    return None


def _safe_path(root: Path, value: Any) -> Path:
    text = _str(value, "series bundlePath")
    if "\\" in text or ":" in text:
        raise CrossBackendError("series bundlePath must use package-relative POSIX separators")
    relative = PurePosixPath(text)
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise CrossBackendError("series bundlePath escapes the series root")
    candidate = (root.resolve() / Path(*relative.parts)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise CrossBackendError("series bundlePath escapes the series root") from error
    return candidate


def _reject_markers(path: Path) -> None:
    document = _load(path, "attachment series")
    found = _marker(document, "series")
    if found:
        raise CrossBackendError(f"qualification evidence contains a forbidden candidate/skip marker: {found}")
    ticks = document.get("ticks")
    if not isinstance(ticks, list):
        return
    for index, tick in enumerate(ticks):
        if not isinstance(tick, dict) or "bundlePath" not in tick:
            continue
        bundle = _load(_safe_path(path.parent, tick["bundlePath"]), f"series bundle {index}")
        found = _marker(bundle, f"bundle[{index}]")
        if found:
            raise CrossBackendError(f"qualification evidence contains a forbidden candidate/skip marker: {found}")


def _bundle_path(series: AttachmentSeries, tick: int) -> Path:
    matches = [item for item in series.ticks if item.capture_tick == tick]
    if len(matches) != 1:
        raise CrossBackendError(f"series does not contain exactly one required capture tick {tick}")
    return (series.path.parent / matches[0].bundle_path).resolve()


def _roi_values_for(
    attachment: Any,
    roi: dict[str, Any],
    normalized: dict[str, tuple[float, float, float, float]],
) -> tuple[tuple[float, ...], dict[str, int]]:
    try:
        x, y, width, height = normalized[str(roi["normalizedRegionId"])]
    except KeyError as error:
        raise CrossBackendError(f"ROI {roi['id']} references an unknown sample-manifest region") from error
    left, top = math.floor(x * attachment.width), math.floor(y * attachment.height)
    right = math.ceil((x + width) * attachment.width)
    bottom = math.ceil((y + height) * attachment.height)
    if left < 0 or top < 0 or right > attachment.width or bottom > attachment.height or right <= left or bottom <= top:
        raise CrossBackendError(f"ROI {roi['id']} maps outside {attachment.semantic}")
    pixels = {"x": left, "y": top, "width": right - left, "height": bottom - top}
    return _roi_values(attachment, pixels, str(roi["id"])), pixels


def _temporal_mae(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    if len(left) != len(right) or not left:
        raise CrossBackendError("temporal ROI sample counts do not match")
    value = sum(abs(before - after) for before, after in zip(left, right, strict=True)) / len(left)
    if not math.isfinite(value):
        raise CrossBackendError("temporal ROI metric is not finite")
    return value


def _signed_delta_field(
    before: tuple[float, ...],
    after: tuple[float, ...],
) -> tuple[float, ...]:
    if len(before) != len(after) or not before:
        raise CrossBackendError("temporal ROI sample counts do not match")
    result = tuple(
        newer - older for older, newer in zip(before, after, strict=True)
    )
    if not all(math.isfinite(value) for value in result):
        raise CrossBackendError("temporal ROI delta field is not finite")
    return result


def _temporal_ok(roi: dict[str, Any], value: float) -> bool:
    return value <= float(roi["maxTemporalMae"]) and (
        roi["temporalMode"] != "dynamic" or value >= float(roi["minTemporalMae"])
    )


def _compare_pair(
    reference: AttachmentSeries,
    candidate: AttachmentSeries,
    policy: dict[str, Any],
    normalized: dict[str, tuple[float, float, float, float]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[str], list[str]]:
    for field in (
        "renderPath",
        "instancingMode",
        "requestedRecipe",
        "resolvedRecipe",
        "profileHash",
    ):
        if (
            reference.render_configuration_identity[field]
            != candidate.render_configuration_identity[field]
        ):
            raise CrossBackendError(
                "candidate runtime configuration differs from the DX12 "
                f"reference: {field}"
            )
    ticks = policy["requiredCaptureTicks"]
    comparison_tick = int(ticks["comparisonTick"])
    older_tick, newer_tick = (int(item) for item in ticks["temporalTicks"])
    reference_bundle = load_bundle(_bundle_path(reference, comparison_tick))
    candidate_bundle = load_bundle(_bundle_path(candidate, comparison_tick))
    visual: list[dict[str, Any]] = []
    temporal: list[dict[str, Any]] = []
    visual_failures: list[str] = []
    temporal_failures: list[str] = []
    for semantic in REQUIRED_SEMANTICS:
        expected, actual = reference_bundle.attachments[semantic], candidate_bundle.attachments[semantic]
        if (expected.width, expected.height, expected.channels, expected.format, expected.color_domain) != (
            actual.width, actual.height, actual.channels, actual.format, actual.color_domain,
        ):
            raise CrossBackendError(f"{semantic} metadata does not match the DX12 reference")
        for roi in policy["attachments"][semantic]["rois"]:
            expected_values, expected_roi = _roi_values_for(expected, roi, normalized)
            actual_values, actual_roi = _roi_values_for(actual, roi, normalized)
            if expected_roi != actual_roi:
                raise CrossBackendError(f"{semantic}/{roi['id']} ROI mapping differs from DX12")
            metric = _metrics(expected_values, actual_values, float(roi["dataRange"]))
            passed = (
                float(metric["mae"]) <= float(roi["maxMae"])
                and float(metric["maxAbs"]) <= float(roi["maxAbs"])
                and (bool(metric["psnrIsInfinite"]) or float(metric["psnr"]) >= float(roi["minPsnr"]))
                and float(metric["ssim"]) >= float(roi["minSsim"])
            )
            visual.append({
                "semantic": semantic, "roi": roi["id"],
                "normalizedRegionId": roi["normalizedRegionId"], "pixelROI": actual_roi,
                "passed": passed, **metric,
            })
            if not passed:
                visual_failures.append(f"visual metric threshold failed for {semantic}/{roi['id']}")

    reference_older, reference_newer = (
        load_bundle(_bundle_path(reference, tick)) for tick in (older_tick, newer_tick)
    )
    candidate_older, candidate_newer = (
        load_bundle(_bundle_path(candidate, tick)) for tick in (older_tick, newer_tick)
    )
    for semantic in REQUIRED_SEMANTICS:
        for roi in policy["attachments"][semantic]["rois"]:
            ref_before, _ = _roi_values_for(reference_older.attachments[semantic], roi, normalized)
            ref_after, _ = _roi_values_for(reference_newer.attachments[semantic], roi, normalized)
            candidate_before, _ = _roi_values_for(candidate_older.attachments[semantic], roi, normalized)
            candidate_after, _ = _roi_values_for(candidate_newer.attachments[semantic], roi, normalized)
            reference_field = _signed_delta_field(ref_before, ref_after)
            candidate_field = _signed_delta_field(candidate_before, candidate_after)
            reference_delta = _temporal_mae(ref_before, ref_after)
            candidate_delta = _temporal_mae(candidate_before, candidate_after)
            delta_field_mae = _temporal_mae(reference_field, candidate_field)
            reference_ok = _temporal_ok(roi, reference_delta)
            candidate_ok = _temporal_ok(roi, candidate_delta)
            passed = (
                reference_ok
                and candidate_ok
                and delta_field_mae <= float(roi["maxTemporalDeltaFieldMae"])
            )
            temporal.append({
                "semantic": semantic, "roi": roi["id"],
                "normalizedRegionId": roi["normalizedRegionId"], "temporalMode": roi["temporalMode"],
                "referenceTemporalMae": reference_delta, "candidateTemporalMae": candidate_delta,
                "temporalDeltaFieldMae": delta_field_mae,
                "minTemporalMae": float(roi["minTemporalMae"]),
                "maxTemporalMae": float(roi["maxTemporalMae"]),
                "maxTemporalDeltaFieldMae": float(roi["maxTemporalDeltaFieldMae"]),
                "referenceWithinPolicy": reference_ok, "candidateWithinPolicy": candidate_ok,
                "passed": passed,
            })
            if not passed:
                temporal_failures.append(
                    "temporal signed delta-field threshold failed for "
                    f"{semantic}/{roi['id']}"
                )
    return visual, temporal, visual_failures, temporal_failures


def _write(path: Path, value: dict[str, Any]) -> None:
    target = path.resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.parent / f".{target.name}.{uuid4().hex}.tmp"
    try:
        temporary.write_bytes((json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8"))
        temporary.replace(target)
    finally:
        if temporary.exists():
            temporary.unlink()


def _result(args: argparse.Namespace) -> dict[str, Any]:
    policy, manifest = args.policy.resolve(), args.sample_manifest.resolve()
    return {
        "schema": RESULT_SCHEMA, "schemaVersion": RESULT_VERSION, "status": "failed",
        "comparisonPerformed": False,
        "policy": {"path": str(policy), "sha256": _hash(policy) if policy.is_file() else None},
        "sampleManifest": {"path": str(manifest), "sha256": _hash(manifest) if manifest.is_file() else None},
        "expectedSourceCommit": args.expected_source_commit, "requiredInputs": [],
        "inputSeries": [], "comparisons": [], "failureCategory": "policy-validation",
        "diagnostic": "comparison did not run",
    }


def _validate_inputs(
    inputs: list[SeriesInput], policy: dict[str, Any], manifest: Path,
    expected_commit: str, result: dict[str, Any],
) -> dict[tuple[str, str, str], AttachmentSeries]:
    targets = [policy["reference"], *policy["candidates"]]
    expected = {(target["platform"], target["backend"], pipeline)
                for target in targets for pipeline in policy["requiredPipelines"]}
    supplied = [item.key for item in inputs]
    if len(inputs) != len(expected) or len(set(supplied)) != len(supplied) or set(supplied) != expected:
        raise CrossBackendError("input matrix must contain exactly one series for every policy platform/backend/pipeline")
    result["requiredInputs"] = [
        {"platform": platform, "backend": backend, "pipeline": pipeline}
        for platform, backend, pipeline in sorted(expected)
    ]
    _, manifest_ticks = load_sample_contract(manifest, str(policy["qualificationIdentity"]["sampleId"]))
    expected_ticks = (int(policy["requiredCaptureTicks"]["comparisonTick"]),
                      *(int(value) for value in policy["requiredCaptureTicks"]["temporalTicks"]))
    if tuple(manifest_ticks) != expected_ticks:
        raise CrossBackendError("sample manifest capture ticks do not match cross-backend policy")

    output: dict[tuple[str, str, str], AttachmentSeries] = {}
    paths: set[Path] = set()
    series_hashes: set[str] = set()
    receipts: set[str] = set()
    bundle_paths: set[Path] = set()
    target_facts: dict[tuple[str, str], tuple[str, str, dict[str, str], str]] = {}
    common = policy["qualificationIdentity"]
    for item in inputs:
        if item.path in paths:
            raise CrossBackendError("input series paths must be unique")
        _reject_markers(item.path)
        try:
            series = load_series(item.path, manifest)
        except ContractError as error:
            raise CrossBackendError(f"{item.platform}/{item.backend}/{item.pipeline}: {error}") from error
        if series.source_commit != expected_commit:
            raise CrossBackendError("all input series must bind the requested source commit")
        if series.qualification_identity["backend"] != item.backend:
            raise CrossBackendError("series backend does not match its input label")
        if series.qualification_identity["pipeline"] != item.pipeline:
            raise CrossBackendError("series pipeline does not match its input label")
        for field in COMMON_IDENTITY:
            if series.qualification_identity[field] != common[field]:
                raise CrossBackendError(f"series qualification identity differs from policy: {field}")
        if tuple(tick.capture_tick for tick in series.ticks) != expected_ticks:
            raise CrossBackendError("series capture ticks do not match cross-backend policy")
        if series.series_sha256 in series_hashes:
            raise CrossBackendError("input series SHA-256 values must be globally unique")
        target_key = item.platform, item.backend
        facts = (
            series.run_id,
            series.preset,
            series.hardware_identity,
            series.manifest_sha256,
        )
        existing_facts = target_facts.get(target_key)
        if existing_facts is not None and existing_facts != facts:
            raise CrossBackendError(
                "both pipelines for one platform/backend must share one "
                "required-suite runId, preset, hardware identity, and manifest"
            )
        target_facts[target_key] = facts
        for tick in series.ticks:
            bundle_path = (series.path.parent / tick.bundle_path).resolve()
            if bundle_path in bundle_paths:
                raise CrossBackendError("input series reuse a bundle path")
            if tick.receipt_sha256 in receipts:
                raise CrossBackendError("input series reuse a render execution receipt")
            bundle_paths.add(bundle_path)
            receipts.add(tick.receipt_sha256)
        result["inputSeries"].append({
            "platform": item.platform, "backend": item.backend, "pipeline": item.pipeline,
            "path": str(item.path), "seriesSha256": series.series_sha256,
            "sourceCommit": series.source_commit, "runId": series.run_id, "preset": series.preset,
            "qualificationIdentity": series.qualification_identity, "hardwareIdentity": series.hardware_identity,
            "renderConfigurationIdentity": series.render_configuration_identity,
        })
        output[item.key] = series
        paths.add(item.path)
        series_hashes.add(series.series_sha256)
    return output


def _run(args: argparse.Namespace) -> int:
    result = _result(args)
    category = "policy-validation"
    try:
        policy = _policy(args.policy.resolve())
        result["policy"]["policyId"] = policy["policyId"]
        category = "input-matrix"
        inputs = [_parse_series(value) for value in args.series]
        category = "series-validation"
        by_key = _validate_inputs(inputs, policy, args.sample_manifest.resolve(), args.expected_source_commit, result)
        normalized, _ = load_sample_contract(args.sample_manifest.resolve(), str(policy["qualificationIdentity"]["sampleId"]))
        reference = policy["reference"]
        all_visual: list[str] = []
        all_temporal: list[str] = []
        category = "comparison-contract"
        for pipeline in policy["requiredPipelines"]:
            ref_key = (reference["platform"], reference["backend"], pipeline)
            for target in policy["candidates"]:
                candidate_key = (target["platform"], target["backend"], pipeline)
                visual, temporal, visual_failures, temporal_failures = _compare_pair(
                    by_key[ref_key], by_key[candidate_key], policy, normalized)
                result["comparisonPerformed"] = True
                result["comparisons"].append({
                    "pipeline": pipeline,
                    "reference": {"platform": reference["platform"], "backend": reference["backend"], "seriesSha256": by_key[ref_key].series_sha256},
                    "candidate": {"platform": target["platform"], "backend": target["backend"], "seriesSha256": by_key[candidate_key].series_sha256},
                    "comparisonTick": policy["requiredCaptureTicks"]["comparisonTick"],
                    "temporalTicks": policy["requiredCaptureTicks"]["temporalTicks"],
                    "visualMetrics": visual, "temporalMetrics": temporal,
                    "passed": not visual_failures and not temporal_failures,
                })
                all_visual.extend(f"{pipeline}/{target['backend']}: {failure}" for failure in visual_failures)
                all_temporal.extend(f"{pipeline}/{target['backend']}: {failure}" for failure in temporal_failures)
        if all_visual or all_temporal:
            category = "visual-and-temporal-metric" if all_visual and all_temporal else "visual-metric" if all_visual else "temporal-metric"
            raise CrossBackendError("; ".join([*all_visual, *all_temporal]))
        result["status"] = "passed"
        result["failureCategory"] = None
        result["diagnostic"] = "cross-backend attachment comparison passed"
        _write(args.report, result)
        print("Render quality cross-backend comparison passed")
        return 0
    except (CrossBackendError, ContractError, OSError, ValueError) as error:
        result["status"] = "failed"
        result["failureCategory"] = category
        result["diagnostic"] = str(error)
        _write(args.report, result)
        print(f"Render quality cross-backend comparison rejected: {error}", file=sys.stderr)
        return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--series", action="append", required=True, help="repeat platform/backend/pipeline=path")
    parser.add_argument("--sample-manifest", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--expected-source-commit", required=True)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    return _run(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
