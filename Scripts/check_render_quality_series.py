#!/usr/bin/env python3
"""Fail-closed validation for one same-process RenderQuality attachment series."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from check_render_quality_bundle import (
    ContractError,
    load_bundle,
    load_sample_contract,
)


SCHEMA = "RVX.RenderQualityAttachmentSeries"
SCHEMA_VERSION = 1
SHA256 = re.compile(r"^[0-9a-f]{64}$")
IDENTITY_FIELDS = (
    "sampleId",
    "pipeline",
    "quality",
    "backend",
    "view",
    "visualContractVersion",
)
HARDWARE_FIELDS = (
    "backend",
    "adapterName",
    "driverVersion",
    "osBuild",
    "capabilityHash",
)
TICK_FIELDS = (
    "captureTick",
    "bundleId",
    "bundlePath",
    "bundleSha256",
    "receiptSha256",
    "frameSequence",
    "submittedFrame",
    "completedFrame",
)


@dataclass(frozen=True)
class SeriesTick:
    capture_tick: int
    bundle_id: str
    bundle_path: Path
    bundle_sha256: str
    receipt_sha256: str
    frame_sequence: int
    submitted_frame: int
    completed_frame: int


@dataclass(frozen=True)
class AttachmentSeries:
    path: Path
    source_commit: str
    run_id: str
    preset: str
    qualification_identity: dict[str, str | int]
    hardware_identity: dict[str, str]
    manifest_sha256: str
    ticks: tuple[SeriesTick, ...]
    series_sha256: str


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


def _require_keys(value: dict[str, Any], keys: set[str], context: str) -> None:
    missing = keys - value.keys()
    unknown = value.keys() - keys
    if missing:
        raise ContractError(f"{context} is missing fields: {sorted(missing)}")
    if unknown:
        raise ContractError(
            f"{context} contains unsupported fields: {sorted(unknown)}"
        )


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{context} must be a non-empty string")
    return value


def _integer(value: Any, context: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ContractError(f"{context} must be an integer >= {minimum}")
    return value


def _sha(value: Any, context: str) -> str:
    digest = _string(value, context)
    if not SHA256.fullmatch(digest):
        raise ContractError(f"{context} must be a lowercase SHA-256 digest")
    return digest


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _resolve(root: Path, value: Any, context: str) -> Path:
    text = _string(value, context)
    if "\\" in text:
        raise ContractError(f"{context} must use package-relative POSIX separators")
    relative = PurePosixPath(text)
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise ContractError(f"{context} escapes the series root")
    resolved_root = root.resolve()
    resolved = (resolved_root / Path(*relative.parts)).resolve()
    if resolved_root not in resolved.parents:
        raise ContractError(f"{context} escapes the series root")
    return resolved


def _append_binding_field(output: bytearray, name: str, value: Any) -> None:
    encoded = str(value).encode("utf-8")
    output.extend(name.encode("ascii"))
    output.extend(b":")
    output.extend(str(len(encoded)).encode("ascii"))
    output.extend(b":")
    output.extend(encoded)
    output.extend(b"\n")


def compute_series_sha256(
    source_commit: str,
    run_id: str,
    preset: str,
    identity: dict[str, str | int],
    hardware: dict[str, str],
    manifest_sha256: str,
    ticks: tuple[SeriesTick, ...],
) -> str:
    binding = bytearray()
    for name, value in (
        ("schema", SCHEMA),
        ("schemaVersion", SCHEMA_VERSION),
        ("sourceCommit", source_commit),
        ("runId", run_id),
        ("preset", preset),
    ):
        _append_binding_field(binding, name, value)
    for name in IDENTITY_FIELDS:
        _append_binding_field(binding, f"identity.{name}", identity[name])
    for name in HARDWARE_FIELDS:
        _append_binding_field(binding, f"hardware.{name}", hardware[name])
    _append_binding_field(binding, "manifestSha256", manifest_sha256)
    for tick in ticks:
        for name, value in (
            ("captureTick", tick.capture_tick),
            ("bundleId", tick.bundle_id),
            ("bundlePath", tick.bundle_path.as_posix()),
            ("bundleSha256", tick.bundle_sha256),
            ("receiptSha256", tick.receipt_sha256),
            ("frameSequence", tick.frame_sequence),
            ("submittedFrame", tick.submitted_frame),
            ("completedFrame", tick.completed_frame),
        ):
            _append_binding_field(binding, f"tick.{name}", value)
    return hashlib.sha256(binding).hexdigest()


def load_series(
    path: Path,
    sample_manifest_path: Path,
) -> AttachmentSeries:
    document = _load_json(path, "attachment series")
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
            "manifestSha256",
            "ticks",
            "seriesSha256",
        },
        "attachment series",
    )
    if document["schema"] != SCHEMA or document["schemaVersion"] != SCHEMA_VERSION:
        raise ContractError("attachment series schema/version is unsupported")
    source_commit = _string(document["sourceCommit"], "series.sourceCommit")
    run_id = _string(document["runId"], "series.runId")
    preset = _string(document["preset"], "series.preset")

    identity_document = document["qualificationIdentity"]
    if not isinstance(identity_document, dict):
        raise ContractError("series qualificationIdentity must be an object")
    _require_keys(
        identity_document, set(IDENTITY_FIELDS), "series qualificationIdentity"
    )
    identity: dict[str, str | int] = {
        name: _string(identity_document[name], f"identity.{name}")
        for name in IDENTITY_FIELDS
        if name != "visualContractVersion"
    }
    identity["visualContractVersion"] = _integer(
        identity_document["visualContractVersion"],
        "identity.visualContractVersion",
        1,
    )

    hardware_document = document["hardwareIdentity"]
    if not isinstance(hardware_document, dict):
        raise ContractError("series hardwareIdentity must be an object")
    _require_keys(
        hardware_document, set(HARDWARE_FIELDS), "series hardwareIdentity"
    )
    hardware = {
        name: _string(hardware_document[name], f"hardware.{name}")
        for name in HARDWARE_FIELDS
    }
    hardware["capabilityHash"] = _sha(
        hardware["capabilityHash"], "hardware.capabilityHash"
    )
    if hardware["backend"] != identity["backend"]:
        raise ContractError("series hardware backend does not match qualification")

    manifest_sha256 = _sha(document["manifestSha256"], "series.manifestSha256")
    if not sample_manifest_path.is_file() or _hash(sample_manifest_path) != manifest_sha256:
        raise ContractError("sample manifest hash does not match attachment series")
    _, declared_ticks = load_sample_contract(
        sample_manifest_path, str(identity["sampleId"])
    )

    ticks_document = document["ticks"]
    if not isinstance(ticks_document, list) or not ticks_document:
        raise ContractError("attachment series ticks must be a non-empty array")
    if len(ticks_document) > 4096:
        raise ContractError("attachment series tick count exceeds limit")
    root = path.resolve().parent
    ticks: list[SeriesTick] = []
    previous_tick = 0
    previous_frame = 0
    previous_submitted = -1
    previous_completed = -1
    observed_bundle_ids: set[str] = set()
    observed_receipts: set[str] = set()
    observed_paths: set[Path] = set()
    for index, value in enumerate(ticks_document):
        context = f"series.ticks[{index}]"
        if not isinstance(value, dict):
            raise ContractError(f"{context} must be an object")
        _require_keys(value, set(TICK_FIELDS), context)
        capture_tick = _integer(value["captureTick"], f"{context}.captureTick", 1)
        bundle_id = _string(value["bundleId"], f"{context}.bundleId")
        bundle_relative = PurePosixPath(
            _string(value["bundlePath"], f"{context}.bundlePath")
        )
        bundle_path = _resolve(root, bundle_relative.as_posix(), f"{context}.bundlePath")
        bundle_sha256 = _sha(value["bundleSha256"], f"{context}.bundleSha256")
        receipt_sha256 = _sha(value["receiptSha256"], f"{context}.receiptSha256")
        frame_sequence = _integer(
            value["frameSequence"], f"{context}.frameSequence", 1
        )
        submitted_frame = _integer(
            value["submittedFrame"], f"{context}.submittedFrame", 0
        )
        completed_frame = _integer(
            value["completedFrame"], f"{context}.completedFrame", 0
        )
        if (
            capture_tick <= previous_tick
            or frame_sequence <= previous_frame
            or submitted_frame <= previous_submitted
            or completed_frame <= previous_completed
            or completed_frame < submitted_frame
        ):
            raise ContractError("series tick/frame ownership is stale or out of order")
        if bundle_id in observed_bundle_ids:
            raise ContractError("attachment series reuses a bundleId")
        if receipt_sha256 in observed_receipts:
            raise ContractError("attachment series reuses a receipt")
        if bundle_path in observed_paths:
            raise ContractError("attachment series reuses a bundle path")
        if not bundle_path.is_file() or _hash(bundle_path) != bundle_sha256:
            raise ContractError("series bundle is missing or its hash mismatches")

        bundle = load_bundle(bundle_path)
        bundle_identity = {
            key: item
            for key, item in bundle.qualification_identity.items()
            if key != "captureTick"
        }
        execution = bundle.execution
        if (
            bundle.source_commit != source_commit
            or bundle.bundle_id != bundle_id
            or bundle_identity != identity
            or int(bundle.qualification_identity["captureTick"]) != capture_tick
            or execution["runId"] != run_id
            or execution["preset"] != preset
            or execution["renderExecutionReceiptSha256"] != receipt_sha256
            or int(execution["frameSequence"]) != frame_sequence
            or int(execution["submittedFrame"]) != submitted_frame
            or int(execution["completedFrame"]) != completed_frame
        ):
            raise ContractError("series tick metadata does not match its bundle")
        ticks.append(
            SeriesTick(
                capture_tick,
                bundle_id,
                Path(*bundle_relative.parts),
                bundle_sha256,
                receipt_sha256,
                frame_sequence,
                submitted_frame,
                completed_frame,
            )
        )
        previous_tick = capture_tick
        previous_frame = frame_sequence
        previous_submitted = submitted_frame
        previous_completed = completed_frame
        observed_bundle_ids.add(bundle_id)
        observed_receipts.add(receipt_sha256)
        observed_paths.add(bundle_path)

    parsed_ticks = tuple(ticks)
    if tuple(tick.capture_tick for tick in parsed_ticks) != declared_ticks:
        raise ContractError("attachment series ticks do not match sample manifest")
    series_sha256 = _sha(document["seriesSha256"], "series.seriesSha256")
    if compute_series_sha256(
        source_commit,
        run_id,
        preset,
        identity,
        hardware,
        manifest_sha256,
        parsed_ticks,
    ) != series_sha256:
        raise ContractError("attachment series SHA-256 binding mismatch")
    return AttachmentSeries(
        path.resolve(),
        source_commit,
        run_id,
        preset,
        identity,
        hardware,
        manifest_sha256,
        parsed_ticks,
        series_sha256,
    )


def _write_report(path: Path | None, value: dict[str, Any]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--series", required=True, type=Path)
    parser.add_argument("--sample-manifest", required=True, type=Path)
    parser.add_argument("--expected-source-commit", required=True)
    parser.add_argument("--expected-run-id", required=True)
    parser.add_argument("--expected-preset", required=True)
    parser.add_argument("--expected-adapter-name")
    parser.add_argument("--expected-driver-version")
    parser.add_argument("--expected-os-build")
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result: dict[str, Any] = {
        "schema": "RVX.RenderQualityAttachmentSeriesResult",
        "schemaVersion": 1,
        "status": "failed",
        "failures": [],
    }
    try:
        series = load_series(args.series, args.sample_manifest)
        if series.source_commit != args.expected_source_commit:
            raise ContractError("attachment series sourceCommit is stale")
        if series.run_id != args.expected_run_id:
            raise ContractError("attachment series belongs to another run")
        if series.preset != args.expected_preset:
            raise ContractError("attachment series belongs to another preset")
        for argument, field, label in (
            (args.expected_adapter_name, "adapterName", "adapter"),
            (args.expected_driver_version, "driverVersion", "driver"),
            (args.expected_os_build, "osBuild", "OS build"),
        ):
            if argument and series.hardware_identity[field] != argument:
                raise ContractError(
                    f"attachment series {label} identity is unexpected"
                )
        result.update(
            {
                "status": "passed",
                "sourceCommit": series.source_commit,
                "runId": series.run_id,
                "preset": series.preset,
                "qualificationIdentity": series.qualification_identity,
                "hardwareIdentity": series.hardware_identity,
                "manifestSha256": series.manifest_sha256,
                "seriesSha256": series.series_sha256,
                "captureTicks": [
                    tick.capture_tick for tick in series.ticks
                ],
            }
        )
        _write_report(args.report, result)
        print("Render quality attachment series validation passed")
        return 0
    except ContractError as error:
        result["failures"] = [str(error)]
        _write_report(args.report, result)
        print(
            f"Render quality attachment series rejected: {error}",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
