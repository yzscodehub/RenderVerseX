#!/usr/bin/env python3
"""Prepare, approve, verify, and preview hash-bound RenderQuality Goldens."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any
from uuid import uuid4

from check_render_quality_bundle import (
    ContractError,
    REQUIRED_SEMANTICS,
    _comparison,
    _temporal,
    load_bundle,
    load_policy,
    load_sample_contract,
)
from check_render_quality_series import (
    CONFIGURATION_FIELDS,
    HARDWARE_FIELDS,
    IDENTITY_FIELDS,
    AttachmentSeries,
    load_series,
)
from render_quality_performance_baseline import _assert_approval_source_is_clean

SCHEMA = "RVX.RenderQualityGoldenRecord"
VERSION = 2
SHEET_SCHEMA = "RVX.RenderQualityContactSheet"
RUN_COUNT = 5
PANEL_W, PANEL_H, COLUMNS, ROWS = 320, 180, 4, 2
SHA256 = re.compile(r"^[0-9a-f]{64}$")


class GoldenError(ValueError):
    """Golden evidence is incomplete, stale, or unsafe to promote."""


def _load(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise GoldenError(f"cannot read {label}: {error}") from error
    except json.JSONDecodeError as error:
        raise GoldenError(f"{label} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise GoldenError(f"{label} root must be an object")
    return value


def _write(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(
        value, indent=2, sort_keys=True, allow_nan=False
    ) + "\n"
    path.write_bytes(payload.encode("utf-8"))


def _copy_json_lf(source: Path, target: Path, label: str) -> None:
    _load(source, label)
    text = source.read_text(encoding="utf-8")
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(normalized.encode("utf-8"))


def _keys(value: dict[str, Any], required: set[str], label: str) -> None:
    missing, unknown = required - value.keys(), value.keys() - required
    if missing:
        raise GoldenError(f"{label} is missing fields: {sorted(missing)}")
    if unknown:
        raise GoldenError(f"{label} contains unsupported fields: {sorted(unknown)}")


def _str(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise GoldenError(f"{label} must be a non-empty string")
    return value


def _int(value: Any, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise GoldenError(f"{label} must be an integer >= {minimum}")
    return value


def _sha(value: Any, label: str) -> str:
    value = _str(value, label)
    if not SHA256.fullmatch(value):
        raise GoldenError(f"{label} must be a lowercase SHA-256 digest")
    return value


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _relative(root: Path, value: Any, label: str) -> Path:
    text = _str(value, label)
    if "\\" in text or ":" in text:
        raise GoldenError(f"{label} must use record-relative POSIX paths")
    relative = PurePosixPath(text)
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise GoldenError(f"{label} escapes the Golden record directory")
    root = root.resolve()
    result = (root / Path(*relative.parts)).resolve()
    if result != root and root not in result.parents:
        raise GoldenError(f"{label} escapes the Golden record directory")
    return result


def _reltext(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as error:
        raise GoldenError("Golden output must remain within its record directory") from error


def _identity(value: Any, label: str) -> dict[str, str | int]:
    if not isinstance(value, dict):
        raise GoldenError(f"{label} must be an object")
    _keys(value, set(IDENTITY_FIELDS), label)
    return {
        name: _int(value[name], f"{label}.{name}", 1)
        if name == "visualContractVersion" else _str(value[name], f"{label}.{name}")
        for name in IDENTITY_FIELDS
    }


def _hardware(value: Any, label: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise GoldenError(f"{label} must be an object")
    _keys(value, set(HARDWARE_FIELDS), label)
    result = {name: _str(value[name], f"{label}.{name}") for name in HARDWARE_FIELDS}
    result["capabilityHash"] = _sha(result["capabilityHash"], f"{label}.capabilityHash")
    return result


def _configuration(value: Any, label: str) -> dict[str, str | int]:
    if not isinstance(value, dict):
        raise GoldenError(f"{label} must be an object")
    _keys(value, set(CONFIGURATION_FIELDS), label)
    result: dict[str, str | int] = {
        name: _str(value[name], f"{label}.{name}")
        for name in CONFIGURATION_FIELDS
        if name not in {"profileHash", "renderConfigurationHash"}
    }
    result["profileHash"] = _int(
        value["profileHash"], f"{label}.profileHash", 1
    )
    if result["profileHash"] > (2**64 - 1):
        raise GoldenError(f"{label}.profileHash exceeds uint64")
    result["renderConfigurationHash"] = _sha(
        value["renderConfigurationHash"],
        f"{label}.renderConfigurationHash",
    )
    return result


def _policy(path: Path) -> tuple[dict[str, Any], dict[str, str | int]]:
    try:
        value = load_policy(path)
    except ContractError as error:
        raise GoldenError(str(error)) from error
    return value, _identity(value["qualificationIdentity"], "VisualPolicy qualificationIdentity")


def _tick(series: AttachmentSeries, capture_tick: int) -> tuple[Path, Any]:
    matches = [tick for tick in series.ticks if tick.capture_tick == capture_tick]
    if len(matches) != 1:
        raise GoldenError(f"series does not contain exactly one capture tick {capture_tick}")
    return (series.path.parent / matches[0].bundle_path).resolve(), matches[0]


def _byte(value: float) -> int:
    return min(255, max(0, int(math.floor(min(1.0, max(0.0, value)) * 255.0 + 0.5))))


def _rgb(semantic: str, values: tuple[float, ...], offset: int) -> tuple[int, int, int]:
    if semantic == "BeautyLDR":
        return tuple(_byte(values[offset + index]) for index in range(3))
    if semantic == "PreToneHDR":
        return tuple(
            _byte(max(0.0, values[offset + index]) / (1.0 + max(0.0, values[offset + index])))
            for index in range(3)
        )
    if semantic == "LinearDepth":
        value = _byte(1.0 / (1.0 + max(0.0, values[offset])))
        return value, value, value
    if semantic == "Normal":
        return tuple(_byte(values[offset + index] * 0.5 + 0.5) for index in range(3))
    if semantic == "Velocity":
        return _byte(abs(values[offset])), _byte(abs(values[offset + 1])), 0
    if semantic in {"AmbientOcclusion", "ReactiveMask"}:
        value = _byte(values[offset])
        return value, value, value
    raise GoldenError(f"contact sheet cannot render unknown semantic {semantic}")


def _contact_sheet(bundle_path: Path, series_sha: str, ppm: Path, index: Path) -> None:
    try:
        bundle = load_bundle(bundle_path)
    except ContractError as error:
        raise GoldenError(str(error)) from error
    width, height = PANEL_W * COLUMNS, PANEL_H * ROWS
    output = bytearray(width * height * 3)
    mapping = {
        "BeautyLDR": "rgb-clamp", "PreToneHDR": "reinhard",
        "LinearDepth": "inverse-depth-grayscale", "Normal": "normal-remap",
        "Velocity": "absolute-rg", "AmbientOcclusion": "grayscale",
        "ReactiveMask": "grayscale",
    }
    panels: list[dict[str, Any]] = []
    for panel, semantic in enumerate(REQUIRED_SEMANTICS):
        attachment = bundle.attachments[semantic]
        left, top = panel % COLUMNS * PANEL_W, panel // COLUMNS * PANEL_H
        for y in range(PANEL_H):
            source_y = min(attachment.height - 1, y * attachment.height // PANEL_H)
            for x in range(PANEL_W):
                source_x = min(attachment.width - 1, x * attachment.width // PANEL_W)
                source = (source_y * attachment.width + source_x) * attachment.channels
                destination = ((top + y) * width + left + x) * 3
                output[destination:destination + 3] = bytes(_rgb(semantic, attachment.values, source))
        panels.append({
            "semantic": semantic, "x": left, "y": top,
            "width": PANEL_W, "height": PANEL_H,
            "sourceWidth": attachment.width, "sourceHeight": attachment.height,
            "mapping": mapping[semantic],
        })
    ppm.parent.mkdir(parents=True, exist_ok=True)
    ppm.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + output)
    execution = bundle.execution
    _write(index, {
        "schema": SHEET_SCHEMA, "schemaVersion": VERSION,
        "sourceCommit": bundle.source_commit, "runId": execution["runId"],
        "preset": execution["preset"], "qualificationIdentity": bundle.qualification_identity,
        "renderConfigurationIdentity": bundle.render_configuration_identity,
        "captureTick": bundle.qualification_identity["captureTick"],
        "bundleId": bundle.bundle_id, "bundleSha256": _hash(bundle_path),
        "seriesSha256": series_sha,
        "layout": {
            "columns": COLUMNS, "rows": ROWS, "panelWidth": PANEL_W,
            "panelHeight": PANEL_H, "width": width, "height": height,
        },
        "panels": panels,
    })


def _check_sheet(path: Path, selected: dict[str, Any], record: dict[str, Any]) -> None:
    document = _load(path, "contact sheet index")
    _keys(document, {
        "schema", "schemaVersion", "sourceCommit", "runId", "preset",
        "qualificationIdentity", "renderConfigurationIdentity", "captureTick", "bundleId", "bundleSha256",
        "seriesSha256", "layout", "panels",
    }, "contact sheet index")
    if document["schema"] != SHEET_SCHEMA or document["schemaVersion"] != VERSION:
        raise GoldenError("contact sheet index schema/version is unsupported")
    if (
        _str(document["sourceCommit"], "contact sourceCommit") != record["sourceCommit"]
        or _str(document["runId"], "contact runId") != selected["runId"]
        or _str(document["preset"], "contact preset") != selected["preset"]
        or _identity(
            {
                key: value for key, value in document["qualificationIdentity"].items()
                if key != "captureTick"
            }
            if isinstance(document["qualificationIdentity"], dict)
            else document["qualificationIdentity"],
            "contact identity",
        ) != record["qualificationIdentity"]
        or _configuration(
            document["renderConfigurationIdentity"],
            "contact renderConfigurationIdentity",
        ) != record["renderConfigurationIdentity"]
        or _int(document["captureTick"], "contact captureTick", 1) != selected["captureTick"]
        or _str(document["bundleId"], "contact bundleId") != selected["bundleId"]
        or _sha(document["bundleSha256"], "contact bundleSha256") != selected["bundleSha256"]
        or _sha(document["seriesSha256"], "contact seriesSha256") != selected["seriesSha256"]
    ):
        raise GoldenError("contact sheet index does not bind selected Golden")
    expected = {
        "columns": COLUMNS, "rows": ROWS, "panelWidth": PANEL_W,
        "panelHeight": PANEL_H, "width": PANEL_W * COLUMNS, "height": PANEL_H * ROWS,
    }
    if document["layout"] != expected:
        raise GoldenError("contact sheet layout is not canonical")
    panels = document["panels"]
    if not isinstance(panels, list) or len(panels) != len(REQUIRED_SEMANTICS):
        raise GoldenError("contact sheet must have exactly seven panels")
    for ordinal, panel in enumerate(panels):
        label = f"contact sheet panels[{ordinal}]"
        if not isinstance(panel, dict):
            raise GoldenError(f"{label} must be an object")
        _keys(panel, {
            "semantic", "x", "y", "width", "height",
            "sourceWidth", "sourceHeight", "mapping",
        }, label)
        if (
            _str(panel["semantic"], f"{label}.semantic") != REQUIRED_SEMANTICS[ordinal]
            or _int(panel["x"], f"{label}.x") != ordinal % COLUMNS * PANEL_W
            or _int(panel["y"], f"{label}.y") != ordinal // COLUMNS * PANEL_H
            or _int(panel["width"], f"{label}.width", 1) != PANEL_W
            or _int(panel["height"], f"{label}.height", 1) != PANEL_H
            or _int(panel["sourceWidth"], f"{label}.sourceWidth", 1) > 8192
            or _int(panel["sourceHeight"], f"{label}.sourceHeight", 1) > 8192
            or _str(panel["mapping"], f"{label}.mapping") != {
                "BeautyLDR": "rgb-clamp", "PreToneHDR": "reinhard",
                "LinearDepth": "inverse-depth-grayscale", "Normal": "normal-remap",
                "Velocity": "absolute-rg", "AmbientOcclusion": "grayscale",
                "ReactiveMask": "grayscale",
            }[REQUIRED_SEMANTICS[ordinal]]
        ):
            raise GoldenError("contact sheet panel layout is invalid")


def _check_ppm(path: Path) -> None:
    try:
        with path.open("rb") as stream:
            header = stream.readline() + stream.readline() + stream.readline()
            payload = stream.read()
    except OSError as error:
        raise GoldenError(f"cannot read contact sheet PPM: {error}") from error
    expected = f"P6\n{PANEL_W * COLUMNS} {PANEL_H * ROWS}\n255\n".encode("ascii")
    if header != expected or len(payload) != PANEL_W * COLUMNS * PANEL_H * ROWS * 3:
        raise GoldenError("contact sheet PPM is not canonical P6 1280x360")


def _approval(value: Any, status: str) -> dict[str, str | None]:
    if not isinstance(value, dict):
        raise GoldenError("approval must be an object")
    _keys(value, {"approvedBy", "approvedAtUtc", "reason"}, "approval")
    result = {name: None if value[name] is None else _str(value[name], f"approval.{name}")
              for name in ("approvedBy", "approvedAtUtc", "reason")}
    if status == "candidate":
        if any(result.values()):
            raise GoldenError("candidate Golden cannot contain approval data")
    else:
        if not all(result.values()):
            raise GoldenError("approved Golden has incomplete approval data")
        try:
            timestamp = datetime.fromisoformat(str(result["approvedAtUtc"]).replace("Z", "+00:00"))
        except ValueError as error:
            raise GoldenError("approval timestamp is invalid") from error
        if timestamp.tzinfo is None:
            raise GoldenError("approval timestamp needs a timezone")
    return result


def _record(path: Path) -> dict[str, Any]:
    document = _load(path, "Golden record")
    _keys(document, {
        "schema", "schemaVersion", "goldenId", "status", "sourceCommit",
        "qualificationIdentity", "renderConfigurationIdentity", "sampleManifest", "hardwareMode",
        "runnerClass", "selectedBundle", "visualPolicy", "contactSheet",
        "seriesAudit", "approval",
    }, "Golden record")
    if document["schema"] != SCHEMA or document["schemaVersion"] != VERSION:
        raise GoldenError("Golden record schema/version is unsupported")
    status = document["status"]
    if status not in {"candidate", "approved"}:
        raise GoldenError("Golden record status is unsupported")
    mode, runner = document["hardwareMode"], document["runnerClass"]
    if mode not in {"exact-hardware", "runner-class"}:
        raise GoldenError("hardwareMode is unsupported")
    if (mode == "exact-hardware" and runner is not None) or (mode == "runner-class" and (not isinstance(runner, str) or not runner)):
        raise GoldenError("hardwareMode/runnerClass combination is invalid")
    selected = document["selectedBundle"]
    if not isinstance(selected, dict):
        raise GoldenError("selectedBundle must be an object")
    _keys(selected, {
        "seriesOrdinal", "seriesSha256", "runId", "preset", "captureTick",
        "bundleId", "bundlePath", "bundleSha256", "receiptSha256",
    }, "selectedBundle")
    selected = {
        "seriesOrdinal": _int(selected["seriesOrdinal"], "selectedBundle.seriesOrdinal", 1),
        "seriesSha256": _sha(selected["seriesSha256"], "selectedBundle.seriesSha256"),
        "runId": _str(selected["runId"], "selectedBundle.runId"),
        "preset": _str(selected["preset"], "selectedBundle.preset"),
        "captureTick": _int(selected["captureTick"], "selectedBundle.captureTick", 1),
        "bundleId": _str(selected["bundleId"], "selectedBundle.bundleId"),
        "bundlePath": _str(selected["bundlePath"], "selectedBundle.bundlePath"),
        "bundleSha256": _sha(selected["bundleSha256"], "selectedBundle.bundleSha256"),
        "receiptSha256": _sha(selected["receiptSha256"], "selectedBundle.receiptSha256"),
    }
    manifest = document["sampleManifest"]
    policy, contact, audit = document["visualPolicy"], document["contactSheet"], document["seriesAudit"]
    if (
        not isinstance(manifest, dict)
        or not isinstance(policy, dict)
        or not isinstance(contact, dict)
        or not isinstance(audit, dict)
    ):
        raise GoldenError(
            "sampleManifest, visualPolicy, contactSheet, and seriesAudit must be objects"
        )
    _keys(manifest, {"path", "sha256"}, "sampleManifest")
    _keys(policy, {"path", "sha256"}, "visualPolicy")
    _keys(contact, {"ppmPath", "ppmSha256", "indexPath", "indexSha256"}, "contactSheet")
    _keys(audit, {"requiredSeriesCount", "selectedSeriesOrdinal", "runs"}, "seriesAudit")
    if audit["requiredSeriesCount"] != RUN_COUNT:
        raise GoldenError("seriesAudit must contain exactly five series")
    chosen = _int(audit["selectedSeriesOrdinal"], "seriesAudit.selectedSeriesOrdinal", 1)
    values = audit["runs"]
    if not isinstance(values, list) or len(values) != RUN_COUNT:
        raise GoldenError("seriesAudit.runs must contain exactly five entries")
    runs: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    seen_hashes: set[str] = set()
    exact: dict[str, str] | None = None
    qualification = _identity(document["qualificationIdentity"], "qualificationIdentity")
    configuration = _configuration(
        document["renderConfigurationIdentity"],
        "renderConfigurationIdentity",
    )
    for index, value in enumerate(values):
        label = f"seriesAudit.runs[{index}]"
        if not isinstance(value, dict):
            raise GoldenError(f"{label} must be an object")
        _keys(value, {"ordinal", "runId", "seriesSha256", "hardwareIdentity"}, label)
        ordinal = _int(value["ordinal"], f"{label}.ordinal", 1)
        run_id = _str(value["runId"], f"{label}.runId")
        digest = _sha(value["seriesSha256"], f"{label}.seriesSha256")
        hardware = _hardware(value["hardwareIdentity"], f"{label}.hardwareIdentity")
        if ordinal != index + 1 or run_id in seen_ids or digest in seen_hashes:
            raise GoldenError("series audit ordinals, runIds, and hashes must be unique")
        if hardware["backend"] != qualification["backend"]:
            raise GoldenError("series audit hardware backend mismatches qualification")
        if mode == "exact-hardware":
            exact = hardware if exact is None else exact
            if exact != hardware:
                raise GoldenError("exact-hardware Golden contains different identities")
        runs.append({
            "ordinal": ordinal, "runId": run_id, "seriesSha256": digest,
            "hardwareIdentity": hardware,
        })
        seen_ids.add(run_id)
        seen_hashes.add(digest)
    if chosen not in range(1, RUN_COUNT + 1) or selected["seriesOrdinal"] != chosen:
        raise GoldenError("selected Golden series ordinal is invalid")
    selected_run = runs[chosen - 1]
    if selected_run["runId"] != selected["runId"] or selected_run["seriesSha256"] != selected["seriesSha256"]:
        raise GoldenError("selected bundle does not bind the selected series audit")
    return {
        "goldenId": _str(document["goldenId"], "goldenId"), "status": status,
        "sourceCommit": _str(document["sourceCommit"], "sourceCommit"),
        "qualificationIdentity": qualification,
        "renderConfigurationIdentity": configuration,
        "sampleManifest": {
            "path": _str(manifest["path"], "sampleManifest.path"),
            "sha256": _sha(manifest["sha256"], "sampleManifest.sha256"),
        },
        "hardwareMode": mode, "runnerClass": runner, "selectedBundle": selected,
        "visualPolicy": {
            "path": _str(policy["path"], "visualPolicy.path"),
            "sha256": _sha(policy["sha256"], "visualPolicy.sha256"),
        },
        "contactSheet": {
            "ppmPath": _str(contact["ppmPath"], "contactSheet.ppmPath"),
            "ppmSha256": _sha(contact["ppmSha256"], "contactSheet.ppmSha256"),
            "indexPath": _str(contact["indexPath"], "contactSheet.indexPath"),
            "indexSha256": _sha(contact["indexSha256"], "contactSheet.indexSha256"),
        },
        "seriesAudit": {
            "requiredSeriesCount": RUN_COUNT, "selectedSeriesOrdinal": chosen,
            "runs": runs,
        },
        "approval": _approval(document["approval"], status),
    }


def verify_golden_record(
    record_path: Path,
    bundle_path: Path,
    policy_path: Path,
    authoritative_manifest_path: Path,
    *,
    require_approved: bool,
) -> dict[str, Any]:
    record_path = record_path.resolve()
    bundle_path = bundle_path.resolve()
    policy_path = policy_path.resolve()
    authoritative_manifest_path = authoritative_manifest_path.resolve()
    root, record = record_path.parent, _record(record_path)
    selected = record["selectedBundle"]
    if _relative(root, selected["bundlePath"], "selectedBundle.bundlePath") != bundle_path:
        raise GoldenError("selected bundle path does not match Golden record")
    if _relative(root, record["visualPolicy"]["path"], "visualPolicy.path") != policy_path:
        raise GoldenError("VisualPolicy path does not match Golden record")
    packaged_manifest_path = _relative(
        root,
        record["sampleManifest"]["path"],
        "sampleManifest.path",
    )
    ppm = _relative(root, record["contactSheet"]["ppmPath"], "contactSheet.ppmPath")
    index = _relative(root, record["contactSheet"]["indexPath"], "contactSheet.indexPath")
    for path, digest, label in (
        (bundle_path, selected["bundleSha256"], "selected bundle"),
        (policy_path, record["visualPolicy"]["sha256"], "VisualPolicy"),
        (
            packaged_manifest_path,
            record["sampleManifest"]["sha256"],
            "Packaged sample manifest",
        ),
        (ppm, record["contactSheet"]["ppmSha256"], "contact sheet"),
        (index, record["contactSheet"]["indexSha256"], "contact sheet index"),
    ):
        if not path.is_file() or _hash(path) != digest:
            raise GoldenError(f"{label} is missing or its SHA-256 mismatches")
    if (
        not authoritative_manifest_path.is_file()
        or _hash(authoritative_manifest_path)
        != record["sampleManifest"]["sha256"]
    ):
        raise GoldenError(
            "Sample manifest is missing or differs from the Golden record"
        )
    try:
        bundle = load_bundle(bundle_path)
    except ContractError as error:
        raise GoldenError(str(error)) from error
    policy, identity = _policy(policy_path)
    tick = int(policy["temporal"]["goldenCaptureTick"])
    try:
        _, declared_ticks = load_sample_contract(
            authoritative_manifest_path,
            str(identity["sampleId"]),
        )
    except ContractError as error:
        raise GoldenError(str(error)) from error
    required_ticks = (
        tick,
        *(
            int(value)
            for value in policy["temporal"]["adjacentCaptureTicks"]
        ),
    )
    if tuple(declared_ticks) != required_ticks:
        raise GoldenError(
            "Sample manifest capture ticks do not match Golden VisualPolicy"
        )
    if (
        identity != record["qualificationIdentity"]
        or bundle.render_configuration_identity != record["renderConfigurationIdentity"]
        or selected["captureTick"] != tick
        or bundle.source_commit != record["sourceCommit"] or bundle.bundle_id != selected["bundleId"]
        or bundle.qualification_identity != {**identity, "captureTick": tick}
        or bundle.execution["runId"] != selected["runId"] or bundle.execution["preset"] != selected["preset"]
        or bundle.execution["renderExecutionReceiptSha256"] != selected["receiptSha256"]
    ):
        raise GoldenError("selected bundle does not bind Golden identity/tick")
    _check_ppm(ppm)
    _check_sheet(index, selected, record)
    if require_approved and record["status"] != "approved":
        raise GoldenError("Golden record is a candidate and is not approved")
    return record


def _series_inputs(paths: list[Path], manifest: Path, policy: dict[str, Any], mode: str, runner: str | None) -> list[AttachmentSeries]:
    if len(paths) != RUN_COUNT:
        raise GoldenError("prepare requires exactly five --series values")
    if (mode == "exact-hardware" and runner is not None) or (mode == "runner-class" and not runner):
        raise GoldenError("hardware mode and runner class are inconsistent")
    identity = _identity(policy["qualificationIdentity"], "VisualPolicy qualificationIdentity")
    manifest_sha = _hash(manifest)
    expected_ticks = (int(policy["temporal"]["goldenCaptureTick"]), *[int(value) for value in policy["temporal"]["adjacentCaptureTicks"]])
    output: list[AttachmentSeries] = []
    seen_paths: set[Path] = set()
    seen_runs: set[str] = set()
    seen_hashes: set[str] = set()
    source: str | None = None
    preset: str | None = None
    configuration: dict[str, str | int] | None = None
    hardware: dict[str, str] | None = None
    for ordinal, raw in enumerate(paths, 1):
        path = raw.resolve()
        if path in seen_paths:
            raise GoldenError("calibration series paths must be unique")
        try:
            series = load_series(path, manifest)
        except ContractError as error:
            raise GoldenError(f"series {ordinal}: {error}") from error
        if (
            series.qualification_identity != identity
            or series.manifest_sha256 != manifest_sha
            or tuple(item.capture_tick for item in series.ticks) != expected_ticks
        ):
            raise GoldenError("series identity, sample manifest, or capture ticks mismatch")
        if series.run_id in seen_runs or series.series_sha256 in seen_hashes:
            raise GoldenError("series runId and seriesSha256 values must be unique")
        source = series.source_commit if source is None else source
        if series.source_commit != source:
            raise GoldenError("all series must use the same source commit")
        preset = series.preset if preset is None else preset
        if series.preset != preset:
            raise GoldenError("all series must use the same preset")
        configuration = (
            series.render_configuration_identity
            if configuration is None else configuration
        )
        if series.render_configuration_identity != configuration:
            raise GoldenError(
                "all series must use the same runtime configuration identity"
            )
        if mode == "exact-hardware":
            hardware = series.hardware_identity if hardware is None else hardware
            if series.hardware_identity != hardware:
                raise GoldenError("exact-hardware Golden requires identical hardware identities")
        seen_paths.add(path)
        seen_runs.add(series.run_id)
        seen_hashes.add(series.series_sha256)
        output.append(series)
    return output


def _contact(args: argparse.Namespace) -> int:
    try:
        series = load_series(args.series.resolve(), args.sample_manifest.resolve())
    except ContractError as error:
        raise GoldenError(str(error)) from error
    policy, identity = _policy(args.visual_policy.resolve())
    expected_ticks = (
        int(policy["temporal"]["goldenCaptureTick"]),
        *(int(value) for value in policy["temporal"]["adjacentCaptureTicks"]),
    )
    if (
        series.qualification_identity != identity
        or tuple(item.capture_tick for item in series.ticks) != expected_ticks
    ):
        raise GoldenError(
            "contact-sheet Series identity or capture ticks do not match VisualPolicy"
        )
    bundle, _ = _tick(series, expected_ticks[0])
    _contact_sheet(bundle, series.series_sha256, args.out_ppm.resolve(), args.out_index.resolve())
    print(f"Render quality contact sheet: {args.out_ppm}")
    return 0


def _prepare(args: argparse.Namespace) -> int:
    output, manifest, policy_path = args.output_dir.resolve(), args.sample_manifest.resolve(), args.visual_policy.resolve()
    if output.exists() and any(output.iterdir()):
        raise GoldenError("candidate output directory must be absent or empty")
    if not manifest.is_file():
        raise GoldenError("sample manifest is missing")
    policy, identity = _policy(policy_path)
    selected_ordinal = _int(args.selected_series_ordinal, "selectedSeriesOrdinal", 1)
    if selected_ordinal > RUN_COUNT:
        raise GoldenError("selected series ordinal exceeds five calibrations")
    series = _series_inputs([path.resolve() for path in args.series], manifest, policy, args.hardware_mode, args.runner_class)
    selected = series[selected_ordinal - 1]
    golden_tick = int(policy["temporal"]["goldenCaptureTick"])
    source_bundle, source_tick = _tick(selected, golden_tick)
    try:
        normalized_regions, declared_capture_ticks = load_sample_contract(
            manifest,
            str(identity["sampleId"]),
        )
        adjacent_ticks = tuple(
            int(value) for value in policy["temporal"]["adjacentCaptureTicks"]
        )
        calibration_ticks = (golden_tick, *adjacent_ticks)
        baseline_bundles = {
            tick: load_bundle(_tick(selected, tick)[0])
            for tick in calibration_ticks
        }
        for ordinal, item in enumerate(series, 1):
            visual_failures: list[str] = []
            for tick in calibration_ticks:
                current_bundle = load_bundle(_tick(item, tick)[0])
                _, tick_failures = _comparison(
                    current_bundle,
                    baseline_bundles[tick],
                    policy,
                    normalized_regions,
                    None,
                )
                visual_failures.extend(
                    f"tick {tick}: {failure}" for failure in tick_failures
                )
            temporal_bundles = [
                load_bundle(_tick(item, tick)[0]) for tick in adjacent_ticks
            ]
            _, temporal_failures = _temporal(
                temporal_bundles,
                policy,
                normalized_regions,
                declared_capture_ticks,
                item.source_commit,
                item.run_id,
                item.preset,
            )
            failures = visual_failures + [
                f"temporal: {failure}" for failure in temporal_failures
            ]
            if failures:
                raise GoldenError(
                    f"series {ordinal} failed VisualPolicy calibration: "
                    + "; ".join(failures)
                )
    except ContractError as error:
        raise GoldenError(f"five-series VisualPolicy calibration failed: {error}") from error
    output.mkdir(parents=True, exist_ok=True)
    bundle_dir = output / "bundle"
    shutil.copytree(source_bundle.parent, bundle_dir, symlinks=True)
    bundle_path = bundle_dir / source_bundle.name
    try:
        bundle = load_bundle(bundle_path)
    except ContractError as error:
        raise GoldenError(f"copied selected bundle is invalid: {error}") from error
    if _hash(bundle_path) != source_tick.bundle_sha256:
        raise GoldenError("copied selected bundle hash drifted")
    saved_policy = output / "visual-policy.json"
    _copy_json_lf(policy_path, saved_policy, "source VisualPolicy")
    saved_manifest = output / "sample-manifest.json"
    _copy_json_lf(manifest, saved_manifest, "source sample manifest")
    ppm, index = output / "contact-sheet.ppm", output / "contact-sheet.json"
    _contact_sheet(bundle_path, selected.series_sha256, ppm, index)
    record = {
        "schema": SCHEMA, "schemaVersion": VERSION, "goldenId": _str(args.golden_id, "goldenId"),
        "status": "candidate", "sourceCommit": selected.source_commit,
        "qualificationIdentity": identity,
        "renderConfigurationIdentity": selected.render_configuration_identity,
        "sampleManifest": {
            "path": _reltext(output, saved_manifest),
            "sha256": _hash(saved_manifest),
        },
        "hardwareMode": args.hardware_mode, "runnerClass": args.runner_class,
        "selectedBundle": {
            "seriesOrdinal": selected_ordinal, "seriesSha256": selected.series_sha256,
            "runId": selected.run_id, "preset": selected.preset, "captureTick": golden_tick,
            "bundleId": bundle.bundle_id, "bundlePath": _reltext(output, bundle_path),
            "bundleSha256": _hash(bundle_path), "receiptSha256": bundle.execution["renderExecutionReceiptSha256"],
        },
        "visualPolicy": {"path": _reltext(output, saved_policy), "sha256": _hash(saved_policy)},
        "contactSheet": {
            "ppmPath": _reltext(output, ppm), "ppmSha256": _hash(ppm),
            "indexPath": _reltext(output, index), "indexSha256": _hash(index),
        },
        "seriesAudit": {
            "requiredSeriesCount": RUN_COUNT, "selectedSeriesOrdinal": selected_ordinal,
            "runs": [{"ordinal": ordinal, "runId": item.run_id, "seriesSha256": item.series_sha256,
                      "hardwareIdentity": item.hardware_identity} for ordinal, item in enumerate(series, 1)],
        },
        "approval": {"approvedBy": None, "approvedAtUtc": None, "reason": None},
    }
    record_path = output / "golden-record.json"
    _write(record_path, record)
    verify_golden_record(
        record_path,
        bundle_path,
        saved_policy,
        saved_manifest,
        require_approved=False,
    )
    print(f"Render quality Golden candidate: {record_path}")
    return 0


def _approve(args: argparse.Namespace) -> int:
    candidate_path = args.candidate_record.resolve()
    candidate_root = candidate_path.parent
    candidate_record = _record(candidate_path)
    candidate = verify_golden_record(
        candidate_path,
        _relative(candidate_root, candidate_record["selectedBundle"]["bundlePath"], "selectedBundle.bundlePath"),
        _relative(candidate_root, candidate_record["visualPolicy"]["path"], "visualPolicy.path"),
        _relative(candidate_root, candidate_record["sampleManifest"]["path"], "sampleManifest.path"),
        require_approved=False,
    )
    if candidate["status"] != "candidate":
        raise GoldenError("only a candidate Golden can be approved")
    _assert_approval_source_is_clean(candidate["sourceCommit"])
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise GoldenError("approval output directory must be absent or empty")
    output.mkdir(parents=True, exist_ok=True)
    selected = candidate["selectedBundle"]
    source_bundle = _relative(candidate_root, selected["bundlePath"], "selectedBundle.bundlePath")
    bundle_dir = output / "bundle"
    shutil.copytree(source_bundle.parent, bundle_dir, symlinks=True)
    bundle_path = bundle_dir / source_bundle.name
    source_policy = _relative(candidate_root, candidate["visualPolicy"]["path"], "visualPolicy.path")
    policy_path = output / "visual-policy.json"
    _copy_json_lf(source_policy, policy_path, "candidate VisualPolicy")
    source_manifest = _relative(
        candidate_root,
        candidate["sampleManifest"]["path"],
        "sampleManifest.path",
    )
    manifest_path = output / "sample-manifest.json"
    _copy_json_lf(source_manifest, manifest_path, "candidate sample manifest")
    source_ppm = _relative(candidate_root, candidate["contactSheet"]["ppmPath"], "contactSheet.ppmPath")
    source_index = _relative(candidate_root, candidate["contactSheet"]["indexPath"], "contactSheet.indexPath")
    ppm, index = output / "contact-sheet.ppm", output / "contact-sheet.json"
    shutil.copy2(source_ppm, ppm)
    shutil.copy2(source_index, index)
    document = _load(candidate_path, "candidate Golden record")
    document["status"] = "approved"
    document["selectedBundle"]["bundlePath"] = _reltext(output, bundle_path)
    document["visualPolicy"]["path"] = _reltext(output, policy_path)
    document["sampleManifest"]["path"] = _reltext(output, manifest_path)
    document["contactSheet"]["ppmPath"] = _reltext(output, ppm)
    document["contactSheet"]["indexPath"] = _reltext(output, index)
    document["approval"] = {
        "approvedBy": _str(args.approved_by, "approvedBy"),
        "approvedAtUtc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "reason": _str(args.reason, "reason"),
    }
    record_path = output / "golden-record.json"
    _write(record_path, document)
    verify_golden_record(
        record_path,
        bundle_path,
        policy_path,
        manifest_path,
        require_approved=True,
    )
    print(f"Approved render quality Golden: {record_path}")
    return 0


def _verify(args: argparse.Namespace) -> int:
    record = verify_golden_record(
        args.record,
        args.bundle,
        args.policy,
        args.sample_manifest,
        require_approved=args.require_approved,
    )
    print(f"Render quality Golden valid: {record['goldenId']} ({record['status']})")
    return 0


COMPARISON_SCHEMA = "RVX.RenderQualityGoldenComparisonResult"
COMPARISON_VERSION = 2


def _write_comparison_report(path: Path, value: dict[str, Any]) -> None:
    """Publish a comparison result atomically, including a fail-closed result."""
    target = path.resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.parent / f".{target.name}.{uuid4().hex}.tmp"
    try:
        payload = json.dumps(
            value, indent=2, sort_keys=True, allow_nan=False
        ) + "\n"
        temporary.write_bytes(payload.encode("utf-8"))
        temporary.replace(target)
    finally:
        if temporary.exists():
            temporary.unlink()


def _comparison_result(
    args: argparse.Namespace,
) -> dict[str, Any]:
    record_path = args.record.resolve()
    sample_manifest_path = args.sample_manifest.resolve()
    series_manifest_path = (
        args.series_manifest.resolve()
        if args.series_manifest is not None else sample_manifest_path
    )
    return {
        "schema": COMPARISON_SCHEMA,
        "schemaVersion": COMPARISON_VERSION,
        "status": "failed",
        "comparisonPerformed": False,
        "goldenId": None,
        "goldenRecord": {
            "path": str(record_path),
            "sha256": _hash(record_path) if record_path.is_file() else None,
        },
        "goldenSampleManifest": {
            "path": str(sample_manifest_path),
            "sha256": (
                _hash(sample_manifest_path)
                if sample_manifest_path.is_file() else None
            ),
        },
        "seriesManifest": {
            "path": str(series_manifest_path),
            "sha256": (
                _hash(series_manifest_path)
                if series_manifest_path.is_file() else None
            ),
        },
        "requiredHardwareMode": args.required_hardware_mode,
        "runnerClass": args.runner_class,
        "currentHardwareIdentity": None,
        "baselineHardwareIdentity": None,
        "currentRenderConfigurationIdentity": None,
        "baselineRenderConfigurationIdentity": None,
        "visualMetrics": [],
        "temporalMetrics": [],
        "failureCategory": "series-validation",
        "diagnostic": "comparison did not run",
    }


def _json_semantically_equal(left: Any, right: Any) -> bool:
    """Compare parsed JSON without treating bool and integer values as equal."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return (
            left.keys() == right.keys()
            and all(_json_semantically_equal(left[key], right[key]) for key in left)
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _json_semantically_equal(item, other)
            for item, other in zip(left, right, strict=True)
        )
    return left == right


def _compare_series(args: argparse.Namespace) -> int:
    """Compare a completion-owned attachment series with an approved Golden."""
    result = _comparison_result(args)
    failure_category = "series-validation"
    try:
        sample_manifest_path = args.sample_manifest.resolve()
        series_manifest_path = (
            args.series_manifest.resolve()
            if args.series_manifest is not None else sample_manifest_path
        )
        series = load_series(args.series.resolve(), series_manifest_path)
        result["currentHardwareIdentity"] = {
            field: series.hardware_identity[field] for field in HARDWARE_FIELDS
        }
        result["currentRenderConfigurationIdentity"] = (
            series.render_configuration_identity
        )
        if series.source_commit != args.expected_source_commit:
            raise GoldenError("attachment series sourceCommit is stale")
        if series.run_id != args.expected_run_id:
            raise GoldenError("attachment series belongs to another run")
        if series.preset != args.expected_preset:
            raise GoldenError("attachment series belongs to another preset")

        failure_category = "golden-validation"
        record = verify_golden_record(
            args.record.resolve(),
            args.baseline.resolve(),
            args.policy.resolve(),
            sample_manifest_path,
            require_approved=True,
        )
        result["goldenId"] = record["goldenId"]
        result["goldenRecord"]["sha256"] = _hash(args.record.resolve())
        result["baselineRenderConfigurationIdentity"] = (
            record["renderConfigurationIdentity"]
        )

        failure_category = "manifest-validation"
        source_manifest = _load(series_manifest_path, "series manifest")
        packaged_manifest = _load(sample_manifest_path, "sample manifest")
        if not _json_semantically_equal(source_manifest, packaged_manifest):
            failure_category = "manifest-semantic-mismatch"
            raise GoldenError(
                "series manifest is not semantically equal to the Golden sample manifest"
            )

        failure_category = "hardware-mode"
        if record["hardwareMode"] != args.required_hardware_mode:
            raise GoldenError(
                "Golden record hardwareMode does not match --required-hardware-mode"
            )
        selected_ordinal = int(record["seriesAudit"]["selectedSeriesOrdinal"])
        baseline_hardware = record["seriesAudit"]["runs"][selected_ordinal - 1][
            "hardwareIdentity"
        ]
        result["baselineHardwareIdentity"] = {
            field: baseline_hardware[field] for field in HARDWARE_FIELDS
        }
        if args.required_hardware_mode == "exact-hardware":
            if args.runner_class is not None:
                raise GoldenError(
                    "--runner-class is not valid for exact-hardware Golden comparison"
                )
            mismatches = [
                field
                for field in HARDWARE_FIELDS
                if series.hardware_identity[field] != baseline_hardware[field]
            ]
            if mismatches:
                failure_category = "hardware-mismatch"
                raise GoldenError(
                    "exact-hardware identity differs from approved Golden: "
                    + ", ".join(mismatches)
                )
        else:
            if not args.runner_class:
                raise GoldenError(
                    "runner-class Golden comparison requires --runner-class"
                )
            if args.runner_class != record["runnerClass"]:
                raise GoldenError(
                    "--runner-class does not match the approved Golden record"
                )

        failure_category = "render-configuration-mismatch"
        if (
            series.render_configuration_identity
            != record["renderConfigurationIdentity"]
        ):
            raise GoldenError(
                "series runtime configuration does not match the approved Golden"
            )

        failure_category = "comparison-contract"
        policy, identity = _policy(args.policy.resolve())
        normalized_regions, declared_capture_ticks = load_sample_contract(
            sample_manifest_path,
            str(identity["sampleId"]),
        )
        golden_tick = int(policy["temporal"]["goldenCaptureTick"])
        adjacent_ticks = tuple(
            int(value) for value in policy["temporal"]["adjacentCaptureTicks"]
        )
        expected_ticks = (golden_tick, *adjacent_ticks)
        if (
            series.qualification_identity != identity
            or tuple(tick.capture_tick for tick in series.ticks) != expected_ticks
            or tuple(declared_capture_ticks) != expected_ticks
        ):
            raise GoldenError(
                "attachment series identity or capture ticks do not match VisualPolicy"
            )

        current_bundle = load_bundle(_tick(series, golden_tick)[0])
        baseline_bundle = load_bundle(args.baseline.resolve())
        result["comparisonPerformed"] = True
        visual_metrics, visual_failures = _comparison(
            current_bundle,
            baseline_bundle,
            policy,
            normalized_regions,
            None,
        )
        temporal_bundles = [
            load_bundle(_tick(series, tick)[0]) for tick in adjacent_ticks
        ]
        temporal_metrics, temporal_failures = _temporal(
            temporal_bundles,
            policy,
            normalized_regions,
            tuple(declared_capture_ticks),
            series.source_commit,
            series.run_id,
            series.preset,
        )
        result["visualMetrics"] = visual_metrics
        result["temporalMetrics"] = temporal_metrics
        if visual_failures or temporal_failures:
            if visual_failures and temporal_failures:
                failure_category = "visual-and-temporal-metric"
            elif visual_failures:
                failure_category = "visual-metric"
            else:
                failure_category = "temporal-metric"
            raise GoldenError("; ".join([*visual_failures, *temporal_failures]))

        result["status"] = "passed"
        result["failureCategory"] = None
        result["diagnostic"] = "approved Golden comparison passed"
        _write_comparison_report(args.report, result)
        print(f"Render quality Golden comparison passed: {record['goldenId']}")
        return 0
    except (GoldenError, ContractError, OSError, ValueError) as error:
        result["status"] = "failed"
        result["failureCategory"] = failure_category
        result["diagnostic"] = str(error)
        _write_comparison_report(args.report, result)
        print(f"Render quality Golden comparison rejected: {error}", file=sys.stderr)
        return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    contact = commands.add_parser("contact-sheet", help="write a deterministic 4x2 PPM preview")
    contact.add_argument("--series", required=True, type=Path)
    contact.add_argument("--sample-manifest", required=True, type=Path)
    contact.add_argument("--visual-policy", required=True, type=Path)
    contact.add_argument("--out-ppm", required=True, type=Path)
    contact.add_argument("--out-index", required=True, type=Path)
    contact.set_defaults(action=_contact)
    prepare = commands.add_parser("prepare", help="create an unapproved five-series Golden candidate")
    prepare.add_argument("--series", action="append", required=True, type=Path)
    prepare.add_argument("--sample-manifest", required=True, type=Path)
    prepare.add_argument("--visual-policy", required=True, type=Path)
    prepare.add_argument("--output-dir", required=True, type=Path)
    prepare.add_argument("--golden-id", required=True)
    prepare.add_argument("--selected-series-ordinal", required=True, type=int)
    prepare.add_argument("--hardware-mode", required=True, choices=("exact-hardware", "runner-class"))
    prepare.add_argument("--runner-class")
    prepare.set_defaults(action=_prepare)
    approve = commands.add_parser("approve", help="promote a human-reviewed Golden candidate")
    approve.add_argument("--candidate-record", required=True, type=Path)
    approve.add_argument("--output-dir", required=True, type=Path)
    approve.add_argument("--approved-by", required=True)
    approve.add_argument("--reason", required=True)
    approve.set_defaults(action=_approve)
    verify = commands.add_parser("verify", help="validate a Golden package")
    verify.add_argument("--record", required=True, type=Path)
    verify.add_argument("--bundle", required=True, type=Path)
    verify.add_argument("--policy", required=True, type=Path)
    verify.add_argument("--sample-manifest", required=True, type=Path)
    verify.add_argument("--require-approved", action="store_true")
    verify.set_defaults(action=_verify)
    compare = commands.add_parser(
        "compare-series",
        help="compare a completion-owned series with an approved Golden",
    )
    compare.add_argument("--series", required=True, type=Path)
    compare.add_argument("--record", required=True, type=Path)
    compare.add_argument("--baseline", required=True, type=Path)
    compare.add_argument("--policy", required=True, type=Path)
    compare.add_argument("--sample-manifest", required=True, type=Path)
    compare.add_argument(
        "--series-manifest",
        type=Path,
        help="source manifest bound by --series; defaults to --sample-manifest",
    )
    compare.add_argument(
        "--required-hardware-mode",
        required=True,
        choices=("exact-hardware", "runner-class"),
    )
    compare.add_argument("--expected-source-commit", required=True)
    compare.add_argument("--expected-run-id", required=True)
    compare.add_argument("--expected-preset", required=True)
    compare.add_argument("--runner-class")
    compare.add_argument("--report", required=True, type=Path)
    compare.set_defaults(action=_compare_series)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return args.action(args)
    except (GoldenError, ContractError, OSError, ValueError) as error:
        print(f"Render quality Golden rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
